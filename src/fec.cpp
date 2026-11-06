#include "fec.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <array>
#include <cstring>
#include <vector>

#include "util.h"

namespace pd {
namespace fec {
namespace {

constexpr uint8_t kFieldPolyLow = 0x1d;

uint8_t gf_mul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  while (b != 0) {
    if (b & 1) p ^= a;
    b >>= 1;
    uint8_t carry = a & 0x80;
    a <<= 1;
    if (carry) a ^= kFieldPolyLow;
  }
  return p;
}

std::vector<uint8_t> generator_poly(int roots) {
  std::vector<uint8_t> g(static_cast<size_t>(roots) + 1, 0);
  g[0] = 1;
  uint8_t root = 1;
  for (int i = 0; i < roots; ++i) {
    for (int k = i + 1; k > 0; --k) {
      g[k] = static_cast<uint8_t>(g[k - 1] ^ gf_mul(g[k], root));
    }
    g[0] = gf_mul(g[0], root);
    root = gf_mul(root, 2);
  }
  return g;
}

class Encoder {
 public:
  explicit Encoder(int roots) : roots_(roots), mul_(static_cast<size_t>(roots)) {
    auto g = generator_poly(roots);
    for (int j = 0; j < roots; ++j) {
      uint8_t c = g[roots - 1 - j];
      for (int f = 0; f < 256; ++f) {
        mul_[static_cast<size_t>(j)][static_cast<size_t>(f)] =
            gf_mul(c, static_cast<uint8_t>(f));
      }
    }
  }

  int roots() const { return roots_; }

  // `states` holds len(syms)*roots bytes. Each syms byte is the next data
  // symbol of an independent codeword.
  inline void update(uint8_t* states, const uint8_t* syms, size_t n) const {
    const int r = roots_;
    if (r == 2) {
      const uint8_t* m0 = mul_[0].data();
      const uint8_t* m1 = mul_[1].data();
      uint8_t* st = states;
      for (size_t i = 0; i < n; ++i, st += 2) {
        uint8_t f = static_cast<uint8_t>(syms[i] ^ st[0]);
        st[0] = static_cast<uint8_t>(st[1] ^ m0[f]);
        st[1] = m1[f];
      }
      return;
    }
    for (size_t i = 0; i < n; ++i) {
      uint8_t* st = states + i * static_cast<size_t>(r);
      uint8_t f = static_cast<uint8_t>(syms[i] ^ st[0]);
      for (int j = 0; j < r - 1; ++j) {
        st[j] = static_cast<uint8_t>(st[j + 1] ^ mul_[static_cast<size_t>(j)][f]);
      }
      st[r - 1] = mul_[static_cast<size_t>(r - 1)][f];
    }
  }

 private:
  int roots_;
  std::vector<std::array<uint8_t, 256>> mul_;
};

struct Scratch {
  std::vector<uint8_t> syms;
  std::vector<uint8_t> states;
};

}  // namespace

int Params::rounds() const {
  int n = rsn();
  int r = data_blocks / n;
  if (data_blocks % n != 0) ++r;
  return r;
}

int Params::batch_width() const {
  int w = batch_rounds <= 0 ? kDefaultBatchRounds : batch_rounds;
  return std::min(w, rounds());
}

int Params::batches() const {
  int r = rounds();
  int w = batch_rounds <= 0 ? kDefaultBatchRounds : batch_rounds;
  int b = r / w;
  if (r % w != 0) ++b;
  return b;
}

int64_t Params::parity_bytes() const {
  return static_cast<int64_t>(rounds()) * roots * kBlockSize;
}

void Params::validate() const {
  if (data_blocks <= 0) {
    throwf("fec: data block count must be positive, got %d", data_blocks);
  }
  if (roots < 1 || roots > 254) {
    throwf("fec: roots must be in 1..254, got %d", roots);
  }
  if (batch_rounds < 0) {
    throwf("fec: batch rounds must not be negative, got %d", batch_rounds);
  }
  if (static_cast<int64_t>(data_blocks) >
      INT64_MAX / static_cast<int64_t>(kBlockSize)) {
    throw Error("fec: data exceeds addressable range");
  }
}

void generate(ThreadPool& pool, OutFile& out, int64_t data_offset,
              int64_t fec_offset, const Params& params,
              const std::function<void(int, int)>& progress) {
  params.validate();
  const int bs = kBlockSize;
  const int roots = params.roots;
  const int rounds = params.rounds();
  const int width = params.batch_width();
  const int batches = params.batches();
  const int data_blocks = params.data_blocks;
  const int rsn = params.rsn();
  Encoder encoder(roots);

  std::atomic<bool> failed{false};
  std::mutex error_mu;
  std::string error;

  pool.parallel_for(static_cast<size_t>(batches), [&](size_t batch) {
    if (failed.load(std::memory_order_relaxed)) return;
    try {
      if (stop_requested()) throw Error("payload: interrupted");
      thread_local Scratch scratch;
      const int r0 = static_cast<int>(batch) * width;
      const int w = std::min(width, rounds - r0);
      const size_t span = static_cast<size_t>(w) * bs;
      scratch.syms.resize(span);
      scratch.states.resize(span * roots);
      uint8_t* syms = scratch.syms.data();
      uint8_t* states = scratch.states.data();
      memset(states, 0, span * roots);

      for (int j = 0; j < rsn; ++j) {
        int64_t first = static_cast<int64_t>(j) * rounds + r0;
        int have = 0;
        if (first < data_blocks) {
          have = w;
          if (data_blocks - first < have) {
            have = data_blocks - static_cast<int>(first);
          }
          out.read_at(static_cast<uint64_t>(data_offset + first * bs), syms,
                      static_cast<size_t>(have) * bs);
        }
        if (have < w) {
          memset(syms + static_cast<size_t>(have) * bs, 0,
                 static_cast<size_t>(w - have) * bs);
        }
        encoder.update(states, syms, span);
      }
      out.write_at(static_cast<uint64_t>(fec_offset) +
                       static_cast<uint64_t>(r0) * roots * bs,
                   states, span * roots);
      if (progress) progress(static_cast<int>(batch) + 1, batches);
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(error_mu);
      if (!failed.load()) {
        error = e.what();
        failed.store(true);
      }
    }
  });
  if (failed.load()) throw Error(error);
}

}  // namespace fec
}  // namespace pd

#include "verity.h"

#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <vector>

#include "fec.h"
#include "sha.h"
#include "util.h"

namespace pd {
namespace {

constexpr uint64_t kSparseHole = kSparseHoleBlock;

struct SingleExtent {
  uint64_t start = 0;
  uint64_t blocks = 0;
};

SingleExtent single_extent(const Extents& extents, const char* what) {
  SingleExtent out;
  bool have = false;
  for (const auto& e : extents) {
    if (e.num_blocks == 0) continue;
    if (have) throwf("payload: %s must be a single contiguous extent", what);
    out.start = e.start_block;
    out.blocks = e.num_blocks;
    have = true;
  }
  return out;
}

uint64_t next_power_of_two(uint64_t n) {
  uint64_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

struct Hasher {
  const uint8_t* salt;
  size_t salt_len;
  bool sha1;

  void digest(const uint8_t* data, size_t len, uint8_t* out) const {
    if (sha1) {
      auto d = sha1_concat(salt, salt_len, data, len);
      memcpy(out, d.data(), d.size());
    } else {
      auto d = sha256_concat(salt, salt_len, data, len);
      memcpy(out, d.data(), d.size());
    }
  }
};

void write_hash_tree(ThreadPool& pool, OutFile& out, const Partition& part,
                     uint64_t block_size) {
  const auto& info = part.hash_tree;
  validate_extents(info.tree_extent, block_size);
  validate_extents(info.data_extent, block_size);
  const SingleExtent tree = single_extent(info.tree_extent, "hash_tree_extent");
  const SingleExtent data =
      single_extent(info.data_extent, "hash_tree_data_extent");
  if (data.blocks == 0) {
    throwf("payload: partition %s: hash_tree_extent is set but "
           "hash_tree_data_extent is missing",
           part.name.c_str());
  }

  const std::string& algorithm = info.algorithm;
  size_t digest_len = 0;
  bool sha1 = false;
  if (algorithm == "sha256") {
    digest_len = 32;
  } else if (algorithm == "sha1" || algorithm.empty()) {
    digest_len = 20;
    sha1 = true;
  } else {
    throwf("payload: unsupported hash tree algorithm %s", algorithm.c_str());
  }
  const uint64_t hash_size = next_power_of_two(digest_len);
  if ((block_size & (block_size - 1)) != 0 || block_size < hash_size * 2) {
    throwf("payload: partition %s: invalid hash tree block size %llu",
           part.name.c_str(), static_cast<unsigned long long>(block_size));
  }
  if (tree.start == kSparseHole || data.start == kSparseHole) {
    throw Error("payload: sparse hole extents are not supported");
  }
  if ((tree.start + tree.blocks) * block_size > part.new_size ||
      (data.start + data.blocks) * block_size > part.new_size) {
    throwf("payload: partition %s: hash tree extent exceeds partition size",
           part.name.c_str());
  }
  if (tree.start < data.start + data.blocks &&
      data.start < tree.start + tree.blocks) {
    throwf("payload: partition %s: hash tree extent overlaps hash tree data "
           "extent",
           part.name.c_str());
  }

  uint64_t tree_blocks_calc = 0;
  for (uint64_t blocks = data.blocks;;) {
    blocks = (blocks + block_size / hash_size - 1) / (block_size / hash_size);
    tree_blocks_calc += blocks;
    if (blocks <= 1) break;
  }
  if (tree_blocks_calc != tree.blocks) {
    throwf("payload: partition %s: computed hash tree size does not match "
           "addressable hash_tree extent size",
           part.name.c_str());
  }

  const uint64_t data_offset = data.start * block_size;
  std::vector<std::vector<uint8_t>> levels;
  std::vector<uint8_t> level(
      static_cast<size_t>(align_up(data.blocks * hash_size, block_size)), 0);
  Hasher hasher{info.salt.data(), info.salt.size(), sha1};
  const uint64_t data_blocks = data.blocks;

  std::atomic<bool> failed{false};
  std::mutex error_mu;
  std::string error;
  auto fail = [&](const std::exception& e) {
    std::lock_guard<std::mutex> lock(error_mu);
    if (!failed.load()) {
      error = e.what();
      failed.store(true);
    }
  };
  auto check_stop = [&] {
    if (stop_requested()) throw Error("payload: interrupted");
  };

  pool.parallel_for(static_cast<size_t>(data_blocks), [&](size_t i) {
    if (failed.load(std::memory_order_relaxed)) return;
    try {
      check_stop();
      // thread_local storage must be sized on every worker thread, not only
      // on the thread that entered this function.
      thread_local std::vector<uint8_t> block_buf;
      block_buf.resize(static_cast<size_t>(block_size));
      out.read_at(data_offset + i * block_size, block_buf.data(),
                  static_cast<size_t>(block_size));
      uint8_t digest[32];
      hasher.digest(block_buf.data(), static_cast<size_t>(block_size), digest);
      memcpy(level.data() + i * hash_size, digest, digest_len);
    } catch (const std::exception& e) {
      fail(e);
    }
  });
  if (failed.load()) throw Error(error);
  levels.push_back(std::move(level));

  while (levels.back().size() / block_size > 1) {
    const std::vector<uint8_t>& prev = levels.back();
    const uint64_t blocks = prev.size() / block_size;
    std::vector<uint8_t> next(
        static_cast<size_t>(align_up(blocks * hash_size, block_size)), 0);
    pool.parallel_for(static_cast<size_t>(blocks), [&](size_t i) {
      if (failed.load(std::memory_order_relaxed)) return;
      try {
        check_stop();
        uint8_t digest[32];
        hasher.digest(prev.data() + i * block_size,
                      static_cast<size_t>(block_size), digest);
        memcpy(next.data() + i * hash_size, digest, digest_len);
      } catch (const std::exception& e) {
        fail(e);
      }
    });
    if (failed.load()) throw Error(error);
    levels.push_back(std::move(next));
  }

  std::vector<uint8_t> tree_data;
  for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
    tree_data.insert(tree_data.end(), it->begin(), it->end());
  }
  const uint64_t expected = tree.blocks * block_size;
  if (tree_data.size() != expected) {
    throwf("payload: partition %s: computed hash tree size %zu does not match "
           "hash_tree extent size %llu",
           part.name.c_str(), tree_data.size(),
           static_cast<unsigned long long>(expected));
  }
  if (!tree_data.empty()) {
    out.write_at(tree.start * block_size, tree_data.data(), tree_data.size());
  }
}

bool extents_overlap(const Extents& a, const Extents& b) {
  const uint64_t a_blocks = total_blocks(a);
  const uint64_t b_blocks = total_blocks(b);
  if (a_blocks == 0 || b_blocks == 0) return false;
  const SingleExtent ea = single_extent(a, "extent");
  const SingleExtent eb = single_extent(b, "extent");
  return ea.start < eb.start + eb.blocks && eb.start < ea.start + ea.blocks;
}

void write_fec(ThreadPool& pool, OutFile& out, const Partition& part,
               uint64_t block_size,
               const std::function<void(int, int)>& progress) {
  validate_extents(part.fec.fec_extent, block_size);
  validate_extents(part.fec.data_extent, block_size);
  validate_extents(part.hash_tree.tree_extent, block_size);
  const SingleExtent fec_ext = single_extent(part.fec.fec_extent, "fec_extent");
  const SingleExtent data_ext =
      single_extent(part.fec.data_extent, "fec_data_extent");
  if (data_ext.blocks == 0) {
    throwf("payload: partition %s: fec_extent is set but fec_data_extent is "
           "missing",
           part.name.c_str());
  }
  if (block_size != static_cast<uint64_t>(fec::kBlockSize)) {
    throwf("payload: partition %s: FEC generation requires block size %d, got "
           "%llu",
           part.name.c_str(), fec::kBlockSize,
           static_cast<unsigned long long>(block_size));
  }
  if (part.fec.roots < 2 || part.fec.roots > 24) {
    throwf("payload: partition %s: fec_roots %u is outside the supported range "
           "2..24",
           part.name.c_str(), part.fec.roots);
  }
  if (data_ext.blocks > static_cast<uint64_t>(INT32_MAX) ||
      fec_ext.blocks > static_cast<uint64_t>(INT64_MAX) / block_size) {
    throwf("payload: partition %s: fec block count exceeds addressable range",
           part.name.c_str());
  }

  fec::Params params;
  params.data_blocks = static_cast<int>(data_ext.blocks);
  params.roots = static_cast<int>(part.fec.roots);
  // Give every worker several batches while keeping each batch's scratch
  // buffers bounded (128 rounds is only 3 batches for a 256 MiB partition).
  const int total_rounds = params.rounds();
  const int workers = static_cast<int>(std::max<size_t>(1, pool.size()));
  int width = fec::kDefaultBatchRounds;
  const int target_batches = workers * 4;
  width = std::min(width, std::max(1, total_rounds / target_batches));
  const int memory_cap =
      std::max(1, (16 << 20) / (fec::kBlockSize * (1 + params.roots)));
  width = std::min(width, memory_cap);
  params.batch_rounds = std::max(1, width);
  params.validate();
  const uint64_t want = static_cast<uint64_t>(params.rounds()) * params.roots;
  if (fec_ext.blocks != want) {
    throwf("payload: partition %s: fec extent has %llu blocks, expected %llu "
           "(rounds=%d, fec_roots=%u)",
           part.name.c_str(), static_cast<unsigned long long>(fec_ext.blocks),
           static_cast<unsigned long long>(want), params.rounds(),
           part.fec.roots);
  }
  if (extents_overlap(part.fec.fec_extent, part.fec.data_extent)) {
    throwf("payload: partition %s: fec extent overlaps fec data extent",
           part.name.c_str());
  }
  if (extents_overlap(part.fec.fec_extent, part.hash_tree.tree_extent)) {
    throwf("payload: partition %s: fec extent overlaps hash tree extent",
           part.name.c_str());
  }
  if ((data_ext.start + data_ext.blocks) * block_size > part.new_size ||
      (fec_ext.start + fec_ext.blocks) * block_size > part.new_size) {
    throwf("payload: partition %s: fec extent ends beyond partition size",
           part.name.c_str());
  }

  fec::generate(pool, out,
                static_cast<int64_t>(data_ext.start * block_size),
                static_cast<int64_t>(fec_ext.start * block_size), params,
                progress);
}

}  // namespace

bool write_verity(ThreadPool& pool, OutFile& out, const Partition& part,
                  uint64_t block_size, bool no_fec,
                  const std::function<void(int, int)>& fec_progress) {
  if (part.hash_tree.has_tree &&
      total_blocks(part.hash_tree.tree_extent) > 0) {
    write_hash_tree(pool, out, part, block_size);
  }
  if (!part.fec.has_fec || total_blocks(part.fec.fec_extent) == 0) {
    return false;
  }
  if (no_fec) return true;
  write_fec(pool, out, part, block_size, fec_progress);
  return false;
}

}  // namespace pd

#pragma once

#include <cstdint>
#include <functional>

#include "pool.h"

namespace pd {

class OutFile;

namespace fec {

constexpr int kBlockSize = 4096;
constexpr int kDefaultBatchRounds = 128;

struct Params {
  int data_blocks = 0;
  int roots = 2;
  int batch_rounds = kDefaultBatchRounds;

  int rsn() const { return 255 - roots; }
  int rounds() const;
  int batch_width() const;
  int batches() const;
  int64_t parity_bytes() const;
  void validate() const;
};

// Encodes parity for `data_blocks` 4 KiB blocks starting at data_offset and
// writes it at fec_offset. The on-disk layout is [round][byte][root], matching
// Android's libfec interleaving.
void generate(ThreadPool& pool, OutFile& out, int64_t data_offset,
              int64_t fec_offset, const Params& params,
              const std::function<void(int, int)>& progress);

}  // namespace fec
}  // namespace pd

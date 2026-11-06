#pragma once

#include <cstdint>
#include <vector>

#include "util.h"

namespace pd {

struct Extent {
  uint64_t start_block = 0;
  uint64_t num_blocks = 0;
};
using Extents = std::vector<Extent>;

constexpr uint64_t kSparseHoleBlock = UINT64_MAX;

uint64_t total_blocks(const Extents& extents);
void validate_extents(const Extents& extents, uint64_t block_size);
// If every extent is empty or the extents describe one contiguous run,
// returns true and the first block index.
bool extents_span(const Extents& extents, uint64_t block_size,
                  uint64_t* start_block, uint64_t* bytes);

class ExtentWriter {
 public:
  ExtentWriter(OutFile& out, const Extents& extents, uint64_t block_size);
  void write(const void* data, size_t len);
  uint64_t written() const { return written_; }

 private:
  OutFile& out_;
  const Extents& extents_;
  uint64_t block_size_;
  size_t idx_ = 0;
  uint64_t consumed_ = 0;
  uint64_t written_ = 0;
};

// Scatter-gather read of source extents into a contiguous buffer.
void read_extents(const uint8_t* src, uint64_t src_size,
                  const Extents& extents, uint64_t block_size, uint8_t* dst,
                  uint64_t dst_len);

struct BlockRange {
  uint64_t start;
  uint64_t end;
};
std::vector<BlockRange> uncovered_ranges(const Extents& covered,
                                         uint64_t total_blocks);

}  // namespace pd

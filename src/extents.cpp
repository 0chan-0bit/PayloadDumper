#include "extents.h"

#include <algorithm>
#include <cstring>

namespace pd {

uint64_t total_blocks(const Extents& extents) {
  uint64_t n = 0;
  for (const auto& e : extents) n += e.num_blocks;
  return n;
}

void validate_extents(const Extents& extents, uint64_t block_size) {
  for (const auto& e : extents) {
    if (e.start_block == kSparseHoleBlock) {
      throw Error("payload: sparse hole extents are not supported");
    }
    if (e.num_blocks == 0) continue;
    uint64_t end = e.start_block + e.num_blocks;
    if (end < e.start_block ||
        end > static_cast<uint64_t>(INT64_MAX) / block_size) {
      throwf("payload: extent out of addressable range (start_block=%llu "
             "num_blocks=%llu)",
             static_cast<unsigned long long>(e.start_block),
             static_cast<unsigned long long>(e.num_blocks));
    }
  }
}

bool extents_span(const Extents& extents, uint64_t block_size,
                  uint64_t* start_block, uint64_t* bytes) {
  bool have = false;
  bool contiguous = true;
  uint64_t start = 0;
  uint64_t next = 0;
  uint64_t total = 0;
  for (const auto& e : extents) {
    if (e.num_blocks == 0) continue;
    if (!have) {
      start = e.start_block;
      next = e.start_block + e.num_blocks;
      have = true;
    } else {
      if (e.start_block != next) contiguous = false;
      next = e.start_block + e.num_blocks;
    }
    total += e.num_blocks;
  }
  *start_block = start;
  *bytes = total * block_size;
  return contiguous;
}

ExtentWriter::ExtentWriter(OutFile& out, const Extents& extents,
                           uint64_t block_size)
    : out_(out), extents_(extents), block_size_(block_size) {}

void ExtentWriter::write(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (len > 0) {
    while (idx_ < extents_.size() &&
           consumed_ >= extents_[idx_].num_blocks * block_size_) {
      idx_++;
      consumed_ = 0;
    }
    if (idx_ >= extents_.size()) {
      throw Error("payload: attempted to write past destination extents");
    }
    const Extent& e = extents_[idx_];
    uint64_t extent_bytes = e.num_blocks * block_size_;
    size_t chunk = static_cast<size_t>(
        std::min<uint64_t>(len, extent_bytes - consumed_));
    out_.write_at(e.start_block * block_size_ + consumed_, p, chunk);
    consumed_ += chunk;
    written_ += chunk;
    p += chunk;
    len -= chunk;
  }
}

void read_extents(const uint8_t* src, uint64_t src_size,
                  const Extents& extents, uint64_t block_size, uint8_t* dst,
                  uint64_t dst_len) {
  uint64_t out_off = 0;
  for (const auto& e : extents) {
    uint64_t bytes = e.num_blocks * block_size;
    if (bytes == 0) continue;
    uint64_t off = e.start_block * block_size;
    if (off > src_size || bytes > src_size - off) {
      throwf("payload: source data ends before extent (offset=%llu "
             "length=%llu)",
             static_cast<unsigned long long>(off),
             static_cast<unsigned long long>(bytes));
    }
    if (out_off + bytes > dst_len) {
      throw Error("payload: source extent span exceeds destination buffer");
    }
    memcpy(dst + out_off, src + off, bytes);
    out_off += bytes;
  }
  if (out_off != dst_len) {
    throwf("payload: read %llu source bytes, expected %llu",
           static_cast<unsigned long long>(out_off),
           static_cast<unsigned long long>(dst_len));
  }
}

std::vector<BlockRange> uncovered_ranges(const Extents& covered,
                                         uint64_t total) {
  std::vector<BlockRange> ranges;
  ranges.reserve(covered.size());
  for (const auto& e : covered) {
    if (e.num_blocks == 0) continue;
    ranges.push_back({e.start_block, e.start_block + e.num_blocks});
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const BlockRange& a, const BlockRange& b) {
              return a.start < b.start;
            });
  std::vector<BlockRange> gaps;
  uint64_t cursor = 0;
  for (const auto& r : ranges) {
    if (r.start > cursor) {
      uint64_t end = std::min(r.start, total);
      if (end > cursor) gaps.push_back({cursor, end});
    }
    cursor = std::max(cursor, r.end);
    if (cursor >= total) return gaps;
  }
  if (cursor < total) gaps.push_back({cursor, total});
  return gaps;
}

}  // namespace pd

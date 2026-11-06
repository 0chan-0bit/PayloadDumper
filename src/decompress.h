#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pd {

struct Span {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

enum class Compression {
  kRaw = 0,
  kBzip2 = 1,
  kBrotli = 2,
  kXz = 3,
  kZstd = 4,
};

// Push-style destination used by streaming decompressors. prepare() returns a
// writable buffer and its capacity; commit() consumes the first n bytes.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual uint8_t* prepare(size_t* capacity) = 0;
  virtual void commit(size_t n) = 0;
  virtual uint64_t produced() const = 0;
};

void decompress(Compression type, Span src, Sink& sink);

// Pull-style reader for the three bspatch streams.
class ByteReader {
 public:
  virtual ~ByteReader() = default;
  // Reads up to n bytes; returns 0 only at end of stream.
  virtual size_t read(uint8_t* dst, size_t n) = 0;
};

std::unique_ptr<ByteReader> make_bz2_reader(Span src);
std::unique_ptr<ByteReader> make_brotli_reader(Span src);

}  // namespace pd

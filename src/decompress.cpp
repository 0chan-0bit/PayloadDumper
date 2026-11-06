#include "decompress.h"

#include <bzlib.h>
#include <brotli/decode.h>
#include <lzma.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include "util.h"

namespace pd {
namespace {

[[noreturn]] void corrupt(const char* what) {
  throwf("decompression failed (%s)", what);
}

void decompress_raw(Span src, Sink& sink) {
  size_t off = 0;
  while (off < src.size) {
    size_t cap = 0;
    uint8_t* dst = sink.prepare(&cap);
    if (cap == 0) throw Error("decompression: destination made no progress");
    size_t n = std::min(cap, src.size - off);
    memcpy(dst, src.data + off, n);
    sink.commit(n);
    off += n;
  }
}

void decompress_bz2(Span src, Sink& sink) {
  bz_stream zs{};
  if (BZ2_bzDecompressInit(&zs, 0, 0) != BZ_OK) corrupt("bz2 init");
  struct Guard {
    bz_stream* s;
    ~Guard() { BZ2_bzDecompressEnd(s); }
  } guard{&zs};

  size_t in_off = 0;
  bool eof = false;
  for (;;) {
    if (zs.avail_in == 0 && in_off < src.size) {
      size_t chunk = std::min<size_t>(src.size - in_off, 1u << 30);
      zs.next_in = const_cast<char*>(reinterpret_cast<const char*>(src.data + in_off));
      zs.avail_in = static_cast<unsigned int>(chunk);
      in_off += chunk;
    }
    size_t cap = 0;
    uint8_t* dst = sink.prepare(&cap);
    if (cap == 0) throw Error("decompression: destination made no progress");
    unsigned int give = static_cast<unsigned int>(std::min<size_t>(cap, 1u << 30));
    zs.next_out = reinterpret_cast<char*>(dst);
    zs.avail_out = give;
    int ret = BZ2_bzDecompress(&zs);
    size_t produced = give - zs.avail_out;
    if (produced) sink.commit(produced);
    if (ret == BZ_STREAM_END) {
      eof = true;
      break;
    }
    if (ret != BZ_OK) corrupt("bz2 stream");
    if (zs.avail_in == 0 && in_off >= src.size && produced == 0) {
      corrupt("bz2 truncated stream");
    }
  }
  (void)eof;
}

struct XzCtx {
  lzma_stream stream = LZMA_STREAM_INIT;
  uint64_t allocations = 0;
  uint64_t allocated_bytes = 0;
  uint64_t operations = 0;
  lzma_allocator allocator{};
  bool profile = std::getenv("PD_PROFILE") != nullptr;
  XzCtx() {
    if (profile) {
      allocator.opaque = this;
      allocator.alloc = [](void* opaque, size_t n, size_t size) -> void* {
        if (size && n > SIZE_MAX / size) return nullptr;
        auto& ctx = *static_cast<XzCtx*>(opaque);
        ++ctx.allocations;
        ctx.allocated_bytes += n * size;
        return std::malloc(n * size);
      };
      allocator.free = [](void*, void* p) { std::free(p); };
      stream.allocator = &allocator;
    }
  }
  ~XzCtx() {
    lzma_end(&stream);
    if (profile) fprintf(stderr, "profile xz: operations=%llu allocations=%llu allocated_bytes=%llu\n",
                         static_cast<unsigned long long>(operations),
                         static_cast<unsigned long long>(allocations),
                         static_cast<unsigned long long>(allocated_bytes));
  }
};

void decompress_xz(Span src, Sink& sink) {
  thread_local XzCtx holder;
  lzma_stream& zs = holder.stream;
  ++holder.operations;
  // liblzma permits reinitialization without lzma_end, retaining reusable
  // coder allocations. Buffer fields are not reset by the initializer.
  zs.next_in = nullptr;
  zs.avail_in = 0;
  zs.next_out = nullptr;
  zs.avail_out = 0;
  if (lzma_stream_decoder(&zs, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) {
    corrupt("xz init");
  }

  size_t in_off = 0;
  for (;;) {
    if (zs.avail_in == 0 && in_off < src.size) {
      zs.next_in = src.data + in_off;
      zs.avail_in = src.size - in_off;
      in_off = src.size;
    }
    size_t cap = 0;
    uint8_t* dst = sink.prepare(&cap);
    if (cap == 0) throw Error("decompression: destination made no progress");
    zs.next_out = dst;
    zs.avail_out = cap;
    lzma_ret ret = lzma_code(&zs, in_off >= src.size ? LZMA_FINISH : LZMA_RUN);
    size_t produced = cap - zs.avail_out;
    if (produced) sink.commit(produced);
    if (ret == LZMA_STREAM_END) break;
    if (ret != LZMA_OK) corrupt("xz stream");
    if (zs.avail_in == 0 && in_off >= src.size && produced == 0) {
      corrupt("xz truncated stream");
    }
  }
}

struct ZstdCtx {
  ZSTD_DCtx* ctx = ZSTD_createDCtx();
  ~ZstdCtx() { ZSTD_freeDCtx(ctx); }
};

void decompress_zstd(Span src, Sink& sink) {
  thread_local ZstdCtx holder;
  if (holder.ctx == nullptr) throw Error("decompression: cannot allocate zstd context");
  ZSTD_DCtx* ctx = holder.ctx;
  ZSTD_DCtx_reset(ctx, ZSTD_reset_session_only);

  size_t in_off = 0;
  for (;;) {
    if (in_off < src.size) {
      ZSTD_inBuffer in{src.data + in_off, src.size - in_off, 0};
      size_t cap = 0;
      uint8_t* dst = sink.prepare(&cap);
      if (cap == 0) throw Error("decompression: destination made no progress");
      ZSTD_outBuffer out{dst, cap, 0};
      size_t ret = ZSTD_decompressStream(ctx, &out, &in);
      in_off += in.pos;
      if (out.pos) sink.commit(out.pos);
      if (ZSTD_isError(ret)) corrupt("zstd stream");
      if (ret == 0) return;  // end of frame
      if (in.pos == in.size && out.pos == 0) corrupt("zstd truncated stream");
    } else {
      // Drain any buffered output after input is exhausted.
      size_t cap = 0;
      uint8_t* dst = sink.prepare(&cap);
      if (cap == 0) throw Error("decompression: destination made no progress");
      ZSTD_inBuffer in{nullptr, 0, 0};
      ZSTD_outBuffer out{dst, cap, 0};
      size_t ret = ZSTD_decompressStream(ctx, &out, &in);
      if (out.pos) sink.commit(out.pos);
      if (ZSTD_isError(ret)) corrupt("zstd stream");
      if (ret == 0) return;
      if (out.pos == 0) corrupt("zstd truncated stream");
    }
  }
}

void decompress_brotli(Span src, Sink& sink) {
  BrotliDecoderState* st = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
  if (st == nullptr) corrupt("brotli init");
  struct Guard {
    BrotliDecoderState* s;
    ~Guard() { BrotliDecoderDestroyInstance(s); }
  } guard{st};

  size_t in_off = 0;
  size_t avail_in = 0;
  const uint8_t* next_in = nullptr;
  for (;;) {
    if (avail_in == 0 && in_off < src.size) {
      size_t chunk = std::min<size_t>(src.size - in_off, 1u << 30);
      next_in = src.data + in_off;
      avail_in = chunk;
      in_off += chunk;
    }
    size_t cap = 0;
    uint8_t* dst = sink.prepare(&cap);
    if (cap == 0) throw Error("decompression: destination made no progress");
    size_t avail_out = cap;
    uint8_t* next_out = dst;
    BrotliDecoderResult ret = BrotliDecoderDecompressStream(
        st, &avail_in, &next_in, &avail_out, &next_out, nullptr);
    size_t produced = cap - avail_out;
    if (produced) sink.commit(produced);
    if (ret == BROTLI_DECODER_RESULT_SUCCESS) return;
    if (ret != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT &&
        ret != BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
      corrupt("brotli stream");
    }
    if (ret == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && avail_in == 0 &&
        in_off >= src.size) {
      corrupt("brotli truncated stream");
    }
  }
}

class Bz2Reader : public ByteReader {
 public:
  explicit Bz2Reader(Span src) : src_(src) {
    if (BZ2_bzDecompressInit(&zs_, 0, 0) != BZ_OK) corrupt("bz2 init");
    ok_ = true;
  }
  ~Bz2Reader() override {
    if (ok_) BZ2_bzDecompressEnd(&zs_);
  }
  size_t read(uint8_t* dst, size_t n) override {
    size_t total = 0;
    while (total < n) {
      if (zs_.avail_in == 0 && in_off_ < src_.size) {
        size_t chunk = std::min<size_t>(src_.size - in_off_, 1u << 30);
        zs_.next_in = const_cast<char*>(
            reinterpret_cast<const char*>(src_.data + in_off_));
        zs_.avail_in = static_cast<unsigned int>(chunk);
        in_off_ += chunk;
      }
      unsigned int give =
          static_cast<unsigned int>(std::min<size_t>(n - total, 1u << 30));
      zs_.next_out = reinterpret_cast<char*>(dst + total);
      zs_.avail_out = give;
      int ret = BZ2_bzDecompress(&zs_);
      size_t produced = give - zs_.avail_out;
      total += produced;
      if (ret == BZ_STREAM_END) {
        eof_ = true;
        break;
      }
      if (ret != BZ_OK) corrupt("bz2 patch stream");
      if (produced == 0 && zs_.avail_in == 0 && in_off_ >= src_.size) {
        corrupt("bz2 patch stream truncated");
      }
    }
    return total;
  }

 private:
  Span src_;
  bz_stream zs_{};
  size_t in_off_ = 0;
  bool ok_ = false;
  bool eof_ = false;
};

class BrotliReader : public ByteReader {
 public:
  explicit BrotliReader(Span src) : src_(src) {
    st_ = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (st_ == nullptr) corrupt("brotli init");
  }
  ~BrotliReader() override {
    if (st_) BrotliDecoderDestroyInstance(st_);
  }
  size_t read(uint8_t* dst, size_t n) override {
    size_t total = 0;
    while (total < n) {
      if (avail_in_ == 0 && in_off_ < src_.size) {
        size_t chunk = std::min<size_t>(src_.size - in_off_, 1u << 30);
        next_in_ = src_.data + in_off_;
        avail_in_ = chunk;
        in_off_ += chunk;
      }
      size_t avail_out = n - total;
      uint8_t* next_out = dst + total;
      BrotliDecoderResult ret = BrotliDecoderDecompressStream(
          st_, &avail_in_, &next_in_, &avail_out, &next_out, nullptr);
      size_t produced = (n - total) - avail_out;
      total += produced;
      if (ret == BROTLI_DECODER_RESULT_SUCCESS) {
        eof_ = true;
        break;
      }
      if (ret != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT &&
          ret != BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
        corrupt("brotli patch stream");
      }
      if (ret == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && avail_in_ == 0 &&
          in_off_ >= src_.size) {
        corrupt("brotli patch stream truncated");
      }
    }
    return total;
  }

 private:
  Span src_;
  BrotliDecoderState* st_ = nullptr;
  size_t in_off_ = 0;
  size_t avail_in_ = 0;
  const uint8_t* next_in_ = nullptr;
  bool eof_ = false;
};

}  // namespace

void decompress(Compression type, Span src, Sink& sink) {
  switch (type) {
    case Compression::kRaw:
      decompress_raw(src, sink);
      break;
    case Compression::kBzip2:
      decompress_bz2(src, sink);
      break;
    case Compression::kBrotli:
      decompress_brotli(src, sink);
      break;
    case Compression::kXz:
      decompress_xz(src, sink);
      break;
    case Compression::kZstd:
      decompress_zstd(src, sink);
      break;
  }
}

std::unique_ptr<ByteReader> make_bz2_reader(Span src) {
  return std::make_unique<Bz2Reader>(src);
}

std::unique_ptr<ByteReader> make_brotli_reader(Span src) {
  return std::make_unique<BrotliReader>(src);
}

}  // namespace pd

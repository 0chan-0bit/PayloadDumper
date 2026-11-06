#include "bspatch.h"

#include <cstring>
#include <limits>

#include "util.h"

namespace pd {
namespace {

constexpr size_t kHeaderLen = 32;

int64_t parse_int64(const uint8_t* b) {
  uint64_t v = static_cast<uint64_t>(b[0]) | (static_cast<uint64_t>(b[1]) << 8) |
               (static_cast<uint64_t>(b[2]) << 16) |
               (static_cast<uint64_t>(b[3]) << 24) |
               (static_cast<uint64_t>(b[4]) << 32) |
               (static_cast<uint64_t>(b[5]) << 40) |
               (static_cast<uint64_t>(b[6]) << 48) |
               (static_cast<uint64_t>(b[7]) << 56);
  int64_t value = static_cast<int64_t>(v & 0x7FFFFFFFFFFFFFFFull);
  if (b[7] & 0x80) value = -value;
  return value;
}

void read_full(ByteReader& r, uint8_t* dst, size_t len, const char* what) {
  size_t off = 0;
  while (off < len) {
    size_t n = r.read(dst + off, len - off);
    if (n == 0) throwf("bspatch: truncated %s stream", what);
    off += n;
  }
}

std::unique_ptr<ByteReader> open_stream(Compression type, Span data) {
  switch (type) {
    case Compression::kBzip2:
      return make_bz2_reader(data);
    case Compression::kBrotli:
      return make_brotli_reader(data);
    default:
      throwf("bspatch: unsupported compressor type %d",
             static_cast<int>(type));
  }
}

}  // namespace

void apply_bsdiff(const uint8_t* old, size_t old_size, Span patch,
                  uint64_t max_new_size, std::vector<uint8_t>& out) {
  if (patch.size < kHeaderLen) {
    throwf("bspatch: patch too small (%zu bytes)", patch.size);
  }
  Compression ctrl_type;
  Compression diff_type;
  Compression extra_type;
  if (memcmp(patch.data, "BSDIFF40", 8) == 0) {
    ctrl_type = diff_type = extra_type = Compression::kBzip2;
  } else if (memcmp(patch.data, "BSDF2", 5) == 0) {
    auto map = [](uint8_t t) {
      switch (t) {
        case 1:
          return Compression::kBzip2;
        case 2:
          return Compression::kBrotli;
        default:
          throwf("bspatch: unsupported compressor type %u", t);
      }
    };
    ctrl_type = map(patch.data[5]);
    diff_type = map(patch.data[6]);
    extra_type = map(patch.data[7]);
  } else {
    throw Error("bspatch: unrecognized patch magic");
  }

  int64_t ctrl_len = parse_int64(patch.data + 8);
  int64_t diff_len = parse_int64(patch.data + 16);
  int64_t new_size = parse_int64(patch.data + 24);
  if (ctrl_len < 0 || diff_len < 0 || new_size < 0 ||
      static_cast<int64_t>(patch.size) - static_cast<int64_t>(kHeaderLen) <
          ctrl_len ||
      static_cast<int64_t>(patch.size) - static_cast<int64_t>(kHeaderLen) -
              ctrl_len <
          diff_len) {
    throwf("bspatch: corrupt header (ctrl=%lld diff=%lld new_size=%lld "
           "patch=%zu)",
           static_cast<long long>(ctrl_len), static_cast<long long>(diff_len),
           static_cast<long long>(new_size), patch.size);
  }
  if (static_cast<uint64_t>(new_size) > max_new_size) {
    throwf("bspatch: patch output size %lld exceeds destination capacity %llu",
           static_cast<long long>(new_size),
           static_cast<unsigned long long>(max_new_size));
  }

  Span ctrl_span{patch.data + kHeaderLen, static_cast<size_t>(ctrl_len)};
  Span diff_span{patch.data + kHeaderLen + ctrl_len,
                 static_cast<size_t>(diff_len)};
  Span extra_span{patch.data + kHeaderLen + ctrl_len + diff_len,
                  patch.size - kHeaderLen - static_cast<size_t>(ctrl_len) -
                      static_cast<size_t>(diff_len)};

  auto ctrl = open_stream(ctrl_type, ctrl_span);
  auto diff = open_stream(diff_type, diff_span);
  auto extra = open_stream(extra_type, extra_span);

  out.assign(static_cast<size_t>(new_size), 0);
  int64_t old_pos = 0;
  int64_t new_pos = 0;
  const int64_t old_sz = static_cast<int64_t>(old_size);
  uint8_t ctrl_buf[24];

  while (new_pos < new_size) {
    read_full(*ctrl, ctrl_buf, sizeof(ctrl_buf), "control");
    int64_t diff_size = parse_int64(ctrl_buf);
    int64_t extra_size = parse_int64(ctrl_buf + 8);
    int64_t offset_increment = parse_int64(ctrl_buf + 16);
    if (diff_size < 0 || extra_size < 0) {
      throw Error("bspatch: corrupt patch (negative control entry sizes)");
    }
    if (diff_size > new_size - new_pos) {
      throw Error("bspatch: corrupt patch (diff block past output end)");
    }
    read_full(*diff, out.data() + new_pos, static_cast<size_t>(diff_size),
              "diff");
    for (int64_t i = 0; i < diff_size; ++i) {
      int64_t p = old_pos + i;
      if (p >= 0 && p < old_sz) {
        out[static_cast<size_t>(new_pos + i)] += old[static_cast<size_t>(p)];
      }
    }
    new_pos += diff_size;
    if (old_pos > std::numeric_limits<int64_t>::max() - diff_size) {
      throw Error("bspatch: corrupt patch (old position overflow)");
    }
    old_pos += diff_size;
    if (extra_size > new_size - new_pos) {
      throw Error("bspatch: corrupt patch (extra block past output end)");
    }
    read_full(*extra, out.data() + new_pos, static_cast<size_t>(extra_size),
              "extra");
    new_pos += extra_size;
    if (offset_increment > 0 &&
        old_pos > std::numeric_limits<int64_t>::max() - offset_increment) {
      throw Error("bspatch: corrupt patch (old position overflow)");
    }
    old_pos += offset_increment;
  }
}

}  // namespace pd

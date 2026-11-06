#include "zip.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <cstring>
#include <vector>

namespace pd {
namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kZip64LocatorSig = 0x07064b50;
constexpr uint32_t kZip64EocdSig = 0x06064b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;

uint16_t rd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t rd64(const uint8_t* p) {
  return static_cast<uint64_t>(rd32(p)) |
         (static_cast<uint64_t>(rd32(p + 4)) << 32);
}

std::string temp_template() {
  const char* dir = getenv("TMPDIR");
  if (dir == nullptr || *dir == '\0') dir = "/tmp";
  return std::string(dir) + "/payload_XXXXXX";
}

}  // namespace

bool looks_like_zip(const uint8_t* data, size_t size) {
  return size >= 4 && data[0] == 'P' && data[1] == 'K' &&
         (data[2] == 3 || data[2] == 5 || data[2] == 7);
}

ZipArchive::ZipArchive(const uint8_t* data, size_t size)
    : data_(data), size_(size) {
  parse();
}

void ZipArchive::parse() {
  if (size_ < 22) throw Error("zip: archive is too small");
  // Locate the end-of-central-directory record within the last 64 KiB (+22).
  size_t search_end = size_;
  size_t search_start = size_ > 65557 ? size_ - 65557 : 0;
  size_t eocd = std::string::npos;
  for (size_t i = search_end - 22 + 1; i-- > search_start;) {
    if (rd32(data_ + i) == kEocdSig) {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string::npos) throw Error("zip: end-of-central-directory not found");

  uint64_t total_entries = rd16(data_ + eocd + 10);
  uint64_t cd_size = rd32(data_ + eocd + 12);
  uint64_t cd_offset = rd32(data_ + eocd + 16);

  if (total_entries == 0xFFFF || cd_size == 0xFFFFFFFF ||
      cd_offset == 0xFFFFFFFF) {
    if (eocd < 20 || rd32(data_ + eocd - 20) != kZip64LocatorSig) {
      throw Error("zip: zip64 locator missing");
    }
    uint64_t z64_off = rd64(data_ + eocd - 20 + 8);
    if (z64_off > size_ || size_ - z64_off < 56 ||
        rd32(data_ + z64_off) != kZip64EocdSig) {
      throw Error("zip: invalid zip64 end-of-central-directory");
    }
    total_entries = rd64(data_ + z64_off + 32);
    cd_size = rd64(data_ + z64_off + 40);
    cd_offset = rd64(data_ + z64_off + 48);
  }

  if (cd_offset > size_ || cd_size > size_ - cd_offset) {
    throw Error("zip: central directory is out of bounds");
  }
  const uint8_t* p = data_ + cd_offset;
  const uint8_t* end = p + cd_size;
  entries_.reserve(static_cast<size_t>(total_entries));
  for (uint64_t i = 0; i < total_entries; ++i) {
    if (p + 46 > end || rd32(p) != kCentralSig) {
      throw Error("zip: corrupt central directory entry");
    }
    ZipEntry e;
    e.method = rd16(p + 10);
    e.crc32 = rd32(p + 16);
    e.compressed_size = rd32(p + 20);
    e.uncompressed_size = rd32(p + 24);
    uint16_t name_len = rd16(p + 28);
    uint16_t extra_len = rd16(p + 30);
    uint16_t comment_len = rd16(p + 32);
    uint64_t local_off = rd32(p + 42);
    if (p + 46 + name_len + extra_len > end) {
      throw Error("zip: truncated central directory entry");
    }
    e.name.assign(reinterpret_cast<const char*>(p + 46), name_len);

    const uint8_t* extra = p + 46 + name_len;
    size_t extra_rem = extra_len;
    while (extra_rem >= 4) {
      uint16_t id = rd16(extra);
      uint16_t len = rd16(extra + 2);
      if (static_cast<size_t>(len) + 4 > extra_rem) break;
      if (id == 0x0001) {
        const uint8_t* q = extra + 4;
        size_t rem = len;
        if (e.uncompressed_size == 0xFFFFFFFF && rem >= 8) {
          e.uncompressed_size = rd64(q);
          q += 8;
          rem -= 8;
        }
        if (e.compressed_size == 0xFFFFFFFF && rem >= 8) {
          e.compressed_size = rd64(q);
          q += 8;
          rem -= 8;
        }
        if (local_off == 0xFFFFFFFF && rem >= 8) {
          local_off = rd64(q);
          q += 8;
          rem -= 8;
        }
      }
      extra += 4 + len;
      extra_rem -= 4 + len;
    }

    if (local_off > size_ || size_ - local_off < 30 ||
        rd32(data_ + local_off) != kLocalSig) {
      throw Error("zip: invalid local file header for " + e.name);
    }
    uint16_t local_name_len = rd16(data_ + local_off + 26);
    uint16_t local_extra_len = rd16(data_ + local_off + 28);
    e.data_offset = local_off + 30 + local_name_len + local_extra_len;
    if (e.data_offset > size_ ||
        e.compressed_size > size_ - e.data_offset) {
      throw Error("zip: data for " + e.name + " is out of bounds");
    }
    entries_.push_back(std::move(e));
    p += 46 + name_len + extra_len + comment_len;
  }
}

const ZipEntry* ZipArchive::find(const std::string& name) const {
  for (const auto& e : entries_) {
    if (e.name == name) return &e;
  }
  return nullptr;
}

const ZipEntry* ZipArchive::find_payload() const {
  if (const ZipEntry* e = find("payload.bin")) return e;
  for (const auto& e : entries_) {
    if (e.uncompressed_size == 0) continue;
    size_t slash = e.name.rfind('/');
    const std::string base = slash == std::string::npos ? e.name : e.name.substr(slash + 1);
    if (base == "payload.bin") return &e;
  }
  return nullptr;
}

MappedFile ZipArchive::read_entry(const ZipEntry& entry) const {
  if (entry.method == 0) {
    // Stored: alias the archive mapping through a section-like wrapper. We
    // cannot return a subrange MappedFile without owning a base pointer, so
    // copy the range into a temporary file mapping? Better: the caller can use
    // data_offset directly. This function is only called for deflated entries.
    throw Error("zip: read_entry called for a stored entry");
  }
  if (entry.method != 8) {
    throwf("zip: unsupported compression method %u for %s", entry.method,
           entry.name.c_str());
  }

  std::string tmpl = temp_template();
  std::vector<char> path(tmpl.begin(), tmpl.end());
  path.push_back('\0');
  int fd = mkstemp(path.data());
  if (fd < 0) throwf("zip: cannot create temporary file: %s", strerror(errno));

  z_stream zs{};
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
    close(fd);
    unlink(path.data());
    throw Error("zip: inflateInit2 failed");
  }
  const uint8_t* in = data_ + entry.data_offset;
  size_t in_left = static_cast<size_t>(entry.compressed_size);
  std::vector<uint8_t> out(1 << 20);
  uint64_t written = 0;
  uLong crc = crc32(0L, Z_NULL, 0);
  int ret = Z_OK;
  while (in_left > 0 && ret != Z_STREAM_END) {
    if (zs.avail_in == 0) {
      uInt chunk = in_left > (1u << 30) ? (1u << 30) : static_cast<uInt>(in_left);
      zs.next_in = const_cast<Bytef*>(in);
      zs.avail_in = chunk;
      in += chunk;
      in_left -= chunk;
    }
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&zs);
      close(fd);
      unlink(path.data());
      throw Error("zip: inflate failed while extracting payload.bin");
    }
    size_t produced = out.size() - zs.avail_out;
    if (produced > 0) {
      crc = crc32(crc, out.data(), static_cast<uInt>(produced));
      pwrite_full(fd, out.data(), produced, written, "payload.bin (zip)");
      written += produced;
    }
  }
  inflateEnd(&zs);
  if (written != entry.uncompressed_size) {
    close(fd);
    unlink(path.data());
    throwf("zip: payload.bin inflated to %llu bytes, expected %llu",
           static_cast<unsigned long long>(written),
           static_cast<unsigned long long>(entry.uncompressed_size));
  }
  if (static_cast<uint32_t>(crc) != entry.crc32) {
    close(fd);
    unlink(path.data());
    throw Error("zip: payload.bin CRC32 mismatch");
  }
  if (ftruncate(fd, static_cast<off_t>(written)) != 0) {
    close(fd);
    unlink(path.data());
    throwf("zip: cannot size temporary payload: %s", strerror(errno));
  }
  return MappedFile::from_fd(fd, written, true, path.data());
}

}  // namespace pd

#pragma once

#include <cstdint>
#include <string>

#include "util.h"

namespace pd {

struct ZipEntry {
  std::string name;
  uint16_t method = 0;
  uint64_t compressed_size = 0;
  uint64_t uncompressed_size = 0;
  uint32_t crc32 = 0;
  uint64_t data_offset = 0;
};

class ZipArchive {
 public:
  // Does not take ownership of the mapping; it must outlive the archive.
  ZipArchive(const uint8_t* data, size_t size);

  // Finds "payload.bin" at the root first, then any entry whose basename is
  // payload.bin. Returns nullptr when absent.
  const ZipEntry* find_payload() const;

  // Returns a read-only mapping of the entry contents. For stored entries the
  // returned mapping aliases the archive; for deflated entries the data is
  // inflated into an unlinked temporary file.
  MappedFile read_entry(const ZipEntry& entry) const;

 private:
  void parse();
  const ZipEntry* find(const std::string& name) const;

  const uint8_t* data_;
  size_t size_;
  std::vector<ZipEntry> entries_;
};

bool looks_like_zip(const uint8_t* data, size_t size);

}  // namespace pd

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace pd {

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void throwf(const char* fmt, ...);

std::string hex(const uint8_t* data, size_t len);
std::string hex(const std::vector<uint8_t>& data);
std::string human_bytes(uint64_t bytes);
std::string format_timestamp();

// Read-only mmap of a whole file (or an owned fd). Empty files map to
// data()==nullptr with size()==0.
class MappedFile {
 public:
  MappedFile() = default;
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;

  static MappedFile open(const std::string& path);
  // Takes ownership of fd. The file is unlinked immediately if cleanup is set.
  static MappedFile from_fd(int fd, uint64_t size, bool unlink_path = false,
                            const std::string& path = {});

  const uint8_t* data() const { return data_; }
  uint8_t* mutable_data() { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  void reset();

 private:
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

// Writable output image backed by mmap when possible, with pwrite fallback.
class OutFile {
 public:
  OutFile() = default;
  ~OutFile();
  OutFile(const OutFile&) = delete;
  OutFile& operator=(const OutFile&) = delete;
  OutFile(OutFile&& other) noexcept;
  OutFile& operator=(OutFile&& other) noexcept;

  static OutFile create(const std::string& path, uint64_t size);

  uint64_t size() const { return size_; }
  const std::string& path() const { return path_; }
  bool mapped() const { return data_ != nullptr; }
  const uint8_t* mapped_data() const { return data_; }
  uint8_t* mapped_mutable_data() { return data_; }

  void write_at(uint64_t off, const void* src, size_t len);
  void read_at(uint64_t off, void* dst, size_t len) const;
  // Flushes and unmaps. Returns false if an I/O error was observed.
  bool finish();

 private:
  std::string path_;
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  uint64_t size_ = 0;
  bool ok_ = true;
};

inline uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

uint64_t now_ms();
bool stop_requested();
void request_stop();

void pread_full(int fd, void* dst, size_t len, uint64_t off, const char* what);
void pwrite_full(int fd, const void* src, size_t len, uint64_t off, const char* what);

}  // namespace pd

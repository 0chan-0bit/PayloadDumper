#include "util.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <linux/falloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace pd {

void throwf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  throw Error(buf);
}

std::string hex(const uint8_t* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out[2 * i] = kHex[data[i] >> 4];
    out[2 * i + 1] = kHex[data[i] & 0xf];
  }
  return out;
}

std::string hex(const std::vector<uint8_t>& data) {
  return hex(data.data(), data.size());
}

std::string human_bytes(uint64_t bytes) {
  static const char* units[] = {"B", "kB", "MB", "GB", "TB", "PB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1000.0 && unit < 5) {
    value /= 1000.0;
    ++unit;
  }
  char buf[64];
  if (unit == 0) {
    snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  } else if (value < 10.0) {
    snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
  } else {
    snprintf(buf, sizeof(buf), "%.0f %s", value, units[unit]);
  }
  return buf;
}

std::string format_timestamp() {
  using clock = std::chrono::system_clock;
  std::time_t t = clock::to_time_t(clock::now());
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
  return buf;
}

MappedFile::~MappedFile() { reset(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(other.data_), size_(other.size_) {
  other.data_ = nullptr;
  other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    reset();
    data_ = other.data_;
    size_ = other.size_;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

void MappedFile::reset() {
  if (data_ != nullptr) {
    munmap(data_, size_);
    data_ = nullptr;
  }
  size_ = 0;
}

MappedFile MappedFile::open(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throwf("cannot open %s: %s", path.c_str(), strerror(errno));
  }
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    int err = errno;
    close(fd);
    throwf("cannot stat %s: %s", path.c_str(), strerror(err));
  }
  if (st.st_size <= 0) {
    close(fd);
    return MappedFile();
  }
  void* p = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE,
                 fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    throwf("cannot mmap %s: %s", path.c_str(), strerror(errno));
  }
  MappedFile m;
  m.data_ = static_cast<uint8_t*>(p);
  m.size_ = static_cast<size_t>(st.st_size);
  return m;
}

MappedFile MappedFile::from_fd(int fd, uint64_t size, bool unlink_path,
                               const std::string& path) {
  MappedFile m;
  if (size > 0) {
    void* p = mmap(nullptr, static_cast<size_t>(size), PROT_READ, MAP_PRIVATE, fd,
                   0);
    if (p == MAP_FAILED) {
      int err = errno;
      if (unlink_path) unlink(path.c_str());
      close(fd);
      throwf("cannot mmap %s: %s", path.c_str(), strerror(err));
    }
    m.data_ = static_cast<uint8_t*>(p);
    m.size_ = static_cast<size_t>(size);
  }
  if (unlink_path) unlink(path.c_str());
  close(fd);
  return m;
}

OutFile::~OutFile() { finish(); }

OutFile::OutFile(OutFile&& other) noexcept
    : path_(std::move(other.path_)),
      fd_(other.fd_),
      data_(other.data_),
      size_(other.size_),
      ok_(other.ok_) {
  other.fd_ = -1;
  other.data_ = nullptr;
  other.size_ = 0;
}

OutFile& OutFile::operator=(OutFile&& other) noexcept {
  if (this != &other) {
    finish();
    path_ = std::move(other.path_);
    fd_ = other.fd_;
    data_ = other.data_;
    size_ = other.size_;
    ok_ = other.ok_;
    other.fd_ = -1;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

OutFile OutFile::create(const std::string& path, uint64_t size) {
  OutFile f;
  f.path_ = path;
  f.fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (f.fd_ < 0) {
    throwf("cannot create %s: %s", path.c_str(), strerror(errno));
  }
  if (size > 0) {
    if (ftruncate(f.fd_, static_cast<off_t>(size)) != 0) {
      int err = errno;
      close(f.fd_);
      f.fd_ = -1;
      unlink(path.c_str());
      throwf("cannot size %s to %llu bytes: %s", path.c_str(),
             static_cast<unsigned long long>(size), strerror(err));
    }
    // Reserve space up front. Mapping is only used when the reservation
    // succeeded: a dirty mmap page on a full filesystem raises SIGBUS instead
    // of returning ENOSPC, while the pwrite fallback reports the error.
    const bool reserved =
        fallocate(f.fd_, 0, 0, static_cast<off_t>(size)) == 0;
    if (reserved) {
      void* p = mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE,
                     MAP_SHARED, f.fd_, 0);
      if (p != MAP_FAILED) {
        f.data_ = static_cast<uint8_t*>(p);
      }
    }
  }
  f.size_ = size;
  return f;
}

void OutFile::write_at(uint64_t off, const void* src, size_t len) {
  if (len == 0) return;
  if (off > size_ || len > size_ - off) {
    throwf("write past end of %s (offset=%llu length=%zu size=%llu)",
           path_.c_str(), static_cast<unsigned long long>(off), len,
           static_cast<unsigned long long>(size_));
  }
  if (data_ != nullptr) {
    memcpy(data_ + off, src, len);
    return;
  }
  pwrite_full(fd_, src, len, off, path_.c_str());
}

void OutFile::read_at(uint64_t off, void* dst, size_t len) const {
  if (len == 0) return;
  if (off > size_ || len > size_ - off) {
    throwf("read past end of %s (offset=%llu length=%zu size=%llu)", path_.c_str(),
           static_cast<unsigned long long>(off), len,
           static_cast<unsigned long long>(size_));
  }
  if (data_ != nullptr) {
    memcpy(dst, data_ + off, len);
    return;
  }
  pread_full(fd_, dst, len, off, path_.c_str());
}

bool OutFile::finish() {
  if (data_ != nullptr) {
    // Dirty pages are written back by the kernel; an explicit synchronous
    // msync would serialize the whole image and buy nothing here.
    if (madvise(data_, size_, MADV_NORMAL) != 0) {
      // Ignore: purely advisory.
    }
    munmap(data_, size_);
    data_ = nullptr;
  }
  if (fd_ >= 0) {
    if (close(fd_) != 0) ok_ = false;
    fd_ = -1;
  }
  return ok_;
}

uint64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

namespace {
volatile std::sig_atomic_t g_stop = 0;
}

bool stop_requested() { return g_stop != 0; }
void request_stop() { g_stop = 1; }

void pread_full(int fd, void* dst, size_t len, uint64_t off, const char* what) {
  uint8_t* p = static_cast<uint8_t*>(dst);
  while (len > 0) {
    ssize_t n = pread(fd, p, len, static_cast<off_t>(off));
    if (n < 0) {
      if (errno == EINTR) continue;
      throwf("read error on %s: %s", what, strerror(errno));
    }
    if (n == 0) throwf("unexpected end of %s", what);
    p += n;
    len -= static_cast<size_t>(n);
    off += static_cast<uint64_t>(n);
  }
}

void pwrite_full(int fd, const void* src, size_t len, uint64_t off,
                 const char* what) {
  const uint8_t* p = static_cast<const uint8_t*>(src);
  while (len > 0) {
    ssize_t n = pwrite(fd, p, len, static_cast<off_t>(off));
    if (n < 0) {
      if (errno == EINTR) continue;
      throwf("write error on %s: %s", what, strerror(errno));
    }
    p += n;
    len -= static_cast<size_t>(n);
    off += static_cast<uint64_t>(n);
  }
}

}  // namespace pd

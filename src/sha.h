#pragma once

#include <openssl/sha.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pd {

using Sha256Digest = std::array<uint8_t, 32>;
using Sha1Digest = std::array<uint8_t, 20>;

Sha256Digest sha256(const void* data, size_t len);
// Hashes salt || data || data2 (salt may be empty).
Sha256Digest sha256_concat(const void* a, size_t alen, const void* b, size_t blen);
Sha1Digest sha1_concat(const void* a, size_t alen, const void* b, size_t blen);

class Sha256Stream {
 public:
  Sha256Stream() { SHA256_Init(&ctx_); }
  void update(const void* data, size_t len) { SHA256_Update(&ctx_, data, len); }
  Sha256Digest finish() {
    Sha256Digest out{};
    SHA256_Final(out.data(), &ctx_);
    return out;
  }

 private:
  SHA256_CTX ctx_{};
};

bool digest_equal(const uint8_t* a, const uint8_t* b, size_t len);

}  // namespace pd

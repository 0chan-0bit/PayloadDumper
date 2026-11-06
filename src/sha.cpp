#include "sha.h"

#include <cstring>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace pd {

Sha256Digest sha256(const void* data, size_t len) {
  Sha256Digest out{};
  SHA256(static_cast<const unsigned char*>(data), len, out.data());
  return out;
}

Sha256Digest sha256_concat(const void* a, size_t alen, const void* b,
                           size_t blen) {
  Sha256Digest out{};
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  if (alen) SHA256_Update(&ctx, a, alen);
  if (blen) SHA256_Update(&ctx, b, blen);
  SHA256_Final(out.data(), &ctx);
  return out;
}

Sha1Digest sha1_concat(const void* a, size_t alen, const void* b, size_t blen) {
  Sha1Digest out{};
  SHA_CTX ctx;
  SHA1_Init(&ctx);
  if (alen) SHA1_Update(&ctx, a, alen);
  if (blen) SHA1_Update(&ctx, b, blen);
  SHA1_Final(out.data(), &ctx);
  return out;
}

bool digest_equal(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

}  // namespace pd

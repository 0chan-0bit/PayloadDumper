#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "decompress.h"

namespace pd {

// Applies a BSDIFF40 or BSDF2 patch. `old` may be null when old_size is zero.
// The decoded output is stored in `out` and must not exceed max_new_size.
void apply_bsdiff(const uint8_t* old, size_t old_size, Span patch,
                  uint64_t max_new_size, std::vector<uint8_t>& out);

}  // namespace pd

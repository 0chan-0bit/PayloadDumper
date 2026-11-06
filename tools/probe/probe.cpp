#include <cstdio>
#include <cstdint>
extern "C" uint64_t probe_sum(const uint64_t* p, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; ++i) s += p[i] * 3 + 1;
    return s;
}

#!/bin/bash
src="$(dirname "$0")/probe.cpp"
test_flag() {
  local desc="$1"; shift
  if g++ -std=c++20 "$@" -c "$src" -o /tmp/probe.o 2>/tmp/err; then
    echo "OK    | $desc | $*"
  else
    echo "FAIL  | $desc | $* | $(head -1 /tmp/err)"
  fi
}
test_flag "baseline" -O2
test_flag "O3" -O3
test_flag "Ofast" -Ofast
test_flag "march=native" -O3 -march=native
test_flag "mtune=znver4" -O3 -mtune=znver4
test_flag "avx512" -O3 -mavx512f -mavx512bw -mavx512vl -mavx512dq -mavx512vbmi -mavx512vnni
test_flag "lto-compile" -O3 -march=native -flto=auto -ffat-lto-objects
test_flag "pgo-generate" -O3 -fprofile-generate
test_flag "pgo-use" -O3 -fprofile-use
test_flag "fno-exceptions" -O3 -fno-exceptions
test_flag "fno-rtti" -O3 -fno-rtti
test_flag "fno-semantic-interposition" -O3 -fno-semantic-interposition
test_flag "gc-sections" -O3 -ffunction-sections -fdata-sections
test_flag "fno-plt" -O3 -fno-plt
test_flag "vector-width512" -O3 -mprefer-vector-width=512
test_flag "align-functions" -O3 -falign-functions=32
test_flag "unroll" -O3 -funroll-loops
test_flag "fast-math" -O3 -ffast-math
test_flag "fno-math-errno" -O3 -fno-math-errno
test_flag "c++23" -std=c++23 -O3
test_flag "std=c++17" -std=c++17 -O3
test_flag "fuse-ld=gold" -O3 -fuse-ld=gold
test_flag "fuse-ld=lld" -O3 -fuse-ld=lld
test_flag "fuse-ld=mold" -O3 -fuse-ld=mold
test_flag "static-libstdc++" -O3 -static-libstdc++
test_flag "native+64" -O3 -march=native -mtune=native
test_flag "lto thin" -O3 -flto=auto

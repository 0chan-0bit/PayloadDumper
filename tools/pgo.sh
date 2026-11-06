#!/bin/bash
# Profile-guided optimization build.
#
#   1. instrumented build into build/gen
#   2. run the end-to-end test suite to produce profiles
#   3. rebuild the normal target with those profiles
#
# For best results replace step 2 with a run over a representative OTA.
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
PROFILE="$ROOT/pgo-data"
BASE=(-std=c++20 -O3 -march=native -mtune=native -fno-plt
      -fno-semantic-interposition -ffunction-sections -fdata-sections
      -Wno-deprecated-declarations)

rm -rf "$PROFILE"
make clean
# Use the normal object paths: GCC names .gcda files after the object path, so
# the instrumented and final builds must agree on where objects live.
make -j"$(nproc)" \
  CXXFLAGS="${BASE[*]} -fprofile-generate=$PROFILE" \
  LDFLAGS="-flto=auto -fprofile-generate=$PROFILE -Wl,--gc-sections -Wl,-O1"

PD_BIN="$ROOT/build/payload-dumper" python3 tests/run_tests.py

make clean
make -j"$(nproc)" \
  CXXFLAGS="${BASE[*]} -flto=auto -fprofile-use=$PROFILE -fprofile-correction" \
  LDFLAGS="-flto=auto -fprofile-use=$PROFILE -fprofile-correction -Wl,--gc-sections -Wl,-O1"

echo "PGO build complete: build/payload-dumper"

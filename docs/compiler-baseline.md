# GCC / machine baseline

Recorded on the development machine (`lscpu`, `gcc -Q --help=*`, probe builds
under `tools/probe/`).

## Hardware

| Item | Value |
| --- | --- |
| CPU | AMD Ryzen 5 7600 (Zen 4), 6 cores / 12 threads |
| Topology | 1 socket, 1 NUMA node, 32 MiB L3, 6 MiB L2 |
| Vector ISA | AVX-512 F/BW/DQ/VL/VBMI/VBMI2/VNNI/BITALG/VPOPCNTDQ/IFMA/BF16 |
| Crypto | AES-NI, SHA-NI, VAES, VPCLMULQDQ |
| Storage | NVMe |

`gcc -march=native -Q --help=target` resolves the target to **`znver4`** and
enables, among others:

```
mavx2 mavx512f mavx512bw mavx512cd mavx512dq mavx512vl
mavx512vbmi mavx512vbmi2 mavx512vnni mavx512bitalg mavx512vpopcntdq
mavx512ifma mavx512bf16 msha mpclmul mpopcnt mbmi mbmi2 madx
```

`-mprefer-vector-width` is `none` (GCC's Zen 4 tuning chooses the width), so
the build does not force 512-bit vectors.

## Toolchain

| Tool | Version / availability |
| --- | --- |
| gcc / g++ | 14.2.0 (Debian 14.2.0-19) |
| GNU ld (bfd) | 2.44 |
| ld.gold / lld / mold | not installed (link probes fail) |
| GNU make | available |
| protoc | 3.21.12 (libprotobuf-lite 32.0.12) |
| LTO plugin | `liblto_plugin.so` present; `-flto=auto` compiles and links |
| PGO | `-fprofile-generate` / `-fprofile-use` round-trip verified |

## Flag support matrix

All of the following compile successfully with g++ 14.2:

```
-O2 -O3 -Ofast
-march=native -mtune=znver4 -march=native -mtune=native
-mavx512f -mavx512bw -mavx512vl -mavx512dq -mavx512vbmi -mavx512vnni
-flto=auto -fuse-linker-plugin -ffat-lto-objects
-fprofile-generate -fprofile-use
-fno-exceptions -fno-rtti -fno-semantic-interposition
-ffunction-sections -fdata-sections -fno-plt
-mprefer-vector-width=512 -falign-functions=32 -funroll-loops
-ffast-math -fno-math-errno
-std=c++17 -std=c++20 -std=c++23
-static-libstdc++
```

`-fuse-ld=gold|lld|mold` is accepted at compile time but **fails at link time**
because those linkers are not installed. The build therefore uses GNU ld.

## Chosen release flags

```
-std=c++20 -O3 -DNDEBUG -march=native -mtune=native
-fno-plt -fno-semantic-interposition
-ffunction-sections -fdata-sections
-flto=auto -fuse-linker-plugin
-Wl,--gc-sections -Wl,-O1 -Wl,--as-needed -Wl,-z,relro,-z,now
```

`-Ofast`/`-ffast-math` are *not* used: the hot paths are integer/memory
operations, and fast-math would only risk correctness.

`make PORTABLE=1` substitutes `-march=x86-64-v3 -mtune=generic` for
redistributable binaries.

## PGO

`tools/pgo.sh` performs an instrumented build, runs the end-to-end test suite
to collect profiles, and rebuilds with `-fprofile-use`. On real workloads, run
the instrumented binary against a representative OTA and then repeat the final
step for a better profile.

# payload-dumper-cpp

A high-performance C++20 reimplementation of
[payload-dumper-go](https://github.com/ssut/payload-dumper-go): it extracts
partition images from Android OTA `payload.bin` files, including incremental
(delta) payloads, and regenerates the dm-verity hash tree and FEC parity that
delta payloads omit.

```
payload-dumper [options] payload.bin|ota.zip
```

## Features

- Full and delta (incremental) payloads, with base images supplied via `-old`.
- Install operations: `REPLACE`, `REPLACE_BZ`, `REPLACE_XZ`, `ZSTD`, `ZERO`,
  `DISCARD`, `MOVE`, `BSDIFF`, `SOURCE_COPY`, `SOURCE_BSDIFF`,
  `BROTLI_BSDIFF` (both `BSDIFF40` and `BSDF2` patch containers).
- Rebuilds the dm-verity hash tree (SHA-1/SHA-256, salted) and Reed-Solomon
  FEC parity (roots 2..24) for delta payloads; output is bit-exact.
- Copies unmodified blocks from the source image for delta payloads.
- Verifies operation blobs, source images and final image SHA-256 hashes.
- Reads OTA zips in place when `payload.bin` is stored; deflated entries are
  inflated to an unlinked temp file and mmapped.
- Zip64 support.
- Parallel extraction with operation-level, not just partition-level,
  concurrency; direct decompression into the mmapped output image.
- Progress bars, machine-readable (`-m`) and quiet (`-q`) modes.
- No `avbtool`, `fec` or `xz` CLI dependencies.

Not implemented (same limitation as the Go reference): `PUFFDIFF`,
`ZUCCHINI`, `LZ4DIFF_BSDIFF`, `LZ4DIFF_PUFFDIFF`.

## Download (portable Linux build)

A statically linked single-file build for Linux x86-64 is published on the
[Releases](https://github.com/0chan-0bit/PayloadDumper/releases/latest) page:

- [`payload-dumper-portable-v1.0.tar.xz`](https://github.com/0chan-0bit/PayloadDumper/releases/download/portable/payload-dumper-portable-v1.0.tar.xz)
  (2.9 MB, SHA-256
  `25be88737513bbaf70bd955e8a0b550102dbd3de4a2fd62ddb48cc51bad9fa2c`)
- Contains a single `payload-dumper-portable` executable; no runtime shared
  libraries are required (`ldd` reports "not a dynamic executable").

```sh
curl -LO https://github.com/0chan-0bit/PayloadDumper/releases/download/portable/payload-dumper-portable-v1.0.tar.xz
echo "25be88737513bbaf70bd955e8a0b550102dbd3de4a2fd62ddb48cc51bad9fa2c  payload-dumper-portable-v1.0.tar.xz" | sha256sum -c -
tar -xJf payload-dumper-portable-v1.0.tar.xz
./payload-dumper-portable -l payload.bin
```

The released binary targets the x86-64-v3 baseline. See [Build](#build) to
rebuild it for another CPU or to create the portable binary locally.

## Build

Dependencies (Debian/Ubuntu package names):

```
build-essential protobuf-compiler libprotobuf-dev libzstd-dev liblzma-dev
libbz2-dev libbrotli-dev zlib1g-dev libssl-dev
```

Then:

```sh
make -j"$(nproc)"                               # native release -> build/payload-dumper
make BUILD=build/release -j"$(nproc)"           # isolated native release build
make portable -j"$(nproc)"                      # static single-file -> build/portable/payload-dumper
make BUILD=build/debug DEBUG=1 -j"$(nproc)"     # -O1 -g, no LTO
```

Release builds use `-O3 -DNDEBUG`, native tuning and LTO by default. The
isolated release binary is `build/release/payload-dumper`; it is dynamically
linked and targets the build machine's CPU. `NATIVE=0` selects x86-64-v3,
which still requires compatible hardware and runtime libraries. Use separate
`BUILD` directories when changing compiler flags: make does not track flag
changes as dependencies.

`make portable` (equivalent to `make PORTABLE=1`) builds a statically linked,
single-file executable at `build/portable/payload-dumper`: protobuf-lite,
zstd, liblzma, bzip2, brotli, zlib and OpenSSL are linked into the binary, so
`ldd` reports "not a dynamic executable". It defaults to `NATIVE=0`
(x86-64-v3); pass `NATIVE=1` for a static build tuned to the build host. The
result is self-contained for Linux (static glibc) but remains CPU-dependent
on x86-64-v3 unless rebuilt. Use `PORTABLE_BUILD=DIR` to choose another output
directory.

`protoc` is only needed to regenerate `src/proto/update_metadata.pb.{h,cc}`;
the generated sources are already in the tree.

### Toolchain baseline

This machine: AMD Ryzen 5 7600 (Zen 4, 12 threads), GCC 14.2.0, GNU ld 2.44.
`-march=native` resolves to `znver4` and enables AVX-512 (F/BW/DQ/VL/VBMI/
VNNI/VBMI2/VPOPCNTDQ/IFMA/BF16), SHA-NI, BMI2, etc. LTO and PGO are both
supported; `ld.gold`, `lld` and `mold` are **not** installed, so the default
GNU bfd linker is used.

Chosen release flags:

```
-std=c++20 -O3 -DNDEBUG -march=native -mtune=native
-fno-plt -fno-semantic-interposition -ffunction-sections -fdata-sections
-flto=auto -fuse-linker-plugin
-Wl,--gc-sections -Wl,-O1 -Wl,--as-needed -Wl,-z,relro,-z,now
```

See [docs/compiler-baseline.md](docs/compiler-baseline.md) for the full probe
(flag matrix, ISA features, linker/PGO results).

## Usage

```
Options:
  -c, -concurrency N     Workers to extract concurrently (default: CPUs)
  -l, -list              Show list of partitions in payload.bin
  -o, -output DIR        Set output directory
  -p, -partitions LIST   Dump only selected partitions (comma-separated)
  -old DIR               Directory with base images for incremental OTA
  -q, -quiet             Suppress non-essential output
  -m, -machine-readable  Machine-readable output format
  -no-verify             Skip sha256 verification
  -no-fec                Skip dm-verity FEC generation
```

`-c` controls extraction workers. A separate pool uses
`max(1, floor(workers / 2))` verification workers: `-c 12` means 12 extraction
workers plus 6 verification workers. Partition hashes run asynchronously once
their output is ready; operation-blob and operation-source hashes remain in
the extraction workers. `-no-verify` skips source/final-image verification;
operation-blob integrity checks remain enabled.

Examples:

```sh
payload-dumper -o out ota.zip
payload-dumper -l payload.bin
payload-dumper -p boot,system -o out payload.bin

# Incremental OTA: extract the base first, then apply the delta on top of it.
payload-dumper -o base_images base_full_ota.zip
payload-dumper -old base_images -o new_images incremental_ota.zip
```

`-m` prints `<partition>:<percent>` lines to stdout; progress bars use stderr.
After successful extraction, normal mode prints `Elapsed time: X.XXX s`,
measured with a monotonic clock from opening the input through extraction,
verification and output closure. `-q` and `-m` suppress this summary; listing
partitions does not print extraction timing.
`-no-fec` skips FEC generation and, because the image then does not match its
declared SHA-256, also skips final image verification. Such images must not be
flashed.

### Graphical wrapper (optional)

[`ui/`](ui/README.md) contains a GTK4 / libadwaita front-end for interactive
use: partition checkboxes with filtering, per-partition progress bars fed by
the `-m` output, the advanced options (`-c`, `-old`, `-no-verify`, `-no-fec`),
a collapsible log and a stop button. It needs PyGObject plus GTK 4 and
libadwaita (Debian/Ubuntu: `python3-gi gir1.2-gtk-4.0 gir1.2-adw-1`).

```sh
./ui/payload-dumper-gui   # or: make gui
```

The wrapper finds `build/release/payload-dumper`, then `build/payload-dumper`,
then `payload-dumper` on `PATH`; `PD_BIN` or the in-window settings can
override that.

## Performance

### Real OTA payload (2026-11-07)

Ryzen 5 7600 (6 cores/12 threads), GCC 14.2.0, NVMe, liblzma 5.8.1,
OpenSSL 3.5.7. Input: 9,550,378,792 bytes; 67 output images totaling
12,958,371,840 bytes. Operations: 5,583 XZ, 350 raw, 193 ZERO and 95 Bzip2;
no old images or verity/FEC rebuilding are required by this sample.

| Version | Extraction / verification workers | Wall time | Output throughput |
| --- | --- | --- | --- |
| Current C++ release source | 12 / 6 | 22.754 s (one run) | 569.5 MB/s |
| Earlier C++ bidirectional operation scheduler | 12 / shared extraction pool | 21.874 s (one run) | 592.4 MB/s |
| Go reference, commit `c3a50ba8d784764c06143c1f82971cd81a77f9d6`, Go 1.27.0 | 12 / shared concurrency | 60.466 s (median of 3) | 214.3 MB/s |

All runs enable default verification and quiet mode, create fresh output
files, and read the complete input before timing. Input page residency was
100% immediately before each timed run. Timing includes process startup,
extraction and internal verification, but excludes prewarming, independent
hashing and cleanup. Both implementations return without `fsync`; these are
process-completion times, not durable-write timings. Runs were collected at
different times on an active desktop, with no control over clock speed,
background load or writeback. The current source did not improve on the earlier
21.874-second single result; extra verification threads showed no measurable
improvement over the preceding 22.755-second run. These single-run differences
do not establish a statistically significant speedup or regression.

Independent SHA-256 checks matched all 67 images against the earlier C++ and
Go outputs. Benchmark output images are deleted after successful runs; JSONL
results and hashes are retained.

Reproduce the current C++ measurement (the Go argument is required by the
shared benchmark script but is not executed with `--implementations cpp`):

```sh
python3 tests/bench_real.py --cpp build/release/payload-dumper \
  --go build/payload-dumper-go --implementations cpp --workers 12 \
  --runs 1 --warm-input --work benchmarks/release-real
```

Choose a new `--work` directory for each invocation. See
[the optimization report](docs/five-optimizations.zh-CN.md) and
[the latest comparison](benchmarks/vfy-12w6vw/comparison.json) for details.

### Historical synthetic benchmarks

The following figures were recorded before the current scheduling and
verification changes and have not been remeasured for this release.

Measured on the machine above with `make bench`; payloads are incompressible
(`/dev/urandom`) so decompression does real work. Wall time includes writing
the images and the default SHA-256 verification (except the `-no-verify` row).

512 MiB ZSTD payload, 8 partitions, 12 threads:

| workers | best time | throughput |
| --- | --- | --- |
| 1 | 0.70 s | 763 MB/s |
| 6 | 0.21 s | 2.5 GB/s |
| 12 | 0.16 s | 3.3 GB/s |
| 12, `-no-verify` | 0.13 s | 4.2 GB/s |

256 MiB XZ payload:

| workers | best time | throughput |
| --- | --- | --- |
| 1 | 0.38 s | 714 MB/s |
| 12 | 0.09 s | 3.0 GB/s |

256 MiB delta-style image with SHA-256 hash tree + FEC roots=2 regeneration
(raw data writes + verity): **~2.2 GB/s** with 12 workers.

These are synthetic page-cache numbers; run `make bench` (or
`python3 tests/bench.py --codec xz`) for your own hardware.

### What makes it fast

- `mmap` for the payload, source images and output images; no read/write
  syscalls on the mapped data path; page faults and kernel writeback still
  perform IO, and mapping failures use a checked read/write fallback.
- Operation-level work distribution: independent install operations from the
  same partition run concurrently, so a single huge partition still saturates
  all cores. Operations read the immutable source image and write disjoint
  extents. With 4 or more workers, `floor(workers / 4)` workers take operations
  from partitions in descending output-size order; the remaining workers use
  ascending order. Both groups share operation cursors, so they can converge
  on the last partition without duplicating work. Within each partition,
  operations are stably ordered by estimated cost (output bytes weighted by
  codec); this is a heuristic, not an exact time estimate. Below 4 workers,
  scheduling follows manifest-order FIFO.
  Workers claim only one operation at a time: pending operations are never
  reserved by a busy worker. An idle worker can immediately help with the
  remaining operations of a partition being processed by the other group.
  An operation already running inside a decoder is not preempted or split.
- Contiguous `REPLACE` destinations are decompressed **directly into the
  output mapping**; scattered extents use a reusable 4 MiB scatter buffer.
- `libzstd`/`liblzma`/`libbz2`/`libbrotlidec` streaming decoders with
  thread-local ZSTD contexts and reusable XZ streams.
- SHA-256 via OpenSSL (SHA-NI accelerated), operation blobs hashed directly
  from the input mapping. Partitions that need no verity/FEC rebuilding are verified by
  a dedicated verification pool as soon as their last operation completes.
  The pool has `max(1, floor(workers / 2))` extra threads, so `-c 12` uses
  12 extraction workers plus 6 verification workers. Whole-source and final
  output verification use this pool; operation-blob hashes stay in extraction
  workers. Verification failures cancel extraction and drain both pools before
  output cleanup.
- Full-image ZERO/DISCARD operations retain the freshly truncated file's zero
  ranges without dirtying pages. Destination extents are checked for bounds
  and overlap before extraction; delta zero writes are retained.
- `parallel_for` uses only pool workers and blocks the caller on a condition
  variable, avoiding busy waiting and the former extra calling-thread worker.
- Hash tree levels are hashed in parallel; FEC batches are sized so that every
  worker gets several batches while each batch's scratch memory stays bounded.
- FEC uses per-tap GF(2^8) multiplication tables and a dedicated roots=2 path.
- `-march=native`, LTO and section GC; optional PGO via `tools/pgo.sh`.

## Tests

```sh
make test    # scheduler/thread-pool tests and synthetic end-to-end suite
PD_BIN="$PWD/build/release/payload-dumper" make BUILD=build/release test
make bench   # throughput benchmark
make pgo     # profile-guided rebuild (see below)
```

`tests/run_tests.py` generates payloads from `update_metadata.proto` and
compares every extracted image byte-for-byte. Coverage includes full payloads
with every supported replace codec, scattered destination extents, ZIP stored
and deflated inputs, delta payloads with `SOURCE_COPY`/`MOVE`/`BSDIFF`/
`BROTLI_BSDIFF`, source-image fallthrough, SHA-1/SHA-256 hash trees, FEC roots
2 and 4, partial updates, `-no-verify`/`-no-fec`, and negative cases (corrupt
blobs, wrong hashes, missing source, bad input, out-of-bounds/overlapping
destinations, and asynchronous verification failure cleanup). The current suite
passes 99 end-to-end checks, plus scheduler/thread-pool tests for exact-once
operation claiming, cost order, worker limits and exception propagation.
The FEC oracle in `tests/gen_payload.py` is an independent per-codeword polynomial-division
implementation, not a port of the C++ encoder.

## PGO

```sh
tools/pgo.sh
```

This builds an instrumented binary, runs the test suite to collect profiles,
and rebuilds the release binary with `-fprofile-use`. For production use,
replace the test run with a representative OTA extraction.

## License

Apache-2.0. `src/proto/update_metadata.proto` is derived from AOSP's
`update_engine` (Apache-2.0), via payload-dumper-go. The FEC encoder follows
the dm-verity format documented in AOSP `libfec`; no LGPL code was copied.

## Optional profiling

`PD_PROFILE=1 build/payload-dumper -q -c 12 -o out payload.bin` reports
extraction phase wall times, cumulative per-operation worker times, output
verification worker time, and per-thread XZ allocation counts/bytes. Worker
times overlap and are not CPU time. XZ uses allocator instrumentation only
in this mode; keep it disabled for normal benchmark comparisons.

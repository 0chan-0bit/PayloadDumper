#!/usr/bin/env python3
"""Throughput benchmark for the C++ payload dumper.

Generates a synthetic full OTA payload made of ZSTD-compressed 8 MiB chunks,
then measures wall-clock extraction throughput with different worker counts.
"""

import argparse
import os
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import gen_payload as g  # noqa: E402

BIN = os.environ.get("PD_BIN", os.path.join(ROOT, "build", "payload-dumper"))
MB = 1024 * 1024


def build_bench_payload(path, partitions, size_mb, chunk_mb, codec):
    if codec == "zstd":
        compress = g.zstd_compress
        op_type = g.u.InstallOperation.ZSTD
    elif codec == "xz":
        compress = g.xz_compress
        op_type = g.u.InstallOperation.REPLACE_XZ
    else:
        raise SystemExit(f"unknown codec {codec}")
    specs = []
    total = 0
    chunk = chunk_mb * MB
    for p in range(partitions):
        data = os.urandom(size_mb // partitions * MB)
        total += len(data)
        ops = []
        for off in range(0, len(data), chunk):
            piece = data[off : off + chunk]
            dst = [g.extent(off // g.BS, len(piece) // g.BS)]
            ops.append(g.op_replace(compress(piece), dst, op_type))
        specs.append(g.PartitionSpec(f"part{p:02d}", data, ops))
    g.write_payload(path, specs)
    return total


def time_run(args, repeats):
    times = []
    for _ in range(repeats):
        start = time.perf_counter()
        proc = subprocess.run(
            [BIN] + args,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        elapsed = time.perf_counter() - start
        if proc.returncode != 0:
            print(proc.stderr.decode(errors="replace")[-2000:])
            raise SystemExit(f"benchmark extraction failed: {args}")
        times.append(elapsed)
    return min(times), statistics.median(times)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size-mb", type=int, default=512, help="total uncompressed payload size")
    ap.add_argument("--partitions", type=int, default=8)
    ap.add_argument("--chunk-mb", type=int, default=8)
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--codec", choices=["zstd", "xz"], default="zstd")
    ap.add_argument("--verity", action="store_true",
                    help="benchmark hash-tree + FEC regeneration instead")
    args = ap.parse_args()

    work = os.path.join(ROOT, "tests", "bench-work")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    payload = os.path.join(work, "payload.bin")
    cpus = os.cpu_count() or 1

    if args.verity:
        print(f"generating a {args.size_mb} MiB verity benchmark payload ...")
        start = time.perf_counter()
        data_blocks = args.size_mb * MB // g.BS
        slot = 32
        per = g.BS // slot
        tree_blocks = 0
        b = data_blocks
        while True:
            b = (b + per - 1) // per
            tree_blocks += b
            if b <= 1:
                break
        fec_blocks = ((data_blocks + 252) // 253) * 2
        data = os.urandom(data_blocks * g.BS)
        image = data + bytes((tree_blocks + fec_blocks) * g.BS)
        ops = []
        chunk = args.chunk_mb * MB
        for off in range(0, len(data), chunk):
            piece = data[off : off + chunk]
            ops.append(
                g.op_replace(piece, [g.extent(off // g.BS, len(piece) // g.BS)],
                             g.u.InstallOperation.REPLACE)
            )
        spec = g.PartitionSpec(
            "system", image, ops,
            hash_tree={"data_blocks": data_blocks, "tree_blocks": tree_blocks,
                       "algorithm": "sha256", "salt": b"bench-salt"},
            fec={"data_blocks": data_blocks, "tree_blocks": tree_blocks,
                 "fec_blocks": fec_blocks, "roots": 2},
            omit_hash=True,
        )
        g.write_payload(payload, [spec])
        print(f"  payload: {os.path.getsize(payload)/MB:.1f} MiB on disk, "
              f"generated in {time.perf_counter()-start:.1f}s")
        out = os.path.join(work, "out_verity")
        best, median = time_run(["-q", "-c", str(cpus), "-o", out, payload], args.runs)
        shutil.rmtree(out, ignore_errors=True)
        shutil.rmtree(work, ignore_errors=True)
        print(f"  verity (sha256 + FEC roots=2) over {data_blocks*g.BS/MB:.0f} MiB:"
              f" best {best:6.2f}s  median {median:6.2f}s  "
              f"{data_blocks*g.BS/best/1e6:7.0f} MB/s")
        return

    print(f"generating {args.size_mb} MiB {args.codec} payload ...")
    start = time.perf_counter()
    total = build_bench_payload(
        payload, args.partitions, args.size_mb, args.chunk_mb, args.codec
    )
    print(f"  payload: {os.path.getsize(payload)/MB:.1f} MiB on disk, "
          f"{total/MB:.1f} MiB raw, generated in {time.perf_counter()-start:.1f}s")

    for workers in (1, cpus // 2, cpus):
        out = os.path.join(work, f"out_c{workers}")
        best, median = time_run(
            ["-q", "-c", str(workers), "-o", out, payload], args.runs
        )
        shutil.rmtree(out, ignore_errors=True)
        print(
            f"  -c {workers:2d}: best {best:6.2f}s  median {median:6.2f}s  "
            f"{total / best / 1e6:7.0f} MB/s (verify enabled)"
        )

    out = os.path.join(work, "out_noverify")
    best, median = time_run(
        ["-q", "-no-verify", "-c", str(cpus), "-o", out, payload], args.runs
    )
    shutil.rmtree(out, ignore_errors=True)
    print(
        f"  -c {cpus:2d} -no-verify: best {best:6.2f}s  median {median:6.2f}s  "
        f"{total / best / 1e6:7.0f} MB/s"
    )
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()

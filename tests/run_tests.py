#!/usr/bin/env python3
"""End-to-end tests for the C++ payload dumper.

Builds synthetic payloads (see gen_payload.py) and compares every extracted
image byte-for-byte with the expected image, including delta operations and
regenerated dm-verity hash tree/FEC parity.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import gen_payload as g  # noqa: E402

BIN = os.environ.get("PD_BIN", os.path.join(ROOT, "build", "payload-dumper"))
BS = g.BS
PASS = 0
FAIL = 0
CURRENT = ""


def check(condition, message):
    global PASS, FAIL
    if condition:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL [{CURRENT}] {message}")


def run(args, cwd=None, expect=0, timeout=180):
    proc = subprocess.run(
        [BIN] + args,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    if proc.returncode != expect:
        print(f"  command: {' '.join(args)}")
        print(f"  rc={proc.returncode} expect={expect}")
        print(f"  stdout={proc.stdout.decode(errors='replace')[-2000:]}")
        print(f"  stderr={proc.stderr.decode(errors='replace')[-3000:]}")
    return proc


def compare_images(expected, outdir):
    for name, data in expected.items():
        path = os.path.join(outdir, name + ".img")
        if not os.path.exists(path):
            check(False, f"missing output {name}.img")
            continue
        with open(path, "rb") as f:
            actual = f.read()
        if actual != data:
            first = next(
                (i for i in range(min(len(actual), len(data))) if actual[i] != data[i]),
                min(len(actual), len(data)),
            )
            check(
                False,
                f"{name}.img differs at byte {first} (actual {len(actual)} bytes, "
                f"expected {len(data)})",
            )
        else:
            check(True, f"{name}.img")


def write_old_images(olddir, old):
    os.makedirs(olddir, exist_ok=True)
    for name, data in old.items():
        with open(os.path.join(olddir, name + ".img"), "wb") as f:
            f.write(data)


def blk(data, start, num):
    return data[start * BS : (start + num) * BS]


# ------------------------------------------------------------------ fixtures

def fixture_full(directory, bad_hash=False):
    payload = os.path.join(directory, "payload.bin")
    boot = bytearray(8 * BS)
    boot[0:2 * BS] = os.urandom(2 * BS)
    boot[2 * BS:4 * BS] = os.urandom(2 * BS)
    boot[4 * BS:6 * BS] = os.urandom(2 * BS)
    boot[6 * BS:8 * BS] = bytes(2 * BS)

    vendor = bytearray(6 * BS)
    vendor[0:3 * BS] = os.urandom(3 * BS)
    vendor[3 * BS:4 * BS] = os.urandom(BS)
    vendor[4 * BS:5 * BS] = os.urandom(BS)
    vendor[5 * BS:6 * BS] = os.urandom(BS)

    boot_ops = [
        g.op_replace(blk(boot, 0, 2), [g.extent(0, 2)], g.u.InstallOperation.REPLACE),
        g.op_replace(g.zstd_compress(blk(boot, 2, 2)), [g.extent(2, 2)], g.u.InstallOperation.ZSTD),
        g.op_replace(g.xz_compress(blk(boot, 4, 2)), [g.extent(4, 2)], g.u.InstallOperation.REPLACE_XZ),
        g.op_zero([g.extent(6, 1)]),
        {"type": g.u.InstallOperation.DISCARD, "blob": b"", "dst": [g.extent(7, 1)]},
    ]
    # Two raw ops with scattered destination extents.
    vendor_ops = [
        g.op_replace(g.bz2_compress(blk(vendor, 0, 3)), [g.extent(0, 3)], g.u.InstallOperation.REPLACE_BZ),
        g.op_replace(blk(vendor, 3, 1) + blk(vendor, 5, 1), [g.extent(3, 1), g.extent(5, 1)], g.u.InstallOperation.REPLACE),
        g.op_replace(blk(vendor, 4, 1), [g.extent(4, 1)], g.u.InstallOperation.REPLACE),
    ]
    parts = [
        g.PartitionSpec("boot", bytes(boot), boot_ops),
        g.PartitionSpec("vendor", bytes(vendor), vendor_ops),
        g.PartitionSpec("empty", bytes(3 * BS), []),
    ]
    expected = g.write_payload(payload, parts)
    if bad_hash:
        # Corrupt the manifest's stored new-hash for boot.
        with open(payload, "rb") as f:
            raw = bytearray(f.read())
        manifest_len = struct.unpack(">Q", raw[12:20])[0]
        manifest = raw[24 : 24 + manifest_len]
        import update_metadata_pb2 as u
        m = u.DeltaArchiveManifest()
        m.ParseFromString(manifest)
        m.partitions[0].new_partition_info.hash = b"\x00" * 32
        new_manifest = m.SerializeToString()
        raw[12:20] = struct.pack(">Q", len(new_manifest))
        raw[24 : 24 + len(new_manifest)] = new_manifest
        with open(payload, "wb") as f:
            f.write(raw)
    return payload, expected


def fixture_delta(directory, with_verity=False, roots=2, algorithm="sha256", salt=b"verity-salt"):
    payload = os.path.join(directory, "payload.bin")
    data_blocks = 254
    old = bytes(os.urandom(data_blocks * BS))
    new = bytearray(data_blocks * BS)

    # 0..99 source copy, 100..139 bsdiff, 140..199 replace, 200..223 zero,
    # 224..253 source copy.
    new[0:100 * BS] = old[0:100 * BS]
    new[224 * BS:254 * BS] = old[224 * BS:254 * BS]
    new[140 * BS:200 * BS] = os.urandom(60 * BS)
    new[220 * BS:224 * BS] = old[220 * BS:224 * BS]

    ops = [
        g.op_copy([g.extent(0, 100)], [g.extent(0, 100)], src_hash=g.sha256(old[0:100 * BS])),
        g.op_bsdiff(
            old[100 * BS:140 * BS],
            bytes(new[100 * BS:140 * BS]),
            [g.extent(100, 40)],
            [g.extent(100, 40)],
            "bz2",
            g.u.InstallOperation.SOURCE_BSDIFF,
        ),
        g.op_replace(g.zstd_compress(bytes(new[140 * BS:200 * BS])), [g.extent(140, 60)], g.u.InstallOperation.ZSTD),
        g.op_zero([g.extent(200, 20)]),
        g.op_copy([g.extent(224, 30)], [g.extent(224, 30)], src_hash=g.sha256(old[224 * BS:254 * BS])),
    ]
    new[200 * BS:220 * BS] = bytes(20 * BS)

    expected = bytes(new)
    tree_blocks = 0
    fec_blocks = 0
    if with_verity:
        expected, tree_blocks, fec_blocks = g.verity_regions(
            bytes(new), algorithm=algorithm, salt=salt, roots=roots
        )

    if with_verity:
        ht = {
            "data_blocks": data_blocks,
            "tree_blocks": tree_blocks,
            "algorithm": algorithm,
            "salt": salt,
        }
        fec = {
            "data_blocks": data_blocks,
            "tree_blocks": tree_blocks,
            "fec_blocks": fec_blocks,
            "roots": roots,
        }
        spec = g.PartitionSpec("system", expected, ops, old_data=old, hash_tree=ht, fec=fec)
        parts = [spec]
    else:
            # Blocks 220..223 are intentionally not covered: the extractor must
        # copy them from the source image (fallthrough).
        spec = g.PartitionSpec("system", expected, ops, old_data=old)
        parts = [spec]
    g.write_payload(payload, parts, minor_version=5)
    old_images = {"system": old}
    return payload, {"system": expected}, old_images


def fixture_ops(directory):
    """Exercises MOVE, legacy BSDIFF, BROTLI_BSDIFF and scattered src extents."""
    payload = os.path.join(directory, "payload.bin")
    blocks = 20
    old = bytes(os.urandom(blocks * BS))
    new = bytearray(old)

    new[4 * BS:6 * BS] = os.urandom(2 * BS)
    new[6 * BS:8 * BS] = os.urandom(2 * BS)
    new[8 * BS:9 * BS] = old[8 * BS:9 * BS]
    new[9 * BS:10 * BS] = old[10 * BS:11 * BS]
    new[10 * BS:12 * BS] = old[0:2 * BS]

    ops = [
        g.op_copy([g.extent(0, 2)], [g.extent(10, 2)],
                  src_hash=g.sha256(old[0:2 * BS]),
                  op_type=g.u.InstallOperation.MOVE),
        g.op_bsdiff(old[4 * BS:6 * BS], bytes(new[4 * BS:6 * BS]),
                    [g.extent(4, 2)], [g.extent(4, 2)], "bz2",
                    g.u.InstallOperation.BSDIFF),
        g.op_bsdiff(old[6 * BS:8 * BS], bytes(new[6 * BS:8 * BS]),
                    [g.extent(6, 2)], [g.extent(6, 2)], "brotli",
                    g.u.InstallOperation.BROTLI_BSDIFF),
        g.op_copy([g.extent(8, 1), g.extent(10, 1)], [g.extent(8, 2)]),
    ]
    spec = g.PartitionSpec("system", bytes(new), ops, old_data=old)
    g.write_payload(payload, [spec], minor_version=5)
    return payload, {"system": bytes(new)}, {"system": old}


def fixture_multi_delta(directory):
    payload = os.path.join(directory, "payload.bin")
    old_a = bytes(os.urandom(4 * BS))
    new_a = bytearray(old_a)
    new_a[0:BS] = os.urandom(BS)
    old_b = bytes(os.urandom(3 * BS))

    parts = [
        g.PartitionSpec(
            "a", bytes(new_a),
            [
                g.op_replace(g.zstd_compress(bytes(new_a[0:BS])), [g.extent(0, 1)],
                             g.u.InstallOperation.ZSTD),
                g.op_copy([g.extent(1, 1)], [g.extent(1, 1)]),
            ],
            old_data=old_a,
        ),
        g.PartitionSpec(
            "b", old_b,
            [g.op_replace(old_b, [g.extent(0, 3)], g.u.InstallOperation.REPLACE)],
            old_data=old_b,
        ),
    ]
    g.write_payload(payload, parts, minor_version=5)
    return payload, {"a": bytes(new_a), "b": old_b}, {"a": old_a, "b": old_b}


def fixture_partial(directory):
    payload = os.path.join(directory, "payload.bin")
    data = bytearray(4 * BS)
    data[0:2 * BS] = os.urandom(2 * BS)
    data[2 * BS:4 * BS] = bytes(2 * BS)
    ops = [
        g.op_replace(g.xz_compress(bytes(data[0:4 * BS])), [g.extent(0, 4)], g.u.InstallOperation.REPLACE_XZ),
    ]
    parts = [g.PartitionSpec("product", bytes(data), ops)]
    expected = g.write_payload(payload, parts, minor_version=2, partial_update=True)
    return payload, expected


def case_full(tmp):
    payload, expected = fixture_full(tmp)
    out = os.path.join(tmp, "out")
    proc = run(["-q", "-o", out, payload])
    check(proc.returncode == 0, "full payload extraction")
    compare_images(expected, out)

    # Exercise both scheduling paths and the four-worker threshold, including
    # non-multiples of four and empty partitions in the shared queue.
    for workers in (1, 3, 4, 5, 8):
        scheduled = os.path.join(tmp, f"out_workers_{workers}")
        proc = run(["-q", "-c", str(workers), "-o", scheduled, payload])
        check(proc.returncode == 0, f"extraction with {workers} workers")
        compare_images(expected, scheduled)

    # Listing.
    proc = run(["-m", "-l", payload])
    check(proc.returncode == 0, "machine list rc")
    lines = proc.stdout.decode().strip().splitlines()
    check("boot:32" in lines and "vendor:24" in lines and "empty:12" in lines,
          f"machine list lines: {lines}")

    # Selection.
    out2 = os.path.join(tmp, "out_select")
    proc = run(["-q", "-p", "boot", "-o", out2, payload])
    check(proc.returncode == 0, "selection extraction")
    check(os.path.exists(os.path.join(out2, "boot.img")), "selected boot exists")
    check(not os.path.exists(os.path.join(out2, "vendor.img")), "unselected vendor absent")

    # Machine-readable progress.
    out3 = os.path.join(tmp, "out_machine")
    proc = run(["-m", "-o", out3, payload])
    check(proc.returncode == 0, "machine extraction rc")
    text = proc.stdout.decode()
    for name in expected:
        check(f"{name}:100" in text, f"machine progress for {name}: {text!r}")
    compare_images(expected, out3)


def case_zip(tmp):
    payload, expected = fixture_full(tmp)
    for method, tag in [(zipfile.ZIP_STORED, "stored"), (zipfile.ZIP_DEFLATED, "deflated")]:
        zpath = os.path.join(tmp, f"ota_{tag}.zip")
        with zipfile.ZipFile(zpath, "w", method) as zf:
            zf.write(payload, "payload.bin")
        out = os.path.join(tmp, f"out_{tag}")
        proc = run(["-q", "-o", out, zpath])
        check(proc.returncode == 0, f"zip {tag} extraction")
        compare_images(expected, out)


def case_delta(tmp):
    payload, expected, old = fixture_delta(tmp)
    olddir = os.path.join(tmp, "old")
    write_old_images(olddir, old)
    for workers in (1, 8):
        out = os.path.join(tmp, f"out_{workers}")
        proc = run(["-q", "-c", str(workers), "-old", olddir, "-o", out, payload])
        check(proc.returncode == 0, f"delta extraction (-c {workers})")
        compare_images(expected, out)

    # Missing source must fail with a hint.
    out2 = os.path.join(tmp, "out_missing")
    proc = run(["-q", "-o", out2, payload], expect=1)
    check(b"Hint:" in proc.stderr, "missing source hint")
    check(not os.path.exists(os.path.join(out2, "system.img")), "failed output removed")


def case_verity(tmp, roots=2, algorithm="sha256"):
    payload, expected, old = fixture_delta(
        tmp, with_verity=True, roots=roots, algorithm=algorithm, salt=b"salt-" + bytes([roots])
    )
    olddir = os.path.join(tmp, "old")
    write_old_images(olddir, old)
    out = os.path.join(tmp, "out")
    proc = run(["-q", "-old", olddir, "-o", out, payload])
    check(proc.returncode == 0, f"verity delta extraction roots={roots} {algorithm}")
    compare_images(expected, out)

    # -no-fec skips parity generation and, with it, the final image hash check.
    out2 = os.path.join(tmp, "out_nofec")
    proc = run(["-q", "-no-fec", "-old", olddir, "-o", out2, payload])
    check(proc.returncode == 0, "-no-fec extraction")
    nofec_path = os.path.join(out2, "system.img")
    if os.path.exists(nofec_path):
        with open(nofec_path, "rb") as f:
            data = f.read()
        with open(payload, "rb") as f:
            raw = f.read()
        import struct
        import update_metadata_pb2 as u
        mlen = struct.unpack(">Q", raw[12:20])[0]
        manifest = u.DeltaArchiveManifest()
        manifest.ParseFromString(raw[24 : 24 + mlen])
        part = manifest.partitions[0]
        tree_start = part.hash_tree_extent.start_block * BS
        tree_end = tree_start + part.hash_tree_extent.num_blocks * BS
        fec_start = part.fec_extent.start_block * BS
        fec_end = fec_start + part.fec_extent.num_blocks * BS
        want = expected["system"]
        check(data[tree_start:tree_end] == want[tree_start:tree_end],
              "-no-fec still regenerates hash tree")
        check(data[fec_start:fec_end] == bytes(fec_end - fec_start),
              "-no-fec leaves FEC region unwritten")


def case_ops(tmp):
    payload, expected, old = fixture_ops(tmp)
    olddir = os.path.join(tmp, "old")
    write_old_images(olddir, old)
    out = os.path.join(tmp, "out")
    proc = run(["-q", "-c", "4", "-old", olddir, "-o", out, payload])
    check(proc.returncode == 0, "move/bsdiff/brotli extraction")
    compare_images(expected, out)


def case_multi_delta(tmp):
    payload, expected, old = fixture_multi_delta(tmp)
    olddir = os.path.join(tmp, "old")
    write_old_images(olddir, old)
    out = os.path.join(tmp, "out")
    proc = run(["-q", "-c", "4", "-old", olddir, "-o", out, payload])
    check(proc.returncode == 0, "multi-partition delta extraction")
    compare_images(expected, out)


def case_partial(tmp):
    payload, expected = fixture_partial(tmp)
    out = os.path.join(tmp, "out")
    proc = run(["-q", "-o", out, payload])
    check(proc.returncode == 0, "partial update extraction without -old")
    compare_images(expected, out)


def case_failures(tmp):
    payload, expected = fixture_full(tmp, bad_hash=True)
    out = os.path.join(tmp, "out")
    proc = run(["-q", "-o", out, payload], expect=1)
    check(b"output image verification failed" in proc.stderr, "bad hash detected")
    check(not os.path.exists(os.path.join(out, "boot.img")), "bad image removed")

    # Dedicated verification must drain safely before removing mappings,
    # including the minimum one-vfy-worker case and odd extraction counts.
    for workers in (1, 3, 12):
        failed_out = os.path.join(tmp, f"out_vfy_failure_{workers}")
        proc = run(["-q", "-c", str(workers), "-o", failed_out, payload], expect=1)
        check(proc.returncode == 1 and b"output image verification failed" in proc.stderr,
              f"asynchronous verification error with {workers} workers")
        check(not os.path.exists(failed_out) or not os.listdir(failed_out),
              "all images removed after verification drains")

    # -no-verify bypasses the final hash check.
    out2 = os.path.join(tmp, "out_noverify")
    proc = run(["-q", "-no-verify", "-o", out2, payload])
    check(proc.returncode == 0, "-no-verify extraction")
    for name, data in expected.items():
        with open(os.path.join(out2, name + ".img"), "rb") as f:
            check(f.read() == data, f"-no-verify {name} content")

    # Corrupted operation blob must be rejected.
    good_dir = os.path.join(tmp, "good")
    os.makedirs(good_dir, exist_ok=True)
    good, _ = fixture_full(good_dir)
    with open(good, "rb") as f:
        raw = bytearray(f.read())
    manifest_len = struct.unpack(">Q", raw[12:20])[0]
    data_start = 24 + manifest_len
    raw[data_start + 10] ^= 0xFF
    corrupt = os.path.join(tmp, "corrupt.bin")
    with open(corrupt, "wb") as f:
        f.write(raw)
    out3 = os.path.join(tmp, "out_corrupt")
    proc = run(["-q", "-o", out3, corrupt], expect=1)
    check(b"verification failed" in proc.stderr, "corrupt blob detected")

    # Unknown partition name.
    proc = run(["-q", "-p", "nope", "-o", os.path.join(tmp, "x"), payload], expect=1)
    check(b"not present" in proc.stderr, "unknown partition detected")

    # Unsupported payload major version.
    with open(good, "rb") as f:
        raw = bytearray(f.read())
    raw[4:12] = struct.pack(">Q", 3)
    badver = os.path.join(tmp, "badver.bin")
    with open(badver, "wb") as f:
        f.write(raw)
    proc = run([badver], expect=1)
    check(b"unsupported payload major version" in proc.stderr, "bad version detected")

    # Neither payload nor zip.
    bogus = os.path.join(tmp, "bogus.bin")
    with open(bogus, "wb") as f:
        f.write(b"not a payload at all")
    proc = run([bogus], expect=1)
    check(b"neither a payload" in proc.stderr, "bogus input detected")


def case_many_operations(tmp):
    specs = []
    for name, blocks in (("medium", 25), ("large", 41), ("small", 17)):
        data = os.urandom(blocks * BS)
        ops = []
        for i in range(blocks):
            piece = data[i * BS:(i + 1) * BS]
            ops.append(g.op_replace(g.xz_compress(piece), [g.extent(i, 1)],
                                    g.u.InstallOperation.REPLACE_XZ))
        specs.append(g.PartitionSpec(name, data, ops))
    payload = os.path.join(tmp, "payload.bin")
    expected = g.write_payload(payload, specs)
    for workers in (4, 12):
        out = os.path.join(tmp, f"out_{workers}")
        proc = run(["-q", "-c", str(workers), "-o", out, payload])
        check(proc.returncode == 0, f"many operations with {workers} workers")
        compare_images(expected, out)


def case_destination_validation(tmp):
    payload, _ = fixture_full(tmp)
    with open(payload, "rb") as f:
        raw = f.read()
    manifest_len = struct.unpack(">Q", raw[12:20])[0]
    for kind in ("bounds", "overlap"):
        m = g.u.DeltaArchiveManifest()
        m.ParseFromString(raw[24:24 + manifest_len])
        op = m.partitions[0].operations[1]
        op.dst_extents[0].start_block = 8 if kind == "bounds" else 0
        encoded = m.SerializeToString()
        bad = os.path.join(tmp, f"{kind}.bin")
        with open(bad, "wb") as f:
            f.write(raw[:12] + struct.pack(">Q", len(encoded)) + raw[20:24]
                    + encoded + raw[24 + manifest_len:])
        for workers in (1, 12):
            out = os.path.join(tmp, f"{kind}_{workers}")
            proc = run(["-q", "-c", str(workers), "-o", out, bad], expect=1)
            message = b"exceeds partition" if kind == "bounds" else b"overlapping destination"
            check(proc.returncode == 1 and message in proc.stderr, f"reject {kind} with {workers} workers")
            check(not os.path.exists(out), "invalid destinations rejected before output creation")


CASES = [
    ("destination-validation", case_destination_validation),
    ("many-operations", case_many_operations),
    ("full", case_full),
    ("zip", case_zip),
    ("delta", case_delta),
    ("verity-sha256-roots2", lambda tmp: case_verity(tmp, 2, "sha256")),
    ("verity-sha1-roots4", lambda tmp: case_verity(tmp, 4, "sha1")),
    ("ops-move-bsdiff", case_ops),
    ("multi-delta", case_multi_delta),
    ("partial", case_partial),
    ("failures", case_failures),
]


def main():
    global CURRENT
    if not os.path.exists(BIN):
        print(f"missing binary: {BIN}; run make first")
        return 1
    keep = "--keep" in sys.argv
    base = os.path.join(ROOT, "tests", "work")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base, exist_ok=True)
    print(f"test directory: {base}")
    for name, fn in CASES:
        CURRENT = name
        print(f"[{name}]")
        tmp = os.path.join(base, name)
        os.makedirs(tmp, exist_ok=True)
        try:
            fn(tmp)
        except Exception as exc:  # keep going so all failures are visible
            check(False, f"unexpected exception: {exc!r}")
            import traceback
            traceback.print_exc()
    print(f"\n{PASS} checks passed, {FAIL} failed")
    if FAIL == 0 and not keep:
        shutil.rmtree(base, ignore_errors=True)
    else:
        print(f"artifacts kept in {base}")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

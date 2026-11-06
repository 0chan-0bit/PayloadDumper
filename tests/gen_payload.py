"""Synthetic payload.bin generator used by the test suite.

The verity/FEC reference implementations here are intentionally written from
the format description (naive polynomial arithmetic, per-codeword encoding)
rather than mirroring src/fec.cpp, so agreement is a real cross-check.
"""

import bz2
import ctypes
import ctypes.util
import hashlib
import lzma
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GENERATED = os.path.join(HERE, "generated")
PROTO_DIR = os.path.join(ROOT, "src", "proto")
PROTO_CC = os.path.join(PROTO_DIR, "update_metadata.proto")
PB2 = os.path.join(GENERATED, "update_metadata_pb2.py")

# Make the suite self-contained on a clean checkout.
if not os.path.exists(PB2) or os.path.getmtime(PB2) < os.path.getmtime(PROTO_CC):
    os.makedirs(GENERATED, exist_ok=True)
    subprocess.run(
        ["protoc", f"-I{PROTO_DIR}", f"--python_out={GENERATED}", PROTO_CC],
        check=True,
    )

sys.path.insert(0, GENERATED)

import update_metadata_pb2 as u  # noqa: E402

BS = 4096


def sha256(data):
    return hashlib.sha256(data).digest()


def zstd_compress(data):
    return subprocess.run(
        ["zstd", "-q", "-3", "-c"], input=data, stdout=subprocess.PIPE, check=True
    ).stdout


def xz_compress(data):
    return lzma.compress(data, format=lzma.FORMAT_XZ, preset=0)


def bz2_compress(data):
    return bz2.compress(data, compresslevel=1)


_brotli = ctypes.CDLL(ctypes.util.find_library("brotlienc"))
_brotli.BrotliEncoderMaxCompressedSize.argtypes = [ctypes.c_size_t]
_brotli.BrotliEncoderMaxCompressedSize.restype = ctypes.c_size_t
_brotli.BrotliEncoderCompress.argtypes = [
    ctypes.c_int, ctypes.c_int, ctypes.c_int,
    ctypes.c_size_t, ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t), ctypes.c_char_p,
]
_brotli.BrotliEncoderCompress.restype = ctypes.c_int


def brotli_compress(data):
    cap = _brotli.BrotliEncoderMaxCompressedSize(len(data))
    out = ctypes.create_string_buffer(max(cap, 1))
    size = ctypes.c_size_t(max(cap, 1))
    ok = _brotli.BrotliEncoderCompress(
        5, 22, 0, len(data), data, ctypes.byref(size), out
    )
    if not ok:
        raise RuntimeError("brotli compression failed")
    return out.raw[: size.value]


def extent(start, num):
    return u.Extent(start_block=start, num_blocks=num)


# ------------------------------------------------------------------ bspatch

def make_bsdiff(old, new, compressor="bz2"):
    if compressor == "bz2":
        comp = bz2_compress
        types = (1, 1, 1)
    elif compressor == "brotli":
        comp = brotli_compress
        types = (2, 2, 2)
    else:
        raise ValueError(compressor)

    diff_size = min(len(old), len(new))
    diff = bytearray(diff_size)
    for i in range(diff_size):
        diff[i] = (new[i] - old[i]) & 0xFF
    extra = new[diff_size:]

    ctrl = struct.pack("<qqq", diff_size, len(extra), 0)
    c_ctrl, c_diff, c_extra = comp(ctrl), comp(bytes(diff)), comp(extra)
    if compressor == "bz2":
        magic = b"BSDIFF40"
        header = magic + struct.pack("<qqq", len(c_ctrl), len(c_diff), len(new))
    else:
        magic = b"BSDF2" + bytes(types)
        header = magic + struct.pack("<qqq", len(c_ctrl), len(c_diff), len(new))
    return header + c_ctrl + c_diff + c_extra


# ---------------------------------------------------------------- dm-verity

def next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


def pad_block(data, bs=BS):
    rem = len(data) % bs
    return data + b"\x00" * (bs - rem) if rem else data


def hash_tree(data, algorithm="sha256", salt=b"", bs=BS):
    digest_len = hashlib.new(algorithm).digest_size
    slot = next_pow2(digest_len)
    assert bs % slot == 0
    level = bytearray()
    for off in range(0, len(data), bs):
        d = hashlib.new(algorithm, salt + data[off : off + bs]).digest()
        level += d + b"\x00" * (slot - digest_len)
    level = pad_block(bytes(level), bs)
    levels = [level]
    while len(levels[-1]) // bs > 1:
        prev = levels[-1]
        nxt = bytearray()
        for off in range(0, len(prev), bs):
            d = hashlib.new(algorithm, salt + prev[off : off + bs]).digest()
            nxt += d + b"\x00" * (slot - digest_len)
        levels.append(pad_block(bytes(nxt), bs))
    return b"".join(reversed(levels))


def _gf_tables():
    exp = [0] * 512
    log = [0] * 256
    x = 1
    for i in range(255):
        exp[i] = x
        log[x] = i
        hi = x & 0x80
        x = (x << 1) & 0xFF
        if hi:
            x ^= 0x1D
    for i in range(255, 512):
        exp[i] = exp[i - 255]

    def mul(a, b):
        if a == 0 or b == 0:
            return 0
        return exp[log[a] + log[b]]

    return exp, log, mul


def _generator(roots):
    exp, log, mul = _gf_tables()
    g = [1]
    for i in range(roots):
        root = exp[i]
        nxt = [0] * (len(g) + 1)
        for j, c in enumerate(g):
            nxt[j] ^= c
            nxt[j + 1] ^= mul(c, root)
        g = nxt
    return g  # constant-first, monic at g[roots]


def fec_encode(data, roots, bs=BS):
    """dm-verity FEC parity, layout [round][byte][root]."""
    exp, log, mul = _gf_tables()
    # _generator returns highest-degree-first coefficients (monic first).
    g_high = _generator(roots)
    rsn = 255 - roots
    blocks = len(data) // bs
    assert len(data) % bs == 0
    rounds = (blocks + rsn - 1) // rsn
    out = bytearray()
    for r in range(rounds):
        # Build one 4096-byte codeword block: rows[k] = symbols over j.
        # Long division per byte offset is equivalent to encoding each codeword.
        for k in range(bs):
            msg = bytearray(rsn)
            for j in range(rsn):
                idx = j * rounds + r
                if idx < blocks:
                    msg[j] = data[idx * bs + k]
            # R(x) = msg(x) * x^roots mod g(x), highest-degree-first arrays.
            poly = list(msg) + [0] * roots
            for i in range(rsn):
                coeff = poly[i]
                if coeff:
                    for j in range(roots + 1):
                        poly[i + j] ^= mul(coeff, g_high[j])
            out += bytes(poly[rsn : rsn + roots])
    return bytes(out)


def verity_regions(data, algorithm="sha256", salt=b"", roots=2, bs=BS):
    data_blocks = len(data) // bs
    tree = hash_tree(data, algorithm, salt, bs)
    tree_blocks = len(tree) // bs
    fec = fec_encode(data, roots, bs)
    fec_blocks = len(fec) // bs
    full = data + tree + fec
    assert len(full) == (data_blocks + tree_blocks + fec_blocks) * bs
    return full, tree_blocks, fec_blocks


# ---------------------------------------------------------------- operations

def op_replace(blob, dst, op_type):
    return {"type": op_type, "blob": blob, "dst": dst}


def op_copy(src, dst, src_hash=None, op_type=None):
    return {
        "type": u.InstallOperation.SOURCE_COPY if op_type is None else op_type,
        "blob": b"",
        "src": src,
        "dst": dst,
        "src_hash": src_hash,
    }


def op_bsdiff(old, new, src, dst, compressor, op_type, src_hash=None):
    return {
        "type": op_type,
        "blob": make_bsdiff(old, new, compressor),
        "src": src,
        "dst": dst,
        "src_hash": src_hash,
    }


def op_zero(dst):
    return {"type": u.InstallOperation.ZERO, "blob": b"", "dst": dst}


# ------------------------------------------------------------------ payload

class PartitionSpec:
    def __init__(self, name, new_data, ops, old_data=None, hash_tree=None, fec=None,
                 omit_hash=False):
        self.name = name
        self.new_data = new_data
        self.ops = ops
        self.old_data = old_data
        self.omit_hash = omit_hash
        # hash_tree = dict(data_blocks, tree_blocks, algorithm, salt)
        self.hash_tree = hash_tree
        # fec = dict(data_blocks, fec_blocks, roots)
        self.fec = fec


def write_payload(path, parts, block_size=BS, minor_version=0, partial_update=False):
    manifest = u.DeltaArchiveManifest(
        block_size=block_size,
        minor_version=minor_version,
        partial_update=partial_update,
    )
    blobs = bytearray()
    for spec in parts:
        part = u.PartitionUpdate(partition_name=spec.name)
        part.new_partition_info.size = len(spec.new_data)
        if not spec.omit_hash:
            part.new_partition_info.hash = sha256(spec.new_data)
        if spec.old_data is not None:
            part.old_partition_info.size = len(spec.old_data)
            part.old_partition_info.hash = sha256(spec.old_data)
        for op in spec.ops:
            msg = u.InstallOperation(type=op["type"])
            blob = op.get("blob", b"")
            if blob:
                msg.data_offset = len(blobs)
                msg.data_length = len(blob)
                blobs += blob
                if op.get("data_hash", True):
                    msg.data_sha256_hash = sha256(blob)
            for e in op.get("src", []):
                msg.src_extents.append(e)
            for e in op.get("dst", []):
                msg.dst_extents.append(e)
            if op.get("src_hash") is not None:
                msg.src_sha256_hash = op["src_hash"]
            part.operations.append(msg)
        if spec.hash_tree is not None:
            ht = spec.hash_tree
            part.hash_tree_data_extent.CopyFrom(extent(0, ht["data_blocks"]))
            part.hash_tree_extent.CopyFrom(
                extent(ht["data_blocks"], ht["tree_blocks"])
            )
            part.hash_tree_algorithm = ht.get("algorithm", "sha256")
            part.hash_tree_salt = ht.get("salt", b"")
        if spec.fec is not None:
            fec = spec.fec
            fec_start = fec["data_blocks"] + fec.get("tree_blocks", 0)
            part.fec_data_extent.CopyFrom(extent(0, fec["data_blocks"]))
            part.fec_extent.CopyFrom(extent(fec_start, fec["fec_blocks"]))
            part.fec_roots = fec.get("roots", 2)
        manifest.partitions.append(part)

    manifest_bytes = manifest.SerializeToString()
    header = (
        b"CrAU"
        + struct.pack(">Q", 2)
        + struct.pack(">Q", len(manifest_bytes))
        + struct.pack(">I", 0)
    )
    with open(path, "wb") as f:
        f.write(header + manifest_bytes + bytes(blobs))
    return {spec.name: spec.new_data for spec in parts}

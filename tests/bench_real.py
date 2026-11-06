#!/usr/bin/env python3
"""Compare existing C++/Go binaries on a real payload; retain JSONL evidence."""
import argparse, ctypes, hashlib, json, mmap, os, resource, shutil, subprocess, time
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--go', required=True)
ap.add_argument('--cpp', default='build/payload-dumper')
ap.add_argument('--implementations', nargs='+', choices=['cpp', 'go'], default=['cpp', 'go'])
ap.add_argument('--payload', default='payload.bin')
ap.add_argument('--runs', type=int, default=3)
ap.add_argument('--warm-input', action='store_true', help='read payload fully before every timed run and record mincore residency')
ap.add_argument('--workers', default='1,6,12')
ap.add_argument('--work', default='benchmarks/real')
a = ap.parse_args()
work = Path(a.work).resolve(); work.mkdir(parents=True, exist_ok=True)
results = work / 'results.jsonl'
if results.exists(): raise SystemExit(f'Refusing to overwrite {results}')
bins = {'cpp': str(Path(a.cpp).resolve()), 'go': str(Path(a.go).resolve())}
payload = str(Path(a.payload).resolve())
def residency(path):
    size = os.path.getsize(path)
    pages = (size + mmap.PAGESIZE - 1) // mmap.PAGESIZE
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_COPY) as mm:
        anchor = ctypes.c_char.from_buffer(mm)
        bits = (ctypes.c_ubyte * pages)()
        result = libc.mincore(ctypes.addressof(anchor), size, bits)
        del anchor
        if result: raise OSError(ctypes.get_errno(), 'mincore')
        return sum(b & 1 for b in bits) / pages

def warm(path):
    started = time.perf_counter()
    buffer = bytearray(16 << 20)
    with open(path, 'rb', buffering=0) as f:
        while f.readinto(buffer): pass
    return dict(warm_seconds=time.perf_counter() - started,
                input_resident_before=residency(path))

reference = None
for repeat in range(a.runs):
    for workers in map(int, a.workers.split(',')):
        for impl in (['cpp', 'go'] if repeat % 2 == 0 else ['go', 'cpp']):
            if impl not in a.implementations:
                continue
            out = work / f'{impl}-{workers}-{repeat}'
            out.mkdir()  # fresh outputs; cleanup only this newly created directory
            cmd = [bins[impl], '-q', '-c', str(workers), '-o', str(out), payload]
            log = work / f'{impl}-{workers}-{repeat}.log'
            cache = warm(payload) if a.warm_input else {}
            started = time.perf_counter()
            with log.open('wb') as f:
                proc = subprocess.Popen(cmd, stdout=f, stderr=f)
                _, status, usage = os.wait4(proc.pid, 0)
                proc.returncode = os.waitstatus_to_exitcode(status)
            elapsed = time.perf_counter() - started
            row = dict(implementation=impl, workers=workers, repeat=repeat,
                       seconds=elapsed, user=usage.ru_utime, system=usage.ru_stime,
                       maxrss_kib=usage.ru_maxrss, exitcode=proc.returncode, command=cmd)
            row.update(cache)
            if a.warm_input: row['input_resident_after'] = residency(payload)
            if proc.returncode: raise SystemExit(f'Extraction failed: {log}')
            # Outside timed interval. Verify every image against the first run.
            if repeat == 0 and workers == int(a.workers.split(',')[0]):
                hashes = {}
                for image in sorted(out.glob('*.img')):
                    with image.open('rb') as f: hashes[image.name] = hashlib.file_digest(f, 'sha256').hexdigest()
                (work / f'{impl}-hashes.json').write_text(json.dumps(hashes, indent=2))
                if reference is None: reference = hashes
                elif hashes != reference: raise SystemExit('C++ and Go outputs differ')
                row['crosscheck_images'] = len(hashes)
            with results.open('a') as f: f.write(json.dumps(row) + '\n')
            print(json.dumps(row), flush=True)
            shutil.rmtree(out)

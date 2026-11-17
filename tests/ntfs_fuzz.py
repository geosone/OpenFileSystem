#!/usr/bin/env python3
"""Fuzzing of the NTFS reader: changes a few bytes of
an MFT record or an index block of IMAGE (keeping the update sequence
fixups valid, so the change gets past them), and runs TEST --dump on it.
The reader must end with a result (0 or 1), never with a sanitizer report
(exit code 99), a crash or a hang (4 times the time of the undamaged image, at least 10 s);
it stops after 5 failures.
    ntfs_fuzz.py IMAGE TEST SEED COUNT WORKDIR"""
import os
import random
import resource
import struct
import subprocess
import sys
import time

image, test, seed, count, work = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
orig = open(image, 'rb').read()
spc = orig[0x0D] if orig[0x0D] <= 0x80 else 1 << (256 - orig[0x0D])
csize = 512 * spc
c = struct.unpack_from('<b', orig, 0x40)[0]
rsize = 1 << -c if c < 0 else c * csize
records = [i for i in range(0, len(orig) - rsize, 512) if orig[i:i + 4] == b'FILE']
blocks = [i for i in range(0, len(orig), 512) if orig[i:i + 4] == b'INDX']
rnd = random.Random(seed)
out = os.path.join(work, 'fuzz.img')
bad = 0


def mutate(b, start, size):
    # bytes within the record, never the last two of a sector (fixups)
    for _ in range(rnd.choice((1, 1, 2, 4, 8))):
        o = rnd.randrange(size)
        if o % 512 >= 510:
            continue
        k = rnd.random()
        if k < 0.4:
            b[start + o] = rnd.randrange(256)
        elif k < 0.7:
            b[start + o] ^= 1 << rnd.randrange(8)
        elif k < 0.85:
            b[start + o] = rnd.choice((0, 0xFF, 0x7F, 0x80))
        else:                                   # a 16 or 32 bit field
            for j in range(rnd.choice((2, 4))):
                if (o + j) % 512 < 510 and o + j < size:
                    b[start + o + j] = rnd.choice((0, 0xFF))


with open(out, 'wb') as f:
    f.write(orig)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))  # no core files from killed runs
env = dict(os.environ, ASAN_OPTIONS='exitcode=99:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1:exitcode=99')
t0 = time.monotonic()
if subprocess.run([test, out, '0', '--dump'], stdout=subprocess.DEVNULL, env=env).returncode:
    sys.exit('ntfs_fuzz: the undamaged image fails')
limit = max(10.0, 4 * (time.monotonic() - t0))
for i in range(count):
    b = bytearray(orig)
    if blocks and rnd.random() < 0.35:
        where, size = rnd.choice(blocks), 4096
    else:
        where, size = rnd.choice(records), rsize
    mutate(b, where, size)
    with open(out, 'r+b') as f:
        f.seek(where)
        f.write(b[where:where + size])
    try:
        r = subprocess.run([test, out, '0', '--dump'], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           timeout=limit, env=env)
        rc = r.returncode
        err = r.stderr.decode(errors='replace')
    except subprocess.TimeoutExpired:
        rc, err = 'hang', ''
    if rc not in (0, 1):
        bad += 1
        keep = os.path.join(work, 'fail-%d-%d.img' % (seed, i))
        with open(keep, 'wb') as f:
            f.write(b)
        print('FAIL seed %d case %d: %s -> %s' % (seed, i, rc, keep))
        print('\n'.join(err.splitlines()[:12]), flush=True)
        if bad == 5:
            break
    with open(out, 'r+b') as f:
        f.seek(where)
        f.write(orig[where:where + size])
print('%d cases, %d failed' % (i + 1, bad))
sys.exit(1 if bad else 0)

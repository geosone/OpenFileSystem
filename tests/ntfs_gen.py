#!/usr/bin/env python3
"""Fills a mounted NTFS volume (ntfs-3g) with a random tree for
tests/ntfs_diff.sh, the same for the same seed: deep directories, one large
directory (an index of several levels), long names with umlauts, spaces and
dots, files from empty and resident to some MB, fragmented files (written in
turns), sparse files, hard links, alternate data streams (user.* extended
attributes of ntfs-3g), deleted files, and compressed files in KOMPR.
Then prints the reference listing as tests/ntfs_test --dump does, read
back through ntfs-3g: "D path", "F path size crc32".
    ntfs_gen.py MOUNT SEED CLUSTER
    ntfs_gen.py MOUNT --list        only the listing"""
import os
import random
import sys
import zlib

mnt = sys.argv[1]
only_list = sys.argv[2] == '--list'
seed, cluster = (0, 4096) if only_list else (int(sys.argv[2]), int(sys.argv[3]))
rnd = random.Random(seed)
CHARS = ('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'
         'äöüÄÖÜßéèñçå _-+,;=[]()!#$%&\'@^{}~.')
EXTS = ['', '.txt', '.TXT', '.bin', '.dat', '.tar.gz', '.c', '.Html', '.longextension']
used = {}          # directory -> lower case names in it (NTFS as Windows sees it)


def name(d):
    while True:
        kind = rnd.random()
        if kind < 0.3:      # 8.3 already
            n = ''.join(rnd.choice('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_') for _ in range(rnd.randint(1, 8)))
            if rnd.random() < 0.7:
                n += '.' + ''.join(rnd.choice('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') for _ in range(rnd.randint(1, 3)))
        else:
            n = ''.join(rnd.choice(CHARS) for _ in range(rnd.randint(1, 60 if kind < 0.95 else 200)))
            n += rnd.choice(EXTS)
        n = n.strip(' .')[:250]
        if not n or n.lower() in used.setdefault(d, set()):
            continue
        used[d].add(n.lower())
        return os.path.join(d, n)


def data(size):
    return rnd.randbytes(size) if rnd.random() < 0.8 else bytes([rnd.randrange(256)]) * size


def size():
    k = rnd.random()
    if k < 0.15:
        return 0
    if k < 0.45:
        return rnd.randint(1, 700)                       # resident
    if k < 0.65:
        return cluster * rnd.randint(1, 4) + rnd.choice((-1, 0, 1))
    if k < 0.97:
        return rnd.randint(1, 300000)
    return rnd.randint(1, 8 << 20)


def write(path, n):
    with open(path, 'wb') as f:
        f.write(data(n))


def generate():
    dirs = [mnt]
    for _ in range(rnd.randint(10, 30)):              # a tree up to 8 levels deep
        parent = rnd.choice(dirs)
        if parent.count(os.sep) - mnt.count(os.sep) >= 8:
            continue
        d = name(parent)
        os.mkdir(d)
        dirs.append(d)
    files = []
    for _ in range(rnd.randint(100, 300)):
        f = name(rnd.choice(dirs))
        write(f, size())
        files.append(f)
    big = name(mnt)                                    # an index of several levels
    os.mkdir(big)
    for _ in range(rnd.randint(1500, 4000)):
        f = name(big)
        write(f, rnd.choice((0, 3, 50, 700, 2000)))
    for _ in range(rnd.randint(2, 6)):                 # fragmented: written in turns
        group = [name(rnd.choice(dirs)) for _ in range(rnd.randint(2, 5))]
        for _ in range(rnd.randint(20, 80)):
            for f in group:
                with open(f, 'ab') as o:
                    o.write(data(rnd.randint(1, 3 * cluster)))
                    o.flush()
                    os.fsync(o.fileno())
        files += group
    for _ in range(rnd.randint(2, 6)):                 # sparse
        f = name(rnd.choice(dirs))
        with open(f, 'wb') as o:
            for _ in range(rnd.randint(1, 5)):
                o.seek(rnd.randint(0, 4 << 20))
                o.write(data(rnd.randint(1, 3 * cluster)))
            o.truncate(o.tell() + rnd.randint(0, 1 << 20))
        files.append(f)
    for _ in range(rnd.randint(2, 10)):                # hard links
        src = rnd.choice(files)
        os.link(src, name(rnd.choice(dirs)))
    for f in rnd.sample(files, min(len(files), rnd.randint(2, 10))):    # streams
        os.setxattr(f, 'user.stream' + str(rnd.randint(1, 9)), data(rnd.randint(1, 5000)))
    for f in rnd.sample(files, len(files) // 5):       # deleted (holes, unused records)
        if os.path.exists(f):
            os.unlink(f)
    kompr = name(mnt)                                  # NTFS compresses up to 4 KB clusters
    os.mkdir(kompr)
    if cluster <= 4096:
        os.setxattr(kompr, 'system.ntfs_attrib_be', (0x800).to_bytes(4, 'big'))
    for _ in range(rnd.randint(1, 4) if cluster <= 4096 else 0):
        f = name(kompr)
        with open(f, 'wb') as o:                       # compressed, plain and zero units
            o.write(b'compressible ' * rnd.randint(500, 5000) + rnd.randbytes(rnd.randint(0, 70000)) +
                    bytes(rnd.randint(0, 200000)) + b'%d ' % rnd.randint(0, 9) * rnd.randint(0, 9000))
    os.sync()


def listing(d, rel):
    for e in sorted(os.scandir(d), key=lambda e: e.name):
        p = rel + '/' + e.name
        if e.is_dir(follow_symlinks=False):
            print('D', p)
            listing(e.path, p)
        else:
            c, n = 0, 0
            with open(e.path, 'rb') as f:
                while True:
                    b = f.read(1 << 20)
                    if not b:
                        break
                    c, n = zlib.crc32(b, c), n + len(b)
            print('F %s %d %08x' % (p, n, c))


if not only_list:
    generate()
listing(mnt, '')

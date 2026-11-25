#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks what ntfsfix and ntfs-3g do not: the clusters of an NTFS volume.

  ntfs_check.py IMAGE

Every MFT record in use is read (update sequence fixups); every
non-resident attribute's runs are collected.  Errors (exit 1):
  - a cluster used by two attributes (or twice by one)
  - a cluster used but free in $Bitmap, or set in $Bitmap but used by none
  - a non-resident attribute whose sizes do not agree with its runs
    (allocated size = its clusters, highest VCN, data <= allocated,
    initialized <= data)
  - a directory index entry whose size (the duplicated information Windows
    shows in DIR) is not the size of the file's $DATA
  - a directory index out of order (by $UpCase, then exactly), a child
    block not in use by the index $BITMAP or reached twice, a block in use
    that no entry reaches, an index block's VCN wrong
  - an index entry naming a record not in use, of another sequence number,
    or without that $FILE_NAME; a file's $FILE_NAME no entry of its
    directory names
  - a record in use whose bit in $MFT's $BITMAP is off, a record from 24 on
    with its bit on that is not in use; $MFTMirr not the first records
"""
import struct
import sys


def rd(fmt, b, o):
    return struct.unpack_from(fmt, b, o)[0]


class Vol:
    def __init__(self, path):
        self.f = open(path, "rb")
        boot = self.f.read(512)
        if boot[3:7] != b"NTFS":
            sys.exit("not NTFS")
        self.bps = rd("<H", boot, 0x0B)
        spc = boot[0x0D]
        if spc > 0x80:
            spc = 1 << (256 - spc)
        self.cb = self.bps * spc
        self.clusters = rd("<Q", boot, 0x28) // spc
        c = rd("<b", boot, 0x40)
        self.rs = 1 << -c if c < 0 else c * self.cb
        c = rd("<b", boot, 0x44)
        self.isize = 1 << -c if c < 0 else c * self.cb
        self.mft_lcn = rd("<Q", boot, 0x30)
        self.mirr_lcn = rd("<Q", boot, 0x38)
        rec0 = self.fixup(self.read(self.mft_lcn * self.cb, self.rs))
        a = self.attr(rec0, 0x80)
        self.mft_runs = self.runs(a)
        self.mft_size = rd("<Q", a, 0x30)
        u = self.fixup(self.record(10))
        a = self.attr(u, 0x80)
        raw = self.data(a, rd("<Q", a, 0x30))
        self.upcase = [rd("<H", raw, 2 * i) for i in range(len(raw) // 2)]

    def read(self, off, n):
        self.f.seek(off)
        return bytearray(self.f.read(n))

    def fixup(self, b):
        ofs, cnt = rd("<H", b, 4), rd("<H", b, 6)
        usn = b[ofs:ofs + 2]
        for i in range(1, cnt):
            if b[i * 512 - 2:i * 512] != usn:
                raise ValueError("fixup")
            b[i * 512 - 2:i * 512] = b[ofs + 2 * i:ofs + 2 * i + 2]
        return b

    @staticmethod
    def attrs(b):
        p = rd("<H", b, 0x14)
        while p + 8 <= len(b):
            t = rd("<I", b, p)
            if t == 0xFFFFFFFF:
                return
            n = rd("<I", b, p + 4)
            if n < 0x18 or p + n > len(b):
                raise ValueError("attribute length")
            yield t, b[p:p + n]
            p += n

    def attr(self, b, typ):
        for t, a in self.attrs(b):
            if t == typ and a[9] == 0:
                return a
        return None

    @staticmethod
    def runs(a):
        p, end = rd("<H", a, 0x20), len(a)
        vcn, lcn, out = rd("<Q", a, 0x10), 0, []
        while p < end and a[p]:
            ln, on = a[p] & 15, a[p] >> 4
            p += 1
            length = int.from_bytes(a[p:p + ln], "little")
            p += ln
            if on:
                lcn += int.from_bytes(a[p:p + on], "little", signed=True)
                out.append((vcn, length, lcn))
            else:
                out.append((vcn, length, None))
            p += on
            vcn += length
        return out

    @staticmethod
    def index_entries(d, b, at):
        """(directory, file record, data size) of the entries of an index node
        whose header is at at"""
        first, used = rd("<I", b, at), rd("<I", b, at + 4)
        p, out = at + first, []
        while p + 16 <= at + used:
            elen, klen, flags = rd("<H", b, p + 8), rd("<H", b, p + 10), rd("<H", b, p + 12)
            if flags & 2 or elen < 16:
                break
            if klen >= 0x42:
                out.append((d, rd("<I", b, p), rd("<Q", b, p + 16 + 0x30)))
            p += elen
        return out

    def record(self, no):
        off = no * self.rs
        for vcn, length, lcn in self.mft_runs:
            if vcn * self.cb <= off < (vcn + length) * self.cb:
                return self.read(lcn * self.cb + off - vcn * self.cb, self.rs)
        raise ValueError("record %d not in $MFT" % no)

    def raw_record(self, no):
        return self.record(no)

    def data(self, a, size):
        out = bytearray()
        for vcn, length, lcn in self.runs(a):
            out += bytes(length * self.cb) if lcn is None else self.read(lcn * self.cb, length * self.cb)
        return out[:size]


def i30(a):
    return a[9] == 4 and bytes(a[rd("<H", a, 0x0A):rd("<H", a, 0x0A) + 8]) == "$I30".encode("utf-16-le")


def collate(up, name):
    return ([up[c] if c < len(up) else c for c in name], name)


def fn_key(val):
    n = val[0x40]
    return [rd("<H", val, 0x42 + 2 * i) for i in range(n)], val[0x41]


def check_dirs(v, dirs, records, errors):
    """dirs: record -> (root value, allocation data or None, bitmap bytes or None);
    records: record -> (seq, in use, [(parent, name, ns)])"""
    up = v.upcase
    vbits = v.cb.bit_length() - 1 if v.cb <= v.isize else 9
    named = set()
    for d, (root, alloc, bitmap) in dirs.items():
        seen = set()
        last = [None]

        def node(b, at, where, depth):
            first, used, flags = rd("<I", b, at), rd("<I", b, at + 4), b[at + 12]
            p = at + first
            while p + 16 <= at + used:
                ref, elen, klen, ef = rd("<Q", b, p), rd("<H", b, p + 8), rd("<H", b, p + 10), rd("<H", b, p + 12)
                if elen < 16:
                    errors.append("directory %d %s: entry length %d" % (d, where, elen))
                    return
                if bool(ef & 1) != bool(flags & 1):
                    errors.append("directory %d %s: an entry with%s a child in a node with%s" %
                                  (d, where, "" if ef & 1 else "out", "" if flags & 1 else "out"))
                if ef & 1:
                    vcn = rd("<Q", b, p + elen - 8)
                    k = (vcn << vbits) // v.isize
                    if depth > 16 or alloc is None or (k + 1) * v.isize > len(alloc):
                        errors.append("directory %d %s: child VCN %d past the allocation" % (d, where, vcn))
                    elif k in seen:
                        errors.append("directory %d: block %d reached twice" % (d, k))
                    else:
                        seen.add(k)
                        if not (bitmap and k // 8 < len(bitmap) and bitmap[k // 8] >> (k & 7) & 1):
                            errors.append("directory %d: block %d not in use by the bitmap" % (d, k))
                        ib = bytearray(alloc[k * v.isize:(k + 1) * v.isize])
                        if ib[:4] != b"INDX":
                            errors.append("directory %d: block %d is not INDX" % (d, k))
                        else:
                            v.fixup(ib)
                            if rd("<Q", ib, 0x10) != vcn:
                                errors.append("directory %d: block %d has VCN %d, not %d" % (d, k, rd("<Q", ib, 0x10), vcn))
                            node(ib, 0x18, "block %d" % k, depth + 1)
                if ef & 2:
                    return
                val = b[p + 16:p + 16 + klen]
                name, ns = fn_key(val)
                key = collate(up, name)
                if last[0] is not None and not last[0] < key:
                    errors.append("directory %d %s: %r not after %r" %
                                  (d, where, "".join(map(chr, name)), "".join(map(chr, last[0][1]))))
                last[0] = key
                no, seq = ref & 0xFFFFFFFFFFFF, ref >> 48
                r = records.get(no)
                if no >= 16:
                    if r is None or not r[1]:
                        errors.append("directory %d: %r names record %d, not in use" % (d, "".join(map(chr, name)), no))
                    elif r[0] != seq:
                        errors.append("directory %d: %r names record %d seq %d, it has %d" %
                                      (d, "".join(map(chr, name)), no, seq, r[0]))
                    elif (d, tuple(name), ns) not in r[2]:
                        errors.append("directory %d: record %d has no $FILE_NAME %r" % (d, no, "".join(map(chr, name))))
                    named.add((no, d, tuple(name), ns))
                p += elen

        node(root, 16, "root", 0)
        if bitmap is not None and alloc is not None:
            for k in range(len(alloc) // v.isize):
                if k // 8 < len(bitmap) and bitmap[k // 8] >> (k & 7) & 1 and k not in seen:
                    errors.append("directory %d: block %d in use, no entry reaches it" % (d, k))
    for no, (seq, used, fns) in records.items():
        if no >= 16 and used:
            for parent, name, ns in fns:
                if parent in dirs and (no, parent, name, ns) not in named:
                    errors.append("record %d: no entry of directory %d names %r" % (no, parent, "".join(map(chr, name))))


def main():
    v = Vol(sys.argv[1])
    errors = []
    dirs = {}
    records = {}
    mftbitmap = None
    allfns = {}
    owner = {}
    sizes = {}              # base record: the size of its unnamed $DATA
    entries = []            # (directory, file record, size in the entry)
    extents = {}            # (base record, type, name): [allocated size, highest VCN of all extents]
    bitmap = None
    for no in range(v.mft_size // v.rs):
        b = v.record(no)
        if b[:4] != b"FILE":
            continue
        if not rd("<H", b, 0x16) & 1:
            records[no] = (rd("<H", b, 0x10), False, [])
            continue
        b = v.fixup(b)
        base = rd("<Q", b, 0x20) & 0xFFFFFFFFFFFF
        fns = allfns.setdefault(base or no, [])     # an extension record's go to its base
        records[no] = (rd("<H", b, 0x10), True, allfns.setdefault(no, []))
        droot = dalloc = dbitmap = None
        data_size = fn_sizes = None
        for t, a in v.attrs(b):
            if not a[8]:
                if t == 0x90 and i30(a):             # $INDEX_ROOT $I30
                    entries += v.index_entries(no, a[rd("<H", a, 0x14):], 16)
                    droot = bytearray(a[rd("<H", a, 0x14):rd("<H", a, 0x14) + rd("<I", a, 0x10)])
                if t == 0xB0 and i30(a):
                    dbitmap = a[rd("<H", a, 0x14):rd("<H", a, 0x14) + rd("<I", a, 0x10)]
                if t == 0xB0 and a[9] == 0 and no == 0:
                    mftbitmap = a[rd("<H", a, 0x14):rd("<H", a, 0x14) + rd("<I", a, 0x10)]
                if t == 0x30:
                    val = a[rd("<H", a, 0x14):]
                    name, ns = fn_key(val)
                    fns.append((rd("<Q", val, 0) & 0xFFFFFFFFFFFF, tuple(name), ns))
                    fn_sizes = fn_sizes or []
                    fn_sizes.append(rd("<Q", val, 0x30))
                elif t == 0x80 and a[9] == 0:
                    data_size = rd("<I", a, 0x10)
                continue
            runs = v.runs(a)
            lowest, highest = rd("<Q", a, 0x10), rd("<q", a, 0x18)
            n = sum(r[1] for r in runs)
            if lowest == 0:
                alloc, size, init = rd("<Q", a, 0x28), rd("<Q", a, 0x30), rd("<Q", a, 0x38)
                if rd("<H", a, 0x0C) & 0xFF == 0:     # compressed: allocated by units
                    extents.setdefault((base or no, t, bytes(a[rd("<H", a, 0x0A):][:2 * a[9]])), [None, -1])[0] = alloc
                if not size <= alloc or not init <= size:
                    errors.append("record %d attr %#x: sizes %d/%d/%d" % (no, t, alloc, size, init))
                if t == 0x80 and a[9] == 0 and not base:
                    data_size = size
                if t == 0xB0 and i30(a):
                    dbitmap = v.data(a, size)
                if t == 0xB0 and a[9] == 0 and no == 0:
                    mftbitmap = v.data(a, size)
                if t == 0xA0 and i30(a):             # $INDEX_ALLOCATION $I30
                    alldata = v.data(a, size)
                    dalloc = alldata
                    for blk in range(0, size, v.isize):
                        ib = alldata[blk:blk + v.isize]
                        if ib[:4] == b"INDX":
                            entries += v.index_entries(no, v.fixup(bytearray(ib)), 24)
                if no == 6 and t == 0x80:
                    bitmap = v.data(a, size)
            e = extents.setdefault((base or no, t, bytes(a[rd("<H", a, 0x0A):][:2 * a[9]])), [None, -1])
            e[1] = max(e[1], highest)
            if runs and highest != lowest + n - 1:
                errors.append("record %d attr %#x: runs end at %d, highest VCN %d" % (no, t, lowest + n - 1, highest))
            for vcn, length, lcn in runs:
                if lcn is None:
                    continue
                for c in range(lcn, lcn + length):
                    if c in owner:
                        errors.append("cluster %d of record %d also of record %d" % (c, no, owner[c]))
                    owner[c] = no
        if not base and data_size is not None:
            sizes[no] = data_size
        if not base and droot is not None:
            dirs[no] = (droot, dalloc, dbitmap)
    check_dirs(v, dirs, records, errors)
    for no, (seq, used, fns) in records.items():
        bit = mftbitmap is not None and no // 8 < len(mftbitmap) and mftbitmap[no // 8] >> (no & 7) & 1
        if used and not bit:
            errors.append("record %d in use, its bit in $MFT's $BITMAP off" % no)
        elif bit and not used and no >= 24:
            errors.append("record %d not in use, its bit in $MFT's $BITMAP on" % no)
    n = 4 if v.cb <= 4 * v.rs else v.cb // v.rs
    if v.read(v.mirr_lcn * v.cb, n * v.rs) != b"".join(v.record(i) for i in range(n)):
        errors.append("$MFTMirr is not the first %d records" % n)
    for d, no, size in entries:
        if no >= 16 and no in sizes and size != sizes[no]:
            errors.append("directory %d: the entry of record %d has size %d, $DATA %d" % (d, no, size, sizes[no]))
    for (no, t, name), (alloc, highest) in extents.items():
        if alloc is not None and alloc != (highest + 1) * v.cb:
            errors.append("record %d attr %#x: allocated %d, highest VCN %d" % (no, t, alloc, highest))
    if bitmap is None:
        errors.append("no $Bitmap")
    else:
        for c in range(v.clusters):
            used = bitmap[c >> 3] >> (c & 7) & 1
            if used and c not in owner:
                errors.append("cluster %d set in $Bitmap, used by none" % c)
            elif not used and c in owner:
                errors.append("cluster %d of record %d free in $Bitmap" % (c, owner[c]))
    for e in errors[:20]:
        print("ntfs_check:", e)
    if errors:
        print("ntfs_check: %d errors" % len(errors))
        sys.exit(1)
    print("ntfs_check: %d clusters in use, all accounted for" % len(owner))


if __name__ == "__main__":
    main()

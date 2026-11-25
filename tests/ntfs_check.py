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
        rec0 = self.fixup(self.read(self.mft_lcn * self.cb, self.rs))
        a = self.attr(rec0, 0x80)
        self.mft_runs = self.runs(a)
        self.mft_size = rd("<Q", a, 0x30)

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

    def data(self, a, size):
        out = bytearray()
        for vcn, length, lcn in self.runs(a):
            out += bytes(length * self.cb) if lcn is None else self.read(lcn * self.cb, length * self.cb)
        return out[:size]


def main():
    v = Vol(sys.argv[1])
    errors = []
    owner = {}
    sizes = {}              # base record: the size of its unnamed $DATA
    entries = []            # (directory, file record, size in the entry)
    extents = {}            # (base record, type, name): [allocated size, highest VCN of all extents]
    bitmap = None
    for no in range(v.mft_size // v.rs):
        b = v.record(no)
        if b[:4] != b"FILE" or not rd("<H", b, 0x16) & 1:
            continue
        b = v.fixup(b)
        base = rd("<Q", b, 0x20) & 0xFFFFFFFFFFFF
        data_size = fn_sizes = None
        for t, a in v.attrs(b):
            if not a[8]:
                if t == 0x90 and a[9] == 4:             # $INDEX_ROOT $I30
                    entries += v.index_entries(no, a[rd("<H", a, 0x14):], 16)
                if t == 0x30:
                    val = a[rd("<H", a, 0x14):]
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
                if t == 0xA0 and a[9] == 4:             # $INDEX_ALLOCATION $I30
                    alldata = v.data(a, size)
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

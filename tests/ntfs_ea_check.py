# SPDX-License-Identifier: GPL-2.0-or-later
# ntfs_ea_check.py <NTFS image> <path> [name=length ...]: the Windows NT
# extended attributes of a file, read with ntfscat (ntfsprogs) and checked
# independently of src/ntfs/ntfs.c: $EA as a list of EA_FULL entries (the
# offset to the next on a multiple of 4, flags, name length, value length,
# the name with its NUL, the value), $EA_INFORMATION matching it (the packed
# size: 5 + name + value per EA; the count of EAs flagged "need"; the
# unpacked size: $EA's), every $FILE_NAME repeating the unpacked size at
# 3Ch (as ntfs-3g and Linux's ntfs3 write it).
# Without $EA there must be no $EA_INFORMATION. Prints "name:length" per EA
# (none: nothing); with name=length those must be exactly the EAs.
import struct
import subprocess
import sys


def die(msg):
    print('ntfs_ea_check: ' + msg)
    sys.exit(1)


def attr(img, path, t):
    r = subprocess.run(['ntfscat', '-a', hex(t), img, path], capture_output=True)
    return r.stdout if r.returncode == 0 else None


img, path = sys.argv[1], sys.argv[2]
want = [a.split('=', 1) for a in sys.argv[3:]]
ea, info, fn = attr(img, path, 0xE0), attr(img, path, 0xD0), attr(img, path, 0x30)
if fn is None:
    die('%s: no $FILE_NAME (not found?)' % path)
if not ea:
    if info:
        die('%s: $EA_INFORMATION without $EA' % path)
    eas, packed, unpacked = [], 0, 0
else:
    if info is None or len(info) != 8:
        die('%s: $EA without an $EA_INFORMATION of 8 bytes' % path)
    eas, p, packed, need = [], 0, 0, 0
    while p < len(ea):
        if p + 8 > len(ea):
            die('%s: an EA header past the end at %d' % (path, p))
        nxt, flags, nl, vl = struct.unpack_from('<IBBH', ea, p)
        size = 8 + nl + 1 + vl
        if p + size > len(ea) or ea[p + 8 + nl] != 0:
            die('%s: EA at %d past the end or without its NUL' % (path, p))
        if nxt < size or nxt % 4:
            die('%s: EA at %d: offset to the next %d (needs %d, a multiple of 4)' % (path, p, nxt, size))
        eas.append((ea[p + 8:p + 8 + nl].decode('latin-1'), ea[p + 9 + nl:p + 9 + nl + vl]))
        packed += 5 + nl + vl
        need += flags >> 7
        p += nxt
    if p != len(ea):
        die('%s: the last EA ends at %d, $EA is %d bytes' % (path, p, len(ea)))
    unpacked = len(ea)
    ipacked, ineed, iunpacked = struct.unpack('<HHI', info)
    if (ipacked, ineed, iunpacked) != (packed, need, len(ea)):
        die('%s: $EA_INFORMATION %s, the EAs %s' % (path, (ipacked, ineed, iunpacked), (packed, need, len(ea))))
# every $FILE_NAME (ntfscat gives them one after the other)
p = 0
while p + 0x42 <= len(fn):
    n = fn[0x40 + p]
    if struct.unpack_from('<I', fn, p + 0x38)[0] & 0x400 == 0:   # not a reparse point
        got = struct.unpack_from('<I', fn, p + 0x3C)[0]
        if got != unpacked:
            die('%s: $FILE_NAME has EA size %d, not %d' % (path, got, unpacked))
    p += (0x42 + 2 * n + 7) & ~7
for name, value in eas:
    print('%s:%d' % (name, len(value)))
if want and sorted((n, int(l)) for n, l in want) != sorted((n, len(v)) for n, v in eas):
    die('%s: the EAs %s, not %s' % (path, [(n, len(v)) for n, v in eas], want))

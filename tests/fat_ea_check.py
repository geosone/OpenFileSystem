# SPDX-License-Identifier: GPL-2.0-or-later
# fat_ea_check.py <EA DATA. SF> <bytes per cluster> [handle=OWNER ...],
# fat_ea_check.py --image <FAT12/16 image> [handle=OWNER ...]:
# checks the OS/2 / NT layout of an EA file independently of src/fat/fat_ea.c
# (after the fastfat layout): the header, the offset table, every used
# handle on a set of its own whose header names it, the sets one after the
# other without gaps from EaBaseTable[0] to the end. With handle=OWNER the
# set of that handle must belong to the 8.3 name OWNER. Prints the sets.
#
# FAT32.IFS's EA files on FAT32 and exFAT (src/vol.c), independently too:
# fat_ea_check.py --fealist <file>: the FEALIST of one EA file, its EAs as
#   name:length, one per line;
# fat_ea_check.py --mark <FAT32 image> <name>: the mark byte (0Ch) of the
#   entry in the root directory whose long name (without case) or FCB name
#   (11 characters) it is, in hex;
# fat_ea_check.py --exfat <image>: the root directory of an exFAT volume;
#   every "name EA. SF" must be hidden, system, read-only and belong to a
#   file marked 40h/80h, every marked file must have one; prints
#   "name mark name:length ..." per file with EAs.
import struct
import sys



def die(msg):
    print('fat_ea_check: ' + msg)
    sys.exit(1)


def from_image(path):
    """EA DATA. SF and the cluster size of a FAT12/16 image (no tool such
    as mtools reaches the name: its extension starts with a space)"""
    img = open(path, 'rb').read()
    bps, spc, rsvd, nfats, nroot, tot16, _, spf = struct.unpack_from('<HBHBHHBH', img, 11)
    tot = tot16 or struct.unpack_from('<I', img, 32)[0]
    root = (rsvd + nfats * spf) * bps
    first = root + (nroot * 32 + bps - 1) // bps * bps
    fat12 = (tot - first // bps) // spc < 4085
    fat = img[rsvd * bps:(rsvd + spf) * bps]
    for i in range(nroot):
        e = img[root + 32 * i:root + 32 * i + 32]
        if e[:11] == b'EA DATA  SF' and not e[11] & 0x18:
            break
    else:
        die('no EA DATA. SF in the root directory')
    cl, size = struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0]
    out = b''
    while 2 <= cl < (0xFF7 if fat12 else 0xFFF7) and len(out) < size:
        out += img[first + (cl - 2) * spc * bps:first + (cl - 1) * spc * bps]
        if fat12:
            v, = struct.unpack_from('<H', fat, cl * 3 // 2)
            cl = v >> 4 if cl & 1 else v & 0xFFF
        else:
            cl, = struct.unpack_from('<H', fat, 2 * cl)
    if len(out) < size:
        die('EA DATA. SF: its cluster chain ends early')
    return out[:size], spc * bps


def fealist(data, what):
    """the EAs of an FEALIST as (name, value length, flags)"""
    if len(data) < 4:
        die(what + ': no cbList')
    cb, = struct.unpack_from('<I', data, 0)
    if cb < 4 or cb > len(data):
        die(what + ': cbList %d of %d bytes' % (cb, len(data)))
    eas, p = [], 4
    while p < cb:
        if p + 4 > cb:
            die(what + ': an EA header past cbList')
        flags, nl, vl = struct.unpack_from('<BBH', data, p)
        if p + 5 + nl + vl > cb or data[p + 4 + nl] != 0:
            die(what + ': EA at %d past cbList or without its NUL' % p)
        eas.append((data[p + 4:p + 4 + nl].decode('latin-1'), vl, flags))
        p += 5 + nl + vl
    if not eas:
        die(what + ': an empty list (it should not exist)')
    return eas


def fat32_mark(path, fcb):
    img = open(path, 'rb').read()
    bps, spc, rsvd, nfats = struct.unpack_from('<HBHB', img, 11)
    spf, = struct.unpack_from('<I', img, 36)
    cl, = struct.unpack_from('<I', img, 44)
    fat = img[rsvd * bps:(rsvd + spf) * bps]
    first = (rsvd + nfats * spf) * bps
    lfn = {}
    while 2 <= cl < 0x0FFFFFF8:
        d = img[first + (cl - 2) * spc * bps:first + (cl - 1) * spc * bps]
        for i in range(0, len(d), 32):
            e = d[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = {}
            elif e[11] == 0x0F:
                lfn[e[0] & 0x1F] = e[1:11] + e[14:26] + e[28:32]
            else:
                long = b''.join(lfn[k] for k in sorted(lfn)).decode('utf-16-le').split('\0')[0]
                lfn = {}
                if e[:11] == fcb.encode('latin-1') or long.upper() == fcb.upper():
                    return e[12]
        cl = struct.unpack_from('<I', fat, 4 * cl)[0] & 0x0FFFFFFF
    die('no %r in the root directory' % fcb)


def exfat_root(path):
    img = open(path, 'rb').read()
    if img[3:11] != b'EXFAT   ':
        die('not exFAT')
    fat_off, = struct.unpack_from('<I', img, 80)
    heap, = struct.unpack_from('<I', img, 88)
    root, = struct.unpack_from('<I', img, 96)
    bps, spc = 1 << img[108], 1 << img[109]
    bpc = bps * spc
    def nxt(c):
        return struct.unpack_from('<I', img, fat_off * bps + 4 * c)[0]
    def chain(c, size, nofat):
        out = b''
        n = (size + bpc - 1) // bpc if size else 1 << 30
        while 2 <= c < 0xFFFFFFF7 and n:
            out += img[heap * bps + (c - 2) * bpc:heap * bps + (c - 1) * bpc]
            c = c + 1 if nofat else nxt(c)
            n -= 1
        return out[:size] if size else out
    d = chain(root, 0, False)
    files, i = [], 0
    while i < len(d) and d[i] != 0:
        e = d[i:i + 32]
        if e[0] != 0x85:
            i += 32
            continue
        n = e[1]
        attr, = struct.unpack_from('<H', e, 4)
        mark = e[6]
        st = d[i + 32:i + 64]
        nl = st[3]
        first, size = struct.unpack_from('<IQ', st, 20)
        name = b''.join(d[i + 32 * k + 2:i + 32 * k + 32] for k in range(2, n + 1))
        name = name.decode('utf-16-le')[:nl]
        files.append((name, attr, mark, first, size, bool(st[1] & 2)))
        i += 32 * (n + 1)
    by = {f[0].upper(): f for f in files}
    out = []
    for name, attr, mark, first, size, nofat in files:
        if name.upper().endswith(' EA. SF'):
            if attr & 7 != 7:
                die('%s: attributes %02x' % (name, attr))
            own = by.get(name[:-7].upper())
            if not own or not own[2] & 0xC0:
                die('%s: no marked file beside it' % name)
            continue
        if mark & 0xC0:
            sf = by.get((name + ' EA. SF').upper())
            if not sf:
                die('%s: marked %02x without its EA file' % (name, mark))
            eas = fealist(chain(sf[3], sf[4], sf[5]), name)
            if (mark == 0x80) != any(f & 0x80 for _, _, f in eas):
                die('%s: mark %02x' % (name, mark))
            out.append('%s %02x %s' % (name, mark, ' '.join('%s:%d' % (a, b) for a, b, _ in eas)))
    for line in sorted(out):
        print(line)


if sys.argv[1] == '--fealist':
    for name, vl, _ in fealist(open(sys.argv[2], 'rb').read(), sys.argv[2]):
        print('%s:%d' % (name, vl))
    sys.exit(0)
if sys.argv[1] == '--mark':
    print('%02x' % fat32_mark(sys.argv[2], sys.argv[3]))
    sys.exit(0)
if sys.argv[1] == '--exfat':
    exfat_root(sys.argv[2])
    sys.exit(0)
if sys.argv[1] == '--image':
    data, bpc = from_image(sys.argv[2])
else:
    data, bpc = open(sys.argv[1], 'rb').read(), int(sys.argv[2])
want = dict(a.split('=', 1) for a in sys.argv[3:])


if data[:2] != b'ED' or len(data) % bpc:
    die('no header, or not whole clusters')
base = struct.unpack_from('<240H', data, 32)
table_end = base[0] * bpc
handles = (table_end - 512) // 2
sets = {}
for h in range(min(handles, 30720)):
    off, = struct.unpack_from('<H', data, 512 + 2 * h)
    if off == 0xFFFF:
        continue
    if h == 0:
        die('handle 0 used')
    cl = base[h >> 7] + off
    pos = cl * bpc
    if data[pos:pos + 2] != b'EA':
        die('handle %d: no set at cluster %d' % (h, cl))
    own, need = struct.unpack_from('<HI', data, pos + 2)
    owner = data[pos + 8:pos + 22].rstrip(b'\0').decode('latin-1')
    cb, = struct.unpack_from('<I', data, pos + 26)
    if own != h:
        die('handle %d: the set names %d' % (h, own))
    eas, p, end, needs = [], pos + 30, pos + 26 + cb, 0
    while p < end:
        flags, nl, vl = struct.unpack_from('<BBH', data, p)
        name = data[p + 4:p + 4 + nl].decode('latin-1')
        if data[p + 4 + nl] != 0:
            die('handle %d: name %s without its NUL' % (h, name))
        eas.append((name, vl))
        needs += flags >> 7
        p += 5 + nl + vl
    if p != end or not eas:
        die('handle %d: cbList %d does not end on an EA' % (h, cb))
    if needs != need:
        die('handle %d: need count %d, not %d' % (h, need, needs))
    sets[cl] = (h, (26 + cb + bpc - 1) // bpc, owner, eas)
    if str(h) in want and want.pop(str(h)) != owner:
        die('handle %d belongs to %s' % (h, owner))
if want:
    die('no sets for %s' % ', '.join(want))
cl = base[0]
for c in sorted(sets):
    if c != cl:
        die('a gap or overlap before cluster %d (expected %d)' % (c, cl))
    cl += sets[c][1]
if cl * bpc != len(data):
    die('%d bytes after the last set' % (len(data) - cl * bpc))
for c in sorted(sets):
    h, n, owner, eas = sets[c]
    print('%d %s %s' % (h, owner, ' '.join('%s:%d' % e for e in eas)))

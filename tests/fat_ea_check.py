# SPDX-License-Identifier: GPL-2.0-or-later
# fat_ea_check.py <EA DATA. SF> <bytes per cluster> [handle=OWNER ...],
# fat_ea_check.py --image <FAT12/16 image> [handle=OWNER ...]:
# checks the OS/2 / NT layout of an EA file independently of src/fat/fat_ea.c
# (after the fastfat layout): the header, the offset table, every used
# handle on a set of its own whose header names it, the sets one after the
# other without gaps from EaBaseTable[0] to the end. With handle=OWNER the
# set of that handle must belong to the 8.3 name OWNER. Prints the sets.
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

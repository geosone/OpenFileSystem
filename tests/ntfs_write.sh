#!/bin/sh
# NTFS writing through ofsfuse on images of tests/ntfs_image.sh (clusters
# of 4 KB and 512 bytes on 512-byte sectors, a volume of 4096-byte
# sectors). Stage 1, overwriting in place: data overwritten in a resident
# file, at the start, within and across clusters and at the end of larger
# files, across the runs of a fragmented file and of one whose runs need an
# attribute list. Stage 2a, sizes: appending to a resident file until it
# moves out of its record, truncating and growing again (zeros), writing
# far past the end, appending to a fragmented file, emptying a file and
# filling it again, the volume filled up (ENOSPC) and the file cut back.
# Stage 2b, creating: files with 8.3 and long names (also beyond Latin-1),
# directories in directories, 600 files in a new directory and 200 more in
# the root (index nodes split, the root moved into a block, $MFT grown),
# written to afterwards. Refused: a name there in other case, a name
# Win32 does not allow, removing, writing into a hole of a sparse file or
# into a compressed file. Then ntfsfix finds the volume clean, tests/ntfs_check.py
# finds every cluster accounted for in $Bitmap and none used twice, ntfs-3g
# reads every file as expected, and a written file has a new time (the
# others keep theirs). Extended attributes (Windows NT's $EA): set, read,
# listed, replaced, removed through ofsfuse as user.* xattrs, on files and
# a directory, next to one ntfs-3g wrote (system.ntfs_ea), too large ones
# refused (ENOSPC: not in the MFT record, not yet); then
# tests/ntfs_ea_check.py reads them with ntfscat ($EA, $EA_INFORMATION,
# the packed size in every $FILE_NAME) and ntfs-3g reads them back. A
# volume with a hibernated Windows on it (hiberfil.sys starting with
# "hibr") is mounted read-only.
set -eu
cd "$(dirname "$0")/.."
F=${OFSFUSE:-build/host/ofsfuse}
fail() { echo "$v: $*"; fusermount -u "$m" 2>/dev/null || true; exit 1; }
umnt() {   # a while busy, as long as the last file is still being let go of
    for i in 1 2 3 4 5; do fusermount -u "$m" 2>/dev/null && return; sleep 1; done
    fusermount -u "$m"
}
# both: a shell command run on the volume ($m) and on the expected tree ($e)
both() {
    (e=$m; eval "$1") || fail "on the volume: $1"
    (e=$d/expect; eval "$1")
}
# patch FILE OFFSET LENGTH: the same random bytes into the volume and the expected tree
patch() {
    head -c "$3" /dev/urandom > "$d/p.bin"
    dd if="$d/p.bin" of="$m/$1" bs="$3" count=1 seek="$2" oflag=seek_bytes conv=notrunc status=none ||
        fail "cannot overwrite $1 at $2"
    dd if="$d/p.bin" of="$d/expect/$1" bs="$3" count=1 seek="$2" oflag=seek_bytes conv=notrunc status=none
}
refused() {   # what: the command must fail
    w=$1; shift
    if "$@" 2>/dev/null; then fail "$w was not refused"; fi
}
for v in 4096:512 512:512 4096:4096; do
    c=${v%:*}; s=${v#*:}
    d=build/ntfs-w-$c-$s m=build/ntfs-w-$c-$s/mnt
    sh tests/ntfs_image.sh "$d" 64 "$c" "$s" >/dev/null
    rm -rf "$d/expect"; cp -r "$d/files" "$d/expect"
    mkdir -p "$m"
    # a file with a real hole (ntfs-3g leaves it sparse)
    ntfs-3g "$d/ntfs.img" "$m"
    printf A > "$m/loch.dat"
    printf B | dd of="$m/loch.dat" bs=1 seek=1048576 conv=notrunc status=none
    cp "$m/loch.dat" "$d/expect/loch.dat"
    # an EA as ntfs-3g writes it (the raw $EA): VON3G = "ja" on SUB/A.B
    python3 -I -c 'import os, struct, sys; n, v = b"VON3G", b"ja"; e = struct.pack("<IBBH", (8 + len(n) + 1 + len(v) + 3) & ~3, 0, len(n), len(v)) + n + b"\0" + v; e += bytes(-len(e) % 4); os.setxattr(sys.argv[1], "system.ntfs_ea", e)' "$m/SUB/A.B" ||
        fail "ntfs-3g does not take an EA"
    t_gross=$(stat -c %Y "$m/gross.bin"); t_klein=$(stat -c %Y "$m/SUB/A.B")
    umnt
    sleep 2
    ntfsinfo -v -F /loch.dat "$d/ntfs.img" 2>/dev/null | grep -q HOLE || fail "loch.dat has no hole"
    "$F" -o sector="$s" "$d/ntfs.img" "$m" 2>"$d/mount.txt"
    grep -q "read-only" "$d/mount.txt" && fail "mounted read-only"
    size=$(stat -c %s "$d/files/gross.bin")
    patch README.TXT 1 3
    patch gross.bin 0 512
    patch gross.bin 1000 70000
    patch gross.bin $((size - 100)) 100
    patch frag.bin 4000 150000
    patch viele.bin 0 "$(stat -c %s "$d/files/viele.bin")"
    patch sparse.dat 0 3
    # stage 2a
    head -c 6000 /dev/urandom > "$d/r.bin"
    both 'printf x >> "$e/README.TXT"'
    both 'cat "$d/r.bin" >> "$e/README.TXT"'              # out of the record
    both 'truncate -s 1000 "$e/gross.bin"'
    both 'truncate -s 200000 "$e/gross.bin"'
    patch gross.bin 150000 100                           # past the initialized size
    patch klein.txt 100000 10                            # far past the end of a resident file
    head -c 500000 /dev/urandom > "$d/r.bin"
    both 'cat "$d/r.bin" >> "$e/frag.bin"'
    both ': > "$e/zufall.bin"'
    head -c 5000 /dev/urandom > "$d/r.bin"
    both 'cat "$d/r.bin" >> "$e/zufall.bin"'
    full=$(stat -c %s "$d/files/sparse.dat")
    # filled up: no space (or, where only single free clusters are left, a
    # runlist larger than its record: not yet), the volume full but for those
    if LC_ALL=C head -c 200000000 /dev/zero 2>"$d/fill.txt" >> "$m/sparse.dat"; then fail "200 MB fitted"; fi
    grep -qE "No space left|Operation not supported" "$d/fill.txt" || { cat "$d/fill.txt"; fail "filling"; }
    # (the image keeps 600 single free clusters: 2.4 MB of 4 KB clusters)
    test "$(df -B1 --output=avail "$m" | tail -1)" -lt 8388608 || fail "filling stopped with space left"
    both "truncate -s $full \"\$e/sparse.dat\""
    # stage 2b
    both 'echo neu > "$e/NEU.TXT"'
    both 'mkdir "$e/Neuer Ordner" && echo klein > "$e/Neuer Ordner/klein.txt"'
    both 'mkdir -p "$e/a/b/c" && echo tief > "$e/a/b/c/Tiefe Datei.txt"'
    both 'echo umlaut > "$e/Ärger über Öl.txt" && echo omega > "$e/Ωmega Σigma.txt"'
    both 'mkdir "$e/Neu viele" || exit 1; i=0; while [ $i -lt 600 ]; do echo $i > "$e/Neu viele/Datei mit einem längeren Namen $i.txt" || exit 1; i=$((i + 1)); done'
    both 'i=0; while [ $i -lt 200 ]; do echo $i > "$e/R$i.DAT" || exit 1; i=$((i + 1)); done'
    head -c 70000 /dev/urandom > "$d/r.bin"
    both 'cat "$d/r.bin" >> "$e/Neu viele/Datei mit einem längeren Namen 333.txt"'
    test "$(cat "$m/Neu viele/Datei mit einem längeren Namen 599.txt")" = 599 || fail "a new file reads back wrong"
    # times and the read-only attribute (utimens, chmod)
    touch -m -d '2001-02-03 04:05:06.25' "$m/NEU.TXT" || fail "touch -d"
    touch -a -d '2002-03-04 05:06:07' "$m/NEU.TXT" || fail "touch -a -d"
    chmod a-w "$m/a/b/c/Tiefe Datei.txt" || fail "chmod a-w"
    # extended attributes
    python3 -I - "$m" <<'PY' || fail "extended attributes"
import errno, os, sys
m = sys.argv[1]
def err(f, *a):
    try:
        f(*a)
    except OSError as e:
        return e.errno
    return 0
a, d, u = os.path.join(m, 'NEU.TXT'), os.path.join(m, 'Neuer Ordner'), os.path.join(m, 'Ärger über Öl.txt')
b = os.path.join(m, 'SUB', 'A.B')
assert os.listxattr(a) == []
os.setxattr(a, 'user.Kommentar', b'Hallo, NT')
os.setxattr(a, 'user.bin', bytes([0, 255, 1, 2, 3]))
os.setxattr(d, 'user.ordner', b'ja')
os.setxattr(u, 'user.x', b'1')
assert os.getxattr(a, 'user.KOMMENTAR') == b'Hallo, NT', 'names without case'
assert sorted(os.listxattr(a)) == ['user.Kommentar', 'user.bin']
assert os.getxattr(b, 'user.VON3G') == b'ja', "ntfs-3g's EA"
os.setxattr(b, 'user.zwei', b'2' * 100)
assert sorted(os.listxattr(b)) == ['user.VON3G', 'user.zwei']
assert err(os.setxattr, a, 'user.bin', b'x', os.XATTR_CREATE) == errno.EEXIST
assert err(os.setxattr, a, 'user.neu', b'x', os.XATTR_REPLACE) == errno.ENODATA
assert err(os.getxattr, a, 'user.fehlt') == errno.ENODATA
assert err(os.setxattr, a, 'user.gross', bytes(5000)) == errno.ENOSPC, 'not in the record'
assert sorted(os.listxattr(a)) == ['user.Kommentar', 'user.bin'], 'a refused one changes nothing'
os.setxattr(a, 'user.Kommentar', b'Ein laengerer Kommentar' * 4, os.XATTR_REPLACE)
os.removexattr(a, 'user.bin')
os.setxattr(u, 'user.x', b'')
os.removexattr(u, 'user.x')
assert os.listxattr(u) == []
PY
    refused "a name there in other case" mkdir "$m/neu.txt"
    refused "a name Win32 does not allow" touch "$m/a:b"
    refused "removing" rm "$m/klein.txt"
    refused "writing into a hole" dd if=/dev/zero of="$m/loch.dat" bs=100 count=1 seek=500000 \
        oflag=seek_bytes conv=notrunc status=none
    patch loch.dat 0 1
    refused "writing into a compressed file" dd if=/dev/zero of="$m/KOMPR/zahlen.txt" bs=10 count=1 \
        conv=notrunc status=none
    umnt
    ntfsfix -n "$d/ntfs.img" >/dev/null || fail "ntfsfix finds the volume damaged"
    python3 -I tests/ntfs_check.py "$d/ntfs.img" >"$d/check.txt" || { cat "$d/check.txt"; fail "clusters"; }
    for e in "/NEU.TXT|Kommentar=92" "/Neuer Ordner|ordner=2" "/SUB/A.B|VON3G=2 zwei=100" "/Ärger über Öl.txt|" "/README.TXT|"; do
        f=${e%%|*} w=${e#*|}
        # shellcheck disable=SC2086
        python3 -I tests/ntfs_ea_check.py "$d/ntfs.img" "$f" $w >"$d/ea.txt" || { cat "$d/ea.txt"; fail "the EAs of $f"; }
    done
    ntfs-3g -o ro "$d/ntfs.img" "$m"
    python3 -I -c 'import os, sys; r = os.getxattr(sys.argv[1], "system.ntfs_ea"); assert b"KOMMENTAR" in r.upper() and b"Ein laengerer Kommentar" in r, r' "$m/NEU.TXT" ||
        fail "ntfs-3g does not read the EA"
    diff -r "$m" "$d/expect" >/dev/null || { diff -rq "$m" "$d/expect" | head -5; fail "contents differ"; }
    test "$(stat -c %Y "$m/gross.bin")" -gt "$t_gross" || fail "gross.bin has no new time"
    dt=$(( $(date +%s) - $(stat -c %Y "$m/gross.bin") ))
    test "$dt" -ge -5 && test "$dt" -le 300 || fail "gross.bin's time is ${dt}s off (time zone?)"
    test "$(stat -c %Y "$m/SUB/A.B")" = "$t_klein" || fail "SUB/A.B has a new time"
    test "$(stat -c %y "$m/NEU.TXT")" = "$(date -d '2001-02-03 04:05:06.25' '+%Y-%m-%d %H:%M:%S.250000000 %z')" ||
        fail "NEU.TXT's modification time: $(stat -c %y "$m/NEU.TXT")"
    test "$(stat -c %X "$m/NEU.TXT")" = "$(date -d '2002-03-04 05:06:07' +%s)" || fail "NEU.TXT's access time"
    umnt
    ntfsinfo -F "/a/b/c/Tiefe Datei.txt" "$d/ntfs.img" 2>/dev/null | grep -q "File attributes:.*READONLY" ||
        fail "Tiefe Datei.txt is not read-only"
    echo "NTFS writing with $c-byte clusters on $s-byte sectors passed"
done
# hibernated: read-only
v=hiberfil d=build/ntfs-w-4096-512 m=build/ntfs-w-4096-512/mnt
ntfs-3g "$d/ntfs.img" "$m"
{ printf hibr; head -c 8188 /dev/zero; } > "$m/hiberfil.sys"
umnt
"$F" "$d/ntfs.img" "$m" 2>"$d/mount.txt"
grep -q "read-only" "$d/mount.txt" || fail "a hibernated volume was mounted writable"
refused "writing to a hibernated volume" dd if=/dev/zero of="$m/gross.bin" bs=10 count=1 conv=notrunc status=none
umnt
echo "NTFS with a hibernated Windows stays read-only: passed"

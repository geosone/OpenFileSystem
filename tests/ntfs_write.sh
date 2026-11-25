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
# Refused: creating, writing into a hole of a sparse file or into a
# compressed file. Then ntfsfix finds the volume clean, tests/ntfs_check.py
# finds every cluster accounted for in $Bitmap and none used twice, ntfs-3g
# reads every file as expected, and a written file has a new time (the
# others keep theirs). A volume with a hibernated Windows on it (hiberfil.sys
# starting with "hibr") is mounted read-only.
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
    refused "creating" sh -c "printf x > '$m/NEU.TXT'"
    refused "writing into a hole" dd if=/dev/zero of="$m/loch.dat" bs=100 count=1 seek=500000 \
        oflag=seek_bytes conv=notrunc status=none
    patch loch.dat 0 1
    refused "writing into a compressed file" dd if=/dev/zero of="$m/KOMPR/zahlen.txt" bs=10 count=1 \
        conv=notrunc status=none
    umnt
    ntfsfix -n "$d/ntfs.img" >/dev/null || fail "ntfsfix finds the volume damaged"
    python3 -I tests/ntfs_check.py "$d/ntfs.img" >"$d/check.txt" || { cat "$d/check.txt"; fail "clusters"; }
    ntfs-3g -o ro "$d/ntfs.img" "$m"
    diff -r "$m" "$d/expect" >/dev/null || { diff -rq "$m" "$d/expect" | head -5; fail "contents differ"; }
    test "$(stat -c %Y "$m/gross.bin")" -gt "$t_gross" || fail "gross.bin has no new time"
    dt=$(( $(date +%s) - $(stat -c %Y "$m/gross.bin") ))
    test "$dt" -ge -5 && test "$dt" -le 300 || fail "gross.bin's time is ${dt}s off (time zone?)"
    test "$(stat -c %Y "$m/SUB/A.B")" = "$t_klein" || fail "SUB/A.B has a new time"
    umnt
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

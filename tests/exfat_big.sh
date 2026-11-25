#!/bin/bash
# exFAT past 32 bits (src/exfat/exfat.c with 64-bit sector numbers and
# sizes): a sparse 3 TiB volume, with 512-byte sectors more than 2^32
# sectors, and the same with 4 KiB sectors.
# tests/exfat_big_test.c writes a file and a directory into its last
# clusters (past 2 TiB)
# and a file larger than 4 GiB and reads both back; fsck.exfat checks the
# volume and both files must read the same through the Linux exFAT driver.
# The exFAT check (src/exfat/exfat_check.c, build/host/test_exchk) must
# find the volume clean, with both files and the directory HIGHDIR (with
# a file in it) whose cluster lies past 2^32 sectors.
# With 512-byte sectors the exFAT DEFRAG (src/exfat/exfat_defrag.c, full)
# then moves HIGH.BIN and HIGHDIR down from past 2^32 sectors: the tree
# (names, sizes, CRC-32) must stay the same and the check clean.
# Needs about 4.1 GiB of disk space for the large file.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/exfat-big}
make -C "$ROOT" -s build/host/test_exfat_big build/host/test_exchk build/host/test_exdefrag
t=$ROOT/build/host/test_exfat_big
chk=$ROOT/build/host/test_exchk
dfr=$ROOT/build/host/test_exdefrag
rm -rf "$OUT"; mkdir -p "$OUT"
img=$OUT/big.img
trap 'rm -f "$img"' EXIT
for ss in 512 4096; do
    rm -f "$img"; truncate -s 3T "$img"
    mkfs.exfat -q -s "$ss" -c 1M -L BIG "$img" 2>/dev/null || mkfs.exfat -q -c 1M -L BIG "$img"
    "$t" "$img" > "$OUT/test$ss.txt" || { cat "$OUT/test$ss.txt"; echo "$ss: test_exfat_big failed"; exit 1; }
    fsck.exfat -n "$img" > "$OUT/fsck$ss.txt" 2>&1 || { cat "$OUT/fsck$ss.txt"; echo "$ss: fsck.exfat finds errors"; exit 1; }
    SECTOR=$ss "$chk" "$img" > "$OUT/chk$ss.txt" && grep -q '^3 files, 1 directories' "$OUT/chk$ss.txt" ||
        { cat "$OUT/chk$ss.txt"; echo "$ss: the exFAT check finds errors"; exit 1; }
    loop=$(udisksctl loop-setup --no-user-interaction -r -f "$img" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
    udisksctl mount --no-user-interaction -b "$loop" >/dev/null
    mnt=$(lsblk -no MOUNTPOINT "$loop")
    rc=0
    "$t" -v "$mnt/HIGH.BIN" 1 && "$t" -v "$mnt/BIG.BIN" 2 || rc=1
    udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
    udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
    test $rc = 0 || { echo "$ss: the Linux driver reads other data"; exit 1; }
    if test "$ss" = 512; then
        "$dfr" "$img" T > "$OUT/tree-before.txt"
        "$dfr" "$img" F > "$OUT/defrag.txt" || { cat "$OUT/defrag.txt"; echo "512: defrag failed"; exit 1; }
        grep -q 'gaps 0$' "$OUT/defrag.txt" || { cat "$OUT/defrag.txt"; echo "512: gaps left after the defrag"; exit 1; }
        "$dfr" "$img" T > "$OUT/tree-after.txt"
        cmp -s "$OUT/tree-before.txt" "$OUT/tree-after.txt" ||
            { diff "$OUT/tree-before.txt" "$OUT/tree-after.txt"; echo "512: the defrag changed files"; exit 1; }
        "$chk" "$img" > "$OUT/chk-defrag.txt" && grep -q '^3 files, 1 directories' "$OUT/chk-defrag.txt" ||
            { cat "$OUT/chk-defrag.txt"; echo "512: the check finds errors after the defrag"; exit 1; }
        fsck.exfat -n "$img" > "$OUT/fsck-defrag.txt" 2>&1 ||
            { cat "$OUT/fsck-defrag.txt"; echo "512: fsck.exfat finds errors after the defrag"; exit 1; }
        echo "512-byte sectors: defragmented past 2^32 sectors ($(tail -1 "$OUT/defrag.txt"))"
    fi
    echo "$ss-byte sectors passed: $(tail -1 "$OUT/test$ss.txt")"
done
echo "exFAT big volume tests passed"

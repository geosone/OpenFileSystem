#!/bin/sh
# ofs_bmap (tests/bmap_test.c) on images of mkfs.fat, mkfs.exfat and mkntfs:
# fragmented files (written between others that are removed again) of
# FAT12/16/32, exFAT and NTFS, 512- and 4096-byte sectors, and NTFS's
# resident data (no sectors of its own).
set -eu
H=${H:-build/host}
for t in mkfs.fat mcopy mdel mkfs.exfat mkntfs ntfscp; do
	command -v "$t" >/dev/null || { echo "bmap_host: SKIP ($t not found)"; exit 0; }
done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export MTOOLS_SKIP_CHECK=1
head -c 300000 /dev/urandom >"$work/big"
head -c 200000 /dev/urandom >"$work/mid"
head -c 5000 /dev/urandom >"$work/small"
printf 'tiny\r\n' >"$work/tiny"
fat() { # FAT bits, MiB, sector size
	img=$work/fat$1.img
	rm -f "$img"; truncate -s "$2M" "$img"
	mkfs.fat -F "$1" -S "$3" -s 1 "$img" >/dev/null 2>&1
	for i in 1 2 3 4 5 6; do mcopy -i "$img" "$work/small" "::S$i"; done
	mdel -i "$img" ::S2 ::S4
	mcopy -i "$img" "$work/${4:-big}" ::BIG
	"$H/test_bmap" "$img" "$3" BIG
	"$H/test_bmap" "$img" "$3" S5
}
fat 12 1 512 mid
fat 16 32 512
fat 32 64 512
fat 32 64 4096
img=$work/ex.img
truncate -s 64M "$img"
mkfs.exfat "$img" >/dev/null
"$H/ofimg" "$img" 0 put "$work/small" A put "$work/small" B put "$work/small" C rm B put "$work/big" BIG >/dev/null
"$H/test_bmap" "$img" 512 BIG
"$H/test_bmap" "$img" 512 A
for ss in 512 4096; do
	img=$work/nt$ss.img
	rm -f "$img"; truncate -s 64M "$img"
	mkntfs -q -F -Q -s "$ss" -c 4096 "$img" >/dev/null 2>&1
	ntfscp -q -f "$img" "$work/small" A
	ntfscp -q -f "$img" "$work/big" BIG
	ntfscp -q -f "$img" "$work/tiny" TINY
	"$H/test_bmap" "$img" "$ss" BIG
	"$H/test_bmap" "$img" "$ss" A
	"$H/test_bmap" "$img" "$ss" TINY resident
done
echo "bmap_host: ok"

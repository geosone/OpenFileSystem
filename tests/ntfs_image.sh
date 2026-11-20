#!/bin/sh
# Makes an NTFS test image with mkntfs and fills it through the Linux NTFS
# driver (udisksctl: ntfs3 or ntfs-3g, no root needed): long and German
# names without DOS names (8.3 names come from "~n"), names that collide in
# 8.3, subdirectories, a directory large enough for index blocks, small
# files (resident data), fragmented, sparse and larger files.  Then through
# ntfs-3g (FUSE): a long name with a DOS name of its own (LANGDOS.TXT), a
# file in so many pieces that its runs need an attribute list (viele.bin),
# and compressed files (LZNT1) in KOMPR.  The files are
# also kept in DIR/files for comparison.
#   tests/ntfs_image.sh DIR [SIZE_MB [CLUSTER]]   -> DIR/ntfs.img
set -eu
dir=$1; size=${2:-64}; cluster=${3:-4096}
img=$dir/ntfs.img; files=$dir/files
mkdir -p "$dir"
rm -rf "$files" "$img"
mkdir -p "$files/Unterordner mit langem Namen/Tiefer" "$files/SUB" "$files/VIELE"

printf 'hallo\r\n' > "$files/README.TXT"
printf 'kleingeschrieben\r\n' > "$files/klein.txt"
printf 'Umlaute\r\n' > "$files/Ärger über Größe.txt"
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    printf 'Datei %s\r\n' "$i" > "$files/Ein langer Name $i.txt"
done
printf 'tief\r\n' > "$files/Unterordner mit langem Namen/Tiefer/Datei.dat"
printf 'sub\r\n' > "$files/SUB/A.B"
printf 'zwei Punkte\r\n' > "$files/archiv.tar.gz"
head -c 300000 /dev/urandom > "$files/zufall.bin"
head -c 3000000 /dev/urandom > "$files/gross.bin"
for i in $(seq 1 150); do
    printf 'Nummer %s\r\n' "$i" > "$files/VIELE/DATEI$i.TXT"
done

truncate -s "${size}M" "$img"
mkntfs -q -F -Q -c "$cluster" -L OPENASPI "$img" >/dev/null 2>&1
loop=$(udisksctl loop-setup --no-user-interaction -f "$img" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
trap 'udisksctl unmount --no-user-interaction -b "$loop" >/dev/null 2>&1 || true; udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null 2>&1 || true' EXIT
udisksctl mount --no-user-interaction -b "$loop" >/dev/null
mnt=$(lsblk -no MOUNTPOINT "$loop")
cp -r "$files/." "$mnt/"
# fragmented: two files written in turns, one of them deleted afterwards
for i in $(seq 1 40); do
    head -c 5000 /dev/urandom >> "$mnt/frag.bin"
    head -c 5000 /dev/urandom >> "$mnt/luecke.tmp"
    sync "$mnt/frag.bin"
done
rm "$mnt/luecke.tmp"
cp "$mnt/frag.bin" "$files/frag.bin"
# sparse: a hole between two written parts
printf 'Anfang' > "$mnt/sparse.dat"
truncate -s 200000 "$mnt/sparse.dat"
printf 'Ende' >> "$mnt/sparse.dat"
cp "$mnt/sparse.dat" "$files/sparse.dat"
sync
udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
trap - EXIT
mnt=$dir/mnt
mkdir -p "$mnt"
ntfs-3g "$img" "$mnt"
trap 'fusermount -u "$mnt" 2>/dev/null || true' EXIT
printf 'DOS-Name\r\n' > "$mnt/Langer Dateiname mit DOS.txt"
setfattr -n system.ntfs_dos_name -v LANGDOS.TXT "$mnt/Langer Dateiname mit DOS.txt"
cp "$mnt/Langer Dateiname mit DOS.txt" "$files/"
# holes of one cluster (at least 2 KB: smaller files stay in the MFT): small
# files, the rest of the volume filled, every other small file deleted;
# viele.bin then has to take the holes
piece=$cluster; test "$piece" -ge 2048 || piece=2048
mkdir "$mnt/LOCH"
for i in $(seq 1 600); do
    head -c "$piece" /dev/zero > "$mnt/LOCH/a$i"
    head -c "$piece" /dev/zero > "$mnt/LOCH/b$i"
done
free=$(df -B1 --output=avail "$mnt" | tail -1)
fallocate -l $((free - 600 * piece - 1048576)) "$mnt/fill.bin" 2>/dev/null ||
    head -c $((free - 600 * piece - 1048576)) /dev/zero > "$mnt/fill.bin"
rm "$mnt"/LOCH/b*
head -c $((600 * piece)) /dev/urandom > "$mnt/viele.bin"
rm -r "$mnt/fill.bin" "$mnt/LOCH"
cp "$mnt/viele.bin" "$files/viele.bin"
mkdir "$mnt/KOMPR" "$files/KOMPR"
setfattr -n system.ntfs_attrib_be -v 0x00000800 "$mnt/KOMPR"
# compressed units, units stored plain (random data), units of zeros
# only (no clusters) and chunks stored within compressed units
seq 1 20000 > "$files/KOMPR/zahlen.txt"
head -c 70000 /dev/urandom > "$files/KOMPR/zufall.bin"
{ seq 1 8000; head -c 6000 /dev/urandom; head -c 200000 /dev/zero; seq 5 7 30000; } > "$files/KOMPR/gemischt.bin"
cp "$files/KOMPR/"* "$mnt/KOMPR/"
fusermount -u "$mnt"
trap - EXIT
rmdir "$mnt"
ntfsinfo -F /viele.bin "$img" 2>/dev/null | grep -q ATTRIBUTE_LIST
ntfsfix -n "$img" >/dev/null
echo "$img"

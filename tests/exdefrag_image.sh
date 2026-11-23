#!/bin/sh
# Makes a fragmented exFAT image for the exFAT DEFRAG tests with mkfs.exfat
# and the Linux exFAT driver (udisksctl, no root needed): six files written
# in turns, a directory with more entries than its first cluster holds that
# grows in turns with a file (so the directory itself is fragmented) and has
# subdirectories of its own, deleted files that leave gaps, an empty file, a
# file with a valid data length below its size.  The files are also kept in
# DIR/files.  Without udisksctl (or with EXFAT_WRITER=ofimg) the same
# steps go through tools/ofimg.c (OpenFileSystem's own exFAT writer)
# instead of mkfs.exfat and Linux's driver.
#   tests/exdefrag_image.sh DIR [SIZE_MB [CLUSTER]]   -> DIR/exfat.img
set -eu
dir=$1; size=${2:-32}; cluster=${3:-4K}
img=$dir/exfat.img; files=$dir/files
mkdir -p "$dir"
rm -rf "$files" "$img"
truncate -s "${size}M" "$img"
writer=${EXFAT_WRITER:-}
if [ -z "$writer" ]; then
    writer=ofimg
    command -v udisksctl >/dev/null && command -v mkfs.exfat >/dev/null && writer=linux
fi
if [ "$writer" = ofimg ]; then
    ROOT=$(cd "$(dirname "$0")/.." && pwd)
    make -C "$ROOT" -s build/host/ofimg
    case $cluster in *K) cb=$((${cluster%K} * 1024)) ;; *M) cb=$((${cluster%M} * 1048576)) ;; *) cb=$cluster ;; esac
    tmp=$dir/chunks; rm -rf "$tmp"; mkdir -p "$tmp" "$files/Folder with files"
    set -- cluster "$cb" align 2048 format $((size * 2048)) DEFRAG
    k=0
    chunk() {   # BYTES -> a new host file, its name in $c
        k=$((k + 1)); c=$tmp/$k; head -c "$1" /dev/urandom >"$c"
    }
    for i in $(seq 1 12); do
        for f in A.BIN B.BIN "Long name C.dat" D.BIN E.BIN F.BIN; do
            chunk 3000; cat "$c" >>"$files/$f"; set -- "$@" append "$c" "$f"
        done
    done
    set -- "$@" mkdir "Folder with files"
    for i in $(seq 1 60); do
        k=$((k + 1)); c=$tmp/$k; printf 'file %s\r\n' "$i" >"$c"
        cp "$c" "$files/Folder with files/A long file name number $i.txt"
        set -- "$@" put "$c" "Folder with files/A long file name number $i.txt"
        chunk 2500; cat "$c" >>"$files/grow.bin"; set -- "$@" append "$c" grow.bin
        if test "$i" = 10 || test "$i" = 50; then
            mkdir "$files/Folder with files/Sub $i"
            k=$((k + 1)); c=$tmp/$k; printf 'deep\r\n' >"$c"; cp "$c" "$files/Folder with files/Sub $i/deep.txt"
            set -- "$@" mkdir "Folder with files/Sub $i" put "$c" "Folder with files/Sub $i/deep.txt"
            chunk 9000; cp "$c" "$files/Folder with files/Sub $i/data.bin"; set -- "$@" put "$c" "Folder with files/Sub $i/data.bin"
        fi
        if test "$i" = 40; then
            chunk 12000; cp "$c" "$files/Folder with files/late.bin"; set -- "$@" put "$c" "Folder with files/late.bin"
        fi
    done
    rm "$files/B.BIN" "$files/E.BIN"
    : >"$files/EMPTY.TXT"
    printf 'start' >"$files/sparse.dat"; truncate -s 20000 "$files/sparse.dat"
    printf 'start' >"$tmp/sparse"
    chunk 40000; cp "$c" "$files/last.bin"
    set -- "$@" rm B.BIN rm E.BIN put /dev/null EMPTY.TXT put "$tmp/sparse" sparse.dat size sparse.dat 20000 \
        put "$c" last.bin
    "$ROOT/build/host/ofimg" "$img" 0 "$@"
    rm -rf "$tmp"
    ! command -v fsck.exfat >/dev/null || fsck.exfat -n "$img" >/dev/null
    echo "$img"
    exit 0
fi
mkfs.exfat -q -c "$cluster" -L DEFRAG "$img"
loop=$(udisksctl loop-setup --no-user-interaction -f "$img" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
trap 'udisksctl unmount --no-user-interaction -b "$loop" >/dev/null 2>&1 || true; udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null 2>&1 || true' EXIT
udisksctl mount --no-user-interaction -b "$loop" >/dev/null
mnt=$(lsblk -no MOUNTPOINT "$loop")
for i in $(seq 1 12); do
    for f in A.BIN B.BIN "Long name C.dat" D.BIN E.BIN F.BIN; do
        head -c 3000 /dev/urandom >> "$mnt/$f"
        sync "$mnt/$f"
    done
done
mkdir "$mnt/Folder with files"
for i in $(seq 1 60); do
    printf 'file %s\r\n' "$i" > "$mnt/Folder with files/A long file name number $i.txt"
    sync "$mnt/Folder with files"
    head -c 2500 /dev/urandom >> "$mnt/grow.bin"
    sync "$mnt/grow.bin"
    if test "$i" = 10 || test "$i" = 50; then
        mkdir "$mnt/Folder with files/Sub $i"
        printf 'deep\r\n' > "$mnt/Folder with files/Sub $i/deep.txt"
        head -c 9000 /dev/urandom > "$mnt/Folder with files/Sub $i/data.bin"
    fi
    if test "$i" = 40; then
        head -c 12000 /dev/urandom > "$mnt/Folder with files/late.bin"
    fi
done
rm "$mnt/B.BIN" "$mnt/E.BIN"
: > "$mnt/EMPTY.TXT"
printf 'start' > "$mnt/sparse.dat"
truncate -s 20000 "$mnt/sparse.dat"
head -c 40000 /dev/urandom > "$mnt/last.bin"
sync
rm -rf "$files"; cp -r "$mnt" "$files"
udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
trap - EXIT
fsck.exfat -n "$img" >/dev/null
echo "$img"

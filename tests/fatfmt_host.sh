#!/bin/sh
# The FAT layouts of ASPIFMT/OFORMAT on the host (build/host/test_fat_format
# writing the metadata into sparse images): FAT12/16/32 on 512- and on
# 4096-byte sectors (4Kn disks).  Each image takes a file through mtools and
# is checked by fsck.fat.
#   tests/fatfmt_host.sh [DIR]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$ROOT/build/fatfmt-host}
fmt=$ROOT/build/host/test_fat_format
make -C "$ROOT" -s build/host/test_fat_format
mkdir -p "$dir"
export MTOOLS_SKIP_CHECK=1
echo "OpenFileSystem" > "$dir/x.txt"
fail=0
case_() {   # SECTOR SECTORS [32]; expected FAT type in $want
    img=$dir/t.img
    rm -f "$img"
    SECTOR=$1 "$fmt" "$2" "$img" ${3:-} > /dev/null || { echo "FAILED: layout $1 x $2 ${3:-}"; fail=1; return; }
    if ! file -b "$img" | grep -q "FAT ($want bit)"; then
        echo "FAILED: $1 x $2: not FAT$want: $(file -b "$img")"; fail=1
    fi
    if ! mcopy -i "$img" "$dir/x.txt" ::X.TXT || ! mtype -i "$img" ::X.TXT | grep -q OpenFileSystem; then
        echo "FAILED: mtools on $1 x $2"; fail=1
    fi
    if ! fsck.fat -n "$img" > "$dir/fsck.out" 2>&1; then
        echo "FAILED: fsck.fat on $1 x $2"; cat "$dir/fsck.out"; fail=1
    fi
}
want=12;     case_ 512 2880
want=16;     case_ 512 200000
want=32;     case_ 512 600000 32
want=12;     case_ 4096 1000
want=16;     case_ 4096 4096
want=16;     case_ 4096 500000
want=32;     case_ 4096 70000 32
want=32;     case_ 4096 1048576 32
test "$fail" = 0 && echo "FAT format host tests passed"

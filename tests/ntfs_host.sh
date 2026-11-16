#!/bin/sh
# The NTFS reader on images of tests/ntfs_image.sh (clusters of 4 KB and
# 512 bytes): every file read through 8.3 names equals the original, the
# listing has every name, compressed files are refused.
set -eu
cd "$(dirname "$0")/.."
T=build/host/test_ntfs
for c in 4096 512; do
    d=build/ntfs-$c
    sh tests/ntfs_image.sh "$d" 64 "$c" >/dev/null
    "$T" "$d/ntfs.img" > "$d/list.txt" 2>/dev/null
    check() {   # 8.3 path, file under files/
        "$T" "$d/ntfs.img" 0 "$1" > "$d/out.bin" 2>/dev/null || { echo "$c: cannot read $1"; exit 1; }
        cmp -s "$d/out.bin" "$d/files/$2" || { echo "$c: $1 differs"; exit 1; }
    }
    check README.TXT README.TXT
    check KLEIN.TXT klein.txt
    check ZUFALL.BIN zufall.bin
    check GROSS.BIN gross.bin
    check FRAG.BIN frag.bin
    check SPARSE.DAT sparse.dat
    check VIELE.BIN viele.bin
    check LANGDOS.TXT 'Langer Dateiname mit DOS.txt'
    check 'SUB\A.B' SUB/A.B
    check 'UNTERO~1\TIEFER\DATEI.DAT' 'Unterordner mit langem Namen/Tiefer/Datei.dat'
    check 'VIELE\DATEI150.TXT' VIELE/DATEI150.TXT
    check 'EINLA~12.TXT' 'Ein langer Name 9.txt'
    check ARCHIV~1.GZ archiv.tar.gz
    test "$(grep -c '  DATEI[0-9]*\.TXT ' "$d/list.txt")" = 150 || { echo "$c: VIELE incomplete"; exit 1; }
    grep -q '  ZAHLEN.TXT ' "$d/list.txt"
    if "$T" "$d/ntfs.img" 0 'KOMPR\ZAHLEN.TXT' >/dev/null 2>&1; then echo "$c: compressed file read"; exit 1; fi
    echo "NTFS reading with $c-byte clusters passed"
done

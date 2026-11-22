#!/bin/sh
# exFAT recovery (build/host/test_recover) on the image of
# tests/exdefrag_image.sh (mkfs.exfat and Linux's driver: fragmented files
# with FAT chains, contiguous NoFatChain files, a fragmented directory with
# subdirectories, an empty file, a file whose valid data length is below
# its size), with 4K and 512-byte clusters. Each copy is damaged in turn:
#   intact   everything back, byte for byte
#   fat      the FAT wiped: the contiguous files back, the fragmented ones
#            guessed (counted; sparse.dat and last.bin fill the gaps of
#            deleted files, so no guess gets them right)
#   root     the root directory wiped: "Folder with files" found by the scan
#            under \LOST, its files byte for byte
#   bad      sectors of the cluster heap unreadable: every file, of its size,
#            the bad sectors counted
#   chain    last.bin's entry set deleted: its clusters, still marked in the
#            bitmap, back as \LOST\FILE00001.CHK
#   boot     the main boot sector wiped: the backup boot region taken
# Then the 4K image as TexFAT (tests/exfat_texfat.py, either FAT active):
#   intact, and fat1 (the active FAT wiped): everything back from the other
# The image itself is never written (its checksum is compared).
#   tests/exrecover_host.sh [DIR]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$ROOT/build/exrecover-host}
rec=$ROOT/build/host/test_recover
make -C "$ROOT" -s build/host/test_recover
for t in mkfs.exfat fsck.exfat udisksctl python3; do command -v $t >/dev/null || { echo "SKIP: $t"; exit 0; }; done
rm -rf "$dir"; mkdir -p "$dir"
fail=0

geo='
import struct, sys
def geo(f):
    f.seek(0); b = f.read(512)
    fat, fatlen, heap, count, root = struct.unpack_from("<IIIII", b, 80)
    bps = 1 << b[108]; spc = 1 << b[109]
    return dict(bps=bps, spc=spc, fat=fat, fatlen=fatlen, heap=heap, count=count, root=root)
def chain(f, g, c):
    out = []
    while 2 <= c < g["count"] + 2 and c not in out and len(out) < 1 << 16:
        out.append(c)
        f.seek(g["fat"] * g["bps"] + 4 * c); c = struct.unpack("<I", f.read(4))[0]
    return out
'
damage() {   # IMAGE KIND
	python3 -I - "$1" "$2" <<PY
$geo
f = open(sys.argv[1], "r+b"); g = geo(f); kind = sys.argv[2]; bps = g["bps"]
def wipe(sec, n):
    f.seek(sec * bps); f.write(bytes(n * bps))
def clu(c): return g["heap"] + (c - 2) * g["spc"]
if kind == "fat1":
    act = struct.unpack_from("<H", (f.seek(0), f.read(512))[1], 106)[0] & 1
    f.seek((g["fat"] + act * g["fatlen"]) * bps + 8); f.write(bytes(g["fatlen"] * bps - 8))
elif kind == "fat":
    f.seek(g["fat"] * bps + 8); f.write(bytes(g["fatlen"] * bps - 8))
elif kind == "root":
    for c in chain(f, g, g["root"]): wipe(clu(c), g["spc"])
elif kind == "boot":
    wipe(0, 1)
elif kind == "chain":
    # last.bin's entry set: the in-use bits cleared, the bitmap left (the
    # root's clusters as one, as a set may span two)
    cl = chain(f, g, g["root"]); n = g["spc"] * bps; d = bytearray()
    for c in cl: f.seek(clu(c) * bps); d += f.read(n)
    for i in range(0, len(d) - 95, 32):
        if d[i] == 0x85 and d[i + 32] == 0xC0 and d[i + 64] == 0xC1 and d[i + 66:i + 82].decode("utf-16-le", "replace").startswith("last.bin"):
            for k in range(d[i + 1] + 1): d[i + 32 * k] &= 0x7F
    for j, c in enumerate(cl): f.seek(clu(c) * bps); f.write(d[j * n:(j + 1) * n])
PY
}
heap_sectors() {
	python3 -I -c "$geo
g = geo(open(sys.argv[1], 'rb')); s = g['heap'] + 40 * g['spc']
print('%d-%d' % (s, s + 3))" "$1"
}

for cl in 4K 512; do
	sh "$ROOT/tests/exdefrag_image.sh" "$dir/$cl" 32 $cl >/dev/null
	img=$dir/$cl/exfat.img files=$dir/$cl/files
	for d in intact fat root bad chain boot; do
		name="exFAT $cl $d"
		cp "$img" "$dir/run.img"
		bad=""
		case $d in intact | bad) ;; *) damage "$dir/run.img" $d ;; esac
		[ $d = bad ] && bad=$(heap_sectors "$dir/run.img")
		sum=$(sha1sum <"$dir/run.img")
		out=$dir/out; rm -rf "$out"; mkdir "$out"
		"$rec" "$dir/run.img" "$out" SC $bad >"$dir/rec.txt" || { echo "FAILED: $name"; tail -5 "$dir/rec.txt"; fail=1; continue; }
		[ "$(sha1sum <"$dir/run.img")" = "$sum" ] || { echo "FAILED: $name wrote to the image"; fail=1; }
		res=$(tail -1 "$dir/rec.txt")
		find "$out" -type f -exec sha1sum {} + | cut -d' ' -f1 >"$dir/all.sums"
		has() { grep -q "$(sha1sum <"$files/$1" | cut -d' ' -f1)" "$dir/all.sums"; }
		case $d in
		intact | boot)
			diff -r "$files" "$out" >/dev/null || { echo "FAILED: $name"; diff -r "$files" "$out" | head -5; fail=1; }
			echo "$res" | grep -q 'guessed 0, bad sectors 0, lost directories 0, lost chains 0, failed 0' ||
				{ echo "FAILED: $name: $res"; fail=1; } ;;
		fat)
			for f in EMPTY.TXT "Folder with files/Sub 10/data.bin" "Folder with files/Sub 10/deep.txt" "Folder with files/A long file name number 7.txt"; do
				has "$f" || { echo "FAILED: $name: $f"; fail=1; }
			done
			echo "$res" | grep -q 'guessed [1-9]' || { echo "FAILED: $name: nothing guessed: $res"; fail=1; } ;;
		root)
			for f in "Folder with files/A long file name number 7.txt" "Folder with files/late.bin" \
				"Folder with files/Sub 10/data.bin" "Folder with files/Sub 50/deep.txt"; do
				has "$f" || { echo "FAILED: $name: $f not found"; fail=1; }
			done
			ls "$out"/LOST/DIR*/"Sub 50/data.bin" >/dev/null 2>&1 || { echo "FAILED: $name: the tree below the lost directory"; ls -R "$out/LOST" | head; fail=1; }
			echo "$res" | grep -q 'lost directories [1-9]' || { echo "FAILED: $name: $res"; fail=1; } ;;
		bad)
			n=$(cd "$files" && find . -type f | wc -l); m=$(cd "$out" && find . -type f | wc -l)
			[ "$n" = "$m" ] || { echo "FAILED: $name: $m of $n files"; fail=1; }
			(cd "$files" && find . -type f) | while read -r f; do
				[ "$(stat -c %s "$files/$f")" = "$(stat -c %s "$out/$f")" ] || { echo "FAILED: $name: $f size"; exit 1; }
			done || fail=1
			echo "$res" | grep -q 'bad sectors [1-9]' || { echo "FAILED: $name: $res"; fail=1; } ;;
		chain)
			[ ! -e "$out/last.bin" ] || { echo "FAILED: $name: last.bin still there"; fail=1; }
			[ -f "$out/LOST/FILE00001.CHK" ] && cmp -s -n 40000 "$files/last.bin" "$out/LOST/FILE00001.CHK" ||
				{ echo "FAILED: $name: the lost clusters"; ls "$out/LOST" 2>&1; fail=1; }
			echo "$res" | grep -q 'lost chains 1,' || { echo "FAILED: $name: $res"; fail=1; } ;;
		esac
		echo "$name: $res"
	done
done
for act in 0 1; do
	cp "$dir/4K/exfat.img" "$dir/texfat.img"
	python3 -I "$ROOT/tests/exfat_texfat.py" "$dir/texfat.img" $act
	for d in intact fat1; do
		name="TexFAT (FAT $act active) $d"
		cp "$dir/texfat.img" "$dir/run.img"
		[ $d = intact ] || damage "$dir/run.img" $d
		out=$dir/out; rm -rf "$out"; mkdir "$out"
		"$rec" "$dir/run.img" "$out" SC >"$dir/rec.txt" || { echo "FAILED: $name"; tail -5 "$dir/rec.txt"; fail=1; continue; }
		res=$(tail -1 "$dir/rec.txt")
		diff -r "$dir/4K/files" "$out" >/dev/null || { echo "FAILED: $name"; diff -r "$dir/4K/files" "$out" | head -5; fail=1; }
		echo "$res" | grep -q '^exFAT (TexFAT): .*guessed 0, bad sectors 0, lost directories 0, lost chains 0, failed 0' ||
			{ echo "FAILED: $name: $res"; fail=1; }
		echo "$name: $res"
	done
done
test "$fail" = 0 && echo "exFAT recovery host tests passed"

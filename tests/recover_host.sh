#!/bin/sh
# FAT recovery (build/host/test_recover) on FAT12, FAT16 and FAT32 images
# made by mtools alone: long and short names, nested directories, a
# directory of several clusters, fragmented files (FRAG*.BIN, written into
# the gaps of deleted ones), an empty file. Each image is damaged in turn:
#   intact        everything back, byte for byte
#   fat1          the first FAT wiped: everything back from the second
#   fats          both FATs wiped: every file that was contiguous back (where
#                 its directory's chain is lost too, under \LOST), the
#                 fragmented ones guessed (counted)
#   root          the root directory wiped: the subdirectories found by the
#                 scan under \LOST, their files byte for byte
#   bad           sectors of the data area unreadable: every file there, of
#                 its size, the bad sectors counted
#   chain         a file's directory entry deleted, its chain left: the
#                 chain back as \LOST\FILE00001.CHK, the file at its start
#   boot (FAT32)  the boot sector wiped: the backup in sector 6 is taken
#   deleted       BIG.BIN, a file with a long name and the directory SUB
#                 deleted by mtools, recovered with D: all three under
#                 \DELETED byte for byte (an 8.3 name without a long one
#                 with _ for its lost first character), the rest as it was
#   noboot        the boot sector (FAT32: and its backup) wiped: the geometry
#                 searched for (the FATs, the clusters by the "." entries),
#                 everything back
# The image itself is never written (its checksum is compared).
#   tests/recover_host.sh [DIR]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$ROOT/build/recover-host}
rec=$ROOT/build/host/test_recover
make -C "$ROOT" -s build/host/test_recover
for t in mkfs.fat mcopy mmd mdel mdeltree python3; do command -v $t >/dev/null || { echo "SKIP: $t"; exit 0; }; done
export MTOOLS_SKIP_CHECK=1
rm -rf "$dir"; mkdir -p "$dir"
fail=0
mk() { python3 -I -c 'import sys, random; r = random.Random(sys.argv[2]); sys.stdout.buffer.write(bytes(r.getrandbits(8) for _ in range(int(sys.argv[1]))))' "$1" "$2"; }

# python helpers on an image: the layout, wiping
geo='
import struct, sys
def geo(f):
    f.seek(0); b = f.read(512)
    bps, spc, res, nf = struct.unpack_from("<HBHB", b, 11)
    rootent, tot16, _, fat16 = struct.unpack_from("<HHBH", b, 17)
    tot = tot16 or struct.unpack_from("<I", b, 32)[0]
    fatsz = fat16 or struct.unpack_from("<I", b, 36)[0]
    rootsec = res + nf * fatsz
    heap = rootsec + (rootent * 32 + bps - 1) // bps
    rootclu = struct.unpack_from("<I", b, 44)[0] if not fat16 else 0
    return dict(bps=bps, spc=spc, res=res, nf=nf, fatsz=fatsz, rootsec=rootsec, heap=heap, rootclu=rootclu, tot=tot)
'
damage() {   # IMAGE KIND
	python3 -I - "$1" "$2" <<PY
$geo
f = open(sys.argv[1], "r+b"); g = geo(f); kind = sys.argv[2]; bps = g["bps"]
def wipe(sec, n):
    f.seek(sec * bps); f.write(bytes(n * bps))
if kind == "fat1":
    wipe(g["res"], g["fatsz"])
elif kind == "fats":
    wipe(g["res"], g["nf"] * g["fatsz"])
elif kind == "root":
    if g["rootclu"]:
        wipe(g["heap"] + (g["rootclu"] - 2) * g["spc"], g["spc"])
    else:
        wipe(g["rootsec"], g["heap"] - g["rootsec"])
elif kind == "boot":
    wipe(0, 1)
elif kind == "noboot":
    wipe(0, 1)
    if g["rootclu"]: wipe(6, 1)
elif kind == "chain":
    # BIG.BIN's entry in the root deleted (0xE5), its FAT chain kept
    sec = g["rootsec"] if not g["rootclu"] else g["heap"] + (g["rootclu"] - 2) * g["spc"]
    f.seek(sec * bps); d = bytearray(f.read(g["spc"] * bps))
    for i in range(0, len(d), 32):
        if d[i:i+11] == b"BIG     BIN":
            d[i] = 0xE5
            if i >= 32 and d[i-32+11] == 0x0F: d[i-32] = 0xE5
    f.seek(sec * bps); f.write(d)
PY
}
data_sectors() {   # IMAGE: a range in the middle of the data area
	python3 -I -c "$geo
g = geo(open(sys.argv[1], 'rb')); m = (g['heap'] + g['tot']) // 2
print('%d-%d' % (g['heap'] + 4, g['heap'] + 4 + 2 * g['spc']))" "$1"
}

for kind in fat12 fat16 fat32; do
	case $kind in fat12) size=1440K opt="-F 12" ;; fat16) size=16M opt="-F 16" ;; fat32) size=40M opt="-F 32" ;; esac
	img=$dir/$kind.img files=$dir/$kind.files
	mkdir -p "$files/Folder with a long name/Sub dir" "$files/SUB" "$files/Many"
	truncate -s $size "$img"
	mkfs.fat $opt -s 1 -n RECOVER "$img" >/dev/null
	i=0
	while [ $i -lt 30 ]; do mk 1100 "gap$i" >"$dir/GAP$i.BIN"; mcopy -i "$img" "$dir/GAP$i.BIN" "::GAP$i.BIN"; i=$((i + 1)); done
	i=0
	while [ $i -lt 30 ]; do mdel -i "$img" "::GAP$i.BIN"; i=$((i + 2)); done
	i=1
	while [ $i -lt 30 ]; do mv "$dir/GAP$i.BIN" "$files/"; i=$((i + 2)); done
	rm -f "$dir"/GAP*.BIN
	mk 9000 frag1 >"$files/FRAG1.BIN"; mk 7000 frag2 >"$files/FRAG2.BIN"
	mcopy -i "$img" "$files/FRAG1.BIN" ::FRAG1.BIN; mcopy -i "$img" "$files/FRAG2.BIN" ::FRAG2.BIN
	mk 40000 big >"$files/BIG.BIN"; mcopy -i "$img" "$files/BIG.BIN" ::BIG.BIN
	printf 'hallo\r\n' >"$files/README.TXT"; mcopy -i "$img" "$files/README.TXT" ::README.TXT
	: >"$files/EMPTY.TXT"; mcopy -i "$img" "$files/EMPTY.TXT" ::EMPTY.TXT
	mmd -i "$img" "::Folder with a long name" "::Folder with a long name/Sub dir" ::SUB ::Many
	mk 5000 deep >"$files/Folder with a long name/Sub dir/Deep file.dat"
	mk 3000 long >"$files/Folder with a long name/A long file name.txt"
	mk 2500 sub >"$files/SUB/DATA.BIN"
	mcopy -i "$img" "$files/Folder with a long name/Sub dir/Deep file.dat" "::Folder with a long name/Sub dir/Deep file.dat"
	mcopy -i "$img" "$files/Folder with a long name/A long file name.txt" "::Folder with a long name/A long file name.txt"
	mcopy -i "$img" "$files/SUB/DATA.BIN" ::SUB/DATA.BIN
	i=0
	while [ $i -lt 40 ]; do
		printf 'n %s\r\n' $i >"$files/Many/Entry with a long name $i.txt"
		mcopy -i "$img" "$files/Many/Entry with a long name $i.txt" "::Many/Entry with a long name $i.txt"
		i=$((i + 1))
	done
	fsck.fat -n "$img" >/dev/null 2>&1 || { echo "FAILED: $kind image not clean"; fail=1; }
	kinds="intact fat1 fats root bad chain noboot deleted"
	[ $kind = fat32 ] && kinds="$kinds boot"
	for d in $kinds; do
		name="$kind $d"
		cp "$img" "$dir/run.img"
		bad=""
		flags=SC
		case $d in
		intact | bad) ;;
		deleted)
			flags=SCD
			mdel -i "$dir/run.img" ::BIG.BIN "::Folder with a long name/A long file name.txt"
			mdeltree -i "$dir/run.img" ::SUB ;;
		*) damage "$dir/run.img" $d ;;
		esac
		[ $d = bad ] && bad=$(data_sectors "$dir/run.img")
		sum=$(sha1sum <"$dir/run.img")
		out=$dir/out; rm -rf "$out"; mkdir "$out"
		"$rec" "$dir/run.img" "$out" $flags $bad >"$dir/rec.txt" || { echo "FAILED: $name"; cat "$dir/rec.txt"; fail=1; continue; }
		[ "$(sha1sum <"$dir/run.img")" = "$sum" ] || { echo "FAILED: $name wrote to the image"; fail=1; }
		res=$(tail -1 "$dir/rec.txt")
		case $d in
		intact | fat1 | boot | noboot)
			diff -r "$files" "$out" >/dev/null || { echo "FAILED: $name"; diff -r "$files" "$out" | head -5; cat "$dir/rec.txt" | tail -3; fail=1; }
			echo "$res" | grep -q 'guessed 0, bad sectors 0, lost directories 0, lost chains 0, failed 0' ||
				{ echo "FAILED: $name: $res"; fail=1; } ;;
		fats)
			# at its place, or under LOST when its directory's chain was lost too
			find "$out" -type f -exec sha1sum {} + | cut -d' ' -f1 >"$dir/all.sums"
			for f in README.TXT EMPTY.TXT "Folder with a long name/A long file name.txt" "SUB/DATA.BIN"; do
				cmp -s "$files/$f" "$out/$f" || grep -q "$(sha1sum <"$files/$f" | cut -d' ' -f1)" "$dir/all.sums" ||
					{ echo "FAILED: $name: $f"; fail=1; }
			done
			echo "$res" | grep -q 'guessed [1-9]' || { echo "FAILED: $name: nothing guessed: $res"; fail=1; } ;;
		root)
			# the subdirectories under LOST, each file somewhere there
			find "$out/LOST" -type f -exec sha1sum {} + 2>/dev/null | cut -d' ' -f1 >"$dir/lost.sums"
			for f in "Folder with a long name/Sub dir/Deep file.dat" "Folder with a long name/A long file name.txt" "SUB/DATA.BIN"; do
				grep -q "$(sha1sum <"$files/$f" | cut -d' ' -f1)" "$dir/lost.sums" || { echo "FAILED: $name: $f not found under LOST"; fail=1; }
			done
			[ -f "$out/LOST/DIR00001/Sub dir/Deep file.dat" ] || [ -f "$out/LOST/DIR00002/Sub dir/Deep file.dat" ] ||
				[ -f "$out/LOST/DIR00003/Sub dir/Deep file.dat" ] || { echo "FAILED: $name: the tree below a lost directory"; ls -R "$out/LOST" | head; fail=1; }
			echo "$res" | grep -q 'lost directories [1-9]' || { echo "FAILED: $name: $res"; fail=1; } ;;
		bad)
			n=$(cd "$files" && find . -type f | wc -l); m=$(cd "$out" && find . -type f | wc -l)
			[ "$n" = "$m" ] || { echo "FAILED: $name: $m of $n files"; fail=1; }
			(cd "$files" && find . -type f) | while read -r f; do
				[ "$(stat -c %s "$files/$f")" = "$(stat -c %s "$out/$f")" ] || { echo "FAILED: $name: $f size"; exit 1; }
			done || fail=1
			echo "$res" | grep -q 'bad sectors [1-9]' || { echo "FAILED: $name: $res"; fail=1; } ;;
		deleted)
			# an 8.3 name without a long one has lost its first character (_)
			for f in BIG.BIN:_IG.BIN "Folder with a long name/A long file name.txt:Folder with a long name/A long file name.txt" \
				SUB/DATA.BIN:_UB/_ATA.BIN; do
				g=${f#*:} f=${f%%:*}
				[ ! -e "$out/$f" ] || { echo "FAILED: $name: $f still in the tree"; fail=1; }
				cmp -s "$files/$f" "$out/DELETED/$g" || { echo "FAILED: $name: DELETED/$g"; ls -R "$out/DELETED" | head -20; fail=1; }
			done
			for f in README.TXT FRAG1.BIN FRAG2.BIN "Folder with a long name/Sub dir/Deep file.dat" "Many/Entry with a long name 7.txt"; do
				cmp -s "$files/$f" "$out/$f" || { echo "FAILED: $name: $f"; fail=1; }
			done
			echo "$res" | grep -q 'failed 0; undeleted [1-9]' || { echo "FAILED: $name: $res"; fail=1; } ;;
		chain)
			[ ! -e "$out/BIG.BIN" ] || { echo "FAILED: $name: BIG.BIN still there"; fail=1; }
			[ -f "$out/LOST/FILE00001.CHK" ] && cmp -s -n 40000 "$files/BIG.BIN" "$out/LOST/FILE00001.CHK" ||
				{ echo "FAILED: $name: the chain"; ls "$out/LOST" 2>&1; fail=1; }
			echo "$res" | grep -q 'lost chains 1,' || { echo "FAILED: $name: $res"; fail=1; } ;;
		esac
		echo "$name: $res"
	done
done
test "$fail" = 0 && echo "FAT recovery host tests passed"

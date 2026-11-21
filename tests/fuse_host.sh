#!/bin/bash
# The cores through ofsfuse (hosts/posix), i.e. through the common
# interface every host uses (src/vol.c), against the Linux tools.
#
# FAT12, FAT16, FAT32, exFAT: a fresh volume (mkfs) mounted with ofsfuse
# gets a tree -- deep and large directories, long and German names, files
# of awkward sizes, renames within and across directories (also replacing
# a file), removals, truncation, appending. Every step is done on a
# reference directory too. Then: diff -r through ofsfuse, fsck finds no
# error, and the Linux driver (udisksctl, no root) reads the same tree.
# The volume is mounted with ofsfuse a second time and compared again.
#
# NTFS: tests/ntfs_image.sh's volume (ntfs-3g made it: compressed and
# sparse files, an attribute list ...) read through ofsfuse, read-only,
# compared with its files.
#
# ISO 9660 (xorriso): the same tree with Rock Ridge, with Joliet only
# (also long Joliet names), plain (8.3 names: contents compared), each
# read with 512- and 2048-byte sectors (an image, a CD drive), deep
# directories, and with ISO_BIG=1 a 4.5 GiB file of several extents.
#
# KEEP=dir keeps the images and trees of a failure.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
ofsfuse=$here/build/host/ofsfuse
unitab=${UNITAB:-$here/../mars-unicode-tables/TAB/unitables}
work=$(mktemp -d)
mnt=$work/mnt
mkdir -p "$mnt"
loop=
cleanup() {
	fusermount3 -u "$mnt" 2>/dev/null || true
	if [ -n "$loop" ]; then
		udisksctl unmount --no-user-interaction -b "$loop" >/dev/null 2>&1 || true
		udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null 2>&1 || true
	fi
	if [ -n "${KEEP:-}" ] && [ -n "${failed:-}" ]; then
		mkdir -p "$KEEP"; cp -a "$work/." "$KEEP/"; echo "fuse_host: kept in $KEEP" >&2
	fi
	rm -rf "$work"
}
trap cleanup EXIT
fail() { failed=1; echo "FAIL: $*" >&2; exit 1; }

mount_ofs() {
	"$ofsfuse" -o unitab="$unitab" "$@" "$mnt" 2>"$work/ofsfuse.log" || { cat "$work/ofsfuse.log" >&2; fail "ofsfuse $*"; }
}
umount_ofs() { fusermount3 -u "$mnt"; }

# the same operation in the reference ($ref) and through ofsfuse ($mnt)
both() { (cd "$ref" && eval "$1") && (cd "$mnt" && eval "$1") || fail "$1"; }

tree() {
	both 'mkdir -p "Ordner mit langem Namen/Tiefer/Noch tiefer" SUB VIELE leer'
	both 'printf "hallo\r\n" > README.TXT'
	both 'printf "klein\r\n" > klein.txt'
	both 'printf "Umlaute\r\n" > "Ärger über Größe.txt"'
	for i in $(seq 1 12); do both "printf 'Datei $i\r\n' > 'Ein langer Name $i.txt'"; done
	for i in $(seq 1 150); do both "printf 'Nummer $i\r\n' > VIELE/DATEI$i.TXT"; done
	for n in 0 1 511 512 513 4095 4096 4097 65535 65536 65537 1000003; do
		head -c $n /dev/urandom > "$work/r$n"
		both "cp '$work/r$n' 'SUB/größe $n.bin'"
	done
	head -c 3000000 /dev/urandom > "$work/big"
	both "cp '$work/big' 'Ordner mit langem Namen/Tiefer/Noch tiefer/groß.bin'"
	# renames: in place, across directories, a directory, replacing a file
	both 'mv klein.txt KLEINER.TXT'
	both 'mv "SUB/größe 4096.bin" "Ordner mit langem Namen/verschoben.bin"'
	both 'mv "Ordner mit langem Namen/Tiefer" SUB/Tiefer2'
	both 'mv "Ein langer Name 1.txt" "Ein langer Name 2.txt"'
	# removals: files of a large directory, an empty directory
	for i in $(seq 1 150 | awk 'NR % 3 == 0'); do both "rm VIELE/DATEI$i.TXT"; done
	both 'rmdir leer'
	# truncation and appending
	both 'truncate -s 100 "SUB/größe 65537.bin"'
	both 'truncate -s 70000 "SUB/größe 513.bin"'
	both 'printf "angehängt\r\n" >> README.TXT'
	both 'cp README.TXT "SUB/Kopie von README.TXT"'
	# a name that exists already, in another case: the volume refuses or
	# replaces as the file system decides -- skipped (FAT is case-blind)
}

compare() { diff -r "$ref" "$1" > "$work/diff.txt" 2>&1 || { head -20 "$work/diff.txt" >&2; fail "$2"; }; }

linux_view() {
	command -v udisksctl >/dev/null || { echo "  (no udisksctl: Linux driver comparison skipped)"; return 0; }
	loop=$(udisksctl loop-setup --no-user-interaction -r -f "$1" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
	udisksctl mount --no-user-interaction -b "$loop" -o ro >/dev/null || fail "$2: udisksctl mount"
	lm=$(lsblk -no MOUNTPOINT "$loop")
	compare "$lm" "$2: the Linux driver reads another tree"
	udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
	udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
	loop=
}

for kind in fat12 fat16 fat32 exfat; do
	img=$work/$kind.img ref=$work/ref-$kind
	rm -rf "$ref"; mkdir -p "$ref"
	case $kind in
	fat12) truncate -s 16M "$img"; mkfs.fat -F 12 -n OFSFUSE "$img" >/dev/null ;;
	fat16) truncate -s 64M "$img"; mkfs.fat -F 16 -n OFSFUSE "$img" >/dev/null ;;
	fat32) truncate -s 128M "$img"; mkfs.fat -F 32 -n OFSFUSE "$img" >/dev/null ;;
	exfat) truncate -s 128M "$img"; mkfs.exfat -q -L OFSFUSE "$img" ;;
	esac
	mount_ofs "$img"
	tree
	compare "$mnt" "$kind: the tree through ofsfuse"
	umount_ofs
	case $kind in
	exfat) fsck.exfat -n "$img" > "$work/fsck.txt" 2>&1 || { cat "$work/fsck.txt" >&2; fail "$kind: fsck.exfat"; } ;;
	*) fsck.fat -n "$img" > "$work/fsck.txt" 2>&1 || { cat "$work/fsck.txt" >&2; fail "$kind: fsck.fat"; } ;;
	esac
	mount_ofs -o ro "$img"
	compare "$mnt" "$kind: the tree mounted again"
	umount_ofs
	linux_view "$img" "$kind"
	echo "$kind passed"
done

# NTFS, read-only: the volume of tests/ntfs_image.sh (it needs udisksctl
# and ntfs-3g)
if command -v mkntfs >/dev/null && command -v udisksctl >/dev/null && command -v ntfs-3g >/dev/null; then
	sh "$here/tests/ntfs_image.sh" "$work/ntfs" 64 4096 >/dev/null
	mount_ofs "$work/ntfs/ntfs.img"
	ref=$work/ntfs/files
	compare "$mnt" "ntfs: the tree through ofsfuse"
	if touch "$mnt/neu.txt" 2>/dev/null; then fail "ntfs: writable"; fi
	umount_ofs
	echo "ntfs passed (read-only)"
else
	echo "ntfs skipped (no mkntfs, udisksctl or ntfs-3g)"
fi
# ISO 9660
if command -v xorriso >/dev/null; then
	src=$work/isosrc
	mkdir -p "$src/a/b/c/d/e/f/g/h/i/j" "$src/Ordner mit langem Namen"
	printf 'hallo\r\n' > "$src/README.TXT"
	printf 'tief\r\n' > "$src/a/b/c/d/e/f/g/h/i/j/tief.txt"
	printf 'Umlaute\r\n' > "$src/Ordner mit langem Namen/Größere Datei mit langem Namen.txt"
	for n in 0 1 2047 2048 2049 65535 65536 1000003; do head -c $n /dev/urandom > "$src/r$n.bin"; done
	for mode in "-R -J" "--norock -J" "--norock -J -joliet-long" "--norock"; do
		rm -f "$work/t.iso"
		xorriso -as mkisofs -quiet $mode -V "OFS ISO" -o "$work/t.iso" "$src" 2>/dev/null || fail "xorriso $mode"
		for sec in 512 2048; do
			mount_ofs -o sector=$sec "$work/t.iso"
			grep -q 'ISO9660 "OFS ISO", read-only' "$work/ofsfuse.log" || { cat "$work/ofsfuse.log" >&2; fail "iso [$mode]: mounted as"; }
			if [ "$mode" = "--norock" ]; then
				# 8.3 names in capitals: the contents
				cmp "$src/r1000003.bin" "$mnt/R1000003.BIN" || fail "iso [$mode] $sec: content"
				[ -f "$mnt/A/B/C/D/E/F/G/H/I/J/TIEF.TXT" ] || fail "iso [$mode] $sec: deep directory"
			else
				ref=$src compare "$mnt" "iso [$mode] sector=$sec"
			fi
			if touch "$mnt/neu.txt" 2>/dev/null; then fail "iso: writable"; fi
			umount_ofs
		done
	done
	if [ -n "${ISO_BIG:-}" ]; then
		big=$work/bigsrc
		mkdir -p "$big"
		truncate -s 4500M "$big/gross.bin"
		printf 'ANFANG' | dd of="$big/gross.bin" conv=notrunc 2>/dev/null
		printf 'MITTE-4G' | dd of="$big/gross.bin" bs=1 seek=4294967290 conv=notrunc 2>/dev/null
		printf 'ENDE' | dd of="$big/gross.bin" bs=1 seek=$((4500 * 1048576 - 4)) conv=notrunc 2>/dev/null
		echo klein > "$big/danach.txt"
		xorriso -as mkisofs -quiet -iso-level 3 -R -J -o "$work/big.iso" "$big" 2>/dev/null || fail "xorriso big"
		rm -f "$work/t.iso"
		mount_ofs "$work/big.iso"
		ref=$big compare "$mnt" "iso: a file of several extents"
		umount_ofs
		rm -f "$work/big.iso"
		echo "iso9660 big file passed"
	fi
	echo "iso9660 passed (read-only)"
else
	echo "iso9660 skipped (no xorriso)"
fi
echo "ofsfuse tests passed"

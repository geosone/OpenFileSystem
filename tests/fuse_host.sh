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
# sparse files, an attribute list ...) read through ofsfuse and compared
# with its files (writing NTFS: tests/ntfs_write.sh).
#
# ISO 9660 (xorriso): the same tree with Rock Ridge, with Joliet only
# (also long Joliet names), plain (8.3 names: contents compared), each
# read with 512- and 2048-byte sectors (an image, a CD drive), deep
# directories, and with ISO_BIG=1 a 4.5 GiB file of several extents.
#
# UDF: cdrtools' mkisofs -udf (a UDF 1.02 bridge, read as UDF), and with
# mkudffs (udftools) volumes of UDF 2.01, 2.50 and 2.60 (metadata
# partition) in 512- and 2048-byte blocks, filled through the Linux UDF
# driver (udisksctl); each read with 512- and 2048-byte sectors where the
# block size allows.
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
# FUSE releases closed files asynchronously: right after a write the mount
# can still be busy for a moment
umount_ofs() {
	for _ in 1 2 3 4 5 6 7 8 9 10; do fusermount3 -u "$mnt" 2>/dev/null && return 0; sleep 0.3; done
	fusermount3 -u "$mnt"
}

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
	# times and the read-only attribute: utimens and chmod
	both "touch -m -d '2001-02-03 04:05:06' README.TXT"
	both "touch -m -d '1999-12-31 23:59:58' 'SUB/größe 0.bin'"
	both 'chmod a-w KLEINER.TXT'
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
	for f in README.TXT "SUB/größe 0.bin"; do
		[ "$(stat -c %Y "$mnt/$f")" = "$(stat -c %Y "$ref/$f")" ] || fail "$kind: the time of $f: $(stat -c %y "$mnt/$f")"
	done
	[ ! -w "$mnt/KLEINER.TXT" ] && [ "$(stat -c %a "$mnt/KLEINER.TXT")" = 444 ] || fail "$kind: KLEINER.TXT not read-only"
	case $kind in fat*)
		command -v mdir >/dev/null && { mdir -i "$img" ::README.TXT | grep -q '2001-02-03  *4:05' ||
			{ mdir -i "$img" ::README.TXT >&2; fail "$kind: mtools reads another time"; }; }
		command -v mattrib >/dev/null && { mattrib -i "$img" ::KLEINER.TXT | cut -c1-12 | grep -q R ||
			{ mattrib -i "$img" ::KLEINER.TXT >&2; fail "$kind: mtools finds KLEINER.TXT writable"; }; }
	esac
	umount_ofs
	linux_view "$img" "$kind"
	echo "$kind passed"
done

# FAT32 and exFAT: the EA files are not shown and cannot be made; removing
# and renaming (also into another directory) take them along; a left-over
# EA file beside an unmarked file is not its EAs; an empty directory with
# only a left-over one can be removed.
sf_tests() {
	python3 -I - "$mnt" <<'PY' || fail "$1: the EA files beside the files"
import errno, os, sys
m = sys.argv[1]
def err(f, *a):
    try:
        f(*a)
    except OSError as e:
        return e.errno
    return 0
names = os.listdir(m)
assert not [n for n in names if n.upper().endswith(' EA. SF')], names
assert not os.path.exists(os.path.join(m, 'EINS.TXT EA. SF'))
assert err(open, os.path.join(m, 'x EA. SF'), 'w') in (errno.EINVAL, errno.ENAMETOOLONG, errno.EPERM, errno.EACCES)
os.mkdir(os.path.join(m, 'ZIEL'))
os.rename(os.path.join(m, 'DREI.TXT'), os.path.join(m, 'ZIEL', 'drei neu.txt'))
assert os.getxattr(os.path.join(m, 'ZIEL', 'drei neu.txt'), 'user.b') == b'B' * 3000
os.rename(os.path.join(m, 'ZIEL', 'drei neu.txt'), os.path.join(m, 'DREI.TXT'))
os.rmdir(os.path.join(m, 'ZIEL'))
os.remove(os.path.join(m, 'Vier.txt'))
# a new file of a removed one's name has none
open(os.path.join(m, 'Vier.txt'), 'w').close()
assert os.listxattr(os.path.join(m, 'Vier.txt')) == []
os.remove(os.path.join(m, 'Vier.txt'))
PY
}

# the image read without OpenFileSystem: the marks (fat_ea_check.py --mark),
# the EA files' FEALISTs (FAT32 through mtools, exFAT through dump.exfat's
# view of the directory), fsck
sf_check() {
	case $1 in
	fat32)
		fsck.fat -n "$2" > "$work/fsck.txt" 2>&1 || { cat "$work/fsck.txt" >&2; fail "$1: fsck.fat"; }
		mdir -a -i "$2" ::/ > "$work/mdir.txt" 2>&1 || { cat "$work/mdir.txt" >&2; fail "$1: mdir"; }
		for f in 'EINS.TXT' 'DREI.TXT'; do
			grep -q "$f EA. SF" "$work/mdir.txt" || { cat "$work/mdir.txt" >&2; fail "$1: no $f EA. SF"; }
		done
		! grep -qi "vier.txt EA. SF\|ORDNER EA. SF" "$work/mdir.txt" || { cat "$work/mdir.txt" >&2; fail "$1: left-over EA files"; }
		rm -f "$work/eins.sf"; mtype -i "$2" "::/EINS.TXT EA. SF" > "$work/eins.sf"
		python3 -I "$here/tests/fat_ea_check.py" --fealist "$work/eins.sf" > "$work/fl.txt" &&
			grep -qx 'comment:1280' "$work/fl.txt" || { cat "$work/fl.txt" >&2; fail "$1: the FEALIST of EINS.TXT"; }
		python3 -I "$here/tests/fat_ea_check.py" --mark "$2" 'EINS    TXT' > "$work/mark.txt" &&
			grep -qx 40 "$work/mark.txt" || { cat "$work/mark.txt" >&2; fail "$1: EINS.TXT's mark"; }
		python3 -I "$here/tests/fat_ea_check.py" --mark "$2" 'ORDNER     ' > "$work/mark.txt" &&
			grep -qx 00 "$work/mark.txt" || { cat "$work/mark.txt" >&2; fail "$1: ORDNER's mark"; } ;;
	exfat)
		fsck.exfat -n "$2" > "$work/fsck.txt" 2>&1 || { cat "$work/fsck.txt" >&2; fail "$1: fsck.exfat"; }
		python3 -I "$here/tests/fat_ea_check.py" --exfat "$2" > "$work/ex.txt" || { cat "$work/ex.txt" >&2; fail "$1: the EA files"; }
		grep -qx 'EINS.TXT 40 comment:1280' "$work/ex.txt" && grep -qx 'DREI.TXT 40 a:1 b:3000' "$work/ex.txt" &&
			[ "$(wc -l < "$work/ex.txt")" -eq 2 ] || { cat "$work/ex.txt" >&2; fail "$1: the EA files"; } ;;
	esac
	mount_ofs -o ro "$2"
	python3 -I - "$mnt" <<'PY' || fail "$1: xattrs mounted again"
import os, sys
m = sys.argv[1]
assert os.getxattr(os.path.join(m, 'EINS.TXT'), 'user.comment') == b'Ein deutlich laengerer Kommentar' * 40
assert os.getxattr(os.path.join(m, 'DREI.TXT'), 'user.b') == b'B' * 3000
assert os.listxattr(os.path.join(m, 'ORDNER')) == []
PY
	umount_ofs
}

# Extended attributes as user.* xattrs: OS/2's and NT's "EA DATA. SF" on
# FAT12/16, FAT32.IFS's "name EA. SF" files on FAT32 and exFAT. The EA files
# are checked against the layout by tests/fat_ea_check.py, the volume by
# fsck.fat / fsck.exfat.
for kind in fat12 fat16 fat32 exfat; do
	img=$work/ea-$kind.img
	case $kind in
	fat12) truncate -s 16M "$img"; mkfs.fat -F 12 -n OFSEA "$img" >/dev/null ;;
	fat16) truncate -s 64M "$img"; mkfs.fat -F 16 -n OFSEA "$img" >/dev/null ;;
	fat32) truncate -s 128M "$img"; mkfs.fat -F 32 -n OFSEA "$img" >/dev/null ;;
	exfat) truncate -s 128M "$img"; mkfs.exfat -q -L OFSEA "$img" ;;
	esac
	mount_ofs "$img"
	printf 'eins\r\n' > "$mnt/EINS.TXT"
	printf 'zwei\r\n' > "$mnt/Zwei mit langem Namen.txt"
	printf 'drei\r\n' > "$mnt/DREI.TXT"
	mkdir "$mnt/ORDNER"
	head -c 40000 /dev/urandom > "$work/eabig"
	python3 -I - "$mnt" "$work/eabig" <<'PY' || fail "$kind: xattrs through ofsfuse"
import errno, os, sys
m, big = sys.argv[1], open(sys.argv[2], 'rb').read()
one, two, three, d = (os.path.join(m, n) for n in ('EINS.TXT', 'Zwei mit langem Namen.txt', 'DREI.TXT', 'ORDNER'))
def err(f, *a):
    try:
        f(*a)
    except OSError as e:
        return e.errno
    return 0
os.setxattr(one, 'user.comment', b'Hallo Welt')
os.setxattr(one, 'user.bin', bytes([0, 255, 16]))
os.setxattr(two, 'user.gross', big)
os.setxattr(three, 'user.a', b'A')
os.setxattr(d, 'user.ordner', b'ja')
assert os.getxattr(one, 'user.comment') == b'Hallo Welt'
assert os.getxattr(one, 'user.COMMENT') == b'Hallo Welt', 'names without case'
assert os.getxattr(one, 'user.bin') == bytes([0, 255, 16])
assert os.getxattr(two, 'user.gross') == big
assert os.getxattr(d, 'user.ordner') == b'ja'
assert sorted(os.listxattr(one)) == ['user.bin', 'user.comment'], os.listxattr(one)
assert err(os.setxattr, one, 'user.comment', b'x', os.XATTR_CREATE) == errno.EEXIST
assert err(os.setxattr, one, 'user.neu', b'x', os.XATTR_REPLACE) == errno.ENODATA
assert err(os.getxattr, one, 'user.fehlt') == errno.ENODATA
assert err(os.setxattr, one, 'trusted.x', b'x') in (errno.EOPNOTSUPP, errno.ENOTSUP, errno.EPERM)
assert err(os.setxattr, two, 'user.zuviel', bytes(30000)) in (errno.ENOSPC, errno.E2BIG), 'over 64 KiB'
# changes: replace (longer), remove one, the set of an earlier file grows
os.setxattr(one, 'user.comment', b'Ein deutlich laengerer Kommentar' * 40, os.XATTR_REPLACE)
os.removexattr(one, 'user.bin')
assert os.listxattr(one) == ['user.comment']
os.setxattr(three, 'user.b', b'B' * 3000)
assert os.getxattr(two, 'user.gross') == big, 'moved sets keep their values'
assert os.getxattr(three, 'user.a') == b'A'
# renaming keeps them; removing the last one drops the set
os.rename(two, os.path.join(m, 'Vier.txt'))
assert os.getxattr(os.path.join(m, 'Vier.txt'), 'user.gross') == big
os.removexattr(d, 'user.ordner')
assert os.listxattr(d) == []
PY
	case $kind in
	fat12|fat16)
		sz1=$(stat -c %s "$mnt/EA DATA. SF")
		rm "$mnt/Vier.txt"
		sz2=$(stat -c %s "$mnt/EA DATA. SF")
		[ "$sz2" -lt "$sz1" ] || fail "$kind: removing a file kept its EAs ($sz1 -> $sz2)" ;;
	*)
		sf_tests "$kind"
		umount_ofs
		sf_check "$kind" "$img"
		echo "$kind EAs passed"
		continue ;;
	esac
	umount_ofs
	fsck.fat -n "$img" > "$work/fsck.txt" 2>&1 || { cat "$work/fsck.txt" >&2; fail "$kind: fsck.fat"; }
	mount_ofs -o ro "$img"
	python3 -I - "$mnt" <<'PY' || fail "$kind: xattrs mounted again"
import os, sys
m = sys.argv[1]
assert os.getxattr(os.path.join(m, 'EINS.TXT'), 'user.comment') == b'Ein deutlich laengerer Kommentar' * 40
assert os.getxattr(os.path.join(m, 'DREI.TXT'), 'user.b') == b'B' * 3000
PY
	bpc=$(( $(od -An -tu2 -j11 -N2 "$img") * $(od -An -tu1 -j13 -N1 "$img") ))
	rm -f "$work/eadata"; cp "$mnt/EA DATA. SF" "$work/eadata"
	umount_ofs
	python3 -I "$here/tests/fat_ea_check.py" "$work/eadata" "$bpc" > "$work/sets.txt" || { cat "$work/sets.txt" >&2; fail "$kind: the EA file's layout"; }
	python3 -I "$here/tests/fat_ea_check.py" --image "$img" > "$work/sets2.txt" && cmp -s "$work/sets.txt" "$work/sets2.txt" ||
		{ cat "$work/sets2.txt" >&2; fail "$kind: the EA file read from the image"; }
	grep -q ' EINS.TXT comment:1280$' "$work/sets.txt" && grep -q ' DREI.TXT a:1 b:3000$' "$work/sets.txt" &&
		[ "$(wc -l < "$work/sets.txt")" -eq 2 ] || { cat "$work/sets.txt" >&2; fail "$kind: the sets"; }
	echo "$kind EAs passed"
done

# NTFS: the volume of tests/ntfs_image.sh (it needs udisksctl and ntfs-3g)
if command -v mkntfs >/dev/null && command -v udisksctl >/dev/null && command -v ntfs-3g >/dev/null; then
	sh "$here/tests/ntfs_image.sh" "$work/ntfs" 64 4096 >/dev/null
	mount_ofs "$work/ntfs/ntfs.img"
	ref=$work/ntfs/files
	compare "$mnt" "ntfs: the tree through ofsfuse"
	umount_ofs
	echo "ntfs passed (read; writing: tests/ntfs_write.sh)"
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
# UDF
udfsrc=$work/udfsrc
mkdir -p "$udfsrc/a/b/c/d/e/f/g/h/i/j" "$udfsrc/viele" "$udfsrc/Ordner mit langem Namen"
printf 'hallo\r\n' > "$udfsrc/README.TXT"
printf 'tief\r\n' > "$udfsrc/a/b/c/d/e/f/g/h/i/j/tief.txt"
printf 'Umlaute\r\n' > "$udfsrc/Ordner mit langem Namen/Größere Datei mit langem Namen.txt"
for n in 0 1 511 512 2047 2048 2049 65536 1000003; do head -c $n /dev/urandom > "$udfsrc/r$n.bin"; done
for i in $(seq 1 300); do printf 'Nummer %d\r\n' $i > "$udfsrc/viele/datei$i.txt"; done
if mkisofs -version 2>/dev/null | grep -q '^mkisofs 3'; then
	rm -f "$work/t.iso"
	mkisofs -quiet -udf -R -J -V OFSUDF -o "$work/t.iso" "$udfsrc" 2>/dev/null || fail "mkisofs -udf"
	for sec in 512 2048; do
		mount_ofs -o sector=$sec "$work/t.iso"
		grep -q 'UDF "OFSUDF", read-only' "$work/ofsfuse.log" || { cat "$work/ofsfuse.log" >&2; fail "udf bridge: mounted as"; }
		ref=$udfsrc compare "$mnt" "udf bridge sector=$sec"
		umount_ofs
	done
	echo "udf 1.02 bridge passed (read-only)"
else
	echo "udf bridge skipped (no cdrtools mkisofs)"
fi
if command -v mkudffs >/dev/null && command -v udisksctl >/dev/null; then
	for rev in 2.01 2.50 2.60; do
		for bs in 512 2048; do
			img=$work/udf.img
			rm -f "$img"; truncate -s 64M "$img"
			# an older udftools makes no revision above 2.01 on a disk
			if ! mkudffs --media-type=hd --udfrev=$rev --blocksize=$bs --label=OFSUDF "$img" >"$work/mkudffs.txt" 2>&1; then
				grep -q "not currently supported" "$work/mkudffs.txt" || { cat "$work/mkudffs.txt" >&2; fail "mkudffs $rev $bs"; }
				echo "udf $rev $bs skipped (this mkudffs cannot make it)"
				continue
			fi
			loop=$(udisksctl loop-setup --no-user-interaction -f "$img" | sed -n 's/.* as \(\/dev\/[a-z0-9]*\)\.$/\1/p')
			udisksctl mount --no-user-interaction -b "$loop" >/dev/null || fail "udf $rev $bs: udisksctl mount"
			lm=$(lsblk -no MOUNTPOINT "$loop")
			cp -r "$udfsrc/." "$lm/" || fail "udf $rev $bs: filling through Linux"
			udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
			udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null
			loop=
			for sec in 512 2048; do
				[ $sec -le $bs ] || continue
				mount_ofs -o sector=$sec "$img"
				grep -q 'UDF "OFSUDF", read-only' "$work/ofsfuse.log" || { cat "$work/ofsfuse.log" >&2; fail "udf $rev $bs: mounted as"; }
				ref=$udfsrc compare "$mnt" "udf $rev blocks=$bs sector=$sec"
				umount_ofs
			done
		done
	done
	echo "udf 2.01/2.50/2.60 passed (read-only)"
else
	echo "udf 2.x skipped (no mkudffs or udisksctl)"
fi
echo "ofsfuse tests passed"

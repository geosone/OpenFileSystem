#!/bin/sh
# An NTFS volume whose $MFT is in many pieces, with its runlist in two
# records and a non-resident attribute list: the volume filled through
# ntfs-3g (its MFT zone too), every other file deleted, then 8000 small
# files, so that the MFT grows into the holes.  The NTFS reader must give
# what ntfs-3g gives for all of them.   (needs ntfs-3g with FUSE)
set -eu
test_ntfs=${TEST_NTFS:-build/host/test_ntfs}
work=$(mktemp -d "${TMPDIR:-/tmp}/ntfsmft.XXXXXX")
mnt=$work/mnt; mkdir "$mnt"
trap 'fusermount -u "$mnt" 2>/dev/null || true; rm -rf "$work"' EXIT
img=$work/v.img
truncate -s 64M "$img"
mkntfs -q -F -Q -c 4096 -L MFTFRAG "$img" >/dev/null 2>&1
ntfs-3g "$img" "$mnt"
mkdir "$mnt/f" "$mnt/many"
i=0
while head -c 16384 /dev/zero > "$mnt/f/$i" 2>/dev/null; do i=$((i + 1)); done
j=0
while test "$j" -le "$i"; do rm -f "$mnt/f/$j"; j=$((j + 2)); done
k=0
while test "$k" -lt 8000 && echo "$k" > "$mnt/many/file$k.txt" 2>/dev/null; do k=$((k + 1)); done
python3 -I "$(dirname "$0")/ntfs_gen.py" "$mnt" --list | LC_ALL=C sort > "$work/want"
fusermount -u "$mnt"
ntfsinfo -i 0 "$img" 2>/dev/null | grep -q ATTRIBUTE_LIST || { echo 'ntfs_mftfrag: $MFT has no attribute list'; exit 1; }
"$test_ntfs" "$img" 0 --dump | LC_ALL=C sort > "$work/got"
if cmp -s "$work/want" "$work/got"; then
    echo "NTFS with a fragmented \$MFT: $(grep -c '^F' "$work/want") files passed"
else
    diff "$work/want" "$work/got" | head -20
    exit 1
fi

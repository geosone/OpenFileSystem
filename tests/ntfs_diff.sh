#!/bin/sh
# Differential test of the NTFS reader: for each seed a fresh volume
# (mkntfs, a cluster size from 512 bytes to 64 KB), filled through ntfs-3g
# by tests/ntfs_gen.py with a random tree; every directory and file read
# through the library (ntfs_test --dump) must give the names, sizes and
# CRC-32 that ntfs-3g gives, with distinct 8.3 names that find their file.
#   tests/ntfs_diff.sh [FIRST_SEED [COUNT]]   (needs ntfs-3g with FUSE)
set -eu
first=${1:-1}; count=${2:-8}
test_ntfs=${TEST_NTFS:-build/host/test_ntfs}
work=$(mktemp -d "${TMPDIR:-/tmp}/ntfsdiff.XXXXXX")
mnt=$work/mnt; mkdir "$mnt"
trap 'fusermount -u "$mnt" 2>/dev/null || true; rm -rf "$work"' EXIT
fail=0
seed=$first
while test "$seed" -lt $((first + count)); do
    set -- 512 1024 2048 4096 8192 16384 32768 65536
    shift $((seed % 8)); cluster=$1
    img=$work/s$seed.img
    truncate -s 400M "$img"
    mkntfs -q -F -Q -c "$cluster" -L "SEED$seed" "$img" >/dev/null 2>&1
    ntfs-3g "$img" "$mnt"
    python3 -I "$(dirname "$0")/ntfs_gen.py" "$mnt" "$seed" "$cluster" | LC_ALL=C sort > "$work/want"
    fusermount -u "$mnt"
    ntfsfix -n "$img" >/dev/null
    if "$test_ntfs" "$img" 0 --dump > "$work/got.raw"; then rc=0; else rc=$?; fi
    LC_ALL=C sort "$work/got.raw" > "$work/got"
    if test "$rc" = 0 && cmp -s "$work/want" "$work/got"; then
        echo "NTFS seed $seed, $cluster-byte clusters: $(grep -c '^F' "$work/want") files passed"
    else
        echo "NTFS seed $seed, $cluster-byte clusters: FAILED (rc=$rc)"
        diff "$work/want" "$work/got" | head -20
        fail=1
        test -z "${KEEP:-}" || cp "$img" "${KEEP}/ntfs-seed$seed.img"
    fi
    rm -f "$img"
    seed=$((seed + 1))
done
exit $fail

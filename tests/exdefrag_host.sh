#!/bin/sh
# exFAT DEFRAG (build/host/test_exdefrag) on exFAT images of
# tests/exdefrag_image.sh (4 KiB and 512-byte clusters) and on TexFAT copies
# (tests/exfat_texfat.py, both ActiveFat values): FULL and FILES must leave
# no fragmented file but the stuck ones, the check (test_exchk) no error,
# fsck.exfat neither (exFAT; it knows no TexFAT), TexFAT's two FATs and
# bitmaps equal, and the tree as before (test_exdefrag T: every file's size
# and CRC-32).  Power loss: FULL stopped after N sectors written, for N
# spread over the whole run; afterwards test_exchk F must make the volume
# clean again and no file may have changed (TexFAT: the move under way is
# rolled back, or lost clusters are left where an entry is written in
# place), and FULL run again must finish the work.
#   tests/exdefrag_host.sh [DIR [STEPS]]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$ROOT/build/exdefrag-host}
steps=${2:-40}
dfg=$ROOT/build/host/test_exdefrag
chk=$ROOT/build/host/test_exchk
make -C "$ROOT" -s build/host/test_exdefrag build/host/test_exchk
mkdir -p "$dir"
fail=0
same() {   # IMAGE WHAT: the tree as before
    "$dfg" "$1" T > "$dir/tree" 2>&1 || true
    cmp -s "$dir/tree" "$base.tree" || { echo "FAILED: $name $2: the tree differs"; diff "$base.tree" "$dir/tree" | head; fail=1; }
}
clean() {   # IMAGE WHAT [CUT]: CUT -- TexFAT's inactive copies may hold the cut transaction
    "$chk" "$1" > "$dir/chk" 2>&1 || { echo "FAILED: $name $2: check"; cat "$dir/chk"; fail=1; }
    grep -q '0 errors, 0 warnings' "$dir/chk" || { echo "FAILED: $name $2: warnings"; cat "$dir/chk"; fail=1; }
    case $name${3:-} in
    *cut) ;;
    tex*)
        python3 -I - "$1" <<'PY' || { echo "FAILED: $name $2: the TexFAT copies differ"; fail=1; }
import struct, sys
f = open(sys.argv[1], 'rb'); b = f.read(512)
ss = 1 << b[108]; cs = ss << b[109]
fat, fatlen, heap, nclu, root = struct.unpack_from('<IIIII', b, 80)
f.seek(fat * ss); a = f.read(fatlen * ss); c = f.read(fatlen * ss)
assert a == c, 'FATs'
f.seek(heap * ss + (root - 2) * cs); r = f.read(cs)
bms = [struct.unpack_from('<II', r, i + 20) for i in range(0, cs, 32) if r[i] == 0x81]
assert len(bms) == 2, 'bitmaps'
def rd(first, n):
    f.seek(heap * ss + (first - 2) * cs); return f.read(n)
assert rd(*bms[0]) == rd(*bms[1]), 'bitmaps'
PY
        ;;
    *)
        fsck.exfat -n "$1" > "$dir/fsck" 2>&1 || { echo "FAILED: $name $2: fsck.exfat"; cat "$dir/fsck"; fail=1; } ;;
    esac
}
sh "$ROOT/tests/exdefrag_image.sh" "$dir/img4k" 32 4K >/dev/null
sh "$ROOT/tests/exdefrag_image.sh" "$dir/img512" 16 512 >/dev/null
for name in exfat-4k exfat-512 texfat texfat-second; do
    base=$dir/$name
    case $name in
    exfat-4k) cp "$dir/img4k/exfat.img" "$base.img" ;;
    exfat-512) cp "$dir/img512/exfat.img" "$base.img" ;;
    texfat) cp "$dir/img4k/exfat.img" "$base.img"; python3 -I "$ROOT/tests/exfat_texfat.py" "$base.img" 0 ;;
    texfat-second) cp "$dir/img4k/exfat.img" "$base.img"; python3 -I "$ROOT/tests/exfat_texfat.py" "$base.img" 1 ;;
    esac
    "$dfg" "$base.img" T > "$base.tree"
    "$dfg" "$base.img" A > "$dir/a.out"
    grep -q 'fragmented [1-9]' "$dir/a.out" || { echo "FAILED: $name image not fragmented"; cat "$dir/a.out"; fail=1; }
    for mode in F U; do
        cp "$base.img" "$dir/run.img"
        "$dfg" "$dir/run.img" "$mode" > "$dir/$mode.out" || { echo "FAILED: $name $mode"; cat "$dir/$mode.out"; fail=1; }
        stuck=$(sed -n 's/.*stuck \([0-9]*\);.*/\1/p' "$dir/$mode.out")
        grep -q "after $stuck, stuck" "$dir/$mode.out" || { echo "FAILED: $name $mode: still fragmented"; cat "$dir/$mode.out"; fail=1; }
        clean "$dir/run.img" "$mode"
        same "$dir/run.img" "$mode"
    done
    total=$(sed -n 's/.*sectors written \([0-9]*\);.*/\1/p' "$dir/F.out")
    n=1
    while test "$n" -lt "$total"; do
        cp "$base.img" "$dir/cut.img"
        rc=0; "$dfg" "$dir/cut.img" F "$n" > "$dir/cut.out" || rc=$?
        test "$rc" = 3 || { echo "FAILED: $name cut at $n: rc $rc"; cat "$dir/cut.out"; fail=1; }
        "$chk" "$dir/cut.img" 0 0 F > "$dir/cut.fix" || true
        clean "$dir/cut.img" "cut at $n" cut
        same "$dir/cut.img" "cut at $n"
        "$dfg" "$dir/cut.img" F > "$dir/again.out" || { echo "FAILED: $name again after the cut at $n"; cat "$dir/again.out"; fail=1; }
        grep -q "after $stuck, stuck" "$dir/again.out" || { echo "FAILED: $name again after $n: still fragmented"; cat "$dir/again.out"; fail=1; }
        clean "$dir/cut.img" "again after the cut at $n"
        same "$dir/cut.img" "again after the cut at $n"
        n=$((n + total / steps + 1))
    done
    echo "$name: $(tail -1 "$dir/F.out")"
done
test "$fail" = 0 && echo "exFAT DEFRAG host tests passed"

#!/usr/bin/env bash
# Check that the C++ English-text builder (patch::build_text, `dcb_patch --text-only`) writes
# exactly what tools/text/en_text.py writes, from the same dumps (game data cannot live in the
# repository, so this is a manual step).
#
#   tools/patch/compare_text.sh --jp <dump> --us <dump> [--fixes DIR]
#                               [--cpp-jp <dump>] [--cpp-us <dump>] [--dcb-patch PATH]
#
#   --jp / --us          dumps made by tools/disc/extract_disc.py (the Python side reads exe/ and,
#                        with fixes, manifest.json + the disc image it names)
#   --cpp-jp / --cpp-us  dumps for the C++ side (default: the same); e.g. native `dcb --import`
#                        output. With fixes the US one needs layout.txt + iso_meta.bin (it rebuilds
#                        the disc image from them).
#   --fixes DIR          community .xdelta fixes: both sides apply every *.xdelta in DIR
#
# Outputs go to a temporary directory that is removed afterwards. Exit status 0 when identical.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
jp= us= cpp_jp= cpp_us= fixes= dcb_patch="$repo/build/linux-debug/dcb_patch"
while [ $# -gt 0 ]; do
    case $1 in
        --jp) jp=$2; shift 2 ;;
        --us) us=$2; shift 2 ;;
        --cpp-jp) cpp_jp=$2; shift 2 ;;
        --cpp-us) cpp_us=$2; shift 2 ;;
        --fixes) fixes=$2; shift 2 ;;
        --dcb-patch) dcb_patch=$2; shift 2 ;;
        *) sed -n '2,17p' "$0" >&2; exit 2 ;;
    esac
done
[ -n "$jp" ] && [ -n "$us" ] || { sed -n '2,17p' "$0" >&2; exit 2; }
cpp_jp=${cpp_jp:-$jp}
cpp_us=${cpp_us:-$us}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/py"

py_us=$us
if [ -n "$fixes" ]; then
    # en_text.py reads the fixes from <out>/fixes/ and the disc image from manifest.json, relative
    # to the repository it runs from; point a copy of the manifest at the image by absolute path
    # (it sits next to the dump's own checkout: <root>/extracted/<serial> -> <root>/<image>).
    mkdir -p "$work/py/fixes" "$work/us"
    cp "$fixes"/*.xdelta "$work/py/fixes/"
    abs_us=$(cd "$us" && pwd)
    for d in "$abs_us"/*; do
        [ "$(basename "$d")" = manifest.json ] || ln -s "$d" "$work/us/$(basename "$d")"
    done
    python3 - "$us" "$repo" "$work/us/manifest.json" <<'EOF'
import json, sys
from pathlib import Path
us, repo, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]), Path(sys.argv[3])
m = json.loads((us / "manifest.json").read_text())
for root in (repo, us.parent.parent):
    if (root / m["image"]).exists():
        m["image"] = str((root / m["image"]).resolve())
        break
else:
    sys.exit(f"disc image {m['image']} not found")
out.write_text(json.dumps(m))
EOF
    py_us=$work/us
fi

python3 "$repo/tools/text/en_text.py" --jp "$jp" --us "$py_us" --out "$work/py" > "$work/py.log" 2>&1 ||
    { status=$?; [ $status -eq 1 ] || { cat "$work/py.log" >&2; exit 1; }; }  # 1: catalog problems only
rm -rf "$work/py/fixes"

args=(--text-only --jp "$cpp_jp" --us "$cpp_us" --out "$work/cpp")
[ -n "$fixes" ] && args+=(--fixes "$fixes")
start=$(date +%s.%N)
"$dcb_patch" "${args[@]}" 2> "$work/cpp.log" || { cat "$work/cpp.log" >&2; exit 1; }
seconds=$(echo "$(date +%s.%N) - $start" | bc)
[ -s "$work/cpp.log" ] && sed 's/^/dcb_patch: /' "$work/cpp.log" >&2  # warnings (e.g. a fix that failed)

if diff -r "$work/py" "$work/cpp/SLPS-03101"; then
    echo "identical: $(find "$work/py" -type f | wc -l) files$([ -n "$fixes" ] && echo ", with the fixes in $fixes") (dcb_patch ${seconds}s)"
else
    echo "the C++ output differs from en_text.py" >&2
    exit 1
fi

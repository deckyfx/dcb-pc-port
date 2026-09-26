#!/usr/bin/env bash
# Check that the native importer (`dcb --import`) and tools/disc/extract_disc.py produce
# byte-identical layout.txt, iso_meta.bin and fs/ for a real dump (game data cannot live in the
# repository, so this is a manual step; tests/import checks the same on a synthetic disc).
#
#   tools/disc/verify_import.sh <disc.cue|disc.bin> [path/to/dcb]
#
# Both outputs go to a temporary directory that is removed afterwards.
set -euo pipefail

image=${1:?usage: verify_import.sh <disc.cue|disc.bin> [dcb]}
dcb=${2:-build/linux-debug/dcb}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$dcb" --import "$image" "$work/native" 2> /dev/null
serial=$(ls "$work/native")
python3 "$here/extract_disc.py" "$image" -o "$work/python" > /dev/null

status=0
for f in layout.txt iso_meta.bin; do
    cmp "$work/python/$f" "$work/native/$serial/$f" || status=1
done
diff -r "$work/python/fs" "$work/native/$serial/fs" || status=1
if [ $status -eq 0 ]; then
    echo "$serial: layout.txt, iso_meta.bin and fs/ ($(find "$work/python/fs" -type f | wc -l) files) are byte-identical"
else
    echo "$serial: the native import differs from extract_disc.py" >&2
fi
exit $status

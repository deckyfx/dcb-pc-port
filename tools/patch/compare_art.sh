#!/usr/bin/env bash
# Check patch::build_art (the C++ port, dcb_patch --art-only) against the offline Python pipeline:
# dcb_asset_ripper unpack of both discs + tools/assets/swap_us_images.py --apply.
#
#   tools/patch/compare_art.sh [JP_DUMP] [US_DUMP]
#
# Dumps default to extracted/SLPS-03101 and extracted/SLUS-01328 (anything with fs/: the
# importer's assets/dump/<serial>/ works too). Needs dcb_asset_ripper and dcb_patch in $BUILD
# (default build/linux-debug). Everything is written to a temporary folder, removed at the end;
# the dumps are only read.
#
# Passes when every manifest entry the Python run points at us/*.raw is in the C++ pak with the
# same fields and byte-identical data, and the pak holds nothing else (the JP PNG entries of the
# offline pak are identity art: without an entry the game commits its own upload). Also checks
# the US movie override.
set -euo pipefail
cd "$(dirname "$0")/../.."

JP="$(realpath "${1:-extracted/SLPS-03101}")"
US="$(realpath "${2:-extracted/SLUS-01328}")"
BUILD="${BUILD:-build/linux-debug}"
for tool in dcb_asset_ripper dcb_patch; do
    [[ -x "$BUILD/$tool" ]] || { echo "error: no $BUILD/$tool (build it first)" >&2; exit 1; }
done

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
mkdir -p "$SCRATCH/py/extracted"
# The swap reads <root>/extracted/<serial>/fs and names the manifest dirs after the serial.
ln -s "$JP" "$SCRATCH/py/extracted/SLPS-03101"
ln -s "$US" "$SCRATCH/py/extracted/SLUS-01328"

echo "-- Python: rip both discs, swap"
t0=$(date +%s)
for serial in SLPS-03101 SLUS-01328; do
    "$BUILD/dcb_asset_ripper" unpack "$SCRATCH/py/extracted/$serial" -o "$SCRATCH/py/assets" >/dev/null 2>&1
done
python3 tools/assets/swap_us_images.py --apply --root "$SCRATCH/py" 2>/dev/null | head -1
t1=$(date +%s)
echo "   $((t1 - t0)) s"

echo "-- C++: dcb_patch --art-only"
t0=$(date +%s)
"$BUILD/dcb_patch" --art-only --jp "$JP" --us "$US" --out "$SCRATCH/cpp"
t1=$(date +%s)
echo "   $((t1 - t0)) s"

python3 - "$SCRATCH" "$JP" "$US" <<'EOF'
import json, struct, sys
from pathlib import Path

scratch, jp, us = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
conv = scratch / "py" / "assets" / "converted" / "SLPS-03101"


def read_pak(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    if data[:8] != b"DCBPAK01":
        sys.exit(f"{path}: not a pak")
    (n,), pos, files = struct.unpack_from("<I", data, 8), 12, {}
    for _ in range(n):
        (ln,) = struct.unpack_from("<H", data, pos)
        name = data[pos + 2:pos + 2 + ln].decode()
        off, size, _hash = struct.unpack_from("<QQQ", data, pos + 2 + ln)
        files[name] = data[off:off + size]
        pos += 2 + ln + 24
    return files


py_entries = json.loads((conv / "assets_manifest.json").read_text())["entries"]
py_raw = {e["img"]: e for e in py_entries if e["path"].startswith("us/")}
py_png = [e for e in py_entries if not e["path"].startswith("us/")]
pak = read_pak(scratch / "cpp" / "SLPS-03101.pak")
cpp_entries = json.loads(pak["assets_manifest.json"])["entries"]
cpp = {}
errors = []
for e in cpp_entries:
    if e["img"] in cpp:
        errors.append(f"duplicate entry {e['img']}")
    cpp[e["img"]] = e

images = sum(1 for e in py_raw.values() if not e["us"].endswith("(palette)"))
print(f"Python: {len(py_raw)} us/*.raw entries ({images} images, {len(py_raw) - images} palettes), "
      f"{len(py_png)} JP PNG entries (not needed)")
print(f"C++:    {len(cpp)} entries, {len(pak)} files in the pak")

same = 0
for key, e in py_raw.items():
    mine = cpp.get(key)
    if mine is None:
        errors.append(f"missing {key} ({e['us']})")
        continue
    if mine != e:
        errors.append(f"fields differ for {key}: python {e} / c++ {mine}")
        continue
    want = (conv / e["path"]).read_bytes()
    if pak.get(e["path"]) != want:
        errors.append(f"data differs for {key} ({e['us']})")
        continue
    same += 1
extra_entries = sorted(set(cpp) - set(py_raw))
extra_files = sorted(set(pak) - {"assets_manifest.json"} - {e["path"] for e in cpp_entries})
errors += [f"extra entry {k} ({cpp[k]['path']})" for k in extra_entries]
errors += [f"extra file {f}" for f in extra_files]
# A key both a replaced upload and a JP PNG entry would change which candidate the game picks.
errors += [f"{e['img']} also has a JP PNG entry" for e in py_png if e["img"] in py_raw]
print(f"identical: {same}/{len(py_raw)} entries (fields and data)")

movie = "DIGIMON.MOV.raw2352"
ours = scratch / "cpp" / "SLPS-03101" / "disc" / movie
if (us / "fs" / movie).stat().st_size == (jp / "fs" / movie).stat().st_size:
    if not ours.is_file() or ours.read_bytes() != (us / "fs" / movie).read_bytes():
        errors.append("the US movie override is missing or differs")
    else:
        print("movie:  the US movie is the disc override")
elif ours.exists():
    errors.append("a movie override was written although the sizes differ")

for err in errors[:40]:
    print("  " + err)
if errors:
    sys.exit(f"FAIL: {len(errors)} differences")
print("PASS")
EOF

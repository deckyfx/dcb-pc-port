# Export user-named functions to the repo's symbol map (ghidra/symbols/<serial>.json).
# @category DCB
# @runtime PyGhidra
#
# GUI:       Script Manager > DCB > export_symbols.py (arg optional: output file)
# Headless:  support/pyghidraRun --headless ghidra/project DCB/SLPS-03101 -process SLPS_031.01 -noanalysis \
#              -scriptPath ghidra/scripts -postScript export_symbols.py ghidra/symbols/SLPS-03101.json
#
# Only functions a human named (not FUN_*) are exported; Ghidra 12 ships PyGhidra (CPython 3) only.
# The file is diffable text: one object per address, sorted, with the source noted. The recompiler
# and the call-trace tooling read these names (docs/RE_WORKFLOW.md).
import json

args = getScriptArgs()
out_path = args[0] if args else askFile("SLPS-03101.json", "Export").getAbsolutePath()

symbols = {}
for fn in currentProgram.getFunctionManager().getFunctions(True):
    if fn.isExternal() or fn.isThunk():
        continue
    name = fn.getName()
    if name.startswith("FUN_"):
        continue
    addr = "0x%08X" % fn.getEntryPoint().getOffset()
    symbols[addr] = {"name": name, "source": "ghidra"}

with open(out_path, "w") as f:
    json.dump({"program": currentProgram.getName(), "symbols": symbols}, f, indent=1, sort_keys=True)
    f.write("\n")
print("exported %d symbols to %s" % (len(symbols), out_path))

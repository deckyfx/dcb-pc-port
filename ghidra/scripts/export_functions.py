# Export function boundaries and names for the MIPS->C recompiler.
# @category DCB
# @runtime PyGhidra
#
# GUI:       Script Manager > DCB > export_functions.py (asks for the output file)
# Headless:  support/pyghidraRun --headless ghidra/project DCB/SLPS-03101 -process SLPS_031.01 -noanalysis \
#              -scriptPath ghidra/scripts -postScript export_functions.py config/SLPS-03101/functions.json
#
# Ghidra 12 ships PyGhidra (CPython 3) only; Jython is no longer bundled.
import json

args = getScriptArgs()
out_path = args[0] if args else askFile("functions.json", "Export").getAbsolutePath()

functions = []
for fn in currentProgram.getFunctionManager().getFunctions(True):
    if fn.isExternal() or fn.isThunk():
        continue
    body = fn.getBody()
    ranges = [[r.getMinAddress().getOffset(), r.getMaxAddress().getOffset() + 1] for r in body.getAddressRanges()]
    functions.append({
        "name": fn.getName(),
        "entry": "0x%08X" % fn.getEntryPoint().getOffset(),
        "ranges": [["0x%08X" % a, "0x%08X" % b] for a, b in ranges],
        "user_named": not fn.getName().startswith("FUN_"),
    })

with open(out_path, "w") as f:
    json.dump({"program": currentProgram.getName(), "functions": functions}, f, indent=1)
print("exported %d functions to %s" % (len(functions), out_path))

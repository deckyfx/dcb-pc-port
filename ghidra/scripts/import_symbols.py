# Apply the repo's symbol map (ghidra/symbols/<serial>.json) to this program.
# @category DCB
# @runtime PyGhidra
#
# GUI:       Script Manager > DCB > import_symbols.py (arg optional: symbols file)
# Headless:  support/pyghidraRun --headless ghidra/project DCB/SLPS-03101 -process SLPS_031.01 -noanalysis \
#              -scriptPath ghidra/scripts -postScript import_symbols.py ghidra/symbols/SLPS-03101.json
#
# Re-runnable: only renames FUN_* functions, never overwrites a human name.
import json

args = getScriptArgs()
if args:
    sym_path = args[0]
else:
    sym_path = askFile("SLPS-03101.json", "Import").getAbsolutePath()

with open(sym_path) as f:
    data = json.load(f)

fm = currentProgram.getFunctionManager()
applied = skipped = missing = 0
for addr_str, sym in sorted(data.get("symbols", {}).items()):
    addr = toAddr(int(addr_str, 16))
    fn = fm.getFunctionAt(addr)
    if fn is None:
        missing += 1
        continue
    if not fn.getName().startswith("FUN_"):
        skipped += 1  # a human name already lives here; never overwrite
        continue
    fn.setName(sym["name"], ghidra.program.model.symbol.SourceType.USER_DEFINED)
    applied += 1
print("import_symbols: %d applied, %d kept, %d addresses without function" % (applied, skipped, missing))

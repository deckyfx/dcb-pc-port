# Ghidra

[Home](Home.md)

The project uses Ghidra 12.1.2 in `ghidra/ghidra_12.1.2_PUBLIC`, with the `ghidra_psx_ldr` loader
and Psy-Q signatures. The Ghidra database lives in `ghidra/project/` (`DCB.gpr`, one folder per
serial; gitignored).

## Importing the boot EXE

After extracting the disc with `tools/disc/extract_disc.py` (see [Building](Building.md#workflow)):

```bash
tools/ghidra/import_ghidra.sh SLPS-03101     # ghidra_psx_ldr loader + Psy-Q signatures, ~3 min
```

## Ghidra MCP

`.mcp.json` registers the `ghidra` server (bethington/ghidra-mcp 6.0.0, built for Ghidra 12.1.2).
Install or reinstall it with `tools/ghidra/setup_ghidra_mcp.sh`.

Start Ghidra with `tools/ghidra/ghidra_gui.sh` (or `./dcb.sh -G`): it launches through PyGhidra (so
`.py` scripts work in the GUI) and sets `GHIDRA_MCP_ALLOW_SCRIPTS=1` (so MCP can run repo scripts;
this allows arbitrary Java in Ghidra, loopback only). Then in Ghidra:

1. enable **GhidraMCP** under *File → Configure → Configure All Plugins* (once),
2. open the program,
3. choose *Tools → GhidraMCP → Start MCP Server*.

## Mirroring recompiler discovery into Ghidra

After a recompile (`--target recompile`, which writes `discovered.json`), mirror the discovery into
Ghidra (new functions + overlay blocks such as `KAWSEG::801E2A6C`): in the GUI, *Script Manager >
DCB > ApplyDiscovered.java* (or via MCP `run_ghidra_script`), or headless with Ghidra closed:

```bash
ghidra/ghidra_12.1.2_PUBLIC/support/analyzeHeadless ghidra/project DCB/SLPS-03101 -process SLPS_031.01 \
    -noanalysis -scriptPath ghidra/scripts -postScript ApplyDiscovered.java "$PWD"
```

## Scripts

`ghidra/scripts/` holds the repo's Ghidra scripts (category *DCB* in the Script Manager; each
script's header gives its GUI and headless usage):

| Script | Purpose |
|---|---|
| `ApplyDiscovered.java` | recompiler discovery → Ghidra functions and overlay blocks (above) |
| `ImportCoverage.java` | import a `DCB_COVERAGE` report: tag executed functions, bookmark them, comment call counts and first frame |
| `export_functions.py` | function boundaries and names → `config/<serial>/functions.json` for the recompiler |
| `export_symbols.py` / `import_symbols.py` | user-named functions ⇄ the reviewable symbol map `ghidra/symbols/<serial>.json` |

The `.py` scripts need PyGhidra (`ghidra_gui.sh`, or `support/pyghidraRun --headless`).

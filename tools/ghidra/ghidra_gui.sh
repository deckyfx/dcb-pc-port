#!/usr/bin/env bash
# Start the Ghidra GUI through PyGhidra (Ghidra 12 runs .py scripts only when launched this way)
# with GhidraMCP script execution enabled, so the MCP
# tools run_script_inline / run_ghidra_script work (they run arbitrary Java inside Ghidra).
# The plugin stays bound to 127.0.0.1; set GHIDRA_MCP_AUTH_TOKEN before ever exposing it further.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GHIDRA_INSTALL_DIR="${GHIDRA_INSTALL_DIR:-$ROOT/ghidra/ghidra_12.1.2_PUBLIC}"
[[ -x "$GHIDRA_INSTALL_DIR/support/pyghidraRun" ]] || {
    echo "error: no Ghidra at $GHIDRA_INSTALL_DIR (unpack ghidra_12.1.2_PUBLIC into ghidra/, or set GHIDRA_INSTALL_DIR)" >&2; exit 1; }
# PyGhidra in Ghidra's own venv (where the launcher looks for it), made once with the system
# Python. Put first on PATH: the launcher runs under the first python3 and prefers an active venv,
# so another venv on PATH (e.g. a pipx app's, which has no pip) would otherwise be used.
GHIDRA_VENV="${GHIDRA_VENV:-$HOME/.config/ghidra/$(basename "$GHIDRA_INSTALL_DIR")/venv}"
if [[ ! -x "$GHIDRA_VENV/bin/python3" ]]; then
    echo "-- creating the PyGhidra venv at $GHIDRA_VENV"
    /usr/bin/env -i PATH=/usr/bin:/bin python3 -m venv "$GHIDRA_VENV"
    "$GHIDRA_VENV/bin/python3" -m pip install -q --no-index \
        -f "$GHIDRA_INSTALL_DIR/Ghidra/Features/PyGhidra/pypkg/dist" pyghidra
fi
unset VIRTUAL_ENV PYTHONHOME
export PATH="$GHIDRA_VENV/bin:$PATH"
export GHIDRA_MCP_ALLOW_SCRIPTS=1
exec "$GHIDRA_INSTALL_DIR/support/pyghidraRun" "$@"

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
export GHIDRA_MCP_ALLOW_SCRIPTS=1
exec "$GHIDRA_INSTALL_DIR/support/pyghidraRun" "$@"

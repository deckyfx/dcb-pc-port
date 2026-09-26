#!/usr/bin/env bash
# Install the GhidraMCP extension + MCP bridge (bethington/ghidra-mcp), pinned and checksum-verified.
#
#   GHIDRA_INSTALL_DIR=/path/to/ghidra_12.1.2_PUBLIC tools/ghidra/setup_ghidra_mcp.sh
#
# Afterwards, in the Ghidra GUI (one time):
#   File > Configure > Configure All Plugins > enable "GhidraMCP"
#   Tools > GhidraMCP > Start MCP Server        (every session, with a program open)
set -euo pipefail

VERSION="6.0.0"
BASE_URL="https://github.com/bethington/ghidra-mcp/releases/download/v${VERSION}"
EXT_ZIP="GhidraMCP-${VERSION}.zip"
EXT_SHA256="867731de27d5143632a010943b907a6485dd54d0e19729e2f85ee9f692c99873"
WHEEL="ghidra_mcp_bridge-${VERSION}-py3-none-any.whl"
WHEEL_SHA256="71939a890009826664720166d8f786f3a2ea2460f7effcd83c73759ba4241334"

GHIDRA_INSTALL_DIR="${GHIDRA_INSTALL_DIR:-$(cd "$(dirname "$0")/../../.." && pwd)/ghidra_12.1.2_PUBLIC}"
[[ -f "$GHIDRA_INSTALL_DIR/Ghidra/application.properties" ]] || {
    echo "error: set GHIDRA_INSTALL_DIR (no Ghidra at $GHIDRA_INSTALL_DIR)" >&2; exit 1; }
GHIDRA_VERSION="$(sed -n 's/^application.version=//p' "$GHIDRA_INSTALL_DIR/Ghidra/application.properties")"
USER_EXT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/ghidra/ghidra_${GHIDRA_VERSION}_PUBLIC/Extensions"

command -v uv >/dev/null || { echo "error: uv is required (https://docs.astral.sh/uv/)" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fetch() {  # fetch <file> <sha256>
    curl -fsSL -o "$work/$1" "$BASE_URL/$1"
    echo "$2  $work/$1" | sha256sum --check --quiet
}

echo ">> downloading ghidra-mcp v$VERSION"
fetch "$EXT_ZIP" "$EXT_SHA256"
fetch "$WHEEL" "$WHEEL_SHA256"

plugin_ghidra="$(unzip -p "$work/$EXT_ZIP" GhidraMCP/extension.properties | sed -n 's/^version=//p')"
[[ "$plugin_ghidra" == "$GHIDRA_VERSION" ]] || {
    echo "error: GhidraMCP $VERSION targets Ghidra $plugin_ghidra, you have $GHIDRA_VERSION" >&2; exit 1; }

echo ">> installing Ghidra extension into $USER_EXT_DIR"
mkdir -p "$USER_EXT_DIR"
rm -rf "$USER_EXT_DIR/GhidraMCP"
unzip -q "$work/$EXT_ZIP" -d "$USER_EXT_DIR"

echo ">> installing MCP bridge (bridge-mcp-ghidra)"
uv tool install --force "$work/$WHEEL"

echo
echo "Done. Next, in Ghidra ($GHIDRA_INSTALL_DIR/ghidraRun):"
echo "  1. File > Configure > Configure All Plugins > enable GhidraMCP (once)"
echo "  2. Open the program, then Tools > GhidraMCP > Start MCP Server"
echo "  3. Restart Claude Code in this folder and approve the 'ghidra' server from .mcp.json"

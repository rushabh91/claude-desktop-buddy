#!/bin/bash
# Install the usage companion as a macOS LaunchAgent: starts at login, stays
# running, scans for the buddy, pushes usage only while it's connected, and
# idles (just scanning) when it's powered off. Reload-safe — re-run to update.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PYTHON="$REPO/.venv/bin/python3"
SCRIPT="$SCRIPT_DIR/usage_companion.py"
LABEL="com.claude.usage-companion"
PLIST_SRC="$SCRIPT_DIR/$LABEL.plist"
PLIST_DST="$HOME/Library/LaunchAgents/$LABEL.plist"
DOMAIN="gui/$(id -u)"

if [ ! -x "$PYTHON" ]; then
    echo "ERROR: venv python not found at $PYTHON"
    echo "Create it first:  python3 -m venv .venv && .venv/bin/pip install bleak"
    exit 1
fi

mkdir -p "$HOME/Library/LaunchAgents"

# Fill the placeholders with this clone's absolute paths.
sed -e "s|__PYTHON__|$PYTHON|g" \
    -e "s|__SCRIPT__|$SCRIPT|g" \
    -e "s|__REPO__|$REPO|g" \
    "$PLIST_SRC" > "$PLIST_DST"

# Reload (bootout is harmless if it wasn't loaded). Fall back to legacy load.
launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
if ! launchctl bootstrap "$DOMAIN" "$PLIST_DST" 2>/dev/null; then
    launchctl unload "$PLIST_DST" 2>/dev/null || true
    launchctl load "$PLIST_DST"
fi
launchctl enable "$DOMAIN/$LABEL" 2>/dev/null || true

echo "✓ Installed and started: $LABEL"
echo "  Logs:    $SCRIPT_DIR/companion.log"
echo "  Restart: launchctl kickstart -k $DOMAIN/$LABEL"
echo "  Remove:  $SCRIPT_DIR/uninstall-launchagent.sh"
echo
echo "If the buddy's S__% W__% stays blank, the background process likely lacks"
echo "Bluetooth permission. Open System Settings → Privacy & Security → Bluetooth,"
echo "enable the entry for python3 (add it if needed), then run the Restart command."

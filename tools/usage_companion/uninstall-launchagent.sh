#!/bin/bash
# Stop and remove the usage companion LaunchAgent.
set -euo pipefail

LABEL="com.claude.usage-companion"
PLIST_DST="$HOME/Library/LaunchAgents/$LABEL.plist"
DOMAIN="gui/$(id -u)"

launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null \
    || launchctl unload "$PLIST_DST" 2>/dev/null \
    || true
rm -f "$PLIST_DST"
echo "✓ Uninstalled: $LABEL"

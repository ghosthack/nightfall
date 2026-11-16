#!/usr/bin/env bash
# Installs Nightfall.app to ~/Applications and starts it (menu bar moon + Ctrl+Option+B).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
# release archives ship the app next to this script; a source checkout builds it
app="$here/Nightfall.app"
if [ ! -d "$app" ]; then app="$here/build/Nightfall.app"; [ -d "$app" ] || make -C "$here"; fi
pkill -x Nightfall 2>/dev/null || true
mkdir -p "$HOME/Applications"
rm -rf "$HOME/Applications/Nightfall.app"
cp -R "$app" "$HOME/Applications/"
[ "${1:-}" = "--no-launch" ] || open "$HOME/Applications/Nightfall.app"
echo "Installed ~/Applications/Nightfall.app"

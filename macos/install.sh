#!/usr/bin/env bash
# Installs Nightfall.app to ~/Applications and starts it (menu bar moon + Ctrl+Option+B).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[ -d "$here/build/Nightfall.app" ] || make -C "$here"
pkill -x Nightfall 2>/dev/null || true
mkdir -p "$HOME/Applications"
rm -rf "$HOME/Applications/Nightfall.app"
cp -R "$here/build/Nightfall.app" "$HOME/Applications/"
[ "${1:-}" = "--no-launch" ] || open "$HOME/Applications/Nightfall.app"
echo "Installed ~/Applications/Nightfall.app"

#!/usr/bin/env bash
pkill -x Nightfall 2>/dev/null
rm -rf "$HOME/Applications/Nightfall.app"
echo "Nightfall removed (if you enabled Start at Login, also remove it in System Settings > General > Login Items)"

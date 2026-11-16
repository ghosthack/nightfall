#!/usr/bin/env bash
# Installs Nightfall for the current user (GNOME). No root needed once built.
#   - ~/.local/bin/nightfall
#   - app-grid entry "Nightfall" (+ pinned to the dock)
#   - GNOME shortcut Ctrl+Alt+B -> nightfall
#   flags: --no-dock  --no-shortcut
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="$HOME/.local/bin/nightfall"
apps="$HOME/.local/share/applications"
icons="$HOME/.local/share/icons/hicolor/scalable/apps"
dock=1; shortcut=1
for a in "$@"; do case "$a" in --no-dock) dock=0;; --no-shortcut) shortcut=0;; esac; done

[ -x "$here/build/nightfall" ] || make -C "$here"
install -Dm755 "$here/build/nightfall" "$bin"
install -Dm644 "$here/res/nightfall.svg" "$icons/nightfall.svg"
mkdir -p "$apps"
cat > "$apps/io.github.nightfall.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Nightfall
Comment=Blank the screen now (any key or click to wake)
Exec=$bin
Icon=nightfall
Terminal=false
Categories=Utility;
StartupNotify=false
DESK
update-desktop-database "$apps" 2>/dev/null || true
gtk-update-icon-cache -q "$HOME/.local/share/icons/hicolor" 2>/dev/null || true

if [ $dock = 1 ]; then
  favs=$(gsettings get org.gnome.shell favorite-apps)
  if [[ "$favs" != *io.github.nightfall.desktop* ]]; then
    if [[ "$favs" == "@as []" ]]; then favs="['io.github.nightfall.desktop']"
    else favs="${favs%]}, 'io.github.nightfall.desktop']"; fi
    gsettings set org.gnome.shell favorite-apps "$favs"
  fi
fi

if [ $shortcut = 1 ]; then
  base=org.gnome.settings-daemon.plugins.media-keys
  path=/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/nightfall/
  list=$(gsettings get $base custom-keybindings)
  if [[ "$list" != *"$path"* ]]; then
    if [[ "$list" == "@as []" ]]; then list="['$path']"; else list="${list%]}, '$path']"; fi
    gsettings set $base custom-keybindings "$list"
  fi
  kb="$base.custom-keybinding:$path"
  gsettings set "$kb" name 'Nightfall'
  gsettings set "$kb" command "$bin"
  gsettings set "$kb" binding '<Control><Alt>b'
fi
echo "Installed $bin"

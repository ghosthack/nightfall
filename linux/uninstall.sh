#!/usr/bin/env bash
set -u
pkill -x nightfall 2>/dev/null
rm -f "$HOME/.local/bin/nightfall" "$HOME/.local/share/applications/io.github.nightfall.desktop" \
      "$HOME/.local/share/icons/hicolor/scalable/apps/nightfall.svg"
favs=$(gsettings get org.gnome.shell favorite-apps)
favs=$(sed -e "s/, 'io.github.nightfall.desktop'//; s/'io.github.nightfall.desktop', //; s/'io.github.nightfall.desktop'//" <<<"$favs")
gsettings set org.gnome.shell favorite-apps "$favs"
base=org.gnome.settings-daemon.plugins.media-keys
path=/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/nightfall/
list=$(gsettings get $base custom-keybindings)
list=$(sed -e "s#, '$path'##; s#'$path', ##; s#'$path'##" <<<"$list")
gsettings set $base custom-keybindings "$list"
gsettings reset-recursively "$base.custom-keybinding:$path" 2>/dev/null
echo "Nightfall removed"

#!/usr/bin/env bash
# End-to-end test against the logged-in GNOME session. Run from SSH after ./install.sh.
set -u
here="$(cd "$(dirname "$0")" && pwd)"; . "$here/env.sh"
out=/tmp/nightfall-e2e; rm -rf "$out"; mkdir -p "$out"
bin="$HOME/.local/bin/nightfall"
kb=org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/nightfall/
inp() { sudo python3 "$here/input.py" "$@"; }
pid() { pgrep -x nightfall | head -1; }
ok=0; fail=0
check() { if eval "$2"; then echo "PASS $1"; ok=$((ok+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }

pkill -x nightfall; sleep 0.5
# --- 1: GNOME shortcut Ctrl+Alt+B (command temporarily wrapped to enable debug output)
orig=$(gsettings get "$kb" command)
gsettings set "$kb" command "env NIGHTFALL_DEBUG=$out $bin"
sleep 1
inp hotkey; sleep 3.5
p=$(pid); check "Ctrl+Alt+B launches nightfall" '[ -n "$p" ]'
check "window fullscreen + focused" 'grep -q "start screen=0 .*fullscreen=1 active=1" $out/nightfall.log'
inp move; sleep 1
check "mouse movement does not wake" '[ -n "$(pid)" ]'
[ -n "$p" ] && echo "rss: $(ps -o rss= -p $p) KB  cpu after ~5s: $(ps -o time= -p $p)"
sleep 16
check "clock moved after 20 s" 'grep -q "clock moved" $out/nightfall.log'
[ -n "$p" ] && echo "rss: $(ps -o rss= -p $p) KB  cpu after ~22s: $(ps -o time= -p $p)  (%cpu $(ps -o %cpu= -p $p))"
inp key; sleep 1
check "key wakes and exits" '[ -z "$(pid)" ] && grep -q "key space" $out/nightfall.log'
gsettings set "$kb" command "$orig"

# --- 2: app launcher (same path the dock / app grid uses), wake by mouse click
NIGHTFALL_DEBUG=$out gtk-launch io.github.nightfall; sleep 3.5
check "launcher starts nightfall" '[ -n "$(pid)" ]'
inp move click; sleep 1
check "mouse click (after a nudge) wakes and exits" '[ -z "$(pid)" ] && grep -q "click button 1" $out/nightfall.log'

# --- 3: grace period: keypress right at launch is ignored
NIGHTFALL_DEBUG=$out "$bin" & sleep 0.3
inp esc >/dev/null 2>&1 &   # lands ~1 s later (device settle) -> may or may not be in grace
sleep 4
if [ -n "$(pid)" ]; then inp esc; sleep 1; fi
check "Esc wakes too" '[ -z "$(pid)" ] && grep -q "key Escape" $out/nightfall.log'

check "settings restored" '[ "$(gsettings get "$kb" command)" = "$orig" ]'
echo "---- log"; cat $out/nightfall.log
echo "RESULT: $ok passed, $fail failed"

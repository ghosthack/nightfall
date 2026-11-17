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

# --- 4: digital rain (one-off flag, then the saved style)
mv $out/nightfall.log $out/clock.log
NIGHTFALL_DEBUG=$out "$bin" --rain & sleep 4
check "--rain blanks with the digital rain" '[ -n "$(pid)" ] && grep -q "mode=rain" $out/nightfall.log && grep -q "^.* rain: " $out/nightfall.log'
p=$(pid); [ -n "$p" ] && { t0=$(cut -d" " -f14,15 /proc/$p/stat); sleep 5; t1=$(cut -d" " -f14,15 /proc/$p/stat)
  echo "rain: rss $(ps -o rss= -p $p) KB, cpu $(echo $t0 $t1 | awk '{print ($3+$4-$1-$2)/5 "% of one core"}')"; }
inp key; sleep 1
check "key wakes the rain" '[ -z "$(pid)" ]'
modefile="${XDG_CONFIG_HOME:-$HOME/.config}/nightfall/mode"
saved=$(cat "$modefile" 2>/dev/null)
"$bin" --set-mode rain >/dev/null
NIGHTFALL_DEBUG=$out "$bin" & sleep 3
check "saved style is used (--set-mode rain)" 'grep -c "mode=rain" $out/nightfall.log | grep -q 2'
inp key; sleep 1
if [ -n "$saved" ]; then "$bin" --set-mode "$saved" >/dev/null; else rm -f "$modefile"; fi
check "style restored, rain exits" '[ -z "$(pid)" ] && [ "$(cat "$modefile" 2>/dev/null)" = "$saved" ]'

# --- 5: gray mosaic (one-off flag)
mv $out/nightfall.log $out/rain.log; rm -f $out/start-screen0.png
NIGHTFALL_DEBUG=$out "$bin" --mosaic & sleep 4
check "--mosaic blanks with the gray mosaic" '[ -n "$(pid)" ] && grep -q "mode=mosaic" $out/nightfall.log && grep -q "^.* mosaic: " $out/nightfall.log && [ -s $out/start-screen0.png ]'
p=$(pid); [ -n "$p" ] && { t0=$(cut -d" " -f14,15 /proc/$p/stat); sleep 5; t1=$(cut -d" " -f14,15 /proc/$p/stat)
  echo "mosaic: rss $(ps -o rss= -p $p) KB, cpu $(echo $t0 $t1 | awk '{print ($3+$4-$1-$2)/5 "% of one core"}')"; }
inp key; sleep 1
check "key wakes the mosaic" '[ -z "$(pid)" ]'

# --- 6: the scene-engine styles: each draws something, reports its CPU, and a key wakes it
mv $out/nightfall.log $out/mosaic.log
for st in stars analog words curves maze life ripples plasma fireflies starfield; do
  rm -f $out/nightfall.log
  NIGHTFALL_DEBUG=$out "$bin" --$st &
  drew=0
  for i in $(seq 16); do   # the first debug snapshot lands 2-3 s after launch
    sleep 0.5
    grep -q "scene: $st" $out/nightfall.log 2>/dev/null && grep -q "lit_tiles=[1-9]" $out/nightfall.log && { drew=1; break; }
  done
  p=$(pid)
  [ -n "$p" ] && { t0=$(cut -d" " -f14,15 /proc/$p/stat); sleep 4; t1=$(cut -d" " -f14,15 /proc/$p/stat)
    echo "$st: rss $(ps -o rss= -p $p) KB, cpu $(echo $t0 $t1 | awk '{print ($3+$4-$1-$2)/4 "% of one core"}')"; }
  inp key; sleep 1
  check "--$st draws, key wakes it" '[ $drew = 1 ] && [ -z "$(pid)" ]'
  pkill -x nightfall
done

check "settings restored" '[ "$(gsettings get "$kb" command)" = "$orig" ]'
echo "---- log"; cat $out/clock.log $out/rain.log $out/mosaic.log
echo "RESULT: $ok passed, $fail failed"

#!/usr/bin/env bash
# End-to-end test on this Mac. Blanks the real screen for ~40 s total.
# OS-level input injection needs Accessibility permission, so input is driven through the app's
# real event path (NSApp.sendEvent) via its test hooks; state is checked from its debug log/PNGs.
set -u
app="$HOME/Applications/Nightfall.app"
out=/tmp/nightfall-mac-e2e; rm -rf "$out"; mkdir -p "$out"
log=$out/nightfall.log
ok=0; fail=0
check() { if eval "$2"; then echo "PASS $1"; ok=$((ok+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }
running() { pgrep -x Nightfall >/dev/null; }

pkill -x Nightfall; sleep 0.5
# --- 1: scripted self-test through the real event path
NIGHTFALL_DEBUG=$out NIGHTFALL_SELFTEST=1 "$app/Contents/MacOS/Nightfall" &
for i in $(seq 120); do running || break; sleep 0.5; done
check "hotkey Ctrl+Option+B registered" 'grep -q "launched hotkey_ok=true" $log'
check "covers display, key window, above everything" 'grep -q "start display=0 .*covers=true key=true visible=true level=1000 .*app_active=true" $log'
check "key during grace period ignored" 'grep -q "ignored (grace)" $log'
check "mouse movement does not wake" 'grep -q "st: moved, active=true" $log'
check "clock moved after 20 s" 'grep -q "clock moved" $log'
check "key wakes" 'grep -q "st: after key, active=false" $log'
check "left click wakes" 'grep -q "st: after click, active=false" $log'
check "right click wakes" 'grep -q "st: after right click, active=false" $log'
check "self-test exited" '! running'

# --- 2: real launch + "open the app again" (Spotlight/Finder/Launchpad path) blanks
mv $log $out/selftest.log
open --env NIGHTFALL_DEBUG=$out --env NIGHTFALL_AUTOWAKE=4 "$app"; sleep 2
check "launches into menu bar without blanking" 'running && grep -q launched $log && ! grep -q blank $log'
p=$(pgrep -x Nightfall); echo "idle: rss $(ps -o rss= -p $p) KB"
open "$app"; sleep 3
check "re-opening the app blanks" 'grep -q reopen $log && grep -q "blank:" $log'
echo "blanked: rss $(ps -o rss= -p $p) KB, cpu $(ps -o time= -p $p)"
sleep 3
check "autowake key wakes, app stays in menu bar" 'grep -q wake $log && running'
check "single instance" '[ $(pgrep -x Nightfall | wc -l) -eq 1 ]'
# --- 3: digital rain
pkill -x Nightfall; sleep 0.5
mv $log $out/launch.log
NIGHTFALL_DEBUG=$out NIGHTFALL_AUTOWAKE=8 "$app/Contents/MacOS/Nightfall" --blank --rain &
sleep 4; p=$(pgrep -x Nightfall); c0=$(ps -o time= -p $p)
sleep 3; echo "rain: rss $(ps -o rss= -p $p) KB, cpu $c0 at 4 s -> $(ps -o time= -p $p) at 7 s"
check "--rain blanks with the digital rain" 'grep -q "style=rain" $log && [ -s $out/start-display0.png ]'
sleep 3
check "key wakes the rain" 'grep -q wake $log'
pkill -x Nightfall; sleep 0.5
# --- 4: gray mosaic
mv $log $out/rain.log; rm -f $out/start-display0.png
NIGHTFALL_DEBUG=$out NIGHTFALL_AUTOWAKE=8 "$app/Contents/MacOS/Nightfall" --blank --mosaic &
sleep 4; p=$(pgrep -x Nightfall); c0=$(ps -o time= -p $p)
sleep 3; echo "mosaic: rss $(ps -o rss= -p $p) KB, cpu $c0 at 4 s -> $(ps -o time= -p $p) at 7 s"
check "--mosaic blanks with the gray mosaic" 'grep -q "style=mosaic" $log && grep -q "mosaic: " $log && [ -s $out/start-display0.png ]'
sleep 3
check "key wakes the mosaic" 'grep -q wake $log'
pkill -x Nightfall; sleep 0.5
# --- 5: the scene-engine styles: each draws something, reports its CPU, and a key wakes it
mv $log $out/mosaic.log
for st in stars analog words curves maze life ripples plasma fireflies starfield; do
  rm -f $log
  NIGHTFALL_DEBUG=$out NIGHTFALL_AUTOWAKE=7 "$app/Contents/MacOS/Nightfall" --blank --$st &
  sleep 4; p=$(pgrep -x Nightfall); c0=$(ps -o cputime= -p $p)
  sleep 2.5; echo "$st: rss $(ps -o rss= -p $p) KB, cpu $c0 at 4 s -> $(ps -o cputime= -p $p) at 6.5 s"
  sleep 1.5
  check "--$st draws, key wakes it" 'grep -q "scene: $st" $log && grep -q "lit_tiles=[1-9]" $log && grep -q wake $log'
  pkill -x Nightfall; sleep 0.5
done
# leave it running normally (no debug env)
pkill -x Nightfall; sleep 0.5; open "$app"; sleep 1
check "running normally after test" 'running'
echo "---- self-test log"; cat $out/selftest.log; echo "---- launch log"; cat $out/launch.log; echo "---- rain log"; cat $out/rain.log; echo "---- mosaic log"; cat $out/mosaic.log
echo "RESULT: $ok passed, $fail failed"

#!/usr/bin/env bash
here="$(cd "$(dirname "$0")" && pwd)"; . "$here/env.sh"
out=/tmp/nf-click; rm -rf $out; mkdir -p $out
NIGHTFALL_DEBUG=$out ~/.local/bin/nightfall 2>$out/stderr & sleep 3
pgrep -x nightfall >/dev/null && echo running
sudo python3 "$here/input.py" ${@:-click}; sleep 1
pgrep -x nightfall >/dev/null && echo "still running" || echo exited
cat $out/nightfall.log $out/stderr; pkill -x nightfall

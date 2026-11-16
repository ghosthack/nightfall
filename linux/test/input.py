#!/usr/bin/env python3
"""Inject real input through a virtual uinput device (run with sudo).
usage: input.py hotkey|key|esc|click|move [...]  (several steps run in order)"""
import sys, time
from evdev import UInput, ecodes as e
kbd = UInput({e.EV_KEY: [e.KEY_LEFTCTRL, e.KEY_LEFTALT, e.KEY_B, e.KEY_SPACE, e.KEY_ESC]},
             name='nightfall-test-keyboard')
mouse = UInput({e.EV_KEY: [e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE],
                e.EV_REL: [e.REL_X, e.REL_Y, e.REL_WHEEL]}, name='nightfall-test-mouse')
time.sleep(1.0)  # let libinput pick up the new devices
def tap(*keys, ui=kbd):
    for k in keys: ui.write(e.EV_KEY, k, 1); ui.syn(); time.sleep(0.03)
    for k in reversed(keys): ui.write(e.EV_KEY, k, 0); ui.syn(); time.sleep(0.03)
for step in sys.argv[1:]:
    if step == 'hotkey': tap(e.KEY_LEFTCTRL, e.KEY_LEFTALT, e.KEY_B)
    elif step == 'key':  tap(e.KEY_SPACE)
    elif step == 'esc':  tap(e.KEY_ESC)
    elif step == 'click': tap(e.BTN_LEFT, ui=mouse)
    elif step == 'move':
        for i in range(30):
            mouse.write(e.EV_REL, e.REL_X, 15 if i < 15 else -15); mouse.write(e.EV_REL, e.REL_Y, 5); mouse.syn(); time.sleep(0.02)
    time.sleep(0.3)
time.sleep(0.3)
kbd.close(); mouse.close()

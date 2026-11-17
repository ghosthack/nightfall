# Nightfall

A tiny screen blanker for Windows, Linux (GNOME) and macOS that you start on demand.

- Every display goes black, the cursor hides, and you get one of three styles:
  - **Clock** (default): a dim clock jumps to a random spot every 20 seconds so nothing burns in.
  - **Digital rain**: dim green columns of falling glyphs. Only the cells that change are
    redrawn, 15 times a second.
  - **Gray mosaic**: each display becomes a grid of rectangles (the largest size up to 50 px
    that fits the resolution exactly, e.g. 48x45 cells of 40 px on 1920x1800) in dark grays
    that slowly crossfade, a few at a time, to new shades.
- **Wake:** any key or mouse click. Moving the mouse alone does not wake it.
- No background cost worth mentioning, no network access, no data collected.

| | Start it | Pick the style | Built with | Memory while blanked |
|---|---|---|---|---|
| **Windows 10/11** | tray icon, **Ctrl+Alt+B**, or the desktop shortcut | tray menu, or `/rain` `/mosaic` `/clock` | C / Win32 | ~10 MB (tray idle ~1 MB) |
| **Linux (GNOME, Wayland or X11)** | dock / app grid, or **Ctrl+Alt+B** | right-click the dock icon, `--rain` `--mosaic` `--clock`, or `nightfall --set-mode rain` | C / GTK4 | ~65 MB, nothing resident when idle |
| **macOS 13+** | menu bar moon, **Ctrl+Option+B**, or open the app again | menu bar menu, or `--rain` `--mosaic` `--clock` | Swift / AppKit | ~100 MB (menu bar idle ~45 MB) |

CPU with the clock is effectively zero. The rain and the mosaic measured about 1–2% of one
core on Windows and on Apple-silicon Macs (plus a little in the window server), and 4–11% on
Linux, where GTK has more overhead per frame.

## Build and install

Each platform folder has its own `Makefile`, `install` and `uninstall` scripts, and an
end-to-end test.

**Windows** (cross-compiled on macOS or Linux with mingw-w64):

    make -C windows                  # -> windows/build/nightfall.exe
    # on the Windows machine:
    powershell -ExecutionPolicy Bypass -File windows\install.ps1

Installs per user under `%LOCALAPPDATA%\Programs\Nightfall`, adds Desktop and Start-menu
shortcuts, and starts the tray icon with Windows (toggle from its menu).

**Linux** (Ubuntu 24.04+ / GNOME):

    sudo apt install build-essential pkg-config libgtk-4-dev
    make -C linux && linux/install.sh

Installs to `~/.local/bin`, adds the app-grid entry, pins it to the dock and registers the
Ctrl+Alt+B GNOME shortcut (`--no-dock`, `--no-shortcut` to skip).

**macOS:**

    make -C macos && macos/install.sh      # -> ~/Applications/Nightfall.app

## Deploying to several machines

`deploy.sh` builds and installs over SSH, detecting each host's OS:

    ./deploy.sh --test my-pc my-laptop local
    ./deploy.sh --all --test                # hosts from ./deploy.hosts (gitignored)

`--test` runs the platform's end-to-end suite after installing (it blanks that screen for
about 40 seconds). Linux hosts build from source; `--install-deps` installs GTK4 headers
with apt.

## Tests

- **Windows** `windows/test/run-e2e.ps1`: runs inside the desktop session through a one-shot
  scheduled task, with real input and real screenshots.
- **Linux** `linux/test/e2e.sh`: real input through virtual uinput devices (needs sudo and
  `python3-evdev`); state checked from the app's `NIGHTFALL_DEBUG` log and self-rendered frames.
- **macOS** `macos/test/e2e.sh`: posting real input needs Accessibility permission, so input goes
  through the app's own event path via test hooks; state checked from its debug log and frames.

## License

MIT

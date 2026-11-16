# source me from SSH to talk to the logged-in GNOME session
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
export WAYLAND_DISPLAY=wayland-0
export XDG_SESSION_TYPE=wayland

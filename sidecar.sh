#!/usr/bin/env bash
# The PSP as a USB display beside PSPLink: load the driver, place the monitor.
#
#   ./sidecar.sh start    load psp/sidecar.prx through pspsh; once Linux's gud
#                         driver has the display, put it at a corner of the
#                         first other monitor (POSITION) and give it workspace
#                         WORKSPACE, that one alone: what else is on it goes
#                         to the main monitor (Hyprland, at runtime; no config
#                         file is written)
#   ./sidecar.sh place    place the monitor again, after POSITION, SCALE or
#                         WORKSPACE changed
#   ./sidecar.sh stop     take the driver off the bus and unload it
#   ./sidecar.sh status   one line: what is loaded, bound and shown
#
# Exits 0 when done; 1 PSPLink does not answer; 2 the driver did not get onto
# the bus; 3 gud did not bind (the udev rule in linux/ is missing); 4 the
# kernel has the display but Hyprland could not use it directly or via a stream. Without Hyprland it stops after
# the bind and leaves the placement to the desktop.
#
# The PRX is copied into the directory usbhostfs_pc serves as host0:.
# Env: PSPSH (default: pspsh in PATH), WORKSPACE=9, BOOT_WAIT=30,
#      POSITION=bottom-right (or bottom-left, top-right, top-left, above, below),
#      SCALE=1 (Hyprland's monitor scale, 1 or below for more room: 480x272
#      divides by 0.8, 0.5 and 0.25 too; 0.75 would not, Hyprland makes it 0.8).

set -Eeuo pipefail
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
PSPSH=${PSPSH:-$(command -v pspsh || echo "$HOME/.local/opt/pspdev/bin/pspsh")}
WORKSPACE=${WORKSPACE:-9}
POSITION=${POSITION:-bottom-right}
SCALE=${SCALE:-1}
BOOT_WAIT=${BOOT_WAIT:-30}
PRX=$HERE/psp/sidecar.prx
virtual_output=PSP-Sidecar
stream_unit=pspkit-sidecar-stream.service

psp()      { timeout 20 "$PSPSH" -e "$1" 2>/dev/null | tr -d '\r'; }
shell_up() { psp ver | grep -q PSPLink; }
loaded()   { psp modlist | grep -q 'Name: sidecar$'; }

wait_shell() {
  local i
  for ((i = 0; i < BOOT_WAIT; i++)); do shell_up && return 0; sleep 1; done
  return 1
}

# The display's USB interface: class 0xfe on a Sony device. Prints its sysfs path.
interface() {
  local i
  for i in /sys/bus/usb/devices/*:*; do
    [ "$(cat "${i%:*}/idVendor" 2>/dev/null)" = 054c ] || continue
    [ "$(cat "$i/bInterfaceClass" 2>/dev/null)" = fe ] && { echo "$i"; return 0; }
  done
  return 1
}

bound() { [ "$(basename "$(readlink "$1/driver" 2>/dev/null)" 2>/dev/null)" = gud ]; }

# The DRM connector of the gud device, as the compositor names it (USB-1).
connector() {
  local c
  for c in /sys/class/drm/card*-*; do
    [ "$(basename "$(readlink "${c%-*-*}/device/driver" 2>/dev/null)" 2>/dev/null)" = gud ] || continue
    c=${c##*/}; echo "${c#card*-}"; return 0
  done
  return 1
}

hypr() {
  [ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] ||
    export HYPRLAND_INSTANCE_SIGNATURE=$(ls -t "${XDG_RUNTIME_DIR:-/nonexistent}/hypr" 2>/dev/null | head -1)
  [ -n "$HYPRLAND_INSTANCE_SIGNATURE" ] && command -v hyprctl >/dev/null
}

# Where the display goes, beside the main monitor (the first other one): its
# corner POSITION, outside it. The main monitor is pinned where it is, at 0x0
# if it is the only one: Hyprland lays out "auto" monitors to the right of
# every placed one, so it would move away from the display it is placed next to.
place() {   # place <monitor name>
  local name=$1 rules
  rules=$(hyprctl -j monitors | python3 -c '
import json, sys
name, corner, scale = sys.argv[1], sys.argv[2], float(sys.argv[3])
dw, dh = round(480 / scale), round(272 / scale)   # the display in layout units
others = sorted((m for m in json.load(sys.stdin) if m["name"] != name), key=lambda m: m["id"])
if not others:
    print("0x0"); sys.exit()
m = others[0]
x, y = (0, 0) if len(others) == 1 else (m["x"], m["y"])
w, h = round(m["width"] / m["scale"]), round(m["height"] / m["scale"])
if m["transform"] % 2:
    w, h = h, w
edge, _, offset = corner.partition("-")
if edge in ("top", "bottom", "left", "right") and offset.isdigit():
    fraction = min(int(offset), 100) / 100
    if edge in ("top", "bottom"):
        px = x + round((w - dw) * fraction)
        py = y - dh if edge == "top" else y + h
    else:
        px = x - dw if edge == "left" else x + w
        py = y + round((h - dh) * fraction)
elif corner in ("above", "below"):
    px = x + (w - dw) // 2
    py = y - dh if corner == "above" else y + h
else:
    px = x + w if corner.endswith("right") else x - dw
    py = y if corner.startswith("top") else y + h - dh
print("%dx%d" % (px, py))
print("hl.monitor({ output = \"%s\", mode = \"%dx%d@%.2f\", position = \"%dx%d\", scale = %s, transform = %d })"
      % (m["name"], m["width"], m["height"], m["refreshRate"], x, y, m["scale"], m["transform"]))' "$name" "$POSITION" "$SCALE")
  [ -z "$(sed -n 2p <<<"$rules")" ] || hyprctl -q eval "$(sed -n 2p <<<"$rules")"
  hyprctl -q eval "hl.monitor({ output = \"$name\", mode = \"480x272\", position = \"$(head -1 <<<"$rules")\", scale = $SCALE })"
  hyprctl -q eval "hl.workspace_rule({ workspace = \"$WORKSPACE\", monitor = \"$name\", default = true, persistent = true })"
  hyprctl -q eval "hl.dispatch(hl.dsp.workspace.move({ workspace = \"$WORKSPACE\", monitor = \"$name\" }))"
  alone "$name"
  echo "monitor $name at $(head -1 <<<"$rules") ($POSITION, scale $SCALE), workspace $WORKSPACE"
}

# WORKSPACE alone on the display: every other workspace on it, and every other
# rule that sends one there (an earlier WORKSPACE), goes to the main monitor.
alone() {   # alone <monitor name>
  local name=$1 line
  { hyprctl -j monitors; hyprctl -j workspacerules; hyprctl -j workspaces; } | python3 -c '
import json, sys
name, keep = sys.argv[1], sys.argv[2]
text, at, dec = sys.stdin.read(), 0, json.JSONDecoder()
monitors, at = dec.raw_decode(text, at)
rules, at = dec.raw_decode(text, at + len(text[at:]) - len(text[at:].lstrip()))
spaces, at = dec.raw_decode(text, at + len(text[at:]) - len(text[at:].lstrip()))
others = sorted((m for m in monitors if m["name"] != name), key=lambda m: m["id"])
if not others:
    sys.exit()
main = others[0]["name"]
for r in rules:
    if r.get("monitor") == name and r["workspaceString"] != keep:
        print("hl.workspace_rule({ workspace = \"%s\", monitor = \"%s\", default = false, persistent = false })"
              % (r["workspaceString"], main))
for w in spaces:
    if w["monitor"] == name and str(w["id"]) != keep and w["id"] > 0:
        print("hl.dispatch(hl.dsp.workspace.move({ workspace = \"%d\", monitor = \"%s\" }))" % (w["id"], main))
' "$name" "$WORKSPACE" | while IFS= read -r line; do hyprctl -q eval "$line"; done
}

start() {
  local host0 intf name i
  shell_up || { echo "PSPLink does not answer (usbhostfs_pc running? PSP in PSPLink?)" >&2; exit 1; }
  if ! loaded; then
    [ -f "$PRX" ] || { echo "$PRX is missing: run make in psp/" >&2; exit 2; }
    # host0: is the working directory of whichever usbhostfs serves pspsh's port.
    host0=$(ss -Hltnp 'sport = :10000' | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2)
    host0=$(readlink "/proc/$host0/cwd")
    cp "$PRX" "$host0/sidecar.prx"
    psp "ldstart host0:/sidecar.prx" >/dev/null || true
    sleep 3   # the cable drops once while the bus restarts with both drivers
    wait_shell || { echo "PSPLink did not come back after loading the driver" >&2; exit 1; }
  fi
  intf=$(interface) || { echo "the driver is loaded but its interface is not on the bus" >&2; exit 2; }
  for ((i = 0; i < 10; i++)); do bound "$intf" && break; sleep 0.5; done
  bound "$intf" || {
    echo "Linux's gud driver did not take the display. Once, as root:" >&2
    echo "  sudo cp $HERE/linux/51-pspkit-sidecar.rules /etc/udev/rules.d/ && sudo udevadm control --reload" >&2
    echo "then ./sidecar.sh stop and start again." >&2
    exit 3
  }
  name=$(connector) || { echo "gud is bound but there is no DRM connector" >&2; exit 3; }
  if ! hypr; then echo "display $name is up; no Hyprland here, placement is the desktop's"; return 0; fi
  for ((i = 0; i < 10; i++)); do hyprctl monitors all | grep -q "^Monitor $name " && break; sleep 0.5; done
  if ! hyprctl monitors all | grep -q "^Monitor $name "; then
    if systemctl --user is-active --quiet "$stream_unit"; then
      place "$virtual_output"
      return
    fi
    local fb
    for fb in "$intf"/graphics/fb*; do
      [[ -f $fb/name && $(<"$fb/name") == guddrmfb ]] || continue
      fb=/dev/${fb##*/}
      [[ -w $fb ]] || { echo "No write access to $fb. Install the updated sidecar udev rule." >&2; return 4; }
      command -v wf-recorder >/dev/null || { echo "Install wf-recorder for the virtual display" >&2; return 4; }
      hyprctl output create headless "$virtual_output"
      if ! place "$virtual_output" || ! systemd-run --user --collect --unit="$stream_unit" \
        --setenv=WAYLAND_DISPLAY --setenv=HYPRLAND_INSTANCE_SIGNATURE \
        python3 "$HERE/linux/stream-display.py" "$fb" "$virtual_output"; then
        hyprctl output remove "$virtual_output"
        return 4
      fi
      return 0
    done
    echo "No PSP framebuffer for $name" >&2; return 4
  fi
  place "$name"
}

stop() {
  if systemctl --user is-active --quiet "$stream_unit"; then systemctl --user stop "$stream_unit"; fi
  shell_up || { echo "PSPLink does not answer" >&2; exit 1; }
  loaded || { echo "not loaded"; return 0; }
  psp "modstun @sidecar" >/dev/null || true   # the cable drops, pspsh may not get its answer
  sleep 3
  wait_shell || { echo "PSPLink did not come back after unloading" >&2; exit 1; }
  if loaded; then echo "still loaded" >&2; exit 2; fi
  echo "unloaded"
}

status() {
  local intf name=""
  shell_up || { echo "PSPLink does not answer"; return 0; }
  loaded || { echo "driver not loaded"; return 0; }
  intf=$(interface) || { echo "driver loaded, not on the bus"; return 0; }
  bound "$intf" || { echo "driver on the bus, gud not bound"; return 0; }
  name=$(connector) || true
  if systemctl --user is-active --quiet "$stream_unit"; then echo "display $name via $virtual_output"; return; fi
  if hypr && hyprctl monitors all | grep -q "^Monitor $name "; then echo "display $name, shown by Hyprland"
  else echo "display $name, bound by gud"; fi
}

case "${1:-}" in
  start)  start ;;
  stop)   stop ;;
  status) status ;;
  place)  name=$(connector) && hypr || { echo "no display to place" >&2; exit 3; }
          if systemctl --user is-active --quiet "$stream_unit"; then name=$virtual_output; fi
          place "$name" ;;
  *)      sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 64 ;;
esac

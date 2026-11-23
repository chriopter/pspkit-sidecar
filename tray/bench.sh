#!/usr/bin/env bash
# What the bar widget and the tray icon run: power the PSP, bring the
# display up or down.
#
#   bench.sh state   prints one word, from sysfs alone (no network, no pspsh):
#                    off      no PSP on USB
#                    psplink  PSP in PSPLink, display driver not on the bus
#                    loaded   display interface on the bus, Linux has not taken it
#                    display  Linux's gud driver has the display
#   bench.sh on      unless PSPLink answers already: a power cycle (the PSP
#                    boots only when power comes back after OFF_S without it:
#                    relay off unless it has been off that long, relay on,
#                    wait for PSPLink), then ../sidecar.sh start
#   bench.sh off     ../sidecar.sh stop, PSPLink poweroff, wait for USB to go;
#                    the relay stays on
#   bench.sh start | stop   ../sidecar.sh start | stop, the PSP stays as it is
#   bench.sh place   ../sidecar.sh place, after POSITION changed
#   bench.sh relay on|off|status   the relay alone: no power cycle, no clean
#                    poweroff; status prints on or off
#   bench.sh position [bottom-right|bottom-left|top-right|top-left|above|below]
#                    prints POSITION, or keeps a new one in the config and
#                    moves a display that is up
#   bench.sh scale [1|0.8|0.5|0.25]   the same for SCALE
#   bench.sh workspace [1..10]        the same for WORKSPACE, the one Hyprland
#                    workspace on the display
#   bench.sh host    start usbhostfs_pc in HOST0 unless something already serves
#                    pspsh's port 10000, any usbhostfs (on and start do this
#                    first); it stays up after this script and the tray end
#
# The relay is pspkit-autoboot's (autoboot.sh relay on|off, its own config).
# on, off and relay print each step on stdout as it begins, one line each, for
# the bar widget to show. Exits 0 when done, sidecar.sh's code if the display
# did not come up (the PSP stays on in PSPLink then), 1 if PSPLink never
# answered (on: within BOOT_MAX; off: at all, the PSP is left as it is), 2
# without autoboot.sh or when the relay did not switch.
#
# Config: ~/.config/pspkit-sidecar.env (or $PSPKIT_SIDECAR_ENV), optional:
#   HOST0=/path/served/as/host0     default ~/psp-host0, created if missing
#   PSPKIT_AUTOBOOT=/path/to/autoboot.sh   defaults to the sibling pspkit-autoboot checkout
#   POSITION=bottom-right   position relative to the main monitor, for sidecar.sh (the widget sets it)
#   SCALE=1                 the display's scale, for sidecar.sh (the widget sets it)
#   WORKSPACE=9             the Hyprland workspace on the display (the widget sets it)
# Env: PSPSH (default: pspsh in PATH), BOOT_MAX=90, OFF_S=15.

set -Eeuo pipefail
export LC_ALL=C
HERE=$(dirname "$(readlink -f "$0")")
SIDECAR=$HERE/../sidecar.sh
CONF=${PSPKIT_SIDECAR_ENV:-$HOME/.config/pspkit-sidecar.env}
# shellcheck source=/dev/null
[ -f "$CONF" ] && . "$CONF"
HOST0=${HOST0:-$HOME/psp-host0}
AUTOBOOT=${PSPKIT_AUTOBOOT:-$HERE/../../pspkit-autoboot/autoboot.sh}
if [[ -z ${PSPKIT_AUTOBOOT:-} && ! -x $AUTOBOOT && -x $HOME/git/pspkit-autoboot/autoboot.sh ]]; then
  AUTOBOOT=$HOME/git/pspkit-autoboot/autoboot.sh
fi
PSPSH=${PSPSH:-$(command -v pspsh || echo "$HOME/.local/opt/pspdev/bin/pspsh")}
BOOT_MAX=${BOOT_MAX:-90}
OFF_S=${OFF_S:-15}
STATE_DIR=${XDG_STATE_HOME:-$HOME/.local/state}/pspkit-sidecar
OFF_SINCE=$STATE_DIR/relay-off   # its mtime: when this script last cut the power
export POSITION=${POSITION:-bottom-right} SCALE=${SCALE:-1} WORKSPACE=${WORKSPACE:-9}

state() {
  local d i s=off
  for d in /sys/bus/usb/devices/*; do
    [ -f "$d/idVendor" ] || continue
    case "$(cat "$d/idVendor"):$(cat "$d/idProduct")" in
      1d50:614d) echo display; return ;;
      054c:01c9) s=psplink
        for i in "$d":*; do
          [ "$(cat "$i/bInterfaceClass" 2>/dev/null)" = fe ] || continue
          s=loaded
          [ "$(basename "$(readlink "$i/driver" 2>/dev/null)" 2>/dev/null)" = gud ] && s=display
        done ;;
    esac
  done
  echo "$s"
}

# Something listens on pspsh's port: usbhostfs_pc, or another usbhostfs build.
serving() { [ -n "$(ss -Hltn 'sport = :10000' 2>/dev/null)" ]; }

host() {
  serving && return 0
  local bin pid i
  bin=$(command -v usbhostfs_pc || echo "$(dirname "$PSPSH")/usbhostfs_pc")
  [ -x "$bin" ] || { echo "no usbhostfs_pc in PATH or next to pspsh" >&2; exit 2; }
  mkdir -p "$HOST0" "$STATE_DIR"
  pid=$(cd "$HOST0" && { setsid nohup "$bin" >>"$STATE_DIR/usbhostfs.log" 2>&1 </dev/null & echo $!; })
  for ((i = 0; i < 10; i++)); do serving && return 0; sleep 0.2; done
  # Without the port it would still hold the PSP's USB interface from the one that has it.
  kill "$pid" 2>/dev/null
  echo "usbhostfs_pc did not get port 10000, see $STATE_DIR/usbhostfs.log" >&2; exit 2
}

shell_up() { timeout 10 "$PSPSH" -e ver 2>/dev/null | grep -q PSPLink; }
say() { echo "$*"; }

need_autoboot() { [ -x "$AUTOBOOT" ] || { echo "no autoboot.sh at $AUTOBOOT" >&2; exit 2; }; }
relay_status() { "$AUTOBOOT" relay status 2>/dev/null | tail -1; }
relay_set() {
  "$AUTOBOOT" relay "$1" >/dev/null || { echo "the relay did not switch $1" >&2; exit 2; }
  mkdir -p "$STATE_DIR"
  if [ "$1" = off ]; then touch "$OFF_SINCE"; else rm -f "$OFF_SINCE"; fi
}

# keep NAME VALUE: one line NAME=VALUE in the config, the rest as it was;
# a display that is up is placed again.
keep() {
  mkdir -p "$(dirname "$CONF")"; touch "$CONF"
  { grep -v "^$1=" "$CONF" || true; echo "$1=$2"; } >"$CONF.new" && mv "$CONF.new" "$CONF"
  export "$1=$2"
  if [ "$(state)" = display ]; then "$SIDECAR" place; fi
}

# Seconds since this script cut the power; large when it did not.
off_for() { if [ -f "$OFF_SINCE" ]; then echo $(($(date +%s) - $(stat -c %Y "$OFF_SINCE"))); else echo 999999; fi; }

# Power off long enough, then on: the only way the PSP boots by itself.
power_cycle() {
  local left
  if [ "$(relay_status)" = on ] || [ ! -f "$OFF_SINCE" ]; then
    say "Relay off: the PSP boots only after ${OFF_S} s without power"
    relay_set off
  fi
  left=$((OFF_S - $(off_for)))
  for ((; left > 0; left--)); do say "Without power, ${left} s left"; sleep 1; done
  say "Relay on, the PSP is starting"
  relay_set on
  SECONDS=0
  while ((SECONDS < BOOT_MAX)); do
    shell_up && return 0
    say "Waiting for PSPLink, ${SECONDS} s"
    sleep 1
  done
  echo "PSPLink did not answer within ${BOOT_MAX} s" >&2; exit 1
}

case "${1:-}" in
  state) state ;;
  on)
    need_autoboot
    say "Starting usbhostfs"; host
    # Just cut, a PSP can still answer for a moment on what is left in it.
    if [ -f "$OFF_SINCE" ] && (($(off_for) < OFF_S)); then
      power_cycle
    else
      say "Asking PSPLink"
      shell_up || power_cycle
    fi
    say "Loading the display"
    "$SIDECAR" start ;;
  off)
    [ "$(state)" = off ] && { say "PSP off"; exit 0; }
    say "Starting usbhostfs"; host   # without it PSPLink cannot be asked to power off
    say "Asking PSPLink"
    shell_up || { echo "PSPLink does not answer; Relay off cuts the power" >&2; exit 1; }
    say "Display off"
    "$SIDECAR" stop >/dev/null || true
    say "The PSP is shutting down"
    timeout 20 "$PSPSH" -e poweroff >/dev/null 2>&1 || true
    for ((i = 0; i < 100; i++)); do [ "$(state)" = off ] && break; sleep 0.2; done
    say "PSP off, the relay stays on" ;;
  host)  host ;;
  start) host; "$SIDECAR" start ;;
  stop)  "$SIDECAR" stop ;;
  place) "$SIDECAR" place ;;
  position)
    case "${2:-}" in
      "") echo "$POSITION" ;;
      bottom-right | bottom-left | top-right | top-left | above | below) keep POSITION "$2" ;;
      *) [[ $2 =~ ^(top|bottom|left|right)-(100|[1-9]?[0-9])$ ]] || {
        echo "Choose a preset or an edge and offset (e.g. bottom-50)" >&2; exit 64
      }
      keep POSITION "$2" ;;
    esac ;;
  scale)
    case "${2:-}" in
      "") echo "$SCALE" ;;
      1 | 0.8 | 0.5 | 0.25) keep SCALE "$2" ;;
      *) echo "scale 1|0.8|0.5|0.25" >&2; exit 64 ;;
    esac ;;
  workspace)
    case "${2:-}" in
      "") echo "$WORKSPACE" ;;
      [1-9] | 10) keep WORKSPACE "$2" ;;
      *) echo "workspace 1..10" >&2; exit 64 ;;
    esac ;;
  relay)
    need_autoboot
    case "${2:-}" in
      status) relay_status ;;
      off) say "Relay off"; relay_set off; say "Relay off: PSP on boots it once it has been off ${OFF_S} s" ;;
      on)  say "Relay on"; relay_set on; say "Relay on: the PSP boots only after ${OFF_S} s without power, PSP on does that" ;;
      *) echo "relay on|off|status" >&2; exit 64 ;;
    esac ;;
  *) awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"; exit 64 ;;
esac

#!/usr/bin/env python3
"""PSP Sidecar in the system tray: turn the PSP with its display on and off.

A status-notifier icon whose menu opens on either click: the state, "PSP on"
(bench.sh on: usbhostfs, a power cycle through pspkit-autoboot's relay,
PSPLink, the display), "PSP off" (bench.sh off: the display, PSPLink's
poweroff; the relay stays on), "Relay on" / "Relay off" (the relay alone) and
"Position" (bench.sh position: the corner of the main monitor the display
goes to), "Scale" (bench.sh scale) and "Workspace" (bench.sh workspace: the
one Hyprland workspace on the display).

The item is a StatusNotifierItem of its own with ItemIsMenu set, so that
the bar opens the menu on a left click too; libayatana-appindicator leaves
that unset. Sleeps in the GLib main loop. Every 10 s it reads the PSP's USB
state from sysfs, in this process: no command is started, nothing is sent to
the PSP. Needs python-gobject and libdbusmenu-glib. Runs until killed.
"""
import glob
import os
import subprocess
import sys
import threading

import gi

gi.require_version("Dbusmenu", "0.4")
from gi.repository import Dbusmenu, Gio, GLib  # noqa: E402

BENCH = os.path.join(os.path.dirname(os.path.realpath(__file__)), "bench.sh")
TEXT = {
    "off": "PSP off",
    "psplink": "PSP on, display off",
    "loaded": "Driver loaded, Linux has not taken the display",
    "display": "Display on",
}
ICON = {"off": "input-gaming-symbolic", "psplink": "input-gaming-symbolic",
        "loaded": "input-gaming-symbolic", "display": "video-display-symbolic"}
POSITIONS = [("bottom-right", "Bottom right"), ("bottom-left", "Bottom left"),
             ("top-right", "Top right"), ("top-left", "Top left"),
             ("above", "Above"), ("below", "Below")]
SCALES = [("0.25", "25%"), ("0.5", "50%"), ("0.8", "80%"), ("1", "100%")]
WORKSPACES = [(str(n), str(n)) for n in range(1, 11)]

SNI = """<node><interface name="org.kde.StatusNotifierItem">
  <property name="Category" type="s" access="read"/>
  <property name="Id" type="s" access="read"/>
  <property name="Title" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="IconName" type="s" access="read"/>
  <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
  <property name="ItemIsMenu" type="b" access="read"/>
  <property name="Menu" type="o" access="read"/>
  <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
  <signal name="NewIcon"/>
  <signal name="NewToolTip"/>
</interface></node>"""
ITEM_PATH, MENU_PATH = "/StatusNotifierItem", "/MenuBar"
WATCHER = "org.kde.StatusNotifierWatcher"


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def usb_state():
    """As bench.sh state: off | psplink | loaded | display."""
    state = "off"
    for dev in glob.glob("/sys/bus/usb/devices/*/idVendor"):
        dev = os.path.dirname(dev)
        ids = read(dev + "/idVendor") + ":" + read(dev + "/idProduct")
        if ids == "1d50:614d":
            return "display"
        if ids != "054c:01c9":
            continue
        state = "psplink"
        for intf in glob.glob(dev + ":*"):
            if read(intf + "/bInterfaceClass") != "fe":
                continue
            driver = os.path.basename(os.path.realpath(intf + "/driver")) if os.path.exists(intf + "/driver") else ""
            state = "display" if driver == "gud" else "loaded"
    return state


def item(label="", activate=None, **props):
    mi = Dbusmenu.Menuitem.new()
    if label:
        mi.property_set(Dbusmenu.MENUITEM_PROP_LABEL, label)
    for key, value in props.items():
        (mi.property_set_int if isinstance(value, int) and not isinstance(value, bool)
         else mi.property_set_bool if isinstance(value, bool) else mi.property_set)(key.replace("_", "-"), value)
    if activate:
        mi.connect(Dbusmenu.MENUITEM_SIGNAL_ITEM_ACTIVATED, lambda *_: activate())
    return mi


def separator():
    return item(type="separator")


class Tray:
    def __init__(self):
        self.state, self.busy, self.error, self.shown = None, False, "", None
        self.position = subprocess.run([BENCH, "position"], capture_output=True, text=True).stdout.strip() or "bottom-right"
        self.scale = subprocess.run([BENCH, "scale"], capture_output=True, text=True).stdout.strip() or "1"
        self.workspace = subprocess.run([BENCH, "workspace"], capture_output=True, text=True).stdout.strip() or "9"
        self.status = item(enabled=False)
        self.on = item("PSP on", activate=lambda: self.run("on"))
        self.off = item("PSP off", activate=lambda: self.run("off"))
        self.relay = [item("Relay on", activate=lambda: self.run("relay", "on")),
                      item("Relay off", activate=lambda: self.run("relay", "off"))]
        self.places, self.scales, self.workspaces = {}, {}, {}
        place = item("Position", children_display="submenu")
        for key, label in POSITIONS:
            self.places[key] = item(label, activate=lambda key=key: self.place(key), toggle_type="radio")
            place.child_append(self.places[key])
        scale = item("Scale", children_display="submenu")
        for key, label in SCALES:
            self.scales[key] = item(label, activate=lambda key=key: self.set_scale(key), toggle_type="radio")
            scale.child_append(self.scales[key])
        workspace = item("Workspace", children_display="submenu")
        for key, label in WORKSPACES:
            self.workspaces[key] = item(label, activate=lambda key=key: self.set_workspace(key), toggle_type="radio")
            workspace.child_append(self.workspaces[key])
        root = Dbusmenu.Menuitem.new()
        for mi in (self.status, separator(), self.on, self.off, separator(), *self.relay,
                   separator(), place, scale, workspace):
            root.child_append(mi)
        self.menu = Dbusmenu.Server.new(MENU_PATH)
        self.menu.set_root(root)

        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION)
        self.name = "org.kde.StatusNotifierItem-%d-1" % os.getpid()
        self.bus.register_object_with_closures2(ITEM_PATH, Gio.DBusNodeInfo.new_for_xml(SNI).interfaces[0],
                                                 self.on_call, self.on_property, None)
        Gio.bus_own_name_on_connection(self.bus, self.name, Gio.BusNameOwnerFlags.NONE, None, None)
        # The bar's watcher comes and goes with the shell: register with each one.
        Gio.bus_watch_name_on_connection(self.bus, WATCHER, Gio.BusNameWatcherFlags.NONE,
                                         lambda *_: self.register(), None)
        self.refresh()
        GLib.timeout_add_seconds(10, self.refresh)

    def register(self):
        self.bus.call(WATCHER, "/StatusNotifierWatcher", WATCHER, "RegisterStatusNotifierItem",
                      GLib.Variant("(s)", (self.name,)), None, Gio.DBusCallFlags.NONE, -1, None, None)

    def on_call(self, conn, sender, path, iface, method, params, invocation):
        invocation.return_value(None)   # ItemIsMenu: the bar opens the menu itself

    def on_property(self, conn, sender, path, iface, prop):
        state = self.state or "off"
        return {
            "Category": GLib.Variant("s", "Hardware"),
            "Id": GLib.Variant("s", "pspkit-sidecar"),
            "Title": GLib.Variant("s", "PSP Sidecar"),
            "Status": GLib.Variant("s", "Active"),
            "IconName": GLib.Variant("s", ICON[state]),
            "ToolTip": GLib.Variant("(sa(iiay)ss)", (ICON[state], [], "PSP Sidecar", self.label())),
            "ItemIsMenu": GLib.Variant("b", True),
            "Menu": GLib.Variant("o", MENU_PATH),
        }.get(prop)

    def label(self):
        return "Working…" if self.busy else self.error or TEXT[self.state or "off"]

    def refresh(self):
        state = usb_state()
        if (state, self.busy, self.error, self.position, self.scale, self.workspace) != self.shown:
            icon_changed = self.shown is None or ICON[state] != ICON[self.shown[0]]
            self.shown = (state, self.busy, self.error, self.position, self.scale, self.workspace)
            self.state = state
            self.status.property_set(Dbusmenu.MENUITEM_PROP_LABEL, self.label())
            # Half way (PSP on, no display) offers both: on finishes, off undoes.
            self.on.property_set_bool(Dbusmenu.MENUITEM_PROP_VISIBLE, state != "display")
            self.off.property_set_bool(Dbusmenu.MENUITEM_PROP_VISIBLE, state != "off")
            self.on.property_set_bool(Dbusmenu.MENUITEM_PROP_ENABLED, not self.busy)
            self.off.property_set_bool(Dbusmenu.MENUITEM_PROP_ENABLED, not self.busy)
            for mi in self.relay:   # its state is the plug's, not polled: both are always there
                mi.property_set_bool(Dbusmenu.MENUITEM_PROP_ENABLED, not self.busy)
            for chosen, items in ((self.position, self.places), (self.scale, self.scales),
                                  (self.workspace, self.workspaces)):
                for key, mi in items.items():
                    mi.property_set_int(Dbusmenu.MENUITEM_PROP_TOGGLE_STATE,
                                        Dbusmenu.MENUITEM_TOGGLE_STATE_CHECKED if key == chosen
                                        else Dbusmenu.MENUITEM_TOGGLE_STATE_UNCHECKED)
            if icon_changed:
                self.bus.emit_signal(None, ITEM_PATH, "org.kde.StatusNotifierItem", "NewIcon", None)
            self.bus.emit_signal(None, ITEM_PATH, "org.kde.StatusNotifierItem", "NewToolTip", None)
        return True

    def place(self, position):
        self.position = position
        self.run("position", position)

    def set_scale(self, scale):
        self.scale = scale
        self.run("scale", scale)

    def set_workspace(self, workspace):
        self.workspace = workspace
        self.run("workspace", workspace)

    def run(self, *what):
        if self.busy:
            return
        self.busy, self.error = True, ""
        self.refresh()

        def work():
            r = subprocess.run([BENCH, *what], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            lines = r.stderr.strip().splitlines()
            if r.returncode:   # the whole message for the journal, the first line for the menu
                print("bench.sh %s: exit %d\n%s" % (" ".join(what), r.returncode, r.stderr.strip()), file=sys.stderr, flush=True)
            GLib.idle_add(self.done, lines[0] if r.returncode and lines else "")

        threading.Thread(target=work, daemon=True).start()

    def done(self, error):
        self.busy, self.error = False, error
        self.refresh()
        return False


if __name__ == "__main__":
    Tray()
    GLib.MainLoop().run()

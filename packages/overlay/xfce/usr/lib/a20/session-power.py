#!/usr/bin/env python3
"""Expose XFCE power actions through the upstream OpenRC lifecycle.

The D-Bus adapter only translates desktop actions. OpenRC PID1 runs the
shutdown level before its final process cleanup and checked power call.
"""
import os
import subprocess
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

BUS_NAME = "org.xfce.SessionManager"
OBJECT_PATH = "/org/xfce/SessionManager"
INTERFACE = "org.xfce.Session.Manager"

INIT_RESTART = "reboot"
INIT_POWER_OFF = "poweroff"

XML = """<node>
  <interface name="org.xfce.Session.Manager">
    <method name="CanShutdown"><arg type="b" direction="out"/></method>
    <method name="CanRestart"><arg type="b" direction="out"/></method>
    <method name="CanSuspend"><arg type="b" direction="out"/></method>
    <method name="CanHibernate"><arg type="b" direction="out"/></method>
    <method name="CanHybridSleep"><arg type="b" direction="out"/></method>
    <method name="Shutdown"><arg type="b" direction="in"/></method>
    <method name="Restart"><arg type="b" direction="in"/></method>
    <method name="Logout">
      <arg type="b" direction="in"/>
      <arg type="b" direction="in"/>
    </method>
  </interface>
</node>"""


def log(message):
    print("session-power: " + message, flush=True)


def power_action(action):
    """Ask the upstream init to perform an orderly system transition."""
    if action not in (INIT_RESTART, INIT_POWER_OFF):
        raise ValueError("unsupported power action")
    try:
        result = subprocess.run(
            ["/sbin/" + action],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=15, check=False)
        if result.returncode == 0:
            log("init accepted " + action)
            return GLib.SOURCE_REMOVE
        reason = (result.stdout or "").strip() or "exit status %d" % result.returncode
    except (OSError, subprocess.TimeoutExpired) as error:
        reason = str(error)
    log("init power request failed: " + reason)
    try:
        gi.require_version("Gtk", "3.0")
        from gi.repository import Gtk
        dialog = Gtk.MessageDialog(
            message_type=Gtk.MessageType.ERROR, buttons=Gtk.ButtonsType.CLOSE,
            text="无法提交关机或重启请求")
        dialog.format_secondary_text(reason)
        dialog.connect("response", lambda widget, response: widget.destroy())
        dialog.show_all()
    except Exception as error:
        log("cannot display power error: " + type(error).__name__)
    return GLib.SOURCE_REMOVE


def logout():
    """Terminate the compositor; init restarts the session."""
    log("logout requested, restarting the desktop through OpenRC")
    subprocess.Popen(["/sbin/rc-service", "a20-desktop", "restart"],
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.STDOUT, start_new_session=True)
    return GLib.SOURCE_REMOVE


class Manager:
    def __init__(self):
        self.node = Gio.DBusNodeInfo.new_for_xml(XML)
        self._iface = self.node.interfaces[0]

    def on_call(self, connection, sender, path, iface, method, params, invocation):
        if method in ("CanShutdown", "CanRestart"):
            invocation.return_value(GLib.Variant("(b)", (True,)))
        elif method.startswith("Can"):
            invocation.return_value(GLib.Variant("(b)", (False,)))
        elif method == "Shutdown":
            log("shutdown requested")
            invocation.return_value(None)
            GLib.idle_add(power_action, INIT_POWER_OFF)
        elif method == "Restart":
            log("restart requested")
            invocation.return_value(None)
            GLib.idle_add(power_action, INIT_RESTART)
        elif method == "Logout":
            invocation.return_value(None)
            GLib.idle_add(logout)
        else:
            invocation.return_dbus_error(
                "org.freedesktop.DBus.Error.UnknownMethod", method)

    def on_bus_acquired(self, connection, name):
        # register_object_with_closures has no stable spelling across the
        # PyGObject versions in the worlds; the plain API is the portable one.
        connection.register_object(OBJECT_PATH, self._iface, self.on_call,
                                   None, None)
        log("owning " + BUS_NAME)

    def on_name_lost(self, connection, name):
        log("lost " + BUS_NAME)


def main():
    manager = Manager()
    Gio.bus_own_name(Gio.BusType.SESSION, BUS_NAME,
                     Gio.BusNameOwnerFlags.NONE,
                     manager.on_bus_acquired, None, manager.on_name_lost)
    # PyGObject's MainLoop override installs a signal wakeup fd, which is not
    # usable here; iterate the default context directly instead.
    context = GLib.MainContext.default()
    while True:
        context.iteration(True)


if __name__ == "__main__":
    main()

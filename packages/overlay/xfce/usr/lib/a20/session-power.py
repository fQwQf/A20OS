#!/usr/bin/env python3
"""org.xfce.SessionManager provider for the A20OS desktop.

The XFCE actions plugin asks a session manager two things: whether it can
shut down or restart, and then to perform it.  A20OS runs labwc without
elogind/systemd, so nothing owns org.xfce.SessionManager; the plugin's
fallback (`shutdown --poweroff`) does not exist either, which made every
power entry a silent no-op.

This service owns the name inside the session bus and maps the two actions
onto the kernel's orderly reboot path (vfs_system_shutdown, then firmware
power-off/reboot).  It must run before the panel starts so the capability
queries at menu build time see a provider.

Log Out terminates the compositor; init restarts the session, so the desktop
comes back instead of leaving a kiosk with no userspace.
"""
import ctypes
import os
import signal
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

BUS_NAME = "org.xfce.SessionManager"
OBJECT_PATH = "/org/xfce/SessionManager"
INTERFACE = "org.xfce.Session.Manager"

LINUX_REBOOT_CMD_RESTART = 0x01234567
LINUX_REBOOT_CMD_POWER_OFF = 0x4321FEDC

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


def power_action(cmd):
    """Take the orderly path: the kernel syncs and unmounts before poweroff.

    libc's reboot() is the one-argument musl/glibc wrapper; it supplies the
    Linux magic values itself.  The VFS shutdown transaction reports a
    retryable EBUSY when a file operation was in flight when it tried to close
    admission (see the shutdown state machine's RETRY_* contract), so a busy
    desktop needs a bounded retry rather than a one-shot failure.
    """
    import errno
    import time

    libc = ctypes.CDLL(None, use_errno=True)
    for attempt in range(20):
        # The kernel's checked shutdown transaction flushes filesystem data.
        # A global sync before every EBUSY retry repeats expensive USB writes
        # while admission is still open and other applications keep writing.
        libc.reboot(cmd)
        err = ctypes.get_errno()
        if err in (errno.EBUSY, errno.ETIMEDOUT) and attempt < 19:
            time.sleep(0.25)
            continue
        log("reboot(0x%x) failed: %s" % (cmd, os.strerror(err)))
        break
    # A refused shutdown has not completed. Keep the D-Bus provider alive so
    # the next request can retry, and show the reason instead of a silent no-op.
    try:
        gi.require_version("Gtk", "3.0")
        from gi.repository import Gtk
        dialog = Gtk.MessageDialog(
            message_type=Gtk.MessageType.ERROR, buttons=Gtk.ButtonsType.CLOSE,
            text="无法完成关机或重启")
        dialog.format_secondary_text("内核返回：%s。桌面仍在运行，可以稍后重试。" % os.strerror(err))
        dialog.connect("response", lambda widget, response: widget.destroy())
        dialog.show_all()
    except Exception as error:
        log("cannot display power error: " + type(error).__name__)
    return GLib.SOURCE_REMOVE


def logout():
    """Terminate the compositor; init restarts the session."""
    log("logout requested, stopping the compositor")
    os.kill(os.getppid(), signal.SIGTERM)


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
            GLib.idle_add(power_action, LINUX_REBOOT_CMD_POWER_OFF)
        elif method == "Restart":
            log("restart requested")
            invocation.return_value(None)
            GLib.idle_add(power_action, LINUX_REBOOT_CMD_RESTART)
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

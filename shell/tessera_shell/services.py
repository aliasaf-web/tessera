"""System status: sound, brightness, battery, Wi-Fi, Bluetooth, power.

Every service keeps its current state and calls its listeners when it
changes. Reading is done through D-Bus or small official command line
tools (wpctl, nmcli); nothing here needs root."""

import glob
import os
import re
import shutil
import subprocess

from gi.repository import Gio, GLib


class Service:
    def __init__(self):
        self._listeners = []

    def connect(self, callback):
        self._listeners.append(callback)
        callback()

    def disconnect(self, callback):
        if callback in self._listeners:
            self._listeners.remove(callback)

    def changed(self):
        for callback in list(self._listeners):
            try:
                callback()
            except Exception as exc:
                print(f"tessera-shell: listener error: {exc}")


def run(args, timeout=5):
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)
        return out.returncode, out.stdout
    except (OSError, subprocess.TimeoutExpired):
        return 1, ""


def run_async(args, done=None):
    """Run a command without blocking the interface."""
    try:
        proc = Gio.Subprocess.new(args, Gio.SubprocessFlags.STDOUT_PIPE |
                                  Gio.SubprocessFlags.STDERR_MERGE)
    except GLib.Error as exc:
        if done:
            done(False, str(exc.message))
        return

    def finished(p, result):
        try:
            _ok, out, _err = p.communicate_utf8_finish(result)
        except GLib.Error as exc:
            out = exc.message
        if done:
            done(p.get_successful(), out or "")

    proc.communicate_utf8_async(None, None, finished)


# --------------------------------------------------------------------------
# Sound (PipeWire)
# --------------------------------------------------------------------------

class Audio(Service):
    def __init__(self):
        super().__init__()
        self.available = shutil.which("wpctl") is not None
        self.volume = 0.0
        self.muted = False
        self._watch = None
        if self.available:
            self.refresh()
            self._start_watch()
            GLib.timeout_add_seconds(10, self._poll)

    def refresh(self):
        code, out = run(["wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"], timeout=2)
        if code != 0:
            return
        m = re.search(r"Volume:\s*([0-9.]+)", out)
        volume = float(m.group(1)) if m else 0.0
        muted = "MUTED" in out
        if abs(volume - self.volume) > 0.001 or muted != self.muted:
            self.volume, self.muted = volume, muted
            self.changed()

    def _poll(self):
        if self._watch is None:
            self.refresh()
        return True

    def _start_watch(self):
        """`pactl subscribe` tells us about changes made by other apps."""
        if not shutil.which("pactl"):
            return
        try:
            self._watch = Gio.Subprocess.new(["pactl", "subscribe"],
                                             Gio.SubprocessFlags.STDOUT_PIPE)
        except GLib.Error:
            self._watch = None
            return
        stream = Gio.DataInputStream.new(self._watch.get_stdout_pipe())

        def on_line(s, res):
            try:
                line, _ = s.read_line_finish_utf8(res)
            except GLib.Error:
                line = None
            if line is None:
                self._watch = None
                return
            if "sink" in line or "server" in line:
                GLib.idle_add(self._idle_refresh)
            s.read_line_async(GLib.PRIORITY_LOW, None, on_line)

        stream.read_line_async(GLib.PRIORITY_LOW, None, on_line)

    def _idle_refresh(self):
        self.refresh()
        return False

    def set_volume(self, value):
        value = max(0.0, min(1.0, value))
        run(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", f"{value:.2f}"], timeout=2)
        if self.muted and value > 0:
            run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "0"], timeout=2)
        self.refresh()

    def step(self, delta):
        self.set_volume(round((self.volume + delta) * 20) / 20)

    def toggle_mute(self):
        run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"], timeout=2)
        self.refresh()

    def toggle_mic(self):
        run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle"], timeout=2)

    def icon(self):
        if self.muted or self.volume <= 0.001:
            return "audio-volume-muted-symbolic"
        if self.volume < 0.34:
            return "audio-volume-low-symbolic"
        if self.volume < 0.67:
            return "audio-volume-medium-symbolic"
        return "audio-volume-high-symbolic"


# --------------------------------------------------------------------------
# Screen brightness (through logind, so no special permissions are needed)
# --------------------------------------------------------------------------

class Brightness(Service):
    def __init__(self):
        super().__init__()
        self.device = None
        self.max = 1
        self.value = 0
        devices = sorted(glob.glob("/sys/class/backlight/*"))
        # Prefer the laptop panel's own controller over generic ones.
        for preferred in ("intel_backlight", "amdgpu_bl", "acpi_video0"):
            for d in devices:
                if os.path.basename(d).startswith(preferred):
                    self.device = d
                    break
            if self.device:
                break
        if not self.device and devices:
            self.device = devices[0]
        self.available = self.device is not None
        self.proxy = None
        if self.available:
            try:
                self.max = int(open(os.path.join(self.device, "max_brightness")).read())
            except (OSError, ValueError):
                self.available = False
        if self.available:
            self.refresh()
            GLib.timeout_add_seconds(5, self._poll)

    def _poll(self):
        self.refresh()
        return True

    def refresh(self):
        try:
            value = int(open(os.path.join(self.device, "brightness")).read())
        except (OSError, ValueError):
            return
        if value != self.value:
            self.value = value
            self.changed()

    @property
    def level(self):
        return self.value / self.max if self.max else 0

    def set_level(self, level):
        level = max(0.02, min(1.0, level))
        value = int(round(level * self.max))
        try:
            if self.proxy is None:
                self.proxy = Gio.DBusProxy.new_for_bus_sync(
                    Gio.BusType.SYSTEM, Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES, None,
                    "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                    "org.freedesktop.login1.Session", None)
            self.proxy.call_sync("SetBrightness",
                                 GLib.Variant("(ssu)", ("backlight", os.path.basename(self.device), value)),
                                 Gio.DBusCallFlags.NONE, 1000, None)
        except GLib.Error as exc:
            print(f"tessera-shell: brightness: {exc.message}")
            return
        self.value = value
        self.changed()

    def step(self, delta):
        self.set_level(round((self.level + delta) * 20) / 20)


# --------------------------------------------------------------------------
# Battery (UPower)
# --------------------------------------------------------------------------

class Battery(Service):
    def __init__(self):
        super().__init__()
        self.present = False
        self.percent = 0
        self.charging = False
        self.full = False
        self.icon_name = "battery-missing-symbolic"
        self.time_to_empty = 0
        self.time_to_full = 0
        try:
            self.proxy = Gio.DBusProxy.new_for_bus_sync(
                Gio.BusType.SYSTEM, Gio.DBusProxyFlags.NONE, None,
                "org.freedesktop.UPower", "/org/freedesktop/UPower/devices/DisplayDevice",
                "org.freedesktop.UPower.Device", None)
            self.proxy.connect("g-properties-changed", lambda *a: self.refresh())
        except GLib.Error:
            self.proxy = None
        self.refresh()

    def _get(self, name, default):
        if self.proxy is None:
            return default
        value = self.proxy.get_cached_property(name)
        return value.unpack() if value is not None else default

    def refresh(self):
        self.present = bool(self._get("IsPresent", False)) and self._get("Type", 0) == 2
        self.percent = int(round(self._get("Percentage", 0)))
        state = self._get("State", 0)
        self.charging = state in (1, 5)
        self.full = state == 4
        self.icon_name = self._get("IconName", "") or "battery-good-symbolic"
        self.time_to_empty = self._get("TimeToEmpty", 0)
        self.time_to_full = self._get("TimeToFull", 0)
        self.changed()

    def describe(self):
        if not self.present:
            return ""
        if self.full:
            return f"Battery full ({self.percent}%)"
        if self.charging:
            if self.time_to_full:
                return f"{self.percent}% · charging, full in {format_duration(self.time_to_full)}"
            return f"{self.percent}% · charging"
        if self.time_to_empty:
            return f"{self.percent}% · about {format_duration(self.time_to_empty)} left"
        return f"{self.percent}%"


def format_duration(seconds):
    minutes = int(seconds // 60)
    hours, minutes = divmod(minutes, 60)
    if hours and minutes:
        return f"{hours} h {minutes} min"
    if hours:
        return f"{hours} h"
    return f"{minutes} min"


# --------------------------------------------------------------------------
# Wi-Fi (NetworkManager through nmcli)
# --------------------------------------------------------------------------

def _split_nmcli(line):
    """nmcli -t escapes ':' as '\\:' inside fields."""
    fields, cur, i = [], "", 0
    while i < len(line):
        c = line[i]
        if c == "\\" and i + 1 < len(line):
            cur += line[i + 1]
            i += 2
            continue
        if c == ":":
            fields.append(cur)
            cur = ""
        else:
            cur += c
        i += 1
    fields.append(cur)
    return fields


class Network(Service):
    def __init__(self):
        super().__init__()
        self.available = shutil.which("nmcli") is not None
        self.wifi_enabled = False
        self.has_wifi = False
        self.connected_ssid = ""
        self.signal = 0
        self.wired = False
        self.networks = []  # dicts: ssid, signal, secure, active, known
        self.busy = False
        if self.available:
            self.refresh()
            GLib.timeout_add_seconds(8, self._poll)
            self._watch()

    def _watch(self):
        """`nmcli monitor` prints a line whenever something changes."""
        try:
            proc = Gio.Subprocess.new(["nmcli", "monitor"], Gio.SubprocessFlags.STDOUT_PIPE)
        except GLib.Error:
            return
        stream = Gio.DataInputStream.new(proc.get_stdout_pipe())
        self._pending = False

        def on_line(s, res):
            try:
                line, _ = s.read_line_finish_utf8(res)
            except GLib.Error:
                line = None
            if line is None:
                return
            if not self._pending:
                self._pending = True
                GLib.timeout_add(400, self._delayed_refresh)
            s.read_line_async(GLib.PRIORITY_LOW, None, on_line)

        stream.read_line_async(GLib.PRIORITY_LOW, None, on_line)

    def _delayed_refresh(self):
        self._pending = False
        self.refresh()
        return False

    def _poll(self):
        self.refresh()
        return True

    def refresh(self):
        def got_radio(ok, out):
            self.wifi_enabled = out.strip() == "enabled"
            run_async(["nmcli", "-t", "-f", "TYPE,STATE,CONNECTION", "device"], got_devices)

        def got_devices(ok, out):
            self.has_wifi = False
            self.wired = False
            for line in out.splitlines():
                f = _split_nmcli(line)
                if len(f) < 3:
                    continue
                if f[0] == "wifi":
                    self.has_wifi = True
                if f[0] == "ethernet" and f[1] == "connected":
                    self.wired = True
            if self.has_wifi and self.wifi_enabled:
                run_async(["nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY",
                           "device", "wifi", "list", "--rescan", "no"], got_list)
            else:
                self.networks = []
                self.connected_ssid = ""
                self.signal = 0
                self.changed()

        def got_list(ok, out):
            best = {}
            active_ssid, active_signal = "", 0
            for line in out.splitlines():
                f = _split_nmcli(line)
                if len(f) < 4 or not f[1]:
                    continue
                in_use, ssid, signal, security = f[0] == "*", f[1], f[2], f[3]
                try:
                    signal = int(signal)
                except ValueError:
                    signal = 0
                if in_use:
                    active_ssid, active_signal = ssid, signal
                entry = best.get(ssid)
                if entry is None or signal > entry["signal"]:
                    best[ssid] = {"ssid": ssid, "signal": signal,
                                  "secure": security not in ("", "--"), "active": in_use}
                elif in_use:
                    entry["active"] = True
            self.connected_ssid = active_ssid
            self.signal = active_signal
            self.networks = sorted(best.values(), key=lambda n: (not n["active"], -n["signal"]))
            self.changed()

        run_async(["nmcli", "radio", "wifi"], got_radio)

    def rescan(self):
        run_async(["nmcli", "device", "wifi", "rescan"], lambda ok, out: GLib.timeout_add(2500, self._delayed_refresh))

    def set_enabled(self, enabled):
        run_async(["nmcli", "radio", "wifi", "on" if enabled else "off"],
                  lambda ok, out: self.refresh())

    def known(self, ssid):
        code, out = run(["nmcli", "-t", "-f", "NAME,TYPE", "connection", "show"], timeout=3)
        for line in out.splitlines():
            f = _split_nmcli(line)
            if len(f) >= 2 and f[0] == ssid and "wireless" in f[1]:
                return True
        return False

    def connect_to(self, ssid, password=None, done=None):
        if password is None and self.known(ssid):
            args = ["nmcli", "connection", "up", "id", ssid]
        else:
            args = ["nmcli", "device", "wifi", "connect", ssid]
            if password:
                args += ["password", password]
        self.busy = True
        self.changed()

        def finished(ok, out):
            self.busy = False
            self.refresh()
            if done:
                done(ok, out)

        run_async(args, finished)

    def disconnect(self):
        run_async(["nmcli", "connection", "down", "id", self.connected_ssid],
                  lambda ok, out: self.refresh())

    def forget(self, ssid):
        run_async(["nmcli", "connection", "delete", "id", ssid], lambda ok, out: self.refresh())

    def icon(self):
        if self.wired:
            return "network-wired-symbolic"
        if not self.has_wifi:
            return "network-offline-symbolic"
        if not self.wifi_enabled:
            return "network-wireless-disabled-symbolic"
        if not self.connected_ssid:
            return "network-wireless-offline-symbolic"
        return signal_icon(self.signal)

    def describe(self):
        if self.wired:
            return "Cable connected"
        if not self.has_wifi:
            return "No Wi-Fi"
        if not self.wifi_enabled:
            return "Off"
        if self.connected_ssid:
            return self.connected_ssid
        return "Not connected"


def signal_icon(signal):
    if signal >= 75:
        return "network-wireless-signal-excellent-symbolic"
    if signal >= 50:
        return "network-wireless-signal-good-symbolic"
    if signal >= 25:
        return "network-wireless-signal-ok-symbolic"
    return "network-wireless-signal-weak-symbolic"


# --------------------------------------------------------------------------
# Bluetooth (BlueZ over D-Bus)
# --------------------------------------------------------------------------

class Bluetooth(Service):
    def __init__(self):
        super().__init__()
        self.available = False
        self.powered = False
        self.adapter_path = None
        self.devices = []  # dicts: path, name, connected, paired, icon
        self.discovering = False
        try:
            self.manager = Gio.DBusObjectManagerClient.new_for_bus_sync(
                Gio.BusType.SYSTEM, Gio.DBusObjectManagerClientFlags.NONE,
                "org.bluez", "/", None, None, None)
            self.manager.connect("object-added", lambda *a: self.refresh())
            self.manager.connect("object-removed", lambda *a: self.refresh())
            self.manager.connect("interface-proxy-properties-changed", lambda *a: self.refresh())
        except GLib.Error:
            self.manager = None
        self.refresh()

    def refresh(self):
        self.available = False
        self.devices = []
        if self.manager is None or self.manager.get_name_owner() is None:
            self.changed()
            return
        for obj in self.manager.get_objects():
            adapter = obj.get_interface("org.bluez.Adapter1")
            if adapter is not None and self.adapter_path in (None, obj.get_object_path()):
                self.available = True
                self.adapter_path = obj.get_object_path()
                p = adapter.get_cached_property("Powered")
                self.powered = bool(p.unpack()) if p else False
                d = adapter.get_cached_property("Discovering")
                self.discovering = bool(d.unpack()) if d else False
        for obj in self.manager.get_objects():
            dev = obj.get_interface("org.bluez.Device1")
            if dev is None:
                continue

            def prop(name, default):
                v = dev.get_cached_property(name)
                return v.unpack() if v is not None else default

            name = prop("Alias", "") or prop("Name", "")
            if not name:
                continue
            paired = bool(prop("Paired", False))
            self.devices.append({
                "path": obj.get_object_path(),
                "name": name,
                "connected": bool(prop("Connected", False)),
                "paired": paired,
                "icon": (prop("Icon", "") or "bluetooth") + "-symbolic",
            })
        self.devices.sort(key=lambda d: (not d["connected"], not d["paired"], d["name"].lower()))
        self.changed()

    def _call(self, path, iface, method, args=None, done=None):
        def finished(conn, res):
            ok = True
            try:
                conn.call_finish(res)
            except GLib.Error as exc:
                ok = False
                print(f"tessera-shell: bluetooth {method}: {exc.message}")
            if done:
                done(ok)
            GLib.timeout_add(300, lambda: (self.refresh(), False)[1])

        Gio.bus_get_sync(Gio.BusType.SYSTEM, None).call(
            "org.bluez", path, iface, method, args, None, Gio.DBusCallFlags.NONE,
            30000, None, finished)

    def set_powered(self, powered):
        if not self.adapter_path:
            return
        if powered:
            # Bluetooth may be soft-blocked; unblocking needs no password.
            run(["rfkill", "unblock", "bluetooth"], timeout=2)
        self._call(self.adapter_path, "org.freedesktop.DBus.Properties", "Set",
                   GLib.Variant("(ssv)", ("org.bluez.Adapter1", "Powered",
                                          GLib.Variant("b", powered))))

    def set_discovering(self, on):
        if self.adapter_path:
            self._call(self.adapter_path, "org.bluez.Adapter1",
                       "StartDiscovery" if on else "StopDiscovery")

    # A pairing agent that only says yes while the user is pairing a
    # device from our own panel; requests at any other time are refused.
    AGENT_XML = """
    <node>
      <interface name="org.bluez.Agent1">
        <method name="Release"/>
        <method name="RequestPinCode"><arg type="o" direction="in"/><arg type="s" direction="out"/></method>
        <method name="DisplayPinCode"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
        <method name="RequestPasskey"><arg type="o" direction="in"/><arg type="u" direction="out"/></method>
        <method name="DisplayPasskey"><arg type="o" direction="in"/><arg type="u" direction="in"/><arg type="q" direction="in"/></method>
        <method name="RequestConfirmation"><arg type="o" direction="in"/><arg type="u" direction="in"/></method>
        <method name="RequestAuthorization"><arg type="o" direction="in"/></method>
        <method name="AuthorizeService"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
        <method name="Cancel"/>
      </interface>
    </node>"""

    pairing_active = False
    _agent_registered = False

    def _ensure_agent(self):
        if self._agent_registered:
            return
        try:
            bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
            info = Gio.DBusNodeInfo.new_for_xml(self.AGENT_XML)
            bus.register_object("/org/tessera/bluetooth_agent", info.interfaces[0],
                                self._agent_method, None, None)
            bus.call_sync("org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
                          GLib.Variant("(os)", ("/org/tessera/bluetooth_agent", "NoInputNoOutput")),
                          None, Gio.DBusCallFlags.NONE, 3000, None)
            self._agent_registered = True
        except GLib.Error as exc:
            print(f"tessera-shell: bluetooth agent: {exc.message}")

    def _agent_method(self, conn, sender, path, iface, method, params, invocation):
        if method in ("Release", "Cancel", "DisplayPinCode", "DisplayPasskey"):
            invocation.return_value(None)
            return
        if not self.pairing_active:
            invocation.return_dbus_error("org.bluez.Error.Rejected", "Not pairing right now")
            return
        if method == "RequestPinCode":
            invocation.return_value(GLib.Variant("(s)", ("0000",)))
        elif method == "RequestPasskey":
            invocation.return_value(GLib.Variant("(u)", (0,)))
        else:
            invocation.return_value(None)

    def toggle_device(self, device, done=None):
        path = device["path"]
        if device["connected"]:
            self._call(path, "org.bluez.Device1", "Disconnect", done=done)
            return
        if not device["paired"]:
            self._ensure_agent()
            self.pairing_active = True

            def paired(ok):
                self.pairing_active = False
                if ok:
                    self._call(path, "org.freedesktop.DBus.Properties", "Set",
                               GLib.Variant("(ssv)", ("org.bluez.Device1", "Trusted",
                                                      GLib.Variant("b", True))))
                    self._call(path, "org.bluez.Device1", "Connect", done=done)
                elif done:
                    done(False)
            self._call(path, "org.bluez.Device1", "Pair", done=paired)
            return
        self._call(path, "org.bluez.Device1", "Connect", done=done)

    def connected_names(self):
        return [d["name"] for d in self.devices if d["connected"]]


# --------------------------------------------------------------------------
# Power actions
# --------------------------------------------------------------------------

def logind_call(method, *args):
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        bus.call_sync("org.freedesktop.login1", "/org/freedesktop/login1",
                      "org.freedesktop.login1.Manager", method,
                      GLib.Variant("(b)", (True,)) if not args else GLib.Variant(*args),
                      None, Gio.DBusCallFlags.ALLOW_INTERACTIVE_AUTHORIZATION, 5000, None)
        return True
    except GLib.Error as exc:
        print(f"tessera-shell: {method}: {exc.message}")
        return False


def power_off():
    logind_call("PowerOff")


def reboot():
    logind_call("Reboot")


def suspend():
    logind_call("Suspend")

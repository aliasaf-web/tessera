"""System status: sound, brightness, battery, Wi-Fi, Bluetooth, power.

Every service keeps its current state and calls its listeners when it
changes. Reading is done through D-Bus or small official command line
tools (wpctl); nothing here needs root."""

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
    """Volume of the default output, through wpctl. Changes show at once and
    are sent in the background; while one is being sent, the next waits and
    only the latest value is used, so dragging the slider stays smooth."""

    WATCHED = re.compile(r"^Event '\w+' on (sink|server) #")

    def __init__(self):
        super().__init__()
        self.available = shutil.which("wpctl") is not None
        self.volume = 0.0
        self.muted = False
        self._watch = None
        self._sending = False
        self._wanted = None       # volume still to send
        self._refresh_source = None
        if self.available:
            self.refresh()
            self._start_watch()
            GLib.timeout_add_seconds(10, self._poll)

    def refresh(self):
        if not self.available or self._sending:
            return  # the state is read again when sending is done
        run_async(["wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"], self._got_volume)

    def _got_volume(self, ok, out):
        if not ok or self._sending:
            return
        m = re.search(r"Volume:\s*([0-9.]+)", out)
        volume = float(m.group(1)) if m else 0.0
        muted = "MUTED" in out
        if abs(volume - self.volume) > 0.001 or muted != self.muted:
            self.volume, self.muted = volume, muted
            self.changed()

    def _refresh_soon(self):
        """Several changes in a row lead to one refresh."""
        if self._refresh_source is None:
            self._refresh_source = GLib.timeout_add(150, self._refresh_now)

    def _refresh_now(self):
        self._refresh_source = None
        self.refresh()
        return False

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
            # Only the outputs and the default device; not every app's stream.
            if self.WATCHED.match(line):
                self._refresh_soon()
            s.read_line_async(GLib.PRIORITY_LOW, None, on_line)

        stream.read_line_async(GLib.PRIORITY_LOW, None, on_line)

    def set_volume(self, value):
        value = max(0.0, min(1.0, value))
        unmute = self.muted and value > 0
        self.volume = value
        if unmute:
            self.muted = False
        self.changed()
        self._wanted = (value, unmute or (self._wanted is not None and self._wanted[1]))
        self._send()

    def _send(self):
        if self._sending or self._wanted is None:
            return
        value, unmute = self._wanted
        self._wanted = None
        self._sending = True

        def volume_done(_ok, _out):
            if unmute:
                run_async(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "0"], sent)
            else:
                sent(True, "")

        def sent(_ok, _out):
            self._sending = False
            if self._wanted is not None:
                self._send()
            else:
                self.refresh()

        run_async(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", f"{value:.2f}"], volume_done)

    def step(self, delta):
        self.set_volume(round((self.volume + delta) * 20) / 20)

    def toggle_mute(self):
        if not self.available:
            return
        self.muted = not self.muted
        self.changed()
        run_async(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"],
                  lambda _ok, _out: self._refresh_soon())

    def toggle_mic(self):
        run_async(["wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle"])

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
# Wi-Fi (NetworkManager over D-Bus)
# --------------------------------------------------------------------------

# NetworkManager's D-Bus interface (see "man 5 nm-settings" and the
# NetworkManager D-Bus API reference). Nothing is polled: NetworkManager
# announces every change, and Gio keeps the latest property values.
NM = "org.freedesktop.NetworkManager"
NM_PATH = "/org/freedesktop/NetworkManager"
NM_SETTINGS = "org.freedesktop.NetworkManager.Settings"
NM_SETTINGS_PATH = "/org/freedesktop/NetworkManager/Settings"
NM_CONNECTION = "org.freedesktop.NetworkManager.Settings.Connection"
NM_DEVICE = "org.freedesktop.NetworkManager.Device"
NM_WIRELESS = "org.freedesktop.NetworkManager.Device.Wireless"
NM_AP = "org.freedesktop.NetworkManager.AccessPoint"
NM_ACTIVE = "org.freedesktop.NetworkManager.Connection.Active"
DEVICE_TYPE_ETHERNET, DEVICE_TYPE_WIFI = 1, 2
DEVICE_STATE_ACTIVATED = 100
ACTIVE_ACTIVATED, ACTIVE_DEACTIVATED = 2, 4
REASON_NO_SECRETS, REASON_LOGIN_FAILED = 9, 10
AP_FLAGS_PRIVACY = 0x1
KEY_MGMT_PSK, KEY_MGMT_802_1X, KEY_MGMT_SAE = 0x100, 0x200, 0x400
CONNECT_TIMEOUT = 90  # seconds


def _prop(proxy, name, default=None):
    value = proxy.get_cached_property(name) if proxy is not None else None
    return value.unpack() if value is not None else default


def _ssid_text(raw):
    return bytes(raw).decode("utf-8", errors="replace")


class Network(Service):
    """Wi-Fi and cable through NetworkManager.

    `connect_to` reports how it went with done(ok, problem), where problem is
    None, "password", "range", "enterprise" or "other"."""

    def __init__(self):
        super().__init__()
        self.available = False
        self.wifi_enabled = False
        self.has_wifi = False
        self.connected_ssid = ""
        self.signal = 0
        self.wired = False
        self.networks = []  # dicts: ssid, signal, secure, active, known
        self.busy = False
        self._bus = None
        self._manager = None
        self._devices = {}      # path -> {"device": proxy, "wireless": proxy or None}
        self._aps = {}          # path -> proxy
        self._saved = {}        # connection path -> SSID text (Wi-Fi profiles only)
        self._list_wanted = False
        self._refresh_source = None
        self._summary = None
        self._attempts = {}     # active connection path -> (state, reason, event number)
        self._event_number = 0
        self._waiting = {}      # active connection path -> callback
        try:
            self._bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        except GLib.Error:
            return
        # These follow NetworkManager across restarts, so they are set up once.
        self._bus.signal_subscribe(NM, NM_SETTINGS, None, NM_SETTINGS_PATH, None,
                                   Gio.DBusSignalFlags.NONE, self._settings_signal)
        self._bus.signal_subscribe(NM, NM_CONNECTION, "Updated", None, None,
                                   Gio.DBusSignalFlags.NONE,
                                   lambda _c, _s, path, *_a: self._load_saved(path))
        self._bus.signal_subscribe(NM, NM_ACTIVE, "StateChanged", None, None,
                                   Gio.DBusSignalFlags.NONE, self._active_state)
        Gio.bus_watch_name_on_connection(self._bus, NM, Gio.BusNameWatcherFlags.NONE,
                                         self._appeared, self._vanished)

    # ------------------------------------------------------------ tracking

    def _appeared(self, _conn, _name, _owner):
        Gio.DBusProxy.new(self._bus, Gio.DBusProxyFlags.NONE, None, NM, NM_PATH, NM, None,
                          self._got_manager)
        self._call(NM_SETTINGS_PATH, NM_SETTINGS, "ListConnections", None, self._got_saved_list)

    def _vanished(self, _conn, _name):
        self.available = False
        self._manager = None
        self._devices.clear()
        self._aps.clear()
        self._saved.clear()
        self._refresh_soon()

    def _got_manager(self, _source, result):
        try:
            self._manager = Gio.DBusProxy.new_finish(result)
        except GLib.Error:
            return
        self.available = True
        self._manager.connect("g-properties-changed", lambda *_a: self._sync_devices())
        self._sync_devices()

    def _sync_devices(self):
        paths = set(_prop(self._manager, "Devices", []))
        for path in list(self._devices):
            if path not in paths:
                del self._devices[path]
        for path in paths - set(self._devices):
            self._devices[path] = {"device": None, "wireless": None}
            Gio.DBusProxy.new(self._bus, Gio.DBusProxyFlags.NONE, None, NM, path, NM_DEVICE, None,
                              self._got_device, path)
        self._sync_aps()

    def _got_device(self, _source, result, path):
        try:
            proxy = Gio.DBusProxy.new_finish(result)
        except GLib.Error:
            return
        if path not in self._devices:
            return
        self._devices[path]["device"] = proxy
        proxy.connect("g-properties-changed", lambda *_a: self._refresh_soon())
        if _prop(proxy, "DeviceType") == DEVICE_TYPE_WIFI:
            Gio.DBusProxy.new(self._bus, Gio.DBusProxyFlags.NONE, None, NM, path, NM_WIRELESS,
                              None, self._got_wireless, path)
        self._refresh_soon()

    def _got_wireless(self, _source, result, path):
        try:
            proxy = Gio.DBusProxy.new_finish(result)
        except GLib.Error:
            return
        if path not in self._devices:
            return
        self._devices[path]["wireless"] = proxy
        proxy.connect("g-properties-changed", lambda *_a: self._sync_aps())
        self._sync_aps()

    def _sync_aps(self):
        paths = set()
        for dev in self._devices.values():
            paths.update(_prop(dev["wireless"], "AccessPoints", []))
        for path in list(self._aps):
            if path not in paths:
                del self._aps[path]
        for path in paths - set(self._aps):
            self._aps[path] = None
            Gio.DBusProxy.new(self._bus, Gio.DBusProxyFlags.NONE, None, NM, path, NM_AP, None,
                              self._got_ap, path)
        self._refresh_soon()

    def _got_ap(self, _source, result, path):
        try:
            proxy = Gio.DBusProxy.new_finish(result)
        except GLib.Error:
            return
        if path not in self._aps:
            return
        self._aps[path] = proxy
        proxy.connect("g-properties-changed", lambda *_a: self._ap_changed(path))
        self._refresh_soon()

    def _ap_changed(self, path):
        # Signal strength changes all the time; only the network in use and an
        # open network list need them.
        if self._list_wanted or path in self._active_aps():
            self._refresh_soon()

    def _active_aps(self):
        return {_prop(dev["wireless"], "ActiveAccessPoint", "/")
                for dev in self._devices.values()} - {"/"}

    def _got_saved_list(self, ok, result):
        if ok:
            for path in result[0]:
                self._load_saved(path)

    def _settings_signal(self, _conn, _sender, _path, _iface, signal, params):
        path = params.unpack()[0]
        if signal == "NewConnection":
            self._load_saved(path)
        elif signal == "ConnectionRemoved" and self._saved.pop(path, None) is not None:
            self._refresh_soon()

    def _load_saved(self, path):
        def got(ok, result):
            if not ok:
                return
            settings = result[0]
            wifi = settings.get("802-11-wireless")
            if settings.get("connection", {}).get("type") == "802-11-wireless" and wifi:
                self._saved[path] = _ssid_text(wifi.get("ssid", b""))
            else:
                self._saved.pop(path, None)
            self._refresh_soon()

        self._call(path, NM_CONNECTION, "GetSettings", None, got)

    def _refresh_soon(self):
        """Several changes in a row lead to one update."""
        if self._refresh_source is None:
            self._refresh_source = GLib.timeout_add(250, self._refresh_now)

    def _refresh_now(self):
        self._refresh_source = None
        self.refresh()
        return False

    def refresh(self):
        """Works out the state from what NetworkManager last announced."""
        self.wifi_enabled = bool(_prop(self._manager, "WirelessEnabled", False) and
                                 _prop(self._manager, "WirelessHardwareEnabled", True))
        self.has_wifi = any(dev["wireless"] is not None for dev in self._devices.values())
        self.wired = any(_prop(dev["device"], "DeviceType") == DEVICE_TYPE_ETHERNET and
                         _prop(dev["device"], "State") == DEVICE_STATE_ACTIVATED
                         for dev in self._devices.values())
        active = self._active_aps()
        known = set(self._saved.values())
        best = {}
        self.connected_ssid, self.signal = "", 0
        for path, ap in self._aps.items():
            if ap is None:
                continue
            ssid = _ssid_text(_prop(ap, "Ssid", b""))
            if not ssid:
                continue  # hidden network
            strength = _prop(ap, "Strength", 0)
            flags, wpa, rsn = _prop(ap, "Flags", 0), _prop(ap, "WpaFlags", 0), _prop(ap, "RsnFlags", 0)
            is_active = path in active
            if is_active:
                self.connected_ssid, self.signal = ssid, strength
            entry = best.get(ssid)
            if entry is None or strength > entry["signal"]:
                best[ssid] = {"ssid": ssid, "signal": strength, "path": path,
                              "secure": bool(flags & AP_FLAGS_PRIVACY or wpa or rsn),
                              "active": is_active or (entry is not None and entry["active"]),
                              "known": ssid in known}
            elif is_active:
                entry["active"] = True
        if not (self.has_wifi and self.wifi_enabled):
            best = {}
            self.connected_ssid, self.signal = "", 0
        # Steady order: the network in use, then by signal (in the steps
        # shown as icons), then by name.
        self.networks = sorted(best.values(), key=lambda n: (
            not n["active"], -min(n["signal"] // 25, 3), n["ssid"].lower()))
        summary = (self.available, self.wifi_enabled, self.has_wifi, self.wired,
                   self.connected_ssid, min(self.signal // 25, 3), self.busy,
                   tuple((n["ssid"], min(n["signal"] // 25, 3), n["secure"], n["active"],
                          n["known"]) for n in self.networks))
        if summary != self._summary:
            self._summary = summary
            self.changed()

    def set_list_wanted(self, wanted):
        """Quick settings shows the network list: keep signal strengths fresh."""
        self._list_wanted = wanted

    # ------------------------------------------------------------- actions

    def _call(self, path, iface, method, args, done=None):
        if self._bus is None:
            if done:
                done(False, None)
            return

        def finished(conn, res):
            try:
                result = conn.call_finish(res).unpack()
            except GLib.Error as exc:
                print(f"tessera-shell: network {method}: {exc.message}")
                if done:
                    done(False, exc)
                return
            if done:
                done(True, result)

        self._bus.call(NM, path, iface, method, args, None, Gio.DBusCallFlags.NONE, 30000, None,
                       finished)

    def rescan(self):
        for dev in self._devices.values():
            if dev["wireless"] is not None:
                # NetworkManager refuses scans that come too often; that's fine.
                self._call(dev["wireless"].get_object_path(), NM_WIRELESS, "RequestScan",
                           GLib.Variant("(a{sv})", ({},)))

    def set_enabled(self, enabled):
        self._call(NM_PATH, "org.freedesktop.DBus.Properties", "Set",
                   GLib.Variant("(ssv)", (NM, "WirelessEnabled", GLib.Variant("b", enabled))))

    def known(self, ssid):
        return ssid in self._saved.values()

    def _wifi_device_for(self, ap_path):
        for path, dev in self._devices.items():
            if dev["wireless"] is not None and ap_path in _prop(dev["wireless"], "AccessPoints", []):
                return path
        return None

    def connect_to(self, ssid, password=None, done=None):
        network = next((n for n in self.networks if n["ssid"] == ssid), None)
        device = self._wifi_device_for(network["path"]) if network else None
        if network is None or device is None:
            if done:
                done(False, "range")
            return
        ap_path = network["path"]
        saved = [path for path, name in self._saved.items() if name == ssid]
        since = self._event_number

        def finished(ok, problem, created=None):
            if problem == "password" and not network["secure"]:
                problem = "other"  # an open network has no password to retype
            self.busy = False
            self._summary = None
            self.refresh()
            # A new profile whose password was wrong is removed again; profiles
            # that existed before are never removed here.
            if not ok and created and problem == "password":
                self._call(created, NM_CONNECTION, "Delete", None)
            if done:
                done(ok, problem)

        self.busy = True
        self._summary = None
        self.refresh()
        if saved:
            def activate():
                self._call(NM_PATH, NM, "ActivateConnection",
                           GLib.Variant("(ooo)", (saved[0], device, ap_path)),
                           lambda ok, res: self._follow(res[0] if ok else None, finished, since))
            if password:
                self._set_password(saved[0], password, lambda ok: activate() if ok
                                   else finished(False, "other"))
            else:
                activate()
            return
        settings = self._security_for(self._aps.get(ap_path), password or "")
        if settings is None:
            finished(False, "enterprise")
            return

        def added(ok, res):
            if not ok:
                finished(False, "other")
                return
            created, active = res
            self._follow(active, lambda ok2, problem: finished(ok2, problem, created), since)

        self._call(NM_PATH, NM, "AddAndActivateConnection",
                   GLib.Variant("(a{sa{sv}}oo)", (settings, device, ap_path)), added)

    @staticmethod
    def _security_for(ap, password):
        """Settings for a new profile; the password goes to NetworkManager over
        D-Bus, never on a command line. None means the network needs a user
        name too (802.1X), which isn't supported here."""
        flags, wpa, rsn = _prop(ap, "Flags", 0), _prop(ap, "WpaFlags", 0), _prop(ap, "RsnFlags", 0)
        if not (flags & AP_FLAGS_PRIVACY or wpa or rsn):
            return {}
        if (wpa | rsn) & KEY_MGMT_802_1X and not (wpa | rsn) & (KEY_MGMT_PSK | KEY_MGMT_SAE):
            return None
        if not (wpa or rsn):  # old WEP networks
            security = {"key-mgmt": GLib.Variant("s", "none"),
                        "wep-key0": GLib.Variant("s", password),
                        "wep-key-type": GLib.Variant("u", 2)}
        else:
            sae_only = rsn & KEY_MGMT_SAE and not (wpa | rsn) & KEY_MGMT_PSK
            security = {"key-mgmt": GLib.Variant("s", "sae" if sae_only else "wpa-psk"),
                        "psk": GLib.Variant("s", password)}
        return {"802-11-wireless-security": security}

    def _set_password(self, path, password, done):
        """Stores a new password in an existing profile (it changed)."""
        def got(conn, res):
            try:
                raw = conn.call_finish(res).get_child_value(0)
            except GLib.Error:
                done(False)
                return
            # Rebuilt group by group to keep every value's exact D-Bus type.
            settings = {}
            for i in range(raw.n_children()):
                group = raw.get_child_value(i)
                values = group.get_child_value(1)
                settings[group.get_child_value(0).get_string()] = {
                    values.get_child_value(j).get_child_value(0).get_string():
                        values.get_child_value(j).get_child_value(1).get_variant()
                    for j in range(values.n_children())}
            security = settings.setdefault("802-11-wireless-security", {})
            # Flags 0: kept by NetworkManager itself, not by a keyring that may
            # not be running.
            if "wep-key-type" in security:
                security["wep-key0"] = GLib.Variant("s", password)
                security["wep-key-flags"] = GLib.Variant("u", 0)
            else:
                security["psk"] = GLib.Variant("s", password)
                security["psk-flags"] = GLib.Variant("u", 0)
            self._call(path, NM_CONNECTION, "Update", GLib.Variant("(a{sa{sv}})", (settings,)),
                       lambda ok, _r: done(ok))

        self._bus.call(NM, path, NM_CONNECTION, "GetSettings", None, None,
                       Gio.DBusCallFlags.NONE, 10000, None, got)

    def _active_state(self, _conn, _sender, path, _iface, _signal, params):
        """Every connection attempt's progress, remembered for a while, so a
        result that comes before we start waiting for it isn't missed."""
        state, reason = params.unpack()
        self._event_number += 1
        number = self._event_number
        self._attempts[path] = (state, reason, number)

        def forget():
            if self._attempts.get(path, (0, 0, 0))[2] == number:
                del self._attempts[path]
            return False

        GLib.timeout_add_seconds(CONNECT_TIMEOUT, forget)
        waiter = self._waiting.get(path)
        if waiter:
            waiter(state, reason)

    def _follow(self, active_path, done, since):
        """Waits until NetworkManager says whether the connection came up.
        Only news numbered after `since` (when the request was made) counts."""
        if not active_path or active_path == "/":
            done(False, "other")
            return
        timeout = None

        def check(state, reason=0):
            if state not in (ACTIVE_ACTIVATED, ACTIVE_DEACTIVATED) or \
                    self._waiting.get(active_path) is not check:
                return
            del self._waiting[active_path]
            if timeout:
                GLib.source_remove(timeout)
            if state == ACTIVE_ACTIVATED:
                done(True, None)
            else:
                done(False, "password" if reason in (REASON_NO_SECRETS, REASON_LOGIN_FAILED)
                     else "other")

        def give_up():
            nonlocal timeout
            timeout = None
            if self._waiting.get(active_path) is check:
                del self._waiting[active_path]
                done(False, "other")
            return False

        self._waiting[active_path] = check
        timeout = GLib.timeout_add_seconds(CONNECT_TIMEOUT, give_up)
        state, reason, number = self._attempts.get(active_path, (0, 0, 0))
        if number > since:
            check(state, reason)
        else:
            # Already up before it had anything to announce.
            self._call(active_path, "org.freedesktop.DBus.Properties", "Get",
                       GLib.Variant("(ss)", (NM_ACTIVE, "State")),
                       lambda ok, res: check(res[0]) if ok else None)

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
    """Bluetooth through BlueZ.

    `busy` is {"action": "pair" | "connect" | "disconnect", "name": ...} while
    a device is being paired, connected or disconnected, otherwise None.

    While pairing, `prompt` describes what the user must do, or is None:
    {"kind": "confirm" | "display" | "pin" | "passkey", "name": ..., "code": ...}
    "confirm": check the code matches the device, then answer(True/False).
    "display": type the code on the device (a keyboard); answer(False) cancels.
    "pin", "passkey": type the code the device shows, answer(True, code)."""

    # Changes that come all the time while searching and don't change the list.
    NOISY = {"RSSI", "TxPower", "ManufacturerData", "ServiceData", "AdvertisingFlags",
             "AdvertisingData"}

    def __init__(self):
        super().__init__()
        self.available = False
        self.powered = False
        self.adapter_path = None
        self.devices = []  # dicts: path, name, connected, paired, icon
        self.discovering = False
        self.busy = None
        self.prompt = None
        self.pairing_device = None
        self._declined = False
        self._invocation = None
        self._agent_exported = False
        self._agent_registered = False
        self._refresh_source = None
        self._summary = None
        try:
            self.manager = Gio.DBusObjectManagerClient.new_for_bus_sync(
                Gio.BusType.SYSTEM, Gio.DBusObjectManagerClientFlags.NONE,
                "org.bluez", "/", None, None, None)
            self.manager.connect("object-added", lambda *a: self._refresh_soon())
            self.manager.connect("object-removed", lambda *a: self._refresh_soon())
            self.manager.connect("interface-proxy-properties-changed", self._properties_changed)
            self.manager.connect("notify::name-owner", self._owner_changed)
        except GLib.Error:
            self.manager = None
        self.refresh()

    def _owner_changed(self, *_args):
        # BlueZ started or restarted: it has forgotten the agent and any
        # request that was being shown.
        self._agent_registered = False
        self._invocation = None
        self.prompt = None
        self._refresh_soon()

    def _properties_changed(self, _manager, _obj, _iface, changed, invalidated):
        if (set(changed.unpack()) | set(invalidated)) - self.NOISY:
            self._refresh_soon()

    def _refresh_soon(self):
        """Several changes in a row lead to one update."""
        if self._refresh_source is None:
            self._refresh_source = GLib.timeout_add(200, self._refresh_now)

    def _refresh_now(self):
        self._refresh_source = None
        self.refresh()
        return False

    def refresh(self):
        self.available = self.powered = self.discovering = False
        self.devices = []
        adapter_path, self.adapter_path = self.adapter_path, None
        if self.manager is not None and self.manager.get_name_owner() is not None:
            for obj in self.manager.get_objects():
                adapter = obj.get_interface("org.bluez.Adapter1")
                if adapter is None or self.adapter_path is not None:
                    continue
                # Keep using the same adapter while it is there.
                if adapter_path in (None, obj.get_object_path()) or not any(
                        o.get_object_path() == adapter_path for o in self.manager.get_objects()):
                    self.available = True
                    self.adapter_path = obj.get_object_path()
                    self.powered = bool(_prop(adapter, "Powered", False))
                    self.discovering = bool(_prop(adapter, "Discovering", False))
            for obj in self.manager.get_objects():
                dev = obj.get_interface("org.bluez.Device1")
                if dev is None:
                    continue
                name = _prop(dev, "Alias", "") or _prop(dev, "Name", "")
                if not name:
                    continue
                self.devices.append({
                    "path": obj.get_object_path(),
                    "name": name,
                    "connected": bool(_prop(dev, "Connected", False)),
                    "paired": bool(_prop(dev, "Paired", False)),
                    "icon": (_prop(dev, "Icon", "") or "bluetooth") + "-symbolic",
                })
        self.devices.sort(key=lambda d: (not d["connected"], not d["paired"], d["name"].lower()))
        summary = (self.available, self.powered, self.discovering, repr(self.busy),
                   repr(self.prompt),
                   tuple(tuple(d.values()) for d in self.devices))
        if summary != self._summary:
            self._summary = summary
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
            self._refresh_soon()

        Gio.bus_get_sync(Gio.BusType.SYSTEM, None).call(
            "org.bluez", path, iface, method, args, None, Gio.DBusCallFlags.NONE,
            60000, None, finished)

    def set_powered(self, powered):
        if not self.adapter_path:
            return

        def set_power(_ok=True, _out=""):
            self._call(self.adapter_path, "org.freedesktop.DBus.Properties", "Set",
                       GLib.Variant("(ssv)", ("org.bluez.Adapter1", "Powered",
                                              GLib.Variant("b", powered))))

        if powered:
            # Bluetooth may be soft-blocked; unblocking needs no password.
            run_async(["rfkill", "unblock", "bluetooth"], set_power)
        else:
            set_power()

    def set_discovering(self, on):
        if self.adapter_path:
            self._call(self.adapter_path, "org.bluez.Adapter1",
                       "StartDiscovery" if on else "StopDiscovery")

    # ---------------------------------------------------------- pairing

    # BlueZ asks this agent to show or check pairing codes. It answers only
    # for the device the user is pairing from quick settings; any other
    # request is refused.
    AGENT_PATH = "/org/tessera/bluetooth_agent"
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

    def _with_agent(self, then):
        """Makes sure BlueZ knows the agent, then calls then()."""
        if self._agent_registered:
            then()
            return
        bus = self.manager.get_connection()
        if not self._agent_exported:
            try:
                info = Gio.DBusNodeInfo.new_for_xml(self.AGENT_XML)
                bus.register_object(self.AGENT_PATH, info.interfaces[0], self._agent_method,
                                    None, None)
                self._agent_exported = True
            except GLib.Error as exc:
                print(f"tessera-shell: bluetooth agent: {exc.message}")
                then()
                return

        def registered(conn, res):
            try:
                conn.call_finish(res)
                self._agent_registered = True
            except GLib.Error as exc:
                if Gio.DBusError.get_remote_error(exc) == "org.bluez.Error.AlreadyExists":
                    self._agent_registered = True
                else:
                    print(f"tessera-shell: bluetooth agent: {exc.message}")
            then()

        # It can show a code and take typed input, so keyboards and phones use
        # their proper pairing methods.
        bus.call("org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
                 GLib.Variant("(os)", (self.AGENT_PATH, "KeyboardDisplay")),
                 None, Gio.DBusCallFlags.NONE, 5000, None, registered)

    def _set_prompt(self, prompt, invocation=None):
        if self._invocation is not None:
            self._invocation.return_dbus_error("org.bluez.Error.Canceled", "Replaced")
        self.prompt, self._invocation = prompt, invocation
        self.refresh()

    def _agent_method(self, _conn, _sender, _path, _iface, method, params, invocation):
        args = params.unpack()
        if method == "Release":
            self._agent_registered = False
            invocation.return_value(None)
            return
        if method == "Cancel":
            # BlueZ gave up on the request that was being shown.
            self._invocation = None
            self._set_prompt(None)
            invocation.return_value(None)
            return
        device = args[0]
        if self.pairing_device is None or device != self.pairing_device:
            invocation.return_dbus_error("org.bluez.Error.Rejected", "Not pairing this device")
            return
        name = next((d["name"] for d in self.devices if d["path"] == device), "the device")
        if method == "DisplayPinCode":
            self._set_prompt({"kind": "display", "name": name, "code": args[1]})
            invocation.return_value(None)
        elif method == "DisplayPasskey":
            self._set_prompt({"kind": "display", "name": name, "code": f"{args[1]:06d}"})
            invocation.return_value(None)
        elif method == "RequestConfirmation":
            self._set_prompt({"kind": "confirm", "name": name, "code": f"{args[1]:06d}"},
                             invocation)
        elif method == "RequestPinCode":
            self._set_prompt({"kind": "pin", "name": name, "code": ""}, invocation)
        elif method == "RequestPasskey":
            self._set_prompt({"kind": "passkey", "name": name, "code": ""}, invocation)
        else:
            # RequestAuthorization and AuthorizeService for the device being
            # paired: the user asked for it by clicking it.
            invocation.return_value(None)

    def answer(self, accept, code=""):
        """The user's reply to the pairing prompt."""
        prompt, invocation = self.prompt, self._invocation
        self.prompt, self._invocation = None, None
        if not accept:
            self._declined = True
        if invocation is not None:
            if not accept:
                invocation.return_dbus_error("org.bluez.Error.Rejected", "Refused by the user")
            elif prompt["kind"] == "pin":
                invocation.return_value(GLib.Variant("(s)", (code,)))
            elif prompt["kind"] == "passkey":
                invocation.return_value(GLib.Variant("(u)", (int(code),)))
            else:
                invocation.return_value(None)
        elif not accept and self.pairing_device:
            self._call(self.pairing_device, "org.bluez.Device1", "CancelPairing")
        self.refresh()

    def _set_busy(self, action, name=None):
        self.busy = {"action": action, "name": name} if action else None
        self.refresh()

    def toggle_device(self, device, done=None):
        """done(ok) is called at the end; ok is None if the user turned down
        the pairing."""
        path, name = device["path"], device["name"]

        def finished(ok):
            self._set_busy(None)
            if done:
                done(ok)

        if device["connected"]:
            self._set_busy("disconnect", name)
            self._call(path, "org.bluez.Device1", "Disconnect", done=finished)
            return
        if not device["paired"]:
            self.pairing_device = path
            self._declined = False
            self._set_busy("pair", name)

            def paired(ok):
                self.pairing_device = None
                if self.prompt is not None:
                    self._invocation = None
                    self._set_prompt(None)
                if not ok:
                    finished(None if self._declined else False)
                    return
                self._call(path, "org.freedesktop.DBus.Properties", "Set",
                           GLib.Variant("(ssv)", ("org.bluez.Device1", "Trusted",
                                                  GLib.Variant("b", True))))
                self._set_busy("connect", name)
                self._call(path, "org.bluez.Device1", "Connect", done=finished)

            self._with_agent(lambda: self._call(path, "org.bluez.Device1", "Pair", done=paired))
            return
        self._set_busy("connect", name)
        self._call(path, "org.bluez.Device1", "Connect", done=finished)

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

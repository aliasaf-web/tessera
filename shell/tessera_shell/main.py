"""Entry point for the Tessera shell."""

import os
import shutil
import subprocess
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Gtk4LayerShell", "1.0")
gi.require_version("GdkPixbuf", "2.0")

from gi.repository import Gdk, Gio, GLib, Gtk  # noqa: E402

# These were only needed to start the shell itself; keep them away from
# the apps it opens.
for _name in ("LD_PRELOAD", "PYTHONPATH"):
    os.environ.pop(_name, None)

from . import ipc, services  # noqa: E402
from .bar import Bar  # noqa: E402
from .help import Help  # noqa: E402
from .launcher import Launcher  # noqa: E402
from .notifications import Notifications  # noqa: E402
from .osd import Osd  # noqa: E402
from .quicksettings import QuickSettings  # noqa: E402
from .switcher import Switcher  # noqa: E402
from .theme import Theme, apply_app_theme, read_config  # noqa: E402


class Shell:
    def __init__(self, app):
        self.app = app
        self.state = {}
        self.cfg = read_config()

        self.theme = Theme()
        apply_app_theme(self.theme.dark, self.theme.accent_name)
        self.audio = services.Audio()
        self.brightness = services.Brightness()
        self.battery = services.Battery()
        self.network = services.Network()
        self.bluetooth = services.Bluetooth()

        self.notifications = Notifications(self)
        self.launcher = Launcher(self)
        self.quick_settings = QuickSettings(self)
        self.osd = Osd(self)
        self.help = Help(self)
        self.switcher = Switcher(self)

        self.bars = {}
        monitors = Gdk.Display.get_default().get_monitors()
        monitors.connect("items-changed", lambda *_: self.sync_bars())
        self.sync_bars()

        self.events = ipc.EventStream(self.on_event)
        self._low_battery_warned = False
        self.battery.connect(self._check_battery)

        # The welcome screen waits until the desktop is unlocked.
        self._welcome_pending = self.help.first_run()

    # ------------------------------------------------------------- monitors

    def sync_bars(self):
        monitors = Gdk.Display.get_default().get_monitors()
        present = [monitors.get_item(i) for i in range(monitors.get_n_items())]
        for mon in list(self.bars):
            if mon not in present:
                self.bars.pop(mon).destroy()
        for mon in present:
            if mon not in self.bars:
                self.bars[mon] = Bar(self, mon)
        return False

    def metrics(self, monitor):
        """Sizes of the bar and the space around windows. Small screens
        (up to 800 pixels tall) get a slimmer bar unless set otherwise;
        the window manager uses the same rule."""
        if monitor is None:
            monitors = Gdk.Display.get_default().get_monitors()
            monitor = monitors.get_item(0) if monitors.get_n_items() else None
        height = monitor.get_geometry().height if monitor is not None else 1080
        size = self.cfg.get("layout_size", "auto")
        compact = size == "compact" or (size not in ("compact", "comfortable") and height <= 800)
        try:
            gaps = int(self.cfg.get("gaps", ""))
            gaps = max(0, min(64, gaps))
        except ValueError:
            gaps = 8 if compact else 14
        return {"compact": compact, "gaps": gaps, "bar": 36 if compact else 44,
                "margin": 8 if compact else 12}

    def reload(self):
        """Settings changed: pick up colours, sizes and so on."""
        self.cfg = read_config()
        self.theme.reload()
        apply_app_theme(self.theme.dark, self.theme.accent_name)
        for bar in list(self.bars.values()):
            bar.destroy()
        self.bars = {}
        self.sync_bars()
        self.quick_settings.place()
        self.quick_settings.update_theme()
        self.osd.place()
        self.notifications.place()

    def open_settings(self):
        self.quick_settings.hide()
        try:
            app = Gio.DesktopAppInfo.new("org.tessera.Settings.desktop")
        except TypeError:
            self.notifications.notify_internal("Could not open Settings",
                                               "Settings is not installed.")
            return
        # Started like any app, so an open Settings window comes to the front.
        self.launcher.launch(app)

    def current_monitor(self):
        # The compositor places a popup on the screen in use when no monitor
        # is given; return None so that behaviour applies.
        return None

    def set_status_open(self, is_open):
        for bar in self.bars.values():
            bar.set_open(is_open)

    # ---------------------------------------------------------------- toggles

    def toggle_launcher(self, monitor=None):
        """Opens the launcher on `monitor` (the screen of the bar button that was
        clicked), or on the screen in use."""
        if self.quick_settings.visible:
            self.quick_settings.hide()
        self.launcher.toggle(monitor if monitor is not None else self.current_monitor())

    def toggle_quick_settings(self, monitor=None, page=None):
        if self.launcher.visible:
            self.launcher.hide()
        self.quick_settings.toggle(monitor, page)

    # ----------------------------------------------------------------- events

    def on_event(self, event):
        kind = event.get("event")
        if kind == "state":
            previous, self.state = self.state, event
            for bar in self.bars.values():
                bar.update_workspaces(event)
            if event.get("locked"):
                self.launcher.hide()
                self.quick_settings.hide()
                self.help.window.set_visible(False)
            if self._welcome_pending and not event.get("locked"):
                self._welcome_pending = False
                GLib.timeout_add(800, lambda: (self.help.show(self.current_monitor()), False)[1])
            if event.get("night_light") != previous.get("night_light"):
                self.quick_settings.update_theme()
        elif kind == "reload":
            self.reload()
        elif kind == "switcher":
            if event.get("visible"):
                # The chosen window gets the keyboard, so close what it would cover.
                self.launcher.hide()
                self.quick_settings.hide()
                self.help.window.set_visible(False)
            self.switcher.update(event)
        elif kind == "key":
            self.on_key(event.get("action", ""))

    def on_key(self, action):
        if action == "launcher":
            self.toggle_launcher()
        elif action == "launcher-show":
            if not self.launcher.visible:
                self.toggle_launcher()
        elif action == "launcher-hide":
            self.launcher.hide()
            self.quick_settings.hide()
        elif action == "power":
            self.toggle_quick_settings(None, "power")
        elif action == "help":
            self.help.show(self.current_monitor())
        elif action in ("volume-up", "volume-down", "volume-mute"):
            if not self.audio.available:
                return
            if action == "volume-mute":
                self.audio.toggle_mute()
            else:
                self.audio.step(0.05 if action == "volume-up" else -0.05)
            self.osd.show(self.audio.icon(), 0 if self.audio.muted else self.audio.volume)
        elif action == "mic-mute":
            self.audio.toggle_mic()
        elif action in ("brightness-up", "brightness-down"):
            if not self.brightness.available:
                return
            self.brightness.step(0.05 if action == "brightness-up" else -0.05)
            self.osd.show("display-brightness-symbolic", self.brightness.level)
        elif action.startswith("media-") and shutil.which("playerctl"):
            command = {"media-play-pause": "play-pause", "media-next": "next",
                       "media-previous": "previous"}.get(action)
            if command:
                subprocess.Popen(["playerctl", command],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        elif action in ("screenshot", "screenshot-area"):
            self.screenshot(area=action == "screenshot-area")

    # ------------------------------------------------------------ screenshots

    def screenshot(self, area=False):
        if not shutil.which("grim"):
            self.notifications.notify_internal("Screenshots need the “grim” program",
                                               "It is installed by the Tessera installer.")
            return
        self.launcher.hide()
        self.quick_settings.hide()
        folder = GLib.get_user_special_dir(GLib.UserDirectory.DIRECTORY_PICTURES) or \
            os.path.expanduser("~/Pictures")
        folder = os.path.join(folder, "Screenshots")
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, time.strftime("Screenshot %Y-%m-%d %H-%M-%S.png"))

        # Everything runs in the background, so the bar and notifications
        # keep working while an area is being chosen.
        def saved(ok, _out):
            if not ok:
                return
            if shutil.which("wl-copy"):
                with open(path, "rb") as f:
                    subprocess.Popen(["wl-copy", "--type", "image/png"], stdin=f,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.notifications.notify_internal(
                "Screenshot saved", "In Pictures › Screenshots, and copied so you can paste it.",
                path, ["open", "Open"],
                lambda key: Gio.AppInfo.launch_default_for_uri(
                    GLib.filename_to_uri(path, None), None))

        def capture(geometry=None):
            services.run_async(["grim"] + (["-g", geometry] if geometry else []) + [path], saved)

        def area_chosen(proc, result):
            try:
                _ok, out, _err = proc.communicate_utf8_finish(result)
            except GLib.Error:
                return
            geometry = (out or "").strip()
            if proc.get_successful() and geometry:  # nothing if Escape was pressed
                capture(geometry)

        def start():
            if not area:
                capture()
                return False
            if not shutil.which("slurp"):
                self.notifications.notify_internal("Area screenshots need the “slurp” program",
                                                   "It is installed by the Tessera installer.")
                return False
            try:
                proc = Gio.Subprocess.new(["slurp"], Gio.SubprocessFlags.STDOUT_PIPE |
                                          Gio.SubprocessFlags.STDERR_SILENCE)
            except GLib.Error:
                return False
            proc.communicate_utf8_async(None, None, area_chosen)
            return False

        # Let the pop-ups disappear before capturing the screen.
        GLib.timeout_add(250, start)

    # ---------------------------------------------------------------- battery

    def _check_battery(self):
        bat = self.battery
        if not bat.present:
            return
        if not bat.charging and bat.percent <= 10 and not self._low_battery_warned:
            self._low_battery_warned = True
            self.notifications.show("Battery", 0, "battery-caution-symbolic", "Battery is low",
                                    f"{bat.percent}% left. Plug in your charger soon.",
                                    [], {"urgency": 2}, 0)
        elif bat.charging or bat.percent > 15:
            self._low_battery_warned = False


def main():
    app = Gtk.Application(application_id="org.tessera.Shell",
                          flags=Gio.ApplicationFlags.DEFAULT_FLAGS)
    holder = {}

    def activate(application):
        if "shell" in holder:
            return
        application.hold()
        holder["shell"] = Shell(application)

    app.connect("activate", activate)
    return app.run(sys.argv[:1])


if __name__ == "__main__":
    sys.exit(main())

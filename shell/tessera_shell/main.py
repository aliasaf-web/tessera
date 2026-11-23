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
from .theme import Theme, apply_app_theme, read_config  # noqa: E402


class Shell:
    def __init__(self, app):
        self.app = app
        self.state = {}
        cfg = read_config()
        try:
            self.gaps = max(0, min(64, int(cfg.get("gaps", "14"))))
        except ValueError:
            self.gaps = 14

        self.theme = Theme()
        apply_app_theme(self.theme.dark)
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

    def current_monitor(self):
        # The compositor places a popup on the screen in use when no monitor
        # is given; return None so that behaviour applies.
        return None

    def set_status_open(self, is_open):
        for bar in self.bars.values():
            bar.set_open(is_open)

    # ---------------------------------------------------------------- toggles

    def toggle_launcher(self):
        if self.quick_settings.visible:
            self.quick_settings.hide()
        self.launcher.toggle(self.current_monitor())

    def toggle_quick_settings(self, monitor=None, page=None):
        if self.launcher.visible:
            self.launcher.hide()
        self.quick_settings.toggle(monitor, page)

    # ----------------------------------------------------------------- events

    def on_event(self, event):
        kind = event.get("event")
        if kind == "state":
            self.state = event
            for bar in self.bars.values():
                bar.update_workspaces(event)
            if event.get("locked"):
                self.launcher.hide()
                self.quick_settings.hide()
                self.help.window.set_visible(False)
            if self._welcome_pending and not event.get("locked"):
                self._welcome_pending = False
                GLib.timeout_add(800, lambda: (self.help.show(self.current_monitor()), False)[1])
            if event.get("theme") and event.get("theme") != self.theme.name:
                self.theme.reload()
                self.quick_settings.update_theme()
        elif kind == "key":
            self.on_key(event.get("action", ""))

    def on_key(self, action):
        if action == "launcher":
            self.toggle_launcher()
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

        def take():
            args = ["grim"]
            if area:
                if not shutil.which("slurp"):
                    return False
                sel = subprocess.run(["slurp"], capture_output=True, text=True, check=False)
                if sel.returncode != 0 or not sel.stdout.strip():
                    return False
                args += ["-g", sel.stdout.strip()]
            args.append(path)
            if subprocess.run(args, check=False).returncode != 0:
                return False
            if shutil.which("wl-copy"):
                with open(path, "rb") as f:
                    subprocess.Popen(["wl-copy", "--type", "image/png"], stdin=f,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.notifications.notify_internal(
                "Screenshot saved", "In Pictures › Screenshots, and copied so you can paste it.",
                path, ["open", "Open"],
                lambda key: Gio.AppInfo.launch_default_for_uri(
                    GLib.filename_to_uri(path, None), None))
            return False

        # Let the pop-ups disappear before capturing the screen.
        GLib.timeout_add(250, take)

    # ---------------------------------------------------------------- battery

    def _check_battery(self):
        bat = self.battery
        if not bat.present:
            return
        if not bat.charging and bat.percent <= 10 and not self._low_battery_warned:
            self._low_battery_warned = True
            self.notifications.show("Battery", 0, "battery-caution-symbolic",
                                    "Battery is low", f"{bat.percent}% left. Plug in your charger soon.",
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

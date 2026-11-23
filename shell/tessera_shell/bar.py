"""The bar along the bottom of the screen."""

import time

from gi.repository import GLib, Gtk

from . import ipc
from .layer import Edge, Layer, button, clear, hbox, icon, label, layer_window


class Bar:
    HEIGHT = 44

    def __init__(self, shell, monitor):
        self.shell = shell
        self.monitor = monitor
        gaps = shell.gaps
        self.window = layer_window(
            shell.app, "tessera-bar", Layer.TOP,
            (Edge.BOTTOM, Edge.LEFT, Edge.RIGHT),
            {Edge.BOTTOM: 12, Edge.LEFT: gaps, Edge.RIGHT: gaps},
            monitor=monitor, exclusive=True)
        self.window.set_default_size(-1, self.HEIGHT)

        center = Gtk.CenterBox()
        center.add_css_class("bar")

        apps = button(icon("view-app-grid-symbolic", 18), css="round",
                      tooltip="All apps  (Super)", on_click=shell.toggle_launcher)
        self.ws_box = hbox(0)
        self.ws_box.add_css_class("pill")
        self.ws_box.add_css_class("workspaces")
        self.ws_box.set_valign(Gtk.Align.FILL)
        left = hbox(10, apps, self.ws_box)
        center.set_start_widget(left)

        self.net_icon = icon("network-wireless-offline-symbolic")
        self.vol_icon = icon("audio-volume-high-symbolic")
        self.bat_icon = icon("battery-good-symbolic")
        self.bat_label = label("", "percent")
        self.clock = label("", "clock")
        self.bt_icon = icon("bluetooth-active-symbolic")
        status_box = hbox(12, self.net_icon, self.bt_icon, self.vol_icon,
                          hbox(4, self.bat_icon, self.bat_label), self.clock)
        self.status = button(status_box, css="status", tooltip="Quick settings",
                             on_click=lambda: shell.toggle_quick_settings(self.monitor))
        center.set_end_widget(self.status)

        self.window.set_child(center)
        self.window.present()

        shell.network.connect(self.update_network)
        shell.audio.connect(self.update_audio)
        shell.battery.connect(self.update_battery)
        shell.bluetooth.connect(self.update_bluetooth)
        self.update_clock()
        self._clock_source = GLib.timeout_add_seconds(self._seconds_to_minute(), self._tick)
        self.update_workspaces(shell.state)

    def destroy(self):
        if self._clock_source:
            GLib.source_remove(self._clock_source)
            self._clock_source = None
        self.window.destroy()

    @staticmethod
    def _seconds_to_minute():
        return max(1, 60 - time.localtime().tm_sec)

    def _tick(self):
        self.update_clock()
        self._clock_source = GLib.timeout_add_seconds(self._seconds_to_minute(), self._tick)
        return False

    def update_clock(self):
        self.clock.set_label(time.strftime("%H:%M"))
        self.status.set_tooltip_text(time.strftime("%A %-d %B %Y"))

    def update_network(self):
        net = self.shell.network
        self.net_icon.set_from_icon_name(net.icon())
        self.net_icon.set_visible(net.available)
        self.net_icon.set_tooltip_text(net.describe())

    def update_audio(self):
        audio = self.shell.audio
        self.vol_icon.set_from_icon_name(audio.icon())
        self.vol_icon.set_visible(audio.available)

    def update_battery(self):
        bat = self.shell.battery
        self.bat_icon.get_parent().set_visible(bat.present)
        if bat.present:
            self.bat_icon.set_from_icon_name(bat.icon_name)
            self.bat_label.set_label(f"{bat.percent}%")

    def update_bluetooth(self):
        bt = self.shell.bluetooth
        self.bt_icon.set_visible(bool(bt.available and bt.powered and bt.connected_names()))

    def set_open(self, is_open):
        if is_open:
            self.status.add_css_class("open")
        else:
            self.status.remove_css_class("open")

    def update_workspaces(self, state):
        clear(self.ws_box)
        current = state.get("workspace", 1)
        occupied = set(state.get("occupied", []))
        count = max([3, current] + list(occupied))
        for i in range(1, count + 1):
            dot = Gtk.Button()
            dot.add_css_class("ws-dot")
            if i == current:
                dot.add_css_class("current")
            elif i in occupied:
                dot.add_css_class("occupied")
            dot.set_valign(Gtk.Align.CENTER)
            dot.set_halign(Gtk.Align.CENTER)
            dot.set_focus_on_click(False)
            dot.set_can_target(False)
            hit = Gtk.Button()
            hit.add_css_class("ws-hit")
            hit.set_child(dot)
            hit.set_tooltip_text(f"Workspace {i}  (Super+{i})")
            hit.set_focus_on_click(False)
            hit.connect("clicked", lambda _b, n=i: ipc.send("workspace", n))
            self.ws_box.append(hit)

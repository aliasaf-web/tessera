"""The small pop-up that shows volume or brightness while you change it."""

from gi.repository import GLib, Gtk

from .layer import Edge, Layer, hbox, icon, layer_window, set_monitor


class Osd:
    def __init__(self, shell):
        self.shell = shell
        self.window = layer_window(shell.app, "tessera-osd", Layer.OVERLAY,
                                   (Edge.BOTTOM,), {Edge.BOTTOM: 12 + 44 + 24})
        self.icon = icon("audio-volume-high-symbolic", 20)
        self.level = Gtk.LevelBar()
        self.level.add_css_class("osd-level")
        self.level.set_min_value(0)
        self.level.set_max_value(1)
        self.level.set_size_request(220, -1)
        self.level.set_valign(Gtk.Align.CENTER)
        # Remove the default offsets so the bar uses the accent colour throughout.
        for name in ("low", "high", "full"):
            self.level.remove_offset_value(name)
        box = hbox(14, self.icon, self.level)
        box.add_css_class("osd")
        self.window.set_child(box)
        self.timer = None

    def show(self, icon_name, value, monitor=None):
        self.icon.set_from_icon_name(icon_name)
        self.level.set_value(max(0.0, min(1.0, value)))
        set_monitor(self.window, monitor)
        self.window.present()
        if self.timer:
            GLib.source_remove(self.timer)
        self.timer = GLib.timeout_add(1400, self._hide)

    def _hide(self):
        self.timer = None
        self.window.set_visible(False)
        return False

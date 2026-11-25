"""The Alt+Tab window switcher. The window manager decides which window
is chosen; this only shows the list it sends."""

from gi.repository import Gio, GLib, Gtk, Pango

from . import ipc
from .layer import Layer, clear, hbox, label, layer_window, vbox

_app_cache = {}


def app_for_window(app_id):
    """Find the installed app a window belongs to, for its icon and name."""
    if not app_id:
        return None
    key = app_id.lower()
    if key in _app_cache:
        return _app_cache[key]
    found = None
    for candidate in (app_id, key):
        try:
            found = Gio.DesktopAppInfo.new(candidate + ".desktop")
        except TypeError:
            found = None
        if found is not None:
            break
    if found is None:
        for app in Gio.AppInfo.get_all():
            if not isinstance(app, Gio.DesktopAppInfo):
                continue
            desktop_id = (app.get_id() or "").lower().removesuffix(".desktop")
            wm_class = (app.get_startup_wm_class() or "").lower()
            if key in (wm_class, desktop_id) or desktop_id.endswith("." + key):
                found = app
                break
    _app_cache[key] = found
    return found


class Switcher:
    MAX_SHOWN = 8

    def __init__(self, shell):
        self.shell = shell
        self.window = layer_window(shell.app, "tessera-switcher", Layer.OVERLAY)
        card = vbox(12)
        card.add_css_class("card")
        card.add_css_class("switcher")
        self.row = hbox(6)
        self.row.set_halign(Gtk.Align.CENTER)
        self.caption = label("", xalign=0.5, ellipsize=True)
        self.caption.set_max_width_chars(60)
        self.where = label("", "dim", xalign=0.5)
        card.append(self.row)
        card.append(self.caption)
        card.append(self.where)
        outer = Gtk.Box()  # the window's direct child is kept transparent
        outer.append(card)
        self.window.set_child(outer)
        self.pending = None

    def update(self, event):
        if not event.get("visible"):
            if self.pending:
                GLib.source_remove(self.pending)
                self.pending = None
            self.window.set_visible(False)
            return
        windows = event.get("windows", [])
        selected = event.get("selected", 0)
        clear(self.row)
        # Show a window of up to MAX_SHOWN entries around the selection.
        start = max(0, min(selected - self.MAX_SHOWN // 2, len(windows) - self.MAX_SHOWN))
        for index in range(start, min(len(windows), start + self.MAX_SHOWN)):
            win = windows[index]
            app = app_for_window(win.get("app_id", ""))
            image = Gtk.Image()
            if app is not None and app.get_icon() is not None:
                image.set_from_gicon(app.get_icon())
            else:
                image.set_from_icon_name("application-x-executable")
            image.set_pixel_size(48)
            name = ((app.get_display_name() if app else "") or win.get("app_id")
                    or win.get("title") or "Window")
            text = Gtk.Label(label=name)
            text.set_ellipsize(Pango.EllipsizeMode.END)
            text.set_max_width_chars(12)
            item = Gtk.Button()
            item.add_css_class("switch-item")
            if index == selected:
                item.add_css_class("selected")
            item.set_child(vbox(8, image, text))
            item.set_focus_on_click(False)
            item.connect("clicked", lambda _b, wid=win["id"]: ipc.send("focus", wid))
            self.row.append(item)
        if 0 <= selected < len(windows):
            win = windows[selected]
            self.caption.set_label(win.get("title") or "")
            # Only for a window that no screen is showing now.
            ws = win.get("workspace", 0)
            hidden = bool(ws and win.get("hidden"))
            self.where.set_label(f"On workspace {ws}" if hidden else "")
            self.where.set_visible(hidden)
        if not self.window.get_visible() and self.pending is None:
            # A quick Alt+Tab tap switches without flashing the list.
            self.pending = GLib.timeout_add(120, self._show)

    def _show(self):
        self.pending = None
        self.window.present()
        return False

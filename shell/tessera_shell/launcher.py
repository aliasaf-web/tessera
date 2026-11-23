"""The app launcher: search box, all apps, and the power buttons."""

import os
import pwd

from gi.repository import Gdk, Gio, GLib, Gtk, Pango

from . import ipc, services
from .layer import (button, click_outside_closes, fullscreen_overlay, hbox, icon,
                    label, on_escape, set_monitor, vbox)


def display_name():
    try:
        pw = pwd.getpwuid(os.getuid())
        name = (pw.pw_gecos or "").split(",")[0].strip()
        return name or pw.pw_name
    except KeyError:
        return os.environ.get("USER", "")


class ConfirmButton:
    """A button that asks "are you sure?" by needing a second click."""

    def __init__(self, icon_name, text, confirm_text, action):
        self.text = text
        self.confirm_text = confirm_text
        self.action = action
        self.armed = False
        self.lbl = label(text)
        self.widget = button(hbox(8, icon(icon_name), self.lbl), css="flat",
                             on_click=self.clicked)

    def clicked(self):
        if self.armed:
            self.reset()
            self.action()
            return
        self.armed = True
        self.lbl.set_label(self.confirm_text)
        self.widget.add_css_class("danger")
        GLib.timeout_add_seconds(4, self.reset)

    def reset(self):
        self.armed = False
        self.lbl.set_label(self.text)
        self.widget.remove_css_class("danger")
        return False


class Launcher:
    def __init__(self, shell):
        self.shell = shell
        self.window = fullscreen_overlay(shell.app, "tessera-launcher")
        self.window.connect("close-request", lambda *_: (self.hide(), True)[1])

        backdrop = Gtk.Box()
        backdrop.add_css_class("backdrop")

        self.card = vbox(16)
        self.card.add_css_class("card")
        self.card.set_size_request(660, 580)
        self.card.set_halign(Gtk.Align.CENTER)
        self.card.set_valign(Gtk.Align.CENTER)
        self.card.set_hexpand(True)
        self.card.set_vexpand(True)

        self.search = Gtk.SearchEntry()
        self.search.add_css_class("search")
        self.search.set_placeholder_text("Type to search apps")
        self.search.connect("search-changed", lambda *_: self.refilter())
        self.search.connect("activate", lambda *_: self.launch_first())
        self.search.set_key_capture_widget(self.window)
        self.card.append(self.search)

        self.flow = Gtk.FlowBox()
        self.flow.add_css_class("apps")
        self.flow.set_selection_mode(Gtk.SelectionMode.NONE)
        self.flow.set_homogeneous(True)
        self.flow.set_min_children_per_line(5)
        self.flow.set_max_children_per_line(5)
        self.flow.set_row_spacing(4)
        self.flow.set_column_spacing(4)
        self.flow.set_valign(Gtk.Align.START)
        self.flow.set_filter_func(self._filter)

        scroller = Gtk.ScrolledWindow()
        scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scroller.set_vexpand(True)
        scroller.set_child(self.flow)
        self.card.append(scroller)

        self.empty = label("No apps match your search", "dim", xalign=0.5)
        self.empty.set_visible(False)
        self.card.append(self.empty)

        name = display_name()
        avatar = label((name[:1] or "?").upper(), "avatar", xalign=0.5)
        avatar.set_valign(Gtk.Align.CENTER)
        who = hbox(10, avatar, label(name))
        who.set_hexpand(True)
        footer = hbox(8, who)
        footer.append(button(hbox(8, icon("system-lock-screen-symbolic"), label("Lock")),
                             css="flat", on_click=self.lock))
        footer.append(ConfirmButton("system-log-out-symbolic", "Log out", "Really log out?",
                                    lambda: ipc.send("logout")).widget)
        footer.append(ConfirmButton("system-reboot-symbolic", "Restart", "Really restart?",
                                    services.reboot).widget)
        footer.append(ConfirmButton("system-shutdown-symbolic", "Shut down", "Really shut down?",
                                    services.power_off).widget)
        self.card.append(footer)

        backdrop.append(self.card)
        self.window.set_child(backdrop)
        click_outside_closes(backdrop, self.card, self.hide)
        on_escape(self.window, self.hide)

        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", self._on_key)
        self.window.add_controller(keys)

        self.apps = []
        self.monitor = Gio.AppInfoMonitor.get()
        self.monitor.connect("changed", lambda *_: self.load_apps())
        self.load_apps()

    @property
    def visible(self):
        return self.window.get_visible()

    def load_apps(self):
        child = self.flow.get_first_child()
        while child is not None:
            nxt = child.get_next_sibling()
            self.flow.remove(child)
            child = nxt
        apps = [a for a in Gio.AppInfo.get_all() if a.should_show()]
        apps.sort(key=lambda a: (a.get_display_name() or "").lower())
        self.apps = apps
        for app in apps:
            image = Gtk.Image()
            gicon = app.get_icon()
            if gicon is not None:
                image.set_from_gicon(gicon)
            else:
                image.set_from_icon_name("application-x-executable")
            image.set_pixel_size(48)
            name = Gtk.Label(label=app.get_display_name() or "")
            name.set_wrap(True)
            name.set_lines(2)
            name.set_ellipsize(Pango.EllipsizeMode.END)
            name.set_justify(Gtk.Justification.CENTER)
            name.set_max_width_chars(12)
            box = vbox(8, image, name)
            btn = Gtk.Button()
            btn.add_css_class("app")
            btn.set_child(box)
            btn.set_tooltip_text(app.get_description() or app.get_display_name())
            btn.connect("clicked", lambda _b, a=app: self.launch(a))
            child = Gtk.FlowBoxChild()
            child.set_child(btn)
            child.app = app
            child.search_text = self._search_text(app)
            self.flow.append(child)

    @staticmethod
    def _search_text(app):
        parts = [app.get_display_name() or "", app.get_name() or "",
                 app.get_description() or "", app.get_executable() or ""]
        if isinstance(app, Gio.DesktopAppInfo):
            parts.append(app.get_generic_name() or "")
            parts.extend(app.get_keywords() or [])
        return " ".join(parts).lower()

    def _filter(self, child):
        query = self.search.get_text().strip().lower()
        if not query:
            return True
        return all(word in child.search_text for word in query.split())

    def refilter(self):
        self.flow.invalidate_filter()
        self.empty.set_visible(self._first_visible() is None)

    def _first_visible(self):
        child = self.flow.get_first_child()
        while child is not None:
            if child.get_child_visible() and self._filter(child):
                return child
            child = child.get_next_sibling()
        return None

    def _on_key(self, ctrl, keyval, keycode, state):
        if keyval in (Gdk.KEY_Down, Gdk.KEY_Tab) and self.search.has_focus():
            first = self._first_visible()
            if first is not None:
                first.get_child().grab_focus()
                return True
        return False

    def launch_first(self):
        first = self._first_visible()
        if first is not None:
            self.launch(first.app)

    def launch(self, app):
        self.hide()
        ctx = Gdk.Display.get_default().get_app_launch_context()
        ctx.unsetenv("LD_PRELOAD")
        ctx.unsetenv("PYTHONPATH")
        try:
            app.launch([], ctx)
        except GLib.Error as exc:
            self.shell.notifications.notify_internal(
                "Could not open " + (app.get_display_name() or "the app"), exc.message,
                "dialog-error-symbolic")

    def lock(self):
        self.hide()
        ipc.send("lock")

    def show(self, monitor=None):
        set_monitor(self.window, monitor)
        self.search.set_text("")
        self.refilter()
        self.window.present()
        self.search.grab_focus()
        adj = self.flow.get_parent()
        while adj is not None and not isinstance(adj, Gtk.ScrolledWindow):
            adj = adj.get_parent()
        if adj is not None:
            adj.get_vadjustment().set_value(0)

    def hide(self):
        self.window.set_visible(False)

    def toggle(self, monitor=None):
        if self.visible:
            self.hide()
        else:
            self.show(monitor)

"""Notification server (org.freedesktop.Notifications) and pop-ups."""

import html
import re
import time

from gi.repository import GdkPixbuf, Gdk, Gio, GLib, Gtk, Gtk4LayerShell as LayerShell, Pango

from . import __version__
from .layer import Edge, Layer, button, hbox, icon, label, layer_window, vbox

INTROSPECTION = """
<node>
  <interface name="org.freedesktop.Notifications">
    <method name="GetCapabilities">
      <arg type="as" direction="out"/>
    </method>
    <method name="Notify">
      <arg type="s" name="app_name" direction="in"/>
      <arg type="u" name="replaces_id" direction="in"/>
      <arg type="s" name="app_icon" direction="in"/>
      <arg type="s" name="summary" direction="in"/>
      <arg type="s" name="body" direction="in"/>
      <arg type="as" name="actions" direction="in"/>
      <arg type="a{sv}" name="hints" direction="in"/>
      <arg type="i" name="expire_timeout" direction="in"/>
      <arg type="u" direction="out"/>
    </method>
    <method name="CloseNotification">
      <arg type="u" name="id" direction="in"/>
    </method>
    <method name="GetServerInformation">
      <arg type="s" direction="out"/>
      <arg type="s" direction="out"/>
      <arg type="s" direction="out"/>
      <arg type="s" direction="out"/>
    </method>
    <signal name="NotificationClosed">
      <arg type="u" name="id"/>
      <arg type="u" name="reason"/>
    </signal>
    <signal name="ActionInvoked">
      <arg type="u" name="id"/>
      <arg type="s" name="action_key"/>
    </signal>
  </interface>
</node>
"""

ALLOWED_TAGS = re.compile(r"</?(b|i|u)>")


def safe_markup(body):
    """Keep the simple formatting the spec allows, escape everything else."""
    escaped = html.escape(html.unescape(re.sub(r"<a [^>]*>|</a>|<img[^>]*>", "", body)),
                          quote=False)
    escaped = re.sub(r"&lt;(/?)(b|i|u)&gt;", r"<\1\2>", escaped)
    try:
        Pango.parse_markup(escaped, -1, "\0")
        return escaped
    except GLib.Error:
        return html.escape(re.sub(r"<[^>]+>", "", body), quote=False)


def plain(body):
    return html.unescape(re.sub(r"<[^>]+>", "", body)).replace("\n", " ")


def pixbuf_from_hint(value):
    """Decode the `image-data` hint: (iiibiiay)."""
    try:
        width, height, rowstride, has_alpha, bits, channels, data = value
        return GdkPixbuf.Pixbuf.new_from_bytes(GLib.Bytes.new(bytes(data)),
                                               GdkPixbuf.Colorspace.RGB, has_alpha,
                                               bits, width, height, rowstride)
    except (TypeError, ValueError, GLib.Error):
        return None


class Notifications:
    DEFAULT_TIMEOUT = 6000
    MAX_VISIBLE = 4

    def __init__(self, shell):
        self.shell = shell
        self.next_id = 1
        self.active = {}      # id -> dict with widgets
        self.history = []     # newest first
        self.dnd = False
        self._history_listeners = []
        self.connection = None

        self.window = layer_window(shell.app, "tessera-notifications", Layer.OVERLAY,
                                   (Edge.TOP, Edge.RIGHT))
        self.place()
        self.column = vbox(10)
        self.column.set_size_request(360, -1)
        self.window.set_child(self.column)

        node = Gio.DBusNodeInfo.new_for_xml(INTROSPECTION)
        self.iface = node.interfaces[0]
        Gio.bus_own_name(Gio.BusType.SESSION, "org.freedesktop.Notifications",
                         Gio.BusNameOwnerFlags.REPLACE | Gio.BusNameOwnerFlags.ALLOW_REPLACEMENT,
                         self._bus_acquired, None,
                         lambda *_: print("tessera-shell: another notification service is running"))

    def place(self):
        gaps = self.shell.metrics(None)["gaps"]
        LayerShell.set_margin(self.window, Edge.TOP, gaps)
        LayerShell.set_margin(self.window, Edge.RIGHT, gaps)

    # ---------------------------------------------------------------- D-Bus

    def _bus_acquired(self, connection, name):
        self.connection = connection
        connection.register_object("/org/freedesktop/Notifications", self.iface,
                                   self._method_call, None, None)

    def _method_call(self, conn, sender, path, iface, method, params, invocation):
        if method == "GetCapabilities":
            invocation.return_value(GLib.Variant("(as)", (["body", "body-markup", "actions",
                                                           "icon-static", "persistence"],)))
        elif method == "GetServerInformation":
            invocation.return_value(GLib.Variant("(ssss)", ("Tessera", "Tessera", __version__, "1.2")))
        elif method == "CloseNotification":
            (nid,) = params.unpack()
            self.close(nid, 3)
            invocation.return_value(None)
        elif method == "Notify":
            app_name, replaces, app_icon, summary, body, actions, hints, timeout = params.unpack()
            nid = self.show(app_name, replaces, app_icon, summary, body, actions, hints, timeout)
            invocation.return_value(GLib.Variant("(u)", (nid,)))
        else:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod", method)

    def _emit(self, signal, variant):
        if self.connection is not None:
            try:
                self.connection.emit_signal(None, "/org/freedesktop/Notifications",
                                            "org.freedesktop.Notifications", signal, variant)
            except GLib.Error:
                pass

    # ---------------------------------------------------------------- logic

    def notify_internal(self, summary, body="", icon_name="dialog-information-symbolic",
                        actions=None, on_action=None):
        nid = self.show("Tessera", 0, icon_name, summary, body, actions or [], {}, -1)
        if on_action:
            self.active[nid]["callback"] = on_action
        return nid

    def show(self, app_name, replaces, app_icon, summary, body, actions, hints, timeout):
        urgency = hints.get("urgency", 1) if isinstance(hints, dict) else 1
        if isinstance(urgency, GLib.Variant):
            urgency = urgency.unpack()
        if replaces and (replaces in self.active or
                         any(h["id"] == replaces for h in self.history)):
            nid = replaces
        else:
            nid = self.next_id
            self.next_id += 1

        entry = {
            "id": nid, "app": app_name, "summary": summary or app_name,
            "body": body, "body_plain": plain(body), "time": time.strftime("%H:%M"),
        }
        if not hints.get("transient", False):
            self.history = [h for h in self.history if h["id"] != nid]
            self.history.insert(0, entry)
            del self.history[30:]
            self._history_changed()

        if self.dnd and urgency < 2:
            return nid

        if nid in self.active:
            self._remove_widget(nid)

        card = hbox(12)
        card.add_css_class("toast")
        if urgency >= 2:
            card.add_css_class("critical")

        image = None
        data = hints.get("image-data") or hints.get("image_data") or hints.get("icon_data")
        if data is not None:
            pix = pixbuf_from_hint(data)
            if pix is not None:
                image = Gtk.Image.new_from_paintable(Gdk.Texture.new_for_pixbuf(pix))
        path = hints.get("image-path") or app_icon
        if image is None and path:
            if path.startswith("file://"):
                path = GLib.filename_from_uri(path)[0]
            if path.startswith("/"):
                image = Gtk.Image.new_from_file(path)
            else:
                image = Gtk.Image.new_from_icon_name(path)
        if image is not None:
            image.set_pixel_size(36)
            image.set_valign(Gtk.Align.START)
            card.append(image)

        text = vbox(4)
        text.set_hexpand(True)
        title = label(summary or app_name, "summary", wrap=True)
        title.set_max_width_chars(34)
        text.append(title)
        if body:
            body_lbl = Gtk.Label()
            body_lbl.set_markup(safe_markup(body))
            body_lbl.add_css_class("body")
            body_lbl.set_xalign(0)
            body_lbl.set_wrap(True)
            body_lbl.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
            body_lbl.set_max_width_chars(38)
            body_lbl.set_lines(4)
            body_lbl.set_ellipsize(Pango.EllipsizeMode.END)
            text.append(body_lbl)

        pairs = list(zip(actions[0::2], actions[1::2]))
        buttons = [(k, v) for k, v in pairs if k != "default"]
        if buttons:
            row = hbox(6)
            for key, name in buttons[:3]:
                row.append(button(text=name, css="flat",
                                  on_click=lambda k=key, i=nid: self.invoke(i, k)))
            text.append(row)
        card.append(text)

        close_btn = button(icon("window-close-symbolic", 14), css="icon",
                           tooltip="Dismiss", on_click=lambda i=nid: self.close(i, 2))
        close_btn.set_valign(Gtk.Align.START)
        card.append(close_btn)

        click = Gtk.GestureClick()
        has_default = any(k == "default" for k, _ in pairs)
        click.connect("released", lambda *_a, i=nid, d=has_default:
                      self.invoke(i, "default") if d else self.close(i, 2))
        card.add_controller(click)

        self.column.prepend(card)
        self.active[nid] = {"widget": card, "timer": None, "callback": None}
        if timeout != 0 and urgency < 2:
            ms = self.DEFAULT_TIMEOUT if timeout < 0 else max(timeout, 1500)
            self.active[nid]["timer"] = GLib.timeout_add(ms, self._expire, nid)
        self._trim()
        self.window.present()
        return nid

    def _trim(self):
        ids = list(self.active.keys())
        while len(ids) > self.MAX_VISIBLE:
            self.close(ids.pop(0), 1)

    def _expire(self, nid):
        if nid in self.active:
            self.active[nid]["timer"] = None
            self.close(nid, 1)
        return False

    def _remove_widget(self, nid):
        info = self.active.pop(nid, None)
        if info is None:
            return
        if info["timer"]:
            GLib.source_remove(info["timer"])
        self.column.remove(info["widget"])
        if not self.active:
            self.window.set_visible(False)

    def close(self, nid, reason):
        if nid in self.active:
            self._remove_widget(nid)
        self._emit("NotificationClosed", GLib.Variant("(uu)", (nid, reason)))

    def invoke(self, nid, key):
        info = self.active.get(nid)
        if info and info.get("callback"):
            info["callback"](key)
        self._emit("ActionInvoked", GLib.Variant("(us)", (nid, key)))
        self.close(nid, 2)

    def connect_history(self, callback):
        self._history_listeners.append(callback)
        callback()

    def _history_changed(self):
        for cb in self._history_listeners:
            cb()

    def clear_history(self):
        self.history = []
        self._history_changed()

    def hide_all(self):
        for nid in list(self.active):
            self.close(nid, 2)

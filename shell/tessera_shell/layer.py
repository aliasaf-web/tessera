"""Helpers for windows that sit on the desktop (panels, popups, overlays)."""

from gi.repository import Gtk, Gtk4LayerShell as LayerShell

Edge = LayerShell.Edge
Layer = LayerShell.Layer
Keyboard = LayerShell.KeyboardMode


def layer_window(app, namespace, layer=Layer.TOP, anchors=(), margins=None,
                 keyboard=Keyboard.NONE, monitor=None, exclusive=False):
    win = Gtk.ApplicationWindow(application=app)
    win.add_css_class("tessera-layer")
    win.set_decorated(False)
    LayerShell.init_for_window(win)
    LayerShell.set_namespace(win, namespace)
    LayerShell.set_layer(win, layer)
    for edge in anchors:
        LayerShell.set_anchor(win, edge, True)
    for edge, value in (margins or {}).items():
        LayerShell.set_margin(win, edge, value)
    LayerShell.set_keyboard_mode(win, keyboard)
    if monitor is not None:
        LayerShell.set_monitor(win, monitor)
    if exclusive:
        LayerShell.auto_exclusive_zone_enable(win)
    return win


def fullscreen_overlay(app, namespace, keyboard=Keyboard.EXCLUSIVE):
    """A window covering the whole screen, used behind menus and panels
    so that clicking anywhere outside closes them."""
    return layer_window(app, namespace, Layer.OVERLAY,
                        (Edge.TOP, Edge.BOTTOM, Edge.LEFT, Edge.RIGHT),
                        keyboard=keyboard)


def set_monitor(win, monitor):
    if monitor is not None:
        LayerShell.set_monitor(win, monitor)


def icon(name, size=16):
    image = Gtk.Image.new_from_icon_name(name)
    image.set_pixel_size(size)
    return image


def label(text="", css=None, xalign=0.0, wrap=False, ellipsize=False):
    lbl = Gtk.Label(label=text)
    lbl.set_xalign(xalign)
    if css:
        for c in css.split():
            lbl.add_css_class(c)
    if wrap:
        lbl.set_wrap(True)
    if ellipsize:
        from gi.repository import Pango
        lbl.set_ellipsize(Pango.EllipsizeMode.END)
    return lbl


def button(child=None, text=None, css=None, tooltip=None, on_click=None):
    btn = Gtk.Button()
    if child is not None:
        btn.set_child(child)
    elif text is not None:
        btn.set_label(text)
    if css:
        for c in css.split():
            btn.add_css_class(c)
    if tooltip:
        btn.set_tooltip_text(tooltip)
    if on_click:
        btn.connect("clicked", lambda *_: on_click())
    btn.set_focus_on_click(False)
    return btn


def hbox(spacing=0, *children):
    box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=spacing)
    for child in children:
        box.append(child)
    return box


def vbox(spacing=0, *children):
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=spacing)
    for child in children:
        box.append(child)
    return box


def clear(box):
    child = box.get_first_child()
    while child is not None:
        nxt = child.get_next_sibling()
        box.remove(child)
        child = nxt


def click_outside_closes(backdrop, content, close):
    """Close when a click lands on `backdrop` but outside `content`."""
    gesture = Gtk.GestureClick()

    def pressed(g, n, x, y):
        picked = backdrop.pick(x, y, Gtk.PickFlags.DEFAULT)
        w = picked
        while w is not None:
            if w is content:
                return
            w = w.get_parent()
        close()

    gesture.connect("pressed", pressed)
    backdrop.add_controller(gesture)


def on_escape(window, callback):
    """Call `callback` when Escape is pressed, even while a text box has the
    keyboard (text boxes would otherwise use Escape themselves)."""
    ctrl = Gtk.EventControllerKey()
    ctrl.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)

    def key(c, keyval, keycode, state):
        from gi.repository import Gdk
        if keyval == Gdk.KEY_Escape:
            callback()
            return True
        return False

    ctrl.connect("key-pressed", key)
    window.add_controller(ctrl)

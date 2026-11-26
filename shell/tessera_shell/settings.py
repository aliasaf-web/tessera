"""Tessera Settings: change how the desktop looks and works.

Every choice is written to ~/.config/tessera/config and applied straight
away by asking the window manager to reload its settings."""

import json
import os
import platform
import re
import shutil
import subprocess
import sys

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Adw", "1")
gi.require_version("GdkPixbuf", "2.0")

from gi.repository import Adw, Gdk, GdkPixbuf, Gio, GLib, Gtk  # noqa: E402

from . import __version__, ipc, read_provenance, services  # noqa: E402
from .terminal import terminal_command  # noqa: E402
from .theme import ACCENTS, read_config, write_config_value  # noqa: E402

ACCENT_LABELS = {
    "orange": "Orange", "blue": "Blue", "teal": "Teal", "green": "Green", "yellow": "Yellow",
    "red": "Red", "pink": "Pink", "purple": "Purple", "slate": "Slate",
}

MINUTE_CHOICES = [(1, "1 minute"), (2, "2 minutes"), (5, "5 minutes"), (10, "10 minutes"),
                  (15, "15 minutes"), (30, "30 minutes"), (0, "Never")]

SCALES = [("auto", "Automatic"), ("1", "100%"), ("1.25", "125%"), ("1.5", "150%"),
          ("1.75", "175%"), ("2", "200%")]
SIZE_HINT = "Bigger is easier to read; smaller fits more"

SIZES = [("auto", "Automatic"), ("compact", "Compact"), ("comfortable", "Comfortable")]

FIELD_CODES = re.compile(r"\s*%[fFuUdDnNickvm]")


def data_dir():
    base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "tessera")


def app_command(app):
    """The command that starts an app, without the placeholders from its
    .desktop file (such as %U)."""
    line = app.get_commandline() or app.get_executable() or ""
    return FIELD_CODES.sub("", line).strip()


def run(args):
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=5, check=False)
        return out.returncode, out.stdout
    except (OSError, subprocess.TimeoutExpired):
        return 1, ""


def apply_sound_device(setter, value, on_failure=None):
    """Asks PulseAudio to make `value` the default device, and does not wait.

    Nothing on this page reads anything back from a device change - the row has
    already been chosen - so waiting for pactl would only hold the whole window
    still while it answers. pactl is quick on a healthy machine and can sit for
    seconds on one whose sound server is restarting, which is exactly when the
    settings window is being used. So it goes through the same non-blocking
    runner the rest of the shell uses, which also reaps the child; a failure is
    worth a line to the user, which is what `on_failure` is for.
    """
    def finished(ok, out):
        if not ok and on_failure is not None:
            on_failure(out)

    services.run_async(["pactl", setter, value], finished)


def keyboard_layouts():
    """All keyboard layouts known to the system, as (code, name), by name."""
    layouts = []
    try:
        with open("/usr/share/X11/xkb/rules/evdev.lst", encoding="utf-8") as f:
            section = None
            for line in f:
                if line.startswith("!"):
                    section = line[1:].strip()
                    continue
                if section != "layout" or not line.strip():
                    continue
                code, _, name = line.strip().partition(" ")
                layouts.append((code, name.strip()))
    except OSError:
        pass
    layouts.sort(key=lambda item: item[1].lower())
    return layouts


def system_keyboard():
    try:
        with open("/etc/default/keyboard", encoding="utf-8") as f:
            for line in f:
                if line.startswith("XKBLAYOUT="):
                    return line.split("=", 1)[1].strip().strip('"').split(",")[0]
    except OSError:
        pass
    return "gb"


def os_name():
    try:
        with open("/etc/os-release", encoding="utf-8") as f:
            for line in f:
                if line.startswith("PRETTY_NAME="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return platform.system()


def cpu_name():
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as f:
            for line in f:
                if line.startswith("model name"):
                    return re.sub(r"\s+", " ", line.split(":", 1)[1]).strip()
    except OSError:
        pass
    return platform.machine()


def memory_text():
    try:
        with open("/proc/meminfo", encoding="utf-8") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    kib = int(line.split()[1])
                    return f"{kib / 1024 / 1024:.1f} GB"
    except (OSError, ValueError):
        pass
    return "Unknown"


def build_text():
    """Which build is installed, in the words a person can use when asking
    what went wrong: the commit, the branch it came from, the day it went in,
    and whether the folder also held changes of its own. Empty when nothing is
    installed to report on, and the row is left out rather than shown as
    unknown."""
    note = read_provenance()
    if not note:
        return ""
    parts = []
    if note.get("commit"):
        came_from = "commit " + note["commit"]
        if note.get("branch"):
            came_from += f" on {note['branch']}"
        if note.get("changed"):
            came_from += ", with changes of your own"
        parts.append(came_from)
    if note.get("built"):
        parts.append(f"installed {note['built']}")
    return ", ".join(parts)


def disk_text():
    try:
        usage = shutil.disk_usage("/")
        return f"{usage.free / 1e9:.0f} GB free of {usage.total / 1e9:.0f} GB"
    except OSError:
        return "Unknown"


class SettingsWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Settings")
        self.set_default_size(920, 640)
        self.set_size_request(360, 360)
        self.loading = False
        self.reload_source = None
        self.cfg = read_config()

        self.toasts = Adw.ToastOverlay()
        self.split = Adw.NavigationSplitView()
        self.split.set_min_sidebar_width(200)
        self.split.set_max_sidebar_width(240)

        self.pages = {}
        self.sidebar = Gtk.ListBox()
        self.sidebar.add_css_class("navigation-sidebar")
        self.sidebar.connect("row-selected", self._on_section)

        for key, title, icon_name, builder in (
                ("appearance", "Appearance", "applications-graphics-symbolic",
                 self.page_appearance),
                ("display", "Displays", "video-display-symbolic", self.page_display),
                ("touchpad", "Touchpad & mouse", "input-mouse-symbolic", self.page_touchpad),
                ("keyboard", "Keyboard", "input-keyboard-symbolic", self.page_keyboard),
                ("sound", "Sound", "audio-speakers-symbolic", self.page_sound),
                ("power", "Power & lock", "system-lock-screen-symbolic", self.page_power),
                ("apps", "Default apps", "applications-system-symbolic", self.page_apps),
                ("about", "About", "help-about-symbolic", self.page_about)):
            row = Gtk.ListBoxRow()
            box = Gtk.Box(spacing=12)
            box.set_margin_top(8)
            box.set_margin_bottom(8)
            box.set_margin_start(6)
            image = Gtk.Image.new_from_icon_name(icon_name)
            box.append(image)
            box.append(Gtk.Label(label=title, xalign=0))
            row.set_child(box)
            row.key = key
            self.sidebar.append(row)
            self.pages[key] = (title, builder)

        sidebar_view = Adw.ToolbarView()
        sidebar_view.add_top_bar(Adw.HeaderBar())
        scroller = Gtk.ScrolledWindow()
        scroller.set_child(self.sidebar)
        sidebar_view.set_content(scroller)
        self.split.set_sidebar(Adw.NavigationPage.new(sidebar_view, "Settings"))

        self.content_view = Adw.ToolbarView()
        self.content_view.add_top_bar(Adw.HeaderBar())
        self.content_page = Adw.NavigationPage.new(self.content_view, "")
        self.split.set_content(self.content_page)

        self.toasts.set_child(self.split)
        self.set_content(self.toasts)

        # Narrow windows show one pane at a time.
        breakpoint = Adw.Breakpoint.new(Adw.BreakpointCondition.parse("max-width: 640sp"))
        breakpoint.add_setter(self.split, "collapsed", True)
        self.add_breakpoint(breakpoint)

        self.add_swatch_styles()
        self.apply_style()
        self.sidebar.select_row(self.sidebar.get_row_at_index(0))
        # Follow changes made elsewhere, such as the dark mode tile.
        self.events = ipc.EventStream(self._on_event)

    def _on_event(self, event):
        row = self.sidebar.get_selected_row()
        if event.get("event") == "outputs":
            # A screen was plugged in or out, or the screens were set up again.
            if row is not None and row.key == "display" and self.reload_source is None:
                self.show_page("display")
            return
        if event.get("event") != "reload" or self.reload_source is not None:
            return
        if read_config() == self.cfg:
            return
        if row is not None:
            self.show_page(row.key)
        self.apply_style()

    # ---------------------------------------------------------------- helpers

    def _on_section(self, _box, row):
        if row is None:
            return
        self.show_page(row.key)
        self.split.set_show_content(True)

    def show_page(self, key):
        title, builder = self.pages[key]
        self.cfg = read_config()
        self.loading = True
        try:
            page = builder()
        finally:
            self.loading = False
        self.content_view.set_content(page)
        self.content_page.set_title(title)

    def toast(self, text):
        self.toasts.add_toast(Adw.Toast.new(text))

    def save(self, key, value):
        """Store a setting and apply it shortly afterwards (changes made in
        quick succession, such as dragging a slider, are applied once)."""
        if self.loading:
            return
        try:
            write_config_value(key, value)
        except ValueError as error:
            self.toast(str(error))
            return
        self.cfg[key] = value
        if self.reload_source:
            GLib.source_remove(self.reload_source)
        self.reload_source = GLib.timeout_add(350, self._apply)

    def _apply(self):
        self.reload_source = None
        if ipc.send("reload") is None:
            self.toast("Saved. It will take effect next time you log in.")
        self.apply_style()
        return False

    @staticmethod
    def add_swatch_styles():
        provider = Gtk.CssProvider()
        css = "".join(
            f"button.swatch-{name} {{ background: {colors[0]}; min-width: 26px; min-height: 26px;"
            f" padding: 0; border-radius: 999px; border: 2px solid transparent; }}"
            f" button.swatch-{name}.chosen {{ border-color: @window_fg_color; }}"
            for name, colors in ACCENTS.items())
        if hasattr(provider, "load_from_string"):
            provider.load_from_string(css)
        else:
            provider.load_from_data(css, -1)
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), provider,
                                                  Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    def apply_style(self):
        dark = self.cfg.get("theme", "dark") != "light"
        Adw.StyleManager.get_default().set_color_scheme(
            Adw.ColorScheme.FORCE_DARK if dark else Adw.ColorScheme.FORCE_LIGHT)

    @staticmethod
    def page_with(*groups):
        page = Adw.PreferencesPage()
        for group in groups:
            page.add(group)
        return page

    def combo(self, title, subtitle, choices, current, on_change):
        """A drop-down row. choices: list of (value, label)."""
        row = Adw.ComboRow(title=title)
        if subtitle:
            row.set_subtitle(subtitle)
        row.set_model(Gtk.StringList.new([text for _value, text in choices]))
        values = [value for value, _text in choices]
        if current in values:
            row.set_selected(values.index(current))
        row.connect("notify::selected",
                    lambda r, _p: on_change(values[r.get_selected()])
                    if not self.loading and r.get_selected() < len(values) else None)
        return row

    def switch(self, title, subtitle, active, on_change):
        row = Adw.SwitchRow(title=title)
        if subtitle:
            row.set_subtitle(subtitle)
        row.set_active(active)
        row.connect("notify::active", lambda r, _p: on_change(r.get_active()))
        return row

    def slider(self, title, subtitle, low, high, step, value, on_change, marks=None):
        row = Adw.ActionRow(title=title)
        if subtitle:
            row.set_subtitle(subtitle)
        scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, low, high, step)
        scale.set_value(value)
        scale.set_draw_value(False)
        scale.set_size_request(240, -1)
        scale.set_valign(Gtk.Align.CENTER)
        for mark_value, mark_text in marks or ():
            scale.add_mark(mark_value, Gtk.PositionType.BOTTOM, mark_text)
        scale.connect("value-changed", lambda s: on_change(s.get_value()))
        row.add_suffix(scale)
        return row

    def flag(self, key, default):
        value = self.cfg.get(key, "true" if default else "false")
        return value in ("true", "yes", "1", "on")

    # ------------------------------------------------------------ Appearance

    def page_appearance(self):
        style = Adw.PreferencesGroup(title="Style")
        dark = self.cfg.get("theme", "dark") != "light"
        row = Adw.ActionRow(title="Colours")
        buttons = Gtk.Box(spacing=0)
        buttons.add_css_class("linked")
        buttons.set_valign(Gtk.Align.CENTER)
        dark_button = Gtk.ToggleButton(label="Dark")
        light_button = Gtk.ToggleButton(label="Light")
        light_button.set_group(dark_button)
        (dark_button if dark else light_button).set_active(True)
        dark_button.connect("toggled",
                            lambda b: self.save("theme", "dark" if b.get_active() else "light"))
        buttons.append(dark_button)
        buttons.append(light_button)
        row.add_suffix(buttons)
        style.add(row)

        accent_row = Adw.ActionRow(title="Highlight colour",
                                   subtitle="Used for the active window, switches and buttons")
        swatches = Gtk.Box(spacing=6)
        swatches.set_valign(Gtk.Align.CENTER)
        current = self.cfg.get("accent", "orange") or "orange"
        swatch_buttons = {}

        def pick(name):
            for other, btn in swatch_buttons.items():
                if other == name:
                    btn.add_css_class("chosen")
                else:
                    btn.remove_css_class("chosen")
            self.save("accent", name)

        for name in ACCENTS:
            btn = Gtk.Button()
            btn.add_css_class(f"swatch-{name}")
            btn.set_tooltip_text(ACCENT_LABELS[name])
            if name == current:
                btn.add_css_class("chosen")
            btn.connect("clicked", lambda _b, n=name: pick(n))
            swatch_buttons[name] = btn
            swatches.append(btn)
        accent_row.add_suffix(swatches)
        style.add(accent_row)

        background = Adw.PreferencesGroup(title="Background")
        wallpaper = self.cfg.get("wallpaper", "none") or "none"
        kind = "none" if wallpaper == "none" else ("mosaic" if wallpaper == "mosaic" else "picture")
        choose = Adw.ActionRow(title="Your picture", subtitle="JPG or PNG")
        choose_button = Gtk.Button(label="Choose…")
        choose_button.set_valign(Gtk.Align.CENTER)
        choose_button.connect("clicked", lambda _b: self.choose_picture())
        choose.add_suffix(choose_button)
        choose.set_visible(kind == "picture")

        def background_changed(value):
            choose.set_visible(value == "picture")
            if value == "picture":
                picture = os.path.join(data_dir(), "wallpaper.png")
                if os.path.exists(picture):
                    self.save("wallpaper", picture)
                else:
                    self.choose_picture()
            else:
                self.save("wallpaper", value)

        background.add(self.combo("Background", None,
                                  [("none", "Plain colour"), ("mosaic", "Mosaic"),
                                   ("picture", "Your own picture")],
                                  kind, background_changed))
        background.add(choose)

        layout = Adw.PreferencesGroup(title="Layout")
        layout.add(self.combo(
            "Size of the bar and gaps",
            "Automatic uses compact on small screens",
            SIZES, self.cfg.get("layout_size", "auto"), lambda v: self.save("layout_size", v)))
        layout.add(self.switch("Animations", "Slide between workspaces",
                               self.flag("animations", True),
                               lambda on: self.save("animations", "true" if on else "false")))
        return self.page_with(style, background, layout)

    def choose_picture(self):
        dialog = Gtk.FileDialog(title="Choose a picture")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        images = Gtk.FileFilter()
        images.set_name("Pictures")
        images.add_pixbuf_formats()
        filters.append(images)
        dialog.set_filters(filters)
        pictures = GLib.get_user_special_dir(GLib.UserDirectory.DIRECTORY_PICTURES)
        if pictures and os.path.isdir(pictures):
            dialog.set_initial_folder(Gio.File.new_for_path(pictures))

        def done(dlg, result):
            try:
                chosen = dlg.open_finish(result)
            except GLib.Error:
                return
            if chosen is None or chosen.get_path() is None:
                return
            try:
                # Store a copy no bigger than a 4K screen; the desktop reads PNG.
                path = chosen.get_path()
                pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(path, 3840, 2160, True)
                os.makedirs(data_dir(), exist_ok=True)
                target = os.path.join(data_dir(), "wallpaper.png")
                pixbuf.savev(target, "png", [], [])
            except GLib.Error as exc:
                self.toast(f"That picture could not be used: {exc.message}")
                return
            self.save("wallpaper", target)
            self.toast("Background changed")

        dialog.open(self, None, done)

    # --------------------------------------------------------------- Display

    def page_display(self):
        outputs = (ipc.send("outputs") or {}).get("outputs", [])
        groups = []
        main = next((o for o in outputs if o["main"]), None)
        for output in outputs:
            groups.append(self.display_group(output, outputs, main))
        if not outputs:
            # The window manager can't be asked (Settings started outside
            # Tessera): the size of everything is all that can be chosen.
            screen = Adw.PreferencesGroup(title="Screen")
            scale = self.cfg.get("scale", "auto") or "auto"
            screen.add(self.combo("Size of everything", SIZE_HINT, SCALES, scale,
                                  lambda v: self.save("scale", v)))
            groups.append(screen)

        night = Adw.PreferencesGroup(
            title="Night light",
            description="Warmer colours in the evening are easier on the eyes.")
        mode = self.cfg.get("night_light", "off") or "off"
        hours = [(f"{h:02d}:00", f"{h:02d}:00") for h in range(24)]
        when = self.combo("When", None,
                          [("scheduled", "From evening to morning"), ("always", "All the time")],
                          mode if mode != "off" else "scheduled",
                          lambda v: self.save("night_light", v))
        start = self.combo("Turns on at", None, hours,
                           self.cfg.get("night_light_from", "20:00"),
                           lambda v: self.save("night_light_from", v))
        end = self.combo("Turns off at", None, hours,
                         self.cfg.get("night_light_to", "07:00"),
                         lambda v: self.save("night_light_to", v))
        try:
            temperature = int(self.cfg.get("night_light_temperature", "4000"))
        except ValueError:
            temperature = 4000
        warmth = self.slider("Warmth", None, 2700, 5500, 100, 8200 - temperature,
                             lambda v: self.save("night_light_temperature", str(int(8200 - v))),
                             [(2700, "Less"), (5500, "More")])

        def mode_changed(on):
            for row in (when, start, end, warmth):
                row.set_sensitive(on)
            if not self.loading:
                values = ["scheduled", "always"]
                self.save("night_light", values[when.get_selected()] if on else "off")

        enabled = self.switch("Night light", None, mode != "off", mode_changed)
        night.add(enabled)
        for row in (when, start, end, warmth):
            night.add(row)
            row.set_sensitive(mode != "off")
        when.connect("notify::selected",
                     lambda r, _p: (start.set_visible(r.get_selected() == 0),
                                    end.set_visible(r.get_selected() == 0)))
        start.set_visible(mode != "always")
        end.set_visible(mode != "always")
        return self.page_with(*groups, night)

    @staticmethod
    def display_title(output):
        if output["builtin"]:
            return "Built-in screen"
        name = " ".join(part for part in (output["make"], output["model"])
                        if part and part.lower() != "unknown")
        return name or output["name"]

    def display_group(self, output, outputs, main):
        """One screen: on or off, resolution, size of everything, and where it
        is next to the main screen (or showing the same picture)."""
        name = output["name"]
        key = f"display.{name}."
        group = Adw.PreferencesGroup(title=self.display_title(output),
                                     description=f"Plugged into {name}")
        rows = []
        if len(outputs) > 1:
            group.add(self.switch("Use this screen", None, output["enabled"],
                                  lambda on: self.change_display(key + "enabled",
                                                                 "true" if on else "false")))

        sizes = {}
        for mode in output["modes"]:
            size = (mode["width"], mode["height"])
            sizes[size] = sizes.get(size, False) or mode["preferred"]
        if sizes:
            choices = [("auto", "Recommended")] + [
                (f"{w}x{h}", f"{w} × {h}" + ("  (recommended)" if preferred else ""))
                for (w, h), preferred in sorted(sizes.items(), key=lambda i: -i[0][0] * i[0][1])]
            current = (self.cfg.get(key + "mode", "auto") or "auto").split("@")[0]
            rows.append(self.combo("Resolution", None, choices, current,
                                   lambda v: self.change_display(key + "mode", v)))

        scale = self.cfg.get(key + "scale") or self.cfg.get("scale", "auto") or "auto"
        size_row = self.combo("Size of everything", SIZE_HINT, SCALES, scale,
                              lambda v: self.change_display(key + "scale", v))
        if output["mirror"]:
            size_row.set_subtitle("Follows the main screen while showing the same picture")
            size_row.set_sensitive(False)
        rows.append(size_row)

        if main is not None and output is not main:
            other = "the built-in screen" if main["builtin"] else "the main screen"
            choices = [("right", "On the right"), ("left", "On the left"), ("above", "Above"),
                       ("below", "Below"), ("mirror", "Same picture (mirror)")]
            current = self.cfg.get(key + "position", "auto") or "auto"
            if current not in dict(choices):
                current = "right"
            rows.append(self.combo("Position", f"Compared with {other}", choices, current,
                                   lambda v: self.change_display(key + "position", v)))
        for row in rows:
            row.set_sensitive(row.get_sensitive() and output["enabled"])
            group.add(row)
        return group

    def change_display(self, key, value):
        """Screen changes are tried out: unless they are kept, they are undone
        after 15 seconds, in case the screen can't show them."""
        if self.loading:
            return
        field = key.rsplit(".", 1)[1]
        previous = self.cfg.get(key) or ("true" if field == "enabled" else "auto")
        if field == "scale" and key not in self.cfg:
            previous = self.cfg.get("scale", "auto") or "auto"
        if value == previous:
            return
        self.save(key, value)

        state = {"left": 15, "done": False, "timer": None}
        dialog = Adw.AlertDialog(heading="Keep these display settings?")

        def body():
            return (f"If you don't choose, the old settings come back in {state['left']} "
                    f"second{'s' if state['left'] != 1 else ''}.")

        def finish(keep):
            if state["done"]:
                return
            state["done"] = True
            if state["timer"] is not None:
                GLib.source_remove(state["timer"])
                state["timer"] = None
            if not keep:
                self.save(key, previous)
                self.show_page("display")

        def tick():
            state["left"] -= 1
            if state["left"] > 0:
                dialog.set_body(body())
                return True
            state["timer"] = None
            finish(False)
            dialog.force_close()
            return False

        dialog.set_body(body())
        dialog.add_response("undo", "Undo")
        dialog.add_response("keep", "Keep")
        dialog.set_response_appearance("keep", Adw.ResponseAppearance.SUGGESTED)
        dialog.set_close_response("undo")
        dialog.connect("response", lambda _d, response: finish(response == "keep"))
        state["timer"] = GLib.timeout_add_seconds(1, tick)
        dialog.present(self)

    # -------------------------------------------------------------- Touchpad

    def page_touchpad(self):
        touchpad = Adw.PreferencesGroup(title="Touchpad")
        touchpad.add(self.switch("Tap to click", "Tap the touchpad instead of pressing it",
                                 self.flag("tap_to_click", True),
                                 lambda on: self.save("tap_to_click", "true" if on else "false")))
        touchpad.add(self.switch("Natural scrolling",
                                 "Content follows your fingers, like on a phone",
                                 self.flag("natural_scroll", False),
                                 lambda on: self.save("natural_scroll", "true" if on else "false")))
        try:
            speed = float(self.cfg.get("pointer_speed", "0"))
        except ValueError:
            speed = 0.0
        touchpad.add(self.slider("Pointer speed", None, -1, 1, 0.1, speed,
                                 lambda v: self.save("pointer_speed", f"{v:.1f}"),
                                 [(-1, "Slow"), (0, None), (1, "Fast")]))

        gestures = Adw.PreferencesGroup(
            title="Three-finger swipes",
            description="Left and right change workspace; up opens your apps.")
        direction = self.combo("Direction", None,
                               [("natural", "Natural (like a phone)"), ("reversed", "Reversed")],
                               self.cfg.get("swipe_direction", "natural"),
                               lambda v: self.save("swipe_direction", v))

        def gestures_changed(on):
            direction.set_sensitive(on)
            self.save("gestures", "true" if on else "false")

        gestures.add(self.switch("Swipe gestures", None, self.flag("gestures", True),
                                 gestures_changed))
        direction.set_sensitive(self.flag("gestures", True))
        gestures.add(direction)
        return self.page_with(touchpad, gestures)

    # -------------------------------------------------------------- Keyboard

    def page_keyboard(self):
        group = Adw.PreferencesGroup(title="Keyboard")
        layouts = keyboard_layouts()
        current = self.cfg.get("keyboard_layout", "") or system_keyboard()
        if layouts:
            row = self.combo("Layout", "The language your keyboard is printed for",
                             layouts, current, lambda v: self.save("keyboard_layout", v))
            row.set_enable_search(True)
            row.set_expression(Gtk.PropertyExpression.new(Gtk.StringObject, None, "string"))
            group.add(row)
        else:
            group.add(Adw.ActionRow(title="Layout", subtitle=current))

        shortcuts = Adw.ActionRow(title="Keyboard shortcuts",
                                  subtitle="All the keys and touchpad swipes Tessera understands")
        show = Gtk.Button(label="Show")
        show.set_valign(Gtk.Align.CENTER)
        show.connect("clicked", lambda _b: ipc.send("help"))
        shortcuts.add_suffix(show)
        group.add(shortcuts)
        return self.page_with(group)

    # ----------------------------------------------------------------- Sound

    def page_sound(self):
        group = Adw.PreferencesGroup(title="Sound",
                                     description="Volume is in quick settings (click the clock).")
        if not shutil.which("pactl"):
            group.add(Adw.ActionRow(title="Sound settings are not available",
                                    subtitle="The pulseaudio-utils package is missing"))
            return self.page_with(group)

        def devices(kind):
            code, out = run(["pactl", "-f", "json", "list", kind])
            try:
                items = json.loads(out) if code == 0 else []
            except ValueError:
                items = []
            result = []
            for item in items:
                name = item.get("name", "")
                if kind == "sources" and (name.endswith(".monitor") or item.get("monitor_of_sink")
                                          not in (None, "", "n/a")):
                    continue
                result.append((name, item.get("description") or name))
            return result

        for kind, title, getter, setter in (
                ("sinks", "Output", "get-default-sink", "set-default-sink"),
                ("sources", "Input", "get-default-source", "set-default-source")):
            found = devices(kind)
            if not found:
                group.add(Adw.ActionRow(title=title, subtitle="No devices found"))
                continue
            _code, default = run(["pactl", getter])

            def changed(value, s=setter):
                # A device list can go stale between building the page and
                # clicking a row in it, which is worth a line of text rather
                # than a window that stops answering.
                apply_sound_device(s, value,
                                   lambda why: self.toast(f"Could not reach PulseAudio: {why}"))

            group.add(self.combo(title, None, found, default.strip(), changed))
        return self.page_with(group)

    # ----------------------------------------------------------------- Power

    def page_power(self):
        group = Adw.PreferencesGroup(
            title="When you are away",
            description="If locking is on, turning the screen off locks it too. "
                        "It always locks before the computer goes to sleep.")

        def minutes(key, default):
            try:
                return int(self.cfg.get(key, str(default)))
            except ValueError:
                return default

        group.add(self.combo("Lock the screen after", None, MINUTE_CHOICES,
                             minutes("idle_lock_minutes", 5),
                             lambda v: self.save("idle_lock_minutes", str(v))))
        group.add(self.combo("Turn the screen off after", None, MINUTE_CHOICES,
                             minutes("idle_screen_off_minutes", 10),
                             lambda v: self.save("idle_screen_off_minutes", str(v))))
        return self.page_with(group)

    # ---------------------------------------------------------- Default apps

    def page_apps(self):
        group = Adw.PreferencesGroup(
            title="Default apps",
            description="Used by the keyboard shortcuts and when opening links and folders.")

        def choices_for(apps):
            seen, result = set(), []
            for app in apps:
                command = app_command(app)
                if command and command not in seen and app.should_show():
                    seen.add(command)
                    result.append((app, command, app.get_display_name()))
            return result

        browsers = choices_for(Gio.AppInfo.get_all_for_type("x-scheme-handler/https"))
        files = choices_for(Gio.AppInfo.get_all_for_type("inode/directory"))
        terminals = choices_for([a for a in Gio.AppInfo.get_all()
                                 if isinstance(a, Gio.DesktopAppInfo)
                                 and "TerminalEmulator" in (a.get_categories() or "")])

        def add(title, key, options, mime_types):
            if not options:
                group.add(Adw.ActionRow(title=title, subtitle="None installed"))
                return
            current = self.cfg.get(key, "")
            commands = [command for _app, command, _name in options]
            if current not in commands and mime_types:
                default = Gio.AppInfo.get_default_for_type(mime_types[0], False)
                current = app_command(default) if default is not None else ""
            if current not in commands:
                current = commands[0] if key == "terminal" else ""

            def changed(command):
                for app, cmd, _name in options:
                    if cmd == command:
                        for mime in mime_types:
                            try:
                                app.set_as_default_for_type(mime)
                            except GLib.Error:
                                pass
                self.save(key, command)

            row = self.combo(title, None, [(c, n) for _a, c, n in options], current, changed)
            if current == "":
                row.set_selected(Gtk.INVALID_LIST_POSITION)
            group.add(row)

        add("Web browser", "browser", browsers,
            ["x-scheme-handler/http", "x-scheme-handler/https", "text/html"])
        add("Files", "files", files, ["inode/directory"])
        add("Terminal", "terminal", terminals, [])
        return self.page_with(group)

    # ----------------------------------------------------------------- About

    def page_about(self):
        group = Adw.PreferencesGroup(title="This computer")
        about = (("Tessera", __version__), ("System", os_name()),
                 ("Processor", cpu_name()), ("Memory", memory_text()),
                 ("Storage", disk_text()))
        build = build_text()
        if build:
            about += (("This build", build),)
        for title, value in about:
            row = Adw.ActionRow(title=title, subtitle=value)
            row.set_subtitle_selectable(True)
            group.add(row)

        update = Adw.PreferencesGroup(title="Updates")
        row = Adw.ActionRow(title="Update Tessera",
                            subtitle="Downloads the newest version. Your password will be asked.")
        button = Gtk.Button(label="Update")
        button.add_css_class("suggested-action")
        button.set_valign(Gtk.Align.CENTER)
        button.connect("clicked", lambda _b: self.run_update())
        row.add_suffix(button)
        update.add(row)
        return self.page_with(group, update)

    def run_update(self):
        script = ("tessera-update; echo; printf 'Press Enter to close this window. '; read _")
        args = terminal_command(self.cfg.get("terminal", ""), ["sh", "-c", script])
        try:
            subprocess.Popen(args, start_new_session=True)
        except OSError as exc:
            self.toast(f"Could not open a terminal: {exc}")


class SettingsApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id="org.tessera.Settings",
                         flags=Gio.ApplicationFlags.DEFAULT_FLAGS)
        self.window = None

    def do_activate(self):
        if self.window is None:
            self.window = SettingsWindow(self)
        self.window.present()


def main():
    return SettingsApp().run(sys.argv[:1])


if __name__ == "__main__":
    sys.exit(main())

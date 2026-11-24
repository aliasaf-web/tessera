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

from . import __version__, ipc  # noqa: E402
from .theme import ACCENTS, read_config, write_config_value  # noqa: E402

ACCENT_LABELS = {
    "orange": "Orange", "blue": "Blue", "teal": "Teal", "green": "Green", "yellow": "Yellow",
    "red": "Red", "pink": "Pink", "purple": "Purple", "slate": "Slate",
}

MINUTE_CHOICES = [(1, "1 minute"), (2, "2 minutes"), (5, "5 minutes"), (10, "10 minutes"),
                  (15, "15 minutes"), (30, "30 minutes"), (0, "Never")]

SCALES = [("auto", "Automatic"), ("1", "100%"), ("1.25", "125%"), ("1.5", "150%"),
          ("1.75", "175%"), ("2", "200%")]

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
                ("appearance", "Appearance", "applications-graphics-symbolic", self.page_appearance),
                ("display", "Display", "video-display-symbolic", self.page_display),
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
        if event.get("event") != "reload" or self.reload_source is not None:
            return
        if read_config() == self.cfg:
            return
        row = self.sidebar.get_selected_row()
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
        write_config_value(key, value)
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
        dark_button.connect("toggled", lambda b: self.save("theme", "dark" if b.get_active() else "light"))
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
                pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(chosen.get_path(), 3840, 2160, True)
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
        screen = Adw.PreferencesGroup(title="Screen")
        scale = self.cfg.get("scale", "auto") or "auto"
        screen.add(self.combo("Size of everything", "Bigger is easier to read; smaller fits more",
                              SCALES, scale, lambda v: self.save("scale", v)))

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
        when.connect("notify::selected", lambda r, _p: (start.set_visible(r.get_selected() == 0),
                                                         end.set_visible(r.get_selected() == 0)))
        start.set_visible(mode != "always")
        end.set_visible(mode != "always")
        return self.page_with(screen, night)

    # -------------------------------------------------------------- Touchpad

    def page_touchpad(self):
        touchpad = Adw.PreferencesGroup(title="Touchpad")
        touchpad.add(self.switch("Tap to click", "Tap the touchpad instead of pressing it",
                                 self.flag("tap_to_click", True),
                                 lambda on: self.save("tap_to_click", "true" if on else "false")))
        touchpad.add(self.switch("Natural scrolling", "Content follows your fingers, like on a phone",
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
            group.add(self.combo(title, None, found, default.strip(),
                                 lambda v, s=setter: run(["pactl", s, v])))
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
        for title, value in (("Tessera", __version__), ("System", os_name()),
                             ("Processor", cpu_name()), ("Memory", memory_text()),
                             ("Storage", disk_text())):
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
        terminal = self.cfg.get("terminal", "") or "x-terminal-emulator"
        script = ("tessera-update; echo; printf 'Press Enter to close this window. '; read _")
        if shutil.which("foot") and terminal.split()[0] == "foot":
            args = ["foot", "--title", "Updating Tessera", "sh", "-c", script]
        else:
            args = [terminal.split()[0], "-e", "sh", "-c", script]
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

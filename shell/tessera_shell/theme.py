"""Colours, the shell's stylesheet and the shared desktop settings."""

import os
import re
import tempfile

from gi.repository import Gdk, Gio, Gtk

PALETTES = {
    "dark": {
        "bg": "#0f1012",
        "surface": "#1a1b1e",
        "surface2": "#242529",
        "hover": "#2c2d32",
        "text": "#ededee",
        "dim": "#9a9ca4",
        "border": "rgba(255,255,255,0.06)",
        "shade": "rgba(0,0,0,0.45)",
        "accent": "#f0883e",
    },
    "light": {
        "bg": "#e7e9ed",
        "surface": "#ffffff",
        "surface2": "#f1f2f5",
        "hover": "#e6e8ec",
        "text": "#1b1d22",
        "dim": "#5f636d",
        "border": "rgba(0,0,0,0.10)",
        "shade": "rgba(20,22,28,0.25)",
        "accent": "#c95a22",
    },
}


# Named accent colours, tuned for dark and light backgrounds. The window
# manager uses the same table (compositor/src/config.c).
ACCENTS = {
    "orange": ("#f0883e", "#c95a22"),
    "blue": ("#5b9cf5", "#2f6fd6"),
    "teal": ("#3cc8b4", "#13897a"),
    "green": ("#57c272", "#2e8b47"),
    "yellow": ("#e8b931", "#a87b00"),
    "red": ("#f0605a", "#c83a35"),
    "pink": ("#ec7fb2", "#c24d86"),
    "purple": ("#a77bff", "#7a4de0"),
    "slate": ("#8fa3b8", "#5a6b7d"),
}


# A colour written as #RRGGBB, or #RRGGBBAA (the AA part is not used here).
HEX_COLOUR = re.compile(r"#[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?")


def accent_hex(value, dark):
    """The accent colour for a named accent or a colour code; anything else
    (such as a mistyped code) gets the default orange."""
    if value in ACCENTS:
        return ACCENTS[value][0 if dark else 1]
    if HEX_COLOUR.fullmatch(value):
        return value[:7]
    return ACCENTS["orange"][0 if dark else 1]


def config_path():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "tessera", "config")


def config_value(text):
    """A value as written in the config file: spaces around it are ignored,
    and matching quotes around it are removed (the window manager reads it
    the same way)."""
    text = text.strip()
    if len(text) >= 2 and text[0] in "\"'" and text[-1] == text[0]:
        text = text[1:-1]
    return text


def read_config_file(path, values):
    """Adds the settings in one file to values; a missing file is skipped.
    The format is described at the top of /etc/tessera/config."""
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, value = line.split("=", 1)
                values[config_value(key)] = config_value(value)
    except OSError:
        pass
    return values


def read_config():
    """The settings: /etc/tessera/config, then the user's own file on top."""
    values = {}
    for path in ("/etc/tessera/config", config_path()):
        read_config_file(path, values)
    return values


def config_line(key, value):
    """One setting as it goes in the file, ready for both readers (the window
    manager and config_value above): spaces around it are dropped and matching
    quotes around a value are taken off, so a value that starts and ends with
    the same quote character is wrapped so that it comes back unchanged.
    Raises ValueError for anything the file cannot hold: the window manager
    reads one setting per line, up to 1024 characters, so a value with a line
    in it, or longer than that, would be read back as something else."""
    key = str(key).strip()
    value = str(value)
    if "\n" in value or "\r" in value or "\n" in key or "\r" in key:
        raise ValueError(f"{key} cannot be stored: the setting has a line break in it")
    if len(value) >= 2 and value[0] in "\"'" and value[-1] == value[0]:
        value = f'"{value}"'
    line = f"{key}={value}"
    if len(line) > 1000:
        raise ValueError(f"{key} cannot be stored: the setting is too long")
    return line


def write_config_value(key, value):
    """Change one line of the user's config file, keeping everything else.

    The new file is written beside the old one and moved over it, so a power
    cut or a full disk leaves the settings you had before instead of an empty
    file; a name from mkstemp means two programs writing at once cannot each
    finish the other's half-written file. The mode is set, not inherited."""
    line = config_line(key, value)
    path = config_path()
    folder = os.path.dirname(path)
    os.makedirs(folder, exist_ok=True)
    lines = []
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        pass
    found = False
    for i, old_line in enumerate(lines):
        stripped = old_line.strip()
        if not stripped.startswith("#") and "=" in stripped:
            if stripped.split("=", 1)[0].strip() == key:
                lines[i] = line
                found = True
    if not found:
        lines.append(line)
    handle, tmp = tempfile.mkstemp(dir=folder, prefix="config.", suffix=".tmp")
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except OSError:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


class Theme:
    def __init__(self):
        self.provider = Gtk.CssProvider()
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), self.provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 10)
        self.name = "dark"
        self.colors = PALETTES["dark"]
        self.reload()

    @property
    def dark(self):
        return self.name == "dark"

    def reload(self):
        cfg = read_config()
        self.name = "light" if cfg.get("theme") == "light" else "dark"
        colors = dict(PALETTES[self.name])
        self.accent_name = cfg.get("accent", "orange") or "orange"
        colors["accent"] = accent_hex(self.accent_name, self.dark)
        self.colors = colors
        css = STYLESHEET
        for key, value in colors.items():
            css = css.replace("@" + key + "@", value)
        css = css.replace("@on_accent@", on_accent(colors["accent"]))
        if hasattr(self.provider, "load_from_string"):
            self.provider.load_from_string(css)
        else:
            self.provider.load_from_data(css, -1)
        settings = Gtk.Settings.get_default()
        if settings is not None:
            settings.set_property("gtk-application-prefer-dark-theme", self.dark)


def on_accent(hex_color):
    r = int(hex_color[1:3], 16) / 255
    g = int(hex_color[3:5], 16) / 255
    b = int(hex_color[5:7], 16) / 255
    lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
    return "#111111" if lum > 0.55 else "#ffffff"


_interface_settings = None


def apply_app_theme(dark, accent="orange"):
    """Tell GTK and libadwaita apps about light or dark mode and the accent."""
    global _interface_settings
    source = Gio.SettingsSchemaSource.get_default()
    schema = source.lookup("org.gnome.desktop.interface", True) if source else None
    if schema is not None:
        if _interface_settings is None:
            _interface_settings = Gio.Settings.new("org.gnome.desktop.interface")
        gtk3 = "adw-gtk3-dark" if dark else "adw-gtk3"
        if not os.path.isdir(f"/usr/share/themes/{gtk3}"):
            gtk3 = "Adwaita-dark" if dark else "Adwaita"
        icons = "Papirus-Dark" if dark else "Papirus"
        if not os.path.isdir(f"/usr/share/icons/{icons}"):
            icons = "Adwaita"
        values = [("color-scheme", "prefer-dark" if dark else "prefer-light"),
                  ("gtk-theme", gtk3), ("icon-theme", icons)]
        if accent in ACCENTS:
            # libadwaita apps use the same named accent colours.
            values.append(("accent-color", accent))
        for key, value in values:
            # Older systems may not know every key (accent-color is recent).
            if schema.has_key(key) and _interface_settings.get_string(key) != value:
                _interface_settings.set_string(key, value)
    # GTK 3 apps also read this file.
    path = os.path.expanduser("~/.config/gtk-3.0/settings.ini")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    lines = []
    try:
        with open(path, encoding="utf-8") as f:
            lines = [line for line in f.read().splitlines()
                     if not line.startswith("gtk-application-prefer-dark-theme")]
    except OSError:
        pass
    if "[Settings]" not in lines:
        lines.insert(0, "[Settings]")
    lines.append(f"gtk-application-prefer-dark-theme={'true' if dark else 'false'}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


STYLESHEET = """
* {
  font-family: Inter, "Noto Sans", sans-serif;
}

window.tessera-layer, window.tessera-layer > * {
  background: transparent;
}

/* ---------- Bar ---------- */

.bar {
  padding: 0;
}

.pill {
  background: @surface@;
  border-radius: 999px;
  border: 1px solid @border@;
  color: @text@;
}

button.round {
  min-width: 44px;
  min-height: 44px;
  padding: 0;
  border-radius: 999px;
  background: @surface@;
  border: 1px solid @border@;
  color: @text@;
  box-shadow: none;
}

button.round:hover {
  background: @hover@;
}

button.round.active {
  background: @accent@;
  color: @on_accent@;
  border-color: transparent;
}

.workspaces {
  padding: 0 14px;
  min-height: 44px;
}

button.ws-dot {
  min-width: 8px;
  min-height: 8px;
  padding: 0;
  margin: 0 3px;
  border-radius: 999px;
  background: alpha(@dim@, 0.35);
  border: none;
  box-shadow: none;
}

button.ws-dot.occupied {
  background: alpha(@text@, 0.75);
}

button.ws-dot.urgent {
  background: @accent@;
}

button.ws-dot.current {
  min-width: 24px;
  background: @accent@;
}

button.ws-hit {
  padding: 0 2px;
  min-height: 44px;
  background: transparent;
  border: none;
  box-shadow: none;
}

button.status {
  min-height: 44px;
  padding: 0 18px;
  border-radius: 999px;
  background: @surface@;
  border: 1px solid @border@;
  color: @text@;
  box-shadow: none;
}

button.status:hover, button.status.open {
  background: @hover@;
}

.status label.clock {
  font-weight: 600;
  font-size: 14px;
}

.status label.percent {
  font-size: 13px;
  color: @dim@;
}

.compact button.round {
  min-width: 36px;
  min-height: 36px;
}

.compact .workspaces, .compact button.ws-hit {
  min-height: 36px;
}

.compact button.status {
  min-height: 36px;
  padding: 0 14px;
}

/* ---------- Window switcher ---------- */

.switcher {
  padding: 16px;
}

button.switch-item {
  min-width: 104px;
  padding: 12px 8px;
  border-radius: 16px;
  background: transparent;
  border: 2px solid transparent;
  color: @text@;
  box-shadow: none;
}

button.switch-item.selected {
  background: @surface2@;
  border-color: @accent@;
}

button.switch-item label {
  font-size: 12px;
}

/* ---------- Cards (launcher, quick settings, help) ---------- */

.backdrop {
  background: @shade@;
}

.backdrop.clear {
  background: transparent;
}

.card {
  background: @surface@;
  color: @text@;
  border-radius: 22px;
  border: 1px solid @border@;
  padding: 22px;
  box-shadow: 0 18px 50px rgba(0,0,0,0.35);
}

.card label.title {
  font-size: 18px;
  font-weight: 650;
}

.card label.heading {
  font-size: 13px;
  font-weight: 600;
  color: @dim@;
}

.card label.dim, label.dim {
  color: @dim@;
}

entry.search, .search {
  min-height: 48px;
  border-radius: 14px;
  background: @surface2@;
  color: @text@;
  border: 1px solid transparent;
  padding: 0 14px;
  font-size: 16px;
  box-shadow: none;
  outline: none;
}

entry.search:focus-within, .search:focus-within {
  border-color: alpha(@accent@, 0.7);
}

box.prompt {
  background: alpha(@text@, 0.06);
  border-radius: 14px;
  padding: 14px;
}

label.pairing-code {
  font-size: 26px;
  font-weight: 700;
  letter-spacing: 4px;
  font-feature-settings: "tnum";
  color: @accent@;
}

entry.field {
  min-height: 42px;
  border-radius: 12px;
  background: @surface2@;
  color: @text@;
  border: 1px solid transparent;
  padding: 0 12px;
  box-shadow: none;
}

entry.field:focus-within {
  border-color: alpha(@accent@, 0.7);
}

flowbox.apps {
  background: transparent;
}

flowboxchild {
  padding: 0;
  border-radius: 16px;
  background: transparent;
  outline: none;
}

flowboxchild:selected, flowboxchild:focus {
  background: transparent;
}

button.app {
  padding: 12px 6px;
  border-radius: 16px;
  background: transparent;
  border: none;
  color: @text@;
  box-shadow: none;
}

button.app:hover, flowboxchild:selected button.app, flowboxchild:focus button.app {
  background: @surface2@;
}

button.app label {
  font-size: 13px;
}

button.flat {
  border-radius: 12px;
  padding: 8px 14px;
  background: @surface2@;
  color: @text@;
  border: none;
  box-shadow: none;
}

button.flat:hover {
  background: @hover@;
}

button.flat.danger {
  background: @accent@;
  color: @on_accent@;
}

button.icon {
  min-width: 40px;
  min-height: 40px;
  padding: 0;
  border-radius: 999px;
  background: @surface2@;
  color: @text@;
  border: none;
  box-shadow: none;
}

button.icon:hover {
  background: @hover@;
}

.avatar {
  min-width: 36px;
  min-height: 36px;
  border-radius: 999px;
  background: alpha(@accent@, 0.25);
  color: @accent@;
  font-weight: 700;
}

/* ---------- Quick settings ---------- */

.tile {
  border-radius: 18px;
  background: @surface2@;
  color: @text@;
  min-height: 64px;
}

.tile.on {
  background: @accent@;
  color: @on_accent@;
}

.tile button {
  background: transparent;
  border: none;
  box-shadow: none;
  color: inherit;
  padding: 0 14px;
  min-height: 64px;
  border-radius: 18px;
}

.tile button:hover {
  background: alpha(currentColor, 0.08);
}

.tile label.tile-title {
  font-weight: 600;
  font-size: 15px;
}

.tile label.tile-sub {
  font-size: 13px;
  opacity: 0.8;
}

.slider-row image {
  color: @text@;
}

scale.level trough {
  min-height: 8px;
  border-radius: 999px;
  background: @surface2@;
}

scale.level highlight {
  border-radius: 999px;
  background: @accent@;
}

scale.level slider {
  min-width: 18px;
  min-height: 18px;
  margin: -6px;
  border-radius: 999px;
  background: @text@;
  border: none;
  box-shadow: 0 1px 3px rgba(0,0,0,0.3);
}

switch {
  background: @surface2@;
  border: 1px solid @border@;
}

switch:checked {
  background: @accent@;
  border-color: transparent;
}

switch slider {
  background: #ffffff;
  box-shadow: 0 1px 3px rgba(0,0,0,0.3);
}

list.networks, list.plain {
  background: transparent;
}

list.networks row, list.plain row {
  border-radius: 12px;
  padding: 10px 10px;
  background: transparent;
  color: @text@;
}

list.networks row:hover, list.plain row:hover {
  background: @surface2@;
}

label.badge {
  font-size: 12px;
  color: @accent@;
  font-weight: 600;
}

/* ---------- Notifications ---------- */

.toast {
  background: @surface@;
  color: @text@;
  border-radius: 18px;
  border: 1px solid @border@;
  padding: 14px 16px;
  box-shadow: 0 12px 30px rgba(0,0,0,0.3);
}

.toast label.summary {
  font-weight: 650;
  font-size: 14px;
}

.toast label.body {
  font-size: 13px;
  color: @dim@;
}

.toast.critical {
  border: 2px solid @accent@;
}

.history-item {
  background: @surface2@;
  border-radius: 14px;
  padding: 10px 12px;
}

/* ---------- OSD ---------- */

.osd {
  background: @surface@;
  color: @text@;
  border-radius: 999px;
  border: 1px solid @border@;
  padding: 12px 20px;
  box-shadow: 0 12px 30px rgba(0,0,0,0.3);
}

levelbar.osd-level trough {
  min-height: 6px;
  border-radius: 999px;
  background: @surface2@;
}

levelbar.osd-level block.filled {
  border-radius: 999px;
  background: @accent@;
}

levelbar.osd-level block.empty {
  background: transparent;
}

/* ---------- Help ---------- */

.keycap {
  background: @surface2@;
  border-radius: 8px;
  padding: 3px 9px;
  font-weight: 600;
  font-size: 13px;
  border-bottom: 2px solid @hover@;
}
"""

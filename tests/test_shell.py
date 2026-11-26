"""Tests for the shell's parts that don't need a screen.

    python3 -m unittest discover -s tests
"""

import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "shell"))

import gi  # noqa: E402

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Pango", "1.0")

from tessera_shell import ipc, terminal, theme  # noqa: E402


class ConfigFile(unittest.TestCase):
    """The shell must read tests/config-sample exactly as the window manager
    does (compositor/tests/config_test.c checks the same file)."""

    def setUp(self):
        self.values = theme.read_config_file(ROOT / "tests" / "config-sample", {})

    def test_values(self):
        v = self.values
        self.assertEqual(v["theme"], "light")
        self.assertEqual(v["accent"], "purple")
        self.assertEqual(v["terminal"], "foot --app-id test")
        self.assertEqual(v["wallpaper"], "/home/me/My Pictures/sea.png")
        self.assertEqual(v["gestures"], "false")
        self.assertEqual(v["scale"], "1.25")
        self.assertEqual(v["display.HDMI-A-1.mode"], "1920x1080@59.94")
        self.assertEqual(v["display.HDMI-A-1.position"], "left")
        self.assertEqual(v["display.eDP-1.enabled"], "false")
        self.assertNotIn("not a setting", v)

    def test_missing_file(self):
        self.assertEqual(theme.read_config_file("/nonexistent/tessera/config", {}), {})

    def test_quoting_and_limits(self):
        """The value goes in so that both readers give it back unchanged:
        spaces and a matching pair of quotes are taken off when reading."""
        self.assertEqual(theme.config_line("theme", "dark"), "theme=dark")
        self.assertEqual(theme.config_line("wallpaper", 'sea "winter"'),
                         'wallpaper=sea "winter"')
        # A value that starts and ends with the same quote is wrapped, because
        # a reader would otherwise take that pair off.
        for tricky in ('"x"', "'y'", '"', "'", '""'):
            self.assertEqual(theme.config_value(theme.config_line("k", tricky).split("=", 1)[1]),
                             tricky)
        self.assertEqual(theme.config_line("k", "a=b"), "k=a=b")
        for bad in ("line\nbreak", "carriage\rreturn", "x" * 1000):
            self.assertRaises(ValueError, theme.config_line, "k", bad)

    def test_write_keeps_the_rest(self):
        with tempfile.TemporaryDirectory() as home:
            with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": home}):
                path = pathlib.Path(theme.config_path())
                path.parent.mkdir(parents=True)
                path.write_text("# my notes\ntheme=dark\naccent = blue\n", encoding="utf-8")
                theme.write_config_value("accent", "green")
                theme.write_config_value("display.HDMI-A-1.position", "mirror")
                text = path.read_text(encoding="utf-8")
                self.assertIn("# my notes\n", text)
                self.assertIn("theme=dark\n", text)
                values = theme.read_config_file(path, {})
                self.assertEqual(values["accent"], "green")
                self.assertEqual(values["display.HDMI-A-1.position"], "mirror")
                self.assertEqual(text.count("accent"), 1)
                # Written aside and moved over: nothing left behind, and the
                # mode is the one the file is expected to have.
                self.assertEqual([q.name for q in path.parent.iterdir()], ["config"])
                self.assertEqual(path.stat().st_mode & 0o777, 0o644)

    def test_quote_that_needs_wrapping_survives(self):
        with tempfile.TemporaryDirectory() as home:
            with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": home}):
                pathlib.Path(theme.config_path()).parent.mkdir(parents=True)
                theme.write_config_value("wallpaper", '"/home/me/My \"sea\".png"')
                values = theme.read_config_file(theme.config_path(), {})
                self.assertEqual(values["wallpaper"], '"/home/me/My \"sea\".png"')


class SocketPath(unittest.TestCase):
    """Where the shell looks for the window manager's control socket, which
    has to be where the window manager puts it (compositor/src/ipc.c)."""

    def test_the_same_name_in_both_programs(self):
        source = (ROOT / "compositor" / "src" / "ipc.c").read_text(encoding="utf-8")
        body = source[source.index("static bool socket_path"):]
        body = body[:body.index("\n}")]
        self.assertIn('"%s/tessera-%s.sock"', body)
        self.assertIn("/run/user/%lu", body)
        self.assertIn("XDG_RUNTIME_DIR", body)

    def test_folder_from_the_environment(self):
        with tempfile.TemporaryDirectory() as home:
            env = {"XDG_RUNTIME_DIR": home, "WAYLAND_DISPLAY": "wayland-1", "TESSERA_SOCK": ""}
            with mock.patch.dict(os.environ, env, clear=False):
                self.assertEqual(ipc.socket_path(), os.path.join(home, "tessera-wayland-1.sock"))

    def test_default_display(self):
        with tempfile.TemporaryDirectory() as home:
            env = {"XDG_RUNTIME_DIR": home, "TESSERA_SOCK": ""}
            env["WAYLAND_DISPLAY"] = ""
            with mock.patch.dict(os.environ, env, clear=False):
                os.environ.pop("WAYLAND_DISPLAY", None)
                self.assertEqual(ipc.socket_path(), os.path.join(home, "tessera-wayland-0.sock"))

    def test_a_folder_of_someone_else_is_not_used(self):
        with tempfile.TemporaryDirectory() as home:
            env = {"XDG_RUNTIME_DIR": os.path.join(home, "nope"), "TESSERA_SOCK": ""}
            with mock.patch.dict(os.environ, env, clear=False):
                self.assertIsNone(ipc.socket_path())
                self.assertIsNone(ipc.runtime_dir())

    def test_the_variable_wins_over_the_folder(self):
        with mock.patch.dict(os.environ, {"TESSERA_SOCK": "/tmp/elsewhere.sock"}):
            self.assertEqual(ipc.socket_path(), "/tmp/elsewhere.sock")


class Accents(unittest.TestCase):
    """The accent colours are listed in the window manager and in the shell;
    they must be the same."""

    def test_same_in_both(self):
        source = (ROOT / "compositor" / "src" / "config.c").read_text(encoding="utf-8")
        table = source[source.index("} accents[] = {"):]
        table = table[:table.index("};")]
        in_c = {name: (dark, light) for name, dark, light in
                re.findall(r'\{\s*"(\w+)",\s*"(#[0-9a-fA-F]+)",\s*"(#[0-9a-fA-F]+)"\s*\}', table)}
        self.assertTrue(in_c, "could not find the accents table in config.c")
        self.assertEqual(in_c, theme.ACCENTS)

    def test_colour_codes(self):
        orange_dark, orange_light = theme.ACCENTS["orange"]
        self.assertEqual(theme.accent_hex("blue", True), theme.ACCENTS["blue"][0])
        self.assertEqual(theme.accent_hex("#12aBcD", True), "#12aBcD")
        self.assertEqual(theme.accent_hex("#12abcdef", False), "#12abcd")
        for wrong in ("#zzzzzz", "#12345z", "#1234", "12abcd", "#12abcd ", "", "#12abcde"):
            self.assertEqual(theme.accent_hex(wrong, True), orange_dark, wrong)
            self.assertEqual(theme.accent_hex(wrong, False), orange_light, wrong)
        # Text on the accent is worked out from any colour accepted here.
        self.assertIn(theme.on_accent(theme.accent_hex("#ffffff", True)), ("#111111", "#ffffff"))


class Terminal(unittest.TestCase):
    def which(self, *installed):
        return mock.patch("shutil.which", lambda name: name if name in installed else None)

    def test_flags(self):
        run = ["sh", "-c", "tessera-update"]
        with self.which("foot", "gnome-terminal", "xfce4-terminal", "konsole", "xterm"):
            self.assertEqual(terminal.terminal_command("foot", run), ["foot"] + run)
            self.assertEqual(terminal.terminal_command("gnome-terminal", run),
                             ["gnome-terminal", "--"] + run)
            self.assertEqual(terminal.terminal_command("xfce4-terminal", run),
                             ["xfce4-terminal", "-x"] + run)
            self.assertEqual(terminal.terminal_command("konsole", run), ["konsole", "-e"] + run)
            self.assertEqual(terminal.terminal_command("xterm -fa Mono", run),
                             ["xterm", "-fa", "Mono", "-e"] + run)

    def test_fallbacks(self):
        run = ["true"]
        with self.which("xdg-terminal-exec"):
            self.assertEqual(terminal.terminal_command("missing-terminal", run),
                             ["xdg-terminal-exec", "true"])
        with self.which():
            self.assertEqual(terminal.terminal_command("", run),
                             ["x-terminal-emulator", "-e", "true"])


class Notifications(unittest.TestCase):
    def setUp(self):
        try:
            gi.require_version("Gtk4LayerShell", "1.0")
            from tessera_shell import notifications
        except (ImportError, ValueError) as error:
            self.skipTest(f"needs the shell's libraries: {error}")
        self.safe_markup = notifications.safe_markup

    def test_keeps_simple_formatting(self):
        self.assertEqual(self.safe_markup("<b>Hi</b> & bye"), "<b>Hi</b> &amp; bye")

    def test_removes_links_and_images(self):
        self.assertEqual(self.safe_markup('<a href="x">link</a><img src="y"/>'), "link")

    def test_escapes_everything_else(self):
        self.assertEqual(self.safe_markup("<script>x</script>"), "&lt;script&gt;x&lt;/script&gt;")

    def test_broken_markup(self):
        self.assertEqual(self.safe_markup("<b>open"), "open")


class Readme(unittest.TestCase):
    def test_shortcuts_up_to_date(self):
        result = subprocess.run([sys.executable, str(ROOT / "tools" / "update-readme.py"),
                                 "--check"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()

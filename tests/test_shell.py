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

try:
    import gi  # noqa: E402

    gi.require_version("Gtk", "4.0")
    gi.require_version("Gdk", "4.0")
    gi.require_version("Pango", "1.0")
    from gi.repository import GLib  # noqa: E402
    from tessera_shell import clock, ipc, terminal, theme  # noqa: E402
    GTK = None
except (ImportError, ValueError) as missing:
    # The bindings are their own packages (python3-gi and the .typelib files), so
    # a plain container has this suite and not the desktop the suite looks at.
    # Skipping with a reason says "nothing was checked here"; a bare ImportError
    # says "this patch is broken". Neither is allowed to look like a pass, which
    # is what a red run that nobody can read is.
    GTK = missing


def needs_gtk(test):
    """Skips a test that reaches through the GTK bindings, naming why."""
    return unittest.skipUnless(GTK is None, f"no GTK bindings on this machine ({GTK})")(test)


@needs_gtk
class SoundDeviceChanges(unittest.TestCase):
    """The sound page must not wait for PulseAudio.

    Picking an output device is a request with nothing to read back, and pactl
    can sit for seconds when the sound server is restarting - which is exactly
    when the settings window is being used. The freeze was in the waiting, so
    what is checked is that the change is handed to the non-blocking runner this
    app already has, and that a failure comes back as something to say.
    """

    class Runner:
        """The one call `apply_sound_device` makes, recorded instead of run."""

        def __init__(self):
            self.started = []

        def run_async(self, args, done=None):
            self.started.append((args, done))

    def setUp(self):
        from tessera_shell import settings

        self.settings = settings
        self.runner = self.Runner()
        self.patched = mock.patch.object(settings, "services", self.runner)
        self.patched.start()

    def tearDown(self):
        self.patched.stop()

    def test_pactl_is_handed_over_and_not_waited_for(self):
        said = []
        self.settings.apply_sound_device(
            "set-default-sink", "alsa_output.pci-0000", on_failure=said.append)
        self.assertEqual([args for args, _done in self.runner.started],
                         [["pactl", "set-default-sink", "alsa_output.pci-0000"]])
        _args, done = self.runner.started[0]
        self.assertIsNotNone(done, "nobody was told whether the device changed")
        done(True, "")
        self.assertEqual(said, [], "a request that went out was reported as a failure")

    def test_a_failure_comes_back_as_something_to_say(self):
        said = []
        self.settings.apply_sound_device("set-default-source", "gone", on_failure=said.append)
        _args, done = self.runner.started[0]
        done(False, "GDBus.Error: no such sink")
        self.assertEqual(said, ["GDBus.Error: no such sink"])
        # With nobody to tell, a failure is still not the caller's problem: the
        # row is free to be tried again, and the window must stay open.
        self.settings.apply_sound_device("set-default-source", "gone")
        _args, done = self.runner.started[-1]
        done(False, "no sink")


@needs_gtk
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


@needs_gtk
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

    def test_the_folder_wins_when_the_desktop_is_there(self):
        """A program of yours that sets TESSERA_SOCK would otherwise point the
        bar at a desktop of its own, and its answers would look real."""
        with tempfile.TemporaryDirectory() as home:
            real = os.path.join(home, "tessera-wayland-0.sock")
            pathlib.Path(real).write_text("", encoding="utf-8")
            env = {"XDG_RUNTIME_DIR": home, "TESSERA_SOCK": "/tmp/elsewhere.sock"}
            with mock.patch.dict(os.environ, env, clear=False):
                env.pop("WAYLAND_DISPLAY", None)
                self.assertEqual(ipc.socket_path(), real)

    def test_the_variable_is_the_fallback(self):
        """A desktop that put its socket elsewhere still reaches its own bar:
        it exports the variable, and nothing is in the folder to disagree."""
        with tempfile.TemporaryDirectory() as home:
            env = {"XDG_RUNTIME_DIR": home, "TESSERA_SOCK": "/tmp/elsewhere.sock"}
            with mock.patch.dict(os.environ, env, clear=False):
                self.assertEqual(ipc.socket_path(), "/tmp/elsewhere.sock")


@needs_gtk
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


@needs_gtk
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


@needs_gtk
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


@needs_gtk
class Dates(unittest.TestCase):
    """The time and date the bar and the panel show, said in one place.

    The bug this holds shut is the format string: the day of the month with no
    leading zero is written `%-d` by glibc and printed out as typed by other C
    libraries, so a widget that spelled it that way would show
    `Saturday %-d November` on anything but Debian.
    """

    def day(self, number):
        # The 7th or the 26th of November 2026, at noon, in this machine's zone:
        # only the day of the month is what these tests are about.
        return GLib.DateTime.new_local(2026, 11, number, 12, 0, 0)

    def test_the_day_of_the_month_has_no_leading_zero(self):
        for number, expected in ((7, "7"), (26, "26")):
            words = clock.date_text(at=self.day(number)).split()
            self.assertEqual(words[1], expected, words)

    def test_no_format_letters_leak_into_what_you_see(self):
        for with_year in (False, True):
            text = clock.date_text(with_year=with_year, at=self.day(7))
            self.assertNotIn("%", text, text)
            self.assertNotIn("-", text.split()[1], text)

    def test_the_year_only_shows_when_there_is_room_for_it(self):
        self.assertTrue(clock.date_text(with_year=True, at=self.day(7)).endswith("2026"))
        self.assertNotIn("2026", clock.date_text(at=self.day(7)))

    def test_the_bar_clock_is_two_digits_a_colon_and_two_more(self):
        self.assertRegex(clock.clock_text(at=self.day(7)), r"^\d\d:\d\d$")

    def test_asking_for_this_moment_works_too(self):
        """The widgets pass no time of their own, so the no-argument path is the
        one that matters: it is where a constructor named one letter off hides."""
        self.assertRegex(clock.clock_text(), r"^\d\d:\d\d$")
        for with_year in (False, True):
            text = clock.date_text(with_year=with_year)
            self.assertNotIn("%", text, text)
            self.assertGreaterEqual(len(text.split()), 3, text)


class Readme(unittest.TestCase):
    def test_shortcuts_up_to_date(self):
        result = subprocess.run([sys.executable, str(ROOT / "tools" / "update-readme.py"),
                                 "--check"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()

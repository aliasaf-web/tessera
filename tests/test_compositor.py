"""The window manager itself, answering its control socket.

The desktop is started for real, on wlroots' headless backend with the pixman
renderer: no screen, no graphics driver and no keyboard needed. Everything is
then asked of it the way the bar asks it - `tessera msg ...`, and one open
socket for events - so the protocol, the state the bar draws, the screens it
lists, the workspace rules and what the lock screen shuts out are covered.

The window manager has to be built first (the checks workflow builds it in the
step before these tests run):

    meson setup build compositor && ninja -C build
    python3 -m unittest discover -s tests

Each class starts its own desktop, in an empty folder of its own, with a config
that turns the idle lock, the screen blanking and the workspace animation off -
so nothing can move under a test while it runs, and one test's changes are just
that: changes, which the tests below take into account.

Another build can be tried with TESSERA=/path/to/tessera. With nothing built,
these tests are skipped rather than failed.
"""

import glob
import json
import os
import pathlib
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = os.environ.get("TESSERA") or str(ROOT / "build" / "tessera")
# The little client the window manager's project builds beside it: one window,
# and a list of what the desktop offers. A test that needs a window in the
# desktop asks for this one, and skips itself when the build did not happen.
CLIENT = os.path.join(os.path.dirname(BINARY), "tessera-test-window")

# A first run in a container is slow; nothing here needs more than a moment.
WAIT_SECONDS = 30


def version():
    """The version in the VERSION file, which the desktop reports too."""
    return (ROOT / "VERSION").read_text(encoding="utf-8").strip()


def until(predicate, seconds=WAIT_SECONDS):
    """Waits for a thing to be true, checking often; True as soon as it is."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return False


class DesktopMixin:
    """Starts one window manager for the class and stops it afterwards.

    Every class that uses this gets its own desktop: the lock screen cannot be
    opened again without an account to ask, so a test that locks the screen
    takes the desktop with it rather than leaving it shut for the others.
    """

    screens = "1"

    @classmethod
    def setUpClass(cls):
        if not os.path.isfile(BINARY):
            raise unittest.SkipTest(f"needs the window manager built first (no {BINARY})")
        cls.runtime = tempfile.mkdtemp(prefix="tessera-tests-")
        cls.log = open(os.path.join(cls.runtime, "desktop.log"), "w+")
        # WLR_HEADLESS_OUTPUTS is how a test asks for screens without having
        # any: the headless backend makes them out of nothing, which is how one
        # class here can have six of them - more than the state line used to
        # have room for. HOME and XDG_CONFIG_HOME point at the same empty
        # folder, so the desktop starts from its defaults and a developer's own
        # settings cannot change what these tests see. TESSERA_STARTUP is the
        # command the desktop runs on its own account (the real one starts the
        # bar): here it is something that finishes at once.
        cls.env = dict(os.environ,
                       HOME=cls.runtime,
                       XDG_RUNTIME_DIR=cls.runtime,
                       XDG_CONFIG_HOME=os.path.join(cls.runtime, "config"),
                       WLR_BACKENDS="headless",
                       WLR_RENDERER="pixman",
                       WLR_LIBINPUT_NO_DEVICES="1",
                       WLR_HEADLESS_OUTPUTS=cls.screens,
                       TESSERA_STARTUP="true")
        # A desktop under test must not drift out from under the tests, so it is
        # given a config of its own: no idle lock, no screen blanking, no
        # workspace animation. Left at the defaults, the five-minute idle lock
        # would shut a class out on a slow machine, and a screen turning itself
        # off halfway through a test is not a thing worth debugging.
        folder = os.path.join(cls.env["XDG_CONFIG_HOME"], "tessera")
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, "config"), "w", encoding="utf-8") as f:
            f.write("idle_lock_minutes=0\n")
            f.write("idle_screen_off_minutes=0\n")
            f.write("animations=false\n")
        cls.desktop = subprocess.Popen([BINARY], stdout=cls.log, stderr=subprocess.STDOUT,
                                       env=cls.env, start_new_session=True)
        if not until(cls.sockets):
            output = cls.output()
            cls.stop_desktop()
            raise AssertionError(f"the window manager did not start:\n{output}")
        cls.env["TESSERA_SOCK"] = cls.sockets()[0]

    @classmethod
    def tearDownClass(cls):
        code = cls.stop_desktop()
        text = cls.output()
        cls.log.close()
        shutil.rmtree(cls.runtime, ignore_errors=True)
        if code is None:
            raise AssertionError("the window manager did not close when asked to; it had to be "
                                 f"killed:\n{text}")
        if code < 0:
            raise AssertionError(f"the window manager died ({signal.Signals(-code).name}) "
                                 f"while closing down:\n{text}")

    @classmethod
    def stop_desktop(cls):
        """Stops the desktop with SIGTERM, which is what a session manager
        sends and what the desktop treats as a request to close down. A signal
        rather than `tessera msg exit`, because that command is refused while
        the screen is locked - and one class here tests exactly that. Returns
        the exit status (None if it never stopped)."""
        if cls.desktop.poll() is not None:
            return cls.desktop.returncode
        try:
            os.killpg(os.getpgid(cls.desktop.pid), signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            cls.desktop.terminate()
        try:
            return cls.desktop.wait(timeout=20)
        except subprocess.TimeoutExpired:
            cls.desktop.kill()
            cls.desktop.wait(timeout=20)
            return None

    @classmethod
    def sockets(cls):
        return sorted(glob.glob(os.path.join(cls.runtime, "tessera-wayland-*.sock"))) or None

    @classmethod
    def output(cls):
        """What the desktop wrote, for failure messages."""
        cls.log.flush()
        cls.log.seek(0)
        text = cls.log.read()
        cls.log.seek(0, os.SEEK_END)
        return text[-4000:]

    # ---------------------------------------------------------- the protocol

    def ask(self, *words, expect_ok=True):
        """`tessera msg ...` run against the desktop; returns its answer. A
        reply the bar could not read is a failure in itself, so it is parsed
        here rather than looked at as text. A command refused when it should
        have worked fails with what the desktop wrote, because that is where the
        reason is; `expect_ok=False` is for the refusals a test is after."""
        command = [BINARY, "msg"] + list(words)
        try:
            result = subprocess.run(command, capture_output=True, text=True,
                                    env=self.env, timeout=60)
        except subprocess.TimeoutExpired:
            self.fail(f"{command!r} never answered\n{self.output()}")
        line = result.stdout.strip()
        if not line:
            self.fail(f"{command!r} answered nothing (exit {result.returncode})\n"
                      f"{self.output()}")
        try:
            answer = json.loads(line.splitlines()[0])
        except ValueError as error:
            self.fail(f"{command!r} answered something the bar could not read ({error}): "
                      f"{line[:200]!r}\n{self.output()}")
        if expect_ok and answer.get("ok") is False:
            self.fail(f"{command!r} was refused: {answer.get('error')!r}\n{self.output()}")
        return answer

    def state(self):
        """The state line, with the parts the bar relies on checked."""
        state = self.ask("state")
        self.assertEqual(state.get("event"), "state")
        for key in ("workspace", "workspaces", "occupied", "urgent", "outputs", "title",
                    "app_id", "locked", "theme", "night_light", "compact"):
            self.assertIn(key, state, f"a state line without {key!r}")
        self.assertGreaterEqual(state["workspace"], 1)
        self.assertLessEqual(state["workspace"], state["workspaces"])
        for key in ("occupied", "urgent"):
            self.assertIsInstance(state[key], list)
            for number in state[key]:
                self.assertGreaterEqual(number, 1)
                self.assertLessEqual(number, state["workspaces"])
        for key in ("locked", "night_light", "compact"):
            self.assertIsInstance(state[key], bool, f"{key} is not true or false")
        self.assertIsInstance(state["title"], str)
        self.assertIn(state["theme"], ("dark", "light"))
        return state

    def wait_state(self, **wants):
        """Waits until the desktop reports these things about itself."""
        def matches():
            state = self.state()
            return all(state.get(key) == want for key, want in wants.items())
        if not until(matches):
            self.fail(f"the desktop never reported {wants}; it says {self.state()}\n"
                      f"{self.output()}")
        return True

    def client(self):
        """A socket of our own, for watching rather than asking."""
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(60)
        sock.connect(self.env["TESSERA_SOCK"])
        return sock


class Desktop(DesktopMixin, unittest.TestCase):

    def test_the_plain_commands_answer(self):
        shown = subprocess.run([BINARY, "--version"], capture_output=True, text=True,
                               env=self.env, timeout=30)
        self.assertEqual(shown.stdout.strip(), f"tessera {version()}")
        self.assertEqual(self.ask("version").get("version"), version())
        usage = subprocess.run([BINARY, "msg"], capture_output=True, text=True,
                               env=self.env, timeout=30)
        self.assertNotEqual(usage.returncode, 0)
        self.assertIn("Usage: tessera msg", usage.stderr)
        unknown = self.ask("nothing-such", expect_ok=False)
        self.assertIn("unknown command", unknown.get("error", ""))

    def test_the_socket_is_only_for_this_user(self):
        mode = os.stat(self.env["TESSERA_SOCK"]).st_mode & 0o777
        self.assertEqual(mode, 0o600, "this socket can start programs")

    def test_the_screens_it_lists_are_the_ones_it_made(self):
        listed = self.ask("outputs")["outputs"]
        self.assertEqual(len(listed), int(self.screens))
        for output in listed:
            self.assertTrue(output["name"], "a screen with no name")
            self.assertTrue(output["enabled"])
            self.assertGreater(output["width"], 0)
            self.assertGreater(output["height"], 0)
            self.assertGreaterEqual(output["scale"], 1)
            self.assertIsInstance(output["modes"], list)
            for key in ("make", "model", "builtin", "main", "mirror", "phys_width",
                        "logical_width", "x", "y", "refresh"):
                self.assertIn(key, output)

    def test_workspaces_are_kept_and_reported(self):
        """Every screen keeps its own workspace number and says it in the state
        line. The numbers here are ones no other test in this class uses: one
        desktop is shared by the class, so a test starts where the last one left
        off, and that is worth knowing rather than working around."""
        self.ask("workspace", "7")
        self.wait_state(workspace=7)
        # Next and previous go at most one empty workspace past the last one in
        # use, so with no windows anywhere "next" has nowhere new to go.
        self.ask("workspace", "next")
        self.wait_state(workspace=7)
        self.ask("workspace", "prev")
        self.wait_state(workspace=6)
        # Out of range is refused by doing nothing, rather than by jumping to
        # the first or the last workspace.
        for refused in ("0", "40", "-2", "ten"):
            self.ask("workspace", refused)
            self.wait_state(workspace=6)
        self.ask("workspace", "9")
        self.wait_state(workspace=9)
        self.ask("workspace", "prev")
        self.wait_state(workspace=8)
        # A screen that is not there is said so, quietly.
        answer = self.ask("workspace", "2", "HDMI-NOPE", expect_ok=False)
        self.assertIn("no such screen", answer.get("error", ""))

    def test_a_desktop_with_no_windows_survives_window_commands(self):
        """All of these act on the focused window; with none of them may they
        answer and leave the desktop standing."""
        for words in (("close",), ("fullscreen",), ("float",), ("move-to-workspace", "3"),
                      ("focus", "17"), ("night-light", "toggle"), ("night-light", "on"),
                      ("night-light", "off"), ("reload",)):
            self.ask(*words)  # (an answer of "refused" fails the test itself)
        state = self.state()  # (and the desktop is still there to answer)
        self.assertFalse(state["locked"])

    def test_the_screens_can_be_switched_off_and_back(self):
        self.ask("dpms", "off")
        self.ask("dpms", "on")
        self.wait_state(locked=False)

    def test_exec_runs_what_it_is_asked_to(self):
        marker = os.path.join(self.runtime, "started-by-the-desktop")
        self.ask("exec", f"touch {marker}")
        self.assertTrue(until(lambda: os.path.exists(marker), 20),
                        "the desktop did not run the command it was given")

    def test_a_request_too_long_is_refused_and_forgiven(self):
        """The socket reads one line at a time, so a request over the limit
        gets one refusal and the connection has to work normally afterwards."""
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(60)
            client.connect(self.env["TESSERA_SOCK"])
            client.sendall(b"exec " + b"x" * 4000 + b"\n")
            stream = client.makefile()
            first = json.loads(stream.readline())
            self.assertFalse(first["ok"])
            self.assertIn("too long", first["error"])
            client.sendall(b"state\n")
            second = json.loads(stream.readline())
            self.assertEqual(second.get("event"), "state", "the socket never recovered")

    def test_a_command_the_cli_refuses_before_reaching_out(self):
        """`tessera msg` makes one line out of the words and refuses it before
        connecting, so the desktop never sees half a request."""
        result = subprocess.run([BINARY, "msg", "exec", "y" * 4000], capture_output=True,
                                text=True, env=self.env, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("too long", result.stderr.lower())

    def test_subscribing_gets_the_state_and_the_next_change(self):
        """The bar watches one line per change; only real changes are sent."""
        client = self.client()
        try:
            client.sendall(b"subscribe\n")
            stream = client.makefile()
            opening = json.loads(stream.readline())
            self.assertEqual(opening.get("event"), "state")
            self.ask("workspace", "2")
            for _ in range(20):
                line = json.loads(stream.readline())
                if line.get("event") == "state" and line.get("workspace") == 2:
                    break
            else:
                self.fail("no state line came through after the change")
        finally:
            client.close()


class ManyScreens(DesktopMixin, unittest.TestCase):
    """More screens than the state line used to have room for.

    The state and the screen list were once written into buffers of a size
    settled in advance, which cut the line in half on the fourth screen; the
    bar was left with text it could not read. Six screens are asked of the
    headless backend here, because six is more than that buffer held; the
    desktop has to report all of them, in the same order, in both lines.
    """

    screens = "6"

    def test_every_screen_appears_in_the_state_line(self):
        listed = self.ask("outputs")["outputs"]
        self.assertEqual(len(listed), int(self.screens),
                         "the desktop did not make the screens it was asked for")
        state = self.state()
        names = [entry["name"] for entry in state["outputs"]]
        self.assertEqual(names, [entry["name"] for entry in listed],
                         "the state line and the screen list disagree")
        self.assertEqual(len(set(names)), len(names), f"the same screen twice: {names}")
        for entry in state["outputs"]:
            self.assertEqual(sorted(entry), ["name", "occupied", "urgent", "workspace"])
            self.assertIsInstance(entry["occupied"], list)
            self.assertIsInstance(entry["urgent"], list)
            self.assertGreaterEqual(entry["workspace"], 1)
            self.assertLessEqual(entry["workspace"], state["workspaces"])

    def test_each_screen_keeps_its_own_workspace(self):
        """A bar asks about its own screen by name, so a screen's workspace has
        to be its own and has to come back in the state line."""
        names = [entry["name"] for entry in self.state()["outputs"]]
        for number, name in enumerate(names, start=1):
            self.ask("workspace", str(number), name)
        state = self.state()
        for number, name in enumerate(names, start=1):
            entry = [item for item in state["outputs"] if item["name"] == name][0]
            self.assertEqual(entry["workspace"], number, f"{name} did not move to {number}")


class Locked(DesktopMixin, unittest.TestCase):
    """What the lock screen shuts out, and what it leaves alone."""

    screens = "1"

    def test_the_lock_screen_closes_the_desktop(self):
        # The idle lock is turned off in the config this desktop was given, so
        # nothing but this test can lock it.
        self.wait_state(locked=False)
        self.ask("lock")
        self.wait_state(locked=True)
        # A second request over the socket is refused rather than starting
        # another lock screen; the key binding goes straight in, because
        # `lock_engage` itself does nothing when it is already locked.
        again = self.ask("lock", expect_ok=False)
        self.assertIn("locked", again.get("error", ""))
        self.wait_state(locked=True)

        refused = (("exec", "touch nowhere"), ("logout",), ("exit",), ("reload",),
                   ("dpms", "off"), ("workspace", "7"), ("move-to-workspace", "2"),
                   ("close",), ("focus", "1"), ("fullscreen",), ("float",),
                   ("night-light", "on"))
        for words in refused:
            answer = self.ask(*words, expect_ok=False)
            self.assertIn("locked", answer.get("error", ""), f"{words} gave the wrong reason")

        # Watching is still allowed: that is how the bar keeps up with the
        # lock, and the desktop has to go on answering.
        self.wait_state(locked=True)
        self.assertTrue(self.ask("outputs")["outputs"])
        self.assertEqual(self.ask("version").get("version"), version())
        client = self.client()
        try:
            client.sendall(b"subscribe\n")
            opening = json.loads(client.makefile().readline())
            self.assertEqual(opening.get("event"), "state")
            self.assertTrue(opening["locked"])
        finally:
            client.close()
        # The desktop is still running, and it is stopped by a signal at the
        # end of the test rather than by its own exit command - which, as set
        # up above, is refused while locked. That last part of its life is
        # where the screens are handed back on, so check it ends without a
        # crash (tearDownClass looks at how it exited).
        self.assertIsNone(self.desktop.poll())


class TestWindow:
    """The one window, started and read by the tests below.

    It is a real Wayland client rather than a mock, because the things these
    tests care about only happen on the other side of the socket: what the
    desktop asks a window to be sized like, which interfaces it offers to a
    program that connects while the screen is locked, and whether a window that
    is closed from the control socket really goes away. The client prints one
    line per event and flushes it, so a test reads lines rather than guessing.
    """

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if not os.path.isfile(CLIENT):
            raise unittest.SkipTest(f"needs the test window built (no {CLIENT})")

    def display(self):
        """This desktop's Wayland display name, from the control socket beside
        it: the window manager names the two together, and a client needs the
        first while the tests here talk over the second."""
        name = os.path.basename(self.env["TESSERA_SOCK"])
        return name.removesuffix(".sock").removeprefix("tessera-")

    def window(self, *extra, wait_for="mapped"):
        """Starts the client and waits until it says it has a window."""
        env = dict(self.env, WAYLAND_DISPLAY=self.display())
        # The client gives up on its own after a minute, so a test that stops
        # reading cannot leave a window in the desktop for the classes after it;
        # every wait below is shorter than that.
        process = subprocess.Popen(
            [CLIENT, "--window", "--idle-ms", "60000", *extra],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        self.processes.append(process)
        lines = self.read_lines(process, wait_for)
        if not any(line.startswith(wait_for) for line in lines):
            self.fail(f"the test window never said {wait_for!r}; it said {lines}")
        return process, lines

    @staticmethod
    def read_lines(process, want, seconds=25):
        """Everything the client wrote, up to and including a line that starts
        with `want`. Lines already read are kept, so a test can look back."""
        seen = getattr(process, "seen", None)
        if seen is None:
            seen = process.seen = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if any(line.startswith(want) for line in seen):
                return seen
            ready, _, _ = select.select([process.stdout], [], [], 0.2)
            if not ready:
                if process.poll() is not None:
                    break
                continue
            line = process.stdout.readline()
            if not line:
                break
            seen.append(line.strip())
        return seen

    def stop(self, process):
        """Ends a client and waits, so a test that looks at the desktop
        afterwards is not competing with a window that is still closing."""
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=20)

    def teardown_windows(self):
        for process in getattr(self, "processes", []):
            self.stop(process)


class AWindowInTheDesktop(TestWindow, DesktopMixin, unittest.TestCase):
    """What the bar can see when there is something for it to see.

    Every assertion here is about the pair - the desktop and a client that
    answers it - because each half looks right alone: a state line can list a
    window that was never configured, and a window can be mapped while the
    desktop counts nothing.
    """

    screens = "1"

    def setUp(self):
        self.processes = []
        self.addCleanup(self.teardown_windows)

    def test_a_window_shows_up_where_the_bar_looks(self):
        empty = self.state()
        self.assertEqual(empty["occupied"], [], "the desktop started with windows in it")
        self.assertEqual(empty["title"], "")
        process, lines = self.window()

        # The size the client paints is the size the desktop asked for, not the
        # one it would have liked: that is the whole of a tiling compositor.
        configured = self.configure_sizes(lines)
        mapped = [line.split()[1] for line in lines if line.startswith("mapped ")]
        wanted = f"{configured[-1][0]}x{configured[-1][1]}"
        self.assertEqual(mapped, [wanted],
                         f"the window painted something other than what it was told: {lines}")

        def shown():
            state = self.state()
            return state["occupied"] == [1] and state["title"] == "Tessera test window"
        if not until(shown):
            self.fail(f"the desktop never counted the window; it says {self.state()}\n"
                      f"{self.output()}")
        self.assertEqual(self.state()["app_id"], "tessera-test-window")

    def test_a_title_the_client_chose_is_a_title_the_bar_shows(self):
        self.window("--title", "Blue rectangle", "--app-id", "renamed")
        if not until(lambda: self.state()["title"] == "Blue rectangle"):
            self.fail(f"the desktop did not use the window's own title: {self.state()}")
        self.assertEqual(self.state()["app_id"], "renamed")

    def test_the_desktop_moves_it_and_the_bar_follows(self):
        self.window()
        self.ask("move-to-workspace", "3")
        if not until(lambda: self.state()["occupied"] == [3]):
            self.fail(f"the window did not move to workspace 3; the desktop says "
                      f"{self.state()}\n{self.output()}")
        # Whether the desktop follows a window to its new workspace or stays
        # where it was, the window is on 3 and nowhere else has it - which is
        # the half that a state line built from the focused view can get wrong.
        self.ask("workspace", "1")
        if not until(lambda: self.state()["occupied"] == [] and self.state()["title"] == ""):
            self.fail(f"workspace 1 still shows the window: {self.state()}")
        self.ask("workspace", "3")
        if not until(lambda: self.state()["title"] == "Tessera test window"):
            self.fail(f"the title did not come back with the workspace: {self.state()}")

    def test_closing_it_from_the_socket_takes_it_with_it(self):
        process, _lines = self.window()
        self.ask("close")
        lines = self.read_lines(process, "closed", seconds=20)
        self.assertIn("closed", lines, f"the window was never told to close: {lines}")
        if not until(lambda: self.state()["occupied"] == []):
            self.fail(f"the desktop still has a window after closing it: {self.state()}")

    def test_two_windows_share_the_screen(self):
        """The tiling maths in `layout.c`, seen from the side a client can check.

        `compositor/tests/tile_test.c` works the geometry out for N windows on
        one screen; what nothing checked until now is that the answer reaches the
        windows - that the second one is asked to be smaller, and the first is
        told again rather than left the size it was.
        """
        first, lines = self.window()
        one_window = self.configure_sizes(lines)[-1]
        _second, second_lines = self.window("--app-id", "second", "--title", "Second")
        both = self.configure_sizes(second_lines)
        self.assertTrue(both, f"the second window was never configured: {second_lines}")
        self.assertLess(both[-1][0] * both[-1][1], one_window[0] * one_window[1],
                        f"two windows did not share the screen: {one_window} then {both}")
        if not until(lambda: len(self.configure_sizes(first)) > 1):
            self.fail(f"the first window was never told about the second: {first.seen}")
        self.assertEqual(self.configure_sizes(first)[-1], both[-1],
                         "the two windows were not given the same shape")

    def test_fullscreen_leaves_the_size_it_found(self):
        """Coming back out of fullscreen is a promise about the size, not about
        the paint job: a window that returns to a *different* place than it left
        is the same bug as a screen mode change that snaps it to (0,0)."""
        process, lines = self.window()
        before = self.configure_sizes(lines)[-1]
        self.ask("fullscreen")
        self.ask("fullscreen")

        def restored():
            sizes = self.configure_sizes(process)
            return bool(sizes) and sizes[-1] == before

        if not until(restored):
            self.fail(f"leaving fullscreen did not restore the size: was {before}, "
                      f"the client says {self.configure_sizes(process)}")

    @staticmethod
    def configure_sizes(source):
        """Every `configure WxH` the client printed, as (width, height) pairs.

        Takes the process or its lines: a client keeps what it has said, so a
        test can watch one grow while it waits for the other to follow.
        """
        lines = source if isinstance(source, list) else source.seen
        sizes = []
        for line in lines:
            parts = line.split()
            if len(parts) == 2 and parts[0] == "configure" and "x" in parts[1]:
                width, _, height = parts[1].partition("x")
                sizes.append((int(width), int(height)))
        return sizes


class WhatTheLockTakesAway(TestWindow, DesktopMixin, unittest.TestCase):
    """The interfaces a program sees, before and after the screen is locked.

    The lock works by not offering them at all, rather than by refusing each
    request, so the only honest test is to ask a client what it can see. What it
    cannot be tested doing is taking them back from something that had them
    already - see the note in `filter_global()` and the README - so this stops at
    the boundary the code actually holds.
    """

    screens = "1"

    def setUp(self):
        self.processes = []
        self.addCleanup(self.teardown_windows)

    def globals_of(self):
        """The interfaces this desktop advertises, as names."""
        done = subprocess.run([CLIENT, "--globals"], capture_output=True, text=True,
                              env=dict(self.env, WAYLAND_DISPLAY=self.display()), timeout=60,
                              check=False)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return [line.split()[1] for line in done.stdout.splitlines() if line.startswith("global ")]

    def test_the_screen_tools_are_offered_and_then_not(self):
        before = self.globals_of()
        for name in ("zwlr_screencopy_manager_v1", "zwlr_data_control_manager_v1",
                     "zwlr_output_manager_v1"):
            self.assertIn(name, before, f"the desktop never offered {name} at all")

        self.ask("lock")
        self.wait_state(locked=True)
        after = self.globals_of()
        for name in ("zwlr_screencopy_manager_v1", "zwlr_export_dmabuf_manager_v1",
                     "zwlr_data_control_manager_v1", "zwlr_output_manager_v1"):
            self.assertNotIn(name, after, f"{name} is still on offer behind the lock screen")
        # The window manager is still there, and so is the way to talk to it: a
        # locked screen hides the desktop, it does not disconnect the session.
        self.assertIn("wl_compositor", after)
        self.assertIn("xdg_wm_base", after)

    def test_a_window_that_was_up_stays_up_and_a_new_one_still_opens(self):
        """Locking is not a logout: the apps keep running, and one that starts
        now can still be opened (it simply cannot see or be seen)."""
        process, _ = self.window()
        self.ask("lock")
        self.wait_state(locked=True)
        self.assertEqual(self.state()["occupied"], [1], "locking took a window away")
        other, _ = self.window("--app-id", "second", "--title", "Second")
        self.assertIsNone(process.poll(), "the first window was closed by the lock")
        self.assertIsNone(other.poll(), "a window could not be opened while locked")
        self.assertEqual(self.state()["occupied"], [1])


if __name__ == "__main__":
    unittest.main()

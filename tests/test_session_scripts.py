"""The scripts that hold a session together, read as text.

`tessera-session`, `tessera-autostart` and `polkit-tessera.rules` run before the
desktop has any say in the matter: nothing imports them, no compiler looks at
them, and a mistake in one is found by the person who cannot log in. They are
also the parts most likely to be edited by hand on a machine. So they are read
here, one level below where they run, and the checks are about the *shape* of
each one - the things that stop a small session from becoming a disk full of
logs, a restart loop, or a panel with more power than it needs.
"""

import os
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DATA = ROOT / "data"


def script(name):
    return (DATA / name).read_text(encoding="utf-8")


class LogsThatStop(unittest.TestCase):
    """A log nobody caps is a disk nobody can log in with.

    The session script caps them when a session starts; the autostart script
    caps the bar's log while the desktop runs, because that is the one that can
    grow while nobody is watching. The two have to agree on the size, or the
    cap is a rumour.
    """

    def test_both_of_them_cap_the_log(self):
        session = script("tessera-session")
        autostart = script("tessera-autostart")
        for name, text in (("tessera-session", session), ("tessera-autostart", autostart)):
            self.assertIn("LOG_KEEP=", text, f"{name} has no idea what a log should cost")
            self.assertIn("stat -c %s", text, f"{name} never looks at the size it caps")
            self.assertIn(
                "tail -c", text,
                f"{name} throws the log away instead of keeping the end of it, which is the "
                "part worth reading")

    def test_the_two_agree_on_the_size(self):
        figures = []
        for name in ("tessera-session", "tessera-autostart"):
            text = script(name)
            line = [row for row in text.splitlines() if row.startswith("LOG_KEEP=")]
            self.assertEqual(len(line), 1, f"{name} sets the log size more than once")
            figures.append(line[0].strip())
        self.assertEqual(figures[0], figures[1],
                         "the session and the bar cap the same file at different sizes")

    def test_the_previous_session_is_kept_but_short(self):
        """One rotation, kept small - not the whole history of the machine, and
        not nothing at all the moment something has gone wrong."""
        session = script("tessera-session")
        start = session.index("for log in tessera shell")
        loop = session[start:session.index("done", start)]
        self.assertIn('mv -f "$state/$log.log" "$state/$log.log.old"', loop)
        self.assertLess(loop.index("mv -f"), loop.index('keep_small "$state'),
                        "the log is trimmed before it is rotated, which trims the file that "
                        "was already capped and leaves the one that grew")


class RestartLoop(unittest.TestCase):
    """The bar is restarted, but not into the ground.

    A bar that dies at once - a half-installed update, a setting it cannot read
    - used to be relaunched as fast as the machine allowed, each attempt mapping
    windows over the desktop and burying its own reason in the log.
    """

    def setUp(self):
        self.text = script("tessera-autostart")

    def test_there_is_always_a_wait(self):
        loop = self.text[self.text.index("while :; do"):]
        # The wait is not inside a condition any more: it is what every restart
        # costs, and the only choice is how long.
        self.assertIn('sleep "$delay"', loop)
        self.assertNotIn("sleep 5\n", loop, "the wait is back to a fixed pause after a fast "
                                            "crash, which is the case that loops forever")

    def test_the_wait_grows_while_the_bar_keeps_dying(self):
        self.assertIn("delay=$(( delay * 2 ))", self.text)
        self.assertIn('if [ "$ran" -ge 10 ]', self.text,
                      "nothing distinguishes a bar that lasted from one that did not")
        self.assertIn("delay=2", self.text, "a bar that has run a while is started again "
                                            "with a long wait")
        cap = [line for line in self.text.splitlines() if '"$delay" -lt' in line]
        self.assertEqual(len(cap), 1, "the wait can grow without limit")

    def test_the_wait_stops_growing_at_a_minute(self):
        """A day of crashing should not mean a day between looks, either: the
        point is to stop hammering, not to stop noticing."""
        self.assertIn("delay=60", self.text)
        loop = self.text[self.text.index("while :; do"):self.text.index("\ndone")]
        self.assertLess(loop.index('"$delay" -gt 60'), loop.index("delay=60"),
                        "the wait doubles past the limit it names")

    def test_the_first_restart_is_prompt(self):
        """The wait is used before it is grown, so the first restart after a
        crash is two seconds away rather than four - and a bar that came back
        once is not treated as a bar that keeps failing."""
        loop = self.text[self.text.index("while :; do"):self.text.index("\ndone")]
        self.assertLess(loop.index('sleep "$delay"'), loop.index("delay=$(( delay * 2 ))"))

    def test_a_run_is_measured_before_the_wait(self):
        """How long the bar lasted, not how long the bar and its own pause took
        together: a loop that counts waiting as a healthy run resets the delay
        every few restarts and never settles down."""
        loop = self.text[self.text.index("while :; do"):self.text.index("\ndone")]
        self.assertLess(loop.index("ran=$(("), loop.index('sleep "$delay"'),
                        "the delay is grown from a run that includes the previous delay")

    def test_a_restart_stops_when_the_desktop_is_gone(self):
        """The loop ends with the desktop, not with the sleep: a bar restarted
        after the window manager exited has nowhere to draw."""
        self.assertEqual(self.text.count('kill -0 "${TESSERA_PID:-}"'), 2,
                         "there should be a look at the desktop before the wait and after it")


class NetworkRights(unittest.TestCase):
    """What the quick-settings panel may do without a password.

    The rules file used to hand over the whole NetworkManager action prefix to
    anyone in the `netdev` group, which is every power the daemon has: turning
    all networking off, putting it to sleep, reloading its configuration. Only
    the five the panel actually uses are granted now, so the file is checked
    against what the panel does.
    """

    GRANTED = {
        "org.freedesktop.NetworkManager.enable-disable-wifi",
        "org.freedesktop.NetworkManager.wifi.scan",
        "org.freedesktop.NetworkManager.network-control",
        "org.freedesktop.NetworkManager.settings.modify.system",
        "org.freedesktop.NetworkManager.settings.modify.own",
    }
    # Powers NetworkManager has, that a panel at the bottom of the screen has no
    # business holding.
    DENIED = {
        "org.freedesktop.NetworkManager.enable-disable-network",
        "org.freedesktop.NetworkManager.enable-disable-wwan",
        "org.freedesktop.NetworkManager.enable-disable-bluetooth",
        "org.freedesktop.NetworkManager.sleep-wake",
        "org.freedesktop.NetworkManager.reload",
        "org.freedesktop.NetworkManager.settings.modify.global",
    }

    def setUp(self):
        self.rules = (DATA / "polkit-tessera.rules").read_text(encoding="utf-8")

    def actions(self):
        """The action ids the rule grants, which is the whole file's business."""
        return set(re.findall(r'"(org\.freedesktop\.NetworkManager\.[\w.-]+)":\s*true',
                              self.rules))

    def test_every_action_is_named(self):
        """Prefix matching is the bug: a new action the day daemon invents, or
        one it keeps for emergencies, belongs to the person installing, not to
        whoever wrote this file a year ago."""
        self.assertNotIn("indexOf", self.rules, "the rule matches a prefix of actions again")
        self.assertEqual(self.actions(), self.GRANTED,
                         f"granted {sorted(self.actions())}")

    def test_the_powers_nothing_should_have_are_left_out(self):
        granted = self.actions()
        for action in sorted(self.DENIED):
            self.assertNotIn(action, granted, f"{action} is granted to the panel")

    def test_what_is_granted_is_used(self):
        """A grant for something the panel stopped doing is a permission with no
        owner. Each one is here because of a call in services.py."""
        calls = (ROOT / "shell" / "tessera_shell" / "services.py").read_text(encoding="utf-8")
        for needle in ("WirelessEnabled", "RequestScan", "ActivateConnection",
                       "AddAndActivateConnection"):
            why = f"the panel no longer makes the {needle} call"
            self.assertIn(needle, calls, why + "; the rule granting it should say why")

    def test_the_conditions_stay(self):
        for condition in ('isInGroup("netdev")', "subject.local", "subject.active"):
            self.assertIn(condition, self.rules, f"the rule stopped checking {condition}")


class LogRotation(unittest.TestCase):
    """The rotation run for real, on files of our own.

    Nothing else executes these lines before the desktop has any say in the
    matter, so the block is lifted out of the script and run: what matters is
    not that a line mentioning `tail` is there, it is that a five megabyte log
    ends up small with its last lines intact.
    """

    def rotate(self, folder, files):
        """Runs the session script's log block against a folder of temp files."""
        session = script("tessera-session")
        start = session.index("LOG_KEEP=")
        block = session[start:session.index("\ndone\n", start) + len("\ndone\n")]
        runner = folder / "rotate.sh"
        runner.write_text(f'state="{folder}"\n{block}', encoding="utf-8")
        for name, content in files.items():
            (folder / name).write_bytes(content)
        done = subprocess.run(["/bin/sh", str(runner)], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_a_long_log_ends_up_short_and_keeps_its_end(self):
        with tempfile.TemporaryDirectory() as base:
            folder = pathlib.Path(base)
            tail = b"the reason the desktop died\n"
            self.rotate(folder, {"tessera.log": b"." * (5 * 1024 * 1024) + tail})
            kept = folder / "tessera.log.old"
            self.assertFalse((folder / "tessera.log").exists(),
                             "the session would append to last session's log")
            data = kept.read_bytes()
            self.assertLessEqual(len(data), 256 * 1024, "the cap was not applied")
            self.assertTrue(data.endswith(tail),
                            "what is kept is the end of the log, not the start")

    def test_a_log_worth_keeping_is_kept_exactly(self):
        with tempfile.TemporaryDirectory() as base:
            folder = pathlib.Path(base)
            self.rotate(folder, {"shell.log": b"bar started\n", "shell.log.old": b"last time\n"})
            self.assertEqual((folder / "shell.log.old").read_bytes(), b"bar started\n")
            self.assertFalse((folder / "shell.log").exists())

    def test_a_leftover_is_capped_even_with_nothing_to_rotate(self):
        """A session that never came back leaves an oversized log behind, and no
        second chance to trim it."""
        with tempfile.TemporaryDirectory() as base:
            folder = pathlib.Path(base)
            self.rotate(folder, {"tessera.log.old": b"x" * (9 * 1024 * 1024)})
            self.assertLessEqual((folder / "tessera.log.old").stat().st_size, 256 * 1024)


class RestartLoopRuns(unittest.TestCase):
    """The loop itself, run with a bar that always fails and a desktop that
    leaves on schedule.

    The same text as the installed script - the loop and its trim, lifted out of
    `tessera-autostart` - with two fakes on PATH: a bar that exits at once, and a
    `sleep` that only writes down the number it was given. A delay that never
    grows, a wait that counts itself as a healthy run, or a loop that outlives
    the desktop are all things a person finds out by waiting in front of a screen
    that never comes back, which is the wrong place to find them out.
    """

    def run_the_loop(self, folder, restarts, log=None):
        """Runs the real loop until the fake bar has started `restarts` times,
        and returns the waits it asked for, how many times the bar ran, and the
        log file it writes to.

        The pretend desktop is started by the harness, and dies by name when the
        bar has run its last time. It has to be the harness's own child for that
        to be worth anything: a process killed by someone else stays a zombie
        until its parent looks, and `kill -0` answers a zombie as if it were
        alive - so a loop tested against a desktop the test itself started would
        never be seen to stop.
        """
        autostart = script("tessera-autostart")
        start = autostart.index("LOG_KEEP=")
        end = autostart.index("\n) &", start) + len("\n) &")
        bin_dir = folder / "bin"
        bin_dir.mkdir()
        (folder / "count").write_text("0", encoding="utf-8")
        (folder / "trace").write_text("", encoding="utf-8")
        (folder / "shell.log").write_bytes(log or b"")
        # Scripts on a PATH of our own rather than shell functions: the loop runs
        # in a subshell, and a function does not cross into the next process.
        self._fake(bin_dir / "tessera-shell",
                   "#!/bin/sh\n"
                   'count=$(cat "$T_TEMP/count")\n'
                   "count=$((count + 1))\n"
                   'echo "$count" >"$T_TEMP/count"\n'
                   # The last go takes the desktop down with it, which is the
                   # reason the loop is meant to end.
                   'if [ "$count" = "$T_RESTARTS" ]; then kill "$TESSERA_PID" 2>/dev/null; fi\n'
                   "exit 1\n")
        self._fake(bin_dir / "sleep", '#!/bin/sh\necho "$1" >>"$T_TEMP/trace"\nexit 0\n')
        harness = (f'#!/bin/sh\nstate="{folder}"\nT_TEMP="{folder}"\nT_RESTARTS={restarts}\n'
                   f'export T_TEMP T_RESTARTS\nPATH="{bin_dir}:$PATH"\n'
                   "/bin/sleep 30 &\nTESSERA_PID=$!\nexport TESSERA_PID\n"
                   f'{autostart[start:end]}\nwait\n')
        (folder / "loop.sh").write_text(harness, encoding="utf-8")
        done = subprocess.run(["/bin/sh", str(folder / "loop.sh")], timeout=30,
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        waits = [int(word) for word in (folder / "trace").read_text().split()]
        return waits, int((folder / "count").read_text()), folder / "shell.log"

    def _fake(self, path, body):
        """One of the two programs the loop is made to run against."""
        path.write_text(body, encoding="utf-8")
        path.chmod(0o755)

    def test_the_waits_double_from_two_seconds(self):
        with tempfile.TemporaryDirectory() as base:
            waits, runs, _ = self.run_the_loop(pathlib.Path(base), restarts=5)
            self.assertEqual(runs, 5, "the bar was not started once per turn of the loop")
            self.assertEqual(waits[:4], [2, 4, 8, 16],
                             "a bar that dies at once should be restarted with pauses that "
                             f"grow; they were {waits}")

    def test_the_first_restart_is_two_seconds_away(self):
        """Not four, and not nothing: prompt enough to shrug off a hiccup, slow
        enough that a bar which cannot start is not a machine-gun."""
        with tempfile.TemporaryDirectory() as base:
            waits, runs, _ = self.run_the_loop(pathlib.Path(base), restarts=2)
            self.assertEqual(waits[0], 2)
            self.assertEqual(runs, 2, "the loop should stop with the desktop, mid-wait")

    def test_the_log_stays_small_while_the_loop_runs(self):
        with tempfile.TemporaryDirectory() as base:
            folder = pathlib.Path(base)
            waits, _, log = self.run_the_loop(folder, restarts=2, log=b"x" * (6 * 1024 * 1024))
            self.assertLessEqual(log.stat().st_size, 256 * 1024,
                                 "the trim in this loop is only for show")


class RunnerScripts(unittest.TestCase):
    """Scripts meant to be run are marked as such, in the tree and not just in
    the copy the installer lays down."""

    SCRIPTS = ("tessera-update", "tessera-session", "tessera-autostart", "tessera-shell",
               "tessera-settings")

    def test_the_scripts_can_be_run_from_a_checkout(self):
        """What "installed as 755" is worth: the copy in the folder you fixed has
        to be runnable too, or the first thing a person tries from a checkout is
        a permission denied that has nothing to do with their change."""
        for name in self.SCRIPTS:
            path = DATA / name
            self.assertTrue(path.exists(), f"data/{name} is gone")
            self.assertTrue(os.access(path, os.X_OK),
                            f"data/{name} is not executable here, and reading it with `sh` "
                            "instead is a habit that breaks the moment someone runs it directly")

    def test_the_recorded_mode_says_the_same(self):
        """The filesystem answer above is checked out from git's index, so if git
        is willing to say what it recorded, the two should not disagree."""
        done = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-s", "data/"],
                              capture_output=True, text=True)
        if done.returncode != 0:
            self.skipTest(f"git would not read the index here: {done.stderr.strip()[:200]}")
        modes = {}
        for line in done.stdout.splitlines():
            mode, _, path = line.partition(" ")
            modes[path.split("\t")[-1]] = mode
        for name in self.SCRIPTS:
            self.assertEqual(modes.get(f"data/{name}"), "100755",
                             f"data/{name} is recorded as {modes.get(f'data/{name}')}, so a "
                             "fresh checkout would not be able to run it either")


if __name__ == "__main__":
    unittest.main()

"""The installer's trust checks, run on folders of our own.

`install.sh` refuses to install anything with root rights out of a folder other
people can write to - which is right, but it is also the kind of rule that can
stop a person updating for no reason they can act on. So the folder is fixed
when it belongs to us, and only refused when it cannot be. That, and the notice
shown when the folder holds the version that is already installed, are checked
here.

The block is lifted out of `install.sh` and run on its own, so the words that
are tested are the words that ship. It needs only /bin/sh.
"""

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
INSTALL_SH = ROOT / "install.sh"

START = 'SRC_OWNER="$(stat -c %U "$SRC")"'
END = "# Automatic login"

# The variables the block reads and the three little functions it prints
# through, which install.sh defines above it, and then the mode it left the
# folder in, so a test can see a change rather than only hear about one.
HARNESS_BEFORE = """#!/bin/sh
set -eu
MODE="${MODE:-install}"
ME="$(id -un)"
SRC="$1"
PREFIX="$2"
TESSERA_VERSION="$(cat "$SRC/VERSION")"
say() { printf '\\n\\033[1m==> %s\\033[0m\\n' "$*"; }
warn() { printf '\\033[33m%s\\033[0m\\n' "$*"; }
fail() { printf '\\n\\033[31m%s\\033[0m\\n\\n' "$*"; exit 1; }
"""
HARNESS_AFTER = """
printf 'left with mode %s\\n' "$(stat -c %a "$SRC")"
"""


def trust_checks():
    """The part of install.sh that decides whether the folder can be trusted."""
    text = INSTALL_SH.read_text(encoding="utf-8")
    start = text.index(START)
    end = text.index(END, start)
    block = text[start:end]
    assert "chmod go-w" in block, "the trust checks no longer fix what they find"
    assert "INSTALLED_VERSION" in block, "the trust checks lost the version notice"
    return block


class Trust(unittest.TestCase):
    def setUp(self):
        self.base = tempfile.TemporaryDirectory(prefix="tessera-install-test-")
        self.addCleanup(self.base.cleanup)
        self.folder = pathlib.Path(self.base.name, "tessera")
        self.folder.mkdir()
        self.prefix = pathlib.Path(self.base.name, "prefix")
        (self.prefix / "share" / "tessera").mkdir(parents=True)
        self.script = pathlib.Path(self.base.name, "guard.sh")
        self.script.write_text(HARNESS_BEFORE + trust_checks() + HARNESS_AFTER, encoding="utf-8")
        self.script.chmod(0o755)

    def install(self, mode=0o755, version="1.7.1", installed=None, update=False):
        """Runs the checks against a folder with the given permissions, and
        returns its exit status and everything it printed."""
        self.folder.chmod(mode)
        (self.folder / "VERSION").write_text(version + "\n", encoding="utf-8")
        recorded = self.prefix / "share" / "tessera" / "VERSION"
        if installed is None:
            recorded.unlink(missing_ok=True)
        else:
            recorded.write_text(installed + "\n", encoding="utf-8")
        env = {"MODE": "update" if update else "install",
               "PATH": "/usr/bin:/bin:/usr/local/bin"}
        done = subprocess.run(
            ["/bin/sh", str(self.script), str(self.folder), str(self.prefix)],
            capture_output=True, text=True, env=env)
        return done.returncode, done.stdout + done.stderr

    def test_a_folder_the_group_can_write_is_closed_off(self):
        """Group or other write means another account could decide what gets
        installed as root. It is ours to take off, so it is taken off and the
        install carries on rather than stopping to ask about it."""
        code, out = self.install(mode=0o775)
        self.assertEqual(code, 0, out)
        self.assertIn("yours alone now", out, out)
        self.assertIn("left with mode 755", out, out)

    def test_a_world_writable_folder_is_closed_off_too(self):
        code, out = self.install(mode=0o777)
        self.assertEqual(code, 0, out)
        self.assertIn("yours alone now", out, out)
        self.assertIn("left with mode 755", out, out)

    def test_a_private_folder_is_not_touched_or_mentioned(self):
        """No lecture and no change when nothing is wrong: warnings people have
        learned to ignore are worth less than silence."""
        code, out = self.install(mode=0o755)
        self.assertEqual(code, 0, out)
        self.assertIn("left with mode 755", out, out)
        self.assertNotIn("yours alone", out, out)

    def test_the_notice_when_nothing_new_arrived(self):
        """The one way an update can look like it worked and change nothing."""
        code, out = self.install(installed="1.7.1", update=True)
        self.assertEqual(code, 0, out)
        self.assertIn("nothing here will change", out, out)
        self.assertIn("git status -sb", out, out)

    def test_no_notice_when_there_is_something_new(self):
        """A newer version in the folder, a first install, and an install with
        nothing recorded yet all mean: get on with it."""
        for installed, update in (("1.7.0", True), ("1.7.1", False), (None, True)):
            with self.subTest(installed=installed, update=update):
                code, out = self.install(installed=installed, update=update)
                self.assertEqual(code, 0, out)
                self.assertNotIn("nothing here will change", out, out)


if __name__ == "__main__":
    unittest.main()

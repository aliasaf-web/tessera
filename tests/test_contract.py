"""The words the two halves of Tessera have agreed on.

The window manager (C) and the bar (Python) never share a header: they talk in
strings. A key press becomes `{"event":"key","action":"volume-up"}`, the bar
sends `workspace 4`, and both sides keep their own list of what those words
mean. Nothing stops one of the lists from drifting - a renamed action is a
button that does nothing, and a command missing from `tessera msg --help` is a
feature nobody can find.

These tests read both sides as text (and the Python as a syntax tree) and
check the lists still agree. They start nothing and need no libraries, so they
run on a bare checkout, before the window manager is built.

Two more agreements are checked here, because they were broken in the same easy
way: a fact the installer writes down that nobody reads, and one limit written
twice in two files.
"""

import ast
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
INPUT_C = ROOT / "compositor" / "src" / "input.c"
IPC_C = ROOT / "compositor" / "src" / "ipc.c"
SHELL_DIR = ROOT / "shell" / "tessera_shell"


def source(path):
    return pathlib.Path(path).read_text(encoding="utf-8")


def body_of(text, start_marker):
    """A C function's text, from its signature to the brace that closes it.
    The bodies here are tab-indented, so the first brace in column zero ends
    the function."""
    start = text.index(start_marker)
    end = text.index("\n}", start)
    return text[start:end]


def functions(tree, name):
    """Every function with this name, wherever it is defined (the shell keeps
    its methods inside classes)."""
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == name:
            yield node


def names_compared_to_variable(node, variable):
    """The words a piece of code compares against a variable: `x == "a"`,
    `x in ("a", "b")`, and the keys of `{...}.get(x)`. Returns the words and
    any prefixes from `x.startswith("a-")`."""
    words, prefixes = set(), set()
    for part in ast.walk(node):
        if isinstance(part, ast.Compare) and isinstance(part.left, ast.Name) \
                and part.left.id == variable:
            for operator, other in zip(part.ops, part.comparators):
                if not isinstance(operator, (ast.Eq, ast.In)):
                    continue
                candidates = other.elts if isinstance(other, ast.Tuple) else [other]
                for candidate in candidates:
                    if isinstance(candidate, ast.Constant) and isinstance(candidate.value, str):
                        words.add(candidate.value)
        elif isinstance(part, ast.Dict):
            for key in part.keys:
                if isinstance(key, ast.Constant) and isinstance(key.value, str):
                    words.add(key.value)
        elif isinstance(part, ast.Call) and isinstance(part.func, ast.Attribute) \
                and part.func.attr == "startswith" and part.args:
            first = part.args[0]
            if isinstance(first, ast.Constant) and isinstance(first.value, str):
                prefixes.add(first.value)
    return words, prefixes


class KeyActions(unittest.TestCase):
    """What the keys do, both ways round."""

    @classmethod
    def setUpClass(cls):
        # Every action named in a send_key_event() call, including the ones
        # picked by a condition (Shift+Print asks for a region rather than the
        # whole screen).
        cls.compositor = set()
        for line in source(INPUT_C).splitlines():
            if "send_key_event(" in line and "const char" not in line:
                cls.compositor |= set(re.findall(r'"([a-z][a-z-]*)"', line))
        cls.shell = set()
        cls.prefixes = set()
        tree = ast.parse(source(SHELL_DIR / "main.py"))
        for handler in functions(tree, "on_key"):
            words, prefixes = names_compared_to_variable(handler, "action")
            cls.shell |= words
            cls.prefixes |= prefixes

    def test_the_bar_understands_every_key_the_desktop_sends(self):
        """An action the desktop emits and the bar ignores is a key that does
        nothing at all - the kind of thing nobody notices until someone asks
        why the brightness keys stopped working."""
        unhandled = sorted(self.compositor - self.shell - self.covered_by_prefix())
        self.assertEqual(unhandled, [],
                         f"the desktop sends {unhandled} and the bar has no handler for them")

    def test_the_bar_has_no_handler_for_a_key_that_sends_nothing(self):
        """The other way round: a handler nobody can reach, usually left behind
        when a binding is removed."""
        never_sent = sorted(self.shell - self.compositor)
        self.assertEqual(never_sent, [], f"the bar handles {never_sent}, which the desktop never "
                         "sends; drop the handler or add the binding")

    def covered_by_prefix(self):
        """Actions the bar matches by prefix (media keys are played through
        playerctl), counted against what the desktop actually offers."""
        return {action for action in self.compositor
                if any(action.startswith(prefix) for prefix in self.prefixes)}


class Events(unittest.TestCase):
    """The event names that start a line on the desktop's socket."""

    def test_every_event_the_bar_listens_for_is_sent(self):
        produced = set()
        for path in (ROOT / "compositor" / "src").glob("*.c"):
            produced |= set(re.findall(r'\\"event\\":\\"([a-z-]+)\\"', source(path)))
        self.assertTrue(produced, "no events found to read; did the compositor sources move?")
        tree = ast.parse(source(SHELL_DIR / "main.py"))
        wanted = set()
        for handler in functions(tree, "on_event"):
            wanted |= names_compared_to_variable(handler, "kind")[0]
        self.assertTrue(wanted, "the bar no longer dispatches on any event")
        missing = sorted(wanted - produced)
        self.assertEqual(missing, [], f"the bar listens for {missing}, which is never broadcast")


class Commands(unittest.TestCase):
    """`tessera msg COMMAND`, and the commands the bar itself sends."""

    @classmethod
    def setUpClass(cls):
        handle = body_of(source(IPC_C), "static void handle_command")
        cls.accepted = set(re.findall(r'strcmp\(line, "([a-z-]+)"\)', handle))
        # The text `tessera msg` prints when it is called without a command:
        # several C string literals stuck together.
        usage_text = source(IPC_C)
        start = usage_text.index('fprintf(stderr, "Usage: tessera msg')
        usage_text = usage_text[start:usage_text.index('");', start) + 1]
        cls.usage = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', usage_text))\
            .replace("\\n", "\n")

    def test_every_command_is_documented(self):
        """Someone reading `tessera msg` on a terminal should see every command
        the desktop answers - including the ones only the power menu uses, since
        a terminal is where you go when the menus are the problem."""
        for command in sorted(self.accepted):
            self.assertRegex(self.usage, rf"\b{re.escape(command)}\b",
                             f"{command!r} is answered by the desktop but not in the "
                             "usage text of `tessera msg`")

    def test_the_usage_text_names_real_commands(self):
        """A command in the help that the desktop refuses is worse than no help."""
        listed = set()
        _, _, rest = self.usage.partition("Commands:")
        for piece in rest.split(","):
            words = piece.split()
            if words:
                listed.add(words[0])
        invented = sorted(listed - self.accepted)
        self.assertEqual(invented, [], f"the usage text offers {invented}, which the desktop does "
                         "not accept")
        self.assertGreaterEqual(len(listed), 10, "the usage text stopped listing commands")

    def test_the_bar_only_sends_commands_that_exist(self):
        """`ipc.send("night-light", ...)` with a typo answers `unknown command`
        and the bar carries on as if it had worked."""
        sent = set()
        for path in SHELL_DIR.glob("*.py"):
            for node in ast.walk(ast.parse(source(path))):
                if not isinstance(node, ast.Call) or not node.args:
                    continue
                func = node.func
                if isinstance(func, ast.Attribute) and func.attr == "send":
                    first = node.args[0]
                    if isinstance(first, ast.Constant) and isinstance(first.value, str):
                        sent.add(first.value)
        self.assertGreaterEqual(len(sent), 5, "the bar no longer sends any commands by name")
        unknown = sorted(sent - self.accepted)
        self.assertEqual(unknown, [], f"the bar sends {unknown}, which the desktop does not know")


class Provenance(unittest.TestCase):
    """The note the installer leaves about the build it installed.

    `install.sh` writes `share/tessera/provenance`, the shell reads it from the
    same folder, and Settings prints it. The names are the whole protocol, so
    they are checked against each other: a key written but never read is a fact
    nobody sees, and a key read but never written is a row that quietly vanishes.
    """

    def setUp(self):
        self.install = source(ROOT / "install.sh")
        self.package = source(SHELL_DIR / "__init__.py")
        self.settings = source(SHELL_DIR / "settings.py")

    def provenance_block(self):
        """The few lines of install.sh that write the file."""
        start = self.install.index('PROVENANCE="$(mktemp)"')
        return self.install[start:self.install.index('rm -f "$PROVENANCE"', start)]

    def test_both_sides_look_in_the_same_folder(self):
        """The installer's `$PREFIX/share/tessera` and the shell's walk up from
        its own file have to land on the same directory."""
        self.assertIn('"$PREFIX/share/tessera/provenance"', self.install,
                      "install.sh no longer installs the provenance file beside VERSION")
        self.assertIn('install -m 644 "$SRC/VERSION" "$PREFIX/share/tessera/VERSION"',
                      self.install, "VERSION and provenance are no longer installed side by side")
        # parents[2] from shell/tessera_shell/__init__.py is share/tessera when
        # installed and the top of the source folder in a checkout, which is
        # what lets one line of code serve both.
        self.assertIn("parents[2]", self.package,
                      "the shell no longer looks beside itself for the installed files")
        self.assertIn('_shared() / "provenance"', self.package)
        self.assertIn('_shared() / "VERSION"', self.package)

    def test_every_written_key_is_read_and_understood(self):
        # Only the name matters here - the value is either "%s" or the fixed
        # word "yes" - and the whole line is a printf, so nothing else counts.
        written = set(re.findall(r"printf '(\w+)=", self.provenance_block()))
        read = set(re.findall(r'note\.get\("(\w+)"\)', self.settings))
        self.assertEqual(written, {"version", "built", "commit", "branch", "changed"},
                         f"the installer records {sorted(written)} now")
        self.assertEqual(written - {"version"}, read,
                         "written but never shown, or shown but never written: "
                         f"{sorted((written ^ read) - {'version'})}")

    def test_the_note_carries_the_version_being_installed(self):
        """About shows one version, so it cannot disagree with the desktop."""
        lines = self.provenance_block().splitlines()
        version = [line for line in lines if "version=" in line]
        self.assertEqual(len(version), 1, "the note stopped recording the version once")
        self.assertIn(r"printf 'version=%s\n'", version[0])
        self.assertIn("$TESSERA_VERSION", version[0])


class FindingTheDesktop(unittest.TestCase):
    """Both halves look for the running desktop the same way round.

    The bar (Python) and `tessera msg` (C) each work out where the control
    socket is from the runtime folder, and fall back to `TESSERA_SOCK`. The
    order is the whole point: the folder is where the desktop actually put the
    socket, and a variable is something any program you run can set - so a
    client that asked the variable first could be pointed at a desktop of that
    program's own, whose answers would look exactly like the real ones.
    """

    def test_the_folder_is_asked_first_in_both(self):
        ipc_c = source(ROOT / "compositor" / "src" / "ipc.c")
        ipc_py = source(SHELL_DIR / "ipc.py")
        client = body_of(ipc_c, "int ipc_client_main(")
        self.assertIn("access(in_folder, F_OK) == 0", client,
                      "the C client stopped checking that the socket is really there")
        self.assertLess(client.index("access(in_folder"), client.index("env && env[0]"),
                        "the C client consults the variable before the folder")
        self.assertIn("if os.path.exists(derived):\n        return derived", ipc_py,
                      "the bar consults the variable before the folder")

    def test_the_variable_is_still_honoured(self):
        """A desktop that put its socket elsewhere has to be reachable: it is
        how the window manager tells its own programs where it is, and how a
        test aims at a desktop of its own."""
        client = body_of(source(ROOT / "compositor" / "src" / "ipc.c"), "int ipc_client_main(")
        self.assertIn('snprintf(path, sizeof(path), "%s", env)', client)
        self.assertIn('os.environ.get("TESSERA_SOCK")', source(SHELL_DIR / "ipc.py"))


class DragIcons(unittest.TestCase):
    """A drag icon's scene tree is wlroots' to destroy, and only its.

    wlr_scene_drag_icon_create() adds its own listener to the icon's destroy
    signal and takes the tree down there. A compositor that keeps the tree and
    destroys it again frees a node that is already gone - which is what ended
    the session whenever a file was dragged from one program to another, so the
    rule is written down here as well as in the source.
    """

    def test_nothing_here_destroys_the_drag_icons_tree(self):
        text = source(INPUT_C)
        start = text.index("static void start_drag(")
        body = text[start:text.index("\n}\n", start)]
        self.assertIn("wlr_scene_drag_icon_create(", body,
                      "the drag icon is no longer given a scene tree at all")
        self.assertNotIn("wlr_scene_node_destroy", body,
                         "the drag icon's tree is destroyed a second time")
        # The whole file, because the destroy used to live in its own function
        # beside this one rather than in the function that made the tree.
        for line in text.splitlines():
            if "wlr_scene_node_destroy" in line and "drag" in line.lower():
                self.fail("a drag icon's tree is destroyed by hand: " + line.strip())


class LockScreenLimits(unittest.TestCase):
    """The longest password the lock screen can send, said once."""

    def setUp(self):
        self.server_h = source(ROOT / "compositor" / "src" / "server.h")
        self.lock_c = source(ROOT / "compositor" / "src" / "lock.c")

    def test_the_password_field_and_pam_agree(self):
        cap = re.search(r"#define TS_PASSWORD_MAX (\d+)", self.server_h)
        self.assertIsNotNone(cap, "the lock screen stopped saying how long a password can be")
        # X/Open and Linux-PAM both take a response of 512 bytes counting the
        # terminating NUL, so more than that cannot be checked by anyone.
        self.assertLessEqual(int(cap.group(1)), 512, "PAM will not take a longer answer")
        self.assertIn("char password[TS_PASSWORD_MAX];", self.server_h,
                      "the field stopped using the limit it documents")
        self.assertIn("char password[TS_PASSWORD_MAX];", self.lock_c,
                      "the helper that asks PAM has its own idea of the size, which is how a "
                      "long password ends up silently shortened")

    def test_an_over_long_password_is_said_out_loud(self):
        """A field that stops listening mid-word fails with no explanation at
        all, so the notice is the fix rather than the cap."""
        appended = body_of(self.lock_c, "static void append_utf8(")
        self.assertIn("server->password_long = true;", appended,
                      "running out of room is quiet again")
        self.assertIn("if (server->password_len > 0 && !server->password_long)", self.lock_c,
                      "a shortened password is being sent off to be checked")
        self.assertIn("password_long", source(ROOT / "compositor" / "src" / "deco.c"),
                      "the screen never mentions it")

    def test_the_notice_can_be_gotten_out_of(self):
        """Clearing the field, and both ends of a lock cycle, put it right."""
        self.assertGreaterEqual(self.lock_c.count("password_long = false;"), 4,
                                "nobody could get rid of the notice once it appeared")


if __name__ == "__main__":
    unittest.main()

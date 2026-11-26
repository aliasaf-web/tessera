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


if __name__ == "__main__":
    unittest.main()

"""What Enter opens in the launcher, and which apps a search shows.

The bug this is here for: every key in the launcher window goes to the search
box, because that is how typing anywhere searches. Enter went there too, and the
box's own handler opened the *first* app in the list however far down the arrow
keys had moved the highlight - so the highlight was decoration.

The two rules live in `tessera_shell/apps.py`, which imports nothing, so this
file runs on a machine with no GTK bindings - which is where the mistake lived.
"""

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "shell"))

from tessera_shell.apps import app_under, matches  # noqa: E402


class Widget:
    """What the rule is allowed to ask a widget: what it stands for, and who its
    parent is. The real ones are a button inside a flow-box child."""

    def __init__(self, parent=None, app=None):
        self.parent = parent
        if app is not None:
            self.app = app

    def get_parent(self):
        return self.parent


class WhichAppEnterOpens(unittest.TestCase):
    """The app under the focus, not the first one in the list."""

    def test_the_app_on_the_widget_itself(self):
        self.assertEqual(app_under(Widget(app="files")), "files")

    def test_the_app_of_the_child_the_button_sits_in(self):
        """Focus goes to the button; the app is one step out. This is the shape
        the launcher makes, and the one a walk that only looked at the focused
        widget would get wrong."""
        child = Widget(app="editor")
        button = Widget(parent=child)
        self.assertEqual(app_under(button), "editor")

    def test_the_search_box_stands_for_no_app(self):
        """Typing and pressing Enter must fall back to the first match, so the
        search box - and everything above it - has to come back empty."""
        card = Widget()
        search = Widget(parent=card)
        self.assertIsNone(app_under(search))

    def test_nothing_focused(self):
        self.assertIsNone(app_under(None))

    def test_a_widget_whose_parent_chain_never_ends(self):
        """get_parent() returning None is the end; it must not loop or raise."""
        top = Widget()
        self.assertIsNone(app_under(Widget(parent=Widget(parent=top))))


class WhatASearchShows(unittest.TestCase):
    """Every word somewhere, in any order - what people type."""

    TEXT = "photo editor gimp image editing /usr/bin/gimp"

    def test_an_empty_search_shows_everything(self):
        self.assertTrue(matches(self.TEXT, ""))
        self.assertTrue(matches(self.TEXT, "   "))

    def test_one_word(self):
        self.assertTrue(matches(self.TEXT, "gimp"))
        self.assertFalse(matches(self.TEXT, "spreadsheet"))

    def test_two_words_in_any_order(self):
        self.assertTrue(matches(self.TEXT, "photo ed"))
        self.assertTrue(matches(self.TEXT, "ed photo"))

    def test_every_word_has_to_be_there(self):
        self.assertFalse(matches(self.TEXT, "photo spreadsheet"))

    def test_the_query_is_not_case_sensitive(self):
        """A capital letter in the box should not empty the list."""
        self.assertTrue(matches(self.TEXT.lower(), "GIMP"))


class TheLauncherUsesTheRule(unittest.TestCase):
    """The page and the rule are the same code, not two copies of it."""

    def setUp(self):
        self.text = (ROOT / "shell" / "tessera_shell" / "launcher.py").read_text(encoding="utf-8")

    def test_enter_asks_which_app_is_highlighted(self):
        self.assertIn("focused = self.window.get_focus()", self.text)
        self.assertIn("app_under(focused)", self.text)
        self.assertIn('self.search.connect("activate", lambda *_: self.launch_chosen())',
                      self.text)

    def test_the_first_app_is_no_longer_what_enter_opens(self):
        self.assertNotIn("def launch_first", self.text,
                         "Enter opens the first app again instead of the highlighted one")

    def test_the_search_filter_is_the_shared_rule(self):
        self.assertIn("matches(child.search_text, self.search.get_text())", self.text)


if __name__ == "__main__":
    unittest.main()

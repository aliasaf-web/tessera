"""The Displays page's two quiet rules, checked without a screen.

Both are about screen settings that are tried for fifteen seconds before they
are kept, and both were wrong in ways a screenshot cannot show:

- choosing the resolution already in use asked "keep these settings?" and wrote
  the resolution back without the refresh rate stored beside it, silently
  dropping the rate the screen had been running at;
- two changes made before the question was answered opened two questions, each
  remembering a different "before", so letting the older one run out undid a
  setting that had just been kept.

The rules live in `tessera_shell/displays.py`, which imports nothing, so this
file runs on a machine without GTK - which is where these mistakes survive.
"""

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "shell"))

from tessera_shell.displays import Revert, same_choice  # noqa: E402


class SameChoice(unittest.TestCase):
    """Whether picking something would change anything."""

    def test_the_resolution_in_use_with_its_rate_is_not_a_change(self):
        """The list offers resolutions; the file keeps the rate beside one.
        Choosing what is already running must not count as a change."""
        self.assertTrue(same_choice("mode", "1920x1080@60", "1920x1080"))
        self.assertTrue(same_choice("mode", "1920x1080", "1920x1080"))

    def test_a_different_resolution_is_a_change(self):
        self.assertFalse(same_choice("mode", "1920x1080@60", "2560x1440"))

    def test_auto_is_not_a_resolution(self):
        """`auto` is the recommended mode rather than a size, so picking a size
        over it is a change however the strings compare."""
        self.assertFalse(same_choice("mode", "auto", "1920x1080"))
        self.assertTrue(same_choice("mode", "auto", "auto"))

    def test_every_other_setting_compares_exactly(self):
        """Only a resolution has a rate that can be left out. A scale of 1.5 is
        not a scale of 1, and a screen that is on is not a screen that is off."""
        self.assertTrue(same_choice("scale", "1.5", "1.5"))
        self.assertFalse(same_choice("scale", "1.5", "1"))
        self.assertFalse(same_choice("enabled", "true", "false"))
        self.assertTrue(same_choice("position", "right", "right"))

    def test_a_rate_chosen_on_purpose_is_a_change(self):
        """Nothing offers a choice of rate today, but the rule must not make
        one impossible to write: picking 1920x1080@120 over @60 differs."""
        self.assertFalse(same_choice("mode", "1920x1080@60", "1920x1080@120"))


class TheBatchToUndo(unittest.TestCase):
    """What Undo puts back after more than one change."""

    def test_each_setting_goes_back_to_where_it_started(self):
        revert = Revert()
        revert.remember("display.HDMI-A-1.mode", "1920x1080@60")
        revert.remember("display.HDMI-A-1.scale", "auto")
        self.assertEqual(dict(revert.items()),
                         {"display.HDMI-A-1.mode": "1920x1080@60",
                          "display.HDMI-A-1.scale": "auto"})

    def test_a_setting_changed_twice_goes_back_to_the_first_value(self):
        """The bug two questions had: the second question remembered the value
        the first change had already written, so undoing it kept the change."""
        revert = Revert()
        revert.remember("display.HDMI-A-1.scale", "auto")
        revert.remember("display.HDMI-A-1.scale", "1.5")
        self.assertEqual(dict(revert.items()), {"display.HDMI-A-1.scale": "auto"})

    def test_a_first_value_of_nothing_still_counts(self):
        """An empty setting is a value to go back to, not an absent one."""
        revert = Revert()
        revert.remember("display.HDMI-A-1.position", "")
        self.assertTrue(revert)
        self.assertEqual(dict(revert.items()), {"display.HDMI-A-1.position": ""})

    def test_nothing_remembers_nothing(self):
        revert = Revert()
        self.assertFalse(revert)
        self.assertEqual(revert.items(), [])

    def test_items_is_a_copy(self):
        """The dialog walks the list while applying it; walking it while the
        batch is being undone must not depend on the same dict."""
        revert = Revert()
        revert.remember("display.HDMI-A-1.scale", "auto")
        revert.items().clear()
        self.assertEqual(dict(revert.items()), {"display.HDMI-A-1.scale": "auto"})


class WhatTheSettingsWindowUses(unittest.TestCase):
    """The page and the rules are the same code, not two copies of it."""

    def test_the_page_asks_the_rule(self):
        text = (ROOT / "shell" / "tessera_shell" / "settings.py").read_text(encoding="utf-8")
        self.assertIn("same_choice(field, stored, value)", text)
        self.assertIn("state[\"revert\"].remember(key, stored)", text)
        self.assertIn("for key, stored in state[\"revert\"].items():", text)
        # One question for the batch: a second dialog would remember a
        # different "before" and undo a setting that had been kept.
        self.assertIn("if state is None:", text)


if __name__ == "__main__":
    unittest.main()

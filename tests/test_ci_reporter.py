"""The reporter that puts a red run's reason on the check itself.

`tools/ci-annotate.py` is the last thing a failing job runs, so it is also the
one thing that has to work when everything else already has. A test that failed
and a build that stopped are different shapes of output, and the one thing
neither can afford is an annotation that says nothing but "exit code 1" - which
is what a build failure used to get, because the reporter only knew how to read
a unittest report. The Actions log is on a host a restricted network cannot
reach, so for some people that annotation is the whole of the failure.
"""

import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

# Loaded by path, because `tools` is not a package and the file has a dash in it.
_spec = importlib.util.spec_from_file_location("ci_annotate", ROOT / "tools" / "ci-annotate.py")
ci_annotate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ci_annotate)


def failed(name, reason):
    """One test's report, as unittest writes it."""
    return (f"======================================================================\n"
            f"FAIL: {name} (tests.test_x.SomeTests.{name})\n"
            f"----------------------------------------------------------------------\n"
            f"Traceback (most recent call last):\n"
            f"  File \"/__w/tessera/tessera/tests/test_x.py\", line 10, in {name}\n"
            f"    self.assertEqual(1, 2)\n"
            f"AssertionError: {reason}\n")


class TestReports(unittest.TestCase):
    """unittest's failures, one annotation each, with the reason in it."""

    def test_one_failure_is_one_line_saying_why(self):
        log = failed("test_the_thing", "the thing did not happen") + \
            "----------------------------------------------------------------------\n" \
            "Ran 1 test in 0.010s\n\nFAILED (failures=1)\n"
        out = ci_annotate.annotations(log)
        self.assertEqual(len(out), 2, out)  # the failure, and the summary
        self.assertIn("the thing did not happen", out[0])
        self.assertIn("test_the_thing", out[0])
        for line in out:
            self.assertTrue(line.startswith("::error "), f"not an error annotation: {line}")

    def test_the_extra_ones_say_where_they_are(self):
        log = "".join(failed(f"test_number_{number}", f"reason {number}") for number in range(9))
        out = "\n".join(ci_annotate.annotations(log))
        self.assertNotIn("reason 8", out, "everything was crammed onto the check")
        self.assertIn("further failure reports are in the log", out)

    def test_a_green_suite_is_not_blamed_for_a_red_run(self):
        # The reporter runs on `if: failure()`, so a green suite inside a red job
        # means another step is the one to look at - a linter, a shell check. An
        # error quoting `Ran 3 tests / OK` would send someone to the wrong file.
        log = "Ran 3 tests in 0.010s\n\nOK\n"
        out = "\n".join(ci_annotate.annotations(log))
        self.assertIn("the tests passed; the failure was in another step", out)
        self.assertNotIn("the run went red without a failure report", out)


class BuildReports(unittest.TestCase):
    """A run that went red before any test could speak."""

    def test_the_compilers_own_line_is_what_gets_shown(self):
        log = ("[4/40] Compiling 'xdg-shell-protocol.c' with a custom target\n"
               "compositor/tests/window_client.c:64:9: error: function 'say' might be a "
               "candidate for 'gnu_printf' format attribute [-Werror=suggest-attribute=format]\n"
               "cc1: all warnings being treated as errors\n"
               "ninja: build stopped: subcommand failed.\n")
        out = "\n".join(ci_annotate.annotations(log))
        self.assertIn("might be a candidate", out, out)
        self.assertIn("ninja: build stopped", out, out)
        self.assertIn("the step failed", out, "a build error was announced as a test failure")

    def test_mesons_setup_error_is_a_complaint_too(self):
        log = ("compositor/meson.build:19:10: ERROR: Dependency lookup for wlroots-0.18 "
               "with method 'pkg-config' failed\n")
        out = "\n".join(ci_annotate.annotations(log))
        self.assertIn("Dependency lookup", out)

    def test_the_same_line_from_two_build_modes_is_said_once(self):
        # `ninja -C build` and `ninja -C build-sanitizer` both write to the same
        # log folder, and one complaint is the amount anyone needs to read.
        line = "compositor/src/view.c:594:14: warning: unused variable 'black'\n"
        out = "\n".join(ci_annotate.annotations(line + "\ncompiling it again\n\n" + line))
        self.assertEqual(out.count("unused variable"), 1, out)

    def test_a_log_with_nothing_in_it_says_that(self):
        out = "\n".join(ci_annotate.annotations("   \n"))
        self.assertIn("nothing was written to a log", out)


class Escape(unittest.TestCase):
    """The characters the runner would otherwise read back as its own."""

    def test_percent_and_line_breaks_survive(self):
        text = ci_annotate.escape("100% done\nnext\n")
        self.assertNotIn("\n", text)
        self.assertIn("%25", text)
        self.assertIn("%0A", text)


if __name__ == "__main__":
    unittest.main()

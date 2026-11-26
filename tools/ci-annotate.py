#!/usr/bin/env python3
"""Repeats a red test run's reason on the check itself.

Actions keeps its logs on a storage host that not every reader can reach: a
sandbox, a restricted network, anyone working from `gh api` alone. Meanwhile
the useful part of a failed run is small - which test, and what the program
under test said - and the test suite here puts that in its failure messages on
purpose. So the workflow writes the test output to a file and this turns the
failure reports inside it into annotations on the check run, where the checks
tab and `gh api .../check-runs/ID/annotations` both show them without the log.

Usage: python3 tools/ci-annotate.py FILE...

It reads the files given (missing ones are skipped), prints one error line per
failed or errored test, and always exits successfully: a reporter that itself
turns a run red would be a poor reporter.
"""

import pathlib
import re
import sys

# Long enough for a traceback and the tail of the program's own output, short
# enough that six of them still fit on a screen.
MAX_BLOCK = 2600
# Enough for one class going wrong at a time, not enough to bury the check.
MAX_BLOCKS = 6
# The line that names the test is kept whole from the front; the reason, which
# unittest puts at the end of the block, is what is worth reading.
HEAD = 160

FAILURE_HEAD = re.compile(r"^(?:FAIL|ERROR): .*$", re.M)
RAN = re.compile(r"^Ran \d+ tests.*$", re.M)
SUMMARY = re.compile(r"^(?:FAILED|OK).*$", re.M)


def escape(text):
    """A workflow command lives on one line of the log, so the characters the
    runner reads back are written percent-encoded: percent first, then the line
    breaks."""
    return (text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A"))


def shorten(block):
    """Front and back of a failure report, with the middle dropped if it has
    to be: the name of the test is at one end and the reason at the other."""
    block = block.strip()
    if len(block) <= MAX_BLOCK:
        return block
    keep = MAX_BLOCK - HEAD - 20
    return f"{block[:HEAD]}\n... {len(block) - HEAD - keep} characters not shown ...\n" \
        f"{block[len(block) - keep:]}"


def reports(text):
    """Every failed or errored test, with the report under it, in the order
    they were written."""
    heads = [(match.start(), match.group(0)) for match in FAILURE_HEAD.finditer(text)]
    if not heads:
        return []
    after_last = RAN.search(text)
    after_last = after_last.start() if after_last else len(text)
    found = []
    for index, (start, title) in enumerate(heads):
        end = heads[index + 1][0] if index + 1 < len(heads) else after_last
        found.append((title, shorten(text[start:end].rstrip())))
    return found


def main(paths):
    text = ""
    for name in paths:
        path = pathlib.Path(name)
        if path.is_file():
            text += path.read_text(encoding="utf-8", errors="replace") + "\n"
    found = reports(text)
    summary = SUMMARY.findall(text)
    if not found:
        # Nothing that looks like a test failure: whatever made the step red,
        # the last lines are where a build or a crash would say so.
        tail = "\n".join(text.strip().splitlines()[-30:]) or "the tests wrote nothing"
        print("::error::the tests went red without a failure report. "
              f"Last of what they wrote: {escape(shorten(tail))}")
        return 0
    for title, block in found[:MAX_BLOCKS]:
        print(f"::error title={escape(title[:180])}::{escape(block)}")
    if len(found) > MAX_BLOCKS:
        print(f"::warning title=more failures::{len(found) - MAX_BLOCKS} further failure "
              "reports are in the log, not here")
    if summary:
        print(f"::error title=unittest summary::{escape('; '.join(summary[-3:]))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

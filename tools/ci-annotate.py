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
turns a run red would be a poor reporter. A log with no test failures in it at
all is a build that stopped or a program that crashed, so the lines the compiler
or meson complained in are printed instead - those are the whole of what a red
build says, and the tail of the log around them is kept for context.
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
SUMMARY_OK = re.compile(r"^OK\s*$", re.M)
# What a build says when it refuses, and the only part of a build log worth
# putting in front of someone: gcc and clang put a file and line in front of the
# message, meson writes `ERROR:` from its setup step, and ninja only ever says a
# subcommand failed - which is why the last lines come along with them.
BUILD_LINE = re.compile(r":\d+:\d+: (?:error|warning): |\bERROR: |^ninja: build stopped")


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


def build_reports(text):
    """The lines a build or a tool complained in, oldest first, without the
    repeats that a rebuild of the same file writes once per mode."""
    seen, kept = set(), []
    for line in text.splitlines():
        if not BUILD_LINE.search(line):
            continue
        if line in seen:
            continue
        seen.add(line)
        kept.append(line)
    return kept[:MAX_BLOCKS]


def annotations(text):
    """The `::error::` and `::warning::` lines for a run's log output, in the
    order they should be written."""
    found = reports(text)
    if found:
        out = [f"::error title={escape(title[:180])}::{escape(block)}"
               for title, block in found[:MAX_BLOCKS]]
        if len(found) > MAX_BLOCKS:
            out.append(f"::warning title=more failures::{len(found) - MAX_BLOCKS} further "
                       "failure reports are in the log, not here")
        summary = SUMMARY.findall(text)
        if summary:
            out.append(f"::error title=unittest summary::{escape('; '.join(summary[-3:]))}")
        return out

    # Nothing that looks like a test failure. Whatever made the step red, its own
    # complaint is worth more than the tail of a log - and for a build, the tail
    # is mostly ninja naming the files it got through.
    tail = text.strip().splitlines()[-30:]
    complaints = build_reports("\n".join(tail) if tail else text)
    if complaints:
        kept = [line for line in tail[-8:] if line not in complaints]
        lines = complaints + (["", "last lines:"] + kept if kept else [])
        body = "\n".join(lines)
        title = "the step failed; what it complained about"
    elif SUMMARY_OK.search(text):
        # The suite was green and something else went red - a linter, a shell
        # check. Saying so is worth more than an error quoting `OK`.
        body = "\n".join(tail) or "nothing was written to a log"
        title = "the tests passed; the failure was in another step"
    else:
        body = "\n".join(tail) or "nothing was written to a log"
        title = "the run went red without a failure report"
    return [f"::error title={escape(title)}::{escape(shorten(body))}"]

    out = [f"::error title={escape(title[:180])}::{escape(block)}"
           for title, block in found[:MAX_BLOCKS]]
    if len(found) > MAX_BLOCKS:
        out.append(f"::warning title=more failures::{len(found) - MAX_BLOCKS} further failure "
                   "reports are in the log, not here")
    summary = SUMMARY.findall(text)
    if summary:
        out.append(f"::error title=unittest summary::{escape('; '.join(summary[-3:]))}")
    return out


def main(paths):
    text = ""
    for name in paths:
        path = pathlib.Path(name)
        if path.is_file():
            text += path.read_text(encoding="utf-8", errors="replace") + "\n"
    for line in annotations(text):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

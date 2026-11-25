#!/usr/bin/env python3
"""Rewrites the shortcut tables in README.md from shell/tessera_shell/shortcuts.py,
so the README and the help screen (Super + /) always list the same things.

    python3 tools/update-readme.py           rewrite README.md
    python3 tools/update-readme.py --check   only say whether it is up to date
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "shell"))
from tessera_shell.shortcuts import SECTIONS  # noqa: E402

START = "<!-- Shortcuts: made from shell/tessera_shell/shortcuts.py by tools/update-readme.py -->"
END = "<!-- End of shortcuts -->"


def keys_markdown(keys):
    return " or ".join(f"**{way}**" for way in keys.split(" / "))


def tables():
    out = [START]
    for title, items in SECTIONS:
        out += ["", f"**{title}**", "", "| Keys | What it does |", "|---|---|"]
        out += [f"| {keys_markdown(keys)} | {what} |" for keys, what in items]
    out += ["", END]
    return "\n".join(out)


def updated(readme):
    start, end = readme.index(START), readme.index(END) + len(END)
    return readme[:start] + tables() + readme[end:]


def main():
    path = ROOT / "README.md"
    readme = path.read_text(encoding="utf-8")
    new = updated(readme)
    if "--check" in sys.argv[1:]:
        if new != readme:
            print("README.md's shortcuts are out of date: run python3 tools/update-readme.py")
            return 1
        return 0
    path.write_text(new, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())

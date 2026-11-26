"""Tessera shell: the bar, app launcher, quick settings and notifications."""

from pathlib import Path


def _shared():
    """Where the installed shell keeps the files the installer writes beside
    it: share/tessera. In a checkout, the source folder's own top end - the
    same distance up from this file, so both work the same way."""
    return Path(__file__).resolve().parents[2]


def _read_version():
    # The one VERSION file: at the top of the source folder, and installed
    # next to the shell (share/tessera/VERSION).
    try:
        return (_shared() / "VERSION").read_text().strip()
    except OSError:
        return "unknown"


def read_provenance():
    """What the installer recorded about the build it installed, as
    {"commit": "1a2b3c4", "branch": "main", "built": "2026-11-26", "changed":
    "yes"}. Empty in a checkout, where there is no install to report on, and
    for anyone who installed before this was written."""
    found = {}
    try:
        lines = (_shared() / "provenance").read_text(encoding="utf-8").splitlines()
    except OSError:
        return found
    for line in lines:
        if "=" in line:
            key, _, value = line.partition("=")
            found[key.strip()] = value.strip()
    return found


__version__ = _read_version()

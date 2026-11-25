"""Tessera shell: the bar, app launcher, quick settings and notifications."""

from pathlib import Path


def _read_version():
    # The one VERSION file: at the top of the source folder, and installed
    # next to the shell (share/tessera/VERSION).
    try:
        return (Path(__file__).resolve().parents[2] / "VERSION").read_text().strip()
    except OSError:
        return "unknown"


__version__ = _read_version()

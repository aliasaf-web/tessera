"""The time and date, said the way the rest of the desktop says them.

The bar's clock and the quick-settings panel both show these, and both want the
same two things: weekday and month names in the language the session is in, and
the day of the month without its leading zero. That second one is written
`%-d` by glibc and printed out as typed by other C libraries, and putting the
pieces together here is what keeps the two widgets from each discovering that.
"""

from gi.repository import GLib


def _format(moment, pattern):
    """GLib's formatting, which follows LC_TIME; empty rather than an exception
    if there is no time to ask, or nothing to show."""
    if moment is None:
        return ""
    return moment.format(pattern) or ""


def now():
    """This moment, in the desktop's local time, or None if the machine cannot
    say what that is."""
    return GLib.DateTime.new_now_local()


def clock_text(at=None):
    """`18:04` - what the bar shows."""
    return _format(now() if at is None else at, "%H:%M")


def date_text(with_year=False, at=None):
    """`Saturday 7 November`, or `Saturday 7 November 2026` when there is room
    for the year: the bar's tooltip, and the panel's heading."""
    moment = now() if at is None else at
    if moment is None:
        return ""
    day = _format(moment, "%d").lstrip("0") or "0"
    parts = [_format(moment, "%A"), day, _format(moment, "%B")]
    if with_year:
        parts.append(_format(moment, "%Y"))
    return " ".join(part for part in parts if part)

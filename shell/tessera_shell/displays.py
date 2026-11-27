"""The two rules behind Settings › Displays that need no screen to state.

Both were wrong in a way no screenshot shows: whether a choice changes
anything at all, and what to go back to when a screen change is not kept.
They live here, apart from the widgets, so they can be checked on a machine
with no GTK bindings at all.
"""


def same_choice(field, stored, value):
    """True when picking `value` for `field` would change nothing.

    A resolution can be stored with its refresh rate beside it
    (display.HDMI-A-1.mode=1920x1080@60, as data/config documents), while the
    list of resolutions offers only WIDTHxHEIGHT. A choice without a rate is
    therefore about the resolution alone: choosing the one already in use is
    not a change, and the rate written beside it has to be kept instead of
    being replaced by the shorter form. A choice that does carry a rate is a
    statement about the rate, so it is compared in full.
    """
    if field == "mode":
        if "@" in value:
            return stored == value
        return stored.split("@")[0] == value
    return stored == value


class Revert:
    """What a batch of screen changes should be put back to.

    A change is tried for fifteen seconds before it is kept, and more than one
    can be made while that question is up. Each key goes back to the value it
    had when the question opened, which is not the value it had when the last
    field was touched: otherwise changing the resolution and then the scale
    would put the resolution back to the one just chosen.
    """

    def __init__(self):
        self._before = {}

    def remember(self, key, stored):
        """Records what `key` held; the first value seen for it is the one
        kept, so a key changed twice in a row still goes back to the start."""
        self._before.setdefault(key, stored)

    def __bool__(self):
        return bool(self._before)

    def items(self):
        """The keys changed and the value each had, in the order first seen."""
        return list(self._before.items())

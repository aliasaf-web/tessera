"""The two rules of the app list that need no screen to state.

Both are about what a person sees being what happens:

- **Which apps a search shows**: every word of the query somewhere in the app's
  names, description, executable or keywords. People type "photo ed", not a
  prefix, so matching whole words anywhere is the rule that finds what they
  meant.
- **Which app Enter opens**: the one under the highlight. The launcher had the
  first of those already and the second wrong - every key in the window goes to
  the search box (that is how typing anywhere searches), so Enter arrived
  *there* and opened the first app however far down the arrows had gone.

They live here, apart from the widgets, so they can be checked on a machine with
no GTK bindings at all - which is where a rule like this survives being wrong.
"""


def matches(search_text, query):
    """True when `search_text` (already lower case) contains every word of the
    query, in any order."""
    words = query.strip().lower().split()
    return all(word in search_text for word in words)


def app_under(widget):
    """The app a widget stands for, looking outwards through the wrappers.

    The focus sits on the button inside a flow-box child, and the app is on the
    child, so this walks up until something carries an `app`. Anything that is
    not part of the list - the search box, the card, the window - has none all
    the way to the top, which is how "no app is highlighted" is spelled.
    """
    while widget is not None:
        app = getattr(widget, "app", None)
        if app is not None:
            return app
        widget = widget.get_parent()
    return None

"""A friendly overview of how the desktop works, shown on first login and
whenever Super+/ is pressed."""

import os

from gi.repository import Gtk

from .layer import (button, click_outside_closes, fullscreen_overlay, hbox, label,
                    on_escape, set_monitor, vbox)

SECTIONS = [
    ("Getting around", [
        ("Super", "Open the app launcher (just tap it)"),
        ("Super  +  Enter", "Open a terminal"),
        ("Super  +  E", "Open your files"),
        ("Super  +  B", "Open the web browser"),
        ("Super  +  Q", "Close the window"),
        ("Super  +  L", "Lock the screen"),
    ]),
    ("Windows", [
        ("Alt  +  Tab", "Switch windows: hold Alt, tap Tab to choose, let go"),
        ("Super  +  ← → ↑ ↓", "Move between windows"),
        ("Super  +  Shift  +  arrows", "Swap windows around"),
        ("Super  +  F", "Full screen on/off"),
        ("Super  +  M", "Make a window big on/off"),
        ("Super  +  Space", "Let a window float freely on/off"),
    ]),
    ("Workspaces", [
        ("3 fingers  ← →", "Swipe on the touchpad to change workspace"),
        ("3 fingers  ↑ ↓", "Swipe up to show your apps, down to hide them"),
        ("Super  +  Ctrl  +  ← →", "Previous or next workspace"),
        ("Super  +  1 … 9", "Go to workspace 1 to 9"),
        ("Super  +  Shift  +  1 … 9", "Move the window to that workspace"),
        ("Print Screen", "Take a screenshot"),
        ("Super  +  /", "Show this help again"),
    ]),
]

TIPS = ("New windows open in the big space on the left; the others line up on the right. "
        "Drag a window's title bar onto another window to swap them, and drag the gap "
        "between the columns to make one side wider.")


def state_dir():
    base = os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state")
    return os.path.join(base, "tessera")


class Help:
    def __init__(self, shell):
        self.shell = shell
        self.window = fullscreen_overlay(shell.app, "tessera-help")
        backdrop = Gtk.Box()
        backdrop.add_css_class("backdrop")

        card = vbox(18)
        card.add_css_class("card")
        card.set_size_request(-1, -1)
        card.set_halign(Gtk.Align.CENTER)
        card.set_valign(Gtk.Align.CENTER)
        card.set_hexpand(True)
        card.set_vexpand(True)

        card.append(label("Welcome to Tessera", "title"))
        tips = label(TIPS, "dim", wrap=True)
        tips.set_max_width_chars(100)
        card.append(tips)

        # Two columns so everything fits on small screens.
        columns = hbox(36)
        for group in (SECTIONS[:2], SECTIONS[2:]):
            grid = Gtk.Grid()
            grid.set_column_spacing(16)
            grid.set_row_spacing(7)
            grid.set_valign(Gtk.Align.START)
            row = 0
            for title, items in group:
                heading = label(title, "heading")
                heading.set_margin_top(8 if row else 0)
                grid.attach(heading, 0, row, 2, 1)
                row += 1
                for keys, what in items:
                    cap = label(keys, "keycap")
                    cap.set_halign(Gtk.Align.START)
                    cap.set_valign(Gtk.Align.CENTER)
                    grid.attach(cap, 0, row, 1, 1)
                    text = label(what, wrap=True)
                    text.set_max_width_chars(30)
                    grid.attach(text, 1, row, 1, 1)
                    row += 1
            columns.append(grid)
        scroller = Gtk.ScrolledWindow()
        scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scroller.set_propagate_natural_height(True)
        scroller.set_max_content_height(560)
        scroller.set_child(columns)
        card.append(scroller)

        ok = button(text="Got it", css="flat danger", on_click=self.hide)
        ok.set_halign(Gtk.Align.END)
        card.append(ok)

        backdrop.append(card)
        self.window.set_child(backdrop)
        click_outside_closes(backdrop, card, self.hide)
        on_escape(self.window, self.hide)
        self.ok = ok

    def show(self, monitor=None):
        set_monitor(self.window, monitor)
        self.window.present()
        self.ok.grab_focus()

    def hide(self):
        self.window.set_visible(False)
        os.makedirs(state_dir(), exist_ok=True)
        open(os.path.join(state_dir(), "welcomed"), "w").close()

    def first_run(self):
        return not os.path.exists(os.path.join(state_dir(), "welcomed"))

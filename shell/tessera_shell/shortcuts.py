"""Every keyboard shortcut and gesture, in one place. The help screen
(Super + /) shows this list, and the README's table is made from it with
tools/update-readme.py."""

# (section, [(keys, what it does)]). In keys, " + " joins keys pressed
# together, and " / " separates ways of doing the same thing.
SECTIONS = [
    ("Getting around", [
        ("Super / Super + A", "Open the app launcher (tap Super on its own)"),
        ("Super + Enter", "Open a terminal"),
        ("Super + E", "Open your files"),
        ("Super + B", "Open the web browser"),
        ("Super + L", "Lock the screen"),
        ("Super + Esc / Super + Shift + E", "Sleep, restart or shut down"),
    ]),
    ("Windows", [
        ("Alt + Tab / Super + Tab", "Switch windows: hold Alt or Super, tap Tab to choose, let go "
         "(add Shift to go back)"),
        ("Super + Q / Super + W / Alt + F4", "Close the window"),
        ("Super + ← → ↑ ↓", "Move between windows"),
        ("Super + Shift + ← → ↑ ↓", "Swap windows around"),
        ("Super + F", "Full screen on/off"),
        ("Super + M / double-click the title bar", "Make a window big on/off"),
        ("Super + Space", "Let a window float freely on/off"),
        ("Super + left mouse drag", "Move a floating window, or swap a window with another"),
        ("Super + right mouse drag", "Resize a floating window"),
    ]),
    ("Workspaces", [
        ("3 fingers ← →", "Swipe on the touchpad to change workspace"),
        ("3 fingers ↑ ↓", "Swipe up to show your apps, down to hide them"),
        ("Super + Ctrl + ← →", "Previous or next workspace"),
        ("Super + 1 … 9", "Go to workspace 1 to 9"),
        ("Super + Shift + 1 … 9", "Move the window to that workspace"),
    ]),
    ("More", [
        ("Print Screen", "Take a screenshot of the whole screen"),
        ("Shift + Print Screen / Super + Shift + S", "Take a screenshot of an area"),
        ("Super + Shift + R", "Apply changes made by hand to the settings file"),
        ("Super + /", "Show this help again"),
    ]),
]

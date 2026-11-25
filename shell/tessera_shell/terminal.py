"""Running a command in the user's terminal."""

import os
import shlex
import shutil

# What goes between a terminal and the command it should run. Terminals not
# listed take -e, as Debian asks of every x-terminal-emulator.
COMMAND_FLAGS = {
    "foot": [],
    "kitty": [],
    "wezterm": ["start", "--"],
    "gnome-terminal": ["--"],
    "kgx": ["--"],
    "ptyxis": ["--"],
    "xfce4-terminal": ["-x"],
    "mate-terminal": ["-x"],
    "terminator": ["-x"],
}


def terminal_command(terminal, command):
    """The program and arguments that open `terminal` (the Terminal setting,
    which may include its own options) running `command` (a list). Falls
    back to xdg-terminal-exec, then x-terminal-emulator."""
    try:
        words = shlex.split(terminal or "")
    except ValueError:
        words = []
    if words and shutil.which(words[0]):
        name = os.path.basename(words[0])
        return words + COMMAND_FLAGS.get(name, ["-e"]) + list(command)
    if shutil.which("xdg-terminal-exec"):
        return ["xdg-terminal-exec"] + list(command)
    return ["x-terminal-emulator", "-e"] + list(command)

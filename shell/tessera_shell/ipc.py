"""Connection to the Tessera compositor's control socket."""

import json
import os
import socket

from gi.repository import Gio, GLib


def socket_path():
    """The control socket, in the user's private runtime folder (None if
    there is no such folder: there is no safe place for it then)."""
    path = os.environ.get("TESSERA_SOCK")
    if path:
        return path
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime:
        return None
    display = os.environ.get("WAYLAND_DISPLAY", "wayland-0")
    return os.path.join(runtime, f"tessera-{display}.sock")


def send(*words):
    """Send one command and return the reply (or None if it failed)."""
    line = " ".join(str(w) for w in words) + "\n"
    path = socket_path()
    if path is None:
        return None
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(2)
            s.connect(path)
            s.sendall(line.encode())
            reply = s.makefile().readline()
            return json.loads(reply) if reply else None
    except (OSError, ValueError):
        return None


class EventStream:
    """Subscribes to compositor events and calls `handler(event_dict)`.

    Reconnects automatically if the compositor restarts its socket."""

    def __init__(self, handler):
        self.handler = handler
        self.stream = None
        self.conn = None
        self.cancellable = None
        self._connect()

    def _connect(self):
        path = socket_path()
        if path is None:
            return
        client = Gio.SocketClient()
        address = Gio.UnixSocketAddress.new(path)
        try:
            self.conn = client.connect(address, None)
        except GLib.Error:
            GLib.timeout_add_seconds(1, self._retry)
            return
        out = self.conn.get_output_stream()
        out.write_all(b"subscribe\n", None)
        self.stream = Gio.DataInputStream.new(self.conn.get_input_stream())
        self.cancellable = Gio.Cancellable()
        self._read()

    def _retry(self):
        self._connect()
        return False

    def _read(self):
        self.stream.read_line_async(GLib.PRIORITY_DEFAULT, self.cancellable, self._on_line)

    def _on_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            line = None
        if line is None:
            self.conn = None
            GLib.timeout_add_seconds(1, self._retry)
            return
        line = line.strip()
        if line:
            try:
                event = json.loads(line)
            except ValueError:
                event = None
            if isinstance(event, dict):
                try:
                    self.handler(event)
                except Exception as exc:  # keep listening even if a handler fails
                    print(f"tessera-shell: event handler error: {exc}")
        self._read()

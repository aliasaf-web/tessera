"""Quick settings: Wi-Fi, Bluetooth, dark mode, sound, brightness, power."""

from gi.repository import GLib, Gtk

from . import clock, ipc, services
from .theme import write_config_value
from .launcher import ConfirmButton
from .layer import (button, clear, click_outside_closes, fullscreen_overlay, hbox,
                    icon, label, on_escape, set_monitor, vbox)


class Tile:
    """A rounded toggle with an optional arrow that opens more options."""

    def __init__(self, icon_name, title, on_toggle, on_more=None):
        self.box = hbox(0)
        self.box.add_css_class("tile")
        self.box.set_hexpand(True)
        self.icon = icon(icon_name, 18)
        self.title = label(title, "tile-title")
        self.sub = label("", "tile-sub", ellipsize=True)
        self.sub.set_max_width_chars(14)
        text = vbox(2, self.title, self.sub)
        text.set_valign(Gtk.Align.CENTER)
        main = button(hbox(12, self.icon, text), on_click=on_toggle)
        main.set_hexpand(True)
        self.box.append(main)
        if on_more:
            more = button(icon("go-next-symbolic"), tooltip="More options", on_click=on_more)
            self.box.append(more)

    def set(self, on, subtitle, icon_name=None):
        if on:
            self.box.add_css_class("on")
        else:
            self.box.remove_css_class("on")
        self.sub.set_label(subtitle)
        if icon_name:
            self.icon.set_from_icon_name(icon_name)


class SliderRow:
    def __init__(self, icon_name, on_change, on_icon=None, tooltip=None):
        self.updating = False
        self.on_change = on_change
        self.icon = icon(icon_name, 18)
        if on_icon:
            head = button(self.icon, css="icon", tooltip=tooltip, on_click=on_icon)
        else:
            head = self.icon
            head.set_margin_start(11)
            head.set_margin_end(11)
        self.scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 1, 0.01)
        self.scale.add_css_class("level")
        self.scale.set_draw_value(False)
        self.scale.set_hexpand(True)
        self.scale.connect("value-changed", self._changed)
        self.widget = hbox(10, head, self.scale)
        self.widget.add_css_class("slider-row")

    def _changed(self, scale):
        if not self.updating:
            self.on_change(scale.get_value())

    def set(self, value, icon_name=None):
        self.updating = True
        self.scale.set_value(value)
        self.updating = False
        if icon_name:
            self.icon.set_from_icon_name(icon_name)


class QuickSettings:
    WIDTH = 380

    def __init__(self, shell):
        self.shell = shell
        self.monitor = None
        self.window = fullscreen_overlay(shell.app, "tessera-quick-settings")
        self.window.connect("close-request", lambda *_: (self.hide(), True)[1])

        backdrop = Gtk.Box()
        backdrop.add_css_class("backdrop")
        backdrop.add_css_class("clear")

        self.card = vbox(0)
        self.card.add_css_class("card")
        self.card.set_size_request(self.WIDTH, -1)
        self.card.set_halign(Gtk.Align.END)
        self.card.set_valign(Gtk.Align.END)
        self.card.set_hexpand(True)
        self.card.set_vexpand(True)
        self.place()

        self.stack = Gtk.Stack()
        self.stack.set_transition_type(Gtk.StackTransitionType.SLIDE_LEFT_RIGHT)
        self.stack.set_transition_duration(160)
        self.stack.set_vhomogeneous(False)
        self.stack.set_interpolate_size(True)
        self.stack.add_named(self._build_main(), "main")
        self.stack.add_named(self._build_wifi(), "wifi")
        self.stack.add_named(self._build_bluetooth(), "bluetooth")
        self.stack.add_named(self._build_power(), "power")
        self.card.append(self.stack)

        backdrop.append(self.card)
        self.window.set_child(backdrop)
        click_outside_closes(backdrop, self.card, self.hide)
        on_escape(self.window, self.back_or_hide)

        shell.network.connect(self.update_network)
        shell.bluetooth.connect(self.update_bluetooth)
        shell.audio.connect(self.update_audio)
        shell.brightness.connect(self.update_brightness)
        shell.battery.connect(self.update_battery)
        shell.notifications.connect_history(self.update_history)
        self.update_theme()

    # ------------------------------------------------------------------ pages

    def _header(self, title, switch_cb=None):
        back = button(icon("go-previous-symbolic"), css="icon", tooltip="Back",
                      on_click=lambda: self.open_page("main"))
        title_lbl = label(title, "title")
        title_lbl.set_hexpand(True)
        row = hbox(12, back, title_lbl)
        switch = None
        if switch_cb:
            switch = Gtk.Switch()
            switch.set_valign(Gtk.Align.CENTER)
            switch.connect("notify::active", lambda s, _p: switch_cb(s.get_active())
                           if not getattr(s, "updating", False) else None)
            row.append(switch)
        row.set_margin_bottom(14)
        return row, switch

    def _build_main(self):
        page = vbox(14)

        self.battery_label = label("", "dim")
        self.battery_icon = icon("battery-good-symbolic")
        self.battery_row = hbox(8, self.battery_icon, self.battery_label)
        self.battery_row.set_hexpand(True)
        self.battery_row.set_valign(Gtk.Align.CENTER)
        self.date_label = label("", "dim")
        self.date_label.set_hexpand(True)
        head = hbox(8, self.battery_row, self.date_label)
        head.append(button(icon("emblem-system-symbolic"), css="icon", tooltip="Settings",
                           on_click=self.open_settings))
        head.append(button(icon("system-lock-screen-symbolic"), css="icon", tooltip="Lock",
                           on_click=self.lock))
        head.append(button(icon("system-shutdown-symbolic"), css="icon", tooltip="Power",
                           on_click=lambda: self.open_page("power")))
        page.append(head)

        grid = Gtk.Grid()
        grid.set_row_spacing(10)
        grid.set_column_spacing(10)
        grid.set_column_homogeneous(True)
        self.wifi_tile = Tile("network-wireless-symbolic", "Wi-Fi", self.toggle_wifi,
                              lambda: self.open_page("wifi"))
        self.bt_tile = Tile("bluetooth-active-symbolic", "Bluetooth", self.toggle_bluetooth,
                            lambda: self.open_page("bluetooth"))
        self.dark_tile = Tile("weather-clear-night-symbolic", "Dark mode", self.toggle_dark)
        self.night_tile = Tile("display-brightness-symbolic", "Night light", self.toggle_night)
        self.dnd_tile = Tile("notifications-disabled-symbolic", "Do not disturb", self.toggle_dnd)
        self.shot_tile = Tile("camera-photo-symbolic", "Screenshot", self.take_screenshot)
        self.shot_tile.set(False, "Whole screen")
        grid.attach(self.wifi_tile.box, 0, 0, 1, 1)
        grid.attach(self.bt_tile.box, 1, 0, 1, 1)
        grid.attach(self.dark_tile.box, 0, 1, 1, 1)
        grid.attach(self.night_tile.box, 1, 1, 1, 1)
        grid.attach(self.dnd_tile.box, 0, 2, 1, 1)
        grid.attach(self.shot_tile.box, 1, 2, 1, 1)
        page.append(grid)

        self.volume = SliderRow("audio-volume-high-symbolic",
                                lambda v: self.shell.audio.set_volume(v),
                                on_icon=lambda: self.shell.audio.toggle_mute(),
                                tooltip="Mute")
        self.brightness = SliderRow("display-brightness-symbolic",
                                    lambda v: self.shell.brightness.set_level(v))
        page.append(self.volume.widget)
        page.append(self.brightness.widget)

        self.history_header = hbox(8)
        heading = label("Notifications", "heading")
        heading.set_hexpand(True)
        self.history_header.append(heading)
        self.history_header.append(button(text="Clear all", css="flat",
                                          on_click=self.shell.notifications.clear_history))
        self.history_list = vbox(8)
        page.append(self.history_header)
        page.append(self.history_list)
        return page

    def _build_wifi(self):
        page = vbox(8)
        header, self.wifi_switch = self._header("Wi-Fi", self.shell.network.set_enabled)
        page.append(header)
        self.wifi_status = label("", "dim", wrap=True)
        page.append(self.wifi_status)
        self.wifi_list = Gtk.ListBox()
        self.wifi_list.add_css_class("networks")
        self.wifi_list.set_selection_mode(Gtk.SelectionMode.NONE)
        scroller = Gtk.ScrolledWindow()
        scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scroller.set_max_content_height(360)
        scroller.set_propagate_natural_height(True)
        scroller.set_child(self.wifi_list)
        page.append(scroller)
        page.append(button(text="Search again", css="flat",
                           on_click=self.shell.network.rescan))
        self.password_for = None
        return page

    def _build_bluetooth(self):
        page = vbox(8)
        header, self.bt_switch = self._header("Bluetooth", self.shell.bluetooth.set_powered)
        page.append(header)
        self.bt_status = label("", "dim", wrap=True)
        page.append(self.bt_status)
        # Shown while pairing: the code to check or type.
        self.bt_prompt = vbox(10)
        self.bt_prompt.add_css_class("prompt")
        self.bt_prompt.set_visible(False)
        page.append(self.bt_prompt)
        self._shown_prompt = None
        self.bt_list = Gtk.ListBox()
        self.bt_list.add_css_class("plain")
        self.bt_list.set_selection_mode(Gtk.SelectionMode.NONE)
        scroller = Gtk.ScrolledWindow()
        scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scroller.set_max_content_height(360)
        scroller.set_propagate_natural_height(True)
        scroller.set_child(self.bt_list)
        page.append(scroller)
        self.bt_search = button(text="Look for new devices", css="flat",
                                on_click=self.toggle_bt_search)
        page.append(self.bt_search)
        return page

    def _build_power(self):
        page = vbox(8)
        header, _ = self._header("Power")
        page.append(header)
        page.append(button(hbox(10, icon("system-lock-screen-symbolic"), label("Lock")),
                           css="flat", on_click=self.lock))
        page.append(button(hbox(10, icon("weather-clear-night-symbolic"), label("Sleep")),
                           css="flat", on_click=self.sleep))
        for icon_name, text, confirm, action in (
                ("system-log-out-symbolic", "Log out", "Click again to log out",
                 lambda: ipc.send("logout")),
                ("system-reboot-symbolic", "Restart", "Click again to restart",
                 services.reboot),
                ("system-shutdown-symbolic", "Shut down", "Click again to shut down",
                 services.power_off)):
            page.append(ConfirmButton(icon_name, text, confirm, action).widget)
        return page

    # ---------------------------------------------------------------- actions

    def open_page(self, name):
        self.stack.set_visible_child_name(name)
        self.shell.network.set_list_wanted(name == "wifi")
        if name == "wifi":
            self.shell.network.rescan()
        if name == "bluetooth" and self.shell.bluetooth.powered:
            self.update_bluetooth()

    def toggle_wifi(self):
        net = self.shell.network
        if net.has_wifi:
            net.set_enabled(not net.wifi_enabled)

    def toggle_bluetooth(self):
        bt = self.shell.bluetooth
        if bt.available:
            bt.set_powered(not bt.powered)

    def toggle_bt_search(self):
        bt = self.shell.bluetooth
        bt.set_discovering(not bt.discovering)

    def toggle_dark(self):
        # The reload that follows recolours everything, this panel included.
        write_config_value("theme", "light" if self.shell.theme.dark else "dark")
        ipc.send("reload")

    def toggle_night(self):
        ipc.send("night-light", "toggle")

    def take_screenshot(self):
        self.hide()
        self.shell.screenshot()

    def open_settings(self):
        self.hide()
        self.shell.open_settings()

    def toggle_dnd(self):
        n = self.shell.notifications
        n.dnd = not n.dnd
        self.update_theme()

    def lock(self):
        self.hide()
        ipc.send("lock")

    def sleep(self):
        self.hide()
        services.suspend()

    # ---------------------------------------------------------------- updates

    def update_theme(self):
        self.dark_tile.set(self.shell.theme.dark, "On" if self.shell.theme.dark else "Off")
        dnd = self.shell.notifications.dnd
        self.dnd_tile.set(dnd, "On" if dnd else "Off")
        night = bool(self.shell.state.get("night_light"))
        self.night_tile.set(night, "On" if night else "Off")

    def place(self):
        """Sit just above the bar, which is smaller on small screens."""
        size = self.shell.metrics(None)
        self.card.set_margin_end(size["gaps"])
        self.card.set_margin_bottom(size["margin"] + size["bar"] + 10)

    def update_network(self):
        net = self.shell.network
        self.wifi_tile.set(net.wifi_enabled and net.has_wifi, net.describe(), net.icon())
        if self.wifi_switch is not None:
            self.wifi_switch.updating = True
            self.wifi_switch.set_active(net.wifi_enabled)
            self.wifi_switch.set_sensitive(net.has_wifi)
            self.wifi_switch.updating = False
        if not net.has_wifi:
            self.wifi_status.set_label("This computer has no Wi-Fi.")
        elif not net.wifi_enabled:
            self.wifi_status.set_label("Wi-Fi is off. Turn it on with the switch above.")
        elif net.busy:
            self.wifi_status.set_label("Connecting…")
        elif net.connected_ssid:
            self.wifi_status.set_label(f"Connected to {net.connected_ssid}.")
        else:
            self.wifi_status.set_label("Choose a network to connect.")
        if self.password_for is None:
            self._fill_wifi_list()

    def _fill_wifi_list(self):
        net = self.shell.network
        clear(self.wifi_list)
        if not (net.has_wifi and net.wifi_enabled):
            return
        for n in net.networks[:30]:
            row = hbox(10, icon(services.signal_icon(n["signal"])))
            name = label(n["ssid"], ellipsize=True)
            name.set_hexpand(True)
            row.append(name)
            if n["active"]:
                row.append(label("Connected", "badge"))
            if n["secure"]:
                row.append(icon("system-lock-screen-symbolic", 14))
            lbr = Gtk.ListBoxRow()
            lbr.set_child(row)
            click = Gtk.GestureClick()
            click.connect("released", lambda *_a, nw=n: self._network_clicked(nw))
            lbr.add_controller(click)
            self.wifi_list.append(lbr)

    def _network_clicked(self, n):
        net = self.shell.network
        if n["active"]:
            self.wifi_status.set_label(f"Connected to {n['ssid']}.")
            return
        if n["secure"] and not net.known(n["ssid"]):
            self._ask_password(n["ssid"])
            return
        self.wifi_status.set_label(f"Connecting to {n['ssid']}…")
        net.connect_to(n["ssid"], None,
                       lambda ok, problem: self._connected(n["ssid"], ok, problem))

    def _ask_password(self, ssid, again=False):
        self.password_for = ssid
        clear(self.wifi_list)
        box = vbox(10)
        text = (f"That password didn't work. Type the password for “{ssid}” again." if again
                else f"Password for “{ssid}”")
        box.append(label(text, wrap=True))
        entry = Gtk.PasswordEntry()
        entry.set_show_peek_icon(True)
        entry.add_css_class("field")
        box.append(entry)
        cancel = button(text="Cancel", css="flat", on_click=self._cancel_password)
        connect = button(text="Connect", css="flat danger",
                         on_click=lambda: self._submit_password(ssid, entry.get_text()))
        entry.connect("activate", lambda *_: self._submit_password(ssid, entry.get_text()))
        actions = hbox(8, cancel, connect)
        actions.set_halign(Gtk.Align.END)
        box.append(actions)
        row = Gtk.ListBoxRow()
        row.set_activatable(False)
        row.set_child(box)
        self.wifi_list.append(row)
        entry.grab_focus()

    def _cancel_password(self):
        self.password_for = None
        self._fill_wifi_list()

    def _submit_password(self, ssid, password):
        if not password:
            return
        self.password_for = None
        self.wifi_status.set_label(f"Connecting to {ssid}…")
        self._fill_wifi_list()
        self.shell.network.connect_to(ssid, password,
                                      lambda ok, problem: self._connected(ssid, ok, problem))

    def _connected(self, ssid, ok, problem):
        if ok:
            self.wifi_status.set_label(f"Connected to {ssid}.")
            return
        if problem == "password":
            self.wifi_status.set_label(f"Could not connect to {ssid}.")
            if self.window.get_visible() and self.stack.get_visible_child_name() == "wifi":
                self._ask_password(ssid, again=True)
            return
        reason = {
            "range": "That network is out of range.",
            "enterprise": "This network also needs a user name, which isn't supported yet.",
        }.get(problem, "Make sure it is in range and try again.")
        self.wifi_status.set_label(f"Could not connect to {ssid}. {reason}")

    def update_bluetooth(self):
        bt = self.shell.bluetooth
        names = bt.connected_names()
        if not bt.available:
            sub = "Not available"
        elif not bt.powered:
            sub = "Off"
        elif names:
            sub = names[0] if len(names) == 1 else f"{len(names)} devices"
        else:
            sub = "On"
        self.bt_tile.set(bt.available and bt.powered, sub)
        self.bt_tile.box.set_sensitive(bt.available)
        if self.bt_switch is not None:
            self.bt_switch.updating = True
            self.bt_switch.set_active(bt.powered)
            self.bt_switch.set_sensitive(bt.available)
            self.bt_switch.updating = False
        self.bt_search.set_label("Stop looking" if bt.discovering else "Look for new devices")
        self.bt_search.set_visible(bt.powered)
        if not bt.available:
            self.bt_status.set_label("No Bluetooth adapter was found.")
        elif not bt.powered:
            self.bt_status.set_label("Bluetooth is off. Turn it on with the switch above.")
        elif bt.busy:
            verb = {"pair": "Pairing with", "connect": "Connecting to",
                    "disconnect": "Disconnecting from"}[bt.busy["action"]]
            self.bt_status.set_label(f"{verb} {bt.busy['name']}…")
        elif bt.discovering:
            self.bt_status.set_label("Looking for devices. Put your device in pairing mode, "
                                     "then click its name.")
        elif any(dev["paired"] for dev in bt.devices):
            self.bt_status.set_label("Click a device to connect or disconnect it.")
        else:
            self.bt_status.set_label("No devices yet. Click “Look for new devices” to add one.")
        self._update_bt_prompt(bt.prompt)
        clear(self.bt_list)
        if not bt.powered:
            return
        for dev in bt.devices:
            if not dev["paired"] and not bt.discovering:
                continue
            status = "Connected" if dev["connected"] else ("Paired" if dev["paired"] else "New")
            row = hbox(10, icon(dev["icon"]))
            name = label(dev["name"], ellipsize=True)
            name.set_hexpand(True)
            row.append(name)
            row.append(label(status, "badge" if dev["connected"] else "dim"))
            lbr = Gtk.ListBoxRow()
            lbr.set_child(row)
            click = Gtk.GestureClick()
            click.connect("released", lambda *_a, d=dev: self._device_clicked(d))
            lbr.add_controller(click)
            self.bt_list.append(lbr)

    def _update_bt_prompt(self, prompt):
        if prompt == self._shown_prompt:
            return
        self._shown_prompt = prompt
        clear(self.bt_prompt)
        self.bt_prompt.set_visible(prompt is not None)
        if prompt is None:
            return
        # Pairing waits for an answer here, so make sure it can be seen.
        if not self.window.get_visible():
            self.show(self.monitor)
        self.open_page("bluetooth")
        bt = self.shell.bluetooth
        name, code, kind = prompt["name"], prompt["code"], prompt["kind"]
        actions = hbox(8)
        actions.set_halign(Gtk.Align.END)
        if kind == "confirm":
            self.bt_prompt.append(label(f"Does {name} show this number?", wrap=True))
            self.bt_prompt.append(label(code, "pairing-code"))
            actions.append(button(text="No", css="flat", on_click=lambda: bt.answer(False)))
            actions.append(button(text="Yes, pair", css="flat danger",
                                  on_click=lambda: bt.answer(True)))
        elif kind == "display":
            self.bt_prompt.append(label(f"On {name}, type this number and then press Enter:",
                                        wrap=True))
            self.bt_prompt.append(label(code, "pairing-code"))
            actions.append(button(text="Cancel", css="flat", on_click=lambda: bt.answer(False)))
        else:
            self.bt_prompt.append(label(
                f"Type the {'PIN' if kind == 'pin' else 'code'} for {name}. It is shown on the "
                "device or in its manual (often 0000).", wrap=True))
            entry = Gtk.Entry()
            entry.add_css_class("field")
            if kind == "passkey":
                entry.set_input_purpose(Gtk.InputPurpose.DIGITS)
            entry.set_max_length(6 if kind == "passkey" else 16)
            self.bt_prompt.append(entry)

            def send(*_args):
                text = entry.get_text().strip()
                if text and (kind == "pin" or (text.isascii() and text.isdigit())):
                    bt.answer(True, text)

            entry.connect("activate", send)
            actions.append(button(text="Cancel", css="flat", on_click=lambda: bt.answer(False)))
            actions.append(button(text="Pair", css="flat danger", on_click=send))
            GLib.idle_add(lambda: (entry.grab_focus(), False)[1])
        self.bt_prompt.append(actions)

    def _device_clicked(self, dev):
        if self.shell.bluetooth.busy:
            return

        def done(ok):
            if ok is False:
                self.bt_status.set_label(f"That didn't work. Make sure {dev['name']} is "
                                         "switched on and nearby, then try again.")

        self.shell.bluetooth.toggle_device(dev, done)

    def update_audio(self):
        audio = self.shell.audio
        self.volume.widget.set_visible(audio.available)
        self.volume.set(0 if audio.muted else audio.volume, audio.icon())

    def update_brightness(self):
        b = self.shell.brightness
        self.brightness.widget.set_visible(b.available)
        if b.available:
            self.brightness.set(b.level)

    def update_battery(self):
        bat = self.shell.battery
        self.battery_row.set_visible(bat.present)
        self.date_label.set_visible(not bat.present)
        self.date_label.set_label(clock.date_text())
        if bat.present:
            self.battery_icon.set_from_icon_name(bat.icon_name)
            self.battery_label.set_label(bat.describe())

    def update_history(self):
        items = self.shell.notifications.history
        clear(self.history_list)
        self.history_header.set_visible(bool(items))
        for n in items[:4]:
            title = label(n["summary"], ellipsize=True)
            title.set_hexpand(True)
            body = label(n["body_plain"], "dim", ellipsize=True)
            box = vbox(2, hbox(6, title, label(n["time"], "dim")), body)
            box.add_css_class("history-item")
            self.history_list.append(box)

    # ------------------------------------------------------------- show/hide

    @property
    def visible(self):
        return self.window.get_visible()

    def show(self, monitor=None):
        self.monitor = monitor
        set_monitor(self.window, monitor)
        self.place()
        self.open_page("main")
        self.date_label.set_label(clock.date_text())
        self.shell.audio.refresh()
        self.shell.network.refresh()
        self.window.present()
        self.shell.set_status_open(True)

    def hide(self):
        if self.shell.bluetooth.discovering:
            self.shell.bluetooth.set_discovering(False)
        self.shell.network.set_list_wanted(False)
        self.password_for = None
        self.window.set_visible(False)
        self.shell.set_status_open(False)

    def back_or_hide(self):
        if self.stack.get_visible_child_name() != "main":
            self.open_page("main")
        else:
            self.hide()

    def toggle(self, monitor=None, page=None):
        if self.visible and page is None:
            self.hide()
        else:
            self.show(monitor)
            if page:
                self.open_page(page)

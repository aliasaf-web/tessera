#!/bin/sh
# Installs (or updates) the Tessera desktop on Debian 13 "trixie".
# Run it as your normal user, from inside the tessera folder:   sh install.sh

set -eu

cd "$(dirname "$0")"
SRC="$(pwd)"
PREFIX=/usr/local
ME="$(id -un)"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31m%s\033[0m\n\n' "$*"; exit 1; }

# ---------------------------------------------------------------- checks

if [ "$(id -u)" -eq 0 ]; then
	fail "Please run this as your normal user, not as root:  sh install.sh"
fi
if ! command -v sudo >/dev/null 2>&1 || ! sudo -v; then
	fail "This needs 'sudo'. Log in as root (type: su -), then run:
    apt install sudo && adduser $ME sudo
Then log out, log back in as $ME and run this installer again."
fi
. /etc/os-release
if [ "${VERSION_CODENAME:-}" != "trixie" ]; then
	warn "This was made for Debian 13 (trixie); you have ${PRETTY_NAME:-an unknown system}."
	printf 'Continue anyway? [y/N] '
	read -r answer </dev/tty || answer=""
	case "$answer" in y|Y|yes) ;; *) exit 1 ;; esac
fi

AUTOLOGIN=yes
if [ "${TESSERA_AUTOLOGIN:-ask}" = "ask" ]; then
	echo
	echo "When the laptop turns on, Tessera can go straight to its own lock screen,"
	echo "where you type your password. (Otherwise you log in on a text screen first.)"
	printf 'Use the Tessera lock screen as your login screen? [Y/n] '
	read -r answer </dev/tty || answer=""
	case "$answer" in n|N|no) AUTOLOGIN=no ;; esac
else
	AUTOLOGIN="$TESSERA_AUTOLOGIN"
fi

# -------------------------------------------------------------- packages

say "Installing the programs Tessera needs (this can take a while)"
sudo apt-get update

# Needed to build the window manager.
BUILD="build-essential meson ninja-build pkg-config libwlroots-0.18-dev
libwayland-dev wayland-protocols libxkbcommon-dev libinput-dev libpixman-1-dev
libcairo2-dev libpango1.0-dev libdrm-dev libsystemd-dev libpam0g-dev
libxcb1-dev libxcb-icccm4-dev libxcb-ewmh-dev"

# Needed to run the desktop.
RUNTIME="xwayland dbus-user-session polkitd network-manager pipewire-audio
wireplumber pulseaudio-utils bluez rfkill upower xdg-desktop-portal
xdg-desktop-portal-gtk xdg-desktop-portal-wlr xdg-utils xdg-user-dirs
python3 python3-gi gir1.2-gtk-4.0 gir1.2-gtk4layershell-1.0 libgtk4-layer-shell0
gsettings-desktop-schemas dconf-gsettings-backend librsvg2-common
adwaita-icon-theme papirus-icon-theme fonts-inter fonts-noto-color-emoji
grim slurp wl-clipboard playerctl"

# Everyday apps: terminal, files, web browser.
APPS="foot thunar firefox-esr"

# shellcheck disable=SC2086
sudo apt-get install -y $BUILD $RUNTIME $APPS

# Nice to have; skipped quietly if a package is not available.
for pkg in adw-gtk3 gnome-text-editor loupe celluloid pavucontrol file-roller \
		tumbler gvfs-backends lxpolkit thunar-archive-plugin; do
	sudo apt-get install -y "$pkg" >/dev/null 2>&1 || warn "  (skipped optional package $pkg)"
done

# ---------------------------------------------------------------- build

say "Building the Tessera window manager"
rm -rf "$SRC/compositor/build"
meson setup "$SRC/compositor/build" "$SRC/compositor" --prefix="$PREFIX" --buildtype=release
ninja -C "$SRC/compositor/build"
sudo ninja -C "$SRC/compositor/build" install

say "Installing the Tessera shell and settings"
sudo install -d "$PREFIX/share/tessera/shell"
sudo rm -rf "$PREFIX/share/tessera/shell/tessera_shell"
sudo cp -r "$SRC/shell/tessera_shell" "$PREFIX/share/tessera/shell/"
sudo find "$PREFIX/share/tessera/shell" -name '__pycache__' -prune -exec rm -rf {} +
for script in tessera-session tessera-login tessera-autostart tessera-shell; do
	sudo install -m 755 "$SRC/data/$script" "$PREFIX/bin/$script"
done
sudo install -Dm 644 "$SRC/data/tessera.desktop" /usr/share/wayland-sessions/tessera.desktop
sudo install -Dm 644 "$SRC/data/pam-tessera" /etc/pam.d/tessera
sudo install -Dm 644 "$SRC/data/tessera-portals.conf" /etc/xdg/xdg-desktop-portal/tessera-portals.conf
sudo install -Dm 644 "$SRC/data/polkit-tessera.rules" /etc/polkit-1/rules.d/50-tessera.rules
if [ ! -f /etc/tessera/config ]; then
	sudo install -Dm 644 "$SRC/data/config" /etc/tessera/config
fi

# ------------------------------------------------------------ your user

say "Setting up your account"
for group in netdev bluetooth video audio; do
	if getent group "$group" >/dev/null; then
		sudo usermod -aG "$group" "$ME"
	fi
done
xdg-user-dirs-update >/dev/null 2>&1 || true
mkdir -p "$HOME/.config/foot"
if [ ! -f "$HOME/.config/foot/foot.ini" ]; then
	cp "$SRC/data/foot.ini" "$HOME/.config/foot/foot.ini"
fi
if command -v xdg-mime >/dev/null 2>&1 && [ -f /usr/share/applications/thunar.desktop ]; then
	if [ -z "$(xdg-mime query default inode/directory 2>/dev/null)" ]; then
		xdg-mime default thunar.desktop inode/directory || true
	fi
fi

# Start the desktop from the first text screen.
PROFILE="$HOME/.profile"
touch "$PROFILE"
if ! grep -q '# >>> tessera >>>' "$PROFILE"; then
	cat >>"$PROFILE" <<'EOF'

# >>> tessera >>>
# Start the Tessera desktop on the first screen.
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ "$(tty)" = "/dev/tty1" ] && command -v tessera-login >/dev/null 2>&1; then
	tessera-login
	exit 0
fi
# <<< tessera <<<
EOF
fi

GETTY_DIR=/etc/systemd/system/getty@tty1.service.d
if [ "$AUTOLOGIN" = "yes" ]; then
	sudo install -d "$GETTY_DIR"
	printf '[Service]\nExecStart=\nExecStart=-/sbin/agetty --autologin %s --noclear %%I $TERM\n' "$ME" |
		sudo tee "$GETTY_DIR/tessera-autologin.conf" >/dev/null
	sudo sed -i 's/^lock_on_start=.*/lock_on_start=true/' /etc/tessera/config
	grep -q '^lock_on_start=' /etc/tessera/config ||
		echo 'lock_on_start=true' | sudo tee -a /etc/tessera/config >/dev/null
else
	sudo rm -f "$GETTY_DIR/tessera-autologin.conf"
	sudo sed -i 's/^lock_on_start=.*/lock_on_start=false/' /etc/tessera/config
fi
sudo systemctl daemon-reload

# -------------------------------------------------------------- network

# The Debian installer sets up the network in /etc/network/interfaces when
# no desktop is chosen. Hand Wi-Fi over to NetworkManager, keeping the
# network name and password, so it can be managed from quick settings.
say "Letting the desktop manage Wi-Fi and network connections"
IFACES=/etc/network/interfaces
# The file can hold the Wi-Fi password, so only root may read it.
IFACES_COPY="$(mktemp)"
chmod 600 "$IFACES_COPY"
# shellcheck disable=SC2024  # reading needs root, the copy belongs to us
sudo cat "$IFACES" >"$IFACES_COPY" 2>/dev/null || true
if grep -E '^[[:space:]]*iface[[:space:]]' "$IFACES_COPY" |
		grep -qvE '^[[:space:]]*iface[[:space:]]+lo[[:space:]]'; then
	[ -f "$IFACES.before-tessera" ] || sudo cp "$IFACES" "$IFACES.before-tessera"
	current=""
	ssid=""
	psk=""
	migrate_wifi() {
		if [ -n "$current" ] && [ -n "$ssid" ]; then
			if ! nmcli -t -f NAME connection show 2>/dev/null | grep -qxF "$ssid"; then
				if [ -n "$psk" ]; then
					sudo nmcli connection add type wifi con-name "$ssid" ifname "$current" ssid "$ssid" \
						wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$psk" </dev/null >/dev/null &&
						echo "  Saved Wi-Fi network \"$ssid\"" || warn "  Could not save Wi-Fi \"$ssid\""
				else
					sudo nmcli connection add type wifi con-name "$ssid" ifname "$current" ssid "$ssid" \
						</dev/null >/dev/null && echo "  Saved Wi-Fi network \"$ssid\"" ||
						warn "  Could not save Wi-Fi \"$ssid\""
				fi
			fi
		fi
	}
	while IFS= read -r line; do
		# shellcheck disable=SC2086
		set -- $line
		case "${1:-}" in
		iface)
			migrate_wifi
			current="${2:-}"; ssid=""; psk="" ;;
		wpa-ssid|wireless-essid)
			shift; ssid="$*" ;;
		wpa-psk)
			shift; psk="$*" ;;
		esac
	done <"$IFACES_COPY"
	migrate_wifi
	# Keep only the loopback part; NetworkManager handles everything else.
	printf '# Networks are managed by NetworkManager (Tessera quick settings).\n# The previous version of this file is in %s.before-tessera\n\nsource /etc/network/interfaces.d/*\n\nauto lo\niface lo inet loopback\n' "$IFACES" |
		sudo tee "$IFACES" >/dev/null
fi
rm -f "$IFACES_COPY"
sudo systemctl enable NetworkManager >/dev/null 2>&1 || true
sudo systemctl enable bluetooth >/dev/null 2>&1 || true

say "All done!"
echo
echo "  Restart the laptop to start Tessera:   sudo reboot"
echo
if [ "$AUTOLOGIN" = "yes" ]; then
	echo "  After the restart you will see the Tessera lock screen."
	echo "  Type your password and press Enter."
else
	echo "  After the restart, log in on the text screen as usual;"
	echo "  the desktop then starts by itself."
fi
echo "  Tap the Super (Windows) key to open your apps."
echo

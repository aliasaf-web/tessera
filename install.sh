#!/bin/sh
# Installs the Tessera desktop on Debian 13 "trixie".
# Run it as your normal user, from inside the tessera folder:   sh install.sh
# "sh install.sh --update" brings an installed Tessera up to date without
# asking any questions (tessera-update runs it after downloading). An update
# never brings back apps you removed or turns back on what you turned off.

set -eu

MODE=install
case "${1:-}" in
"") ;;
--update) MODE=update ;;
*) printf 'Usage: sh install.sh [--update]\n'; exit 2 ;;
esac

cd "$(dirname "$0")"
SRC="$(pwd)"
PREFIX=/usr/local
ME="$(id -un)"
TESSERA_VERSION="$(cat "$SRC/VERSION")"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31m%s\033[0m\n\n' "$*"; exit 1; }
installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'ok installed'; }

# ---------------------------------------------------------------- checks

if [ "$(id -u)" -eq 0 ]; then
	fail "Please run this as your normal user, not as root:  sh install.sh"
fi
if ! command -v sudo >/dev/null 2>&1 || ! sudo -v; then
	fail "This needs 'sudo'. Log in as root (type: su -), then run:
    apt install sudo && adduser $ME sudo
Then log out, log back in as $ME and run this installer again."
fi
# Read in a subshell: /etc/os-release sets variables such as VERSION.
# shellcheck source=/dev/null
CODENAME="$(. /etc/os-release && printf '%s' "${VERSION_CODENAME:-}")"
# shellcheck source=/dev/null
SYSTEM="$(. /etc/os-release && printf '%s' "${PRETTY_NAME:-an unknown system}")"
if [ "$CODENAME" != "trixie" ]; then
	warn "This was made for Debian 13 (trixie); you have $SYSTEM."
	printf 'Continue anyway? [y/N] '
	read -r answer </dev/tty || answer=""
	case "$answer" in y|Y|yes) ;; *) exit 1 ;; esac
fi

# Everything put in place with root rights is read out of this folder, so no
# one but you must be able to change what is in it: otherwise any program
# running as another user could decide what ends up installed on the machine.
SRC_OWNER="$(stat -c %U "$SRC")"
if [ "$SRC_OWNER" != "$ME" ] && [ "$SRC_OWNER" != "root" ]; then
	fail "The Tessera folder ($SRC) belongs to $SRC_OWNER, not to $ME.
Take it back first, then run the installer again:
    sudo chown -R $ME $SRC"
fi
MODE_BAD="$(find "$SRC" -maxdepth 0 -perm /g+w,o+w -print 2>/dev/null || true)"
if [ -n "$MODE_BAD" ]; then
	# Someone other than you can change what gets installed with root rights.
	# Taking the write bit off is the whole remedy, and this folder is ours to
	# take it off when you own it - so it is done rather than asked about: an
	# update that stops here leaves a person with the version they already had
	# and nothing saying why, which is worse than a quiet fix.
	if [ "$SRC_OWNER" = "$ME" ]; then
		chmod go-w "$SRC" 2>/dev/null || true
	else
		sudo chmod go-w "$SRC" 2>/dev/null || true
	fi
	if [ -n "$(find "$SRC" -maxdepth 0 -perm /g+w,o+w -print 2>/dev/null)" ]; then
		fail "Other people can change the Tessera folder ($SRC), and Tessera
installs from it with root rights, so they could decide what ends up on your
machine. This could not be closed off from here. Make the folder yours alone
and run this again:
    chmod go-w $SRC"
	fi
	warn "  The Tessera folder was open to other people's changes; it is yours alone now."
fi

if [ "$MODE" = "update" ]; then
	# The folder not having anything new in it is the one way an update can look
	# like it worked and change nothing, so it is said out loud. Reinstalling the
	# same version is still allowed: it is how a broken install gets repaired.
	INSTALLED_VERSION="$(cat "$PREFIX/share/tessera/VERSION" 2>/dev/null || true)"
	if [ "$INSTALLED_VERSION" = "$TESSERA_VERSION" ]; then
		warn "This folder holds version $TESSERA_VERSION, which is what is already"
		warn "installed, so nothing here will change. If you meant to get a newer"
		warn "Tessera, this folder may not be on the branch that has it:"
		warn "    cd $SRC && git status -sb && git log --oneline -1"
		echo
	fi
fi

# Automatic login: asked on a first install; an update keeps the choice.
GREETD_CONFIG=/etc/tessera/greetd.toml
OLD_GETTY=/etc/systemd/system/getty@tty1.service.d/tessera-autologin.conf
if [ "$MODE" = "update" ]; then
	if [ -f "$GREETD_CONFIG" ]; then
		if grep -q '^\[initial_session\]' "$GREETD_CONFIG"; then AUTOLOGIN=yes; else AUTOLOGIN=no; fi
	elif [ -f "$OLD_GETTY" ]; then
		AUTOLOGIN=yes # set up by Tessera 1.0 or 1.1
	else
		AUTOLOGIN=no
	fi
elif [ "${TESSERA_AUTOLOGIN:-ask}" = "ask" ]; then
	AUTOLOGIN=yes
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

# What Tessera needs is described as two small packages (see data/packages),
# so apt keeps track of it: "Depends" are always kept, and a "Recommends" app
# that you remove stays removed, because apt only adds recommended packages
# the first time or when a new version recommends something new.
PKGDIR="$(mktemp -d)"
make_package() {
	mkdir -p "$PKGDIR/$1/DEBIAN"
	sed "s/@VERSION@/$TESSERA_VERSION/" "$SRC/data/packages/$1.control" >"$PKGDIR/$1/DEBIAN/control"
	chmod 755 "$PKGDIR/$1" "$PKGDIR/$1/DEBIAN"
	chmod 644 "$PKGDIR/$1/DEBIAN/control"
	dpkg-deb --root-owner-group --build "$PKGDIR/$1" "$PKGDIR/$1.deb" >/dev/null
	chmod 644 "$PKGDIR/$1.deb"
}
make_package tessera-build-deps
make_package tessera-desktop
chmod 755 "$PKGDIR" # apt reads the packages as its own user
PACKAGES="$PKGDIR/tessera-build-deps.deb $PKGDIR/tessera-desktop.deb"

# The login screen starts after the restart, not in the middle of installing.
sudo systemctl mask --runtime greetd.service >/dev/null 2>&1 || true
if [ "$MODE" = "install" ] || installed tessera-desktop; then
	# shellcheck disable=SC2086
	sudo apt-get install -y -o APT::Install-Recommends=true $PACKAGES
else
	# The first update after Tessera 1.0 or 1.1, which installed apps one by
	# one: apps removed since then must stay removed.
	# shellcheck disable=SC2086
	sudo apt-get install -y --no-install-recommends $PACKAGES
fi
sudo systemctl unmask --runtime greetd.service >/dev/null 2>&1 || true
rm -rf "$PKGDIR"

# ---------------------------------------------------------- login screen

# Set up before the new version is built: if a later step fails, the
# computer still starts the installed version, locked as before.
say "Setting up the login screen"
AGREETY=""
for path in /usr/sbin/agreety /usr/bin/agreety; do
	if [ -x "$path" ]; then
		AGREETY="$path"
		break
	fi
done
if [ -z "$AGREETY" ] || ! getent passwd _greetd >/dev/null; then
	fail "The login screen (greetd) did not install correctly. Please run the installer again."
fi
FIRST_LOGIN_SETUP=no
[ -f "$GREETD_CONFIG" ] || FIRST_LOGIN_SETUP=yes
GREETD_TMP="$(mktemp)"
sed -e "s|@AGREETY@|$AGREETY|" -e "s|@PREFIX@|$PREFIX|g" "$SRC/data/greetd.toml" >"$GREETD_TMP"
if [ "$AUTOLOGIN" = "yes" ]; then
	sed -e "s|@PREFIX@|$PREFIX|g" -e "s|@USER@|$ME|" "$SRC/data/greetd-autologin.toml" >>"$GREETD_TMP"
fi
sudo install -Dm 644 "$GREETD_TMP" "$GREETD_CONFIG"
rm -f "$GREETD_TMP"
sudo install -Dm 644 "$SRC/data/greetd-tessera.conf" /etc/systemd/system/greetd.service.d/tessera.conf
sudo systemctl daemon-reload
if [ "$FIRST_LOGIN_SETUP" = "yes" ]; then
	# Later updates leave this alone, in case you choose another login screen.
	if ! sudo systemctl enable greetd.service >/dev/null 2>&1; then
		warn "  Another login screen is already set up on this computer."
		warn "  Choose \"Tessera\" in it to use this desktop."
	fi
	if [ "$(systemctl get-default)" != "graphical.target" ]; then
		sudo systemctl set-default graphical.target
	fi
fi

# Tessera 1.0 and 1.1 started from a text login on the first screen. That
# is replaced by the login screen above, which never leaves a command line
# open (a text login could be interrupted with Ctrl+C).
if [ -f "$OLD_GETTY" ]; then
	sudo rm -f "$OLD_GETTY"
	sudo rmdir --ignore-fail-on-non-empty "$(dirname "$OLD_GETTY")"
	sudo systemctl daemon-reload
fi
sudo rm -f "$PREFIX/bin/tessera-login"
PROFILE="$HOME/.profile"
if [ -f "$PROFILE" ] && grep -q '^# >>> tessera >>>$' "$PROFILE" &&
		grep -q '^# <<< tessera <<<$' "$PROFILE"; then
	sed -i '/^# >>> tessera >>>$/,/^# <<< tessera <<<$/d' "$PROFILE"
fi

# ---------------------------------------------------------------- build

say "Building the Tessera window manager"
BUILDDIR="$SRC/compositor/build"
fresh_build() {
	rm -rf "$BUILDDIR"
	meson setup "$BUILDDIR" "$SRC/compositor" --prefix="$PREFIX" --buildtype=release
	ninja -C "$BUILDDIR"
}
# An update only rebuilds what changed; a full build is the fallback.
if [ "$MODE" = "update" ] && [ -f "$BUILDDIR/build.ninja" ]; then
	ninja -C "$BUILDDIR" || fresh_build
else
	fresh_build
fi
# The build runs as you, and only the finished program is put where it goes
# with root rights. Running "sudo ninja install" instead would let a compiler
# (and anything it runs) act as root inside a folder you can write to, which
# is an easier way in than installing anything.
sudo install -Dm 755 "$BUILDDIR/tessera" "$PREFIX/bin/tessera"

say "Installing the Tessera shell and settings"
sudo install -d "$PREFIX/share/tessera/shell"
sudo rm -rf "$PREFIX/share/tessera/shell/tessera_shell"
sudo cp -r "$SRC/shell/tessera_shell" "$PREFIX/share/tessera/shell/"
sudo install -m 644 "$SRC/VERSION" "$PREFIX/share/tessera/VERSION"
sudo find "$PREFIX/share/tessera/shell" -name '__pycache__' -prune -exec rm -rf {} +
for script in tessera-session tessera-autostart tessera-shell tessera-settings; do
	sudo install -m 755 "$SRC/data/$script" "$PREFIX/bin/$script"
done
# tessera-update needs to know where this version was installed.
UPDATE_TMP="$(mktemp)"
sed "s|@PREFIX@|$PREFIX|g" "$SRC/data/tessera-update" >"$UPDATE_TMP"
sudo install -m 755 "$UPDATE_TMP" "$PREFIX/bin/tessera-update"
rm -f "$UPDATE_TMP"
# Lets tessera-update find this folder.
printf '%s\n' "$SRC" | sudo tee "$PREFIX/share/tessera/source" >/dev/null
sudo install -Dm 644 "$SRC/data/org.tessera.Settings.desktop" \
	"$PREFIX/share/applications/org.tessera.Settings.desktop"
sudo install -Dm 644 "$SRC/data/tessera.desktop" /usr/share/wayland-sessions/tessera.desktop
sudo install -Dm 644 "$SRC/data/pam-tessera" /etc/pam.d/tessera
sudo install -Dm 644 "$SRC/data/tessera-portals.conf" /etc/xdg/xdg-desktop-portal/tessera-portals.conf
sudo install -Dm 644 "$SRC/data/polkit-tessera.rules" /etc/polkit-1/rules.d/50-tessera.rules
# The shared settings file is replaced by the new version; personal choices
# live in ~/.config/tessera/config.
sudo install -Dm 644 "$SRC/data/config" /etc/tessera/config

# ------------------------------------------------- first install only

if [ "$MODE" = "install" ]; then
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

	# The Debian installer sets up the network in /etc/network/interfaces
	# when no desktop is chosen. Hand Wi-Fi over to NetworkManager, keeping
	# the network name and password, so it can be managed from quick settings.
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
		# A value written in double quotes means what is inside them.
		unquote() {
			case "$1" in
			\"*\") v="${1#\"}"; printf '%s' "${v%\"}" ;;
			*) printf '%s' "$1" ;;
			esac
		}
		# NetworkManager's file format: backslashes doubled, a leading space as \s.
		keyfile_value() {
			v="$(printf '%s' "$1" | sed 's/\\/\\\\/g')"
			case "$v" in " "*) v="\\s${v# }" ;; esac
			printf '%s' "$v"
		}
		migrate_wifi() {
			if [ -z "$current" ] || [ -z "$ssid" ]; then
				return
			fi
			if nmcli -t -f NAME connection show 2>/dev/null | grep -qxF "$ssid"; then
				return
			fi
			# Written straight to a root-only file, so the password is never
			# on a command line where other programs could see it.
			uuid="$(cat /proc/sys/kernel/random/uuid)"
			conn="$(mktemp)"
			chmod 600 "$conn"
			{
				printf '[connection]\nid=%s\nuuid=%s\ntype=wifi\ninterface-name=%s\n\n' \
					"$(keyfile_value "$ssid")" "$uuid" "$current"
				printf '[wifi]\nmode=infrastructure\nssid=%s\n\n' "$(keyfile_value "$ssid")"
				if [ -n "$psk" ]; then
					printf '[wifi-security]\nkey-mgmt=wpa-psk\npsk=%s\n\n' "$(keyfile_value "$psk")"
				fi
				printf '[ipv4]\nmethod=auto\n\n[ipv6]\nmethod=auto\n'
			} >"$conn"
			target="/etc/NetworkManager/system-connections/$uuid.nmconnection"
			sudo install -D -m 600 -o root -g root "$conn" "$target"
			rm -f "$conn"
			sudo nmcli connection load "$target" >/dev/null 2>&1 || true
			echo "  Saved Wi-Fi network \"$ssid\""
		}
		while IFS= read -r line; do
			# The first word is the option; the rest of the line is its value,
			# kept exactly as written (inner spaces, * and so on).
			line="${line#"${line%%[![:space:]]*}"}"
			key="${line%%[[:space:]]*}"
			value="${line#"$key"}"
			value="${value#"${value%%[![:space:]]*}"}"
			value="${value%"${value##*[![:space:]]}"}"
			case "$key" in
			iface)
				migrate_wifi
				current="${value%%[[:space:]]*}"; ssid=""; psk="" ;;
			wpa-ssid|wireless-essid)
				ssid="$(unquote "$value")" ;;
			wpa-psk)
				psk="$(unquote "$value")" ;;
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
fi

# --------------------------------------------------------------- finish

if [ "$MODE" = "update" ]; then
	say "Tessera is up to date (version $TESSERA_VERSION)"
	echo
	echo "  The new version starts after a restart."
	printf '  Restart now? Save your work first. [y/N] '
	read -r answer </dev/tty || answer=""
	case "$answer" in
	y|Y|yes) sudo systemctl reboot ;;
	*) echo "  OK. It will be used the next time the computer starts." ;;
	esac
	exit 0
fi

say "All done!"
echo
echo "  Restart the laptop to start Tessera:   sudo reboot"
echo
if [ "$AUTOLOGIN" = "yes" ]; then
	echo "  After the restart you will see the Tessera lock screen."
	echo "  Type your password and press Enter."
else
	echo "  After the restart, type your user name and password on the"
	echo "  login screen; the desktop then starts by itself."
fi
echo "  Tap the Super (Windows) key to open your apps."
echo

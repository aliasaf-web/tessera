#!/bin/sh
# Removes Tessera. Your files and the apps it installed (Firefox, Thunar, ...)
# are kept. Run as your normal user:   sh uninstall.sh

set -eu
PREFIX=/usr/local
ME="$(id -un)"
SRC="$(cd "$(dirname "$0")" && pwd)"

echo "This removes the Tessera desktop from this computer."
printf 'Continue? [y/N] '
read -r answer </dev/tty || answer=""
case "$answer" in y|Y|yes) ;; *) exit 1 ;; esac

sudo rm -f "$PREFIX/bin/tessera" "$PREFIX/bin/tessera-session" "$PREFIX/bin/tessera-login" \
	"$PREFIX/bin/tessera-autostart" "$PREFIX/bin/tessera-shell" "$PREFIX/bin/tessera-settings" \
	"$PREFIX/bin/tessera-update" "$PREFIX/share/applications/org.tessera.Settings.desktop"
sudo rm -rf "$PREFIX/share/tessera"
sudo rm -f /usr/share/wayland-sessions/tessera.desktop /etc/pam.d/tessera \
	/etc/xdg/xdg-desktop-portal/tessera-portals.conf /etc/polkit-1/rules.d/50-tessera.rules

# The login screen: back to Debian's normal text login.
sudo systemctl disable greetd.service >/dev/null 2>&1 || true
sudo rm -f /etc/systemd/system/greetd.service.d/tessera.conf /etc/tessera/greetd.toml
sudo rmdir --ignore-fail-on-non-empty /etc/systemd/system/greetd.service.d 2>/dev/null || true
# Left by Tessera 1.0 and 1.1.
sudo rm -f /etc/systemd/system/getty@tty1.service.d/tessera-autologin.conf
sudo systemctl daemon-reload
if [ -f "$HOME/.profile" ] && grep -q '^# >>> tessera >>>$' "$HOME/.profile" &&
		grep -q '^# <<< tessera <<<$' "$HOME/.profile"; then
	sed -i '/^# >>> tessera >>>$/,/^# <<< tessera <<<$/d' "$HOME/.profile"
fi

# Small changes made to your account when Tessera was first installed.
if [ -f "$HOME/.config/foot/foot.ini" ] && [ -f "$SRC/data/foot.ini" ] &&
		cmp -s "$HOME/.config/foot/foot.ini" "$SRC/data/foot.ini"; then
	rm -f "$HOME/.config/foot/foot.ini"
	rmdir "$HOME/.config/foot" 2>/dev/null || true
fi
GTK3="$HOME/.config/gtk-3.0/settings.ini"
if [ -f "$GTK3" ]; then
	sed -i '/^gtk-application-prefer-dark-theme=/d' "$GTK3"
	if [ -z "$(grep -v '^\[Settings\]$' "$GTK3" | tr -d '[:space:]')" ]; then
		rm -f "$GTK3"
	fi
fi

# The network settings from before Tessera, if it moved Wi-Fi over to
# NetworkManager. Wi-Fi keeps working either way.
IFACES=/etc/network/interfaces
if [ -f "$IFACES.before-tessera" ]; then
	echo
	echo "Tessera moved your network settings to NetworkManager, which keeps"
	echo "working without the desktop (type  nmtui  to change Wi-Fi)."
	printf 'Put back the network settings from before Tessera instead? [y/N] '
	read -r answer </dev/tty || answer=""
	case "$answer" in
	y|Y|yes)
		sudo cp "$IFACES.before-tessera" "$IFACES"
		sudo rm -f "$IFACES.before-tessera"
		echo "  Restored $IFACES" ;;
	esac
fi

# The lists of what Tessera needs. The programs themselves stay installed.
for package in tessera-desktop tessera-build-deps; do
	if dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q 'ok installed'; then
		sudo apt-get remove -y "$package" >/dev/null
	fi
done

echo
echo "Tessera has been removed. Kept, in case you reinstall:"
echo "  - your settings in /etc/tessera and ~/.config/tessera (you can delete them)"
echo "  - the programs it installed; to remove the ones nothing else uses, run:"
echo "      sudo apt autoremove"
echo "  - your account's membership of the netdev, bluetooth, video and audio"
echo "    groups, which let you use Wi-Fi, Bluetooth, brightness and sound"
echo "  - the light/dark choice and icons for apps, and Thunar as the app for folders"
echo "Restart with:  sudo reboot   (you will get the normal text login, user $ME)"

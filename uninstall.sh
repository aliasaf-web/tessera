#!/bin/sh
# Removes Tessera. Your files and the apps it installed (Firefox, Thunar, ...)
# are kept. Run as your normal user:   sh uninstall.sh

set -eu
PREFIX=/usr/local
ME="$(id -un)"

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

# The lists of what Tessera needs. The programs themselves stay installed.
for package in tessera-desktop tessera-build-deps; do
	if dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q 'ok installed'; then
		sudo apt-get remove -y "$package" >/dev/null
	fi
done

echo
echo "Tessera has been removed. Settings are left in /etc/tessera and ~/.config/tessera"
echo "in case you reinstall; you can delete those folders if you like."
echo "The programs it installed are still here; to remove the ones nothing else"
echo "uses, run:  sudo apt autoremove"
echo "Restart with:  sudo reboot   (you will get the normal text login, user $ME)"

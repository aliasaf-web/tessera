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
sudo rm -f /etc/systemd/system/getty@tty1.service.d/tessera-autologin.conf
sudo systemctl daemon-reload

if [ -f "$HOME/.profile" ]; then
	sed -i '/# >>> tessera >>>/,/# <<< tessera <<</d' "$HOME/.profile"
fi

echo
echo "Tessera has been removed. Settings are left in /etc/tessera and ~/.config/tessera"
echo "in case you reinstall; you can delete those folders if you like."
echo "Restart with:  sudo reboot   (you will get the normal text login, user $ME)"

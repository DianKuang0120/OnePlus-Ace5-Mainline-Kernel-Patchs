#!/bin/sh
# Install / update giulia-haptics on the running system.
#
#   daemon + CLI + config + systemd user unit + KWin window-event script
#
# Run as your normal desktop user:  ./install-haptics.sh
# It uses sudo for the system paths (/usr/local, /etc, /usr/share).
#
# Idempotent: safe to re-run to update after rebuilding the daemon.
set -eu

SRC="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/rootfs"

say() { printf '\n== %s ==\n' "$*"; }

say "system files (sudo)"
sudo install -Dm0755 "$SRC/usr/local/bin/giulia-hapticd" /usr/local/bin/giulia-hapticd
sudo install -Dm0755 "$SRC/usr/local/bin/giulia-haptic"  /usr/local/bin/giulia-haptic
sudo install -Dm0644 "$SRC/etc/giulia-hapticd.conf"      /etc/giulia-hapticd.conf
sudo install -Dm0644 "$SRC/usr/share/doc/giulia-hapticd/README.md" \
             /usr/share/doc/giulia-hapticd/README.md
sudo install -Dm0644 "$SRC/etc/systemd/user/giulia-hapticd.service" \
             /etc/systemd/user/giulia-hapticd.service
sudo install -Dm0644 "$SRC/usr/share/kwin/scripts/giulia-haptics/metadata.json" \
             /usr/share/kwin/scripts/giulia-haptics/metadata.json
sudo install -Dm0644 "$SRC/usr/share/kwin/scripts/giulia-haptics/contents/code/main.js" \
             /usr/share/kwin/scripts/giulia-haptics/contents/code/main.js

# A previous user-local install would shadow the system unit, so drop it.
if [ -e "$HOME/.config/systemd/user/giulia-hapticd.service" ]; then
	say "removing previous user-local install"
	systemctl --user disable --now giulia-hapticd.service 2>/dev/null || true
	rm -f "$HOME/.config/systemd/user/giulia-hapticd.service" \
	      "$HOME/.config/giulia-hapticd.conf" \
	      "$HOME/.local/bin/giulia-hapticd" "$HOME/.local/bin/giulia-haptic"
fi

say "daemon user service"
systemctl --user daemon-reload
systemctl --user enable giulia-hapticd.service
# restart even if already running, so a replaced binary takes effect
systemctl --user restart giulia-hapticd.service
sleep 1
systemctl --user --no-pager status giulia-hapticd.service | head -5

say "KWin window-event script"
kwriteconfig6 --file kwinrc --group Plugins --key giulia-hapticsEnabled true
qdbus6 org.kde.KWin /KWin reconfigure >/dev/null 2>&1 || true
if [ "$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.isScriptLoaded giulia-haptics 2>/dev/null)" = "true" ]; then
	echo "KWin script loaded"
else
	echo "KWin script not loaded yet -- log out and back in once"
	echo "(KWin only discovers a newly installed script package at startup;"
	echo " toggling it in System Settings -> Window Management -> KWin Scripts"
	echo " also works)."
fi

say "done"
echo "test:  giulia-haptic slider_mid"
echo "tune:  edit /etc/giulia-hapticd.conf   (effect 0 disables an event)"

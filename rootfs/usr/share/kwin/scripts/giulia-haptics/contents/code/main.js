// SPDX-License-Identifier: GPL-2.0-or-later
//
// giulia-haptics - forward KWin window events to giulia-hapticd.
//
// The daemon owns org.giulia.Haptic on the session bus and exposes
// Trigger(s); the event names are mapped to effects in
// ~/.config/giulia-hapticd.conf (or /etc/giulia-hapticd.conf).
//
//   ui_maximize  window maximized / restored   -> effect 6
//   ui_snap      window quick-tiled / snapped  -> effect 105
//   ui_minimize  window minimized              -> effect 6

const HAPTIC_SERVICE = "org.giulia.Haptic";
const HAPTIC_PATH = "/org/giulia/Haptic";
const HAPTIC_IFACE = "org.giulia.Haptic";

function trigger(event) {
    callDBus(HAPTIC_SERVICE, HAPTIC_PATH, HAPTIC_IFACE, "Trigger", event);
}

function watch(window) {
    if (!window.normalWindow) {
        return;
    }

    window.maximizedChanged.connect(() => trigger("ui_maximize"));
    window.quickTileModeChanged.connect(() => trigger("ui_snap"));
    window.minimizedChanged.connect(() => {
        if (window.minimized) {
            trigger("ui_minimize");
        }
    });
}

function main() {
    workspace.windowList().forEach(watch);
    workspace.windowAdded.connect(watch);
}

main();

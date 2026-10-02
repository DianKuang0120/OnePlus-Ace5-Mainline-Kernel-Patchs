# giulia-hapticd — user-space haptics for OnePlus 13R / Ace 5 (giulia)

Turns kernel input events and desktop events into vibrations on the
PM8550B HV haptics driver, using the stock 809 Android waveforms.

Effect selection is data-driven: `giulia-hapticd` maps an **event name**
to an **effect id**, and `qcom-haptics-play -e <id>` renders the
waveform through the standard input FF interface.

## Events

| event           | source                                       | default effect |
|-----------------|----------------------------------------------|----------------|
| `power`         | `pmic_pwrkey`    `KEY_POWER` (116)           | 2   |
| `volume_up`     | `pmic_resin`     `KEY_VOLUMEUP` (115)        | 111 |
| `volume_down`   | `gpio-keys`      `KEY_VOLUMEDOWN` (114)      | 111 |
| `slider_up`     | `ak09970-slider` `ABS_X` = 0                 | 0 (off) |
| `slider_mid`    | `ak09970-slider` `ABS_X` = 1                 | 365 |
| `slider_down`   | `ak09970-slider` `ABS_X` = 2                 | 308 |
| `notification`  | session bus `org.freedesktop.Notifications Notify` | 105 |
| `ui_desktop`    | `org.kde.KWin.VirtualDesktopManager` `current` | 6 |
| `ui_showdesktop`| `org.kde.KWin` `showingDesktopChanged`       | 6   |

`0` disables an event.  The effect ids come from the Android usage map
in `~/README.md` (SystemUI / key feedback / alert slider / desktop).

## Configuration

`/etc/giulia-hapticd.conf` (system) or `~/.config/giulia-hapticd.conf`
(user-local).  Keys:

    player      = /usr/local/bin/qcom-haptics-play
    gain        = 100          # 1..100
    min_gap_ms  = 40           # global rate limit
    dev_power   = pmic_pwrkey  # evdev names (override when needed)
    dev_volup   = pmic_resin
    dev_voldown = gpio-keys
    dev_slider  = ak09970-slider
    <event>     = <effect_id>

## Control FIFO / CLI

`giulia-hapticd` creates `$XDG_RUNTIME_DIR/giulia-hapticd.fifo`.  Any
process can trigger a named event by writing a line to it; the
`giulia-haptic` helper does that:

    giulia-haptic volume_down
    giulia-haptic slider_mid

Use it for KDE custom shortcuts (System Settings → Shortcuts → Custom →
Command `giulia-haptic <event>`), scripts, or anything that can write a
file.

## Files

    /usr/local/bin/giulia-hapticd        daemon (C)
    /usr/local/bin/giulia-haptic         FIFO trigger helper
    /etc/giulia-hapticd.conf             config
    /etc/systemd/user/giulia-hapticd.service

A user-local install is also supported: `~/.local/bin/giulia-hapticd`,
`~/.config/giulia-hapticd.conf`, `~/.config/systemd/user/…`.

## Build

    cc -O2 -o giulia-hapticd giulia-hapticd.c

## Notes

* The daemon runs as a **user** service (needs the `input` group to read
  `/dev/input/event*` and the session bus for desktop events).
* Each child (`dbus-monitor`, `qcom-haptics-play`) is armed with
  `PR_SET_PDEATHSIG`, so nothing is left behind across restarts.
* Window / taskbar events are not covered yet: those need a KWin script
  calling into the daemon.  See the project hand-off notes.

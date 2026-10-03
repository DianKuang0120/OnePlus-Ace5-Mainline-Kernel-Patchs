# SC8547 charge-pump driver — rewrite design

Status: **design / pre-rewrite**.  Target file:
`drivers/power/supply/sc8547.c` (+ `sc8547.h`).

The current driver was hacked just far enough to make fast charging work
on the OnePlus 13R/Ace 5 (`giulia`).  This document describes what is
wrong with it, the target architecture, and the migration/test plan.

## 1. What the SC8547 is

Southchip SC8547 / SC8547A is a 2:1 charge pump (CP).  On giulia there
are two of them (master + slave) between the USB VBUS (PPS, ~2×Vbat)
and the battery.  The driver talks to them over I2C on CCI/Geni buses
and currently exposes a bare sysfs interface.

Key registers used today (see `sc8547.h`):

| reg | meaning |
|-----|---------|
| 0x00 | BAT_OVP |
| 0x02 | VAC_OVP |
| 0x04 | VBUS_OVP |
| 0x05 | IBUS_OCP |
| 0x06 | status (CP_SWITCHING_STAT) |
| 0x07 | control (REG_RESET, CHG_EN) |
| 0x09 | mode: WATCHDOG[2:0], CHARGE_MODE[7] (SC vs bypass), IBUS_UCP_RISE |
| 0x0C | VAC_OK force |
| 0x0F | interrupt flag |
| 0x11 | ADC enable |
| 0x13/0x15/0x17/0x19/0x1B/0x1F | ADC: IBUS / VBUS / VAC / VOUT / VBAT / TDIE |
| 0x36 | chip id; **reading it also refreshes the watchdog** |

## 2. Problems with the current driver

1. **Watchdog disabled.**  `REG_09 = 0x10` (SC) / `0x90` (bypass) clears
   the 1 s watchdog because reliable feeding was never implemented
   (`kick_dog_work` exists but is compiled out via `SC8547_WDT_ENABLE 0`).
   Without it a hung AP leaves the CP driving into the battery.
2. **Slow to start.**  Enable/disable is driven by
   `auto_enable_worker`, a **1 Hz** poll of `POWER_SUPPLY_PROP_VOLTAGE_NOW`
   on `qcom-battmgr-usb` with fixed 8 V / 6 V thresholds.  Worst case
   ~1 s delay after PPS comes up ("插电有时很慢才能开启快充").
3. **Thresholds ignore real PD state.**  It keys off raw VBUS only, so
   PD renegotiation/overvoltage transients can toggle the CP.
4. **Blocking sleeps in the control path.**  `sc8547_set_chg_enable()`
   does `msleep(50)` before enabling and `msleep(100)` + readback after;
   called from a workqueue/sysfs, this is fragile.
5. **No `power_supply`.**  Only vendor sysfs; nothing for a charger
   daemon (or SVOOC) to coordinate with.
6. **Silent errors.**  `sc8547_init_device()` ignores write failures.
7. **No master/slave coordination.**  Needed for balanced dual-pump
   SVOOC later.

## 3. Target architecture

```
             +---------------------------+
 USB psy --->| sc8547 usb notifier      |  event-driven, no polling
 (type/volt) | (power_supply_reg_notifier)|
             +-------------+-------------+
                           | schedule immediately
                           v
             +---------------------------+
             | state machine work        |
             |  OFF -> CONFIG -> ON      |
             |       -> FAULT            |
             +------+--------+-----------+
                    |        |
             I2C ---+        +--- power_supply (sc8547-psy)
             (regs)                status/online/current_now/...
                                   + WDT kick timer (<1s)
```

### 3.1 Event-driven state machine

* Register a `notifier_block` with `power_supply_reg_notifier()`.
* On any change of the USB psy, read `POWER_SUPPLY_PROP_USB_TYPE`,
  `ONLINE`, `VOLTAGE_NOW` once, and feed the state machine.  Schedule
  the state worker immediately (no 1 s tick).
* States:
  * `OFF`        — CP disabled, ADC off.
  * `CONFIG`     — program SC or bypass profile for the detected source.
  * `ON`         — CHG_EN set, WDT kick timer running, ADC on.
  * `FAULT`      — I2C / OVP / WDT fault; retry with backoff.
* SC mode for PPS (VBUS ~2×Vbat); bypass for the fixed high-voltage
  (VOOC/SVOOC) case.

### 3.2 Watchdog

* Program `REG_09` with WATCHDOG_1S (`0x13` SC / `0x93` bypass) instead
  of WATCHDOG_DIS.
* Feed it with a `delayed_work` every 500 ms (reading `REG_36` both
  refreshes the counter and re-checks the chip id), only while in `ON`.
* On an interrupt with a WDT/OVP flag, drop to `FAULT`, disable, and
  re-arm after the source settles.

### 3.3 power_supply interface

Register an `sc8547-psy` exposing at least:

| prop | direction | notes |
|------|-----------|-------|
| `STATUS` | ro | Charging / Not charging / Fault |
| `ONLINE` | ro | CP enabled |
| `VOLTAGE_NOW` | ro | VOUT |
| `CURRENT_NOW` | ro | IBUS |
| `INPUT_CURRENT_LIMIT` | rw | IBUS OCP |
| `VOLTAGE_MAX` | rw | VBUS OVP headroom |
| `CONSTANT_CHARGE_CURRENT` | rw | max IBUS target (SVOOC gates here) |

The existing sysfs attrs (`enable`, `mode`, ADC reads, `dump`) stay for
debug but stop being the control path.

### 3.4 DT

Keep `southchip,sc8547`; move all tuning constants out of `#define`s into
properties (with the current values as defaults):

```
ovp_reg, ocp_reg, oplus,pps_ocp_max
southchip,master;                     /* role */
southchip,wdt-timeout-ms = <1000>;
southchip,sc-vbus-min-mv = <8000>;
southchip,vbus-ovp-mv   = <12000>;
```

## 4. SVOOC / dual-pump roadmap

* Model the two pumps as one logical charger (master owns the state
  machine, slave follows) or as two `power_supply`s linked via
  `power_supply_get_by_name` + `supplied_to`.
* Current balancing: read both IBUS ADCs, trim one pump's target.
* SVOOC negotiated VOUT is set by the ADSP/PPS layer; the driver only
  enforces OVP/OCP and balance.

## 5. Migration plan

1. **Event-driven + WDT** (highest value, self-contained):
   add the notifier + state machine, enable the watchdog, delete
   `auto_enable_worker`.  Regression test: plug/unplug PPS, repeated.
2. **power_supply interface** + move sysfs control to it.
3. **DT bindings** for the constants; drop `#define`s.
4. **master/slave coordination** + balance, as SVOOC lands.

## 6. Test plan (each step)

```
cat /sys/class/power_supply/qcom-battmgr-usb/{usb_type,online,voltage_now}
cat /sys/class/power_supply/sc8547-psy/*          # after step 2
cat /sys/bus/i2c/devices/*/dump                   # register dump
cat /sys/bus/i2c/devices/*/{enable,ibus,vout,tdie}
dmesg | grep -i sc8547
```

Check: PPS plugged -> CP `ON` within a few ms (not ~1 s); unplug ->
`OFF`; WDT armed (`REG_09[2:0]=3`); TDIE sane; no OVP/OCP faults over a
long charge; both pumps balanced.

## 7. Safety notes

The CP drives the battery directly; a bad register sequence or a stuck
CHG_EN can over-current the cell.  Keep the watchdog on, always disable
on `remove`/`shutdown`, and never enable the CP without an active valid
source.

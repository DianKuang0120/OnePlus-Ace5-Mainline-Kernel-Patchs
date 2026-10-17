# C-PHY bring-up log for the IMX906 on the sm8650 (2026-10-17)

Companion to `FINDINGS.md` (which covers the register capture).  This records
what had to be done to make mainline camss talk to a C-PHY sensor, and where it
currently stands.

## What was wrong, in order

1. **The bus type was never read.** `camss_parse_ports()` walks the *camss*
   device's endpoints and calls `camss_parse_endpoint_node()` on them, so the
   bus type comes from `&camss/ports/port@N` - **not** from the sensor's
   endpoint.  Worse, mainline rejected anything but D-PHY outright.  Symptom:
   `bus_type=5` (D-PHY) in the probe log, `is_cphy=0`, everything C-PHY dead.
   Fix: accept `V4L2_MBUS_CSI2_CPHY` and put `bus-type` on the camss port
   endpoint (commit 836871e).

2. **The C-PHY lane register set.** `lane_regs_sm8650[]` is byte-for-byte the
   OPLUS *2ph* (D-PHY) table; the OPLUS tree also has `csiphy_3ph_v2_2_0_reg`.
   `csiphy_gen2_config_lanes()` now picks `lane_regs_sm8650_cphy[]` when
   `is_cphy` (d7d73ed).

3. **The lane-enable encoding.** CTRL5 (0x1014) is the same register, but C-PHY
   has no clock lane and uses `BIT(pos * 2 + 1)` - 0x2a for three trios, which
   is exactly what Android reported (`lane_enable: 0x2a`).  mainline wrote the
   D-PHY value 0x95 (03edda4, 85e353f).

4. **The reset-exit value.** The OPLUS reset-exit table is PHY-type dependent:

       {0x1000, 0x02, 0x000, CSIPHY_2PH_REGS}     /* D-PHY */
       {0x1000, 0x0e, 0xbe8, CSIPHY_3PH_REGS}     /* C-PHY: 0x0e + ~3 ms */

   mainline wrote 0x00 unconditionally, i.e. left the C-PHY lanes off
   (e42e04c).

5. **The PHY clock.** The downstream DT runs the shared `cphy_rx_clk_src` at
   400 MHz (lowsvs) / 480 MHz (nominal); mainline's sm8650 csiphy resource
   listed only 400 MHz, and `csiphy_set_clock_rates()` picks from that list
   (it errors with "Pixel clock is too high for CSIPHY" once link_freq / 4
   exceeds it).  The camcc table does have 480 MHz, so list more rates
   (b859e16).

6. **The data-rate-specific AFE registers.** OPLUS carries
   `datarate_220_<X>Gsps_enhance_giulia` tables.  Android reports
   `datarate 7471533333`, i.e. 7.47e9 / 2.28 bits-per-symbol = **3.28 Gsym/s**
   -> `datarate_220_3p3Gsps_enhance_giulia`, which holds the C-PHY analogue
   front end (CDR, equaliser, termination) plus the settle registers
   (0x020C/0x0208 and friends).  Appended to `lane_regs_sm8650_cphy[]`
   (8d2277c).

After all of the above the PHY registers read back exactly like the OPLUS
tables:

```
lane_en 0x1014=0x2a   0x02f0=0xef  0x0264=0x01  0x0244=0xb2  0x02bc=0xd0
```

## Where it stands

The pipeline starts (STREAMON succeeds, the sensor's init and stream-on tables
go out) and the C-PHY receiver now matches the OPLUS tables register for
register:

```
ctrl0=0x0e  lane_en 0x1014=0x2a  0x101c=0x7a  0x1018=0x01  0x1084=0  0x108c=1
AFE: 0x0268=0xf1 0x0278=0x14 0x0288=0x20 0x026c=0x2d 0x028c=0x37 0x0270=3 0x020c=0x23
3ph: 0x02f0=0xef 0x0264=1 0x0244=0xb2 0x02bc=0xd0 0x0260=0xa8
```

and the **sensor is confirmed alive and streaming**:

```
after stream-on: chipid=0x0906(rc0) 0x0100=0x01(rc0) 0x0111=0x03(rc0)
```

but the CSIPHY still reports **no activity at all** - both right after the lane
configuration and again at stream stop:

```
status 0x0340=0  lanes 0x0358=0 0x0758=0 0x0b58=0  common_status 0x10b0=0
```

A receiver that is merely mis-tuned normally raises errors here, so a
completely silent PHY with a live transmitter is the thing to explain.

### Two more register values that were wrong (423fb06)

Comparing `csiphy_common_reg_2_2_0` with what mainline writes:

| register | OPLUS (C-PHY) | mainline was | now |
|---|---|---|---|
| CTRL0 (0x1000) | 0x0e (reset-exit) | written 0x00 in lanes_enable, undoing it | 0x0e |
| CTRL7 (0x101c) | 0x7a | 0x02 (the D-PHY value) | 0x7a |
| CTRL6 (0x1018) | 0x01 | 0x01 | ok |
| 0x1084 / 0x108c | 0x00 / 0x01 | added | ok |

## The decisive next step: read the vendor's PHY registers

The OPLUS csiphy driver can dump its own registers, and it is exposed through
debugfs:

```c
debugfs_create_bool("en_common_status_reg_dump", 0644, ...)
```

so on stock Android (root adb) one can

```sh
find /sys/kernel/debug -name 'en_*status_reg_dump'
echo 1 > .../en_common_status_reg_dump
echo 1 > .../en_lane_status_reg_dump      # if present on this revision
# open the main camera, then
dmesg | grep -i csiphy
```

and get the PHY's own view of itself while a working link runs.  Diffing that
against the values printed by `csiphy_lanes_enable()`/`_disable()` in mainline
above should show exactly which register still differs.

## Other hypotheses, in order

1. **Something in the PHY is not programmed at all** - the OPLUS driver writes
   several more tables (`csiphy_irq_reg`, `bist_setting`, the per-phy
   `csiphy_v2_2_0` block).  The debugfs dump answers this directly.
2. **C-PHY trio line order.** The video-interfaces binding encodes ABC/ACB/...
   through `lane-polarities`; the DT has none, so mainline assumes ABC.  The
   OPLUS HAL passes the order explicitly.
3. **PHY clock rate.** `csiphy_set_clock_rates()` now lists 400/480/560/640/800
   MHz and asks for 480 (the vendor's nominal); worth logging the value that
   `clk_set_rate()` actually lands on.
4. **The transmitter is quiet after all.** Reading 0x0100 back only proves the
   mode register; the vendor's stream-on block ends with `0x0b8e..0x0b95`
   (0x01 0x00 0x02 0x1e 0x01 0xfa 0x01 0x00) which we replay verbatim, and the
   `0x0301` write reads back 0x05 rather than the written 0x25 - worth a second
   look at the capture for that block.

## Useful artefacts

* `decode_cci_words.py`, `gen_tables.py` - CCI word decoder and table emitter.
* `imx906_full_decoded.txt` - the full 3685-write sequence.
* `/tmp/opencode/run.sh` - the pipeline setup + capture + dmesg helper used for
  every test (media-ctl links, formats, 10 s capture, dmesg tail).
* CSIPHY register reads are printed by the driver itself
  (`csiphy_lanes_enable` / `csiphy_lanes_disable`); with camss built in,
  `/dev/mem` is blocked, so the driver is the way to see them.

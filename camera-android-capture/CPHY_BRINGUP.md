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
go out, CSI_SIGNALING_MODE reads back 0x03 = C-PHY, lane count 0x02 = 3 trios)
but **the CSIPHY sees no activity at all**:

```
status 0x0340=0  lanes 0x0358=0 0x0758=0 0x0b58=0  common_status 0x10b0=0
```

Zero on every status/error register, read both right after the lane config and
again at stream stop.  That points at the *transmitter* rather than the
receiver (a mis-settled receiver would normally raise errors, not stay silent).

## Next steps

1. **Confirm the AFE writes landed** - the stream-stop dump does not include
   0x0268/0x0278/0x0288/0x026C/0x028C or 0x020C yet; add them.
2. **Prove whether the sensor transmits.** The sensor's own state can be read
   back over CCI (0x0100 should be 1).  A cheap cross-check: put the imx355's
   *known-good* D-PHY link through the same exercise with the 2ph data-rate
   AFE table - if D-PHY + AFE works there, the PHY side is fine.
3. **D-PHY is worth one more try with the AFE registers.** The earlier D-PHY
   attempts (0x0111 = 0x02, 3 and 4 lanes) predate the data-rate tables and
   therefore ran the PHY without its front-end tuning.
4. **Re-capture the CCI stream** if the sensor turns out to be silent - the
   per-frame AE region of the old capture is the least trustworthy part of the
   table, and the QSC/OTP blobs were deliberately skipped.
5. The CSID has no C-PHY handling in mainline (only C-PHY packet/error
   counters in the OPLUS driver), so it is a suspect only once the PHY reports
   activity.

## Useful artefacts

* `decode_cci_words.py`, `gen_tables.py` - CCI word decoder and table emitter.
* `imx906_full_decoded.txt` - the full 3685-write sequence.
* `/tmp/opencode/run.sh` - the pipeline setup + capture + dmesg helper used for
  every test (media-ctl links, formats, 10 s capture, dmesg tail).
* CSIPHY register reads are printed by the driver itself
  (`csiphy_lanes_enable` / `csiphy_lanes_disable`); with camss built in,
  `/dev/mem` is blocked, so the driver is the way to see them.

# giulia camera

A small phone-style camera app for the OnePlus Ace 5 / 13R ultrawide
(Sony IMX355) running mainline Linux.

```
./giulia-camera
```

* live preview (half-resolution, from the four Bayer phases)
* auto exposure (95th percentile metering) and grey-world auto white balance
* **Space** or the ◉ button: full-resolution still
* stills land in `~/Pictures/giulia/giulia-YYYYmmdd-HHMMSS.{png,jpg}`

Requires `PySide6`, `numpy`, `v4l-utils` and the mainline `imx355` driver; the
media pipeline is configured automatically (and the main sensor's
CSIPHY1 -> CSID0 link is dropped first, since CSID0 is shared).

The sensor is accessed through its standard V4L2 controls, so the app works
with any giulia kernel that has camss + imx355 working.

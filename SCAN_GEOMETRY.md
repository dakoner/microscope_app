# Scan geometry: camera axes, stage axes and pixel scale

The mosaic scan lays camera frames onto a canvas measured in **commanded stage
millimetres**.  Three separate quantities control that layout and they are easy
to conflate.  The measurements below were made on the Windows rig driving the
headless camera/stage server on `gork` (2026-11-22).

## 1. Camera axes are square to the stage axes

Measured by cross-correlating full camera frames taken before and after a
commanded stage step (illumination pattern removed with a high-pass filter
first, so only specimen features drive the correlation):

| Commanded move | Measured image shift |
| --- | --- |
| `+0.10 mm` X | `+80 px` along image x, `0 px` along image y |
| `+0.10 mm` Y | `0 px` along image x, `+87 px` along image y |

Both correlations had a single sharp minimum (SAD 1.7) against a flat baseline
of 7, so the direction is unambiguous.  There is **no rotation and no
cross-coupling**: commanded X moves the image along image x, commanded Y along
image y.

Earlier code rotated every scan frame by 90 degrees and swapped the FOV width
and height.  That was wrong and is what produced the diagonal staircase inside
each strip of the composite.  The rotation is now a setting
(`scan_camera_rotation`, 0/90/180/270, default 0) for rigs whose camera really
is mounted turned.

## 2. Two different pixel scales

* **Optical scale** — image pixels per millimetre of specimen.  This is what the
  ruler calibration measures and what the GUI shows.
* **Stage scale** — millimetres the stage actually travels per millimetre it is
  commanded to travel.  FluidNC reports commanded position, so nothing in the
  software can see this directly.

For stitching, only the **product** matters: image pixels per *commanded* stage
millimetre.  Measured on this rig:

| Axis | px per commanded mm | configured `ruler_calibration` | ratio |
| --- | --- | --- | --- |
| X | 800 | 400 | 2.00 |
| Y | 870 | 400 | 2.175 |

The X and Y numbers differ by 8.75 %, which cannot come from the optics (a
lens is isotropic), so a mechanical scale error definitely exists.  The common
factor of ~2 could come from either side:

* If the optics really are 800 px/mm, the configured 400 is a calibration
  mistake and the Y axis over-travels by 8.75 %.  The automatic ruler detector
  assumes a slide with **100 minor divisions per millimetre**
  (`pixelsPerMm = medianTickSpacingPx * 100.0`).  A slide with 0.02 mm minor
  divisions would give exactly half the true value, i.e. 400 instead of 800.
* If the optics really are 400 px/mm, then both axes travel about twice as far
  as commanded, which points at FluidNC steps/mm (a microstepping setting
  mismatch gives exactly 2x).

Settle this with `$$` in the CNC panel (X/Y steps/mm vs motor pitch and
microstepping) or by re-running the ruler calibration on a stage micrometer,
then fix the side that is wrong.

## 3. Settings

| Key | Meaning | Default |
| --- | --- | --- |
| `ruler_calibration` | optical px/mm, used for the composite unless overridden | required |
| `scan_scale_x_px_per_mm` | px per commanded mm along stage X, for stitching | `ruler_calibration` |
| `scan_scale_y_px_per_mm` | px per commanded mm along stage Y, for stitching | `ruler_calibration` |
| `scan_camera_rotation` | camera rotation relative to stage axes | `0` |

The scan scales are the effective values, so they absorb both the optical scale
and any stage scale error.  Set them while the root cause above is unresolved;
once the optics and the mechanics agree, delete them and let the scan fall back
to `ruler_calibration`.

## 4. Why the 2026-11-22 scan looked the way it did

With `ruler_calibration = 400` the code believed the FOV was 3.20 x 2.56 mm and
stepped columns at 75 % of that, a 1.92 mm pitch.  The true FOV at 800 px/mm is
1.60 x 1.18 mm, so every column boundary was separated by a 0.32 mm gap with no
data at all — the hard seams at x = 767, 1535, ... in `scan_composite.tif`.
The 90 degree rotation then turned the ~58 px of per-frame Y motion into lateral
drift, which is the staircase.

## 5. Measurement notes

* The specimen is quasi-periodic (autocorrelation peaks near 76/150/274 px along
  image x and 238-244 px along image y), so test steps must stay below ~76 px,
  i.e. **<= 0.09 mm**, or the correlation aliases.
* Subtract the fixed illumination vignette before correlating
  (`g - GaussianBlur(g, (0,0), 40)`) and compare bright frames with bright
  frames; scene brightness varies by an order of magnitude across the stage.
* Reusable scripts live in the scratch directory: `cmp2.py` (high-pass FFT + SAD
  scan) and `alias.py` (checking for aliased minima).

## 6. Verification of the fix (2026-11-23)

Short scan, region x 25.50-29.50 / y 28.60-31.60 mm, feedrate 50 mm/min,
serpentine, 239 frames in 3 columns
(`videos/scan_region_01_1795450185`).  Metadata confirms the fix is live:
`fov_mm x 1.60 / y 1.177`, `step_x_mm 1.2`, `planned_rows 3`,
`scan_scale_x_px_per_mm 800`, `scan_scale_y_px_per_mm 870`,
`scan_camera_rotation_deg 0`.

Measured with the same metrics on both composites:

| metric | broken scan | fixed scan |
| --- | --- | --- |
| composite size | 8000 x 4400 px | 3200 x 2610 px = exactly 4.0 x 3.0 mm at 800/870 |
| coverage | 100 % but 0.32 mm gaps at every column boundary | 100 %, 640 px of real overlap, 2 px short at one edge |
| `mean abs(dI/dx)` per column at the joins | 26-43x baseline | 4-5x baseline |
| high-pass energy at the joins | 25x baseline | 1.6x baseline |
| width of strong vertical edges in the overlap bands | doubled | 1.00 px in every band |

No gaps, no staircase, no edge doubling: the geometry is correct.

What is left is **photometric, not geometric**.  At each blend entry
(x = 960, 1920) the composite steps by about -1.2 grey levels, consistently in
one direction, with no feature doubling.  Cause: the mosaic blends the overlap
50/50 with a hard weight change, and at the entry to the overlap the
neighbouring column contributes its outermost, heavily vignetted pixels.  Fixing
that means distance-weighted blending or flat-fielding, not more geometry work.

## 7. Coordinate frames after a controller reset

Machine coordinates only mean something after homing.  A controller reset sets
`MPos` to 0,0,0 at whatever physical position the stage happens to be, so every
coordinate recorded before the reset becomes meaningless.  Re-homing re-establishes
the frame from the switches, but any region chosen from the pre-reset frame will
point somewhere else - the verification scan above landed on a blank field for
exactly this reason.  After a reset: home, then re-locate the specimen before
trusting any stored region.

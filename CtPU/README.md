<!-- Last edited: 2026-08-14 09:26:52 UTC -->

# Ctpu-bintape-timechannel

Software for Instrustar / Hantek USB oscilloscopes (ISDS205B, Hantek 6022 and
relatives), built around three ideas that ordinary scope software does not
provide:

| | |
|---|---|
| **CtPU** | *Conversion to Physical Units.* A channel stops being "volts" and becomes what the sensor actually measures — °C, kPa, kg, pF, A. Two reference points define `k` and `b`; screen, measurements and export all follow. |
| **BinTape** | A chart-recorder acquisition model. A bin is a stretch of **tape** — a position in record order — not a signal value. Nothing is ever erased, the point count is exactly what was asked for, and the recording length need not be known in advance. |
| **TimeChannel** | Time as a **first-class physical channel**, not as context. It belongs in the axis list beside CH1/CH2, its sensitivity runs ns/div → h/div, and its measured and estimated components are kept apart. |

## Supported target

This project supports **exactly one** configuration:

| | |
|---|---|
| OS | Windows 11 **x64** |
| Toolchain | MSYS2 **MinGW64**, Qt5 |
| Instrument | Instrustar **ISDS205B** |

Other instrument models inherited from OpenHantek6022 (MDSO, DSO-6021/6022,
DDS120) are still compiled and registered — see
[docs/DEVICE_SCOPE.md](docs/DEVICE_SCOPE.md). They are **not tested**, which is
a different statement from "not supported": the code is there and may well work,
but no claim is made about it.

No other *platform* is supported, built or tested. CI builds this target and no other;
Linux, macOS, MSVC and 32-bit configurations were removed rather than left to
rot untested. Inherited cross-platform code still present in `openhantek/` and
`cmake/` has not been stripped — it is simply not exercised. The ISDS205B**W**
variant is FPGA-based and is *not* supported.

The multi-platform build guide for the base application remains available
upstream at [OpenHantek6022][oh].

## Documentation

| Document | What it answers |
|---|---|
| [docs/ENGINEERING_LOG.md](docs/ENGINEERING_LOG.md) | **Start here when hunting a bug.** Symptom → diagnosis → numbers → decision, including rejected alternatives |
| [docs/COMMIT_INDEX.md](docs/COMMIT_INDEX.md) | When a change appeared and which files it touched, across the predecessor repositories |
| [sessions/](sessions/) | **Session journal.** Context does not survive between sessions; every analysis is logged here so the next session starts from what was reached, not from scratch |
| [docs/TMP_INDEX.md](docs/TMP_INDEX.md) | **Map of the `TMP/` staging area** — 262 files sorted by which access stack each fact belongs to, what is directly applicable, what must be moved out, and what has not been read yet |
| [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md) | Every third-party component, its copyright holders and its licence, and the checklist for adding a new one |
| [LICENSES/](LICENSES/) | Full text of every licence in force here |
| [docs/build.md](docs/build.md) | Build, tests, and the WinUSB driver for ISDS205B |
| [docs/CTPU-AXES.md](docs/CTPU-AXES.md) | **Design decision:** CtPU scales axes, not data — streams stay untouched, conversion happens only at export and on-screen measurement |
| [docs/REFERENCES.md](docs/REFERENCES.md) | Published sources behind the algorithms, and what was taken from each |
| [docs/ACCESS_PATHS.md](docs/ACCESS_PATHS.md) | **Which stack a fact belongs to.** The device is reached by two mutually exclusive stacks (vendor `vdso.dll` vs the fx2lafw firmware this repo ships); a number measured on one may mean nothing on the other |
| [measurements/](measurements/) | Primary logs and results from runs against real hardware (path A) — the numbers behind the working notes |
| [references/](references/) | The source documents themselves — patents, vendor publications, the Instrustar SDK, hardware material — with their provenance and rights status |
| [docs/ROADMAP.md](docs/ROADMAP.md) | What is done and what is ahead |
| [docs/PRIOR_ART.md](docs/PRIOR_ART.md) | Origin of the CtPU / CCtPU terminology |
| [testdata/README.md](testdata/README.md) | The real captures the algorithms were checked against |

This repository was started fresh, with no inherited history. See
[docs/COMMIT_INDEX.md](docs/COMMIT_INDEX.md) for where the earlier commits live.

---

## Why this is a separate project

A derivative work of [OpenHantek6022][oh], but **not** a fork intended for
merging back; compatibility with upstream is neither maintained nor sought.

- Upstream does not test on Windows or on ISDS205B — precisely this project's
  target.
- The features here change assumptions running through the whole program
  (physical units as a first-class concept, a chart-recorder acquisition model
  replacing the ring buffer, time as a channel). They were never part of
  upstream's design and are not proposed for it.
- Several upstream behaviours are deliberately changed rather than preserved.

What this project does **not** do is drop attribution — see
[Credits and licence](#credits-and-licence).

---

## What is different

### New capabilities

- **CtPU** — per-channel linear conversion to physical units
  (`Oscilloscope → Settings → CtPU / Math`).
- **CCtPU** — calibration assistance: pick one of 12 live measurements (DC over
  segment or cycle, std-dev, Vmax, Vpp, AC RMS, dB, Top, Base, Amplitude,
  Overshoot±) and capture it directly into the Zero/Span fields instead of
  reading the screen and typing. Manual entry from a datasheet stays available.
- **Math Stack** — four virtual math channels (M1–M4), each with its own
  operation, sources and physical unit.
- **Multi-curve XY** — up to four independent XY curves over arbitrary channel
  pairs, math channels included.
- **XY continuous recorder** — long-duration pen-plotter recording.
- **Median pre-filter** — removes switching spikes and ringing before binning,
  with a full energy account of what was removed.

### Corrections to inherited behaviour

- **Startup crash** on an empty unit cache — the cache was removed outright
  rather than its initialisation patched.
- **XY decimation cascade** used a hardcoded CH1/CH2 pair instead of the
  channels actually assigned to the curve.
- **XY X-offset** used the T-Y trigger-position formula instead of the X
  channel's own offset.
- **Vertical offset slider range.** Where offset sets the input stage's
  operating point, the trace cannot leave the screen — hence the traditional
  half-screen limit. Here the operating point is wired to GND and only the
  *image* moves, so valid, unclipped data could sit off-screen unreachable:
  visible on AC coupling, on math channels, and on any CtPU-scaled channel.
  Travel is now three screen heights — enough to put the lowest level against
  the top edge and the highest against the bottom.
- **Ring buffer replaced** (below).
- GUI fixes: settings-page icon, XY preview label, a hardcoded untranslated
  string, CSV export timestamp.

### Why the ring buffer had to go

Diagnosed from the code and from a real capture:

1. **Steps on a physically linear ramp.** The cascade emitted a point every
   *N* samples and reset; the remainder hung in the stage until the *next*
   frame, so one output point averaged data torn apart by the USB frame gap.
   The step period was the frame period — nothing in the signal.
2. **The point count was not honoured.** Decimation quantised to integer powers
   of the cascade base (8, 64, 512, 4096…): a request for 2000 points yielded
   977, 1953 or 488 — up to 8× off. A real capture asking for 2000 gave 6689.
3. **Slew rate was a prediction, not a measurement**, and one number cannot
   describe different rise and fall rates.
4. **Data was erased** — on overflow the front of the recording was dropped.

BinTape fixes all four by binning along the tape rather than along the signal,
and by merging adjacent bins when they run out instead of discarding anything.
Verified on that capture: 419 bins within the 500 requested, 6689 of 6689
samples retained, the oldest section coarser rather than missing.

### Measurement, not classification

A bin does not claim to know what the signal was doing. It reports facts —
`path` travelled, `net` displacement, `min`/`max` — and `trust = path/net`:

- `trust ≈ 1` — traversed directly; mean and RMS are meaningful.
- `trust ≫ 1` — loops or jitter inside; the mean does **not** characterise the
  bin, and that is visible rather than concealed behind a plausible number.

Splitting accumulators into "forward" and "reverse" branches was considered and
**rejected**: it imposes a simple up-down sweep model, so with zigzags or
nested minor loops it would yield two tidy, convincing columns of fiction.
`min`/`max` keep a loop visible as a band of real extent even after deep
merging.

### Energy is conserved

The median pre-filter is nonlinear, so energy does **not** split into
"kept + removed". The exact identity is

```
Σx² = Σy² + Σr² + 2Σ(y·r)
```

with `y` the output and `r = x − y` the residual. All three terms are exported
per channel, so recorder data serves RMS and power work without an unexplained
shortfall. The cross term is mandatory: omitting it looks like missing energy
when nothing is missing.

**Operational warning found this way:** on a real capture, a median window ≥ 31
clipped the *tips* of sharp peaks — peak residual reached the full signal span
— while the residual *energy fraction* stayed at 0.0007% and showed nothing.
Judge by `resPeak`, not by energy fraction; windows ≤ 15 were safe there. This
matches Teledyne LeCroy's warning that heavy boxcar decimation can leave "only
one sample on the edge" [[3]](#ref3).

---

## Grounding in established practice

Design decisions are checked against published work from recognised instrument
makers rather than invented locally. Sources are listed so any claim below can
be verified independently.

- **Enhanced-resolution formulas.** Tektronix publishes, for HiRes mode,
  `Enhanced Resolution Bits = 0.5·log2(D)`, a digital boxcar filter on the
  decimated acquisition, and −3 dB bandwidth ≈ 0.44 × sample rate
  [[1]](#ref1) [[2]](#ref2). The formulas in `movingaverage.h`
  (`effectiveBitsGained`, `cutoffHz` at 0.443·SR/N) match these — they are
  cited there, not presented as a local derivation.
- **Bits and bandwidth as indication, not controls.** The Tektronix 5 Series
  MSO shows resulting vertical bits and −3 dB bandwidth as badges while
  exposing only the acquisition control [[2]](#ref2). The planned Digital Zoom
  UI follows the same principle: one control, the consequences displayed
  read-only.
- **Boxcar is not optimal.** Teledyne LeCroy notes that a boxcar reduces
  bandwidth more than a shaped filter of the same length, which is why ERES
  uses linear-phase FIRs with better step response [[3]](#ref3) [[4]](#ref4).
  Recorded as a deliberate simplicity trade-off and a clear improvement path,
  not hidden.
- **Per-axis resolution.** The Tektronix digital-phosphor database keeps a
  cell per display pixel, with intensity accumulating where the waveform passes
  most often [[5]](#ref5) — i.e. resolution comes from the display grid per
  axis, not from one scalar point count. This is the basis for planned
  per-axis binning, and the same structure yields a DPO-style heat map.
- **Vertical framing conventions.** Eight vertical divisions, a ±4·VDIV range
  and 256 codes for an 8-bit ADC, with quantisation treated as noise
  [[6]](#ref6).
- **1-2-5 gain ladder.** Standard logarithmically even spacing for vertical
  ranges [[7]](#ref7); `TimeChannel`'s ns/div → h/div ladder follows the same
  rule, with the seconds-to-hours region following the time scale instead of
  decimal decades.
- **Analogue vernier.** On instruments with a continuously variable fine gain,
  the signal is trimmed to fill the screen *before* digitising [[7]](#ref7).
  This hardware has fixed gain steps and cannot do that, which is why
  HiRes-style averaging is the applicable answer to ADC under-utilisation here
  rather than an optional extra.

### References

<a id="ref1"></a>[1] Tektronix — *Increase in bits of resolution for HiRes mode* (FAQ).
<https://www.tek.com/en/support/faqs/i-could-not-find-any-information-regarding-increase-bits-resolution-hires-mode-there-an>

<a id="ref2"></a>[2] Tektronix — *Tools to Boost Oscilloscope Measurement Resolution to More than 11 Bits* (application note).
<https://www.tek.com/en/documents/application-note/tools-boost-oscilloscope-measurement-resolution-more-11-bits>

<a id="ref3"></a>[3] Teledyne LeCroy — *Differences Between ERES and HiRes* (application note).
<https://cdn.teledynelecroy.com/files/appnotes/differences_between_eres_and_hires.pdf>

<a id="ref4"></a>[4] Teledyne LeCroy — *LAB 767: ERES vs. Boxcar Averaging* (application brief).
<https://cdn.teledynelecroy.com/files/appnotes/lab767.pdf>

<a id="ref5"></a>[5] Tektronix — *Oscilloscope Types* (primer; digital phosphor database).
<https://www.tek.com/en/documents/primer/oscilloscope-types>

<a id="ref6"></a>[6] US Patent 10,534,019 — *Variable resolution oscilloscope*
(±4·VDIV framing, 256 codes, quantisation noise; also cites M. McTigue and
P. Byrne, "An 8-gigasample-per-second, 8-bit data acquisition system for a
sampling digital oscilloscope", *Hewlett-Packard Journal*, Oct 1993, pp. 11–23,
for vertical interleaving).
<https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10534019>

<a id="ref7"></a>[7] *Analog Oscilloscope* — overview of the 1-2-5 vertical
ladder and the variable/vernier fine gain control (ScienceDirect topic page).
<https://www.sciencedirect.com/topics/engineering/analog-oscilloscope>

---

## Building (Windows 11, MSYS2 MinGW64)

Use the **MSYS2 MinGW x64** shell (not "MSYS2", not the x86 shell):

```bash
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
          mingw-w64-x86_64-qt5-base mingw-w64-x86_64-libusb mingw-w64-x86_64-fftw

gcc -dumpmachine          # must print x86_64-w64-mingw32

git clone https://github.com/dtba3a-del/-Ctpu-bintape-timechannel-.git
cd -Ctpu-bintape-timechannel-
mkdir build && cd build
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug ..
mingw32-make -j$(nproc)
file openhantek/CtpuBintapeTimechannel.exe    # must say PE32+ ... x86-64
```

`mingw32-make` carries that name in every MSYS2 environment regardless of
target architecture — it is not a 32-bit build.

If you unpack a source archive instead of cloning, delete any `.git` directory
first: CMake reads the version through `git describe`, and a `.git` belonging
to another repository makes configuration fail.

Tests: `ctest --output-on-failure` — unit tests for CtPU, MathStack, the
processing pipeline, MovingAverage, BinTape/MedianFilter and TimeChannel.

The quickstart above installs `qt5-base` and builds against a dynamically
linked Qt. CI instead installs `qt5-static` and passes
`-D CMAKE_PREFIX_PATH=/mingw64/qt5-static`, which yields a standalone `.exe`.
Both work; pick by whether you want a redistributable binary.
[docs/build.md](docs/build.md) documents the CI variant and the WinUSB driver
setup, and is the reference when a local build disagrees with CI.

## Firmware

`openhantek/res/firmware/isds205b-firmware.hex` is a corrected ISDS205B
firmware derived from **fx2adc** by Steve Markgraf, converted and renamed for
this application and validated on real hardware. It replaces stock firmware
that reports wrong amplitudes on the 100/50/20 mV ranges. The ISDS205B**W**
variant is FPGA-based and is *not* supported.

For diagnostics and first-run bring-up on a clean machine, the fx2adc binaries
plus Zadig (for the WinUSB/libusb driver) are useful; a packaged pre-release
exists for that purpose.

---

## Known limitations

- Frame stitching in T-Y and classic XY remains unresolved. (An earlier draft
  referred to a `TECH_DEBT_frame_stitching.md`; that file was never committed,
  so the open question is recorded here instead.)
- DPO heat-map rendering is designed but not implemented.
- PRE-CtPU gain self-calibration against the built-in calibrator is not
  implemented. Per-range `(k, b)` storage already exists and is already
  applied; what is missing is the measurement loop, which needs live hardware
  to develop against.
- Pre-trigger averaging is wired in but left at N=1 (disabled) pending tuning
  against real noisy signals.
- Per-axis bin counts and time-as-master-axis binning are designed but not yet
  implemented. `TimeChannel` is the tested foundation, not yet integrated into
  the recorder or the UI.

---

## Credits and licence

Licensed under the **GNU General Public License v3** ([LICENSE](LICENSE)).
This is a derivative work; the original authors' copyright notices are
preserved in the sources and in the About dialog, as the licence requires.

Full licence texts for every licence below are in [LICENSES/](LICENSES/).

| Component | Copyright | Licence |
|---|---|---|
| [OpenHantek6022][oh] — base application | © 2010, 2011 Oliver Haag; © 2012– the OpenHantek community; maintainer Martin Homuth-Rosemann | GPL-3.0 |
| [Hantek6022API](https://github.com/Ho-Ro/Hantek6022API) — FX2 firmware | Ho-Ro and contributors | GPL-3.0 |
| [fx2adc](https://github.com/steve-m/fx2adc) — ISDS205B firmware, FX2 streaming | © 2012–2024 Steve Markgraf; © 2012 Dimitri Stolnikov | GPL-3.0-or-later |
| [sigrok-firmware-fx2lafw](https://sigrok.org/wiki/Fx2lafw) — alternative FX2 firmware | The sigrok contributors | GPL-2.0-or-later |
| [libsigrok](https://sigrok.org/wiki/Libsigrok) — instrument access layer | The sigrok contributors | GPL-3.0 |
| [FFTW](https://www.fftw.org/) — FFT | © 2003, 2007–2014 Matteo Frigo; © 2003, 2007–2014 Massachusetts Institute of Technology | GPL-2.0-or-later |
| [libusb](https://libusb.info/) — USB access | The libusb contributors | LGPL-2.1-or-later |
| [Qt 5](https://www.qt.io/) — GUI toolkit | © The Qt Company Ltd. and contributors | LGPL-3.0 |
| [UsbGpib](https://github.com/xyphro/UsbGpib) — USB↔GPIB bridge | © 2019 Kai Gossner | MIT |
| [Zadig / libwdi](https://github.com/pbatard/libwdi) — WinUSB driver install | © 2010–2025 Pete Batard | GPL-3.0 (Zadig) / LGPL-3.0 (libwdi) |
| Windows `.inf` drivers under `utils/` | VictorEEV; updated by gitguest0; `fgrieu/` by fgrieu | see [docs/WinDriverLicense.md](docs/WinDriverLicense.md) |
| [Instrustar SDK](https://github.com/instrustar-dev/SDK) — vendor SDK, `vdso.dll` | Instrustar | **not declared** |

Every licence above is compatible with GPL-3.0, except the Instrustar SDK,
which declares none. No declared licence means no permission, so the SDK is
never built, linked or shipped; its archive is kept under
[references/vendor/](references/vendor/) as working material only, because
`vdso.dll` is the sole route to the instrument in Mode A. Storing is not
shipping — see [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).

Material under [references/](references/) carries mixed rights: the US patents
are public documents, the `.url` shortcuts are just addresses, but the
Tektronix / Teledyne LeCroy publications, the reprinted article and the vendor
SDK are **not redistributable**. Obligations attach on distribution, not on
keeping a document at hand — so if this repository is ever published,
`references/appnotes/`, `references/articles/` and `references/vendor/` must
be excluded from what is published. Per-directory status is in
[references/README.md](references/README.md).

Additions specific to this project — CtPU, CCtPU, Math Stack, multi-curve XY,
BinTape, MedianFilter, MovingAverage, TimeChannel and the fixes listed above —
are copyright © 2026 dtba3a-del and released under the same licence.

**[docs/THIRD_PARTY.md](docs/THIRD_PARTY.md) is the complete record**: what
each component is used for, who holds the copyright, under which licence, and
the checklist for adding a new one. Follow that checklist in the same commit
that adds any third-party code, data or binary.

[oh]: https://github.com/OpenHantek/OpenHantek6022

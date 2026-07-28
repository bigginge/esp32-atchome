# Performance review

A standing assessment of where this project's frame budget actually goes, and
which levers are worth pulling. Reviewed 2026-07-28 against `cd5e5d5`.

It is written as a ranked plan rather than a list of observations, because the
ranking is the finding: the largest item by a wide margin is a build-time
default that no amount of render work can compensate for, and several of the
smaller items are only worth doing *after* it. `src/frame_probe.hpp` holds the
measurement history that this builds on; this file holds what to do next.

Where something is inferred rather than measured, it says so. Two claims here
were checked and turned out to be wrong; they are kept in
[Negative results](#negative-results-checked-no-action-needed) rather than
deleted, because knowing not to spend a week on SIMD is worth as much as any
of the positive items.

## The budget, from the numbers already in the tree

`benchPsram()` in `src/main.cpp` measures, with the LCD scanning out:

| Operation | Throughput | Cycles per RGB565 pixel at 240 MHz |
| --- | --- | --- |
| `memset` to PSRAM | 15.2 MB/s | 32 |
| `memcpy` PSRAM → PSRAM | 9.4 MB/s | 51 |
| `blendSpan` (the sweep) | ~3.6 Mpx/s | ~67, *including* the geometry solve |

The panel reads the framebuffer continuously: 928 × 525 clocks at 12 MHz is
24.63 Hz, and 800 × 480 × 2 B at that rate is **18.9 MB/s**. Aggregate
contended throughput is therefore about 37.7 MB/s (18.8 MB/s of memcpy traffic
plus scan-out), of which **half is spent displaying the picture whether or not
anything changed**.

The first row of that table is the whole story. `blendSpan` is ten integer ops
per pixel and it runs at roughly the same speed as a bare `memcpy`. It is not
arithmetic-bound. Nothing in the render path is. Every lever below therefore
either raises the memory-system ceiling or stops consuming it; none of them
make the code compute faster, because the code is not what is slow.

## Tier 1 — the ceiling is probably a build default, not silicon

The precompiled libraries `arduino-cli` hands us are built from
[`esp32-arduino-lib-builder`](https://github.com/esp-arduino-libs/esp32-arduino-lib-builder).
Its `configs/defconfig.esp32s3` is, in full, the following (plus ULP and MAC
settings omitted here):

```
CONFIG_BT_ENABLED=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_SPIRAM=y
CONFIG_LCD_RGB_RESTART_IN_VSYNC=y
```

and `configs/defconfig.common` carries `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`.

**Neither `CONFIG_SPIRAM_SPEED_80M` nor `CONFIG_ESP32S3_DATA_CACHE_LINE_64B`
appears anywhere.** Both fall through to the ESP-IDF defaults, which are
40 MHz and a 32-byte line. That is two independent halvings, and the second one
is specific to this board:

1. **PSRAM at 40 MHz rather than 80 MHz.** A straight 2x.
2. **A 32-byte cache line on an octal bus.** The ESP32-S3 TRM specifies that
   Octal PSRAM in DDR mode transfers in 64-byte wrap bursts. With a 32-byte
   line, every line fill still costs a full 64-byte burst on the wire — we pay
   for 64 bytes and keep 32. Half the effective bandwidth, discarded at the
   bus. This matters here and not on a quad-PSRAM board, and the FQBN in the
   `Makefile` says `PSRAM=opi`.

The measured figures are consistent with this. 37.7 MB/s is about 24% of an
80 MHz octal bus's theoretical 160 MB/s, which would be implausibly bad; it is
about 47% of a 40 MHz bus, halved again by the line-size mismatch, which is
roughly where a real PSRAM controller lands once per-burst latency is counted.

> **Inferred, not measured.** The absent settings are confirmed — the defconfig
> above was read directly. That IDF then defaults them to 40 MHz and 32 B is
> from the Kconfig defaults, not from this board's boot log. Verify before
> acting.

### Verifying it

Both checks are minutes, not hours:

```bash
# The shipped sdkconfig.h is already on disk, next to the precompiled libs.
grep -E "SPIRAM_SPEED|DATA_CACHE_LINE|DATA_CACHE_[0-9]+KB" \
  ~/.arduino15/packages/esp32/tools/esp32-arduino-libs/*/esp32s3/**/sdkconfig.h
```

and the IDF boot log prints the PSRAM speed at INFO level on the 115200 serial
output — look for the `esp_psram: Found 8MB PSRAM device` block.

### Acting on it

The settings are not reachable from `arduino-cli`: the libraries are
precompiled against a fixed sdkconfig, and no `--build-property` can change
them. Routes out, cheapest first:

- **ESP-IDF with arduino-esp32 as a component.** This project uses very little
  Arduino surface — `WiFi`, `Wire`, `ledc`, `Preferences`, `HTTPClient`, plus
  PCA9557, ArduinoJson and LVGL. All of it works as an IDF component, and it
  buys full control of sdkconfig *and* the compiler flags.
- **pioarduino** (the maintained PlatformIO fork) supports `custom_sdkconfig`
  for arduino-esp32 3.x by driving the lib-builder itself.
- There is a `high_perf/` variant in
  [`arduino-esp32-sdk`](https://github.com/esp-arduino-libs/arduino-esp32-sdk)
  worth evaluating before anything more drastic.

If the check above says 40 MHz or 32 B, **this is the 2x-to-4x and nothing
else in this document comes close.** Everything below is scaled by its outcome.

### `-Os`, separately

`CONFIG_COMPILER_OPTIMIZATION_SIZE=y` covers the IDF libraries, and
`platform.txt`'s `compiler.{c,cpp}.flags` puts `-Os` on LVGL and all of `src/`
too. `extra_flags` lands *after* the base flags in the compile recipe, and GCC
takes the last `-O`, so this is a one-line change in the `Makefile`:

```make
--build-property "compiler.cpp.extra_flags=-I$(SKETCH_DIR) -DLV_CONF_INCLUDE_SIMPLE -O2" \
--build-property "compiler.c.extra_flags=-O2"
```

The `c` variant is the one that matters most — LVGL is C. This will not touch
the memcpy-bound stages; it is for the compute-bound ones: the label anchor
solver at ~4.7 ms, `sectorRowSpan`, and LVGL's mask arithmetic. Typically
20–40% on those. Flash is at 1 390 354 B of a 3 MB `huge_app`, so the size
cost is free.

## Tier 2 — stop spending the bus on things that are not the picture

### Lower the panel refresh rate

The panel is driven at 24.63 Hz for a UI that paints 6–12.5 times a second.
Scan-out bandwidth is linear in refresh rate, so dropping `pclk_hz` from 12 to
9 MHz gives 18.5 Hz and hands **4.7 MB/s back to the renderer — about a 25%
increase in the bandwidth actually available for drawing**, from one constant
in `crowpanel_display.cpp`.

The existing note there says the panel flickers *above* ~12–14 MHz. Downward is
unexplored. This is a one-line experiment: change it, look at the screen,
revert if it shimmers.

### Get the VSYNC stall off the critical path

`dispFlush()` presents and then blocks ~18–21 ms in `lcd.waitVsync()`, inside
`lv_timer_handler`, before returning. At ~12.5 paints/s that is ~260 ms per
second of the UI core parked in a semaphore, *serially with rendering*. Two
fixes, which compose:

- **A third framebuffer.** `esp_lcd_rgb_panel` accepts `num_fbs = 3`, and LVGL
  9.3 exposes `lv_display_set_3rd_draw_buffer()` — `buf_3` is handled
  throughout `lv_refr.c`, including in the buffer-sync path. With three buffers
  LVGL always has a free one and the wait largely disappears. Costs 768 KB of
  PSRAM that is currently unused, and one extra frame (~40 ms) of latency,
  which for a radar is irrelevant.
- **`lv_display_set_flush_wait_cb()`** instead of blocking inside `flush_cb`,
  so LVGL defers the wait until it genuinely needs the buffer. Partial on its
  own: `refr_sync_areas()` calls `wait_for_flushing()` near the *top* of the
  next refresh cycle, so this only overlaps the work between the two points —
  the solver and `invalidateDirty()`, perhaps 5–10 ms of the 21. The third
  buffer is the real fix; this is the cheap fraction of it.

## Tier 3 — fewer and cheaper pixels

### RLE the background instead of caching it as a bitmap

The highest-value code change, because the background restore is the dominant
per-pixel cost of *both* frame types — the sweep wedge and the merged content
regions alike.

Look at what `drawBackground()` actually produces: a flat card fill, one
horizontal and one vertical crosshair, four 1-px arcs, four small text captions,
a home dot and "HOME". A typical row is a handful of flat runs plus a dozen
literal pixels — call it 15–25 runs. Encoded once at `renderBackgroundCache()`
time that is roughly 40 KB, small enough to live in internal SRAM.

Restoring from an RLE rather than from `bgCache_`:

- **deletes the 450 KB PSRAM read entirely** — the source becomes internal SRAM;
- **moves the write from the `memcpy` path to the `memset` path**, which the
  table above measures as 1.6x faster per byte;
- frees 450 KB of PSRAM.

It is pixel-identical to the current output, so `tests/host` continues to hold
unchanged — which is what makes it a safe change to a load-bearing path.

### Two tuning constants are stale

`kMaxRegions = 4` carries its own disclaimer: *"Measured at 6-7 aircraft,
before the legend and per-segment culling below cut the fixed part."* The fixed
per-pass cost was then lowered and the sweep that justified 4 was never re-run.
More regions now means less dead area to restore, and dead-area restore is the
expensive part. Re-measure 6, 8 and 12.

The same argument applies to `kSweepBands = 4`. The table in `radar_view.hpp`
shows 6 bands cutting mean dead area from 1.34x to 1.24x, rejected on a
per-pass cost that has since dropped.

### Hoist the transcendentals out of `sectorRowSpan()`

The ESP32-S3's FPU implements neither divide nor square root; both are library
calls. Each sweep step makes ~19 calls per row — one in pass one, eighteen
slices in pass two — and each call does one `sqrtf` plus up to two float
divides:

- `sqrtf(kSweepRadius² - dy²)` is **identical across all 19 slices for a given
  row**. Precompute the disc half-width per row once per range change: a
  480-entry `int16_t` table, 960 bytes, and it never changes per frame.
- `d0x * dy / d0y` divides *per row*, but `d0x / d0y` is constant for the whole
  slice. GCC cannot hoist it at `-Os` without reassociation. Pass reciprocals
  in and it becomes 38 divides per step instead of ~11 000.

Worth roughly **15–20% of the beam's cost** — on the order of 8.5 M of the
~48 M cycles per second it spends. Not a headline, but it is cheap, it is in
the hottest loop, and `test_sweep` already checks this geometry against a
brute-force reference, so a mistake shows up immediately rather than as a
stale pixel six months later.

### Optional: quantise the tail into fewer, wider bands

The tail is expensive precisely because its opacity is a function of angular
distance from the head, so all of it changes on every step. Quantised into a
few *wide* alpha bands instead of 18 thin slices, only the band boundaries
move, and the changed area drops from (tail + step) to (bands × step) — at four
bands, 19° instead of 60°, or roughly **1.5–2x on the beam** net of the extra
sync copies.

Listed as optional because it is a deliberate look change: it reads as a banded
phosphor rather than a smooth ramp. Arguably more faithful to a real PPI, but
that is a call for whoever owns the design, not a free win.

## Tier 4 — parallelism, and the architectural question

### The second core is idle

`networkTask` polls every 5 s and otherwise sits in `vTaskDelay(20)`. Core 1
does every pixel. `compositeBackground()` is raw pixel writes that touch **no
LVGL state at all** — it is the one part of the renderer that is trivially
splittable, and it is also the one part that has to paint on a clock rather
than on a change. Give core 0 half the bands and join on a semaphore.

The caveat is the reason this is Tier 4 and not Tier 1: two cores contending
for a saturated bus buy nothing. This pays *after* Tier 1 lifts the ceiling.

### SIMD is downstream of Tier 1, not a lever of its own

Espressif ships hand-written Xtensa LX7 SIMD blend routines for LVGL in
`esp_lvgl_port`, wired in through LVGL's `LV_USE_DRAW_SW_ASM =
LV_DRAW_SW_ASM_CUSTOM` and `LV_DRAW_SW_ASM_CUSTOM_INCLUDE` hooks (both present
in `lv_conf_template.h` for v9.3.0), including the SWAR trick of handling two
RGB565 pixels per 32-bit word.

Per the budget section, `blendSpan` already runs at memcpy speed. There is no
arithmetic headroom to reclaim, so this is worth doing **after** the bandwidth
ceiling lifts and not before.

### The panel is the constraint

An RGB-parallel panel is arguably the wrong display class for this UI. It
forces a framebuffer in PSRAM and then reads that framebuffer 24 times a
second forever, whether or not anything changed: 18.9 MB/s, half the memory
system, to display a picture that changes ~25 kpx per frame.

A display with its own GRAM on an 8080 or SPI bus (RA8875, ST7796) inverts
that. Only changed pixels are pushed, the panel controller holds the image, and
**the continuous PSRAM scan-out and both 768 KB framebuffers disappear
entirely** — 50 KB of dirty pixels over a 16-bit 8080 bus at 20 MHz is about
1.25 ms. Alternatively the ESP32-P4 has a hardware 2D pixel-processing
accelerator that does blend and fill (which is to say: the beam), two 400 MHz
cores, and a considerably wider memory bus.

Both are hardware changes and neither is a recommendation. The point is that
the constraint this codebase has been engineered against so carefully is a
property of the panel interface, not of the problem.

## Negative results (checked, no action needed)

Kept deliberately. Both of these looked like findings and were not.

**LVGL's double-buffer sync is not re-copying the tail.** In DIRECT mode with
two buffers, `refr_sync_areas()` looked like it should be copying last frame's
whole wedge from the on-screen buffer every frame, on top of the blend that
then overwrites it. It does not: it runs `lv_area_diff()` against the current
frame's invalid areas *before* copying, so only the ~8% residual that the new
wedge does not cover is moved. Verified by reading `lv_refr.c` at v9.3.0.

**The sweep blend is not arithmetic-bound, so SIMD is not the answer today.**
At 51 cycles per pixel for a bare PSRAM `memcpy` and ~67 for `blendSpan`
including its geometry solve, the blend is running at or below copy speed. The
conclusion already recorded in `frame_probe.hpp` — *"there was no
micro-optimisation to find, only fewer pixels to touch"* — is correct, and it
generalises: no hand-vectorising, no `-ffast-math` on the blend, no assembly.
Raise the ceiling or touch fewer pixels.

## The plan, in order

1. **Read `sdkconfig.h` and the boot log** for PSRAM speed and data-cache line
   size. Everything else is scaled by the answer.
2. **Add an internal-SRAM variant of every loop in `benchPsram()`** — same
   shapes, SRAM buffers. That one comparison settles definitively whether each
   stage is bus-bound or compute-bound, and it is the measurement the probe
   harness is currently missing; today that distinction is inferred.
3. `-O2` via `extra_flags`. One line, reversible.
4. Lower `pclk_hz` to 9–10 MHz and look at the screen.
5. If (1) says 40 MHz or 32 B: **move the build to ESP-IDF with Arduino as a
   component.** This is the 2x-to-4x.
6. RLE the background; re-tune `kMaxRegions` and `kSweepBands`; hoist the
   `sqrtf` and the reciprocals out of `sectorRowSpan()`.
7. Third framebuffer.
8. Then, and only then, split `compositeBackground()` across both cores and
   look at SIMD.

## Sources

- [esp32-arduino-lib-builder](https://github.com/esp-arduino-libs/esp32-arduino-lib-builder) — the defconfigs the precompiled libraries are built from
- [arduino-esp32-sdk](https://github.com/esp-arduino-libs/arduino-esp32-sdk) — the shipped libraries, including the `high_perf/` variant
- [esp_lvgl_port](https://deepwiki.com/espressif/esp-bsp/3.1-esp-lvgl-port) and [esp-bsp issue #621](https://github.com/espressif/esp-bsp/issues/621) — ESP32-S3 SIMD blend for LVGL
- [Adding LVGL to an ESP-IDF project](https://docs.lvgl.io/master/integration/chip_vendors/espressif/add_lvgl_to_esp32_idf_project.html)
- `lvgl/lvgl` at `v9.3.0`: `src/core/lv_refr.c`, `src/display/lv_display.h`, `lv_conf_template.h`

# Performance review

A standing assessment of where this project's frame budget actually goes, and
which levers are worth pulling. Written 2026-07-28 against `cd5e5d5`;
**verified against the hardware and the toolchain the same day**, which
corrected two of its claims and confirmed the rest.

What changed on verification, for anyone who read the first version: PSRAM is
already at 80 MHz, so Tier 1's headline 2x does not exist and the prize is
roughly half what was advertised; the `-O2` recipe was backwards and would have
been a no-op; `-O2` done correctly buys ~0% end to end, which is a
*confirmation* of this document's thesis rather than a disappointment. The
budget model also ignores the panel's bounce buffers. Each is marked in place
below rather than quietly edited, on the same principle as the negative
results.

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

> **This model understates scan-out, because it ignores the bounce buffers.**
> `crowpanel_display.cpp` sets `bounce_buffer_size_px = kWidth * 20`. In that
> mode IDF does not hand the framebuffer to the LCD DMA directly — it refills
> an internal-SRAM bounce buffer from PSRAM with a CPU `memcpy` in the DMA EOF
> ISR. So the 18.9 MB/s is a PSRAM read *plus* an SRAM write *plus* core-1 CPU
> time in an interrupt, none of which appears in any probe slot. Two
> consequences: the Tier 4 "the second core is idle" argument is weaker than it
> looks, since core 1 is already absorbing this; and Tier 2's third framebuffer
> may not be reachable at all, because `crowpanel_display.cpp:73-79` already
> carries a fallback for bounce buffers being rejected alongside a *double*
> framebuffer.
>
> It also cuts the other way, in Tier 1's favour. The bounce buffers exist
> because direct PSRAM scan-out underran under contention. If the 64-byte cache
> line lifts the ceiling, dropping them may become possible — an unclaimed
> second dividend of the migration.

## Tier 1 — half the ceiling is a build default; the other half was a wrong guess

> **Checked 2026-07-28, and half of this section was wrong.** It originally
> claimed PSRAM was running at 40 MHz, inferred from the setting being absent
> from the lib-builder defconfig and falling through to the IDF Kconfig
> default. The shipped `sdkconfig.h` was then read directly and says
> `CONFIG_SPIRAM_SPEED_80M`. **We are already at 80 MHz. There is no 2x
> there.** The cache-line half of the claim survived and is confirmed below.
> The lesson is the obvious one: an absent defconfig entry does not mean the
> Kconfig default applies, because lib-builder sets it elsewhere.

What the precompiled libraries were actually built with — read from
`esp32s3-libs/3.3.11/qio_opi/include/sdkconfig.h`, which is the variant our
FQBN (`FlashMode=qio,PSRAM=opi`) selects, and identical in the other variants:

```
CONFIG_SPIRAM_MODE_OCT 1
CONFIG_SPIRAM_SPEED_80M 1              <- already maxed; no lever here
CONFIG_ESP32S3_DATA_CACHE_LINE_32B 1   <- the lever
CONFIG_ESP32S3_DATA_CACHE_32KB 1       <- a second lever, of an available 64 KB
CONFIG_COMPILER_OPTIMIZATION_SIZE 1
```

So one real finding remains, and it is specific to this board:

**A 32-byte cache line on an octal bus.** The ESP32-S3 TRM specifies that Octal
PSRAM in DDR mode transfers in 64-byte wrap bursts. With a 32-byte line, every
line fill still costs a full 64-byte burst on the wire — we pay for 64 bytes and
keep 32. Half the effective bandwidth, discarded at the bus. This matters here
and not on a quad-PSRAM board, and the FQBN in the `Makefile` says `PSRAM=opi`.

Plus one the original review missed: the data cache is **32 KB of the S3's
available 64 KB**, free in the same migration.

The arithmetic re-derives cleanly, and in fact more cleanly than before. The
original text argued that 37.7 MB/s being 24% of an 80 MHz bus's theoretical
160 MB/s "would be implausibly bad, therefore it must be a 40 MHz bus." We are
on the 80 MHz bus and we *are* at 24%. The 64-byte-burst-for-a-32-byte-line
waste accounts for roughly half of that gap on its own — the same mechanism the
section always described, now carrying the whole weight instead of half of it.

**Revised prize: up to ~2x, not 2x-to-4x** — and it still costs a full
build-system migration, which is why the plan at the end no longer opens with
it.

### Verifying it

```bash
# Windows / this machine. Note the per-chip layout: esp32s3-libs, not the
# esp32-arduino-libs path older docs give, and %LOCALAPPDATA%\Arduino15 rather
# than ~/.arduino15.
grep -E "SPIRAM_SPEED|DATA_CACHE_LINE|DATA_CACHE_[0-9]+KB" \
  "$LOCALAPPDATA/Arduino15/packages/esp32/tools/esp32s3-libs/3.3.11/qio_opi/include/sdkconfig.h"
```

The IDF boot log also prints the PSRAM speed at INFO level on the 115200 serial
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

### `-Os`, separately — done, and it buys ~0% end to end

`CONFIG_COMPILER_OPTIMIZATION_SIZE=y` covers the IDF libraries, and
`platform.txt`'s `compiler.{c,cpp}.flags` puts `-Os` on LVGL and all of `src/`
too.

**The mechanism originally proposed here did not work.** It claimed
`extra_flags` lands *after* the base flags so a trailing `-O2` would win. It
lands *before*:

```
recipe.c.o.pattern="{compiler.c.cmd}" {compiler.c.extra_flags} {compiler.c.flags} ...
compiler.c.flags=... {compiler.optimization_flags} ...
compiler.optimization_flags=-Os
```

Confirmed on a real compile line: the `extra_flags` payload sits at index 127
and `-Os` at index 253. GCC takes the last `-O`, so `-O2` in `extra_flags` is
silently discarded and produces a byte-identical binary — a false negative that
would have read as "`-O2` doesn't help this codebase." The working lever is the
property `-Os` itself comes from, which covers C, C++ and assembly at once:

```make
--build-property "compiler.optimization_flags=-O2"
```

This is now the `Makefile` default via `OPT_FLAGS ?= -O2`; build with
`OPT_FLAGS=-Os` to revert. Flash goes 1 394 430 B → 1 505 498 B (+8.0%, still
47% of a 3 MB `huge_app`), RAM is unchanged.

**Measured, pooled over 88 samples per arm at matched aircraft load** (see the
A/B TRAP note in `frame_probe.hpp` — the first attempt at this measurement was
an artifact and reported a fictitious +29%):

| stage | change |
| --- | --- |
| `tasks` — label anchor solver | **−17%** |
| `fps`, `loop`, `lvgl`, `rast`, `vsync` | within ±4%, i.e. noise |

The prediction in this section was right on both halves, including the half
that says this changes nothing end to end. Keep it: the size is free, it
compounds with anything that lifts the bandwidth ceiling, and it is the only
Tier 1 item reachable without leaving `arduino-cli`. But **it is not a
throughput win today and must not be counted as one.**

It is also the strongest evidence in this document for the document's own
thesis. If `-O2` across LVGL and all of `src/` moves end-to-end throughput by
0%, there is essentially no arithmetic headroom anywhere on the render path —
the same conclusion the SIMD negative result reached, now confirmed by a much
blunter instrument.

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

> **Done, and measured 2026-07-28.** 401 408 B of PSRAM became 36 304 B of
> internal SRAM — 11.1x, close to the 40 KB estimated above. Measured with an
> *interleaved* A/B (`-DATC_BG_RLE_AB=1`, which flips the background source
> every 64 calls inside one session, so both arms see identical traffic):
> **`compositeBackground()` is 12.8% faster**, 17/17 windows favouring the RLE
> over ~4 150 calls per arm, sign-test p ≈ 8e-6. Solid.
>
> **But this section over-sold it, and so did the first attempt to measure it.**
> Flash-to-flash comparison suggested +13.7% fps and −27% on the restore; both
> were inflated by the aircraft-count confound, and the interleaved number is
> the honest one. `compositeBackground()` is ~6.0 ms of the ~15.8 ms `rast`
> pass, i.e. ~25% of the frame, so **12.8% of it is worth ~3% end to end** —
> not the headline "highest-value code change" this section claims.
>
> Two things it did *not* deliver. The 450 KB of PSRAM is **not** freed:
> `bgCache_` has to stay allocated because `renderBackgroundCache()` rasterises
> into it through `lv_canvas` and the RLE is re-encoded from it on every range
> change. And the plan missed that the beam blends *over* the background, so
> `blendSpan` needs random-access source pixels; pass two decodes into an SRAM
> line buffer first, which is why `sweep` also improved ~9%.
>
> The useful discovery is what it leaves behind. `compositeBackground()` is only
> ~38% of `rast`; the other ~62% — roughly **40% of the whole frame** — is
> content drawing, the trails, symbols and labels, and *nothing in this document
> targets it*. That is now the largest single item in the budget.

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

> **Re-verify before leaning on this.** The vendored LVGL is **9.5.0**, not the
> 9.3.0 this was checked against (`C:\repos\arduino-libs\libraries\lvgl`). The
> third-framebuffer item in Tier 2 was checked against the installed tree and
> `lv_display_set_3rd_draw_buffer` is still present, but the `refr_sync_areas()`
> / `lv_area_diff()` reasoning here has not been re-read at 9.5.0.

**The sweep blend is not arithmetic-bound, so SIMD is not the answer today.**
At 51 cycles per pixel for a bare PSRAM `memcpy` and ~67 for `blendSpan`
including its geometry solve, the blend is running at or below copy speed. The
conclusion already recorded in `frame_probe.hpp` — *"there was no
micro-optimisation to find, only fewer pixels to touch"* — is correct, and it
generalises: no hand-vectorising, no `-ffast-math` on the blend, no assembly.
Raise the ceiling or touch fewer pixels.

## The plan, in order

Reordered 2026-07-28. Steps 1 and 3 of the original are done; the migration
moved down, because its prize halved once PSRAM turned out to be at 80 MHz
already while its cost — a whole new build system — did not change.

1. ~~Read `sdkconfig.h` and the boot log.~~ **Done.** 80 MHz, 32-byte line,
   32 KB data cache. See Tier 1.
2. ~~`-O2`.~~ **Done**, via `compiler.optimization_flags`. −17% on the solver,
   ~0% end to end.
3. **Before anything else is measured on hardware: match the load or pool the
   rounds.** See the A/B TRAP note in `frame_probe.hpp`. Aircraft count swings
   these numbers by ~50 points between runs, and every remaining item below is
   predicted to land inside that band. This is now the gate on steps 5 and 6,
   not a footnote.
4. **Add an internal-SRAM variant of every loop in `benchPsram()`** — same
   shapes, SRAM buffers. That one comparison settles definitively whether each
   stage is bus-bound or compute-bound, and it is the measurement the probe
   harness is currently missing; today that distinction is inferred. Add a
   bounce-buffers-off arm while you are in there, to size what they cost.
5. Lower `pclk_hz` to 9–10 MHz and look at the screen.
6. RLE the background — now the best code-level item in this document; re-tune
   `kMaxRegions` and `kSweepBands`; hoist the `sqrtf` and the reciprocals out
   of `sectorRowSpan()`.
7. **Then** evaluate ESP-IDF with Arduino as a component, against a realistic
   ~2x (64-byte cache line, 64 KB data cache, `-O2` on the IDF libraries too,
   and possibly dropping the bounce buffers) rather than the 2x-to-4x this
   document originally advertised.
8. Third framebuffer — check it survives the bounce buffers first.
9. Then, and only then, split `compositeBackground()` across both cores and
   look at SIMD.

## Sources

- [esp32-arduino-lib-builder](https://github.com/esp-arduino-libs/esp32-arduino-lib-builder) — the defconfigs the precompiled libraries are built from
- [arduino-esp32-sdk](https://github.com/esp-arduino-libs/arduino-esp32-sdk) — the shipped libraries, including the `high_perf/` variant
- [esp_lvgl_port](https://deepwiki.com/espressif/esp-bsp/3.1-esp-lvgl-port) and [esp-bsp issue #621](https://github.com/espressif/esp-bsp/issues/621) — ESP32-S3 SIMD blend for LVGL
- [Adding LVGL to an ESP-IDF project](https://docs.lvgl.io/master/integration/chip_vendors/espressif/add_lvgl_to_esp32_idf_project.html)
- `lvgl/lvgl` at `v9.3.0`: `src/core/lv_refr.c`, `src/display/lv_display.h`, `lv_conf_template.h`

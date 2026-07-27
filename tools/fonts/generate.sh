#!/usr/bin/env bash
#
# Regenerate the subset LVGL fonts in src/fonts/.
#
# Run manually after changing a face, a size, or the glyph set -- NOT part of
# `make compile`, so node stays out of the build path. Commit the generated .c
# files alongside this script.
#
#   bash tools/fonts/generate.sh
#
# Faces (both SIL OFL, redistributable):
#   Inter            - designed for UI at small sizes, tall x-height. The
#                      variable file's default instance is Regular.
#   JetBrains Mono   - clock only. Proportional digits make the seconds field
#                      visibly jitter as 1 and 8 swap in, since the clock
#                      rewrites HH:MM:SS every second.
#
# FontAwesome comes from LVGL's own scripts/built_in_font/ so the LV_SYMBOL_*
# codepoints match exactly what lvgl.h defines.

set -euo pipefail
cd "$(dirname "$0")"

OUT=../../src/fonts
mkdir -p "$OUT"

CONV="npx -y lv_font_conv@latest"

# Text plus the three non-ASCII characters actually used: degree, em dash
# (info_panel's placeholder) and bullet (the stats separator -- and, less
# obviously, lv_textarea's password mask character).
TEXT_RANGE='0x20-0x7F,0xB0,0x2014,0x2022'

# Every LV_SYMBOL_* this project references, plus 0xF8A2 (LV_SYMBOL_NEW_LINE).
# That last one appears nowhere in our source: the stock lv_keyboard's default
# map uses it for the Enter key. Omit it and Enter renders as a missing-glyph
# box.
ICON_RANGE='0xF00C,0xF00D,0xF013,0xF021,0xF053,0xF054,0xF06E,0xF070,0xF071,0xF11C,0xF1EB,0xF55A,0xF8A2'

# 4 bpp throughout. Light-on-dark antialiasing is the hard case -- thin stems on
# a near-black field at 1 or 2 bpp visibly crawl -- and the panel's 12 MHz pixel
# clock already costs edge definition. The whole set is ~30 KB either way.
common=(--no-compress --no-prefilter --force-fast-kern-format --no-kerning
        --bpp 4 --format lvgl --lv-include lvgl.h)

for size in 14 16 20; do
  echo "==> atc_sans_${size}"
  $CONV "${common[@]}" --size "$size" \
    --font Inter-var.ttf -r "$TEXT_RANGE" \
    --font 'FontAwesome5-Solid+Brands+Regular.woff' -r "$ICON_RANGE" \
    --lv-font-name "atc_sans_${size}" \
    -o "$OUT/atc_sans_${size}.c"
done

# Digits, colon, space and hyphen only -- the hyphen matters, it is what the
# clock shows as "--:--:--" before NTP syncs. The full ASCII range at this size
# would cost ~36 KB; thirteen glyphs cost ~5 KB.
echo "==> atc_num_44"
$CONV "${common[@]}" --size 44 \
  --font JetBrainsMono-Medium.ttf -r '0x20,0x2D,0x30-0x3A' \
  --lv-font-name atc_num_44 \
  -o "$OUT/atc_num_44.c"

echo
echo "Generated into $OUT:"
ls -l "$OUT"

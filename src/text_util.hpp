#pragma once

/** Strips non-ASCII bytes in place.
 *
 *  The built-in Montserrat fonts carry 0x20-0x7F plus the LV_SYMBOL glyphs and
 *  nothing else, so any UTF-8 that reaches a label renders as missing-glyph
 *  boxes. Everything arriving from an API goes through here first. */
inline void sanitizeAscii(char *str) {
  if (str == nullptr) return;
  char *dst = str;
  for (char *src = str; *src; ++src) {
    if ((static_cast<unsigned char>(*src) & 0x80) == 0) {
      *dst++ = *src;
    }
  }
  *dst = '\0';
}

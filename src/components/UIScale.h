#pragma once
#include "fontIds.h"

// FreeInkUI font slots. Row heights, header height, and touch sizes are not
// chosen here: FreeInkApp derives all metric tokens from the body font's line
// height (themeTokensForLineHeight). One fixed tier for every board — the
// user-facing UI-scale setting was removed.
struct UIScaleSpec {
  int smallFontId;
  int bodyFontId;
  int titleFontId;
};

inline UIScaleSpec uiScaleSpec() {
  UIScaleSpec spec{};
  // The small slot carries the text a CJK list is made of -- file names, book titles, setting names
  // -- and every one of those glyphs has to be prewarmed into one contiguous arena. At 10 pt that
  // ask stopped fitting: with 23 KB free and an 8.6 KB largest run, a second page of Japanese file
  // names failed to warm and the render read 4355 glyphs one at a time (58 s). The 8 pt glyphs are
  // about three quarters the bytes, and the size the firmware themes already use for their own
  // small text, so the two agree again.
  spec.smallFontId = SMALL_FONT_ID;
  spec.bodyFontId = UI_12_FONT_ID;
  // Titles use the UI font, not a reader font: fui headers draw book and
  // directory titles, and the built-in Ubuntu UI fonts cover Hebrew (plus the
  // size-matched SD CJK fallback) where the NotoSans reader subsets do not.
  // Same font develop's drawHeader used, so script coverage matches develop.
  spec.titleFontId = UI_12_FONT_ID;
  return spec;
}

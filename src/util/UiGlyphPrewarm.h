#pragma once

#include <string>

class GfxRenderer;

// Batch-loads the glyphs a UI screen is about to draw.
//
// Glyphs the cache does not hold are fetched while drawing, and SdCardFont keeps only eight of
// those (OVERFLOW_CAPACITY). A screen carrying more distinct CJK characters than that -- any
// Japanese list -- thrashes the ring, so every glyph costs a seek plus a read from the card, in
// draw order, which is scattered order in the file, on every repaint. Collecting the screen's text
// first lets the same glyphs be read sorted by position, in a single forward pass. On the home
// screen that was the difference between ~6000 ms and ~630 ms per repaint.
//
// Latin-only setups are unaffected either way: their UI glyphs come from the built-in fonts, and
// GfxRenderer::prewarmText resolves to the SD fallback only for text that needs it.
//
// Usage: accumulate every string the render is about to draw, then apply() before the first draw
// call. For a paged list, add only the page in view -- a directory of hundreds of Japanese
// filenames has far more distinct characters than one page shows, and each extra one is a card read
// for a glyph nothing draws.
class UiGlyphPrewarm {
 public:
  UiGlyphPrewarm();

  void add(const char* text);
  void add(const std::string& text);

  // Loads the accumulated text into the UI font caches. No-op when nothing was added.
  void apply(const GfxRenderer& renderer) const;

  // Drop what apply() loaded. A screen must call this as it exits: the glyph bitmaps for a
  // screenful of CJK are tens of KB, and left resident they are tens of KB the next allocation does
  // not get. A section build needs one contiguous block for ZIP inflate, and a chapter opened
  // straight from a list screen failed to find one -- "Failed to index - invalid book" -- until the
  // list gave its cache back on the way out.
  static void release(const GfxRenderer& renderer);

  // First index of the page holding `selectedIndex`, matching how the themes page a list
  // (BaseTheme::getListPageItems). Callers use it to add only the rows on screen; the arithmetic
  // lives here so the four list screens cannot drift apart from each other.
  static int pageStart(int selectedIndex, int itemsPerPage);

 private:
  std::string text_;
};

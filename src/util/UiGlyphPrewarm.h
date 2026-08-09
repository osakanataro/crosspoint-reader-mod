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
// Text is added per role rather than in one heap. Three separate .cpfont files back the UI sizes,
// each with its own cache, and handing all three the same text loaded every glyph three times over:
// the chapter picker was reading a screenful of chapter titles into the header font that draws five
// characters of heading, and into the subtitle font it never used at all. Beyond the wasted reads
// that tripled the memory held, which pushed free heap under SdCardFont's retention floor -- so the
// arenas were dropped between repaints and every repaint paid the cold cost again.
//
// Latin-only setups are unaffected either way: their UI glyphs come from the built-in fonts, and
// GfxRenderer::prewarmText resolves to the SD fallback only for text that needs it.
//
// Usage: add every string the render is about to draw under the role that will draw it, then
// apply() before the first draw call. For a paged list, add only the page in view -- a directory of
// hundreds of Japanese filenames has far more distinct characters than one page shows, and each
// extra one is a card read for a glyph nothing draws.
class UiGlyphPrewarm {
 public:
  // Which of the three UI sizes draws the text. The themes fix this mapping: headers, tab labels
  // and cover-tile titles are UI_12 (also the only size any theme draws bold), list rows, menu
  // items and button hints are UI_10, and subtitles and right-hand labels are SMALL.
  //
  // Naming a font here is a deliberate coupling. The alternative -- a scan pass that records what
  // each drawText actually used -- means drawing the screen twice, which the home screen cannot do:
  // its cover tile mutates state as it draws. Get a role wrong and that element stays on the slow
  // path; nothing renders incorrectly.
  enum class Role : uint8_t {
    Header,    // UI_12: headings, tab labels, cover-tile titles
    Body,      // UI_10: list rows, menu items, button hints
    Subtitle,  // SMALL: subtitles, authors, right-hand labels, path lines
  };

  void add(Role role, const char* text);
  void add(Role role, const std::string& text);

  // Loads each role's text into the font that draws it. Roles with nothing added are skipped.
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
  std::string header_;
  std::string body_;
  std::string subtitle_;
};

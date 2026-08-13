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
// Text is added per role, and a role names WHAT DRAWS THE TEXT rather than a font size. The font
// each role resolves to is decided in one place (apply(), from uiScaleSpec()), because that mapping
// is not stable: the FreeInkUI conversion moved the list slots up a size, and a prewarm that kept
// naming sizes warmed the 8 pt font while the rows drew at 10 pt. Every glyph then came off the
// on-demand path with the prewarm reporting success -- 34 s per repaint on a file browser page of
// Japanese file names, with nothing in the diagnostics to say why.
//
// Roles that resolve to the same font AND style are warmed in one call: SdCardFont rebuilds a
// style's cache per prewarm, so a second call for the same pair would drop what the first loaded.
// Different styles of one font hold separate caches and do not interfere.
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
  // What draws the text. Get one wrong and that text stays on the slow path; nothing renders
  // incorrectly.
  enum class Role : uint8_t {
    Header,         // GUI.drawHeader's title (uiScaleSpec title font, the one place bold is drawn)
    ListRow,        // FreeInkUI text in theme().bodyText: list row labels, centred empty-state text
    ListSmall,      // FreeInkUI text in theme().smallText: subtitles, values, small-font row labels
    ListSmallBold,  // the same slot in bold: recent-books titles
    ThemeBody,      // the firmware themes' fixed body size: button hints, the home menu's rows
    ThemeSmall,     // their fixed small size: the reader status bar, the file browser's path line
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
  // lives here so the list screens cannot drift apart from each other.
  static int pageStart(int selectedIndex, int itemsPerPage);

 private:
  static constexpr uint8_t ROLE_COUNT = 6;

  std::string text_[ROLE_COUNT];
};

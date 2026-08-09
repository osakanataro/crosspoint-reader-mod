#include "UiGlyphPrewarm.h"

#include <GfxRenderer.h>

#include "fontIds.h"

namespace {
struct UiFontWarm {
  int fontId;
  uint8_t styleMask;
};

constexpr uint8_t kRegular = 1u << EpdFontFamily::REGULAR;
constexpr uint8_t kBold = 1u << EpdFontFamily::BOLD;

// Exactly the sizes SdCardFontSystem::setupUiFallbacks registers CJK fallbacks for. Each is a
// separate .cpfont with its own cache, so each needs its own pass.
//
// Bold is warmed for UI_12 alone: every EpdFontFamily::BOLD draw in every theme is at that size
// (BaseTheme header, tabs and popup titles; LyraTheme titles; RoundedRaffTheme's kTitleFontId is
// UI_12_FONT_ID). Warming bold at the other two sizes was two extra passes over the card for glyphs
// nothing draws.
constexpr UiFontWarm kUiFonts[] = {
    {SMALL_FONT_ID, kRegular},
    {UI_10_FONT_ID, kRegular},
    {UI_12_FONT_ID, static_cast<uint8_t>(kRegular | kBold)},
};

// One allocation per repaint, released at scope exit, sized to hold a screen's worth of labels
// without regrowing. Repaints are hundreds of ms apart and this replaces hundreds of card reads.
constexpr size_t TEXT_RESERVE = 512;
}  // namespace

UiGlyphPrewarm::UiGlyphPrewarm() { text_.reserve(TEXT_RESERVE); }

void UiGlyphPrewarm::add(const char* text) {
  if (text != nullptr && *text != '\0') text_ += text;
}

void UiGlyphPrewarm::add(const std::string& text) {
  if (!text.empty()) text_ += text;
}

void UiGlyphPrewarm::release(const GfxRenderer& renderer) { renderer.releaseFallbackGlyphCaches(); }

int UiGlyphPrewarm::pageStart(const int selectedIndex, const int itemsPerPage) {
  if (itemsPerPage <= 0 || selectedIndex <= 0) return 0;
  return (selectedIndex / itemsPerPage) * itemsPerPage;
}

void UiGlyphPrewarm::apply(const GfxRenderer& renderer) const {
  if (text_.empty()) return;
  for (const auto& font : kUiFonts) {
    renderer.prewarmText(font.fontId, text_.c_str(), font.styleMask);
  }
}

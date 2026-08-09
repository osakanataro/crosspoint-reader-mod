#include "UiGlyphPrewarm.h"

#include <GfxRenderer.h>

#include "fontIds.h"

namespace {
constexpr uint8_t kRegular = 1u << EpdFontFamily::REGULAR;
constexpr uint8_t kBold = 1u << EpdFontFamily::BOLD;

// Bold is warmed for the header size alone: every EpdFontFamily::BOLD draw in every theme is at
// UI_12 (BaseTheme header, tabs and popup titles; LyraTheme titles; RoundedRaffTheme's kTitleFontId
// is UI_12_FONT_ID). Warming bold elsewhere is a second pass over the card for glyphs nothing draws.
constexpr uint8_t HEADER_STYLES = static_cast<uint8_t>(kRegular | kBold);

// A screen's worth of labels without regrowing. One allocation per role per repaint, released at
// scope exit, against the hundreds of card reads it replaces.
constexpr size_t TEXT_RESERVE = 256;

void append(std::string& target, const char* text) {
  if (text == nullptr || *text == '\0') return;
  if (target.capacity() == 0) target.reserve(TEXT_RESERVE);
  target += text;
}
}  // namespace

void UiGlyphPrewarm::add(const Role role, const char* text) {
  switch (role) {
    case Role::Header:
      append(header_, text);
      break;
    case Role::Body:
      append(body_, text);
      break;
    case Role::Subtitle:
      append(subtitle_, text);
      break;
  }
}

void UiGlyphPrewarm::add(const Role role, const std::string& text) {
  if (text.empty()) return;
  add(role, text.c_str());
}

void UiGlyphPrewarm::apply(const GfxRenderer& renderer) const {
  if (!header_.empty()) renderer.prewarmText(UI_12_FONT_ID, header_.c_str(), HEADER_STYLES);
  if (!body_.empty()) renderer.prewarmText(UI_10_FONT_ID, body_.c_str(), kRegular);
  if (!subtitle_.empty()) renderer.prewarmText(SMALL_FONT_ID, subtitle_.c_str(), kRegular);
}

void UiGlyphPrewarm::release(const GfxRenderer& renderer) { renderer.releaseFallbackGlyphCaches(); }

int UiGlyphPrewarm::pageStart(const int selectedIndex, const int itemsPerPage) {
  if (itemsPerPage <= 0 || selectedIndex <= 0) return 0;
  return (selectedIndex / itemsPerPage) * itemsPerPage;
}

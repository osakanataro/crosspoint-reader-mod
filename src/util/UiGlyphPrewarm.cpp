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

// Fingerprint of the text last handed to each role, so a repaint drawing the same labels does not
// warm them again.
//
// SdCardFont::prewarm allocates and frees a 2 KB codepoint buffer on every call, so a cursor moving
// inside one page of a list churned three of those per repaint for glyphs already resident. On a
// device where a section build needs a contiguous 16 KB block, that churn is what leaves the heap
// with 48 KB free and no 12 KB run in it.
//
// A hash rather than the strings: keeping three std::strings resident forever to save a call is the
// wrong trade on 380 KB. A collision skips a warm that was needed, which leaves that text on the
// on-demand path -- slower, never wrong.
uint32_t appliedHeaderHash = 0;
uint32_t appliedBodyHash = 0;
uint32_t appliedSubtitleHash = 0;

// FNV-1a. Empty text hashes to 0 so "nothing added" and "nothing applied" compare equal.
uint32_t textHash(const std::string& text) {
  if (text.empty()) return 0;
  uint32_t hash = 2166136261u;
  for (const char c : text) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return hash;
}

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
  const uint32_t headerHash = textHash(header_);
  const uint32_t bodyHash = textHash(body_);
  const uint32_t subtitleHash = textHash(subtitle_);
  if (headerHash == appliedHeaderHash && bodyHash == appliedBodyHash && subtitleHash == appliedSubtitleHash) {
    return;  // same labels as last time; the glyphs are still where they were left
  }

  if (!header_.empty()) renderer.prewarmText(UI_12_FONT_ID, header_.c_str(), HEADER_STYLES);
  if (!body_.empty()) renderer.prewarmText(UI_10_FONT_ID, body_.c_str(), kRegular);
  if (!subtitle_.empty()) renderer.prewarmText(SMALL_FONT_ID, subtitle_.c_str(), kRegular);

  appliedHeaderHash = headerHash;
  appliedBodyHash = bodyHash;
  appliedSubtitleHash = subtitleHash;
}

void UiGlyphPrewarm::release(const GfxRenderer& renderer) {
  renderer.releaseFallbackGlyphCaches();
  // The glyphs are gone, so the next apply() must run whatever it is handed.
  appliedHeaderHash = 0;
  appliedBodyHash = 0;
  appliedSubtitleHash = 0;
}

int UiGlyphPrewarm::pageStart(const int selectedIndex, const int itemsPerPage) {
  if (itemsPerPage <= 0 || selectedIndex <= 0) return 0;
  return (selectedIndex / itemsPerPage) * itemsPerPage;
}

#include "UiGlyphPrewarm.h"

#include <GfxRenderer.h>

#include "InputDiag.h"
#include "fontIds.h"

namespace {
constexpr uint8_t kRegular = 1u << EpdFontFamily::REGULAR;
constexpr uint8_t kBold = 1u << EpdFontFamily::BOLD;

// Bold is warmed only where something draws bold, since each style is a separate cache and a second
// style is a second pass over the card. That is the header size (BaseTheme header, tabs and popup
// titles; LyraTheme titles; RoundedRaffTheme's kTitleFontId is UI_12_FONT_ID) and, since the
// FreeInkUI lists landed, the small size on rows that carry a bold title over a regular subtitle.
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
//
// Recorded only for a role whose prewarm reported success, and only alongside the glyph cache
// generation it was recorded under. Recording it unconditionally made a single failure permanent:
// a warm that could not build its arena (the mini bitmap wants one contiguous block, and max-alloc
// runs at ~15 KB with a book open) still counted as done, so every later repaint of the same text
// skipped it and drew through the 8-entry overflow ring -- 34 s on a file browser page of Japanese
// names. The generation covers the other way in: a warm that succeeded and was then released by
// SdCardFont's retention floor.
uint32_t appliedHeaderHash = 0;
uint32_t appliedBodyHash = 0;
uint32_t appliedSubtitleHash = 0;
uint32_t appliedSubtitleBoldHash = 0;
uint32_t appliedGeneration = 0;

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
    case Role::SubtitleBold:
      append(subtitleBold_, text);
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
  const uint32_t subtitleBoldHash = textHash(subtitleBold_);
  const uint32_t generation = renderer.glyphCacheGeneration();
  if (generation == appliedGeneration && headerHash == appliedHeaderHash && bodyHash == appliedBodyHash &&
      subtitleHash == appliedSubtitleHash && subtitleBoldHash == appliedSubtitleBoldHash) {
    return;  // same labels, and nothing has dropped the glyphs since
  }

  // A role that reports failure is recorded as unwarmed, so the next repaint tries again rather
  // than trusting a load that did not happen.
  const auto warm = [&renderer](const int fontId, const std::string& text, const uint8_t styles,
                                const uint32_t hash) -> uint32_t {
    if (text.empty()) return 0;
    if (renderer.prewarmText(fontId, text.c_str(), styles)) return hash;
    // No-op unless built with INPUT_DIAG. A failed warm is invisible from the outside: the screen
    // simply draws a glyph at a time.
    InputDiag::noteUiPrewarmFailure();
    return 0;
  };

  appliedHeaderHash = warm(UI_12_FONT_ID, header_, HEADER_STYLES, headerHash);
  appliedBodyHash = warm(UI_10_FONT_ID, body_, kRegular, bodyHash);
  appliedSubtitleHash = warm(SMALL_FONT_ID, subtitle_, kRegular, subtitleHash);
  appliedSubtitleBoldHash = warm(SMALL_FONT_ID, subtitleBold_, kBold, subtitleBoldHash);
  // Read after the warms: building one arena can be what pushes the heap under the retention floor
  // that drops another, and that release has to invalidate this record, not be recorded into it.
  appliedGeneration = renderer.glyphCacheGeneration();
}

void UiGlyphPrewarm::release(const GfxRenderer& renderer) {
  renderer.releaseFallbackGlyphCaches();
  // The glyphs are gone, so the next apply() must run whatever it is handed.
  appliedHeaderHash = 0;
  appliedBodyHash = 0;
  appliedSubtitleHash = 0;
  appliedSubtitleBoldHash = 0;
  appliedGeneration = renderer.glyphCacheGeneration();
}

int UiGlyphPrewarm::pageStart(const int selectedIndex, const int itemsPerPage) {
  if (itemsPerPage <= 0 || selectedIndex <= 0) return 0;
  return (selectedIndex / itemsPerPage) * itemsPerPage;
}

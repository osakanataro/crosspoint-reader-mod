#include "UiGlyphPrewarm.h"

#include <GfxRenderer.h>

#include "InputDiag.h"
#include "components/UIScale.h"
#include "fontIds.h"

namespace {
constexpr uint8_t kRegular = 1u << EpdFontFamily::REGULAR;
constexpr uint8_t kBold = 1u << EpdFontFamily::BOLD;

// A screen's worth of labels without regrowing. One allocation per role per repaint, released at
// scope exit, against the hundreds of card reads it replaces.
constexpr size_t TEXT_RESERVE = 256;

// One warm: every string this frame draws in this font and style, in one call.
struct WarmTarget {
  int fontId = 0;
  uint8_t styles = 0;
  std::string text;
};

// What each role resolves to. uiScaleSpec() owns the FreeInkUI slot sizes, so changing a slot there
// moves the prewarm with it instead of leaving the two to disagree silently.
WarmTarget targetFor(const UiGlyphPrewarm::Role role) {
  const auto spec = uiScaleSpec();
  WarmTarget target;
  switch (role) {
    case UiGlyphPrewarm::Role::Header:
      // Bold as well as regular: every EpdFontFamily::BOLD draw in every theme is a title
      // (BaseTheme header, tabs and popup titles; LyraTheme titles; RoundedRaffTheme's
      // kTitleFontId). Warming bold for a role that never draws it is a second pass over the card.
      target = {spec.titleFontId, static_cast<uint8_t>(kRegular | kBold), {}};
      break;
    case UiGlyphPrewarm::Role::ListRow:
      target = {spec.bodyFontId, kRegular, {}};
      break;
    case UiGlyphPrewarm::Role::ListSmall:
      target = {spec.smallFontId, kRegular, {}};
      break;
    case UiGlyphPrewarm::Role::ListSmallBold:
      target = {spec.smallFontId, kBold, {}};
      break;
    case UiGlyphPrewarm::Role::ThemeBody:
      // BaseTheme::drawButtonHints and drawButtonMenu draw at this size directly, not through a
      // FreeInkUI slot, so they do not follow uiScaleSpec.
      target = {UI_10_FONT_ID, kRegular, {}};
      break;
    case UiGlyphPrewarm::Role::ThemeSmall:
      // Likewise drawStatusBar, the header's battery percent, and the file browser's path line.
      target = {SMALL_FONT_ID, kRegular, {}};
      break;
  }
  return target;
}

// What each font+style pair was last warmed with, so a repaint drawing the same labels does not
// warm them again.
//
// SdCardFont::prewarm allocates and frees a 2 KB codepoint buffer on every call, so a cursor moving
// inside one page of a list churned one of those per target per repaint for glyphs already
// resident. On a device where a section build needs a contiguous 16 KB block, that churn is what
// leaves the heap with 48 KB free and no 12 KB run in it.
//
// Keyed on the pair rather than the role because roles share fonts (a header and its list rows both
// draw in the title font once the slots line up), and recorded only when the warm reported success:
// recording it unconditionally made one failure permanent, since every later repaint of the same
// text then skipped a load that had never happened.
constexpr uint8_t MAX_TARGETS = 6;
struct AppliedWarm {
  int fontId = 0;
  uint8_t styles = 0;
  uint32_t hash = 0;
};
AppliedWarm applied[MAX_TARGETS];
uint8_t appliedCount = 0;
// Bumped by SdCardFont whenever prewarmed data is dropped -- by the retention floor, or by a failed
// rebuild. A successful warm can be released again before the next repaint, so the record above
// only describes the font while this has not moved.
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

uint32_t* findApplied(const int fontId, const uint8_t styles) {
  for (uint8_t i = 0; i < appliedCount; i++) {
    if (applied[i].fontId == fontId && applied[i].styles == styles) return &applied[i].hash;
  }
  if (appliedCount >= MAX_TARGETS) return nullptr;
  applied[appliedCount] = {fontId, styles, 0};
  return &applied[appliedCount++].hash;
}

// Largest length not past `limit` that ends on a UTF-8 boundary, so a shortened warm asks for whole
// codepoints.
size_t utf8Truncate(const std::string& text, size_t limit) {
  if (limit >= text.size()) return text.size();
  while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xC0) == 0x80) limit--;
  return limit;
}

// Load `text` into `fontId`, and on failure again with half of it, then a quarter.
//
// The arena a warm builds is one contiguous block, so on a fragmented heap the ask can simply not
// fit -- measured on an X3 with 23 KB free and 8.6 KB as the largest run. A warm that gives up
// entirely leaves the whole screen on the on-demand path, which cost 4355 glyph reads and 58 s in
// one file browser render. Half a screenful resident costs about half that: this degrades in
// proportion rather than collapsing. Text is added top row first, so the half that survives is the
// half nearest the top of the list.
//
// Returns true only for a warm that took the whole text; a shortened one is reported as a failure
// so the record does not claim the rest is resident.
// Kerning and ligatures are skipped throughout: a UI string only reaches an SD fallback font when
// it carries CJK, which does not kern, and the tables were most of what the warm allocated.
constexpr bool UI_KERN_LIG = false;

bool warmWithFallback(const GfxRenderer& renderer, const int fontId, const std::string& text, const uint8_t styles) {
  if (renderer.prewarmText(fontId, text.c_str(), styles, UI_KERN_LIG)) return true;

  constexpr uint8_t MAX_SHRINK_STEPS = 2;
  size_t limit = text.size();
  for (uint8_t step = 0; step < MAX_SHRINK_STEPS; step++) {
    limit = utf8Truncate(text, limit / 2);
    if (limit == 0) break;
    if (renderer.prewarmText(fontId, text.substr(0, limit).c_str(), styles, UI_KERN_LIG)) break;
  }
  return false;
}

void append(std::string& target, const char* text) {
  if (text == nullptr || *text == '\0') return;
  if (target.capacity() == 0) target.reserve(TEXT_RESERVE);
  target += text;
}
}  // namespace

void UiGlyphPrewarm::add(const Role role, const char* text) { append(text_[static_cast<uint8_t>(role)], text); }

void UiGlyphPrewarm::add(const Role role, const std::string& text) {
  if (text.empty()) return;
  add(role, text.c_str());
}

void UiGlyphPrewarm::apply(const GfxRenderer& renderer) const {
  // No-op unless built with INPUT_DIAG.
  InputDiag::noteUiPrewarmBegin();
  // Merge the roles into one entry per font+style pair before warming anything: two calls for the
  // same pair would leave only the second one's glyphs resident.
  WarmTarget targets[ROLE_COUNT];
  uint8_t targetCount = 0;
  for (uint8_t i = 0; i < ROLE_COUNT; i++) {
    if (text_[i].empty()) continue;
    const WarmTarget resolved = targetFor(static_cast<Role>(i));
    uint8_t slot = 0;
    while (slot < targetCount && (targets[slot].fontId != resolved.fontId || targets[slot].styles != resolved.styles)) {
      slot++;
    }
    if (slot == targetCount) {
      targets[targetCount] = resolved;
      targets[targetCount].text.reserve(TEXT_RESERVE);
      targetCount++;
    }
    targets[slot].text += text_[i];
  }

  const uint32_t generation = renderer.glyphCacheGeneration();
  const bool dropped = generation != appliedGeneration;
  for (uint8_t i = 0; i < targetCount; i++) {
    const WarmTarget& target = targets[i];
    const uint32_t hash = textHash(target.text);
    uint32_t* record = findApplied(target.fontId, target.styles);
    if (!dropped && record != nullptr && *record == hash) {
      continue;  // same text, and nothing has dropped the glyphs since
    }
    if (warmWithFallback(renderer, target.fontId, target.text, target.styles)) {
      if (record != nullptr) *record = hash;
      continue;
    }
    // Some of the text is still on the on-demand path: record it as unwarmed so the next repaint
    // tries the whole set again rather than trusting a load that did not happen, and so the rows
    // that missed out are not skipped once the heap has room again. No-op unless built with
    // INPUT_DIAG -- a failed warm is invisible from the outside, the screen simply gets slow.
    if (record != nullptr) *record = 0;
    InputDiag::noteUiPrewarmFailure();
  }
  // Read after the warms: building one arena can be what pushes the heap under the retention floor
  // that drops another, and that release has to invalidate this record, not be recorded into it.
  appliedGeneration = renderer.glyphCacheGeneration();
  InputDiag::noteUiPrewarmEnd();
}

void UiGlyphPrewarm::release(const GfxRenderer& renderer) {
  renderer.releaseFallbackGlyphCaches();
  // The glyphs are gone, so the next apply() must run whatever it is handed.
  appliedCount = 0;
  appliedGeneration = renderer.glyphCacheGeneration();
}

int UiGlyphPrewarm::pageStart(const int selectedIndex, const int itemsPerPage) {
  if (itemsPerPage <= 0 || selectedIndex <= 0) return 0;
  return (selectedIndex / itemsPerPage) * itemsPerPage;
}

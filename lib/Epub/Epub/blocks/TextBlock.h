#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"
#include "Epub/FootnoteEntry.h"

// Represents a line of text on a page.
//
// All per-word data lives in ONE flat heap allocation (the arena) instead of
// six parallel vectors: a resident page holds ~25-30 of these blocks, and the
// vector-of-string layout cost ~250 throwing allocations per page load, which
// was the primary driver of heap fragmentation on the ESP32-C3.
//
// Arena layout, in order (4-byte alignment holds by construction: source ranges come first, then 16-bit
// arrays and the arena base is allocator-aligned; RISC-V faults on
// unaligned multi-byte access):
//   SourceRange sourceRanges[wordCount]  chapter codepoint offsets, [start, end)
//   uint16_t textOff[wordCount]        byte offset of word i's text in text[]
//   int16_t  xpos[wordCount]
//   int16_t  ypos[wordCount]           present only when isVertical (tategaki)
//   uint16_t focusSplitX[wordCount]   present only when focusPresent
//   uint8_t  styles[wordCount]
//   uint8_t  focusBoundary[wordCount]  present only when focusPresent
//   char     text[textBytes]           all words back to back, NUL-terminated
//
// Vertical (tategaki) blocks carry a per-word ypos array and lay each word out
// down a column (xpos is the shared column offset, ypos the stacking position);
// the page composes columns right-to-left. Vertical blocks never carry focus
// splits (bionic reading is a horizontal-only feature), so ypos and focus arrays
// are mutually exclusive in practice, though the layout permits either.
//
// Each word is stored NUL-terminated so render() can hand `text + textOff[i]`
// straight to C APIs (drawText) with no std::string materialization.
//
// Focus split semantics (unchanged from the vector layout): boundary N > 0
// means the first N bytes of word i render bold, the remainder in the base
// style. N is bounded to 9 codepoints (<= 36 UTF-8 bytes) by the clamp in
// ParsedText::addWord. focusSplitX is the pre-computed pixel offset from the
// word start to the second visual run (bold prefix in RTL, regular suffix in LTR).
// Both arrays are omitted when no word on the line has a split.
class TextBlock final : public Block {
 public:
  // The unused high bit of the cached style byte carries clipping metadata.
  static constexpr uint8_t DISCRETIONARY_HYPHEN_FLAG = 0x80;
  // Word font slot bit for CSS small caps; the style byte's high bit is the hyphen flag.
  static constexpr uint8_t SMALL_CAPS_SLOT_FLAG = 0x80;
  struct SourceRange {
    uint32_t start = UINT32_MAX;
    uint32_t end = UINT32_MAX;
  };

  struct LinkSpan {
    char href[FOOTNOTE_HREF_LEN];
    int16_t x;
    int16_t width;
    int16_t topLift;
  };

 private:
  BlockStyle blockStyle;
  uint16_t numWords = 0;
  uint16_t paragraphStartWord = UINT16_MAX;  // Visual index of the paragraph's first logical word.
  uint16_t textBytes = 0;                    // total size of the text region, including NULs
  bool focusPresent = false;
  bool isVertical = false;  // tategaki: arena carries a per-word ypos array
  // Full-width (CJK em) cell advance the layout used, carried because a column
  // holding only sideways runs has no upright word to re-derive it from at draw
  // time -- the line-height stand-in it fell back to overshot the cell by ~40%,
  // pushing the column's ruby into its right-hand neighbour. 0 = re-derive.
  uint16_t vertCellWidth = 0;
  bool isValid = true;
  // The ONLY allocation: makeUniqueNoThrow, so OOM yields an invalid block
  // instead of abort() (bare new is not nothrow with -fno-exceptions).
  std::unique_ptr<uint8_t[]> arena;
  // Typed views into the arena, bound once after the arena is filled. All
  // 16-bit bases sit at even offsets, so direct dereference is alignment-safe.
  const SourceRange* sourceRanges = nullptr;
  const uint16_t* textOffArr = nullptr;
  const int16_t* xposArr = nullptr;
  const int16_t* yposArr = nullptr;          // null when !isVertical
  const uint16_t* focusSplitXArr = nullptr;  // null when !focusPresent
  const uint8_t* stylesArr = nullptr;
  const uint8_t* focusBoundaryArr = nullptr;  // null when !focusPresent
  const char* textArr = nullptr;
  std::vector<std::string> rubyTexts;
  // Words sized apart from the line (CSS font-size spans): MAX_WORD_FONTS int32 font ids,
  // then one slot per word (0 = block font, n = font n-1). Null when every word uses the
  // block font, so ordinary lines pay only this pointer.
  std::unique_ptr<uint8_t[]> wordFontData;
  // Layout-only metadata. ChapterHtmlSlimParser moves it into Page::links
  // immediately; cached TextBlocks therefore keep the same compact format.
  std::vector<LinkSpan> linkSpans;

  TextBlock() = default;  // deserialize() fills the fields directly
  static size_t arenaSize(uint16_t wordCount, bool hasFocus, bool hasVertical, uint16_t textBytes);
  void bindArenaPointers();
  void renderVertical(const GfxRenderer& renderer, int fontId, int x, int y) const;

 public:
  // Flatten-on-construct: copies the layout-time vectors into the arena; the
  // vectors die with the caller. On arena OOM the block is empty and valid()
  // is false -- callers must check and fail the line instead of using it.
  explicit TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusSplitX, const BlockStyle& blockStyle = BlockStyle(),
                     std::vector<std::string> rubyTexts = {}, std::vector<LinkSpan> linkSpans = {},
                     const std::vector<SourceRange>& ranges = {}, uint16_t paragraphStartWord = UINT16_MAX);
  // Vertical (tategaki) block: per-word Y stacking positions instead of a shared
  // baseline. No focus-split support (bionic reading is horizontal-only).
  explicit TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<int16_t>& wordYpos, const std::vector<EpdFontFamily::Style>& wordStyles,
                     const BlockStyle& blockStyle = BlockStyle(), std::vector<std::string> rubyTexts = {},
                     uint16_t cellWidth = 0, std::vector<LinkSpan> linkSpans = {});
  ~TextBlock() override = default;
  TextBlock(const TextBlock&) = delete;
  TextBlock& operator=(const TextBlock&) = delete;

  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  // The font this line was laid out in: a CSS font-size override, else the section font.
  int blockFontId(const int sectionFontId) const { return blockStyle.fontId ? blockStyle.fontId : sectionFontId; }

  static constexpr uint8_t MAX_WORD_FONTS = 3;
  // slots[i] picks word i's font: 0 = the block font, n = fonts[n-1]; SMALL_CAPS_SLOT_FLAG
  // marks small caps. Returns false on OOM.
  bool setWordFonts(const uint8_t* slots, const int32_t (&fonts)[MAX_WORD_FONTS]);
  int wordFontId(uint16_t i, int sectionFontId) const;
  bool wordHasSmallCaps(uint16_t i) const {
    return wordFontData && (wordFontData[sizeof(int32_t) * MAX_WORD_FONTS + i] & SMALL_CAPS_SLOT_FLAG);
  }
  // fontId when it is (or can be) registered with the renderer, else the section font.
  static int renderFontId(const GfxRenderer& renderer, int fontId, int sectionFontId);
  // Ascent of the line's shared baseline: the tallest font among its words.
  int lineAscent(const GfxRenderer& renderer, int sectionFontId) const;
  // Offset from the line top at which to draw word i so every word shares the baseline.
  int wordYOffset(const GfxRenderer& renderer, int sectionFontId, uint16_t i) const;
  // Extra height the line needs beyond its block font for taller words.
  int extraAscent(const GfxRenderer& renderer, int sectionFontId) const;
  bool isEmpty() override { return numWords == 0; }
  bool valid() const { return isValid; }
  uint16_t wordCount() const { return numWords; }
  // NUL-terminated by construction; safe to pass to C APIs directly.
  const char* wordText(const uint16_t i) const { return textArr + textOffArr[i]; }
  uint16_t wordTextLen(const uint16_t i) const {
    const uint16_t end = (i + 1 < numWords) ? textOffArr[i + 1] : textBytes;
    return end - textOffArr[i] - 1;  // exclude the NUL
  }
  SourceRange wordSourceRange(const uint16_t i) const { return sourceRanges[i]; }
  bool wordStartsParagraph(const uint16_t i) const { return i == paragraphStartWord; }
  int16_t wordXpos(const uint16_t i) const { return xposArr[i]; }
  int16_t wordYpos(const uint16_t i) const { return isVertical ? yposArr[i] : 0; }
  bool vertical() const { return isVertical; }
  // Vertical blocks never hyphenate, so there the style byte's high bit is VERTICAL_FLIP
  // (which shares SMALL_CAPS's value) rather than the hyphen flag.
  EpdFontFamily::Style wordStyle(const uint16_t i) const {
    if (isVertical) return static_cast<EpdFontFamily::Style>(stylesArr[i]);
    return static_cast<EpdFontFamily::Style>((stylesArr[i] & ~DISCRETIONARY_HYPHEN_FLAG) |
                                             (wordHasSmallCaps(i) ? EpdFontFamily::SMALL_CAPS : 0));
  }
  // The style to hand the renderer: in vertical blocks VERTICAL_FLIP would read as SMALL_CAPS.
  EpdFontFamily::Style drawStyle(const uint16_t i) const {
    return isVertical ? static_cast<EpdFontFamily::Style>(stylesArr[i] & ~EpdFontFamily::VERTICAL_FLIP) : wordStyle(i);
  }
  bool wordHasDiscretionaryHyphen(const uint16_t i) const {
    return !isVertical && (stylesArr[i] & DISCRETIONARY_HYPHEN_FLAG) != 0;
  }
  uint8_t focusBoundary(const uint16_t i) const { return focusPresent ? focusBoundaryArr[i] : 0; }
  uint16_t focusSplitX(const uint16_t i) const { return focusPresent ? focusSplitXArr[i] : 0; }
  bool hasRuby() const;
  int getRubyShift(int ascender) const { return hasRuby() ? (ascender / 2) : 0; }
  // Vertical only: whether it is worth measuring the column's extent for ruby clamping.
  bool blockHasRubyExtent() const { return isVertical && numWords > 0 && hasRuby(); }
  const std::vector<std::string>& getRubyTexts() const { return rubyTexts; }
  std::vector<LinkSpan> takeLinkSpans() { return std::move(linkSpans); }

#ifdef INPUT_DIAG
  // Diagnostic only: where renderVertical spends its time, accumulated across the blocks of one
  // page. Reading the stats zeroes them, so a caller brackets the pass it cares about: the scan
  // pass runs these same loops and would otherwise be counted alongside the real draw.
  struct VerticalRenderStats {
    uint32_t bodyMs;         // stacking and drawing the body cells
    uint32_t bodyCells;      // how many cells that was
    uint32_t rubyMeasureMs;  // measuring each ruby run before any of it is placed
    uint32_t rubyDrawMs;     // drawing the ruby, a glyph at a time
    uint32_t rubyGroups;     // how many annotations that was
  };
  static VerticalRenderStats takeVerticalRenderStats();
#endif

  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(HalFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(HalFile& file);
};

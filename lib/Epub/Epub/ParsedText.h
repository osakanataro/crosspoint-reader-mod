#pragma once

#include <EpdFontFamily.h>
#include <VerticalTextUtils.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "WordStore.h"
#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  // Word text lives in wordStore (chunked bump arena, NUL-terminated entries);
  // words holds 8-byte handles into it. This replaces the former
  // std::deque<std::string>: per-word string objects, their SSO spills, and
  // every hyphenation/NFC temporary were the layout path's dominant
  // small-allocation churn, and any failed implicit allocation abort()s under
  // -fno-exceptions. Handles stay in a std::deque for the #2814 reason: no
  // large contiguous reallocation at CJK token counts (deque grows in fixed
  // ~512 B nodes). On arena OOM the word is dropped and hadDroppedWords()
  // latches so the section build can fail readably instead of aborting.
  // rubyTexts stays a deque of strings: ruby is rare and per-block small.
  // The per-token parallel arrays below stay vectors: 1 byte / 1 bit each,
  // they never approach the contiguous-block ceiling.
  WordStore wordStore;
  std::deque<WordStore::StoredWord> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  // Boundary flags use all four combinations:
  //   continues=false, noSpace=false: ordinary breakable word gap
  //   continues=false, noSpace=true:  breakable zero-width, stretchable CJK/Korean gap
  //   continues=true,  noSpace=false: unbreakable attachment
  //   continues=true,  noSpace=true:  breakable zero-width, non-stretching attachment
  std::vector<bool> wordContinues;
  std::vector<bool> wordNoSpaceBefore;
  // Focus Reading emphasis: bytes [0, wordFocusBoundary) render bold, the rest at wordStyles.
  // 0 = none. An annotation rather than a token split, so the hyphenator and line breaker still
  // see whole words; TextBlock stores emphasis the same way, so extractLine passes it through.
  std::vector<uint8_t> wordFocusBoundary;
  // Internal-link identity through tokenization, hyphenation and BiDi reorder.
  // Zero means plain text; non-zero indexes linkTargets. Kept at one byte per
  // token and discarded after layout, never added to the page-cache TextBlock.
  std::vector<uint8_t> wordLinkIds;
  // Words sized by an inline font-size span: a slot per word into sizeSlotScales (absolute
  // multiples of the reader's body size). Slot 0 follows the block's own size; the parser
  // resolves each slot's font before layout.
  std::vector<uint8_t> wordSizeSlots;
  static constexpr uint8_t MAX_SIZE_SLOTS = TextBlock::MAX_WORD_FONTS + 1;
  float sizeSlotScales[MAX_SIZE_SLOTS] = {1.0f};
  int32_t sizeSlotFontIds[MAX_SIZE_SLOTS] = {};
  uint8_t sizeSlotCount = 1;
  int wordFontId(size_t wordIndex, int blockFontId) const;
  std::vector<std::string> linkTargets;
  // Per-word vertical orientation (tategaki). Populated only in vertical mode, in lockstep
  // with words[]; empty in horizontal mode. Consumed by layoutVerticalColumns.
  std::vector<VerticalTextUtils::VerticalBehavior> wordVerticalBehaviors;
  // Zero-based visible Unicode-codepoint offsets in the spine body, stored as
  // uint16_t deltas from a shared base to keep this layout-only metadata small.
  // Pathological spans wider than uint16_t use sparse rebases; rendered
  // TextBlocks retain absolute ranges for portable clipping anchors.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  // Sparse NFC source positions, in small nothrow chunks rather than a large contiguous array.
  struct AbsorbedSourceChunk {
    std::unique_ptr<AbsorbedSourceChunk> next;
    uint32_t base = 0;
    uint16_t deltas[64] = {};
    uint8_t begin = 0;
    uint8_t count = 0;
    ~AbsorbedSourceChunk() {
      while (next) {
        auto retired = std::move(next);
        next = std::move(retired->next);
      }
    }
  };
  std::unique_ptr<AbsorbedSourceChunk> absorbedSourceHead;
  AbsorbedSourceChunk* absorbedSourceTail = nullptr;
  mutable const AbsorbedSourceChunk* absorbedSourceCursor = nullptr;
  mutable uint32_t absorbedSourceCursorStart = 0;
  std::deque<std::string> rubyTexts;
  BlockStyle blockStyle;
  uint8_t wordSpacingPercent = 100;
  uint8_t paragraphIndentSpaces;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  // True when this block is laid out as vertical (tategaki) columns. Kept here so addWord can
  // maintain the wordVerticalBehaviors invariant even on shared markup paths that don't know
  // about vertical mode.
  bool verticalMode;
  bool isNaturalAlign;
  // True once this paragraph has handed a line to the page. The parser soft-flushes a paragraph
  // over the word threshold, so layout runs several times over the same ParsedText; each run
  // numbers its own lines from zero and would otherwise re-apply the first-line indent at every
  // flush boundary. Never reset: one ParsedText is one paragraph (startNewTextBlock builds a
  // fresh one).
  bool lineEmitted = false;
  bool topSpacingClaimed = false;
  bool hasRtlWord;
  bool droppedWords = false;
  int linesAfterCurrent = -1;
  bool firstLineConsumed = false;
  // Separate from firstLineConsumed: a <br> line suppresses the indent but still starts a
  // new clipping paragraph.
  bool paragraphStartEmitted = false;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedFocusBoundaryScratch;
  std::vector<uint16_t> visualOrderScratch;

  std::string_view wordAt(const size_t i) const { return wordStore.view(words[i]); }
  bool storeWord(std::string_view text, WordStore::StoredWord& out);
  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  uint32_t sourceOffsetAfter(uint32_t start, uint32_t renderedLength) const;
  bool recordAbsorbedSourceOffset(uint32_t offset);
  void retireAbsorbedSourceOffsets(uint32_t remainingStart);
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  // True when the paragraph already opens with an ideographic space (U+3000), which
  // paperback-derived EPUBs use as the first-line indent itself. Adding the reader's own
  // indent on top of it sets the line in two characters.
  bool beginsWithIdeographicSpace() const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec, size_t maxLines);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool hyphenationEnabled = false, const bool focusReadingEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle(), const uint8_t paragraphIndentSpaces = 2,
                      const bool verticalMode = false)
      : blockStyle(blockStyle),
        paragraphIndentSpaces(paragraphIndentSpaces),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        verticalMode(verticalMode),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint32_t visibleTextOffset = 0, uint8_t linkId = 0, uint8_t sizeSlot = 0, bool noSpaceBefore = false);
  uint8_t addLinkTarget(const char* href);
  bool linkTargetMatches(uint8_t linkId, const char* href) const;
  // Marks tokens [first, size()) as belonging to link target linkId. The vertical tokenizer
  // pushes a run of cells at once and tags them afterwards, as the emphasis path does.
  void setLinkIdFrom(size_t first, uint8_t linkId) {
    for (size_t i = first; i < wordLinkIds.size(); ++i) wordLinkIds[i] = linkId;
  }
  // Vertical (tategaki) token: one already-tokenized unit (typically a single codepoint)
  // plus its orientation class. Bypasses the horizontal focus/bidi machinery -- vertical
  // layout stacks tokens down a column and composes columns right-to-left.
  void addVerticalToken(std::string_view token, EpdFontFamily::Style fontStyle, VerticalTextUtils::VerticalBehavior vb,
                        uint32_t visibleTextOffset = 0);
  // Reserve the per-token parallel arrays for `additionalTokens` more pushes. Callers that
  // append a burst of tokens (a CJK-split word, a buffer of vertical cells) should call this
  // first so the arrays grow once instead of doubling repeatedly mid-burst.
  void ensureTokenCapacity(size_t additionalTokens);
  // Read access to a token's text (the parser's bouten path needs each CJK token's length).
  std::string_view tokenAt(size_t index) const { return wordAt(index); }
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  // The word's renderer style: stored flags removed, small caps restored from its size slot.
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    if (index >= wordStyles.size()) return EpdFontFamily::REGULAR;
    const bool smallCaps = index < wordSizeSlots.size() && (wordSizeSlots[index] & TextBlock::SMALL_CAPS_SLOT_FLAG);
    return static_cast<EpdFontFamily::Style>((wordStyles[index] & ~TextBlock::DISCRETIONARY_HYPHEN_FLAG) |
                                             (smallCaps ? EpdFontFamily::SMALL_CAPS : 0));
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void suppressFirstLineIndent() { firstLineConsumed = true; }
  void resetFirstLineIndent() { firstLineConsumed = paragraphStartEmitted = false; }
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  // True the first time only. The block's top margin belongs before its first line, and a
  // paragraph past the soft-flush threshold is laid out in several passes -- whichever pass
  // comes first claims it, so it is neither lost at the head nor inserted mid-paragraph.
  bool claimTopSpacing() {
    if (topSpacingClaimed) return false;
    topSpacingClaimed = true;
    return true;
  }
  // True once any word was dropped because the text arena could not allocate.
  // Callers must treat the block as incomplete and fail the section build.
  bool hadDroppedWords() const { return droppedWords; }
  // While a line is being handed to layoutAndExtractLines' callback: how many lines of the
  // paragraph follow it, or -1 when the rest of the paragraph is not laid out yet.
  int linesAfterCurrentLine() const { return linesAfterCurrent; }
  // Slot for words at an absolute size; reuses a matching slot, or the nearest once all are taken.
  uint8_t sizeSlotFor(float scale);
  uint8_t sizeSlotsInUse() const { return sizeSlotCount; }
  float sizeSlotScale(uint8_t slot) const { return sizeSlotScales[slot]; }
  void setSizeSlotFontId(uint8_t slot, int32_t fontId) { sizeSlotFontIds[slot] = fontId; }
  // A set stop flag abandons extraction; the caller must discard this block.
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                             bool includeLastLine = true, int8_t characterSpacing = 0, uint8_t wordSpacingPercent = 100,
                             size_t maxLines = SIZE_MAX, const bool* stopRequested = nullptr);
  // Vertical (tategaki) analogue of layoutAndExtractLines: stacks tokens down columns of
  // height columnHeight, applying kinsoku at column boundaries, and emits one TextBlock per
  // column (words positioned by ypos; the page composes columns right-to-left) with the
  // visible offset of its first token. Consumes emitted words like the horizontal path;
  // includeLastColumn=false preserves a trailing partial column across mid-block flushes.
  // cjkCellWidthMemo carries the full-width cell advance across paragraphs: a block with
  // an upright word writes its advance there, and a block without one (a pure-Latin
  // paragraph) reads it back instead of falling back to the line height, which is ~40%
  // wider and used to push such columns' ruby into the neighbouring column.
  void layoutVerticalColumns(const GfxRenderer& renderer, int fontId, uint16_t columnHeight,
                             const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processColumn,
                             int* cjkCellWidthMemo = nullptr, bool includeLastColumn = true,
                             int8_t characterSpacing = 0);
};

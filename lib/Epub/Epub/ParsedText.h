#pragma once

#include <EpdFontFamily.h>
#include <VerticalTextUtils.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  // words/rubyTexts are std::deque, not std::vector: a paragraph can hold thousands
  // of tokens (CJK splits every character), and a vector grows by reallocating its
  // whole element array into one contiguous block (32 B/std::string -> 64-128 KB at
  // a few thousand tokens). On the ESP32-C3 that single large contiguous request
  // fails under a fragmented, BLE-resident heap and the throwing operator new
  // abort()s the firmware (fresh-open CJK crash). A deque grows in fixed ~512 B nodes
  // (largest contiguous alloc stays ~2 KB regardless of token count), so it never
  // triggers that. The per-token parallel arrays below stay vectors: 1 byte / 1 bit
  // each, they never approach the contiguous-block ceiling.
  std::deque<std::string> words;
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
  std::vector<std::string> linkTargets;
  // Per-word vertical orientation (tategaki). Populated only in vertical mode, in lockstep
  // with words[]; empty in horizontal mode. Consumed by layoutVerticalColumns.
  std::vector<VerticalTextUtils::VerticalBehavior> wordVerticalBehaviors;
  // Zero-based visible Unicode-codepoint offsets in the spine body, stored as
  // uint16_t deltas from a shared base to keep this layout-only metadata small.
  // Pathological spans wider than uint16_t use sparse rebases; rendered
  // TextBlocks do not carry any of this metadata.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  std::deque<std::string> rubyTexts;
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  // True when this block is laid out as vertical (tategaki) columns. Kept here so addWord can
  // maintain the wordVerticalBehaviors invariant even on shared markup paths that don't know
  // about vertical mode.
  bool verticalMode;
  bool isNaturalAlign;
  // True once this paragraph has handed a line (horizontal) or a column (vertical) to the page.
  // The parser soft-flushes a paragraph over the word threshold, so layout runs several times
  // over the same ParsedText; each run numbers its own lines from zero and would otherwise
  // re-apply the first-line indent at every flush boundary. Never reset: one ParsedText is one
  // paragraph (startNewTextBlock builds a fresh one).
  bool lineEmitted = false;
  // Set when a layout pass gave up for want of memory. The parser turns it into a build
  // failure, so the chapter is rebuilt on the next open instead of the reader aborting
  // mid-paragraph (observed 2026-09-10 in layoutVerticalColumns at ~1 KB free).
  bool layoutFailed_ = false;
  bool hasRtlWord;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedFocusBoundaryScratch;
  std::vector<uint16_t> visualOrderScratch;

  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;

  // True when the paragraph already opens with an ideographic space (U+3000),
  // which paperback-derived EPUBs use as the first-line indent itself. Adding
  // the reader's own indent on top of it sets the line in two characters.
  bool beginsWithIdeographicSpace() const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);
  // Whether the heap can hold the arrays a layout pass sizes by word count. Checked once at
  // the top of each pass: every one of them is a bare operator new, which terminates the
  // firmware on failure (-fno-exceptions) rather than returning null, so there is nowhere
  // downstream to fail gracefully.
  static bool hasHeapForLayout(size_t wordCount, size_t bytesPerWord);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const bool focusReadingEnabled = false, const BlockStyle& blockStyle = BlockStyle(),
                      const bool verticalMode = false)
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        verticalMode(verticalMode),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint32_t visibleTextOffset = 0, uint8_t linkId = 0);
  uint8_t addLinkTarget(const char* href);
  bool linkTargetMatches(uint8_t linkId, const char* href) const;
  // Vertical (tategaki) token: one already-tokenized unit (typically a single codepoint)
  // plus its orientation class. Bypasses the horizontal focus/bidi machinery — vertical
  // layout stacks tokens down a column and composes columns right-to-left. Does not track
  // a visible-text offset (see the wordVisibleOffsetDeltas comment above).
  void addVerticalToken(std::string token, EpdFontFamily::Style fontStyle, VerticalTextUtils::VerticalBehavior vb);
  // Reserve the per-token parallel arrays for `additionalTokens` more pushes. Callers that
  // append a burst of tokens (a CJK-split word, a buffer of vertical cells) should call this
  // first so the arrays grow once instead of doubling repeatedly mid-burst.
  void ensureTokenCapacity(size_t additionalTokens);
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  // Read access to a token's text (the parser's bouten path needs each CJK token's length).
  const std::string& wordAt(size_t index) const { return words[index]; }
  bool isEmpty() const { return words.empty(); }
  // True when a layout pass could not get the memory it needed and emitted nothing. The
  // pages built so far are still correct; what follows this paragraph is missing, so the
  // caller must discard the build rather than persist it.
  [[nodiscard]] bool layoutFailed() const { return layoutFailed_; }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                             bool includeLastLine = true);
  // Vertical (tategaki) analogue of layoutAndExtractLines: stacks tokens down columns of
  // height columnHeight, applying kinsoku at column boundaries, and emits one TextBlock per
  // column (words positioned by ypos; the page composes columns right-to-left). Consumes
  // emitted words like the horizontal path; includeLastColumn=false preserves a trailing
  // partial column across mid-block flushes.
  // cjkCellWidthMemo carries the full-width cell advance across paragraphs: a block with
  // an upright word writes its advance there, and a block without one (a pure-Latin
  // paragraph) reads it back instead of falling back to the line height, which is ~40%
  // wider and used to push such columns' ruby into the neighbouring column.
  void layoutVerticalColumns(const GfxRenderer& renderer, int fontId, uint16_t columnHeight,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processColumn,
                             int* cjkCellWidthMemo = nullptr, bool includeLastColumn = true);
};

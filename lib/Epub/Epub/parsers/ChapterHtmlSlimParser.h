#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <array>
#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  bool imagePopupFired = false;   // popupFn fired for the first image probe (single-shot)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  // Vertical layout only: an HTML whitespace run is waiting to be emitted as a separator token.
  // Held rather than emitted on sight so leading and trailing runs stay collapsed away — it is
  // spent only when another word actually follows within the same block. See flushPartWordBuffer.
  bool pendingVerticalWhitespace = false;
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  // Ruby text state
  bool inRuby = false;
  int rubyStartWordIndex = -1;
  bool collectingRubyText = false;
  std::string rubyTextBuffer;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int16_t currentPageNextX = 0;  // vertical (tategaki): X of the next column, advancing right-to-left
  // Page index currentPageNextX was last anchored for; -1 = never. addColumnToPage compares it
  // against completedPageCount to detect a page started elsewhere and re-anchor to the right margin.
  int verticalCursorPageIndex = -1;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  // See ReaderRenderSpec::rightMargin. Only used by verticalRubyReserve().
  uint8_t rightMargin = 0;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool isVertical;  // tategaki: lay text out in right-to-left vertical columns
  // Lazy probe of the reading face for Vertical Forms punctuation (U+FE10-FE12).
  // Bit n of the low nibble: form FE10+n probed; bit n of the high nibble: present.
  uint8_t vertFormProbe = 0;
  // Same lazy probe for the two sesame marks (U+FE45/FE46), which the reading faces do not
  // all carry. Bit 0/1: FE45/FE46 probed; bit 2/3: present.
  uint8_t sesameProbe = 0;
  // Full-width cell advance carried across paragraphs for layoutVerticalColumns,
  // so a pure-Latin paragraph keeps its neighbours' cell width.
  int verticalCellWidthMemo = 0;

  // Geometry of one column in vertical writing: the cell the glyphs occupy, and the gap to the
  // next column. Both derive from the font's full-width cell, never from its line height --
  // see VERTICAL_COLUMN_PITCH_EM. Every site that positions a column (text, inline images,
  // full-width images) must use these two so the drawn cell and the layout agree.
  int verticalColumnWidth() const;
  int verticalColumnSpacing() const;
  // Space kept clear at the right edge of the page for the rightmost column's ruby, which
  // TextBlock draws beside the column rather than inside it.
  int verticalRubyReserve() const;
  // One vert_layout report per build, from the first column placed.
  bool verticalLayoutReported_ = false;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasTextDecoration = false;
    CssTextDecoration textDecoration = CssTextDecoration::None;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool setsParagraphDirection = false;
    bool hasTextAlign = false;
    CssTextAlign textAlign = CssTextAlign::Left;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
    bool hasEmphasis = false;
    CssTextEmphasis emphasis = CssTextEmphasis::None;
    bool hasOrientation = false;
    CssTextOrientation orientation = CssTextOrientation::Mixed;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  CssTextDecoration effectiveTextDecoration = CssTextDecoration::None;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveTextAlignDefined = false;
  CssTextAlign effectiveTextAlign = CssTextAlign::Left;
  bool effectiveSup = false;
  bool effectiveSub = false;
  static constexpr size_t MAX_GRID_TABLE_COLUMNS = 4;
  static constexpr size_t MAX_GRID_TABLE_CELL_WORDS = 32;
  static constexpr size_t MAX_GRID_TABLE_CELL_BYTES = 512;
  // Active text-emphasis (bouten). Drawn as a synthetic ruby annotation, so a real
  // <rt> on the same run overwrites it -- furigana wins over bouten.
  CssTextEmphasis effectiveEmphasis = CssTextEmphasis::None;
  // Active text-orientation / text-combine-upright (vertical layout only). Mixed is the
  // default tokenizer behaviour; anything else routes the run through flushForcedOrientation.
  CssTextOrientation effectiveOrientation = CssTextOrientation::Mixed;
  // One entry per open element, outermost first, so descendant selectors (".vrtl .start-1em")
  // can be matched against the element's ancestors. Pushed at the top of startElement and popped
  // at the top of endElement, independent of every other branch in those handlers.
  std::vector<CssParser::AncestorRef> ancestorStack;
  int tableDepth = 0;
  bool insideTableCell = false;
  bool tableRowStacked = false;
  bool tableRowRtl = false;
  uint16_t tableRowsSpannedRemaining = 0;
  size_t tableCellTextBytes = 0;
  std::vector<std::unique_ptr<ParsedText>> tableRowCells;
  std::array<std::vector<std::shared_ptr<TextBlock>>, MAX_GRID_TABLE_COLUMNS> tableCellLines;
  std::vector<uint32_t> tableLineVisibleOffsets;
  bool listItemBulletOnly = false;  // true when currentTextBlock has only the <li> bullet

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  // Anchors whose element has been flushed but whose first line is not on a page yet. The page
  // an anchor names must be the page its content lands on, and that is not known until
  // addLineToPage/addColumnToPage has decided whether the line still fits: a block starting
  // exactly at a page boundary used to be recorded one page early, sending a footnote jump to
  // the page before the note. Committed by those two sites, and by the end of the parse for
  // an anchored element that produces no text of its own.
  std::vector<std::string> anchorsAwaitingPlacement;
  // Only an id'd element that lays out nothing can leave an entry waiting, so a run this long
  // means a run of empty anchored elements; past it they are recorded at the current page,
  // which is the behaviour this replaces and no worse.
  static constexpr size_t MAX_ANCHORS_AWAITING_PLACEMENT = 16;
  // A layout pass gave up for want of heap, so the chapter is incomplete from that point on.
  // The build is failed rather than persisted: a section file missing the rest of a paragraph
  // would be indistinguishable from a good one on the next open.
  bool layoutOom_ = false;
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;
  // Canonical reading-position counter: zero-based Unicode codepoints in visible
  // <body> text. Token offsets flow through line breaking so every completed page
  // records the first source character it renders.
  uint32_t visibleTextOffset = 0;
  uint32_t partWordVisibleOffset = 0;
  uint32_t currentPageVisibleOffset = 0;
  bool currentPageVisibleOffsetSet = false;
  bool insideBody = false;
  bool htmlEnded_ = false;
  bool syntheticCharacterData = false;
  uint16_t nonVisibleTextDepth = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  uint8_t currentFootnoteLinkId = 0;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  // Resumable parse state. The one-shot parseAndBuildPages() drives these
  // internally; the incremental section builder drives them across render ticks
  // so a large single chapter can yield between pages instead of blocking the UI
  // until the whole thing is laid out. parseFile_ and the expat parser stay alive
  // for the lifetime of the parse so it can be paused and resumed at buffer
  // boundaries.
  XML_Parser xmlParser_ = nullptr;
  HalFile parseFile_;
  uint32_t parseStartTime_ = 0;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  // Record every anchor waiting for placement against the page in progress. Called once the
  // caller has put content on that page, so completedPageCount is the page the reader will
  // see the anchored content on.
  void commitAnchorsAwaitingPlacement();
  void flushPartWordBuffer();
  void maybeSoftFlushTextBlock();
  void fallbackTableRowToStacked();
  void closeTableCell();
  void finishTableRow();
  void addTableRowSeparator();
  void flushPartWordBufferVertical(EpdFontFamily::Style fontStyle);
  bool fontHasVerticalForm(uint32_t formCp);
  bool fontHasCodepoint(uint32_t cp) const;
  void substituteMissingCompatibilityIdeographs();
  const char* resolveEmphasisMark(CssTextEmphasis e);
  void setCurrentPageVisibleOffset(uint32_t offset);
  void makePages();
  // Vertical (tategaki) analogue of addLineToPage: places a laid-out column at the current
  // right-to-left X cursor, starting a new page when the cursor runs off the left edge.
  void addColumnToPage(std::shared_ptr<TextBlock> column);
  static EpdFontFamily::Style fontStyleForTextDecoration(CssTextDecoration decoration);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css);
  void pushTableTextStyleEntry(const CssStyle& cssStyle);
  static void applyTextEmphasisToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextOrientationToEntry(StyleStackEntry& entry, const CssStyle& css);
  // Vertical run under a CSS text-orientation / text-combine-upright other than mixed. Returns
  // false when the run should go through the ordinary tokenizer after all.
  bool flushForcedOrientation(EpdFontFamily::Style fontStyle, CssTextOrientation orientation);
  // Sideways token, split into column-sized pieces when it is taller than the column.
  void addSidewaysToken(std::string token, EpdFontFamily::Style fontStyle);
  void applyHorizontalEmphasis(size_t wordIndex);
  void pushDecorationStyleEntry(CssTextDecoration defaultDecoration, const CssStyle& cssStyle);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(
      std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer, const int fontId,
      const float lineCompression, const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
      const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
      const bool focusReadingEnabled, const bool isVertical,
      const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>& completePageFn,
      const bool embeddedStyle, const std::string& contentBase, const std::string& imageBasePath,
      const uint8_t imageRendering = 0, std::vector<std::string> tocAnchors = {},
      const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr,
      const uint8_t rightMargin = 0)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        rightMargin(rightMargin),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        isVertical(isVertical),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();

  // One-shot parse: builds every page before returning (begin + step* + finish).
  bool parseAndBuildPages();

  // Resumable parse, for the incremental section builder. Drive as:
  //   if (!beginParse()) fail;
  //   loop: switch (parseStep()) { More: keep going / yield; Done: finishParse(); Error: abortParse(); }
  // Pages are emitted via completePageFn as they complete during parseStep(), so
  // the caller can stop once enough pages are built and resume on a later tick.
  enum class ParseStatus { More, Done, Error };
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)

  void addLineToPage(std::shared_ptr<TextBlock> line, uint32_t visibleOffset);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }
};

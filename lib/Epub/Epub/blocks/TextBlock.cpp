#include "TextBlock.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>
#include <VerticalTextUtils.h>

#include <climits>
#include <cstring>

#include "../../../../src/fontIds.h"

size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasFocus, const bool hasVertical,
                            const uint16_t textBytes) {
  // Layout documented in TextBlock.h: 16-bit arrays first, then 8-bit arrays, then text.
  size_t size = static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t));
  if (hasVertical) {
    size += static_cast<size_t>(wordCount) * sizeof(int16_t);
  }
  if (hasFocus) {
    size += static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(uint8_t));
  }
  return size + textBytes;
}

void TextBlock::bindArenaPointers() {
  uint8_t* base = arena.get();
  const size_t wc = numWords;
  textOffArr = reinterpret_cast<const uint16_t*>(base);
  xposArr = reinterpret_cast<const int16_t*>(base + wc * 2);
  size_t off = wc * 4;
  // ypos stays among the 16-bit arrays (right after xpos) so 2-byte alignment holds.
  if (isVertical) {
    yposArr = reinterpret_cast<const int16_t*>(base + off);
    off += wc * 2;
  }
  if (focusPresent) {
    focusSuffixXArr = reinterpret_cast<const uint16_t*>(base + off);
    off += wc * 2;
  }
  stylesArr = base + off;
  off += wc;
  if (focusPresent) {
    focusBoundaryArr = base + off;
    off += wc;
  }
  textArr = reinterpret_cast<const char*>(base + off);
}

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusSuffixX, const BlockStyle& blockStyle,
                     std::vector<std::string> rubyTexts)
    : blockStyle(blockStyle), rubyTexts(std::move(rubyTexts)) {
  // Same invariant as deserialize(): a block never holds an all-empty rubyTexts, so a
  // ruby-less line costs nothing beyond its arena. The layout engine hands one over for
  // every line it extracts, ruby or not; release it here rather than carrying it for the
  // block's lifetime. Move-assigning an empty vector frees the buffer (clear() would not).
  if (!hasRuby()) {
    this->rubyTexts = std::vector<std::string>{};
  }

  // Focus annotations are optional: empty vectors mean no word in this block has a split.
  // When present, they must be sized in lockstep with words[].
  const bool hasFocus = !focusBoundary.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() || words.size() > 10000 ||
      (hasFocus && (words.size() != focusBoundary.size() || words.size() != focusSuffixX.size()))) {
    LOG_ERR("TXB", "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(focusBoundary.size()),
            static_cast<uint32_t>(focusSuffixX.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  focusPresent = hasFocus;
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  // Pass 1: total text size, one NUL per word. A line is at most a physical
  // row of the page, so uint16_t offsets are ample; reject anything larger.
  size_t totalText = 0;
  for (const auto& w : words) totalText += w.size() + 1;
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    focusPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, focusPresent, isVertical, textBytes);
  arena = makeUniqueNoThrow<uint8_t[]>(size);
  if (!arena) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    focusPresent = false;
    isValid = false;
    return;
  }
  bindArenaPointers();

  // Pass 2: fill. Mutable aliases of the const views bound above.
  auto* textOff = const_cast<uint16_t*>(textOffArr);
  auto* xpos = const_cast<int16_t*>(xposArr);
  auto* styles = const_cast<uint8_t*>(stylesArr);
  auto* text = const_cast<char*>(textArr);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = wordXpos[i];
    styles[i] = static_cast<uint8_t>(wordStyles[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
  if (focusPresent) {
    auto* suffixX = const_cast<uint16_t*>(focusSuffixXArr);
    auto* boundary = const_cast<uint8_t*>(focusBoundaryArr);
    for (uint16_t i = 0; i < numWords; i++) {
      suffixX[i] = focusSuffixX[i];
      boundary[i] = focusBoundary[i];
    }
  }
}

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<int16_t>& wordYpos, const std::vector<EpdFontFamily::Style>& wordStyles,
                     const BlockStyle& blockStyle, std::vector<std::string> rubyTexts)
    : blockStyle(blockStyle), rubyTexts(std::move(rubyTexts)) {
  // Same invariant the horizontal constructor keeps: never hold an all-empty rubyTexts.
  // A column split out of a ruby-bearing paragraph often lands entirely on unannotated
  // words, and that column should not pay for a vector of empty strings.
  if (!hasRuby()) {
    this->rubyTexts = std::vector<std::string>{};
  }

  if (words.size() != wordXpos.size() || words.size() != wordYpos.size() || words.size() != wordStyles.size() ||
      words.size() > 10000) {
    LOG_ERR("TXB", "Vertical construction failed: size mismatch (words=%u, xpos=%u, ypos=%u, styles=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordYpos.size()), static_cast<uint32_t>(wordStyles.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  isVertical = true;
  focusPresent = false;
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  size_t totalText = 0;
  for (const auto& w : words) totalText += w.size() + 1;
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Vertical construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    isVertical = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, focusPresent, isVertical, textBytes);
  arena = makeUniqueNoThrow<uint8_t[]>(size);
  if (!arena) {
    LOG_ERR("TXB", "OOM: vertical arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    isVertical = false;
    isValid = false;
    return;
  }
  bindArenaPointers();

  auto* textOff = const_cast<uint16_t*>(textOffArr);
  auto* xpos = const_cast<int16_t*>(xposArr);
  auto* ypos = const_cast<int16_t*>(yposArr);
  auto* styles = const_cast<uint8_t*>(stylesArr);
  auto* text = const_cast<char*>(textArr);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = wordXpos[i];
    ypos[i] = wordYpos[i];
    styles[i] = static_cast<uint8_t>(wordStyles[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
}

namespace {
// Re-derive the token's vertical behaviour at draw time. layoutVerticalColumns
// classified it the same way from the same text, so this stays in step without
// widening the cached block format with a per-word behaviour array.
//
// Upright: CJK, kana and the punctuation table. Tate-chu-yoko: runs of 1-2 digits and
// an exclamation/question pair, set upright inside one cell. Everything else (Latin
// words, 3+ digit numbers) is sideways.
bool isTateChuYokoToken(const char* word) {
  if (VerticalTextUtils::isTateChuYokoPunctuationPair(word)) return true;
  if (word[0] < '0' || word[0] > '9') return false;
  int digits = 0;
  for (const char* p = word; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') return false;
    digits++;
  }
  return digits <= 2;
}

bool isSidewaysToken(const char* word) {
  const auto* p = reinterpret_cast<const unsigned char*>(word);
  const uint32_t first = utf8NextCodepoint(&p);
  if (first == 0) return false;
  if (VerticalTextUtils::isUprightInVertical(first) ||
      VerticalTextUtils::getVerticalPunctuationOffset(first) != nullptr) {
    return false;
  }
  return !isTateChuYokoToken(word);
}

// Ruby written in Latin script is turned clockwise and run down the column, the same way
// a Latin word in the body is. Stacking it a letter at a time upright is what a reader
// sees as a smear rather than a word: the stack advances by each letter's own width, and
// a proportional 'i' or 'r' is a fraction as wide as it is tall, so the letters print on
// top of each other. Anything with a CJK character in it keeps the upright stack, where
// mixing the two would be worse than either.
//
// A reading that is only digits turns with the rest, where the body would set one or two
// of them upright as tate-chu-yoko. That divergence is deliberate, not an oversight: ruby
// is already half size, so a two-digit reading set upright across the column would leave
// each digit a quarter of the body's width -- consistent with the body, and unreadable.
// Checked on the device against the chapter of test/epubs-ja that carries 3, 12 and 2024
// as readings for exactly this comparison.
bool isSidewaysRuby(const char* ruby) {
  const auto* p = reinterpret_cast<const unsigned char*>(ruby);
  bool sawGlyph = false;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p)) != 0) {
    if (VerticalTextUtils::isUprightInVertical(cp) || VerticalTextUtils::getVerticalPunctuationOffset(cp) != nullptr) {
      return false;
    }
    sawGlyph = true;
  }
  return sawGlyph;
}

// Separation kept between the ruby of one group and the next down the column. One pixel
// is the least that reads as two runs rather than one on a 1-bit panel; more than that
// costs column space, which the ruby that needs separating is already short of.
constexpr int RUBY_GROUP_GAP = 1;
}  // namespace

void TextBlock::renderVertical(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  // Each token is stacked at its precomputed (xpos, ypos): CJK and kana upright,
  // Latin runs and the rotating punctuation turned clockwise (see VERTICAL_PUNCTUATION).
  //
  // Both adjustments below are measured against the full-width character cell -- the
  // em advance layoutVerticalColumns stacked by -- and NOT the column pitch. The two
  // differ: the pitch is the font's line height, which for a CJK face runs well wider
  // than the em (NotoSansJP 16pt: 48 px pitch, 33 px em). Upright glyphs land against
  // the cell's left edge, so centring anything on the pitch instead pushes it right,
  // out of line with the column of kanji above and below it.
  //
  // Every upright token is full-width, so the first one measures the cell for the whole
  // block. A block of nothing but Latin has none to measure; fall back to the line height,
  // matching layoutVerticalColumns' own fallback for cjkCharAdvance.
  int cellWidth = 0;
  for (uint16_t i = 0; i < numWords && cellWidth == 0; i++) {
    if (!isSidewaysToken(wordText(i))) {
      cellWidth = renderer.getTextAdvanceX(fontId, wordText(i), wordStyle(i));
    }
  }
  if (cellWidth == 0) {
    cellWidth = renderer.getLineHeight(fontId);
  }

  // Cell top -> the y drawText expects. drawText adds the ascender itself, and for a CJK
  // face that reaches past the em box, dropping every glyph low in its cell until the last
  // one of a column overlaps the status bar. Centring the line box in the cell pulls it back.
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int lineBox = ascender - renderer.getFontDescenderSize(fontId);
  const int uprightYAdjust = (cellWidth - lineBox) / 2;

  // A vertical block is one column, so its own stacking positions give the column's
  // extent: ruby is clamped to it below.
  int columnTop = 0;
  int columnEnd = 0;
  if (blockHasRubyExtent()) {
    columnTop = yposArr[0] + y;
    const uint16_t lastWord = numWords - 1;
    columnEnd = yposArr[lastWord] + y + renderer.getTextAdvanceX(fontId, wordText(lastWord), wordStyle(lastWord));
  }

  // Ruby sits to the right of the words it annotates, the vertical counterpart of
  // sitting above them. The grouping is the horizontal path's, unchanged: the leader
  // word carries the text and each continuation word is flagged RUBY_CONTINUE, so the
  // group is however many flagged tokens follow. Per-codepoint tokenisation makes those
  // groups longer than in horizontal mode, not different in kind.
  const bool blockHasRuby = hasRuby();

  for (uint16_t i = 0; i < numWords; i++) {
    const char* word = wordText(i);
    const int cellX = xposArr[i] + x;
    const int cellY = yposArr[i] + y;

    // Sideways runs: the column reserved the run's *width* as its vertical extent,
    // so drawing it upright would spill across the columns to the left.
    if (isSidewaysToken(word)) {
      renderer.drawTextSideways(fontId, cellX, cellY, word, cellWidth, true, wordStyle(i));
      continue;
    }

    // Punctuation drawn from a horizontal-layout font is in the wrong place, or the
    // wrong orientation, for a vertical column (see VERTICAL_PUNCTUATION).
    const auto* p = reinterpret_cast<const unsigned char*>(word);
    const uint32_t cp = utf8NextCodepoint(&p);
    const VerticalTextUtils::PunctuationOffset* punct = VerticalTextUtils::getVerticalPunctuationOffset(cp);

    if (punct != nullptr && punct->rotate) {
      // Brackets and long marks turn with the column. Rotating the horizontal glyph also
      // carries its ink to the right corner of the cell on its own: 「 ends up opening
      // downward at the cell top, 」 closing at the bottom, ー running along the column.
      // The advance is unchanged, so the cell still measures one em.
      renderer.drawTextSideways(fontId, cellX, cellY, word, cellWidth, true, wordStyle(i));
      continue;
    }

    int drawX = cellX;
    int drawY = cellY + uprightYAdjust;
    if (punct != nullptr) {
      drawX += cellWidth * punct->dxEighths / 8;
      drawY += cellWidth * punct->dyEighths / 8;
    } else if (isTateChuYokoToken(word)) {
      // Tate-chu-yoko sets its run upright inside one cell, which the layout reserved
      // at a full em. Two half-width digits fill that exactly, but a lone digit covers
      // half of it, and a proportional pair like "!?" is narrower still (26.6 px against
      // a 33.3 px cell in NotoSansJP) -- all of which would hug the cell's left edge,
      // off the column's axis. Centre whatever comes out narrower than the cell.
      const int advance = renderer.getTextAdvanceX(fontId, word, wordStyle(i));
      if (advance < cellWidth) {
        drawX += (cellWidth - advance) / 2;
      }
    }

    renderer.drawText(fontId, drawX, drawY, word, true, wordStyle(i));
  }

  // Ruby is placed in a pass of its own, running up the column from its foot. It cannot
  // be placed in the body's pass: where a group ends up depends on the groups after it,
  // and a run at the foot of the column would be drawn under the status bar, where it is
  // not misplaced but gone. Taken from the foot instead, each group sits at its ideal
  // position or as high as the group below leaves room for, whichever is lower, and a
  // single running variable carries that room -- no table of positions, and nothing
  // measured that is not drawn immediately after.
  if (blockHasRuby) {
    int rubyRoomBelow = (columnEnd > columnTop) ? columnEnd : INT_MAX;
    for (int w = static_cast<int>(numWords) - 1; w >= 0; w--) {
      const auto i = static_cast<uint16_t>(w);
      if (i >= rubyTexts.size() || rubyTexts[i].empty() || (wordStyle(i) & EpdFontFamily::RUBY_CONTINUE) != 0) {
        continue;
      }
      const int cellX = xposArr[i] + x;
      // Ruby runs down the column beside the body. A CJK ruby is stacked a glyph at a
      // time like the body is -- one drawText for the whole string would lay it across
      // the column instead -- while a Latin one is turned clockwise and drawn as a single
      // run, which is both how Japanese typesetting sets it and what keeps it legible.
      uint16_t groupWords = 1;
      while (i + groupWords < numWords && (wordStyle(i + groupWords) & EpdFontFamily::RUBY_CONTINUE) != 0) {
        groupWords++;
      }
      // Span the group along the column from the first cell's top to the last cell's
      // bottom, taken from the stacking positions the layout already computed rather
      // than re-summing advances -- ypos carries the inter-cell spacing too.
      const int groupTop = yposArr[i] + y;
      const uint16_t lastWord = i + groupWords - 1;
      const int lastAdvance = renderer.getTextAdvanceX(fontId, wordText(lastWord), wordStyle(lastWord));
      const int groupSpan = (yposArr[lastWord] + y + lastAdvance) - groupTop;

      // The stack advances on a fixed half-em, not on each glyph's own width. For kana
      // and kanji the two are the same number, since a full-width glyph halved by SUP is
      // exactly the half cell; for anything proportional they are not, and the glyph
      // width is the wrong one -- it is a measure across the line, and the stack runs
      // down it. JIS X 4051 sets ruby on that fixed pitch for the same reason.
      const bool rubySideways = isSidewaysRuby(rubyTexts[i].c_str());
      // A ruby glyph is half-width, so it sits in the half cell just right of the body one.
      const int rubyCellWidth = cellWidth / 2;
      const int rubyX = cellX + cellWidth;
      int rubySpan;
      if (rubySideways) {
        // Turned, the run's extent down the column is its width across the line, which is
        // what getTextAdvanceX reports -- SUP halves the glyphs and their advances alike.
        rubySpan = renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP);
      } else {
        int rubyCells = 0;
        const auto* cp = reinterpret_cast<const unsigned char*>(rubyTexts[i].c_str());
        while (utf8NextCodepoint(&cp) != 0) rubyCells++;
        rubySpan = rubyCells * rubyCellWidth;
      }
      int rubyCursorY = groupTop + (groupSpan - rubySpan) / 2;

      // A reading longer than what it annotates overhangs both ends of its group, so the
      // group below this one may already have taken the room it wanted. Take what is left.
      if (rubyCursorY + rubySpan > rubyRoomBelow) {
        rubyCursorY = rubyRoomBelow - rubySpan;
      }
      // Japanese typography answers the overhang at a column's ends the same way (JIS X
      // 4051): ruby that would overhang the line start is set flush with it instead of
      // centred. Holding it here is also what decides where a column carrying more ruby
      // than it has room for gives way -- at its head, against the top margin, rather
      // than off the top of the page.
      if (columnEnd > columnTop && rubyCursorY < columnTop) {
        rubyCursorY = columnTop;
      }
      rubyRoomBelow = rubyCursorY - RUBY_GROUP_GAP;

      if (rubySideways) {
        renderer.drawTextSideways(fontId, rubyX, rubyCursorY, rubyTexts[i].c_str(), rubyCellWidth, true,
                                  EpdFontFamily::SUP);
      } else {
        const auto* rp = reinterpret_cast<const unsigned char*>(rubyTexts[i].c_str());
        while (*rp != '\0') {
          const unsigned char* cpStart = rp;
          if (utf8NextCodepoint(&rp) == 0) break;
          const size_t cpLen = static_cast<size_t>(rp - cpStart);
          // A UTF-8 sequence is at most 4 bytes; a longer step means a malformed string.
          char glyph[5] = {};
          if (cpLen == 0 || cpLen >= sizeof(glyph)) break;
          memcpy(glyph, cpStart, cpLen);

          // Ruby is set in the column, so its punctuation needs turning and shifting for
          // the column exactly as the body's does -- an ー left upright in a katakana
          // reading is as wrong there as it is in the text it annotates. The body's own
          // table and the same two cases answer it, on the ruby cell instead of the full
          // one. Sideways drawing scales for SUP, so the rotating case is the same call.
          const auto* gp = reinterpret_cast<const unsigned char*>(glyph);
          const uint32_t gcp = utf8NextCodepoint(&gp);
          const VerticalTextUtils::PunctuationOffset* rubyPunct = VerticalTextUtils::getVerticalPunctuationOffset(gcp);

          if (rubyPunct != nullptr && rubyPunct->rotate) {
            renderer.drawTextSideways(fontId, rubyX, rubyCursorY, glyph, rubyCellWidth, true, EpdFontFamily::SUP);
            rubyCursorY += rubyCellWidth;
            continue;
          }

          // drawText always offsets by the full ascender, but a SUP glyph is half-scale, so
          // centre the halved line box in the ruby cell the same way the body centres the
          // full one -- otherwise the glyph lands an ascender's worth below its cell.
          int rubyDrawX = rubyX;
          int rubyDrawY = rubyCursorY + (rubyCellWidth - lineBox / 2) / 2 - ascender / 2;
          if (rubyPunct != nullptr) {
            rubyDrawX += rubyCellWidth * rubyPunct->dxEighths / 8;
            rubyDrawY += rubyCellWidth * rubyPunct->dyEighths / 8;
          } else {
            // The stack steps down the column on the half-em cell, but each glyph is drawn
            // from that cell's left edge, and a proportional one does not fill it. A thin
            // letter then sits off the axis the rest of the ruby runs along -- an 'I' among
            // 'A' and 'S' reads as pushed left, because it is. Centre whatever comes out
            // narrower than the cell, the same way the body centres a tate-chu-yoko run in
            // its own. Kana and kanji are unaffected: halved by SUP they are the cell.
            const int glyphAdvance = renderer.getTextAdvanceX(fontId, glyph, EpdFontFamily::SUP);
            if (glyphAdvance < rubyCellWidth) {
              rubyDrawX += (rubyCellWidth - glyphAdvance) / 2;
            }
          }
          renderer.drawText(fontId, rubyDrawX, rubyDrawY, glyph, true, EpdFontFamily::SUP);
          rubyCursorY += rubyCellWidth;
        }
      }
    }
  }
}

bool TextBlock::hasRuby() const {
  for (const auto& rt : rubyTexts) {
    if (!rt.empty()) return true;
  }
  return false;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid block");
    return;
  }

  if (isVertical) {
    renderVertical(renderer, fontId, x, y);
    return;
  }

  const bool scanning = renderer.isFontCacheScanning();
  const int ascender = renderer.getFontAscenderSize(fontId);

  // Resolve ruby positions. Layout (extractLine) has already reserved extraStartOffset on the
  // left and extraEndOffset on the right, so the centered rubyX is always within the page margins.
  struct RubyDrawInfo {
    int x;
    std::string text;
    BidiUtils::BidiBaseDir baseDir;
  };
  const bool blockHasRuby = hasRuby();
  std::vector<RubyDrawInfo> rubies;
  if (blockHasRuby) {
    rubies.resize(numWords);
    for (uint16_t i = 0; i < numWords; i++) {
      if (i < rubyTexts.size() && !rubyTexts[i].empty() && (wordStyle(i) & EpdFontFamily::RUBY_CONTINUE) == 0) {
        int groupWordCount = 1;
        while (i + groupWordCount < numWords && (wordStyle(i + groupWordCount) & EpdFontFamily::RUBY_CONTINUE) != 0) {
          groupWordCount++;
        }
        int groupActualWidth = 0;
        for (int k = 0; k < groupWordCount; ++k) {
          groupActualWidth += renderer.getTextAdvanceX(fontId, wordText(i + k), wordStyle(i + k));
        }
        const int rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP);
        const int leaderWordX = xposArr[i] + x;
        const auto baseDir =
            static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(wordText(i), blockStyle.isRtl ? 1 : 0));
        rubies[i] = {leaderWordX - (rubyWidth - groupActualWidth) / 2, rubyTexts[i], baseDir};
        i += groupWordCount - 1;
      }
    }
  }

  struct DecorationLineTracker {
    EpdFontFamily::Style style;
    int yOffset;
    int startX = -1;
    int endX = -1;
    int yPos = 0;

    bool active() const { return startX != -1; }
    void reset() {
      startX = -1;
      endX = -1;
      yPos = 0;
    }
  };

  DecorationLineTracker decorationLines[] = {
      {EpdFontFamily::UNDERLINE, ascender + 2},
      {EpdFontFamily::STRIKETHROUGH, ascender * 4 / 5},
  };

  const auto flushDecoration = [&](DecorationLineTracker& line) {
    if (line.active()) {
      renderer.drawLine(line.startX, line.yPos, line.endX, line.yPos, 2, true);
      line.reset();
    }
  };
  const auto flushDecorations = [&]() {
    for (auto& line : decorationLines) {
      flushDecoration(line);
    }
  };

  // Loop-invariant: hoisted out of the word loop so rubyTexts is scanned once,
  // not once per word.
  const int rubyShift = getRubyShift(ascender);

  for (uint16_t i = 0; i < numWords; i++) {
    const char* word = wordText(i);
    const int wordX = xposArr[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyle(i);
    const auto baseDir =
        static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(word, blockStyle.isRtl ? 1 : 0));
    const uint8_t boundary = focusBoundary(i);

    // SUP/SUB shift the baseline passed to drawText; the glyph is also scaled 50% inside
    // drawText, so these offsets are chosen relative to the full-size ascender:
    //   SUP: raise by 40% of ascender — sits clearly above the cap-height
    //   SUB: lower by 25% of ascender — descends below baseline without clashing with ascenders below
    int wordY = y + rubyShift;
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    const int drawX = wordX;

    if (boundary > 0) {
      // Focus split: draw bold prefix, then the regular suffix at a pre-computed x offset.
      // The bold prefix is bounded to 9 codepoints by the clamp on targetBoldChars in
      // ParsedText::addWord; 9 UTF-8 codepoints occupy at most 9 * 4 = 36 bytes, +1 for null = 37.
      // suffixX is computed at cache-creation time to avoid font metric lookups at render time.
      static constexpr size_t MAX_FOCUS_PREFIX_BYTES = 9 * 4 + 1;
      char boldBuf[40];
      static_assert(sizeof(boldBuf) >= MAX_FOCUS_PREFIX_BYTES,
                    "boldBuf too small for max focus prefix (9 codepoints * 4 UTF-8 bytes + null)");
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      const size_t boldLen =
          std::min<size_t>({static_cast<size_t>(boundary), static_cast<size_t>(wordTextLen(i)), sizeof(boldBuf) - 1});
      memcpy(boldBuf, word, boldLen);
      boldBuf[boldLen] = '\0';
      renderer.drawText(fontId, drawX, wordY, boldBuf, true, boldStyle, baseDir);
      const int suffixX = drawX + focusSuffixXArr[i];
      renderer.drawText(fontId, suffixX, wordY, word + boldLen, true, currentStyle, baseDir);
    } else {
      renderer.drawText(fontId, drawX, wordY, word, true, currentStyle, baseDir);
    }

    // Horizontal ruby text rendering
    if (blockHasRuby && i < rubyTexts.size() && !rubyTexts[i].empty() &&
        (wordStyle(i) & EpdFontFamily::RUBY_CONTINUE) == 0) {
      const int rubyY = wordY - ascender;
      renderer.drawText(fontId, rubies[i].x, rubyY, rubies[i].text.c_str(), true, EpdFontFamily::SUP,
                        rubies[i].baseDir);
    }

    if (scanning) {
      continue;
    }

    if (EpdFontFamily::hasTextDecoration(currentStyle)) {
      int lineStartX = drawX;
      int lineWidth = renderer.getTextWidth(fontId, word, currentStyle, baseDir);

      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        lineWidth = (lineWidth + 1) / 2;
      }

      // Do not decorate the synthetic em-space used for paragraph indentation.
      if (wordTextLen(i) >= 3 && static_cast<uint8_t>(word[0]) == 0xE2 && static_cast<uint8_t>(word[1]) == 0x80 &&
          static_cast<uint8_t>(word[2]) == 0x83) {
        const char* visibleText = word + 3;
        lineStartX += renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        lineWidth = renderer.getTextWidth(fontId, visibleText, currentStyle, baseDir);
        if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
          lineWidth = (lineWidth + 1) / 2;
        }
      }

      for (auto& line : decorationLines) {
        if ((currentStyle & line.style) == 0) {
          flushDecoration(line);
          continue;
        }

        const int lineY = wordY + line.yOffset;
        if (line.active() && line.yPos != lineY) {
          flushDecoration(line);
        }
        if (!line.active()) {
          line.startX = lineStartX;
          line.yPos = lineY;
        }
        line.endX = lineStartX + lineWidth;
      }
    } else {
      flushDecorations();
    }
  }
  flushDecorations();
}

bool TextBlock::serialize(HalFile& file) const {
  if (!isValid) {
    LOG_ERR("TXB", "Serialization failed: invalid block");
    return false;
  }

  // Word data: scalars, then the arena verbatim -- its in-memory layout is
  // exactly the on-disk layout (see TextBlock.h), so one write covers all
  // per-word arrays and the text blob.
  serialization::writePod(file, numWords);
  serialization::writePod(file, static_cast<uint8_t>(focusPresent ? 1 : 0));
  serialization::writePod(file, static_cast<uint8_t>(isVertical ? 1 : 0));
  serialization::writePod(file, textBytes);
  if (numWords > 0) {
    const size_t size = arenaSize(numWords, focusPresent, isVertical, textBytes);
    if (file.write(arena.get(), size) != size) {
      LOG_ERR("TXB", "Serialization failed: arena write (%u bytes)", static_cast<uint32_t>(size));
      return false;
    }
  }

  // Ruby text data
  for (size_t i = 0; i < numWords; i++) {
    serialization::writeString(file, (i < rubyTexts.size()) ? rubyTexts[i] : std::string());
  }

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.isRtl);
  serialization::writePod(file, blockStyle.directionDefined);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile& file) {
  uint16_t wc;
  uint8_t hasFocus;
  uint8_t hasVertical;
  uint16_t textBytes;
  serialization::readPod(file, wc);
  serialization::readPod(file, hasFocus);
  serialization::readPod(file, hasVertical);
  serialization::readPod(file, textBytes);

  // Sanity checks: cap the arena allocation and reject impossible geometry
  // (every word carries at least its NUL terminator).
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }
  if ((wc == 0 && textBytes != 0) || (wc > 0 && textBytes < wc)) {
    LOG_ERR("TXB", "Deserialization failed: bad text size %u for %u words", textBytes, wc);
    return nullptr;
  }

  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    LOG_ERR("TXB", "OOM: TextBlock");
    return nullptr;
  }
  block->numWords = wc;
  block->textBytes = textBytes;
  block->focusPresent = hasFocus != 0;
  block->isVertical = hasVertical != 0;

  if (wc > 0) {
    const size_t size = arenaSize(wc, block->focusPresent, block->isVertical, textBytes);
    block->arena = makeUniqueNoThrow<uint8_t[]>(size);
    if (!block->arena) {
      LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
      return nullptr;
    }
    if (file.read(block->arena.get(), size) != size) {
      LOG_ERR("TXB", "Deserialization failed: arena read (%u bytes)", static_cast<uint32_t>(size));
      return nullptr;
    }
    block->bindArenaPointers();

    // Validate offsets before anything dereferences wordText(): offset 0 first,
    // strictly increasing, in bounds, and every word NUL-terminated (word i ends
    // at the byte before offset i+1; the last word at the last text byte).
    const uint16_t* textOff = block->textOffArr;
    const char* text = block->textArr;
    if (textOff[0] != 0 || text[textBytes - 1] != '\0') {
      LOG_ERR("TXB", "Deserialization failed: corrupt text layout");
      return nullptr;
    }
    for (uint16_t i = 1; i < wc; i++) {
      if (textOff[i] <= textOff[i - 1] || textOff[i] >= textBytes || text[textOff[i] - 1] != '\0') {
        LOG_ERR("TXB", "Deserialization failed: corrupt word offset %u", i);
        return nullptr;
      }
    }
  }

  // Ruby text data. Ruby is a CJK feature, so for nearly every book every entry here
  // is the empty string. Materializing the vector regardless costs wordCount * 24 bytes
  // (sizeof(std::string)) plus a heap block per line, held for as long as the page is
  // resident -- several KB of DRAM on a full page, none of it ever read. An empty
  // rubyTexts is already the "no ruby" representation: hasRuby() reports false and every
  // other reader is guarded by `i < rubyTexts.size()`, so allocate lazily and only once a
  // non-empty annotation actually shows up.
  //
  // `scratch` is reused across words: readString() resizes it to the incoming length and
  // overwrites every byte, so a moved-from value carries nothing into the next iteration.
  std::string scratch;
  for (uint16_t i = 0; i < wc; i++) {
    serialization::readString(file, scratch);
    if (scratch.empty()) continue;
    if (block->rubyTexts.empty()) {
      block->rubyTexts.resize(wc);
    }
    block->rubyTexts[i] = std::move(scratch);
  }

  // Style (alignment + margins/padding/indent)
  BlockStyle& blockStyle = block->blockStyle;
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.isRtl);
  serialization::readPod(file, blockStyle.directionDefined);

  return block;
}

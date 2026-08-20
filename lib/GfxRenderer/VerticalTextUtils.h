#pragma once

#include <cstdint>

// Vertical (tategaki / vertical-rl) text layout helpers.
//
// All tables here are `constexpr` so they live in flash (.rodata) and cost no
// DRAM — see the "constexpr First" resource rule. The functions are pure and
// engine-independent: layout (ParsedText), pagination (ChapterHtmlSlimParser),
// and drawing (GfxRenderer / TextBlock) all key off these classifiers.
namespace VerticalTextUtils {

// Character behavior in vertical text layout.
enum class VerticalBehavior : uint8_t {
  Upright,      // CJK ideographs, kana - draw normally, advance downward
  Sideways,     // Latin letters, 3+ digit numbers - rotate 90 CW
  TateChuYoko,  // 1-2 digit numbers - horizontal-in-vertical
};

// Punctuation offset for vertical text (ratio of character size, in 1/8 units).
struct PunctuationOffset {
  uint32_t codepoint;
  int8_t dxEighths;  // horizontal offset in 1/8 of charWidth
  int8_t dyEighths;  // vertical offset in 1/8 of charHeight
  bool rotate;       // true = rotate 90 CW (e.g. long vowel mark)
};

// CJK punctuation and brackets whose horizontal-layout glyph is in the wrong
// place (or the wrong orientation) for vertical text. Two mechanisms:
//
//   dx/dyEighths — translate the glyph inside its cell, shape unchanged. This
//     is what 、。，． need: the horizontal glyph sits in the bottom-left
//     quadrant of the em box and vertical typography puts it in the top-right
//     one, which is a move of half a cell right and half a cell up. Applied by
//     TextBlock::renderVertical; only the draw position moves, so the cell
//     advance, column layout, kinsoku and cached geometry are unaffected.
//
//   rotate — 90 CW rotation, which brackets and long marks need (「 opens
//     downward in vertical, ー runs along the column). Drawn through
//     GfxRenderer::drawTextSideways, the same path Latin runs use. The rotation
//     moves the ink to the right corner of the cell by itself, so these entries
//     need no offsets on top.
//
// Membership follows the Vertical_Orientation property of UAX #50: vo=R and vo=Tr
// rotate, vo=U and vo=Tu stay upright. Only characters isUprightInVertical() calls
// upright need an entry -- everything else already reaches the rotated path as a
// sideways token. To find omissions, list the codepoints in those blocks and diff
// them against VerticalOrientation.txt rather than adding them one report at a time.
// A vo=Tu character with no vertical alternate in the face (、。) is approximated by
// the offsets above; a vo=Tr one is approximated by plain rotation.
static constexpr PunctuationOffset VERTICAL_PUNCTUATION[] = {
    // Punctuation - translate from the bottom-left to the top-right quadrant
    {0x3001, 4, -4, false},  // 、 ideographic comma
    {0x3002, 4, -4, false},  // 。 ideographic period
    {0xFF0C, 4, -4, false},  // ， fullwidth comma
    {0xFF0E, 4, -4, false},  // ． fullwidth period
    // Centered in the em box in both writing modes - listed so the classifier
    // treats them as upright cells, but they need no adjustment.
    {0xFF01, 0, 0, false},  // ！ fullwidth exclamation
    {0xFF1F, 0, 0, false},  // ？ fullwidth question mark
    {0xFF1A, 0, 0, true},   // ： fullwidth colon
    {0xFF1B, 0, 0, true},   // ； fullwidth semicolon
    // Brackets - rotate so opening/closing direction matches vertical flow
    {0x300C, 0, 0, true},  // 「 left corner bracket
    {0x300D, 0, 0, true},  // 」 right corner bracket
    {0x300E, 0, 0, true},  // 『 left white corner bracket
    {0x300F, 0, 0, true},  // 』 right white corner bracket
    {0x3010, 0, 0, true},  // 【 left black lenticular bracket
    {0x3011, 0, 0, true},  // 】 right black lenticular bracket
    {0xFF08, 0, 0, true},  // （ fullwidth left paren
    {0xFF09, 0, 0, true},  // ） fullwidth right paren
    {0x3008, 0, 0, true},  // 〈 left angle bracket
    {0x3009, 0, 0, true},  // 〉 right angle bracket
    {0x300A, 0, 0, true},  // 《 left double angle bracket
    {0x300B, 0, 0, true},  // 》 right double angle bracket
    {0x3014, 0, 0, true},  // 〔 left tortoise shell bracket
    {0x3015, 0, 0, true},  // 〕 right tortoise shell bracket
    {0x3016, 0, 0, true},  // 〖 left white lenticular bracket
    {0x3017, 0, 0, true},  // 〗 right white lenticular bracket
    {0x3018, 0, 0, true},  // 〘 left white tortoise shell bracket
    {0x3019, 0, 0, true},  // 〙 right white tortoise shell bracket
    {0x301D, 0, 0, true},  // 〝 reversed double prime quotation mark
    {0x301E, 0, 0, true},  // 〞 double prime quotation mark
    {0x301F, 0, 0, true},  // 〟 low double prime quotation mark
    {0xFF3B, 0, 0, true},  // ［ fullwidth left square bracket
    {0xFF3D, 0, 0, true},  // ］ fullwidth right square bracket
    {0xFF5B, 0, 0, true},  // ｛ fullwidth left curly bracket
    {0xFF5D, 0, 0, true},  // ｝ fullwidth right curly bracket
    {0xFF5F, 0, 0, true},  // ｟ fullwidth left white parenthesis
    {0xFF60, 0, 0, true},  // ｠ fullwidth right white parenthesis
    // Comparison signs - vo=R, so they turn with the column even though neither
    // BIZ UD nor Noto Sans CJK ships a vertical alternate: rotating the upright
    // glyph is the whole of what is needed.
    {0xFF1C, 0, 0, true},  // ＜ fullwidth less-than
    {0xFF1E, 0, 0, true},  // ＞ fullwidth greater-than
    // Long marks, dashes and rules - rotate so the stroke runs along the column.
    // Every entry here is classified upright by its block (CJK symbols, katakana,
    // fullwidth forms), so without a table entry it would draw lying across the
    // column. The ones outside those blocks -- ‥ U+2025, ∼ U+223C -- already reach
    // the rotated path as sideways tokens and need no entry.
    {0x30FC, 0, 0, true},  // ー katakana long vowel mark
    {0x30A0, 0, 0, true},  // ゠ katakana-hiragana double hyphen
    {0x2014, 0, 0, true},  // — em dash
    {0x2015, 0, 0, true},  // ― horizontal bar
    {0x2026, 0, 0, true},  // … ellipsis
    {0xFF5E, 0, 0, true},  // ～ fullwidth tilde
    {0x301C, 0, 0, true},  // 〜 wave dash
    {0xFF1D, 0, 0, true},  // ＝ fullwidth equals sign
    {0xFF0D, 0, 0, true},  // － fullwidth hyphen-minus
    {0x3030, 0, 0, true},  // 〰 wavy dash
    {0xFF3F, 0, 0, true},  // ＿ fullwidth low line
    {0xFF5C, 0, 0, true},  // ｜ fullwidth vertical line
    {0xFFE3, 0, 0, true},  // ￣ fullwidth macron
};
static constexpr int VERTICAL_PUNCTUATION_COUNT = sizeof(VERTICAL_PUNCTUATION) / sizeof(VERTICAL_PUNCTUATION[0]);

// Look up punctuation offset. Returns nullptr if no special handling needed.
inline const PunctuationOffset* getVerticalPunctuationOffset(uint32_t cp) {
  for (int i = 0; i < VERTICAL_PUNCTUATION_COUNT; i++) {
    if (VERTICAL_PUNCTUATION[i].codepoint == cp) return &VERTICAL_PUNCTUATION[i];
  }
  return nullptr;
}

// An exclamation/question pair (!? !! ?? ?!) is set as one upright cell in vertical
// Japanese, the same treatment a 1-2 digit number gets. Longer ASCII runs are not:
// "Wow!!" is a Latin phrase and turns with the column like any other.
inline bool isTateChuYokoPunctuationPair(const char* text) {
  return text != nullptr && (text[0] == '!' || text[0] == '?') && (text[1] == '!' || text[1] == '?') && text[2] == '\0';
}

// Determine if a codepoint should be drawn upright in vertical text.
// CJK ideographs, kana, CJK symbols, fullwidth forms, etc.
inline bool isUprightInVertical(uint32_t cp) {
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;  // CJK Unified Ideographs
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;  // CJK Extension A
  if (cp >= 0x3040 && cp <= 0x309F) return true;  // Hiragana
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;  // Katakana
  if (cp >= 0x3000 && cp <= 0x303F) return true;  // CJK Symbols and Punctuation
  // Halfwidth and Fullwidth Forms is not one class: the fullwidth forms are upright,
  // but halfwidth katakana and halfwidth punctuation (FF61-FF9F) are vo=R in UAX #50
  // and must rotate. Excluding them here routes them to the sideways draw path, which
  // also reserves their real (half-em) advance as the cell height.
  if (cp >= 0xFF00 && cp <= 0xFF60) return true;  // Fullwidth Forms
  if (cp >= 0xFFA0 && cp <= 0xFFEF) return true;  // Halfwidth Hangul, fullwidth signs
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;  // CJK Compatibility Ideographs
  if (cp >= 0x3200 && cp <= 0x32FF) return true;  // Enclosed CJK Letters
  if (cp >= 0x3300 && cp <= 0x33FF) return true;  // CJK Compatibility
  if (cp >= 0x3100 && cp <= 0x312F) return true;  // Bopomofo
  if (cp >= 0xAC00 && cp <= 0xD7AF) return true;  // Hangul
  return false;
}

// Should this codepoint use the OpenType 'vert' substitute glyph?
// Returns true only for punctuation, brackets, and long marks that need
// a different glyph shape in vertical text. Kana and ideographs are excluded
// because their vert variants differ only in metrics (designed for use with
// a full shaping engine), and bitmap-only substitution looks wrong.
inline bool shouldUseVertGlyph(uint32_t cp) {
  // CJK punctuation and brackets (3000-303F), excluding ideographs like 〆(3006)
  if (cp == 0x3001 || cp == 0x3002) return true;  // 、。
  if (cp >= 0x3008 && cp <= 0x3011) return true;  // 〈〉《》「」『』【】
  if (cp >= 0x3014 && cp <= 0x301B) return true;  // 〔〕〖〗〘〙〚〛
  if (cp >= 0x301D && cp <= 0x301F) return true;  // 〝〞〟
  // Fullwidth punctuation and brackets
  if (cp == 0xFF01 || cp == 0xFF1F) return true;  // ！？
  if (cp == 0xFF08 || cp == 0xFF09) return true;  // （）
  if (cp == 0xFF0C || cp == 0xFF0E) return true;  // ，．
  if (cp == 0xFF1A || cp == 0xFF1B) return true;  // ：；
  if (cp == 0xFF3B || cp == 0xFF3D) return true;  // ［］
  if (cp == 0xFF5B || cp == 0xFF5D) return true;  // ｛｝
  if (cp == 0xFF5E) return true;                  // ～
  // Long marks and dashes
  if (cp == 0x30FC) return true;                  // ー
  if (cp == 0x2014 || cp == 0x2015) return true;  // —―
  if (cp == 0x2025 || cp == 0x2026) return true;  // ‥…
  if (cp == 0x22EF) return true;                  // ⋯
  return false;
}

// Kinsoku (禁則) processing for vertical text column breaks.
// Returns true if this codepoint must NOT appear at the start of a column.
inline bool isKinsokuHead(uint32_t cp) {
  // Closing brackets and punctuation (行頭禁止)
  if (cp == 0x3001 || cp == 0x3002) return true;                                  // 、。
  if (cp == 0x300D || cp == 0x300F || cp == 0x3011) return true;                  // 」』】
  if (cp == 0x3015 || cp == 0x3017 || cp == 0x3019 || cp == 0x301B) return true;  // 〕〗〙〛
  if (cp == 0xFF09 || cp == 0xFF3D || cp == 0xFF5D) return true;                  // ）］｝
  if (cp == 0xFF0C || cp == 0xFF0E) return true;                                  // ，．
  if (cp == 0xFF01 || cp == 0xFF1F) return true;                                  // ！？
  if (cp == 0xFF1A || cp == 0xFF1B) return true;                                  // ：；
  if (cp == 0x3009 || cp == 0x300B) return true;                                  // 〉》
  // Small kana (行頭禁止)
  if (cp == 0x3041 || cp == 0x3043 || cp == 0x3045 || cp == 0x3047 || cp == 0x3049) return true;  // ぁぃぅぇぉ
  if (cp == 0x3063 || cp == 0x3083 || cp == 0x3085 || cp == 0x3087) return true;                  // っゃゅょ
  if (cp == 0x30A1 || cp == 0x30A3 || cp == 0x30A5 || cp == 0x30A7 || cp == 0x30A9) return true;  // ァィゥェォ
  if (cp == 0x30C3 || cp == 0x30E3 || cp == 0x30E5 || cp == 0x30E7) return true;                  // ッャュョ
  if (cp == 0x30FC) return true;                                                                  // ー
  return false;
}

// Returns true if this codepoint must NOT appear at the end of a column.
inline bool isKinsokuTail(uint32_t cp) {
  // Opening brackets (行末禁止)
  if (cp == 0x300C || cp == 0x300E || cp == 0x3010) return true;                  // 「『【
  if (cp == 0x3014 || cp == 0x3016 || cp == 0x3018 || cp == 0x301A) return true;  // 〔〖〘〚
  if (cp == 0xFF08 || cp == 0xFF3B || cp == 0xFF5B) return true;                  // （［｛
  if (cp == 0x3008 || cp == 0x300A) return true;                                  // 〈《
  return false;
}

}  // namespace VerticalTextUtils

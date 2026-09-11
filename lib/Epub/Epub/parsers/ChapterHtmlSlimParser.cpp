#include "ChapterHtmlSlimParser.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <new>

#include "../../../../src/fontIds.h"
#include "../InlineImageToken.h"
#if INPUT_DIAG
#include "../../../../src/util/InputDiag.h"
// Tail of the src attribute, so /image-diag.txt lines identify the image without
// blowing the 96-byte event budget on directory prefixes.
#define IMG_DIAG(fmt, ...)                                        \
  do {                                                            \
    char imgDiagBuf[72];                                          \
    snprintf(imgDiagBuf, sizeof(imgDiagBuf), fmt, ##__VA_ARGS__); \
    InputDiag::noteImageEvent(imgDiagBuf);                        \
  } while (0)
#else
#define IMG_DIAG(fmt, ...)
#endif
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/VisibleTextUtils.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/converters/PngStreamDecoder.h"
#include "Epub/htmlEntities.h"

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;

// This number comes from PR #73
// If we have > 750 words buffered up, perform the layout and consume out all but the last line
// There should be enough here to build out 1-2 full pages and doing this will free up a lot of
// memory.
// Spotted when reading Intermezzo, there are some really long text blocks in there.
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS = 750;

// When CSS is enabled, flush earlier to save RAM. 320 is still more than enough to build a CJK
// page at font size 14
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS = 320;

// Hard cap on the number of anchor IDs recorded per chapter. Legitimate navigation
// anchors (TOC entries, footnotes, cross-references) rarely exceed a few hundred per
// chapter. A runaway count usually means a converter injected machine-generated IDs on
// every text fragment (e.g. Kobo KePub spans). The cap prevents unbounded heap growth
// on resource-constrained devices (~380KB heap). TOC anchors bypass this cap.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;

// Reuse serializable PageLine/PageHorizontalRule elements for a small grid.
constexpr int16_t TABLE_CELL_HORIZONTAL_PADDING = 4;
constexpr int16_t TABLE_ROW_SEPARATOR_GAP = 4;
constexpr uint8_t TABLE_ROW_SEPARATOR_THICKNESS = 1;
constexpr int16_t TABLE_MIN_CELL_WIDTH_LINE_HEIGHTS = 3;

// Column pitch in vertical writing, as a multiple of the full-width cell, before the reader's
// line-spacing setting scales it. JLREQ 2.3 sets the line pitch as a ratio of the character
// size (body text runs about 1.5 to 1.75 em); the pitch used to be the font's own line height
// plus a quarter, which is not a fixed ratio at all -- BIZUDGothic_12 came out at 1.25 em and
// NotoSansJP_12 at 1.8 em from the same setting, so changing font silently changed how much
// text a page held. With the four line-spacing steps (0.95/1.0/1.1/1.2) this spans 1.43 to
// 1.8 em, which covers the range JLREQ describes.
constexpr float VERTICAL_COLUMN_PITCH_EM = 1.5f;
// Cap on the marks a single horizontal word gets. Japanese bouten runs are short; a longer
// word is almost always Latin, where the per-character annotation buys nothing and the
// spacer string would outweigh the word.
constexpr size_t MAX_EMPHASIS_CODEPOINTS_PER_WORD = 24;

constexpr const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr const char* BOLD_TAGS[] = {"b", "strong"};
constexpr const char* ITALIC_TAGS[] = {"i", "em"};
constexpr const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr const char* LINETHROUGH_TAGS[] = {"del", "s", "strike"};
constexpr const char* IMAGE_TAGS[] = {"img", "image"};
bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// CJK Compatibility Ideographs (U+F900-FAFF) -> the unified ideograph each one
// decomposes to, 0 where there is none or it lives outside the BMP (no reading face
// carries those). Publishers' typesetting systems reach for a compatibility codepoint
// to pin a particular glyph shape, but a face that never drew that shape has nothing
// there: BIZ UD has 蓮 U+F999 and neither 溺 U+F9EC nor 煉 U+F993, so a book using them
// drew the replacement box. The character is the same either way, so falling back to
// the unified form shows the word instead of a hole.
//
// 512 uint16 = 1 KB of flash, no RAM: the table is only read on the rare codepoint
// that lands in the block.
constexpr uint16_t CJK_COMPAT_UNIFIED[512] = {
    0x8C48, 0x66F4, 0x8ECA, 0x8CC8, 0x6ED1, 0x4E32, 0x53E5, 0x9F9C,  // U+F900
    0x9F9C, 0x5951, 0x91D1, 0x5587, 0x5948, 0x61F6, 0x7669, 0x7F85,  // U+F908
    0x863F, 0x87BA, 0x88F8, 0x908F, 0x6A02, 0x6D1B, 0x70D9, 0x73DE,  // U+F910
    0x843D, 0x916A, 0x99F1, 0x4E82, 0x5375, 0x6B04, 0x721B, 0x862D,  // U+F918
    0x9E1E, 0x5D50, 0x6FEB, 0x85CD, 0x8964, 0x62C9, 0x81D8, 0x881F,  // U+F920
    0x5ECA, 0x6717, 0x6D6A, 0x72FC, 0x90CE, 0x4F86, 0x51B7, 0x52DE,  // U+F928
    0x64C4, 0x6AD3, 0x7210, 0x76E7, 0x8001, 0x8606, 0x865C, 0x8DEF,  // U+F930
    0x9732, 0x9B6F, 0x9DFA, 0x788C, 0x797F, 0x7DA0, 0x83C9, 0x9304,  // U+F938
    0x9E7F, 0x8AD6, 0x58DF, 0x5F04, 0x7C60, 0x807E, 0x7262, 0x78CA,  // U+F940
    0x8CC2, 0x96F7, 0x58D8, 0x5C62, 0x6A13, 0x6DDA, 0x6F0F, 0x7D2F,  // U+F948
    0x7E37, 0x964B, 0x52D2, 0x808B, 0x51DC, 0x51CC, 0x7A1C, 0x7DBE,  // U+F950
    0x83F1, 0x9675, 0x8B80, 0x62CF, 0x6A02, 0x8AFE, 0x4E39, 0x5BE7,  // U+F958
    0x6012, 0x7387, 0x7570, 0x5317, 0x78FB, 0x4FBF, 0x5FA9, 0x4E0D,  // U+F960
    0x6CCC, 0x6578, 0x7D22, 0x53C3, 0x585E, 0x7701, 0x8449, 0x8AAA,  // U+F968
    0x6BBA, 0x8FB0, 0x6C88, 0x62FE, 0x82E5, 0x63A0, 0x7565, 0x4EAE,  // U+F970
    0x5169, 0x51C9, 0x6881, 0x7CE7, 0x826F, 0x8AD2, 0x91CF, 0x52F5,  // U+F978
    0x5442, 0x5973, 0x5EEC, 0x65C5, 0x6FFE, 0x792A, 0x95AD, 0x9A6A,  // U+F980
    0x9E97, 0x9ECE, 0x529B, 0x66C6, 0x6B77, 0x8F62, 0x5E74, 0x6190,  // U+F988
    0x6200, 0x649A, 0x6F23, 0x7149, 0x7489, 0x79CA, 0x7DF4, 0x806F,  // U+F990
    0x8F26, 0x84EE, 0x9023, 0x934A, 0x5217, 0x52A3, 0x54BD, 0x70C8,  // U+F998
    0x88C2, 0x8AAA, 0x5EC9, 0x5FF5, 0x637B, 0x6BAE, 0x7C3E, 0x7375,  // U+F9A0
    0x4EE4, 0x56F9, 0x5BE7, 0x5DBA, 0x601C, 0x73B2, 0x7469, 0x7F9A,  // U+F9A8
    0x8046, 0x9234, 0x96F6, 0x9748, 0x9818, 0x4F8B, 0x79AE, 0x91B4,  // U+F9B0
    0x96B8, 0x60E1, 0x4E86, 0x50DA, 0x5BEE, 0x5C3F, 0x6599, 0x6A02,  // U+F9B8
    0x71CE, 0x7642, 0x84FC, 0x907C, 0x9F8D, 0x6688, 0x962E, 0x5289,  // U+F9C0
    0x677B, 0x67F3, 0x6D41, 0x6E9C, 0x7409, 0x7559, 0x786B, 0x7D10,  // U+F9C8
    0x985E, 0x516D, 0x622E, 0x9678, 0x502B, 0x5D19, 0x6DEA, 0x8F2A,  // U+F9D0
    0x5F8B, 0x6144, 0x6817, 0x7387, 0x9686, 0x5229, 0x540F, 0x5C65,  // U+F9D8
    0x6613, 0x674E, 0x68A8, 0x6CE5, 0x7406, 0x75E2, 0x7F79, 0x88CF,  // U+F9E0
    0x88E1, 0x91CC, 0x96E2, 0x533F, 0x6EBA, 0x541D, 0x71D0, 0x7498,  // U+F9E8
    0x85FA, 0x96A3, 0x9C57, 0x9E9F, 0x6797, 0x6DCB, 0x81E8, 0x7ACB,  // U+F9F0
    0x7B20, 0x7C92, 0x72C0, 0x7099, 0x8B58, 0x4EC0, 0x8336, 0x523A,  // U+F9F8
    0x5207, 0x5EA6, 0x62D3, 0x7CD6, 0x5B85, 0x6D1E, 0x66B4, 0x8F3B,  // U+FA00
    0x884C, 0x964D, 0x898B, 0x5ED3, 0x5140, 0x55C0, 0x0000, 0x0000,  // U+FA08
    0x585A, 0x0000, 0x6674, 0x0000, 0x0000, 0x51DE, 0x732A, 0x76CA,  // U+FA10
    0x793C, 0x795E, 0x7965, 0x798F, 0x9756, 0x7CBE, 0x7FBD, 0x0000,  // U+FA18
    0x8612, 0x0000, 0x8AF8, 0x0000, 0x0000, 0x9038, 0x90FD, 0x0000,  // U+FA20
    0x0000, 0x0000, 0x98EF, 0x98FC, 0x9928, 0x9DB4, 0x90DE, 0x96B7,  // U+FA28
    0x4FAE, 0x50E7, 0x514D, 0x52C9, 0x52E4, 0x5351, 0x559D, 0x5606,  // U+FA30
    0x5668, 0x5840, 0x58A8, 0x5C64, 0x5C6E, 0x6094, 0x6168, 0x618E,  // U+FA38
    0x61F2, 0x654F, 0x65E2, 0x6691, 0x6885, 0x6D77, 0x6E1A, 0x6F22,  // U+FA40
    0x716E, 0x722B, 0x7422, 0x7891, 0x793E, 0x7949, 0x7948, 0x7950,  // U+FA48
    0x7956, 0x795D, 0x798D, 0x798E, 0x7A40, 0x7A81, 0x7BC0, 0x7DF4,  // U+FA50
    0x7E09, 0x7E41, 0x7F72, 0x8005, 0x81ED, 0x8279, 0x8279, 0x8457,  // U+FA58
    0x8910, 0x8996, 0x8B01, 0x8B39, 0x8CD3, 0x8D08, 0x8FB6, 0x9038,  // U+FA60
    0x96E3, 0x97FF, 0x983B, 0x6075, 0x0000, 0x8218, 0x0000, 0x0000,  // U+FA68
    0x4E26, 0x51B5, 0x5168, 0x4F80, 0x5145, 0x5180, 0x52C7, 0x52FA,  // U+FA70
    0x559D, 0x5555, 0x5599, 0x55E2, 0x585A, 0x58B3, 0x5944, 0x5954,  // U+FA78
    0x5A62, 0x5B28, 0x5ED2, 0x5ED9, 0x5F69, 0x5FAD, 0x60D8, 0x614E,  // U+FA80
    0x6108, 0x618E, 0x6160, 0x61F2, 0x6234, 0x63C4, 0x641C, 0x6452,  // U+FA88
    0x6556, 0x6674, 0x6717, 0x671B, 0x6756, 0x6B79, 0x6BBA, 0x6D41,  // U+FA90
    0x6EDB, 0x6ECB, 0x6F22, 0x701E, 0x716E, 0x77A7, 0x7235, 0x72AF,  // U+FA98
    0x732A, 0x7471, 0x7506, 0x753B, 0x761D, 0x761F, 0x76CA, 0x76DB,  // U+FAA0
    0x76F4, 0x774A, 0x7740, 0x78CC, 0x7AB1, 0x7BC0, 0x7C7B, 0x7D5B,  // U+FAA8
    0x7DF4, 0x7F3E, 0x8005, 0x8352, 0x83EF, 0x8779, 0x8941, 0x8986,  // U+FAB0
    0x8996, 0x8ABF, 0x8AF8, 0x8ACB, 0x8B01, 0x8AFE, 0x8AED, 0x8B39,  // U+FAB8
    0x8B8A, 0x8D08, 0x8F38, 0x9072, 0x9199, 0x9276, 0x967C, 0x96E3,  // U+FAC0
    0x9756, 0x97DB, 0x97FF, 0x980B, 0x983B, 0x9B12, 0x9F9C, 0x0000,  // U+FAC8
    0x0000, 0x0000, 0x3B9D, 0x4018, 0x4039, 0x0000, 0x0000, 0x0000,  // U+FAD0
    0x9F43, 0x9F8E, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,  // U+FAD8
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,  // U+FAE0
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,  // U+FAE8
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,  // U+FAF0
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,  // U+FAF8
};

// Unified form for a compatibility ideograph, 0 when there is no BMP one.
uint32_t unifiedIdeographFor(const uint32_t cp) {
  if (cp < 0xF900 || cp > 0xFAFF) return 0;
  return CJK_COMPAT_UNIFIED[cp - 0xF900];
}

// UTF-8 mark glyph for a text-emphasis style, nullptr for none. The sesame forms are the
// Vertical Forms codepoints; every mark here is inside the coverage the reading faces
// already carry for kutouten and enclosed CJK.
const char* emphasisMarkUtf8(const CssTextEmphasis e) {
  switch (e) {
    case CssTextEmphasis::FilledDot:
      return "\xE2\x80\xA2";  // •
    case CssTextEmphasis::OpenDot:
      return "\xE2\x97\xA6";  // ◦
    case CssTextEmphasis::FilledCircle:
      return "\xE2\x97\x8F";  // ●
    case CssTextEmphasis::OpenCircle:
      return "\xE2\x97\x8B";  // ○
    case CssTextEmphasis::FilledSesame:
      return "\xEF\xB9\x85";  // ﹅
    case CssTextEmphasis::OpenSesame:
      return "\xEF\xB9\x86";  // ﹆
    case CssTextEmphasis::FilledTriangle:
      return "\xE2\x96\xB2";  // ▲
    case CssTextEmphasis::OpenTriangle:
      return "\xE2\x96\xB3";  // △
    case CssTextEmphasis::FilledDoubleCircle:
      return "\xE2\x97\x89";  // ◉
    case CssTextEmphasis::OpenDoubleCircle:
      return "\xE2\x97\x8E";  // ◎
    default:
      return nullptr;
  }
}

std::string trimAndNormalize(const std::string& str) {
  if (str.empty()) return "";
  size_t start = 0;
  while (start < str.size() && isWhitespace(str[start])) {
    start++;
  }
  if (start == str.size()) return "";
  size_t end = str.size() - 1;
  while (end > start && isWhitespace(str[end])) {
    end--;
  }
  std::string result;
  result.reserve(end - start + 1);
  bool inSpace = false;
  for (size_t i = start; i <= end; i++) {
    if (isWhitespace(str[i])) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(str[i]);
      inSpace = false;
    }
  }
  return result;
}

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

bool isNonVisibleTextTag(const char* name) { return VisibleTextUtils::isNonVisibleElement(name); }

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

uint16_t parseTableSpan(const char* value) {
  if (!value || value[0] == '\0') return 1;

  uint32_t span = 0;
  for (const char* current = value; *current != '\0'; ++current) {
    if (*current < '0' || *current > '9') return 1;
    const uint32_t digit = static_cast<uint32_t>(*current - '0');
    if (span > (UINT16_MAX - digit) / 10) return UINT16_MAX;
    span = span * 10 + digit;
  }
  return span == 0 ? UINT16_MAX : static_cast<uint16_t>(span);
}

// Returns true if the HTML element is a purely inline, non-navigable wrapper.
// IDs on these elements are never meaningful navigation targets in epub content.
// Reading-system converters (Kobo KePub, Calibre, etc.) frequently inject thousands
// of such IDs for progress tracking or internal bookkeeping, and recording each one
// as a navigation anchor exhausts the heap on memory-constrained devices.
// Block-level, sectioning, and structural elements are always considered navigable.
bool isNonNavigableInlineElement(const char* name) { return strcmp(name, "span") == 0; }

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS));
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

void ChapterHtmlSlimParser::applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasDirection()) {
    entry.hasDirection = true;
    entry.direction = css.direction;
  }
}

EpdFontFamily::Style ChapterHtmlSlimParser::fontStyleForTextDecoration(const CssTextDecoration decoration) {
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  if ((decoration & CssTextDecoration::Underline) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);
  }
  if ((decoration & CssTextDecoration::LineThrough) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::STRIKETHROUGH);
  }
  return style;
}

void ChapterHtmlSlimParser::applyTextEmphasisToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasTextEmphasis()) {
    entry.hasEmphasis = true;
    entry.emphasis = css.textEmphasis;
  }
}

void ChapterHtmlSlimParser::applyTextOrientationToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasTextOrientation()) {
    entry.hasOrientation = true;
    entry.orientation = css.textOrientation;
  }
}

void ChapterHtmlSlimParser::applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasTextDecoration()) {
    entry.hasTextDecoration = true;
    entry.textDecoration = css.textDecoration;
  }
}

void ChapterHtmlSlimParser::applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (!css.hasVerticalAlign()) return;
  if (css.verticalAlign == CssVerticalAlign::Super) {
    entry.hasSup = true;
    entry.sup = true;
  } else if (css.verticalAlign == CssVerticalAlign::Sub) {
    entry.hasSub = true;
    entry.sub = true;
  }
}

void ChapterHtmlSlimParser::pushTableTextStyleEntry(const CssStyle& cssStyle) {
  if (!cssStyle.hasFontWeight() && !cssStyle.hasFontStyle() && !cssStyle.hasTextDecoration() &&
      !cssStyle.hasDirection() && !cssStyle.hasTextAlign()) {
    return;
  }

  StyleStackEntry entry;
  entry.depth = depth;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyTextDecorationToEntry(entry, cssStyle);
  applyDirectionToEntry(entry, cssStyle);
  entry.setsParagraphDirection = true;
  if (cssStyle.hasTextAlign()) {
    entry.hasTextAlign = true;
    entry.textAlign = cssStyle.textAlign;
  }
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

void ChapterHtmlSlimParser::pushDecorationStyleEntry(const CssTextDecoration defaultDecoration,
                                                     const CssStyle& cssStyle) {
  StyleStackEntry entry;
  entry.depth = depth;
  entry.hasTextDecoration = true;
  entry.textDecoration = cssStyle.hasTextDecoration() ? cssStyle.textDecoration : defaultDecoration;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyDirectionToEntry(entry, cssStyle);
  applyTextEmphasisToEntry(entry, cssStyle);
  applyTextOrientationToEntry(entry, cssStyle);
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

// Update effective bold/italic/decorations based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveTextDecoration =
      currentCssStyle.hasTextDecoration() ? currentCssStyle.textDecoration : CssTextDecoration::None;
  bool paragraphDirectionDefined = false;
  bool paragraphIsRtl = false;
  if (!blockStyleStack.empty()) {
    const auto& blockStyle = blockStyleStack.back();
    paragraphDirectionDefined = blockStyle.directionDefined;
    paragraphIsRtl = blockStyle.isRtl;
  }
  effectiveDirectionDefined = paragraphDirectionDefined;
  effectiveDirection = paragraphIsRtl ? CssTextDirection::Rtl : CssTextDirection::Ltr;
  effectiveTextAlignDefined = currentCssStyle.hasTextAlign();
  effectiveTextAlign = currentCssStyle.textAlign;
  effectiveSup = false;
  effectiveSub = false;
  effectiveEmphasis = currentCssStyle.hasTextEmphasis() ? currentCssStyle.textEmphasis : CssTextEmphasis::None;
  effectiveOrientation =
      currentCssStyle.hasTextOrientation() ? currentCssStyle.textOrientation : CssTextOrientation::Mixed;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    // CSS line decorations propagate through descendants; child entries add
    // their own lines but cannot cancel an ancestor's already active line.
    if (entry.hasTextDecoration) {
      effectiveTextDecoration = effectiveTextDecoration | entry.textDecoration;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
      if (entry.setsParagraphDirection) {
        paragraphDirectionDefined = true;
        paragraphIsRtl = entry.direction == CssTextDirection::Rtl;
      }
    }
    if (entry.hasTextAlign) {
      effectiveTextAlignDefined = true;
      effectiveTextAlign = entry.textAlign;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
    // Unlike line decorations, a descendant's "none" cancels an ancestor's mark.
    if (entry.hasEmphasis) {
      effectiveEmphasis = entry.emphasis;
    }
    if (entry.hasOrientation) {
      effectiveOrientation = entry.orientation;
    }
  }

  // Keep flow direction in the active empty text block. Inline direction remains
  // available for CSS inheritance without replacing the paragraph's base direction.
  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    style.directionDefined = paragraphDirectionDefined;
    style.isRtl = paragraphIsRtl;
  }
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    }
  }

  // Hold the anchor until a line of its block has actually been placed. completedPageCount here
  // is the page in progress, and the block's first line may not fit on it -- addLineToPage then
  // emits that page and puts the line on the next one. A TOC anchor came out right before this
  // change only because it forces the break above first; nothing else did.
  if (anchorsAwaitingPlacement.size() < MAX_ANCHORS_AWAITING_PLACEMENT) {
    anchorsAwaitingPlacement.push_back(std::move(pendingAnchorId));
  } else {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
  }
  pendingAnchorId.clear();
}

void ChapterHtmlSlimParser::commitAnchorsAwaitingPlacement() {
  if (anchorsAwaitingPlacement.empty()) return;
  for (auto& anchor : anchorsAwaitingPlacement) {
    anchorData.push_back({std::move(anchor), static_cast<uint16_t>(completedPageCount)});
  }
  anchorsAwaitingPlacement.clear();
}

void ChapterHtmlSlimParser::setCurrentPageVisibleOffset(const uint32_t offset) {
  if (currentPageVisibleOffsetSet) return;
  // The first page always begins at the start of the body, even when the XHTML
  // contains leading formatting whitespace before its first rendered word.
  currentPageVisibleOffset = completedPageCount == 0 ? 0 : offset;
  currentPageVisibleOffsetSet = true;
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (!currentTextBlock) {
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return;
  }

  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | fontStyleForTextDecoration(effectiveTextDecoration));
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  substituteMissingCompatibilityIdeographs();
  const size_t wordBytes = static_cast<size_t>(partWordBufferIndex);
  // Upstream's grid tables lay cells out in horizontal-model coordinates; a vertical page has
  // no place for that, so vertical books keep the stacked (flattened) table form.
  if (insideTableCell && !tableRowStacked &&
      (isVertical || tableCellTextBytes + wordBytes > MAX_GRID_TABLE_CELL_BYTES)) {
    fallbackTableRowToStacked();
  }
  if (isVertical) {
    // Spend the pending whitespace run as a separator token.
    //
    // characterData drops HTML whitespace as a bare word boundary, which is right for horizontal
    // layout: that path re-inserts the gap at layout time via getSpaceAdvance() between words that
    // do not continue. Vertical layout has no such step — layoutVerticalColumns stacks each token by
    // its own advance and nothing else — so with no token to carry it the space simply vanished and
    // Latin phrases came out run together ("character length calculator" as one string).
    //
    // Emitting a token here rather than adding a rule to the vertical layout keeps the two writing
    // modes agreeing on where a space belongs, instead of giving vertical its own notion of it.
    //
    // Guarded on a non-empty buffer so an empty flush (an inline tag boundary, say) neither spends
    // the run nor emits a trailing separator, and on a non-empty block so a run that opens a
    // paragraph is discarded the way CSS collapsing discards it.
    if (partWordBufferIndex > 0) {
      if (pendingVerticalWhitespace && currentTextBlock && !currentTextBlock->isEmpty()) {
        currentTextBlock->addVerticalToken(" ", fontStyle, VerticalTextUtils::VerticalBehavior::Sideways);
      }
      pendingVerticalWhitespace = false;
    }
    // Vertical layout tokenizes per codepoint: each glyph is its own cell, classified
    // (upright CJK / sideways Latin / tate-chu-yoko digits) so layoutVerticalColumns can
    // stack and orient it. Latin runs and 1-2 digit numbers are grouped into one token.
    // Per-token visible-offset tracking is not implemented for this path (see
    // addColumnToPage for the page-granularity fallback used instead).
    flushPartWordBufferVertical(fontStyle);
  } else {
    uint8_t linkId = 0;
    if (insideFootnoteLink) {
      if (!currentTextBlock->linkTargetMatches(currentFootnoteLinkId, currentFootnote.href)) {
        currentFootnoteLinkId = currentTextBlock->addLinkTarget(currentFootnote.href);
      }
      linkId = currentFootnoteLinkId;
    }
    const size_t wordIndex = currentTextBlock->size();
    currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues, partWordVisibleOffset, linkId);
    applyHorizontalEmphasis(wordIndex);
    if (insideTableCell && !tableRowStacked) {
      tableCellTextBytes += wordBytes;
      if (currentTextBlock->size() > MAX_GRID_TABLE_CELL_WORDS) {
        fallbackTableRowToStacked();
      }
    }
  }
  partWordBufferIndex = 0;
  nextWordContinues = false;
  listItemBulletOnly = false;
}

// Attach bouten to a word the horizontal path just added, as a synthetic ruby annotation.
//
// The spacer is what makes the marks line up. Ruby draws in SUP style, which the renderer
// puts out at 50% scale, and one annotation is centred over the whole word, so bare marks
// advance only half a cell each and bunch into the middle of the run they mark. U+3000 is
// full-width, so at SUP it is the other half: mark + space is exactly one character cell,
// making the annotation as wide as the word and landing one mark per character.
//
// U+3000 shares its block with the brackets and kutouten on every page, so it is present
// in any face that can render the text being marked.
void ChapterHtmlSlimParser::applyHorizontalEmphasis(const size_t wordIndex) {
  const char* mark = resolveEmphasisMark(effectiveEmphasis);
  if (!mark) return;

  size_t codepoints = 0;
  for (int i = 0; i < partWordBufferIndex; i++) {
    if ((static_cast<unsigned char>(partWordBuffer[i]) & 0xC0) != 0x80) codepoints++;
  }
  // Long runs would build a string bigger than the word itself for no legibility gain.
  if (codepoints == 0 || codepoints > MAX_EMPHASIS_CODEPOINTS_PER_WORD) return;

  static constexpr char IDEOGRAPHIC_SPACE[] = "\xE3\x80\x80";  // U+3000
  constexpr size_t SPACE_LEN = sizeof(IDEOGRAPHIC_SPACE) - 1;
  const size_t markLen = strlen(mark);

  // addWord splits CJK text into one token per character, so the run this flush produced is
  // words[wordIndex .. size()). Each token gets as many marks as it has codepoints: one per kanji
  // or kana, several for a Latin or digit token that stayed whole. Hanging the whole run's marks
  // on the first token (the previous form) made that one character carry a ruby wider than the
  // line, and the horizontal ruby layout then spread its neighbours across the page to fit it
  // (seen 2026-09-06 on test_horizontal_ja.epub: 「は 一 文」 with thirteen marks above).
  const size_t tokenEnd = currentTextBlock->size();
  for (size_t t = wordIndex; t < tokenEnd; t++) {
    const std::string& token = currentTextBlock->wordAt(t);
    size_t tokenCodepoints = 0;
    for (const char c : token) {
      if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) tokenCodepoints++;
    }
    if (tokenCodepoints == 0) continue;
    std::string marks;
    marks.reserve((markLen + SPACE_LEN) * tokenCodepoints);
    for (size_t i = 0; i < tokenCodepoints; i++) {
      marks.append(mark, markLen);
      if (i + 1 < tokenCodepoints) marks.append(IDEOGRAPHIC_SPACE, SPACE_LEN);
    }
    currentTextBlock->setRubyForWordAt(t, marks);
  }
}

// Tokenize the pending buffer into vertical cells. Emits one token per CJK/upright
// codepoint; consecutive ASCII letters coalesce into a Sideways run and 1-2 digit
// numbers into a TateChuYoko token (3+ digits fall back to Sideways). A number keeps
// any separator standing between two of its digits, so 3.14 and 12:34 are one cell each
// and cannot be broken across a column.
void ChapterHtmlSlimParser::flushPartWordBufferVertical(const EpdFontFamily::Style fontStyle) {
  // Vertical layout emits roughly one token per codepoint, so a full buffer becomes a burst of
  // pushes. Reserve up front (worst case one token per byte) so the parallel arrays grow once.
  currentTextBlock->ensureTokenCapacity(static_cast<size_t>(partWordBufferIndex));

  // Bouten ride the ruby path, so they need the token range this flush produces.
  const char* emphasisMark = resolveEmphasisMark(effectiveEmphasis);
  const size_t emphasisFirstToken = emphasisMark ? currentTextBlock->size() : 0;
  // One mark per token. Vertical layout already gives every upright codepoint its own
  // cell, so a per-token annotation lands one mark beside one character with no spacing
  // trick needed; a coalesced Latin or tate-chu-yoko run takes a single mark over the
  // cell it occupies. The mark is 3 UTF-8 bytes, inside the small-string buffer, so this
  // costs no allocation per token.
  const auto applyEmphasis = [&]() {
    if (!emphasisMark) return;
    const size_t tokenEnd = currentTextBlock->size();
    for (size_t i = emphasisFirstToken; i < tokenEnd; i++) {
      currentTextBlock->setRubyForWordAt(i, emphasisMark);
    }
  };

  // CSS text-orientation / text-combine-upright on this run (the EBPAJ upright / sideways /
  // tcy classes). The classifier below decides per character; these decide for the span.
  if (effectiveOrientation != CssTextOrientation::Mixed && flushForcedOrientation(fontStyle, effectiveOrientation)) {
    applyEmphasis();
    return;
  }

  const auto* p = reinterpret_cast<const unsigned char*>(partWordBuffer);
  const auto* end = p + partWordBufferIndex;
  while (p < end) {
    const unsigned char* cpStart = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    if (VerticalTextUtils::isUprightInVertical(cp) || VerticalTextUtils::getVerticalPunctuationOffset(cp) != nullptr) {
      // Upright CJK/kana/punctuation: one cell each. 、。， become their Vertical
      // Forms counterparts when the reading face carries those glyphs, so the
      // draw path gets a real vertical glyph instead of the shifted horizontal one.
      const uint32_t formCp = VerticalTextUtils::verticalPresentationForm(cp);
      if (formCp != 0 && fontHasVerticalForm(formCp)) {
        std::string token;
        utf8AppendCodepoint(formCp, token);
        currentTextBlock->addVerticalToken(std::move(token), fontStyle, VerticalTextUtils::VerticalBehavior::Upright);
        continue;
      }
      currentTextBlock->addVerticalToken(std::string(reinterpret_cast<const char*>(cpStart), p - cpStart), fontStyle,
                                         VerticalTextUtils::VerticalBehavior::Upright);
      continue;
    }

    // Coalesce a run of ASCII digits or letters into a single sideways/tate-chu-yoko token.
    const bool isDigit = (cp >= '0' && cp <= '9');
    const unsigned char* runStart = cpStart;
    const unsigned char* runEnd = p;
    int runChars = 1;
    while (runEnd < end) {
      const unsigned char* peek = runEnd;
      const uint32_t next = utf8NextCodepoint(&peek);
      const bool nextDigit = (next >= '0' && next <= '9');
      const bool nextAscii = (next >= '!' && next <= '~');
      if (isDigit) {
        if (nextDigit) {
          runEnd = peek;
          runChars++;
          continue;
        }
        // A separator standing between two digits is part of the number, not a break in
        // it: 3.14, 12:34, 1,000, 3/4. Left out of the run, each of those became three
        // cells with a column break free to fall between them, and 3.14 duly came back
        // from the device split across two columns. The digit on the far side is what
        // makes it safe -- the full stop ending a sentence has no digit after it, so it
        // is not swallowed, and neither is the colon introducing a quotation.
        if (next == '.' || next == ',' || next == ':' || next == '/') {
          const unsigned char* after = peek;
          if (after < end) {
            const uint32_t following = utf8NextCodepoint(&after);
            if (following >= '0' && following <= '9') {
              runEnd = after;
              runChars += 2;
              continue;
            }
          }
        }
        break;
      }
      if (nextAscii && !nextDigit) {
        runEnd = peek;
        runChars++;
        continue;
      }
      break;
    }
    p = runEnd;
    std::string token(reinterpret_cast<const char*>(runStart), runEnd - runStart);
    // 1-2 digits and an exclamation/question pair share one upright cell; every other
    // ASCII run turns with the column.
    const bool tateChuYoko =
        (isDigit && runChars <= 2) || VerticalTextUtils::isTateChuYokoPunctuationPair(token.c_str());
    const auto behavior =
        tateChuYoko ? VerticalTextUtils::VerticalBehavior::TateChuYoko : VerticalTextUtils::VerticalBehavior::Sideways;

    if (behavior == VerticalTextUtils::VerticalBehavior::Sideways) {
      addSidewaysToken(std::move(token), fontStyle);
      continue;
    }

    currentTextBlock->addVerticalToken(std::move(token), fontStyle, behavior);
  }

  applyEmphasis();
}

// A sideways run occupies its own WIDTH as the column's vertical extent, and the tokenizer
// coalesces every unbroken ASCII stretch into one token -- so a path or an identifier with
// no space in it ("package/metadata/manifest/spine/item/itemref") becomes a single token
// taller than the column. Column breaking works between tokens, so it cannot split that
// one, and the run is drawn straight through the status bar and off the panel. Break the
// run into column-sized pieces here, where the character boundaries are still known;
// layout then treats them as ordinary adjacent tokens. Only over-long runs are touched, so
// ordinary words are unchanged. The pieces are cut at byte boundaries, which for a Latin
// run is the same thing as character boundaries; a forced-sideways run of CJK text (see
// flushForcedOrientation) is cut at the character instead.
void ChapterHtmlSlimParser::addSidewaysToken(std::string token, const EpdFontFamily::Style fontStyle) {
  constexpr auto kSideways = VerticalTextUtils::VerticalBehavior::Sideways;
  if (viewportHeight <= 0 || renderer.getTextAdvanceX(fontId, token.c_str(), fontStyle) <= viewportHeight) {
    currentTextBlock->addVerticalToken(std::move(token), fontStyle, kSideways);
    return;
  }
  const auto nextCharEnd = [&token](const size_t from) {
    size_t next = from + 1;
    while (next < token.size() && (static_cast<unsigned char>(token[next]) & 0xC0) == 0x80) next++;
    return next;
  };
  size_t pieceStart = 0;
  while (pieceStart < token.size()) {
    // Grow a piece one character at a time until the next one would overflow.
    size_t pieceEnd = pieceStart;
    size_t lastFitting = pieceStart;
    while (pieceEnd < token.size()) {
      const size_t next = nextCharEnd(pieceEnd);
      if (renderer.getTextAdvanceX(fontId, token.substr(pieceStart, next - pieceStart).c_str(), fontStyle) >
          viewportHeight) {
        break;
      }
      lastFitting = next;
      pieceEnd = next;
    }
    // A single character wider than the column would loop forever; emit it anyway.
    if (lastFitting == pieceStart) lastFitting = nextCharEnd(pieceStart);
    currentTextBlock->addVerticalToken(token.substr(pieceStart, lastFitting - pieceStart), fontStyle, kSideways);
    pieceStart = lastFitting;
  }
}

// The three non-default orientations, all of which the draw side reads back from the
// VERTICAL_FLIP style bit (TextBlock::renderVertical derives a token's setting from its
// text and the bit reverses that verdict), so nothing new travels through the page cache:
//
//   upright   every character in its own em cell, set upright. Characters that are upright
//             anyway go the usual way (vertical forms for 、。， included); a Latin letter,
//             a digit or an arrow gets a full cell and the flag, and is centred in it.
//   sideways  every character turned with the column. Characters that turn anyway are
//             coalesced into one run like ordinary Latin; an upright one (a fullwidth ！,
//             kana in a "横倒し" span) becomes a one-character sideways token with the flag.
//   combine   the whole run in one cell (tate-chu-yoko). Only for runs the tokenizer would
//             have turned: 1-2 digits and !? are set that way already, and anything
//             containing an upright character is prose the class was put on by mistake.
//             Wider than one and a half cells cannot be squeezed in and turns as usual.
bool ChapterHtmlSlimParser::flushForcedOrientation(const EpdFontFamily::Style fontStyle,
                                                   const CssTextOrientation orientation) {
  using VerticalTextUtils::VerticalBehavior;
  const auto flipped = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::VERTICAL_FLIP);
  const auto* p = reinterpret_cast<const unsigned char*>(partWordBuffer);
  const auto* end = p + partWordBufferIndex;

  const auto isUprightByDefault = [](const uint32_t cp) {
    return VerticalTextUtils::isUprightInVertical(cp) || VerticalTextUtils::getVerticalPunctuationOffset(cp) != nullptr;
  };
  // What the draw-time classifier would set upright inside one cell on its own.
  const auto isNaturalTateChuYoko = [](const std::string& run) {
    if (VerticalTextUtils::isTateChuYokoPunctuationPair(run.c_str())) return true;
    if (run.empty() || run.size() > 2) return false;
    for (const char c : run) {
      if (c < '0' || c > '9') return false;
    }
    return true;
  };

  if (orientation == CssTextOrientation::Combine) {
    for (const unsigned char* q = p; q < end;) {
      const uint32_t cp = utf8NextCodepoint(&q);
      if (cp == 0) break;
      if (isUprightByDefault(cp)) return false;
    }
    std::string run(partWordBuffer, static_cast<size_t>(partWordBufferIndex));
    if (isNaturalTateChuYoko(run)) return false;
    const int cell = verticalCellWidthMemo > 0 ? verticalCellWidthMemo : renderer.getCjkCellWidth(fontId);
    if (renderer.getTextAdvanceX(fontId, run.c_str(), fontStyle) > cell * 3 / 2) return false;
    currentTextBlock->addVerticalToken(std::move(run), flipped, VerticalBehavior::TateChuYoko);
    return true;
  }

  const bool upright = orientation == CssTextOrientation::Upright;
  std::string run;  // sideways: consecutive characters that turn anyway, kept as one token
  const auto flushRun = [&]() {
    if (run.empty()) return;
    // A lone digit pair or !? would be set upright by the draw-time classifier; flag it so
    // it turns with the rest of the span.
    const EpdFontFamily::Style style = isNaturalTateChuYoko(run) ? flipped : fontStyle;
    addSidewaysToken(std::move(run), style);
    run.clear();
  };
  while (p < end) {
    const unsigned char* cpStart = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    const VerticalTextUtils::PunctuationOffset* punct = VerticalTextUtils::getVerticalPunctuationOffset(cp);
    const bool uprightByDefault = isUprightByDefault(cp);
    // Brackets and long marks are upright tokens that the draw side turns anyway.
    const bool turnsByDefault = punct != nullptr ? punct->rotate : !uprightByDefault;
    std::string token(reinterpret_cast<const char*>(cpStart), p - cpStart);
    if (upright) {
      if (uprightByDefault) {
        const uint32_t formCp = VerticalTextUtils::verticalPresentationForm(cp);
        if (formCp != 0 && fontHasVerticalForm(formCp)) {
          token.clear();
          utf8AppendCodepoint(formCp, token);
        }
        currentTextBlock->addVerticalToken(std::move(token), fontStyle, VerticalBehavior::Upright);
      } else if (cp == ' ') {
        currentTextBlock->addVerticalToken(std::move(token), fontStyle, VerticalBehavior::Sideways);
      } else {
        currentTextBlock->addVerticalToken(std::move(token), flipped, VerticalBehavior::TateChuYoko);
      }
      continue;
    }
    if (turnsByDefault) {
      run += token;
      continue;
    }
    flushRun();
    currentTextBlock->addVerticalToken(std::move(token), flipped, VerticalBehavior::Sideways);
  }
  flushRun();
  return true;
}

// Coverage-interval probe of the reading face, cached per form because the answer is
// needed once per 、。， in the chapter. Faces predating the Vertical Forms block
// answer false and the shifted-horizontal-glyph fallback stays in effect.
// Mark glyph for a text-emphasis style, substituting a dot when the reading face has no
// sesame. U+FE45/FE46 are the default shape for CSS text-emphasis and the usual choice in
// Japanese books, but they sit in Vertical Forms and most faces stop short of it -- the
// generated OST reading faces included. Drawing the replacement box for every marked
// character is worse than the dot every reader recognises as bouten, so the face is probed
// once and the shape downgraded if it comes back empty.
const char* ChapterHtmlSlimParser::resolveEmphasisMark(const CssTextEmphasis e) {
  const bool filledSesame = e == CssTextEmphasis::FilledSesame;
  if (!filledSesame && e != CssTextEmphasis::OpenSesame) {
    return emphasisMarkUtf8(e);
  }

  const uint8_t bit = filledSesame ? 1u : 2u;
  if ((sesameProbe & bit) == 0) {
    sesameProbe |= bit;
    const uint32_t cp = filledSesame ? 0xFE45 : 0xFE46;
    const auto& fonts = renderer.getFontMap();
    const auto it = fonts.find(fontId);
    if (it != fonts.end() && it->second.hasCodepoint(cp)) {
      sesameProbe |= static_cast<uint8_t>(bit << 2);
    }
  }
  if ((sesameProbe & static_cast<uint8_t>(bit << 2)) != 0) {
    return emphasisMarkUtf8(e);
  }
  return emphasisMarkUtf8(filledSesame ? CssTextEmphasis::FilledDot : CssTextEmphasis::OpenDot);
}

bool ChapterHtmlSlimParser::fontHasCodepoint(const uint32_t cp) const {
  const auto& fonts = renderer.getFontMap();
  const auto it = fonts.find(fontId);
  return it != fonts.end() && it->second.hasCodepoint(cp);
}

// Rewrite compatibility ideographs the reading face has no glyph for into their unified
// form. Both sit in the BMP and so encode to three UTF-8 bytes, which is what lets this
// patch the buffer in place instead of rebuilding it. Runs on the flush path so the
// vertical and horizontal tokenizers both see the substituted text.
void ChapterHtmlSlimParser::substituteMissingCompatibilityIdeographs() {
  auto* write = reinterpret_cast<unsigned char*>(partWordBuffer);
  const auto* p = write;
  const auto* end = p + partWordBufferIndex;
  while (p < end) {
    const unsigned char* cpStart = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (cp < 0xF900 || cp > 0xFAFF || (p - cpStart) != 3) continue;
    const uint32_t unified = unifiedIdeographFor(cp);
    if (unified == 0 || fontHasCodepoint(cp)) continue;
    auto* out = write + (cpStart - write);
    out[0] = static_cast<unsigned char>(0xE0 | (unified >> 12));
    out[1] = static_cast<unsigned char>(0x80 | ((unified >> 6) & 0x3F));
    out[2] = static_cast<unsigned char>(0x80 | (unified & 0x3F));
  }
}

bool ChapterHtmlSlimParser::fontHasVerticalForm(const uint32_t formCp) {
  const uint8_t bit = 1u << (formCp - 0xFE10);
  if ((vertFormProbe & bit) == 0) {
    vertFormProbe |= bit;
    const auto& fonts = renderer.getFontMap();
    const auto it = fonts.find(fontId);
    if (it != fonts.end() && it->second.hasCodepoint(formCp)) {
      vertFormProbe |= bit << 4;
    }
  }
  return (vertFormProbe & (bit << 4)) != 0;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;          // New block = new paragraph, no continuation
  pendingVerticalWhitespace = false;  // and no separator carried across the paragraph boundary
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // The stack accumulates horizontal margins and text properties from ancestors.
      // Vertical margins are per-element and not inherited through the stack, but
      // container elements deposit their vertical margins on the empty block when they
      // open. Merge those into the new style so the first child in a container inherits
      // the container's vertical spacing.
      const auto style = currentTextBlock->getBlockStyle();
      BlockStyle incoming = blockStyle;
      if (style.fromBrElement) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(incoming, BlockStyle::CombineAxis::Vertical));

      flushPendingAnchor();
      return;
    }

    // <li> added a bullet as the first word, making the block non-empty. When a nested
    // block-level child (<p>, <div>, etc.) opens, reuse the block instead of flushing
    // the bullet to its own line. The bullet stays inline with the child's text.
    if (listItemBulletOnly) {
      const auto style = currentTextBlock->getBlockStyle();
      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Vertical));
      listItemBulletOnly = false;
      flushPendingAnchor();
      return;
    }

    makePages();
  }
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  currentTextBlock.reset(
      new ParsedText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, blockStyle, isVertical));
  wordsExtractedInBlock = 0;
  listItemBulletOnly = false;
}

void ChapterHtmlSlimParser::emitHorizontalRule(const BlockStyle& blockStyle) {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  if (currentTextBlock) {
    const BlockStyle parentBlockStyle = currentTextBlock->getBlockStyle();
    startNewTextBlock(parentBlockStyle);
  }

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page for horizontal rule");
      return;
    }
    currentPageNextY = 0;
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
  const int16_t defaultVerticalSpacing = static_cast<int16_t>(lineHeight / 2);
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  constexpr uint8_t ruleThickness = 2;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    setCurrentPageVisibleOffset(visibleTextOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    completedPageCount++;
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page after horizontal-rule page break");
      return;
    }
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  currentPageNextY += topSpacing;

  auto pageRule = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(width, ruleThickness, xPos, currentPageNextY));
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    return;
  }
  currentPage->elements.push_back(pageRule);
  setCurrentPageVisibleOffset(visibleTextOffset);
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);
  // The rule is on this page, so anything still waiting is on it too.
  commitAnchorsAwaitingPlacement();

  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
}

void ChapterHtmlSlimParser::fallbackTableRowToStacked() {
  if (tableRowStacked) {
    return;
  }

  auto activeCell = std::move(currentTextBlock);
  tableRowStacked = true;

  for (auto& cell : tableRowCells) {
    currentTextBlock = std::move(cell);
    wordsExtractedInBlock = 0;
    if (currentTextBlock && !currentTextBlock->isEmpty()) {
      makePages();
    }
  }
  tableRowCells.clear();
  currentTextBlock = std::move(activeCell);
  wordsExtractedInBlock = 0;
}

void ChapterHtmlSlimParser::closeTableCell() {
  if (!insideTableCell) {
    return;
  }
  insideTableCell = false;

  if (!currentTextBlock) {
    return;
  }

  if (!tableRowStacked &&
      (tableRowCells.size() >= MAX_GRID_TABLE_COLUMNS || currentTextBlock->size() > MAX_GRID_TABLE_CELL_WORDS)) {
    fallbackTableRowToStacked();
  }

  if (tableRowStacked) {
    wordsExtractedInBlock = 0;
    if (!currentTextBlock->isEmpty()) {
      makePages();
    }
    currentTextBlock.reset();
    return;
  }

  tableRowCells.push_back(std::move(currentTextBlock));
}

void ChapterHtmlSlimParser::addTableRowSeparator() {
  if (!currentPage || currentPage->elements.empty() || viewportWidth == 0 ||
      currentPageNextY + TABLE_ROW_SEPARATOR_GAP > viewportHeight) {
    return;
  }

  auto separator = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(viewportWidth, TABLE_ROW_SEPARATOR_THICKNESS, 0, currentPageNextY + 1));
  if (!separator) {
    LOG_ERR("EHP", "OOM: table row separator");
    return;
  }
  if (currentPage->elements.capacity() == currentPage->elements.size()) {
    currentPage->elements.reserve(currentPage->elements.size() + 1);
  }
  currentPage->elements.push_back(std::move(separator));
  currentPageNextY += TABLE_ROW_SEPARATOR_GAP;
}

void ChapterHtmlSlimParser::finishTableRow() {
  closeTableCell();

  if (tableRowCells.empty()) {
    if (tableRowStacked) {
      addTableRowSeparator();
    }
    tableRowStacked = false;
    return;
  }

  const int16_t lineHeight =
      std::max<int16_t>(1, static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression));
  const size_t columnCount = tableRowCells.size();
  const uint16_t cellWidth = static_cast<uint16_t>(viewportWidth / columnCount);

  // Keep enough width for a few glyphs while allowing ordinary three-column
  // tables to remain tabular at the default font size in portrait.
  if (columnCount < 2 || cellWidth <= TABLE_CELL_HORIZONTAL_PADDING * 2 ||
      cellWidth < lineHeight * TABLE_MIN_CELL_WIDTH_LINE_HEIGHTS) {
    fallbackTableRowToStacked();
    addTableRowSeparator();
    tableRowStacked = false;
    return;
  }

  const uint16_t textWidth = static_cast<uint16_t>(cellWidth - TABLE_CELL_HORIZONTAL_PADDING * 2);
  for (auto& lines : tableCellLines) {
    lines.clear();
  }
  tableLineVisibleOffsets.clear();
  if (tableLineVisibleOffsets.capacity() < MAX_GRID_TABLE_CELL_WORDS * 2) {
    tableLineVisibleOffsets.reserve(MAX_GRID_TABLE_CELL_WORDS * 2);
  }
  size_t maxLineCount = 0;
  const bool rowRtl = tableRowRtl;

  for (size_t column = 0; column < columnCount; ++column) {
    auto& lines = tableCellLines[column];
    // Two wrapped lines per buffered word avoids normal vector growth (max 64).
    if (lines.capacity() < MAX_GRID_TABLE_CELL_WORDS * 2) {
      lines.reserve(MAX_GRID_TABLE_CELL_WORDS * 2);
    }
    tableRowCells[column]->layoutAndExtractLines(
        renderer, fontId, textWidth, [this, &lines](const std::shared_ptr<TextBlock>& line, const uint32_t offset) {
          const size_t lineIndex = lines.size();
          lines.push_back(line);
          if (tableLineVisibleOffsets.size() <= lineIndex) {
            tableLineVisibleOffsets.resize(lineIndex + 1, UINT32_MAX);
          }
          tableLineVisibleOffsets[lineIndex] = std::min(tableLineVisibleOffsets[lineIndex], offset);
        });
    if (tableRowCells[column]->layoutFailed()) layoutOom_ = true;
    maxLineCount = std::max(maxLineCount, lines.size());
  }
  tableRowCells.clear();
  const auto clearLayoutLines = [this]() {
    for (auto& lines : tableCellLines) {
      lines.clear();
    }
    tableLineVisibleOffsets.clear();
  };

  for (size_t lineIndex = 0; lineIndex < maxLineCount; ++lineIndex) {
    const uint32_t lineVisibleOffset =
        lineIndex < tableLineVisibleOffsets.size() ? tableLineVisibleOffsets[lineIndex] : visibleTextOffset;
    int16_t rowLineHeight = lineHeight;
    for (size_t column = 0; column < columnCount; ++column) {
      if (lineIndex < tableCellLines[column].size()) {
        rowLineHeight = std::max<int16_t>(
            rowLineHeight, static_cast<int16_t>(lineHeight + tableCellLines[column][lineIndex]->getRubyShift(
                                                                 renderer.getFontAscenderSize(fontId))));
      }
    }

    const bool pageFull =
        currentPage && !currentPage->elements.empty() && currentPageNextY + rowLineHeight > viewportHeight;
    if (!currentPage || pageFull) {
      if (pageFull) {
        setCurrentPageVisibleOffset(lineVisibleOffset);
        completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
        completedPageCount++;
      }
      currentPage = makeUniqueNoThrow<Page>();
      if (!currentPage) {
        LOG_ERR("EHP", "OOM: page for table row");
        clearLayoutLines();
        return;
      }
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    }

    const int16_t rowY = currentPageNextY;
    const size_t requiredCapacity = currentPage->elements.size() + columnCount;
    if (currentPage->elements.capacity() < requiredCapacity) {
      const size_t linesThatFit =
          std::max<size_t>(1, static_cast<size_t>((viewportHeight - currentPageNextY) / rowLineHeight));
      const size_t linesToReserve = std::min(maxLineCount - lineIndex, linesThatFit);
      currentPage->elements.reserve(currentPage->elements.size() + linesToReserve * columnCount + 1);
    }
    for (size_t column = 0; column < columnCount; ++column) {
      if (lineIndex >= tableCellLines[column].size()) {
        continue;
      }

      auto& line = tableCellLines[column][lineIndex];
      auto style = line->getBlockStyle();
      const size_t physicalColumn = rowRtl ? columnCount - column - 1 : column;
      style.marginLeft = static_cast<int16_t>(physicalColumn * cellWidth + TABLE_CELL_HORIZONTAL_PADDING);
      style.paddingLeft = 0;
      line->setBlockStyle(style);

      // Reset Y so every cell in this slice shares one baseline.
      currentPageNextY = rowY;
      addLineToPage(line, lineVisibleOffset);
    }
    currentPageNextY = static_cast<int16_t>(rowY + rowLineHeight);
  }

  addTableRowSeparator();
  tableRowStacked = false;
  clearLayoutLines();
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  // Ancestor record for descendant selectors (".vrtl .start-1em"): every element goes on
  // here and comes off at the top of endElement, whatever else the two handlers do with it.
  {
    const char* classValue = "";
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "class") == 0) {
          classValue = atts[i + 1];
          break;
        }
      }
    }
    self->ancestorStack.push_back({name, classValue});
  }
  if (strcasecmp(name, "body") == 0) {
    // Case-insensitive to match ParagraphStreamer's tag matching (ProgressMapper). A case
    // mismatch here would leave visibleTextOffset at 0 for the whole section, so every page
    // would record offset 0 while the sync resolver still counts a non-zero offset.
    self->insideBody = true;
  }
  if (self->insideBody && (self->nonVisibleTextDepth > 0 || isNonVisibleTextTag(name))) {
    self->nonVisibleTextDepth++;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (strcmp(name, "p") == 0) {
    self->xpathParagraphIndex++;
  }
  if (strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  // Extract class, style, id, dir and hidden attributes for CSS/RTL processing
  std::string classAttr;
  std::string styleAttr;
  std::string dirAttr;
  bool hasHiddenAttr = false;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer both anchor recording and TOC page breaks until startNewTextBlock,
        // after the previous block is flushed to pages via makePages().
        //
        // Skip IDs on non-navigable inline elements (e.g. <span>): these are never
        // link targets in epub content, but reading-system converters can inject tens
        // of thousands of them per chapter, exhausting the heap. TOC anchors are
        // always recorded regardless of element type, since they drive page breaks.
        const char* idValue = atts[i + 1];
        const bool isTocAnchor =
            std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
        // Both lists count toward the cap: an anchor waiting for placement is one that will be
        // recorded, just not yet.
        if (isTocAnchor ||
            (!isNonNavigableInlineElement(name) &&
             self->anchorData.size() + self->anchorsAwaitingPlacement.size() < MAX_ANCHORS_PER_CHAPTER)) {
          // Flush a displaced anchor before overwriting. Consecutive non-block elements
          // (e.g. <aside id="fn1">text</aside><aside id="fn2">) with no intervening block
          // never trigger startNewTextBlock, so fn1 gets silently overwritten. That leaves
          // fn1 missing from the anchor map -> getPageForAnchor returns nullopt -> reader
          // lands at page 0 (section start) instead of the footnote.
          if (!self->pendingAnchorId.empty()) {
            self->flushPendingAnchor();
          }
          self->pendingAnchorId = idValue;
        }
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = atts[i + 1];
      } else if (strcmp(atts[i], "hidden") == 0) {
        hasHiddenAttr = true;
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    // The element itself is the last ancestorStack entry; its ancestors are the rest.
    cssStyle =
        self->cssParser->resolveStyle(name, classAttr, self->ancestorStack.data(), self->ancestorStack.size() - 1);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
  }

#ifdef INPUT_DIAG
  // What resolveStyle() actually returned for a classed element. The rule count says
  // the stylesheet loaded whole, and emphasis set by a class reaches the text, yet
  // font-weight set the same way does not -- so the split is somewhere between the
  // rule store and this struct, and only the resolved flags can say which side.
  // Spans only: the boilerplate p/div/body classes would fill the 48-entry ring
  // before the decorated runs got their turn. And only spans whose class resolved to
  // something: a KADOKAWA/Kobo body wraps every sentence in a property-less span, and each
  // line here is an SD-card rewrite of the ring file -- 39 of them per chapter pushed the
  // CSS and image lines out and added seconds to the build.
  if (!classAttr.empty() && strcmp(name, "span") == 0 &&
      (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
       cssStyle.hasTextEmphasis())) {
    IMG_DIAG("sty %.6s.%.14s fw=%d/%d fs=%d td=%d/%d em=%d/%d", name, classAttr.c_str(),
             cssStyle.hasFontWeight() ? 1 : 0, static_cast<int>(cssStyle.fontWeight), cssStyle.hasFontStyle() ? 1 : 0,
             cssStyle.hasTextDecoration() ? 1 : 0, static_cast<int>(cssStyle.textDecoration),
             cssStyle.hasTextEmphasis() ? 1 : 0, static_cast<int>(cssStyle.textEmphasis));
  }
#endif
  // HTML hidden attribute overrides CSS display.
  if (hasHiddenAttr) {
    cssStyle.display = CssDisplay::None;
    cssStyle.defined.display = 1;
  }

  // HTML dir attribute overrides CSS direction (case-insensitive per HTML spec)
  if (!dirAttr.empty()) {
    if (strcasecmp(dirAttr.c_str(), "rtl") == 0) {
      cssStyle.direction = CssTextDirection::Rtl;
      cssStyle.defined.direction = 1;
    } else if (strcasecmp(dirAttr.c_str(), "ltr") == 0) {
      cssStyle.direction = CssTextDirection::Ltr;
      cssStyle.defined.direction = 1;
    }
  }

  // Direction is inherited in HTML/CSS. If this element does not define one, carry
  // the currently active inherited direction into its computed style.
  if (!cssStyle.hasDirection() && self->effectiveDirectionDefined) {
    cssStyle.direction = self->effectiveDirection;
    cssStyle.defined.direction = 1;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Buffer one simple row; oversized rows fall back to full-width flow.
  if (strcmp(name, "table") == 0) {
    // Flatten nested content without allocating a recursive row buffer.
    if (self->tableDepth > 0) {
      if (self->tableDepth == 1 && self->insideTableCell && self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->nextWordContinues = false;
      self->tableDepth += 1;
      self->depth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
      self->currentTextBlock.reset();
    }
    self->flushPendingAnchor();
    self->pushTableTextStyleEntry(cssStyle);
    self->tableDepth = 1;
    self->insideTableCell = false;
    self->tableRowStacked = false;
    self->tableRowRtl = cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl;
    self->tableRowsSpannedRemaining = 0;
    self->tableCellTextBytes = 0;
    self->tableRowCells.clear();
    self->tableRowCells.reserve(MAX_GRID_TABLE_COLUMNS);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->finishTableRow();
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      // Text before the first row is typically a <caption>.
      self->makePages();
    }
    self->currentTextBlock.reset();
    self->tableRowStacked = self->tableRowsSpannedRemaining > 0;
    self->tableRowRtl = cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl;
    if (self->tableRowsSpannedRemaining != UINT16_MAX && self->tableRowsSpannedRemaining > 0) {
      self->tableRowsSpannedRemaining--;
    }
    self->pushTableTextStyleEntry(cssStyle);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->closeTableCell();
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->currentTextBlock.reset();

    const uint16_t columnSpan = parseTableSpan(getAttribute(atts, "colspan"));
    const uint16_t rowSpan = parseTableSpan(getAttribute(atts, "rowspan"));
    if (columnSpan > 1 || rowSpan > 1) {
      self->fallbackTableRowToStacked();
    }
    if (rowSpan > 1) {
      const uint16_t remaining = rowSpan == UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(rowSpan - 1);
      self->tableRowsSpannedRemaining = std::max(self->tableRowsSpannedRemaining, remaining);
    }

    auto tableCellBlockStyle = BlockStyle();
    tableCellBlockStyle.textAlignDefined = true;
    tableCellBlockStyle.alignment =
        cssStyle.hasTextAlign()
            ? cssStyle.textAlign
            : (self->effectiveTextAlignDefined
                   ? self->effectiveTextAlign
                   : (cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl ? CssTextAlign::Right
                                                                                             : CssTextAlign::Left));
    if (cssStyle.hasDirection()) {
      tableCellBlockStyle.directionDefined = true;
      tableCellBlockStyle.isRtl = cssStyle.direction == CssTextDirection::Rtl;
    }

    self->currentTextBlock = makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->hyphenationEnabled,
                                                           self->focusReadingEnabled, tableCellBlockStyle);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: table cell");
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
    self->insideTableCell = true;
    self->tableCellTextBytes = 0;
    self->wordsExtractedInBlock = 0;
    self->flushPendingAnchor();
    self->pushTableTextStyleEntry(cssStyle);

    if (strcmp(name, "th") == 0 && (!cssStyle.hasFontWeight() || cssStyle.fontWeight == CssFontWeight::Bold)) {
      self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    }

    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && strcmp(name, "hr") == 0) {
    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && self->insideTableCell && isHeaderOrBlock(name)) {
    // Collapse block markup inside a cell to a word boundary.
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && self->insideTableCell && matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    // Preserve alt text without allocating an image framebuffer in the row.
    const char* alt = getAttribute(atts, "alt");
    if (alt && alt[0] != '\0') {
      self->syntheticCharacterData = true;
      self->characterData(userData, alt, strlen(alt));
      self->syntheticCharacterData = false;
    }
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (src.empty() && (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0)) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }

      const size_t fragmentPos = src.find('#');
      if (fragmentPos != std::string::npos) {
        src.resize(fragmentPos);
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());
        IMG_DIAG("img %s", src.size() > 26 ? src.c_str() + src.size() - 26 : src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

          if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            // Create a unique filename for the cached image
            std::string ext;
            size_t extPos = resolvedPath.rfind('.');
            if (extPos != std::string::npos) {
              ext = resolvedPath.substr(extPos);
            }
            std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

            {
              // Probe the dimensions from the entry's first bytes (early-aborted
              // inflate, a few KB) instead of extracting the whole image now —
              // extraction is deferred to the first render of the page (see
              // ImageBlock's lazy extractor). This is what keeps first-open of an
              // image-heavy chapter from stalling for seconds per image.
              ImageDimensions dims = {0, 0};
              ImageDimsProbe headerProbe;
              bool probeStreamOk;
              {
                // The probe reads only the first bytes, but starting the inflate
                // still wants the full 32KB window -- measured failing at 32-36KB
                // largest block (probe FAIL stream=0) while chapters whose build
                // hit a recovered heap passed. Same loan as the full extraction.
                GfxRenderer::FrameBufferLoan probeLoan(self->renderer);
                probeStreamOk =
                    self->epub->readItemContentsToStream(resolvedPath, headerProbe, 1024, /*allowEarlyStop=*/true);
              }
              bool gotDimensions = headerProbe.getDimensions(dims);
              IMG_DIAG("probe %s stream=%d %dx%d", gotDimensions ? "ok" : "FAIL", probeStreamOk ? 1 : 0, dims.width,
                       dims.height);

              if (!gotDimensions) {
                // No header within the stream (rare) — fall back to extracting the
                // whole image and probing the file. That can take seconds, so
                // surface the indexing popup first (single-shot per parser).
                if (self->popupFn && !self->imagePopupFired) {
                  self->imagePopupFired = true;
                  self->popupFn();
                }
                HalFile cachedImageFile;
                bool extractSuccess = false;
                if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
                  {
                    // Same 32KB-window need as the probe above; the popup is
                    // already on screen, so the framebuffer is free to lend.
                    GfxRenderer::FrameBufferLoan extractLoan(self->renderer);
                    extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
                  }
                  cachedImageFile.flush();
                  cachedImageFile.close();
                }
                if (extractSuccess) {
                  // Retry to absorb SD-card sync latency on slow cards, and to close
                  // the silent-drop bug where a single getDimensions failure was fatal.
                  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
                  for (int attempt = 0; attempt < 3 && !gotDimensions; attempt++) {
                    if (attempt > 0) {
                      delay(50);  // Give a slow SD card time to finish syncing before retrying
                    }
                    gotDimensions = decoder && decoder->getDimensions(cachedImagePath, dims);
                  }
                } else {
                  LOG_ERR("EHP", "Failed to extract image");
                }
                IMG_DIAG("fullext %s dims %s %dx%d", extractSuccess ? "ok" : "FAIL", gotDimensions ? "ok" : "FAIL",
                         dims.width, dims.height);
              }

              if (gotDimensions) {
                LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                const CssStyle& imgStyle = cssStyle;
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();

                // Compute effective container width for percentage-based image sizes.
                // If the image is inside a block with horizontal margins/padding (e.g.
                // <div style="margin: 1em 40%">), percentage widths like width:100%
                // should resolve against the container width, not the full viewport.
                int containerWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) {
                    containerWidth = self->viewportWidth - inset;
                  }
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both, then clamp to viewport preserving requested ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  if (displayWidth < 1) displayWidth = 1;
                  if (displayWidth > containerWidth || displayHeight > self->viewportHeight) {
                    float scaleX =
                        (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
                    float scaleY = (displayHeight > self->viewportHeight)
                                       ? static_cast<float>(self->viewportHeight) / displayHeight
                                       : 1.0f;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
                    displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  LOG_DBG("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive height from aspect ratio
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                } else {
                  // Scale to fit container while maintaining aspect ratio. max-width
                  // and max-height tighten those bounds when the author set them --
                  // which is how commercial EPUBs size illustrations: of 955 vertical
                  // books surveyed the sizing came from max-* classes, never from a
                  // fixed width, so without this the whole convention collapses to
                  // "fit the screen" and every class renders identically. Unlike
                  // width/height these only shrink: a picture already inside the
                  // bound keeps its own size, which is why the clamp below is a
                  // minimum against the container rather than a replacement.
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  if (imgStyle.hasImageMaxWidth()) {
                    const int bound = static_cast<int>(
                        imgStyle.imageMaxWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                    if (bound > 0 && bound < maxWidth) maxWidth = bound;
                  }
                  if (imgStyle.hasImageMaxHeight()) {
                    const int bound = static_cast<int>(
                        imgStyle.imageMaxHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                    if (bound > 0 && bound < maxHeight) maxHeight = bound;
                  }
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                  // Whether the author's max-* bounds reached this calculation at
                  // all, and what they resolved to. Two pictures that differ only
                  // by their max- class look alike on the panel when the property
                  // is being dropped, and alike again when it is honoured but the
                  // source is small enough not to be clipped -- this line tells
                  // those two apart without measuring the screen.
                  // The class attribute travels with the bounds: a bound that is
                  // absent tells you nothing on its own, since the same line
                  // appears for a picture that simply has no class. Seeing which
                  // classes were on the tag separates "the rule never loaded"
                  // from "the tag never asked for it".
                  // rules= is how many CSS rules are resident. A book whose
                  // pictures come back unbounded looks the same whether its
                  // stylesheet was never loaded or was loaded and simply says
                  // nothing about them; the count separates those two before
                  // anyone starts reading the CSS by hand.
                  IMG_DIAG("fit mw=%d%s mh=%d%s rules=%u cls=%.20s", maxWidth, imgStyle.hasImageMaxWidth() ? "*" : "",
                           maxHeight, imgStyle.hasImageMaxHeight() ? "*" : "",
                           self->cssParser ? static_cast<unsigned>(self->cssParser->ruleCount()) : 0,
                           classAttr.empty() ? "-" : classAttr.c_str());
                }

                // Pregenerate the pixel cache now, while the build owns the heap. The
                // render path cannot do this work: extraction wants a contiguous 32KB
                // inflate window and the PNG decoder a ~62KB object, and the mid-render
                // heap supplies neither (measured 12-28KB largest block; releasing
                // caches was measured on-device to move it by zero bytes -- the
                // fragments do not coalesce). Every failure path below simply leaves
                // the old lazy render-time extract/decode as the fallback.
                if (!ImageBlock::hasValidCacheFor(cachedImagePath, displayWidth, displayHeight)) {
                  const unsigned long pregenStartMs = millis();
                  // The popup draws and refreshes the panel, so it must be on screen
                  // before the framebuffer is lent below.
                  if (self->popupFn && !self->imagePopupFired) {
                    self->imagePopupFired = true;
                    self->popupFn();
                  }

                  // A previous session's failed extraction can leave a file that
                  // exists but is empty; existence alone would never retry it.
                  bool haveFile = false;
                  if (Storage.exists(cachedImagePath.c_str())) {
                    HalFile probe;
                    haveFile = Storage.openFileForRead("EHP", cachedImagePath, probe) && probe.size() > 0;
                  }
                  if (!haveFile) {
                    HalFile outFile;
                    if (Storage.openFileForWrite("EHP", cachedImagePath, outFile)) {
                      bool extracted;
                      {
                        // The inflate window comes from the lent framebuffer bytes
                        // (InflateStream claims buildscratch), not the fragmented
                        // heap. Nothing draws inside this scope; the next page
                        // render repaints the restored-white buffer in full.
                        GfxRenderer::FrameBufferLoan loan(self->renderer);
                        // 8 KB, matching extractItemToFile(). 4 KB was measurably worse and 16 KB
                        // was tried and reverted: a deflated entry mallocs the chunk twice, so 16 KB
                        // wants 32 KB contiguous and the extraction of a gaiji failed outright at a
                        // 32 KB largest block (2026-09-11). The chunk barely moves the clock anyway
                        // -- the extraction is limited by what the card delivers (~180 KB/s), not by
                        // the number of trips.
                        extracted = self->epub->readItemContentsToStream(resolvedPath, outFile, 8192);
                      }
                      outFile.flush();
                      outFile.close();
                      if (extracted) {
                        haveFile = true;
                      } else {
                        // A partial file would satisfy the size probe next session.
                        Storage.remove(cachedImagePath.c_str());
                      }
                      // Timed apart from the decode below: the two used to share one figure, and
                      // a 17.7 s "decode" turned out to include inflating a megabyte of JPEG out
                      // of the zip and writing it to the card (2026-09-11).
                      IMG_DIAG("pregen extract %s max=%u ms=%lu", extracted ? "ok" : "FAIL", ESP.getMaxAllocHeap(),
                               millis() - pregenStartMs);
                    }
                  }

                  if (haveFile) {
                    bool cached = false;
                    // Both decode paths time themselves from here, so the figure they report is
                    // the decode alone and not the extraction above it.
                    const unsigned long streamStartMs = millis();
                    if (FsHelpers::hasPngExtension(cachedImagePath)) {
                      // Streamed decode: the inflate state comes out of the lent
                      // framebuffer, so this works under any heap layout -- a
                      // session was measured pinned at a 45KB largest block where
                      // PNGdec's ~62KB object could never exist, build or render.
                      GfxRenderer::FrameBufferLoan decodeLoan(self->renderer);
                      cached = PngStreamDecoder::decodeToCache(
                          cachedImagePath, ImageBlock::cachePathFor(cachedImagePath), displayWidth, displayHeight);
                      IMG_DIAG("pregen stream %s %dx%d max=%u ms=%lu", cached ? "ok" : "FAIL", displayWidth,
                               displayHeight, ESP.getMaxAllocHeap(), millis() - streamStartMs);
                    }
                    if (!cached) {
                      // PNGdec/JPEGDEC fallback (JPEG always; PNG only for the forms
                      // the streamer declines). The PNG object is a single ~62KB heap
                      // block the 48KB loan cannot hold, so release the font caches
                      // when the largest block looks short and hope the pieces
                      // coalesce -- at build time they usually do.
                      constexpr uint32_t PREGEN_MIN_MAX_ALLOC = 68 * 1024;
                      if (ESP.getMaxAllocHeap() < PREGEN_MIN_MAX_ALLOC) {
                        if (auto* fcm = self->renderer.getFontCacheManager()) {
                          fcm->releaseSdFontCaches();
                        }
                      }
                      RenderConfig pregen;
                      pregen.x = 0;
                      pregen.y = 0;
                      pregen.maxWidth = displayWidth;
                      pregen.maxHeight = displayHeight;
                      pregen.useExactDimensions = true;
                      pregen.cacheOnly = true;
                      pregen.cachePath = ImageBlock::cachePathFor(cachedImagePath);
                      const unsigned long decodeStartMs = millis();
                      (void)streamStartMs;
                      ImageToFramebufferDecoder* pregenDecoder = ImageDecoderFactory::getDecoder(cachedImagePath);
                      cached =
                          pregenDecoder && pregenDecoder->decodeToFramebuffer(cachedImagePath, self->renderer, pregen);
                      IMG_DIAG("pregen decode %s %dx%d max=%u ms=%lu", cached ? "ok" : "FAIL", displayWidth,
                               displayHeight, ESP.getMaxAllocHeap(), millis() - decodeStartMs);
                    }
                  }
                }

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                }

                // Vertical text: a character-sized image (a gaiji at 1em, a 3-4em glyph image)
                // stays in the flow as one token of its own height, in the cell its author put
                // it in -- also as a ruby base, since the ruby span counts words. Anything
                // wider than the column pitch would overlap its neighbours and still gets a
                // column of its own below; so does anything taller than half a column.
                if (self->isVertical && self->currentTextBlock && displayWidth > 0 && displayHeight > 0) {
                  const int columnPitch = self->verticalColumnWidth() + self->verticalColumnSpacing();
                  if (displayWidth <= columnPitch && displayHeight <= self->viewportHeight / 2) {
                    self->currentTextBlock->addVerticalToken(
                        InlineImageToken::encode(displayWidth, displayHeight, cachedImagePath, resolvedPath),
                        EpdFontFamily::REGULAR, VerticalTextUtils::VerticalBehavior::InlineImage);
                    IMG_DIAG("inline %dx%d pitch=%d", displayWidth, displayHeight, columnPitch);
                    self->depth += 1;
                    return;
                  }
                }

                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                }

                // Apply vertical margins from the container to the image.
                // Top margin lives on the empty text block (deposited via vertical merge
                // in startNewTextBlock). Bottom margin was stripped by withoutBottom() for
                // deferred application at element close, so read it from the stack.
                int16_t imageMarginTop = 0;
                int16_t imageMarginBottom = 0;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  const auto& bs = self->currentTextBlock->getBlockStyle();
                  imageMarginTop = bs.topInset();
                  if (self->blockStyleStack.size() > 1) {
                    imageMarginBottom = self->blockStyleStack.back().bottomInset();
                  }
                }

                // Create ImageBlock before either placement model runs.
                // nothrow: make_shared uses bare new, which aborts on OOM under
                // -fno-exceptions; images arrive mid-parse when the heap is at its
                // most loaded, so this must fail soft into the null-check below.
                auto imageBlock = std::shared_ptr<ImageBlock>(
                    new (std::nothrow) ImageBlock(cachedImagePath, resolvedPath, displayWidth, displayHeight));
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }

                if (self->isVertical) {
                  // Tategaki: the page fills with columns advancing right-to-left, so an
                  // image consumes horizontal span from the same cursor the columns use
                  // and centers on the column axis. CSS vertical margins belong to the
                  // horizontal model; here the column gap separates image from text. An
                  // image wider than the space left moves to a fresh page, which is also
                  // how a full-page illustration naturally becomes a page of its own.
                  const int columnWidth = self->verticalColumnWidth();
                  const int columnSpacing = self->verticalColumnSpacing();

                  if (!self->currentPage) {
                    self->currentPage.reset(new Page());
                    if (!self->currentPage) {
                      LOG_ERR("EHP", "Failed to create initial page");
                      return;
                    }
                    self->currentPageVisibleOffsetSet = false;
                  }
                  // Same re-anchor rule as addColumnToPage: the cursor belongs to a
                  // page index, not to a Page object.
                  if (self->verticalCursorPageIndex != self->completedPageCount) {
                    self->currentPageNextX =
                        static_cast<int16_t>(self->viewportWidth - columnWidth - self->verticalRubyReserve());
                    self->verticalCursorPageIndex = self->completedPageCount;
                  }

                  int rightEdge = self->currentPageNextX + columnWidth;
                  if (displayWidth > rightEdge && !self->currentPage->elements.empty()) {
                    self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex,
                                         self->xpathListItemIndex, self->currentPageVisibleOffset);
                    self->completedPageCount++;
                    self->currentPage.reset(new Page());
                    if (!self->currentPage) {
                      LOG_ERR("EHP", "Failed to create new page");
                      return;
                    }
                    self->currentPageVisibleOffsetSet = false;
                    self->currentPageNextX =
                        static_cast<int16_t>(self->viewportWidth - columnWidth - self->verticalRubyReserve());
                    self->verticalCursorPageIndex = self->completedPageCount;
                    rightEdge = self->viewportWidth;
                  }

                  int vertX = rightEdge - displayWidth;
                  if (vertX < 0) vertX = 0;
                  int vertY = (self->viewportHeight - displayHeight) / 2;
                  if (vertY < 0) vertY = 0;

                  auto pageImage = std::shared_ptr<PageImage>(new (std::nothrow) PageImage(
                      imageBlock, static_cast<int16_t>(vertX), static_cast<int16_t>(vertY)));
                  if (!pageImage) {
                    LOG_ERR("EHP", "Failed to create PageImage");
                    return;
                  }
                  self->currentPage->elements.push_back(pageImage);
                  IMG_DIAG("placed %dx%d x=%d y=%d vert=1", displayWidth, displayHeight, vertX, vertY);
                  self->setCurrentPageVisibleOffset(self->visibleTextOffset);
                  // An anchor on (or just before) the image names the page the image landed on.
                  self->commitAnchorsAwaitingPlacement();
                  // The next column starts one gap to the image's left.
                  self->currentPageNextX = static_cast<int16_t>(vertX - columnSpacing - columnWidth);

                  // The image consumed the empty block's accumulated spacing; reset it so
                  // the vertical merge in startNewTextBlock doesn't re-apply the margins.
                  if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                    BlockStyle resetStyle;
                    resetStyle.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                               ? CssTextAlign::Justify
                                               : static_cast<CssTextAlign>(self->paragraphAlignment);
                    self->currentTextBlock->setBlockStyle(resetStyle);
                  }
                  self->depth += 1;
                  return;
                }

                // Create page for image - only break if image won't fit remaining space
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom >
                     self->viewportHeight)) {
                  self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex,
                                       self->xpathListItemIndex, self->currentPageVisibleOffset);
                  self->completedPageCount++;
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create new page");
                    return;
                  }
                  self->currentPageNextY = 0;
                  self->currentPageVisibleOffsetSet = false;
                } else if (!self->currentPage) {
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create initial page");
                    return;
                  }
                  self->currentPageNextY = 0;
                  self->currentPageVisibleOffsetSet = false;
                }

                // Apply top margin from container block. Clamp it so the image never
                // overflows the page bottom: a full-viewport-height image leaves no room
                // for the margin, and the break above only fires on non-empty pages, so a
                // fresh page would otherwise place the image at y=marginTop and run
                // marginTop pixels past viewportHeight. A large bottom reserve (status
                // bar / big screen margin) absorbs that overflow silently, but with a
                // thin reserve it crosses the physical screen edge and fails
                // ImageBlock::render's bounds check, dropping the image entirely.
                if (self->currentPageNextY + imageMarginTop + displayHeight > self->viewportHeight) {
                  const int room = self->viewportHeight - displayHeight - self->currentPageNextY;
                  imageMarginTop = static_cast<int16_t>(room > 0 ? room : 0);
                }
                self->currentPageNextY += imageMarginTop;

                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage =
                    std::shared_ptr<PageImage>(new (std::nothrow) PageImage(imageBlock, xPos, self->currentPageNextY));
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  return;
                }
                self->currentPage->elements.push_back(pageImage);
                IMG_DIAG("placed %dx%d y=%d vert=%d", displayWidth, displayHeight, self->currentPageNextY,
                         self->isVertical ? 1 : 0);
                self->setCurrentPageVisibleOffset(self->visibleTextOffset);
                // An anchor on (or just before) the image names the page the image landed on.
                self->commitAnchorsAwaitingPlacement();
                self->currentPageNextY += displayHeight + imageMarginBottom;

                // The image consumed the empty block's accumulated vertical spacing.
                // Reset the block so the Vertical merge in startNewTextBlock doesn't
                // re-apply the same margins to the next text paragraph.
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle resetStyle;
                  resetStyle.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                             ? CssTextAlign::Justify
                                             : static_cast<CssTextAlign>(self->paragraphAlignment);
                  self->currentTextBlock->setBlockStyle(resetStyle);
                }

                self->depth += 1;
                return;
              } else {
                LOG_ERR("EHP", "Failed to get image dimensions");
                Storage.remove(cachedImagePath.c_str());
              }
            }
          }  // isFormatSupported
        }
      }

      // Fallback to alt text if image processing fails
      IMG_DIAG("-> ALT fallback");
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(self->blockStyleStack.back()
                                    .getCombinedBlockStyle(centeredBlockStyle, BlockStyle::CombineAxis::Horizontal)
                                    .withoutBottom());
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->depth += 1;
        self->syntheticCharacterData = true;
        self->characterData(userData, alt.c_str(), alt.length());
        self->syntheticCharacterData = false;
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }

      // No alt text, skip
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
  }

  // Ruby tag handling
  if (strcmp(name, "ruby") == 0) {
    // <ruby> is an inline element: a base that follows text with no whitespace between them
    // continues the same visual word, exactly like <b>/<i> handling in endElement().
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->inRuby = true;
    self->rubyStartWordIndex = self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0;
    if (self->currentTextBlock) {
      self->currentTextBlock->ensureRubyCapacity();
    }
    self->rubyTextBuffer.clear();
    self->depth += 1;
    return;
  }
  if (strcmp(name, "rt") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->collectingRubyText = true;
    self->depth += 1;
    return;
  }

  if (VisibleTextUtils::isNonVisibleElement(name)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Footnote indices are block-relative, so linked rows use ordinary flow.
      if (self->tableDepth >= 1 && self->insideTableCell && !self->tableRowStacked) {
        self->fallbackTableRowToStacked();
      }

      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      self->currentFootnoteLinkId = self->currentTextBlock ? self->currentTextBlock->addLinkTarget(href) : 0;
      self->currentFootnote.href[0] = '\0';
      if (self->currentFootnoteLinkId != 0) strcpy(self->currentFootnote.href, href);
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link.
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasTextDecoration = true;
      entry.textDecoration = CssTextDecoration::Underline;
      applyDirectionToEntry(entry, cssStyle);
      applyVerticalAlignToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

  if (strcmp(name, "hr") == 0) {
    auto hrBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);
    if (!self->embeddedStyle) {
      hrBlockStyle.marginLeft = 0;
      hrBlockStyle.marginRight = 0;
      hrBlockStyle.marginTop = 0;
      hrBlockStyle.marginBottom = 0;
      hrBlockStyle.paddingLeft = 0;
      hrBlockStyle.paddingRight = 0;
      hrBlockStyle.paddingTop = 0;
      hrBlockStyle.paddingBottom = 0;
      hrBlockStyle.textIndentDefined = false;
      hrBlockStyle.textIndent = 0;
    }
    self->emitHorizontalRule(hrBlockStyle);
    self->depth += 1;
    return;
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    const auto accumulated =
        self->blockStyleStack.back().getCombinedBlockStyle(headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    self->blockStyleStack.push_back(accumulated);
    self->startNewTextBlock(accumulated.withoutBottom());
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS))) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      // A <br> after text is a line break: start the next block with the container's
      // vertical margins stripped, matching browsers, which never apply paragraph
      // margins at a <br>. This is what keeps <br>-per-paragraph books (common CJK
      // web-novel formatting) from re-adding container spacing at every paragraph
      // and collapsing page capacity.
      // A <br> on an empty block (consecutive <br>s, or a standalone <br> between
      // blocks) is a scene-break separator: keep the container margins so deposited
      // vertical spacing survives. Either way the block is tagged so that if it
      // stays empty, startNewTextBlock injects a full line-height gap when the next
      // block opens; once text follows the tag is inert.
      // Style comes from the block style stack, not the current block, so a closed
      // element's style can't leak through (#2679).
      BlockStyle brStyle = self->blockStyleStack.back();
      if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        brStyle = brStyle.withoutTop().withoutBottom();
      }
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->currentCssStyle = cssStyle;
      const auto accumulated = self->blockStyleStack.back().getCombinedBlockStyle(userAlignmentBlockStyle,
                                                                                  BlockStyle::CombineAxis::Horizontal);
      self->blockStyleStack.push_back(accumulated);
      self->startNewTextBlock(accumulated.withoutBottom());
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        if (self->isVertical) {
          self->currentTextBlock->addVerticalToken("\xe2\x80\xa2", EpdFontFamily::REGULAR,
                                                   VerticalTextUtils::VerticalBehavior::Upright);
        } else {
          self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR, false, false,
                                          self->visibleTextOffset);
        }
        self->listItemBulletOnly = true;
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::Underline, cssStyle);
  } else if (matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::LineThrough, cssStyle);
  } else if (matches(name, BOLD_TAGS, std::size(BOLD_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    applyTextEmphasisToEntry(entry, cssStyle);
    applyTextOrientationToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    applyTextEmphasisToEntry(entry, cssStyle);
    applyTextOrientationToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling.
    const bool inheritedTableTextAlign = self->tableDepth >= 1 && cssStyle.hasTextAlign();
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasDirection() || cssStyle.hasVerticalAlign() || inheritedTableTextAlign ||
        cssStyle.hasTextEmphasis() || cssStyle.hasTextOrientation()) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      applyTextDecorationToEntry(entry, cssStyle);
      applyDirectionToEntry(entry, cssStyle);
      entry.setsParagraphDirection = strcmp(name, "html") == 0 || strcmp(name, "body") == 0;
      if (inheritedTableTextAlign) {
        entry.hasTextAlign = true;
        entry.textAlign = cssStyle.textAlign;
      }
      applyVerticalAlignToEntry(entry, cssStyle);
      applyTextEmphasisToEntry(entry, cssStyle);
      applyTextOrientationToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  // A layout pass that gave up leaves its words unconsumed, so the block only grows from here
  // and the next token push is what runs the heap out (2026-09-10 abort in addVerticalToken).
  // The build is already failed; stop taking text until parseStep notices.
  if (self->layoutOom_) return;
  const bool countVisibleOffsets = self->insideBody && self->nonVisibleTextDepth == 0 && !self->syntheticCharacterData;
  const uint32_t callbackVisibleOffset = self->visibleTextOffset;
  if (countVisibleOffsets) {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(s);
    const unsigned char* end = ptr + len;
    while (ptr < end) {
      utf8NextCodepoint(&ptr);
      self->visibleTextOffset++;
    }
  }

  // Nested content needs an enclosing bounded cell collector.
  if (self->tableDepth > 1 && !self->insideTableCell) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Collect ruby text instead of normal word processing.
  if (self->collectingRubyText) {
    self->rubyTextBuffer.append(s, len);
    return;
  }

  if (self->tableDepth == 1 && !self->insideTableCell) {
    bool onlyWhitespace = true;
    for (int i = 0; i < len; ++i) {
      if (!isWhitespace(s[i])) {
        onlyWhitespace = false;
        break;
      }
    }
    if (onlyWhitespace) {
      return;
    }
  }

  // Recreate flow storage for valid text (for example a caption) after a row.
  if (!self->currentTextBlock) {
    const BlockStyle flowStyle =
        self->blockStyleStack.empty() ? BlockStyle() : self->blockStyleStack.back().withoutBottom();
    self->currentTextBlock = makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->hyphenationEnabled,
                                                           self->focusReadingEnabled, flowStyle);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: text block for character data");
      return;
    }
    self->wordsExtractedInBlock = 0;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  uint32_t nextCodepointOffset = callbackVisibleOffset;
  for (int i = 0; i < len; i++) {
    const uint32_t codepointOffset = nextCodepointOffset;
    if (countVisibleOffsets && (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
      nextCodepointOffset++;
    }

    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Vertical layout needs the run kept as a separator (see flushPartWordBuffer). Only once the
      // block has content: a run before the first word of a paragraph collapses away.
      if (self->isVertical && self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        self->pendingVerticalWhitespace = true;
      }
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // Skip variation selectors. These are zero-width hints about which glyph variant to
    // draw for the preceding character; they have no glyph of their own and no font
    // carries one. Left unskipped they fell through to the replacement-glyph path and
    // drew a visible tofu box per occurrence.
    //  - VS1..VS16 (U+FE00..U+FE0F = 0xEF 0xB8 0x80..0x8F). VS15/VS16 are the text/emoji
    //    presentation pair; one real book in the Kakuyomu/Narou corpus scan carried 275 of
    //    them (word-processor autocorrect commonly appends VS15 after ☆ and similar marks).
    //  - Ideographic variation selectors VS17..VS256 (U+E0100..U+E01EF = 0xF3 0xA0 0x84..0x87
    //    0x80..0xBF), which the Adobe-Japan1 / Hanyo-Denshi collections use to pick a kanji
    //    variant (葛󠄀 vs 葛). Found in the commercial corpus on 2026-09-06; the bitmap fonts
    //    have one glyph per codepoint, so the base character is drawn and the selector dropped.
    if (s[i] == static_cast<XML_Char>(0xEF) && i + 2 < len && s[i + 1] == static_cast<XML_Char>(0xB8) &&
        (static_cast<unsigned char>(s[i + 2]) & 0xF0) == 0x80) {
      i += 2;
      continue;
    }
    if (s[i] == static_cast<XML_Char>(0xF3) && i + 3 < len && s[i + 1] == static_cast<XML_Char>(0xA0) &&
        static_cast<unsigned char>(s[i + 2]) >= 0x84 && static_cast<unsigned char>(s[i + 2]) <= 0x87) {
      i += 3;
      continue;
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        uint32_t overflowVisibleOffset = self->partWordVisibleOffset;
        const unsigned char* offsetPtr = reinterpret_cast<const unsigned char*>(self->partWordBuffer);
        const unsigned char* const safeEnd = offsetPtr + safeLen;
        while (offsetPtr < safeEnd) {
          utf8NextCodepoint(&offsetPtr);
          overflowVisibleOffset++;
        }
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
        self->partWordVisibleOffset = overflowVisibleOffset;
      } else {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      // Bound how far past the threshold one chunk can carry the block: checking only after
      // the whole chunk was tokenized let a 4 KB read add well over a thousand tokens (648
      // against a 320 threshold, 2026-09-10), and the layout pass then has to allocate for all
      // of them at once. This is the CJK path -- text with no spaces reaches the tokenizer only
      // when the 200-byte buffer fills, and a 3-byte character straddling that boundary leaves
      // the buffer non-empty, so the token-boundary check below never fires on it.
      self->maybeSoftFlushTextBlock();
    }

    if (self->partWordBufferIndex == 0) {
      self->partWordVisibleOffset = codepointOffset;
      // Same bound for space-separated text, which empties the buffer at every word and so
      // never reaches the size trigger above.
      self->maybeSoftFlushTextBlock();
    }
    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  self->maybeSoftFlushTextBlock();
}

// Lay the block out and consume it once it is past the soft-flush threshold, keeping token
// growth bounded: CSS-heavy spans fragment text into many tiny words, so the threshold is
// lower when embedded CSS is active. The trailing line (column) is deliberately not emitted,
// so paragraph flow survives the chunk boundary.
void ChapterHtmlSlimParser::maybeSoftFlushTextBlock() {
  if (!currentTextBlock || inRuby) return;
  const size_t blockWordCount = currentTextBlock->size();
  const size_t softFlushThreshold = embeddedStyle ? TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS : TEXT_BLOCK_SOFT_FLUSH_WORDS;
  if (blockWordCount <= softFlushThreshold) return;

  LOG_DBG("EHP", "Text block soft flush (%u words)", static_cast<unsigned>(blockWordCount));
  InputDiag::noteBuildHeadroom(ESP.getFreeHeap(), ESP.getMaxAllocHeap(), static_cast<uint32_t>(blockWordCount));
  if (isVertical) {
    const int verticalInset =
        currentTextBlock->getBlockStyle().topInset() + currentTextBlock->getBlockStyle().bottomInset();
    const uint16_t effectiveHeight =
        (verticalInset < viewportHeight) ? static_cast<uint16_t>(viewportHeight - verticalInset) : viewportHeight;
    currentTextBlock->layoutVerticalColumns(
        renderer, fontId, effectiveHeight, [this](const std::shared_ptr<TextBlock>& col) { addColumnToPage(col); },
        &verticalCellWidthMemo, false);
  } else {
    const int horizontalInset = currentTextBlock->getBlockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth =
        (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
    currentTextBlock->layoutAndExtractLines(
        renderer, fontId, effectiveWidth,
        [this](const std::shared_ptr<TextBlock>& textBlock, const uint32_t offset) {
          addLineToPage(textBlock, offset);
        },
        false);
  }
  if (currentTextBlock->layoutFailed()) layoutOom_ = true;
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (!self->ancestorStack.empty()) self->ancestorStack.pop_back();
  if (self->nonVisibleTextDepth > 0) {
    self->nonVisibleTextDepth--;
  }

  // Ruby text: </rt> distributes ruby to base words, </ruby> resets ruby state
  if (strcmp(name, "rt") == 0) {
    self->collectingRubyText = false;
    if (self->inRuby && self->currentTextBlock) {
      const int currentWordCount = static_cast<int>(self->currentTextBlock->size());
      const int baseWordCount = currentWordCount - self->rubyStartWordIndex;
      std::string cleanRuby = trimAndNormalize(self->rubyTextBuffer);
      if (!cleanRuby.empty()) {
        if (baseWordCount > 0) {
          self->currentTextBlock->setRubyGroupAt(self->rubyStartWordIndex, baseWordCount, cleanRuby);
          self->rubyStartWordIndex = currentWordCount;
        } else if (self->rubyStartWordIndex > 0) {
          int leaderIdx = self->rubyStartWordIndex - 1;
          while (leaderIdx >= 0 &&
                 (self->currentTextBlock->getWordStyleAt(leaderIdx) & EpdFontFamily::RUBY_CONTINUE) != 0) {
            leaderIdx--;
          }
          if (leaderIdx >= 0) {
            std::string prevRuby = self->currentTextBlock->getRubyTextAt(leaderIdx);
            self->currentTextBlock->setRubyForWordAt(leaderIdx, prevRuby + cleanRuby);
          }
        }
      }
    }
    self->rubyTextBuffer.clear();
    // Inline close: the next base (e.g. 字 in <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>) joins the
    // preceding one with no space. Whitespace in the source resets this in characterData().
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  if (strcmp(name, "ruby") == 0 && self->inRuby) {
    self->inRuby = false;
    self->rubyStartWordIndex = -1;
    self->rubyTextBuffer.clear();
    // Inline close: text following </ruby> joins the annotated base with no space.
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);
  const bool insideSkippedSubtree = self->depth - 1 >= self->skipUntilDepth;

  if (!insideSkippedSubtree && self->tableDepth > 1 && strcmp(name, "table") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->tableDepth -= 1;
    self->depth -= 1;
    LOG_DBG("EHP", "nested table flattened into enclosing cell");
    return;
  }

  if (!insideSkippedSubtree && self->tableDepth >= 1 && self->insideTableCell && headerOrBlockTag) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->depth -= 1;
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) ||
                             matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS)) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnote.href, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
    self->currentFootnoteLinkId = 0;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->closeTableCell();
    self->nextWordContinues = false;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && (strcmp(name, "tr") == 0)) {
    self->finishTableRow();
    self->nextWordContinues = false;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->finishTableRow();
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->currentTextBlock.reset();
    self->tableDepth = 0;
    self->insideTableCell = false;
    self->tableRowStacked = false;
    self->tableRowsSpannedRemaining = 0;
    self->tableCellTextBytes = 0;
    self->tableRowCells.clear();
    self->nextWordContinues = false;

    const BlockStyle flowStyle =
        self->blockStyleStack.empty() ? BlockStyle() : self->blockStyleStack.back().withoutBottom();
    self->currentTextBlock = makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->hyphenationEnabled,
                                                           self->focusReadingEnabled, flowStyle);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: text block after table");
    }
    self->wordsExtractedInBlock = 0;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag && !insideSkippedSubtree) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleStack.size() > 1) {
      // Apply closing element's bottom margin to the current text block so
      // container spacing appears after the element's content (on the last child),
      // not on the first child via the empty-block merge in startNewTextBlock.
      if (self->currentTextBlock) {
        const auto style = self->currentTextBlock->getBlockStyle();
        self->currentTextBlock->setBlockStyle(style.addBottom(self->blockStyleStack.back()));
      }
      self->blockStyleStack.pop_back();
      // Start a new text block with the parent style to prevent subsequent bare text
      // from inheriting the closed block style (e.g. alignment or margins).
      // Vertical margins and paddings are stripped
      self->startNewTextBlock(self->blockStyleStack.back().withoutTop().withoutBottom());
      self->updateEffectiveInlineStyle();
    }

    // </li> closes: if the bullet never got inline text (empty <li> or <li> with only
    // block children that were flushed), clear the flag so the next sibling doesn't
    // merge into this block.
    if (strcmp(name, "li") == 0) {
      self->listItemBulletOnly = false;
    }
  }
  if (strcmp(name, "body") == 0) {
    self->insideBody = false;
  }
  if (strcmp(name, "html") == 0) {
    self->htmlEnded_ = true;
  }
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() { abortParse(); }

bool ChapterHtmlSlimParser::beginParse() {
  htmlEnded_ = false;
  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(this->paragraphAlignment);
  blockStyleStack.clear();
  blockStyleStack.reserve(8);
  blockStyleStack.push_back(rootBlockStyle);

  tableDepth = 0;
  insideTableCell = false;
  tableRowStacked = false;
  tableRowsSpannedRemaining = 0;
  tableCellTextBytes = 0;
  tableRowCells.clear();
  for (auto& lines : tableCellLines) {
    lines.clear();
  }
  tableLineVisibleOffsets.clear();

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  xmlParser_ = XML_ParserCreate(nullptr);
  if (!xmlParser_) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(xmlParser_, defaultHandlerExpand);

  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && parseFile_.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(xmlParser_, this);
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);

  parseStartTime_ = millis();
  return true;
}

ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() {
  void* const buf = XML_GetBuffer(xmlParser_, PARSE_BUFFER_SIZE);
  if (!buf) {
    LOG_ERR("EHP", "Couldn't allocate memory for buffer");
    return ParseStatus::Error;
  }

  const size_t len = parseFile_.read(buf, PARSE_BUFFER_SIZE);

  if (len == 0 && parseFile_.available() > 0) {
    LOG_ERR("EHP", "File read error");
    return ParseStatus::Error;
  }

  const int done = parseFile_.available() == 0;

  if (layoutOom_) {
    LOG_ERR("EHP", "Abandoning build: a layout pass ran out of heap");
    return ParseStatus::Error;
  }

  if (XML_ParseBuffer(xmlParser_, static_cast<int>(len), done) == XML_STATUS_ERROR) {
    if (htmlEnded_) {
      LOG_DBG("EHP", "Ignoring trailing data after </html>: %s", XML_ErrorString(XML_GetErrorCode(xmlParser_)));
      return ParseStatus::Done;
    }
    LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(xmlParser_),
            XML_ErrorString(XML_GetErrorCode(xmlParser_)));
    return ParseStatus::Error;
  }

  return done ? ParseStatus::Done : ParseStatus::More;
}

void ChapterHtmlSlimParser::abortParse() {
  if (xmlParser_) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  // Only close the file if it was successfully opened in beginParse()
  if (parseFile_.isOpen()) {
    parseFile_.close();
  }
}

bool ChapterHtmlSlimParser::finishParse() {
  if (layoutOom_) {
    LOG_ERR("EHP", "Abandoning build: a layout pass ran out of heap");
    if (xmlParser_) {
      destroyXmlParser(xmlParser_);
      xmlParser_ = nullptr;
    }
    parseFile_.close();
    return false;
  }

  if (xmlParser_) {
    LOG_DBG("EHP", "Time to parse and build pages: %lu ms", millis() - parseStartTime_);
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  parseFile_.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
    }
    setCurrentPageVisibleOffset(visibleTextOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    completedPageCount++;
    currentPage.reset();
    currentTextBlock.reset();
  }

  // Anything still waiting laid out nothing of its own -- an empty anchored element, or a
  // chapter that ended in a table. The last page that exists is the closest there is, and is
  // what the old record-on-flush behaviour would have named. Runs after the block above so
  // the final page is already counted.
  if (!anchorsAwaitingPlacement.empty()) {
    const int lastPage = completedPageCount > 0 ? completedPageCount - 1 : 0;
    for (auto& anchor : anchorsAwaitingPlacement) {
      anchorData.push_back({std::move(anchor), static_cast<uint16_t>(lastPage)});
    }
    anchorsAwaitingPlacement.clear();
  }

  return true;
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  if (!beginParse()) {
    return false;
  }
  for (;;) {
    const ParseStatus status = parseStep();
    if (status == ParseStatus::Error) {
      abortParse();
      return false;
    }
    if (status == ParseStatus::Done) {
      break;
    }
  }
  return finishParse();
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line, const uint32_t visibleOffset) {
  const int lineHeight =
      renderer.getLineHeight(fontId, lineCompression) + line->getRubyShift(renderer.getFontAscenderSize(fontId));

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    setCurrentPageVisibleOffset(visibleOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }
  setCurrentPageVisibleOffset(visibleOffset);
  // The page is settled now, so anchors waiting since their block was flushed name this one.
  commitAnchorsAwaitingPlacement();

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  const int rubyShift = line->getRubyShift(renderer.getFontAscenderSize(fontId));
  const int baseLineHeight = renderer.getLineHeight(fontId, lineCompression);
  for (const auto& link : line->takeLinkSpans()) {
    if (!currentPage->addLink(link.href, static_cast<int16_t>(xOffset + link.x),
                              static_cast<int16_t>(currentPageNextY + rubyShift - link.topLift), link.width,
                              static_cast<int16_t>(baseLineHeight + link.topLift))) {
      LOG_DBG("EHP", "Dropped page link: %.48s", link.href);
    }
  }
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
}

int ChapterHtmlSlimParser::verticalColumnWidth() const {
  // The cell the glyphs are drawn in is the full-width advance itself, unscaled: the line
  // spacing setting moves the columns apart, it does not stretch the characters.
  return std::max(1, renderer.getCjkCellWidth(fontId));
}

int ChapterHtmlSlimParser::verticalColumnSpacing() const {
  const int width = verticalColumnWidth();
  const int pitch = static_cast<int>(width * VERTICAL_COLUMN_PITCH_EM * lineCompression + 0.5f);
  // A gap of at least one pixel even if a setting combination rounds the pitch down to the cell.
  return std::max(1, pitch - width);
}

int ChapterHtmlSlimParser::verticalRubyReserve() const {
  // Ruby in vertical writing is set beside its column, to the right (JLREQ 3.3). The gap between
  // columns carries it for every column but the first, whose ruby lands outside the text area and
  // is clipped by the screen edge -- the readings on a page's first column simply went missing
  // (2026-09-11). TextBlock draws ruby in a half-width cell, so reserving exactly that keeps the
  // page's own right edge clear without taking a whole column's worth of width.
  // Only the part the reader's own right margin does not already cover. Reserving the full ruby
  // width inside the viewport cost a whole column: at 18 pt the ruby cell is 19 px and the page
  // held 9 columns up to a reserve of 18, so 19 dropped it to 8 -- an 11% loss of text per page
  // for one pixel (measured 2026-09-11, viewport 512, pitch 57).
  const int rubyWidth = verticalColumnWidth() / 2;
  const int covered = rightMargin;
  return rubyWidth > covered ? rubyWidth - covered : 0;
}

void ChapterHtmlSlimParser::addColumnToPage(std::shared_ptr<TextBlock> column) {
  // Column occupies one full-width cell plus the gap that carries the line-spacing setting.
  const int columnWidth = verticalColumnWidth();
  const int columnSpacing = verticalColumnSpacing();
#ifdef INPUT_DIAG
  // Reported once per build rather than per column: the values are constant for a chapter, and
  // the report needs them to reconcile a column count against the font and the margin setting.
  if (!verticalLayoutReported_) {
    verticalLayoutReported_ = true;
    InputDiag::noteVerticalLayout(static_cast<uint16_t>(columnWidth),
                                  static_cast<uint16_t>(columnWidth + columnSpacing),
                                  static_cast<uint16_t>(verticalRubyReserve()), viewportWidth, viewportHeight);
  }
#endif

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageVisibleOffsetSet = false;
  }

  // Re-anchor the cursor to the right margin whenever the page changed. Pages are also
  // started outside this function (TOC-anchor breaks, image blocks) and those paths only
  // reset the horizontal cursor, so the cursor value alone cannot tell us whether it still
  // belongs to the current page. Keying on the page index instead is immune to that, and to
  // a fresh Page landing on the address of the one just handed off.
  if (verticalCursorPageIndex != completedPageCount) {
    currentPageNextX = static_cast<int16_t>(viewportWidth - columnWidth - verticalRubyReserve());
    verticalCursorPageIndex = completedPageCount;
  }

  // Columns advance right-to-left; a new page starts once the cursor passes the left edge.
  if (currentPageNextX < 0) {
    // Vertical layout does not thread a per-token visible-codepoint offset through
    // addVerticalToken/layoutVerticalColumns, so fall back to the parser's running counter.
    // Note this over-reports: layoutVerticalColumns runs from makePages once the paragraph is
    // fully parsed, so visibleTextOffset already sits at the paragraph's *end* rather than at
    // the first character of this page. Sync positions for vertical books therefore land a
    // paragraph or so ahead of the true position. Per-token offsets would fix it.
    setCurrentPageVisibleOffset(visibleTextOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextX = static_cast<int16_t>(viewportWidth - columnWidth - verticalRubyReserve());
    currentPageVisibleOffsetSet = false;
    verticalCursorPageIndex = completedPageCount;
  }
  setCurrentPageVisibleOffset(visibleTextOffset);
  // Same as addLineToPage: the column's page is settled, so waiting anchors name this one.
  commitAnchorsAwaitingPlacement();

  wordsExtractedInBlock += column->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  const int16_t yOffset = column->getBlockStyle().topInset();
  currentPage->elements.push_back(std::make_shared<PageLine>(column, currentPageNextX, yOffset));
  currentPageNextX -= static_cast<int16_t>(columnWidth + columnSpacing);
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  const int lineHeight = renderer.getLineHeight(fontId, lineCompression);

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();

  InputDiag::noteBuildHeadroom(ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                               static_cast<uint32_t>(currentTextBlock->size()));

  // Vertical (tategaki): lay the paragraph out as right-to-left columns. Block top/bottom
  // margins are a horizontal-flow concept; vertical advances by column width and adds a
  // half-cell inter-paragraph gap instead.
  if (isVertical) {
    const int verticalInset = blockStyle.topInset() + blockStyle.bottomInset();
    const uint16_t effectiveHeight =
        (verticalInset < viewportHeight) ? static_cast<uint16_t>(viewportHeight - verticalInset) : viewportHeight;
    currentTextBlock->layoutVerticalColumns(
        renderer, fontId, effectiveHeight, [this](const std::shared_ptr<TextBlock>& col) { addColumnToPage(col); },
        &verticalCellWidthMemo);
    if (currentTextBlock->layoutFailed()) layoutOom_ = true;
    if (!pendingFootnotes.empty() && currentPage) {
      for (const auto& [idx, fn] : pendingFootnotes) {
        currentPage->addFootnote(fn.number, fn.href);
      }
      pendingFootnotes.clear();
    }
    if (extraParagraphSpacing) {
      currentPageNextX -= static_cast<int16_t>(lineHeight / 2);
    }
    return;
  }

  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock, const uint32_t offset) { addLineToPage(textBlock, offset); });
  if (currentTextBlock->layoutFailed()) layoutOom_ = true;

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior)
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

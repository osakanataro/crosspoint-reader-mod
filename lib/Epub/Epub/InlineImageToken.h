#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

// A character-sized image (EBPAJ "gaiji": <img class="gaiji"> at 1em, the odd 2-3em glyph
// image) set inline in vertical text. It rides the column as one word token so it takes the
// cell its author put it in, instead of a column of its own centred on the page.
//
// Encoding: U+F8F0 (private use) then "<w>,<h>,<imagePath>\t<srcPath>". The section cache
// stores word text verbatim, so nothing changes in the block format; TextBlock re-derives the
// image from the text at draw time the way it re-derives tate-chu-yoko and sideways tokens.
// imagePath is the extracted file in the book's cache dir (its .pxc sits beside it), srcPath
// the zip entry for a lazy re-extract if the cache dir was cleaned.
namespace InlineImageToken {
constexpr char MARKER[] = "\xEF\xA3\xB0";  // U+F8F0
constexpr size_t MARKER_LEN = 3;

struct Spec {
  int16_t width = 0;
  int16_t height = 0;
  std::string imagePath;
  std::string srcPath;
};

inline bool is(const char* word) { return word != nullptr && std::memcmp(word, MARKER, MARKER_LEN) == 0; }

inline std::string encode(const int width, const int height, const std::string& imagePath, const std::string& srcPath) {
  std::string s(MARKER, MARKER_LEN);
  s += std::to_string(width);
  s += ',';
  s += std::to_string(height);
  s += ',';
  s += imagePath;
  s += '\t';
  s += srcPath;
  return s;
}

inline bool decode(const char* word, Spec& out) {
  if (!is(word)) return false;
  const char* p = word + MARKER_LEN;
  char* end = nullptr;
  const long w = std::strtol(p, &end, 10);
  if (end == p || *end != ',') return false;
  p = end + 1;
  const long h = std::strtol(p, &end, 10);
  if (end == p || *end != ',') return false;
  p = end + 1;
  const char* tab = std::strchr(p, '\t');
  if (tab == nullptr) return false;
  out.width = static_cast<int16_t>(w);
  out.height = static_cast<int16_t>(h);
  out.imagePath.assign(p, static_cast<size_t>(tab - p));
  out.srcPath.assign(tab + 1);
  return out.width > 0 && out.height > 0 && !out.imagePath.empty();
}

// Advance along the column (the image's height), or -1 when the word is not an image token.
inline int advance(const char* word) {
  if (!is(word)) return -1;
  const char* p = word + MARKER_LEN;
  char* end = nullptr;
  std::strtol(p, &end, 10);
  if (end == p || *end != ',') return -1;
  const long h = std::strtol(end + 1, nullptr, 10);
  return h > 0 ? static_cast<int>(h) : -1;
}
}  // namespace InlineImageToken

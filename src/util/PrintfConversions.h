#pragma once

#include <cctype>
#include <cstddef>
#include <cstring>

// printf conversions of translated strings, compared with the English ones so
// a translation cannot make a format call read arguments it was not given.
namespace PrintfConversions {

// The next printf conversion at or after `s`, as the arguments it reads: one
// '*' for each width or precision taken from the arguments, then the length
// modifier and the conversion letter. `out` is empty at the end of the string.
// A '%' not followed by a whole conversion is text ("Hide Battery %"). The space
// flag is accepted only with `spaceFlag`: in a label such as "Ẩn % pin" it would
// turn text into a conversion.
inline const char* next(const char* s, char (&out)[8], const bool spaceFlag = false) {
  for (; *s; ++s) {
    if (*s != '%') continue;
    if (s[1] == '%') {
      ++s;
      continue;
    }
    const char* p = s + 1;
    size_t n = 0;
    while (*p && (strchr("-+#0'", *p) || (spaceFlag && *p == ' '))) ++p;
    if (*p == '*') {
      out[n++] = '*';
      ++p;
    } else {
      while (isdigit(static_cast<unsigned char>(*p))) ++p;
    }
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        out[n++] = '*';
        ++p;
      } else {
        while (isdigit(static_cast<unsigned char>(*p))) ++p;
      }
    }
    while (*p && strchr("hljztL", *p) && n < 6) out[n++] = *p++;
    if (*p == '\0' || !strchr("diouxXeEfFgGaAcspn", *p)) continue;
    out[n++] = *p;
    out[n] = '\0';
    return p + 1;
  }
  out[0] = '\0';
  return s;
}

// Whether `translated` reads no printf arguments other than `english` does: the
// same conversions in the same order, possibly fewer. A translation that drops
// an argument only leaves it unused. When the English string reads arguments it
// is a format string, so "% d" in the translation counts as a conversion too.
inline bool safe(const char* translated, const char* english) {
  char ct[8];
  char ce[8];
  next(english, ce);
  const bool spaceFlag = ce[0] != '\0';
  for (;;) {
    translated = next(translated, ct, spaceFlag);
    if (ct[0] == '\0') return true;
    english = next(english, ce, spaceFlag);
    if (strcmp(ct, ce) != 0) return false;
  }
}

}  // namespace PrintfConversions

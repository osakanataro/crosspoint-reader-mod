#include "CjkBreaks.h"

#include <Utf8.h>

#include "CjkLineBreak.h"
#include "TokenBoundary.h"

namespace CjkBreaks {

// The classes themselves are JLREQ's (CjkLineBreak.h, OST); this file adds the explicit-hyphen
// rule upstream keeps here.
bool isNoBreakBeforeCjkPunctuation(const uint32_t cp) {
  return TokenBoundary::allowsBreakAfterExplicitHyphen(cp) || CjkLineBreak::isNoBreakBeforeCjkPunctuation(cp);
}

bool isNoBreakAfterCjkPunctuation(const uint32_t cp) { return CjkLineBreak::isNoBreakAfterCjkPunctuation(cp); }

bool boundaryAllowsBreak(const uint32_t leftCp, const uint32_t rightCp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(leftCp)) {
    return !isNoBreakBeforeCjkPunctuation(rightCp);
  }
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterCjkPunctuation(leftCp) || isNoBreakBeforeCjkPunctuation(rightCp)) return false;
  if (CjkLineBreak::isCjkInseparablePair(leftCp, rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

// Korean separates words with spaces, so a boundary touching Hangul is not a gap-less break inside
// a line. hangulLineEndBreaks() still lets a Hangul word split there at a line end.
bool hasBreakOpportunityBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(leftCp)) {
    return boundaryAllowsBreak(leftCp, rightCp);
  }
  if (utf8IsHangul(leftCp) || utf8IsHangul(rightCp)) return false;
  return boundaryAllowsBreak(leftCp, rightCp);
}

std::vector<size_t> characterBreakByteOffsets(const std::string& text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = ptr;
  uint32_t previous = utf8NextCodepoint(&ptr);
  if (previous == 0) return {};
  bool hasCjkBreakable = utf8IsCjkBreakable(previous);
  // Count first so words without accepted boundaries need no storage.
  size_t boundaryCount = 0;

  while (*ptr) {
    const uint32_t current = utf8NextCodepoint(&ptr);
    if (current == 0) break;
    if (utf8IsCjkBreakable(current)) {
      hasCjkBreakable = true;
    }
    if (hasBreakOpportunityBetween(previous, current)) {
      ++boundaryCount;
    }
    previous = current;
  }

  if (!hasCjkBreakable || boundaryCount == 0) return {};

  std::vector<size_t> allowedOffsets;
  allowedOffsets.reserve(boundaryCount);
  ptr = start;
  previous = utf8NextCodepoint(&ptr);
  while (*ptr) {
    const size_t offset = static_cast<size_t>(ptr - start);
    const uint32_t current = utf8NextCodepoint(&ptr);
    if (current == 0) break;
    if (hasBreakOpportunityBetween(previous, current)) {
      allowedOffsets.push_back(offset);
    }
    previous = current;
  }
  return allowedOffsets;
}

}  // namespace CjkBreaks

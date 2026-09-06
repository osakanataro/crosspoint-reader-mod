#include "ClockFace.h"

#include "images/ClockDigits.h"

namespace {

// Index into ClockLargeGlyphs / ClockSmallGlyphs. Matches the GLYPHS order in
// scripts/build_clock_digits.py.
constexpr uint8_t GLYPH_COLON = 10;
constexpr uint8_t GLYPH_SLASH = 11;

// Gap between cells. The generator already centres each glyph in a common box,
// so this is spacing between boxes rather than per-glyph side bearing.
constexpr int LARGE_GAP = 6;
constexpr int SMALL_GAP = 3;

// Between the date line and the time line.
constexpr int LINE_GAP = 40;

struct GlyphSet {
  const uint8_t* const* glyphs;
  int width;
  int height;
  int gap;
  int descent;  // rows below the baseline in the cell; differs per set ('/' descends, A/P/M don't)
};

constexpr GlyphSet LARGE{ClockLargeGlyphs, CLOCKLARGE_WIDTH, CLOCKLARGE_HEIGHT, LARGE_GAP, CLOCKLARGE_DESCENT};
constexpr GlyphSet SMALL{ClockSmallGlyphs, CLOCKSMALL_WIDTH, CLOCKSMALL_HEIGHT, SMALL_GAP, CLOCKSMALL_DESCENT};
constexpr GlyphSet AMPM{ClockAmPmGlyphs, CLOCKAMPM_WIDTH, CLOCKAMPM_HEIGHT, SMALL_GAP, CLOCKAMPM_DESCENT};

// Index into ClockAmPmGlyphs (glyph order: A, P, M).
constexpr uint8_t GLYPH_A = 0;
constexpr uint8_t GLYPH_P = 1;
constexpr uint8_t GLYPH_M = 2;

// Between the time digits and the AM/PM marker.
constexpr int AMPM_GAP = 18;

int lineWidth(const GlyphSet& set, const uint8_t count) {
  if (count == 0) return 0;
  return count * set.width + (count - 1) * set.gap;
}

void drawLine(const GfxRenderer& renderer, const GlyphSet& set, const uint8_t* indices, const uint8_t count,
              const int originX, const int y) {
  int x = originX;
  for (uint8_t i = 0; i < count; i++) {
    renderer.drawImage(set.glyphs[indices[i]], x, y, set.width, set.height);
    x += set.width + set.gap;
  }
}

// YYYY/MM/DD as glyph indices.
uint8_t buildDate(const Rtc::DateTime& now, uint8_t* out) {
  uint8_t n = 0;
  const uint16_t year = now.year;
  out[n++] = static_cast<uint8_t>((year / 1000) % 10);
  out[n++] = static_cast<uint8_t>((year / 100) % 10);
  out[n++] = static_cast<uint8_t>((year / 10) % 10);
  out[n++] = static_cast<uint8_t>(year % 10);
  out[n++] = GLYPH_SLASH;
  out[n++] = static_cast<uint8_t>(now.month / 10);
  out[n++] = static_cast<uint8_t>(now.month % 10);
  out[n++] = GLYPH_SLASH;
  out[n++] = static_cast<uint8_t>(now.day / 10);
  out[n++] = static_cast<uint8_t>(now.day % 10);
  return n;
}

// HH:MM (or H:MM in 12-hour style, which drops the leading zero) as glyph indices.
uint8_t buildTime(const Rtc::DateTime& now, const bool twelveHour, uint8_t* out) {
  uint8_t n = 0;
  uint8_t hour = now.hour;
  if (twelveHour) {
    hour = static_cast<uint8_t>(hour % 12);
    if (hour == 0) hour = 12;
  }
  if (!twelveHour || hour >= 10) {
    out[n++] = static_cast<uint8_t>(hour / 10);
  }
  out[n++] = static_cast<uint8_t>(hour % 10);
  out[n++] = GLYPH_COLON;
  out[n++] = static_cast<uint8_t>(now.minute / 10);
  out[n++] = static_cast<uint8_t>(now.minute % 10);
  return n;
}

}  // namespace

namespace ClockFace {

void render(const GfxRenderer& renderer, const Rtc::DateTime* now, const HalDisplay::RefreshMode mode,
            const bool twelveHour) {
  // Native panel orientation: rotateCoordinates is the identity here, so the
  // bitmaps land exactly as generated. Any turn the device needs is baked into
  // them by scripts/build_clock_digits.py --rotate.
  const_cast<GfxRenderer&>(renderer).setOrientation(GfxRenderer::LandscapeCounterClockwise);

  renderer.clearScreen();

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  if (now != nullptr) {
    uint8_t date[12];
    uint8_t time[8];
    const uint8_t dateCount = buildDate(*now, date);
    const uint8_t timeCount = buildTime(*now, twelveHour, time);

    const int blockH = SMALL.height + LINE_GAP + LARGE.height;
    const int topY = (screenH - blockH) / 2;
    const int timeY = topY + SMALL.height + LINE_GAP;

    drawLine(renderer, SMALL, date, dateCount, (screenW - lineWidth(SMALL, dateCount)) / 2, topY);

    if (twelveHour) {
      // Centre the digits and the marker as one composition. The marker slot
      // works like a segment LCD's: AM sits in the upper position (top of the
      // digits), PM in the lower one. The lower position aligns by BASELINE,
      // not by cell bottom -- the digit set's cell extends below the baseline
      // for '/', the letter set's does not, so bottom edges sit 14px apart.
      const bool isAm = now->hour < 12;
      const uint8_t marker[2] = {isAm ? GLYPH_A : GLYPH_P, GLYPH_M};
      const int digitsW = lineWidth(LARGE, timeCount);
      const int markerW = lineWidth(AMPM, 2);
      const int originX = (screenW - (digitsW + AMPM_GAP + markerW)) / 2;
      const int baselineY = timeY + LARGE.height - LARGE.descent;
      const int markerY = isAm ? timeY : baselineY - (AMPM.height - AMPM.descent);
      drawLine(renderer, LARGE, time, timeCount, originX, timeY);
      drawLine(renderer, AMPM, marker, 2, originX + digitsW + AMPM_GAP, markerY);
    } else {
      drawLine(renderer, LARGE, time, timeCount, (screenW - lineWidth(LARGE, timeCount)) / 2, timeY);
    }
  }

  renderer.displayBuffer(mode);
}

}  // namespace ClockFace

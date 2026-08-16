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
};

constexpr GlyphSet LARGE{ClockLargeGlyphs, CLOCKLARGE_WIDTH, CLOCKLARGE_HEIGHT, LARGE_GAP};
constexpr GlyphSet SMALL{ClockSmallGlyphs, CLOCKSMALL_WIDTH, CLOCKSMALL_HEIGHT, SMALL_GAP};

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

// HH:MM as glyph indices.
uint8_t buildTime(const Rtc::DateTime& now, uint8_t* out) {
  uint8_t n = 0;
  out[n++] = static_cast<uint8_t>(now.hour / 10);
  out[n++] = static_cast<uint8_t>(now.hour % 10);
  out[n++] = GLYPH_COLON;
  out[n++] = static_cast<uint8_t>(now.minute / 10);
  out[n++] = static_cast<uint8_t>(now.minute % 10);
  return n;
}

}  // namespace

namespace ClockFace {

void render(const GfxRenderer& renderer, const Rtc::DateTime& now) {
  // Native panel orientation: rotateCoordinates is the identity here, so the
  // bitmaps land exactly as generated. Any turn the device needs is baked into
  // them by scripts/build_clock_digits.py --rotate.
  const_cast<GfxRenderer&>(renderer).setOrientation(GfxRenderer::LandscapeCounterClockwise);

  renderer.clearScreen();

  uint8_t date[12];
  uint8_t time[8];
  const uint8_t dateCount = buildDate(now, date);
  const uint8_t timeCount = buildTime(now, time);

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  const int blockH = SMALL.height + LINE_GAP + LARGE.height;
  const int topY = (screenH - blockH) / 2;

  drawLine(renderer, SMALL, date, dateCount, (screenW - lineWidth(SMALL, dateCount)) / 2, topY);
  drawLine(renderer, LARGE, time, timeCount, (screenW - lineWidth(LARGE, timeCount)) / 2,
           topY + SMALL.height + LINE_GAP);

  // Full refresh, not the FAST default: this frame stays on the glass for a
  // minute at a time, and a partial update would let the previous digits ghost
  // through for the whole of it.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

}  // namespace ClockFace

#include "ClockMode.h"

#include <Arduino.h>

namespace {

// Same idiom as SilentRestart's flag in main.cpp: RTC_NOINIT is not cleared on
// a cold boot, so a magic value is what separates "armed" from whatever the
// SRAM happened to hold.
constexpr uint32_t CLOCK_MODE_MAGIC = 0xC10CC0DE;

RTC_NOINIT_ATTR uint32_t clockModeMagic;
RTC_NOINIT_ATTR uint32_t clockModeOffsetQ;
RTC_NOINIT_ATTR uint32_t clockModeWakes;

}  // namespace

namespace ClockMode {

bool isActive() { return clockModeMagic == CLOCK_MODE_MAGIC; }

void activate(const uint8_t utcOffsetQuarterHoursBiased) {
  clockModeOffsetQ = utcOffsetQuarterHoursBiased;
  clockModeWakes = 0;
  clockModeMagic = CLOCK_MODE_MAGIC;
}

uint32_t wakeCount() { return isActive() ? clockModeWakes : 0; }

void noteWake() {
  if (isActive()) clockModeWakes++;
}

void clear() { clockModeMagic = 0; }

uint8_t utcOffsetQuarterHoursBiased() {
  if (!isActive()) return 48;
  // Clamped on read as well as in HalClock: this word survives resets, so a
  // corrupted value would otherwise persist across every wake.
  return clockModeOffsetQ > 104 ? 104 : static_cast<uint8_t>(clockModeOffsetQ);
}

}  // namespace ClockMode

#pragma once

#include <cstdint>

// Clock mode: the device wakes on a timer, redraws the clock face, and sleeps
// again, doing nothing else until the power button releases it.
//
// State lives in RTC_NOINIT rather than in settings because the timer-wake path
// must not mount the SD card -- mounting it is most of what that path would
// cost, and the card is the reason the sleep rail cannot stay down. The UTC
// offset is carried along for the same reason: it is a setting, but reading it
// from the card once a minute would defeat the point.
//
// RTC_NOINIT survives deep sleep but not power loss, so a flat battery ends
// clock mode and the device comes back up normally. That is the intended
// behaviour, not a limitation to work around.

namespace ClockMode {

// True when the device should be showing the clock rather than booting the UI.
bool isActive();

// Arm clock mode with the offset the face should render in (biased quarter
// hours, 48 = UTC+0, as CrossPointSettings::clockUtcOffsetQ).
void activate(uint8_t utcOffsetQuarterHoursBiased);

// Release it. Called on a power-button wake, so the next sleep shows whatever
// sleep screen the settings ask for.
void clear();

// The offset activate() was given. 48 when clock mode is not active.
uint8_t utcOffsetQuarterHoursBiased();

// Timer wakes since clock mode was armed. Drawn on the face, because a frozen
// clock has two causes that look identical from the outside: a timer that never
// fired, and a timer that fired onto an RTC read that failed. This separates
// them -- it climbs in the second case and not in the first.
uint32_t wakeCount();
void noteWake();

// Arms clock mode, paints the first face and sleeps into it. Implemented in
// main.cpp, which owns the renderer and the sleep path; declared here so a menu
// can reach it the way SilentRestart.h exposes silentRestart().
//
// Nothing is written to SETTINGS: clock mode is not a sleep-screen setting, so
// releasing it leaves the configured sleep screen exactly as it was.
[[noreturn]] void enterClockMode(uint8_t utcOffsetQuarterHoursBiased);

// Seconds between redraws. Not exactly 60: the panel refresh and the boot that
// precedes it both take time, and a minute-resolution face only has to land
// somewhere inside each minute, so undershooting keeps it from skipping one.
constexpr uint32_t UPDATE_INTERVAL_SECONDS = 55;

}  // namespace ClockMode

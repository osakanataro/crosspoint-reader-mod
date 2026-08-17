#pragma once

#include <Rtc.h>

#include "activities/Activity.h"

// Clock mode: a full-screen clock that repaints on the minute and does nothing
// else until Back leaves it.
//
// It stays resident rather than sleeping between updates because on this
// hardware it has to. Deep sleep here drives GPIO13 low, which cuts the battery
// rail -- measured 2026-08-17 with a wake counter that never left zero, so no
// deep-sleep timer can bring the device back. Staying awake is the only way to
// own the panel for longer than one frame.
class ClockActivity final : public Activity {
 public:
  ClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Clock", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // The whole point of the mode is that it stays up: the inactivity timeout
  // would otherwise put the device to sleep out from under it.
  bool preventAutoSleep() override { return true; }

  // ...but without pinning the CPU. A clock is idle by nature, and the default
  // (full speed for anything holding off auto-sleep) exists for activities doing
  // real work between frames, which this one is not.
  bool needsFullSpeed() override { return false; }

 private:
  void writeBatteryLog(bool finished) const;

  // Minute last painted, so the repaint fires on the minute rather than a fixed
  // period after whenever the screen was opened. 0xFF until the first paint.
  uint8_t renderedMinute = 0xFF;
  // Throttles the RTC poll that watches for the minute to turn.
  unsigned long lastPollMs = 0;
  // Fallback repaint clock, used only when the RTC cannot be read: without a
  // time there is no minute to follow, and the screen should still refresh.
  unsigned long lastDrawMs = 0;

  // Battery log endpoints. Captured on entry, written out with the closing
  // reading so one session lands in the file as a pair.
  Rtc::DateTime startTime{};
  bool haveStartTime = false;
  unsigned long startMs = 0;
  uint16_t startPercent = 0;
  uint16_t startMillivolts = 0;
};

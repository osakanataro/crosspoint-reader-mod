#pragma once

#include "activities/Activity.h"

// Clock mode: a full-screen clock that redraws about once a minute and does
// nothing else until Back leaves it.
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
  // millis() at the last repaint. Compared against a period rather than watching
  // for the minute to tick: the RTC is only read during a render, and polling it
  // every loop to catch the boundary would cost far more than landing a second
  // or two late.
  unsigned long lastDrawMs = 0;
};

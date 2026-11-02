#pragma once

#include <atomic>
#include <string>

#include "activities/Activity.h"

// Boot-time rebuild of the SD font copy in the inactive OTA slot. Shown when
// the copy is switched on in settings but the slot does not hold the selected
// font -- typically the first boot after a firmware update overwrote it. Runs
// the copy with a progress page, reloads the font from flash, then goes Home.
// A failure switches the setting off (so the next boot does not retry) and
// shows the reason briefly before going Home.
class FontFlashCacheActivity final : public Activity {
 public:
  FontFlashCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FontFlashCache", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return state_.load() == State::Running; }

 private:
  enum class State : uint8_t { Idle, Running, Ready, Failed };

  void run();

  std::atomic<State> state_{State::Idle};
  std::atomic<size_t> completed_{0};
  std::atomic<size_t> total_{1};
  unsigned lastPercent_ = 0;
  std::string familyName_;
  uint8_t pointSize_ = 0;
  bool tooLarge_ = false;
  unsigned long failedAtMs_ = 0;
};

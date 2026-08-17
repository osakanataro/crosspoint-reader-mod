#include "ClockActivity.h"

#include <HalClock.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "util/ClockFace.h"

namespace {

// Redraw period. A minute-resolution face only has to land somewhere inside
// each minute, so this trades a second or two of lag for not polling the RTC.
constexpr unsigned long UPDATE_INTERVAL_MS = 60000;

}  // namespace

void ClockActivity::onEnter() {
  Activity::onEnter();
  lastDrawMs = millis();
  requestUpdate();
}

void ClockActivity::onExit() {
  // ClockFace::render leaves the renderer in the panel's native orientation.
  // Every other screen is laid out for Portrait, so hand it back or Home comes
  // up sideways.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  Activity::onExit();
}

void ClockActivity::loop() {
  // No mappedInput.update() here. The main loop polls once per iteration just
  // before calling this, and a second poll re-samples the pins and clears the
  // edge it set -- the release would be gone before wasReleased() is asked.
  // Only the activities that block inside their own inner loop poll for
  // themselves.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (millis() - lastDrawMs >= UPDATE_INTERVAL_MS) {
    lastDrawMs = millis();
    requestUpdate();
  }
}

void ClockActivity::render(RenderLock&&) {
  Rtc::DateTime now;
  const bool haveTime = halClock.getLocalDateTime(now, SETTINGS.clockUtcOffsetQ);
  if (!haveTime) {
    LOG_ERR("CLK", "Clock mode: RTC unavailable");
  }
  ClockFace::render(renderer, haveTime ? &now : nullptr);
}

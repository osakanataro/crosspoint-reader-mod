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
  mappedInput.update();

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

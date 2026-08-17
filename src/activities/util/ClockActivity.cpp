#include "ClockActivity.h"

#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "util/ClockFace.h"

namespace {

// Where the drain measurement lands. Separate from /input-diag.txt: that file is
// rewritten every few seconds by the render diagnostics, while this one holds
// two readings a session apart and has to survive between them.
constexpr char BATTERY_LOG_PATH[] = "/clock-battery.txt";

// How often the RTC is asked whether the minute has turned. Cheap next to the
// panel refresh it schedules -- one short I2C read against a second of waveform
// -- and it bounds how late a repaint can be by this much rather than by the
// loop period.
constexpr unsigned long MINUTE_POLL_MS = 500;

// Repaint period used only when there is no readable clock to follow.
constexpr unsigned long BLIND_REDRAW_MS = 60000;

}  // namespace

void ClockActivity::onEnter() {
  Activity::onEnter();

  startMs = millis();
  lastDrawMs = startMs;
  lastPollMs = startMs;
  haveStartTime = halClock.getLocalDateTime(startTime, SETTINGS.clockUtcOffsetQ);
  startPercent = powerManager.getBatteryPercentage();
  startMillivolts = powerManager.getBatteryMillivolts();

  // Written now as well as on the way out, so a session that ends with a flat
  // battery or a pulled card still leaves its starting point behind.
  writeBatteryLog(/*finished=*/false);

  requestUpdate();
}

void ClockActivity::onExit() {
  writeBatteryLog(/*finished=*/true);

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

  const unsigned long now = millis();
  if (now - lastPollMs < MINUTE_POLL_MS) return;
  lastPollMs = now;

  Rtc::DateTime current;
  if (halClock.getLocalDateTime(current, SETTINGS.clockUtcOffsetQ)) {
    // Repaint when the minute turns, not a fixed period after the screen
    // opened, so the face changes on the minute the way a clock should. The
    // panel takes a second or so to settle, so the new digits land just after
    // the boundary rather than exactly on it.
    if (current.minute != renderedMinute) {
      renderedMinute = current.minute;
      lastDrawMs = now;
      requestUpdate();
    }
    return;
  }

  if (now - lastDrawMs >= BLIND_REDRAW_MS) {
    lastDrawMs = now;
    requestUpdate();
  }
}

void ClockActivity::render(RenderLock&&) {
  Rtc::DateTime now;
  const bool haveTime = halClock.getLocalDateTime(now, SETTINGS.clockUtcOffsetQ);
  if (haveTime) {
    renderedMinute = now.minute;
  } else {
    LOG_ERR("CLK", "Clock mode: RTC unavailable");
  }
  ClockFace::render(renderer, haveTime ? &now : nullptr);
}

void ClockActivity::writeBatteryLog(const bool finished) const {
  char buf[512];
  int len = snprintf(buf, sizeof(buf), "# clock mode battery log\n");

  if (haveStartTime) {
    len += snprintf(buf + len, sizeof(buf) - len, "start %04u/%02u/%02u %02u:%02u:%02u  bat=%u%%  %umV\n",
                    startTime.year, startTime.month, startTime.day, startTime.hour, startTime.minute, startTime.second,
                    startPercent, startMillivolts);
  } else {
    len += snprintf(buf + len, sizeof(buf) - len, "start (no RTC)  bat=%u%%  %umV\n", startPercent, startMillivolts);
  }

  if (finished) {
    Rtc::DateTime endTime;
    const bool haveEnd = halClock.getLocalDateTime(endTime, SETTINGS.clockUtcOffsetQ);
    const uint16_t endPercent = powerManager.getBatteryPercentage();
    const uint16_t endMillivolts = powerManager.getBatteryMillivolts();

    if (haveEnd) {
      len +=
          snprintf(buf + len, sizeof(buf) - len, "end   %04u/%02u/%02u %02u:%02u:%02u  bat=%u%%  %umV\n", endTime.year,
                   endTime.month, endTime.day, endTime.hour, endTime.minute, endTime.second, endPercent, endMillivolts);
    } else {
      len += snprintf(buf + len, sizeof(buf) - len, "end   (no RTC)  bat=%u%%  %umV\n", endPercent, endMillivolts);
    }

    // Elapsed from millis() rather than the two timestamps: it is monotonic and
    // does not care whether the RTC was readable at either end.
    const unsigned long elapsedMs = millis() - startMs;
    const unsigned long elapsedMin = elapsedMs / 60000UL;
    const int dPercent = static_cast<int>(endPercent) - static_cast<int>(startPercent);
    const int dMillivolts = static_cast<int>(endMillivolts) - static_cast<int>(startMillivolts);
    len += snprintf(buf + len, sizeof(buf) - len, "elapsed %lu min  d_bat=%+d%%  d_mV=%+d\n", elapsedMin, dPercent,
                    dMillivolts);

    // Per-hour rates, in tenths so the line stays integer-only. Below a minute
    // the extrapolation says more about the clock than the battery, so skip it.
    if (elapsedMs >= 60000UL) {
      const long perHourPercentTenths = static_cast<long>(dPercent) * 10L * 3600000L / static_cast<long>(elapsedMs);
      const long perHourMv = static_cast<long>(dMillivolts) * 3600000L / static_cast<long>(elapsedMs);
      len += snprintf(buf + len, sizeof(buf) - len, "rate  %ld.%ld %%/h  %ld mV/h\n", perHourPercentTenths / 10,
                      labs(perHourPercentTenths % 10), perHourMv);
    }
  } else {
    len += snprintf(buf + len, sizeof(buf) - len, "end   (still running)\n");
  }

  if (len <= 0 || static_cast<size_t>(len) >= sizeof(buf)) {
    LOG_ERR("CLK", "Battery log did not fit");
    return;
  }

  HalFile file;
  if (!Storage.openFileForWrite("CLK", BATTERY_LOG_PATH, file)) {
    LOG_ERR("CLK", "Failed to open %s", BATTERY_LOG_PATH);
    return;
  }
  file.write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(len));
}

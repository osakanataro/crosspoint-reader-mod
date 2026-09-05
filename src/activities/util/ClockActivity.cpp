#include "ClockActivity.h"

#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "util/ClockFace.h"

#ifdef DEBUG_RENDER_WATCHDOG
#include <esp_task_wdt.h>
#endif

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

// Minutes between the clean passes that clear what the fast updates leave.
constexpr uint8_t CLEAN_REFRESH_MINUTES = 30;

}  // namespace

void ClockActivity::onEnter() {
  Activity::onEnter();

#ifdef DEBUG_RENDER_WATCHDOG
  // The render task is watched for the whole session (ActivityManager); this adds the main
  // loop, and only while this screen is up. Both hangs seen here left Back dead, which is the
  // main loop's job -- a watchdog on the render task alone would not have fired. Scoped to the
  // clock because other screens block the main loop on purpose (web server, OPDS, font
  // download), where a 30 s timeout would panic on healthy behaviour.
  esp_task_wdt_add(nullptr);
#endif

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
#ifdef DEBUG_RENDER_WATCHDOG
  esp_task_wdt_delete(nullptr);
#endif

  writeBatteryLog(/*finished=*/true);

  // ClockFace::render leaves the renderer in the panel's native orientation.
  // Every other screen is laid out for Portrait, so hand it back or Home comes
  // up sideways.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  Activity::onExit();
}

void ClockActivity::loop() {
#ifdef DEBUG_RENDER_WATCHDOG
  // Before the early returns below, so the feed covers every path through the loop.
  esp_task_wdt_reset();
#endif

  // No mappedInput.update() here. The main loop polls once per iteration just
  // before calling this, and a second poll re-samples the pins and clears the
  // edge it set -- the release would be gone before wasReleased() is asked.
  // Only the activities that block inside their own inner loop poll for
  // themselves.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

#ifdef DEBUG_RENDER_WATCHDOG
  // Self-test for the recovery path, not a test of the clock. Both real hangs left the
  // device dead for a day, and the watchdog that was supposed to prevent that has never
  // once been seen to fire -- on 2026-08-27 the flag was passed to a build whose tree did
  // not implement it, and it failed silently. These two triggers make the failure happen on
  // purpose, so "the device reboots within 30 s and names the stuck task" is something
  // observed rather than assumed. One trigger per watched task, because they are subscribed
  // in different places and either subscription could be the one that is missing.
  // Both front buttons other than Back, and both side buttons, so the tester does not have to
  // know their own remapping to run this. Right is left out: on the X3 it shares GPIO3 with the
  // power button, and a trigger that fires on power-off would be indistinguishable from a hang.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    LOG_DBG("CLK", "forced hang: main task");
    while (true) {
      delay(1000);  // yields, so only the watchdog feed stops -- the CPU is not spun
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    LOG_DBG("CLK", "forced hang: render task requested");
    forceRenderHang = true;
    requestUpdate();
    return;
  }
#endif

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
      // Breadcrumbs, not tracing: the log ring lives in RTC memory and survives a panic
      // reboot, so the last lines before a hang say which half of the minute cycle it was
      // in. One line per minute keeps a few cycles of history in the 16-line ring.
      LOG_DBG("CLK", "minute turn -> %02u:%02u", current.hour, current.minute);
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
#ifdef DEBUG_RENDER_WATCHDOG
  if (forceRenderHang) {
    LOG_DBG("CLK", "forced hang: render task");
    while (true) {
      delay(1000);
    }
  }
#endif

  Rtc::DateTime now;
  const bool haveTime = halClock.getLocalDateTime(now, SETTINGS.clockUtcOffsetQ);
  if (haveTime) {
    renderedMinute = now.minute;
  } else {
    LOG_ERR("CLK", "Clock mode: RTC unavailable");
  }

  // Anchored to the wall clock rather than to a count, so the clean pass lands
  // on the hour and the half hour however long the screen has been up. The
  // counter only covers the case where there is no clock to anchor to.
  bool clean = cleanPending;
  if (!clean) {
    clean = haveTime ? (now.minute % CLEAN_REFRESH_MINUTES == 0) : (++paintsSinceClean >= CLEAN_REFRESH_MINUTES);
  }
  if (clean) {
    cleanPending = false;
    paintsSinceClean = 0;
  }

  LOG_DBG("CLK", "paint %02u:%02u %s%s", now.hour, now.minute, clean ? "clean" : "fast", haveTime ? "" : " (no RTC)");
  ClockFace::render(renderer, haveTime ? &now : nullptr, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH,
                    SETTINGS.clockFormat == 1);
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
      // Sign printed separately. Splitting a negative tenths value into integer
      // and fraction loses it whenever the integer part truncates to zero, which
      // is every rate slower than 1 %/h -- i.e. every rate worth measuring.
      len += snprintf(buf + len, sizeof(buf) - len, "rate  %s%ld.%ld %%/h  %ld mV/h\n",
                      perHourPercentTenths < 0 ? "-" : "", labs(perHourPercentTenths) / 10,
                      labs(perHourPercentTenths) % 10, perHourMv);
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

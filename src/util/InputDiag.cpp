#include "InputDiag.h"

#ifdef INPUT_DIAG

#include <Arduino.h>
#include <HalStorage.h>

#include <cstdio>
#include <cstring>

#include "activities/RenderLock.h"

namespace {
constexpr char DIAG_PATH[] = "/input-diag.txt";
constexpr unsigned long FLUSH_INTERVAL_MS = 5000;
// Split the poll intervals by CPU tier. LOW_POWER_FREQ is 10 MHz on X3 and 80 MHz on PSRAM boards,
// against a normal 160 MHz, so any threshold between the two tiers works.
constexpr uint32_t FULL_SPEED_MIN_MHZ = 120;

// Plain statics, not atomics: written only from the main loop, and read back by flush() on the same
// task. Nothing here is load-bearing, so a torn value would only misreport a diagnostic.
unsigned long lastSampleAt = 0;
unsigned long lastFlushAt = 0;
uint32_t pollGapMaxFullMs = 0;
uint32_t pollGapMaxLowMs = 0;
uint32_t samplesLowPower = 0;
uint32_t debounceEpisodes = 0;
uint32_t committedEdges = 0;
uint32_t cpuMhzMin = 0;
bool wasPending = false;
}  // namespace

void InputDiag::sample(const unsigned long nowMs, const bool committedEdge, const bool debouncePending) {
  const uint32_t mhz = getCpuFrequencyMhz();
  if (cpuMhzMin == 0 || mhz < cpuMhzMin) {
    cpuMhzMin = mhz;
  }

  if (lastSampleAt != 0) {
    const uint32_t gap = static_cast<uint32_t>(nowMs - lastSampleAt);
    if (mhz < FULL_SPEED_MIN_MHZ) {
      samplesLowPower++;
      if (gap > pollGapMaxLowMs) pollGapMaxLowMs = gap;
    } else if (gap > pollGapMaxFullMs) {
      pollGapMaxFullMs = gap;
    }
  }
  lastSampleAt = nowMs;

  // Count episodes rather than samples: one raw change stays pending across several fast re-polls
  // while it waits out the debounce window, and that is one press, not several.
  if (debouncePending && !wasPending) {
    debounceEpisodes++;
  }
  wasPending = debouncePending;

  if (committedEdge) {
    committedEdges++;
  }
}

void InputDiag::flush(const bool inputActive) {
  const unsigned long now = millis();
  if (now - lastFlushAt < FLUSH_INTERVAL_MS) {
    return;
  }
  // Never write while a press is in flight, or while a render holds the storage mutex: the SD access
  // would land inside the very interval this is measuring, or block behind a font read.
  if (inputActive || RenderLock::peek()) {
    return;
  }
  lastFlushAt = now;

  char buf[448];
  const int len = snprintf(buf, sizeof(buf),
                           "uptime_ms=%lu\n"
                           "cpu_mhz_now=%u\n"
                           "cpu_mhz_min=%u\n"
                           "poll_gap_max_fullspeed_ms=%u\n"
                           "poll_gap_max_lowpower_ms=%u\n"
                           "samples_lowpower=%u\n"
                           "debounce_episodes=%u\n"
                           "committed_edges=%u\n"
                           "\n"
                           "# poll_gap_* is the interval between button samples. A press shorter than\n"
                           "# the gap in force at the time cannot be committed at all.\n"
                           "# One clean press = 2 episodes (down, up) and 2 edges. episodes well above\n"
                           "# edges means presses reached the pin and were dropped by the debounce.\n",
                           now, getCpuFrequencyMhz(), cpuMhzMin, pollGapMaxFullMs, pollGapMaxLowMs, samplesLowPower,
                           debounceEpisodes, committedEdges);
  if (len <= 0) {
    return;
  }

  HalFile file;
  if (!Storage.openFileForWrite("DIAG", DIAG_PATH, file)) {
    return;
  }
  file.write(buf, strnlen(buf, sizeof(buf)));

  // Drop the next interval. The SD write above sits between two samples, so measuring across it
  // would report this file's own cost as the worst-case poll gap -- and it would land in the
  // low-power bucket, which is the one number the whole exercise exists to read.
  lastSampleAt = 0;
}

#endif  // INPUT_DIAG

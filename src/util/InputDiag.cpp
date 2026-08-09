#include "InputDiag.h"

#ifdef INPUT_DIAG

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <string>

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
// Written from the render task, read by flush() on the main task. Aligned 32-bit scalars on a
// single core, and nothing downstream acts on them, so a stale read only misreports a diagnostic.
uint32_t renderMaxMs = 0;
uint32_t renderLastMs = 0;
uint32_t renderCount = 0;

// Ring of the most recent renders, so the report shows the shape over time rather than one maximum
// with no context. Names are truncated rather than pointed at: the activity that produced a render
// is deleted on navigation, so keeping the pointer would dangle by the time flush() reads it.
constexpr uint8_t RENDER_LOG_SIZE = 12;
constexpr uint8_t RENDER_NAME_LEN = 12;
struct RenderEntry {
  char name[RENDER_NAME_LEN];
  uint32_t ms;
};
RenderEntry renderLog[RENDER_LOG_SIZE] = {};
uint8_t renderLogNext = 0;

// File-scope so flush() stays inside the 256-byte stack budget for locals. Only the main loop task
// calls flush(), so there is no second writer.
char reportBuf[1024];

// Snapshot of the RTC log ring taken at a failure, waiting to be written out.
constexpr char LOG_PATH[] = "/input-diag-log.txt";
char capturedLogs[2048];
bool capturedLogsPending = false;
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

void InputDiag::noteRender(const char* activityName, const unsigned long durationMs) {
  renderLastMs = static_cast<uint32_t>(durationMs);
  if (renderLastMs > renderMaxMs) renderMaxMs = renderLastMs;
  renderCount++;

  RenderEntry& entry = renderLog[renderLogNext];
  snprintf(entry.name, sizeof(entry.name), "%s", activityName ? activityName : "?");
  entry.ms = renderLastMs;
  renderLogNext = static_cast<uint8_t>((renderLogNext + 1) % RENDER_LOG_SIZE);
}

void InputDiag::captureLogs(const char* reason) {
  // Keep the first capture. A failure often cascades, and the earliest report is the one that
  // still names the original cause.
  if (capturedLogsPending) return;
  const std::string logs = getLastLogs();
  const int len = snprintf(capturedLogs, sizeof(capturedLogs), "reason=%s\nuptime_ms=%lu\n\n%s", reason ? reason : "?",
                           millis(), logs.c_str());
  if (len <= 0) return;
  capturedLogsPending = true;
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

  int len = snprintf(reportBuf, sizeof(reportBuf),
                     "uptime_ms=%lu\n"
                     "cpu_mhz_now=%u\n"
                     "cpu_mhz_min=%u\n"
                     "poll_gap_max_fullspeed_ms=%u\n"
                     "poll_gap_max_lowpower_ms=%u\n"
                     "samples_lowpower=%u\n"
                     "debounce_episodes=%u\n"
                     "committed_edges=%u\n"
                     "render_last_ms=%u\n"
                     "render_max_ms=%u\n"
                     "render_count=%u\n"
                     "heap_free=%u\n"
                     "heap_min_free=%u\n"
                     "heap_max_alloc=%u\n",
                     now, getCpuFrequencyMhz(), cpuMhzMin, pollGapMaxFullMs, pollGapMaxLowMs, samplesLowPower,
                     debounceEpisodes, committedEdges, renderLastMs, renderMaxMs, renderCount, ESP.getFreeHeap(),
                     ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
  if (len <= 0 || static_cast<size_t>(len) >= sizeof(reportBuf)) {
    return;
  }

  // Oldest first, so the list reads in the order the renders happened.
  len += snprintf(reportBuf + len, sizeof(reportBuf) - len, "render_log=");
  for (uint8_t i = 0; i < RENDER_LOG_SIZE && static_cast<size_t>(len) < sizeof(reportBuf); i++) {
    const RenderEntry& entry = renderLog[(renderLogNext + i) % RENDER_LOG_SIZE];
    if (entry.name[0] == '\0') continue;  // ring not full yet
    len += snprintf(reportBuf + len, sizeof(reportBuf) - len, "%s:%u ", entry.name, entry.ms);
  }
  if (static_cast<size_t>(len) >= sizeof(reportBuf)) {
    return;
  }

  len += snprintf(reportBuf + len, sizeof(reportBuf) - len,
                  "\n\n"
                  "# poll_gap_* is the interval between button samples. A press shorter than\n"
                  "# the gap in force at the time cannot be committed at all.\n"
                  "# One clean press = 2 episodes (down, up) and 2 edges. episodes well above\n"
                  "# edges means presses reached the pin and were dropped by the debounce.\n"
                  "# render_* covers drawing plus the panel refresh. The refresh alone is a\n"
                  "# few hundred ms, so a much larger figure is drawing time, not the panel.\n"
                  "# render_log is name:ms per render, oldest first.\n"
                  "# heap_max_alloc is the largest single block still obtainable. A ZIP inflate\n"
                  "# buffer needs one contiguous block, so that number matters more than the total.\n");
  if (len <= 0) {
    return;
  }

  HalFile file;
  if (!Storage.openFileForWrite("DIAG", DIAG_PATH, file)) {
    return;
  }
  file.write(reportBuf, strnlen(reportBuf, sizeof(reportBuf)));

  if (capturedLogsPending) {
    HalFile logFile;
    if (Storage.openFileForWrite("DIAG", LOG_PATH, logFile)) {
      logFile.write(capturedLogs, strnlen(capturedLogs, sizeof(capturedLogs)));
      capturedLogsPending = false;
    }
  }

  // Drop the next interval. The SD write above sits between two samples, so measuring across it
  // would report this file's own cost as the worst-case poll gap -- and it would land in the
  // low-power bucket, which is the one number the whole exercise exists to read.
  lastSampleAt = 0;
}

#endif  // INPUT_DIAG

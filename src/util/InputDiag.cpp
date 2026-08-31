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
// Glyphs a render read one at a time, because the prewarm did not cover them. A prewarm that names
// the wrong font reports success and drops no data, so this is the only number that shows it: it
// climbs by a screenful on every repaint while ui_prewarm_fail stays at zero.
uint32_t onDemandGlyphsLast = 0;
uint32_t onDemandGlyphsMax = 0;
char onDemandGlyphsMaxName[16] = "-";

// UI glyph prewarms that reported failure: the text stayed on the on-demand path, so the screen
// draws through SdCardFont's 8-entry overflow ring. Counted because the failure is silent from the
// outside -- the screen just gets slow -- and the usual cause is that the mini bitmap arena no
// longer fits in one contiguous block, which heap_max_alloc alone cannot confirm.
uint32_t uiPrewarmFailCount = 0;
uint32_t uiPrewarmFailMinAlloc = 0;

// Heap the frame's glyph prewarm consumed, worst case, and the samples the two brackets take.
int32_t uiPrewarmHeapMax = 0;
uint32_t uiPrewarmHeapAtBegin = 0;
uint32_t renderHeapAtStart = 0;

// Geometry of the last list screen built (see noteListBand).
int16_t listBandY = 0;
int16_t listBandHeight = 0;
int16_t listRowHeightPx = 0;
int16_t listVisibleRowCount = 0;
int16_t listScreenHeight = 0;

uint32_t renderMaxMs = 0;
uint32_t renderLastMs = 0;
uint32_t renderCount = 0;
// Which activity produced renderMaxMs. render_log's 12-entry ring rolls the culprit
// out of view long before a slow session ends; this pins it for the whole session.
constexpr uint8_t RENDER_MAX_NAME_LEN = 16;
char renderMaxName[RENDER_MAX_NAME_LEN] = {};

// Mini-arena rebuilds, last render and worst render. The on-demand counter stays at zero when a
// caller warms one string at a time, because a rebuild reads into the arena rather than the
// overflow ring; these are what show it.
uint32_t miniRebuildsLast = 0;
uint32_t miniRebuildsMax = 0;
uint32_t miniRebuildMsTotal = 0;
char miniRebuildsMaxName[RENDER_MAX_NAME_LEN] = {};

// Ring of the most recent renders, so the report shows the shape over time rather than one maximum
// with no context. Names are truncated rather than pointed at: the activity that produced a render
// is deleted on navigation, so keeping the pointer would dangle by the time flush() reads it.
constexpr uint8_t RENDER_LOG_SIZE = 12;
constexpr uint8_t RENDER_NAME_LEN = 12;
struct RenderEntry {
  char name[RENDER_NAME_LEN];
  uint32_t ms;
  // Mini-arena rebuilds during this render. Sits beside the duration because that is the only
  // pairing that separates a render that is slow from a render that rebuilt the glyph arena once
  // per string it drew -- the two look identical in every other figure here.
  uint16_t miniRebuilds;
  // Free heap and the largest run available at the end of this render, in KB. Two numbers because
  // they answer different questions: free falling and never recovering is something not being given
  // back, while free holding steady as the largest run shrinks is fragmentation. The prewarm
  // failures that made a file browser page take six seconds happened with 2 KB as the largest run,
  // which neither a snapshot at flush time nor a session minimum could have shown.
  uint16_t heapFreeKb;
  uint16_t heapMaxAllocKb;
  // Heap this render consumed, in KB: positive means it took memory and had not given it back by
  // the time it finished. A page turn in a file list read -46 here while its cost went from 473 ms
  // to 19 s, which is the difference between a render that is slow and a render that is expensive.
  int16_t heapDeltaKb;
};
RenderEntry renderLog[RENDER_LOG_SIZE] = {};
uint8_t renderLogNext = 0;

// File-scope so flush() stays inside the 256-byte stack budget for locals. Only the main loop task
// calls flush(), so there is no second writer.
// Sized with headroom: the report already filled 1279 of a 1280-byte buffer, which truncated the
// trailing legend and would have silently dropped whatever line was added next.
char reportBuf[1792];

// Last and worst page-render phase split.
// What the last page scope's scan handed to the prewarm, and how often prewarm()
// bailed at its entry (scratch alloc / zero budget). Together with the rebuild
// counters these pin WHERE the per-page warm dies: zero scan bytes = the hook
// never fired; bytes>0 with entry-fails climbing = prewarm can't even start;
// bytes>0, no fails, no rebuilds = subset-hit against data the draw can't see.
uint32_t scanLastBytes = 0;
uint8_t scanLastFonts = 0;
uint32_t scanZeroCount = 0;
uint32_t prewarmEntryFailsTotal = 0;

// Book-open heap checkpoints (see noteOpenStage in the header). KB resolution is
// enough to attribute an ~85KB footprint; labels are truncated to keep the line short.
constexpr uint8_t OPEN_STAGE_COUNT = 6;
struct OpenStage {
  char label[8] = "";
  uint16_t freeKb = 0;
  uint16_t maxAllocKb = 0;
};
OpenStage openStages[OPEN_STAGE_COUNT];

// Reader-teardown heap figures (see noteCloseHeap in the header). 0 = no close yet.
uint16_t closeBeforeFreeKb = 0;
uint16_t closeAfterFreeKb = 0;
uint16_t closeAfterMaxKb = 0;

uint32_t pageRenderPrewarmMs = 0;
uint32_t pageRenderDrawMs = 0;
uint32_t pageRenderDisplayMs = 0;
uint32_t pageRenderPrewarmMaxMs = 0;
uint32_t pageRenderDrawMaxMs = 0;
uint32_t pageRenderDisplayMaxMs = 0;

uint32_t pageBlocksMs = 0;
uint32_t pageStatusBarMs = 0;

// renderVertical's split for the last page drawn.
uint32_t vertBodyMs = 0;
uint32_t vertBodyCells = 0;
uint32_t vertRubyMeasureMs = 0;
uint32_t vertRubyDrawMs = 0;
uint32_t vertRubyGroups = 0;

// The worst grayscale pair seen, kept by LSB duration. Both planes run the same
// loops, so a lopsided pair means the first pass warmed something -- these say
// what: glyphs fetched one at a time, or .pxc draws that went back to the card.
uint32_t aaWorstLsbMs = 0;
uint32_t aaWorstLsbGlyphs = 0;
uint32_t aaWorstLsbSdMs = 0;
uint32_t aaWorstLsbSdDraws = 0;
uint32_t aaWorstMsbMs = 0;
uint32_t aaWorstMsbGlyphs = 0;
uint32_t aaWorstMsbSdMs = 0;
uint32_t aaWorstMsbSdDraws = 0;

// Snapshot of the RTC log ring taken at a failure, waiting to be written out.
constexpr char LOG_PATH[] = "/input-diag-log.txt";
char capturedLogs[2048];
bool capturedLogsPending = false;

// Worst single buildSomeMore() call this session, by duration.
uint32_t buildChunkMaxMs = 0;
int buildChunkMaxSpineIndex = -1;
uint16_t buildChunkMaxPageBefore = 0;
uint16_t buildChunkMaxPageAfter = 0;

// Worst render-level total across all buildSomeMore() calls made to catch one page up,
// by summed duration -- distinct from buildChunkMaxMs, which only sees the single worst
// chunk and misses a render slowed by many small ones.
uint32_t buildTotalMaxMs = 0;
int buildTotalMaxSpineIndex = -1;
int buildTotalMaxChunkCount = 0;
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

void InputDiag::noteListBand(const int bandY, const int bandHeight, const int rowHeight, const int visibleRows,
                             const int screenHeight) {
  listBandY = static_cast<int16_t>(bandY);
  listBandHeight = static_cast<int16_t>(bandHeight);
  listRowHeightPx = static_cast<int16_t>(rowHeight);
  listVisibleRowCount = static_cast<int16_t>(visibleRows);
  listScreenHeight = static_cast<int16_t>(screenHeight);
}

void InputDiag::noteRenderStart() { renderHeapAtStart = ESP.getFreeHeap(); }

void InputDiag::noteUiPrewarmBegin() { uiPrewarmHeapAtBegin = ESP.getFreeHeap(); }

void InputDiag::noteUiPrewarmEnd() {
  if (uiPrewarmHeapAtBegin == 0) return;
  const int32_t consumed = static_cast<int32_t>(uiPrewarmHeapAtBegin) - static_cast<int32_t>(ESP.getFreeHeap());
  if (consumed > uiPrewarmHeapMax) uiPrewarmHeapMax = consumed;
  uiPrewarmHeapAtBegin = 0;
}

void InputDiag::noteRender(const char* activityName, const unsigned long durationMs, const uint32_t onDemandGlyphs,
                           const uint32_t miniRebuilds, const uint32_t miniRebuildMs) {
  renderLastMs = static_cast<uint32_t>(durationMs);
  if (renderLastMs > renderMaxMs) {
    renderMaxMs = renderLastMs;
    snprintf(renderMaxName, sizeof(renderMaxName), "%s", activityName ? activityName : "?");
  }
  renderCount++;

  onDemandGlyphsLast = onDemandGlyphs;
  if (onDemandGlyphs > onDemandGlyphsMax) {
    onDemandGlyphsMax = onDemandGlyphs;
    snprintf(onDemandGlyphsMaxName, sizeof(onDemandGlyphsMaxName), "%s", activityName ? activityName : "?");
  }

  miniRebuildsLast = miniRebuilds;
  miniRebuildMsTotal += miniRebuildMs;
  if (miniRebuilds > miniRebuildsMax) {
    miniRebuildsMax = miniRebuilds;
    snprintf(miniRebuildsMaxName, sizeof(miniRebuildsMaxName), "%s", activityName ? activityName : "?");
  }

  RenderEntry& entry = renderLog[renderLogNext];
  snprintf(entry.name, sizeof(entry.name), "%s", activityName ? activityName : "?");
  entry.ms = renderLastMs;
  entry.miniRebuilds = static_cast<uint16_t>(miniRebuilds > 0xFFFF ? 0xFFFF : miniRebuilds);
  const uint32_t heapNow = ESP.getFreeHeap();
  entry.heapFreeKb = static_cast<uint16_t>(heapNow / 1024);
  entry.heapMaxAllocKb = static_cast<uint16_t>(ESP.getMaxAllocHeap() / 1024);
  const int32_t consumed = static_cast<int32_t>(renderHeapAtStart) - static_cast<int32_t>(heapNow);
  entry.heapDeltaKb = renderHeapAtStart == 0 ? 0 : static_cast<int16_t>(consumed / 1024);
  renderHeapAtStart = 0;
  renderLogNext = static_cast<uint8_t>((renderLogNext + 1) % RENDER_LOG_SIZE);
}

void InputDiag::noteOpenBegin() {
  for (auto& s : openStages) {
    s.label[0] = '\0';
    s.freeKb = 0;
    s.maxAllocKb = 0;
  }
  Storage.remove("/open-heap.txt");
}

void InputDiag::noteCloseHeap(const uint32_t beforeFreeKb, const uint32_t afterFreeKb, const uint32_t afterMaxKb) {
  closeBeforeFreeKb = static_cast<uint16_t>(beforeFreeKb);
  closeAfterFreeKb = static_cast<uint16_t>(afterFreeKb);
  closeAfterMaxKb = static_cast<uint16_t>(afterMaxKb);
}

void InputDiag::noteOpenStage(const uint8_t slot, const char* label) {
  if (slot >= OPEN_STAGE_COUNT) return;
  auto& s = openStages[slot];
  if (s.label[0] != '\0') return;  // write-once until the next noteOpenBegin()
  snprintf(s.label, sizeof(s.label), "%s", label ? label : "?");
  s.freeKb = static_cast<uint16_t>(ESP.getFreeHeap() / 1024);
  s.maxAllocKb = static_cast<uint16_t>(ESP.getMaxAllocHeap() / 1024);

  // Also rewrite a dedicated file immediately: the periodic flush() skips while a RenderLock is
  // held, which is the whole of a blocking section build -- exactly where the open sequence dies.
  // A crash then loses every stage. Rewriting all recorded stages per checkpoint survives it
  // (HalStorage has no append mode; the file is a few lines, so the rewrite is negligible).
  HalFile f;
  if (Storage.openFileForWrite("DIAG", "/open-heap.txt", f)) {
    char line[48];
    for (const auto& stage : openStages) {
      if (stage.label[0] == '\0') continue;
      const int n =
          snprintf(line, sizeof(line), "%s: free=%uKB maxAlloc=%uKB\n", stage.label, stage.freeKb, stage.maxAllocKb);
      if (n > 0) f.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
    }
  }
}

namespace {
// 48 rather than 24: one image writes up to five lines (img, probe, extract,
// decode, placed, plus the fit bounds), so a ten-picture chapter overran the
// ring and dropped exactly the entries an investigation wanted -- the first
// images of the chapter. 48 x 96B is 4.6KB of static DRAM, and only in a
// diagnostic build.
constexpr uint8_t IMG_EVENT_COUNT = 48;
char imgEvents[IMG_EVENT_COUNT][96];
uint8_t imgEventCount = 0;  // total recorded; ring position = count % IMG_EVENT_COUNT
}  // namespace

void InputDiag::noteImageEvent(const char* line) {
  if (!line) return;
  snprintf(imgEvents[imgEventCount % IMG_EVENT_COUNT], sizeof(imgEvents[0]), "%s free=%u max=%u", line,
           static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  imgEventCount++;

  // Same rewrite-per-event scheme as noteOpenStage: build holds RenderLock, the periodic
  // flush never runs there, and a crash must not lose the trail.
  HalFile f;
  if (Storage.openFileForWrite("DIAG", "/image-diag.txt", f)) {
    const uint8_t n = imgEventCount < IMG_EVENT_COUNT ? imgEventCount : IMG_EVENT_COUNT;
    const uint8_t start = imgEventCount < IMG_EVENT_COUNT ? 0 : imgEventCount % IMG_EVENT_COUNT;
    for (uint8_t i = 0; i < n; i++) {
      const char* e = imgEvents[(start + i) % IMG_EVENT_COUNT];
      f.write(reinterpret_cast<const uint8_t*>(e), strlen(e));
      f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
    }
  }
}

void InputDiag::noteScanOutcome(const uint32_t scanBytes, const uint8_t scanFonts, const uint32_t prewarmEntryFails) {
  scanLastBytes = scanBytes;
  scanLastFonts = scanFonts;
  if (scanBytes == 0) scanZeroCount++;
  prewarmEntryFailsTotal = prewarmEntryFails;
}

void InputDiag::notePageRender(const unsigned long prewarmMs, const unsigned long drawMs,
                               const unsigned long displayMs) {
  pageRenderPrewarmMs = static_cast<uint32_t>(prewarmMs);
  pageRenderDrawMs = static_cast<uint32_t>(drawMs);
  pageRenderDisplayMs = static_cast<uint32_t>(displayMs);
  if (pageRenderPrewarmMs > pageRenderPrewarmMaxMs) pageRenderPrewarmMaxMs = pageRenderPrewarmMs;
  if (pageRenderDrawMs > pageRenderDrawMaxMs) pageRenderDrawMaxMs = pageRenderDrawMs;
  if (pageRenderDisplayMs > pageRenderDisplayMaxMs) pageRenderDisplayMaxMs = pageRenderDisplayMs;
}

void InputDiag::notePageDrawParts(const unsigned long blocksMs, const unsigned long statusBarMs) {
  pageBlocksMs = static_cast<uint32_t>(blocksMs);
  pageStatusBarMs = static_cast<uint32_t>(statusBarMs);
}

void InputDiag::noteVerticalRender(const unsigned long bodyMs, const unsigned long bodyCells,
                                   const unsigned long rubyMeasureMs, const unsigned long rubyDrawMs,
                                   const unsigned long rubyGroups) {
  vertBodyMs = static_cast<uint32_t>(bodyMs);
  vertBodyCells = static_cast<uint32_t>(bodyCells);
  vertRubyMeasureMs = static_cast<uint32_t>(rubyMeasureMs);
  vertRubyDrawMs = static_cast<uint32_t>(rubyDrawMs);
  vertRubyGroups = static_cast<uint32_t>(rubyGroups);
}

void InputDiag::noteGrayscaleSplit(const unsigned long lsbMs, const unsigned long lsbGlyphs,
                                   const unsigned long lsbSdMs, const unsigned long lsbSdDraws,
                                   const unsigned long msbMs, const unsigned long msbGlyphs,
                                   const unsigned long msbSdMs, const unsigned long msbSdDraws) {
  if (static_cast<uint32_t>(lsbMs) <= aaWorstLsbMs) return;
  aaWorstLsbMs = static_cast<uint32_t>(lsbMs);
  aaWorstLsbGlyphs = static_cast<uint32_t>(lsbGlyphs);
  aaWorstLsbSdMs = static_cast<uint32_t>(lsbSdMs);
  aaWorstLsbSdDraws = static_cast<uint32_t>(lsbSdDraws);
  aaWorstMsbMs = static_cast<uint32_t>(msbMs);
  aaWorstMsbGlyphs = static_cast<uint32_t>(msbGlyphs);
  aaWorstMsbSdMs = static_cast<uint32_t>(msbSdMs);
  aaWorstMsbSdDraws = static_cast<uint32_t>(msbSdDraws);
}

void InputDiag::noteBuildChunk(const int spineIndex, const uint16_t pageCountBefore, const uint16_t pageCountAfter,
                               const unsigned long durationMs) {
  if (static_cast<uint32_t>(durationMs) <= buildChunkMaxMs) return;
  buildChunkMaxMs = static_cast<uint32_t>(durationMs);
  buildChunkMaxSpineIndex = spineIndex;
  buildChunkMaxPageBefore = pageCountBefore;
  buildChunkMaxPageAfter = pageCountAfter;
}

void InputDiag::noteBuildTotal(const int spineIndex, const unsigned long totalMs, const int chunkCount) {
  if (static_cast<uint32_t>(totalMs) <= buildTotalMaxMs) return;
  buildTotalMaxMs = static_cast<uint32_t>(totalMs);
  buildTotalMaxSpineIndex = spineIndex;
  buildTotalMaxChunkCount = chunkCount;
}

void InputDiag::noteUiPrewarmFailure() {
  uiPrewarmFailCount++;
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if (uiPrewarmFailMinAlloc == 0 || maxAlloc < uiPrewarmFailMinAlloc) {
    uiPrewarmFailMinAlloc = maxAlloc;
  }
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

  // The first write of a boot destroys the previous session's trail -- which,
  // after a crash, is the only record of how the heap got to where it died
  // (crash_report.txt carries the stack, not the approach). Set the old file
  // aside once per boot so a post-mortem still has the render_log heap series.
  static bool previousPreserved = false;
  if (!previousPreserved) {
    previousPreserved = true;
    Storage.remove("/input-diag.prev.txt");
    Storage.rename(DIAG_PATH, "/input-diag.prev.txt");
  }

  int len = snprintf(
      reportBuf, sizeof(reportBuf),
      "uptime_ms=%lu\n"
      "cpu_mhz_now=%u\n"
      "cpu_mhz_min=%u\n"
      "poll_gap_max_fullspeed_ms=%u\n"
      "poll_gap_max_lowpower_ms=%u\n"
      "samples_lowpower=%u\n"
      "debounce_episodes=%u\n"
      "committed_edges=%u\n"
      "render_last_ms=%u\n"
      "render_max_ms=%u (%s)\n"
      "render_count=%u\n"
      "heap_free=%u\n"
      "heap_min_free=%u\n"
      "heap_max_alloc=%u\n"
      "page_prewarm_ms=%u (max %u)\n"
      "page_draw_ms=%u (max %u)\n"
      "page_display_ms=%u (max %u)\n"
      "page_blocks_ms=%u\n"
      "page_statusbar_ms=%u\n"
      "aa_worst_lsb_ms=%u glyphs=%u sd_ms=%u sd_draws=%u\n"
      "aa_worst_msb_ms=%u glyphs=%u sd_ms=%u sd_draws=%u\n"
      "vert_body_ms=%u cells=%u\n"
      "vert_ruby_measure_ms=%u\n"
      "vert_ruby_draw_ms=%u groups=%u\n"
      "build_chunk_max_ms=%u spine=%d pages=%u..%u\n"
      "build_total_max_ms=%u spine=%d chunks=%d\n"
      "ui_prewarm_fail=%u (max_alloc_then=%u)\n"
      "glyph_ondemand_last=%u max=%u (%s)\n"
      "glyph_rebuild_last=%u max=%u (%s) total_ms=%u\n"
      "page_scan_last=%ub/%uf zero=%u prewarm_entry_fails=%u\n"
      "ui_prewarm_heap_max=%d\n"
      "list_band=y%d+h%d row%d -> %d rows (screen %d)\n",
      now, getCpuFrequencyMhz(), cpuMhzMin, pollGapMaxFullMs, pollGapMaxLowMs, samplesLowPower, debounceEpisodes,
      committedEdges, renderLastMs, renderMaxMs, renderMaxName, renderCount, ESP.getFreeHeap(), ESP.getMinFreeHeap(),
      ESP.getMaxAllocHeap(), pageRenderPrewarmMs, pageRenderPrewarmMaxMs, pageRenderDrawMs, pageRenderDrawMaxMs,
      pageRenderDisplayMs, pageRenderDisplayMaxMs, pageBlocksMs, pageStatusBarMs, aaWorstLsbMs, aaWorstLsbGlyphs,
      aaWorstLsbSdMs, aaWorstLsbSdDraws, aaWorstMsbMs, aaWorstMsbGlyphs, aaWorstMsbSdMs, aaWorstMsbSdDraws, vertBodyMs,
      vertBodyCells, vertRubyMeasureMs, vertRubyDrawMs, vertRubyGroups, buildChunkMaxMs, buildChunkMaxSpineIndex,
      buildChunkMaxPageBefore, buildChunkMaxPageAfter, buildTotalMaxMs, buildTotalMaxSpineIndex,
      buildTotalMaxChunkCount, uiPrewarmFailCount, uiPrewarmFailMinAlloc, onDemandGlyphsLast, onDemandGlyphsMax,
      onDemandGlyphsMaxName, miniRebuildsLast, miniRebuildsMax, miniRebuildsMaxName, miniRebuildMsTotal, scanLastBytes,
      scanLastFonts, scanZeroCount, prewarmEntryFailsTotal, uiPrewarmHeapMax, listBandY, listBandHeight,
      listRowHeightPx, listVisibleRowCount, listScreenHeight);
  if (len <= 0 || static_cast<size_t>(len) >= sizeof(reportBuf)) {
    return;
  }

  // Heap checkpoints across the last book open, in stage order (KB free/KB largest block).
  len += snprintf(reportBuf + len, sizeof(reportBuf) - len, "open_heap=");
  for (uint8_t i = 0; i < OPEN_STAGE_COUNT && static_cast<size_t>(len) < sizeof(reportBuf); i++) {
    if (openStages[i].label[0] == '\0') continue;
    len += snprintf(reportBuf + len, sizeof(reportBuf) - len, "%s:%u/%u ", openStages[i].label, openStages[i].freeKb,
                    openStages[i].maxAllocKb);
  }
  if (static_cast<size_t>(len) < sizeof(reportBuf)) {
    len += snprintf(reportBuf + len, sizeof(reportBuf) - len, "\n");
  }

  // Heap around the last reader teardown (KB free before, free/largest block after).
  if (closeBeforeFreeKb != 0 || closeAfterFreeKb != 0) {
    len += snprintf(reportBuf + len, sizeof(reportBuf) - len, "close_heap=%u -> %u/%u\n", closeBeforeFreeKb,
                    closeAfterFreeKb, closeAfterMaxKb);
  }

  // Oldest first, so the list reads in the order the renders happened.
  len += snprintf(reportBuf + len, sizeof(reportBuf) - len, "render_log=");
  for (uint8_t i = 0; i < RENDER_LOG_SIZE && static_cast<size_t>(len) < sizeof(reportBuf); i++) {
    const RenderEntry& entry = renderLog[(renderLogNext + i) % RENDER_LOG_SIZE];
    if (entry.name[0] == '\0') continue;  // ring not full yet
    len += snprintf(reportBuf + len, sizeof(reportBuf) - len, "%s:%u@%u/%u%+d r%u ", entry.name, entry.ms,
                    entry.heapFreeKb, entry.heapMaxAllocKb, entry.heapDeltaKb, entry.miniRebuilds);
  }
  if (static_cast<size_t>(len) >= sizeof(reportBuf)) {
    return;
  }

  len += snprintf(reportBuf + len, sizeof(reportBuf) - len,
                  "\n\n"
                  "# render_log entries are name:ms@freeKB/maxAllocKB+consumedKB rRebuilds, oldest first.\n"
                  "# poll_gap_* is the interval between button samples. A press shorter than\n"
                  "# the gap in force at the time cannot be committed at all.\n"
                  "# One clean press = 2 episodes (down, up) and 2 edges. episodes well above\n"
                  "# edges means presses reached the pin and were dropped by the debounce.\n"
                  "# render_* covers drawing plus the panel refresh. The refresh alone is a\n"
                  "# few hundred ms, so a much larger figure is drawing time, not the panel.\n"
                  "# render_log is name:ms per render, oldest first.\n"
                  "# vert_* splits the vertical draw of the last page. cells and groups are counts,\n"
                  "# so ms/cell and ms/group say whether the cost is per call or in one place.\n"
                  "# page_* splits one page render: glyph prewarm from the card, drawing, then the\n"
                  "# panel refresh. Whichever dominates is where a page turn's cost actually is.\n"
                  "# heap_max_alloc is the largest single block still obtainable. A ZIP inflate\n"
                  "# buffer needs one contiguous block, so that number matters more than the total.\n"
                  "# build_chunk_max_ms is the single slowest Section::buildSomeMore() call this\n"
                  "# session -- it has no internal time budget, so one pathological page freezes\n"
                  "# input for its whole duration. pages=X..Y is the watermark before/after the\n"
                  "# call; the slow content is in that page range of the given spine index.\n"
                  "# build_total_max_ms is the worst render's summed buildSomeMore() time across\n"
                  "# all its chunks. A high total with a low build_chunk_max_ms means many small\n"
                  "# chunks, not one slow page -- chunks=N says how many it took to catch up.\n");
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

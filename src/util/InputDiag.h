#pragma once

#include <cstdint>

// Serial-free input timing diagnostics.
//
// The USB-write-locked X3 units have no usable serial console, so the numbers that decide whether a
// button press can be seen at all have nowhere to go. This writes them to the SD card instead, which
// is already the only channel in and out of those devices.
//
// The quantity that matters is the interval between consecutive gpio.update() calls. InputManager
// commits a state change only once two consecutive samples agree (DEBOUNCE_DELAY = 5 ms), so a press
// shorter than that interval can land in a single sample and never produce an edge at all. At
// LOW_POWER_FREQ the idle loop delay is 50 ms and the loop body itself runs 16x slower, which is how
// menu presses went missing.
//
// Compiled out unless INPUT_DIAG is defined. Put it in platformio.local.ini (gitignored), never in a
// release build: the calls below become empty inlines the optimizer removes, so the shipped firmware
// pays nothing.

#ifdef INPUT_DIAG
class InputDiag {
 public:
  // Once per main-loop iteration, immediately after gpio.update().
  static void sample(unsigned long nowMs, bool committedEdge, bool debouncePending);

  // One completed activity render, measured in the render task: drawing plus the panel refresh.
  // The panel refresh is a fixed few hundred ms, so anything well above that is drawing -- which on
  // a screen full of SD-card CJK glyphs means card reads, not pixels.
  //
  // The activity name is recorded per render because a running maximum cannot say *which* render was
  // slow, and on a screen whose glyph cache warms on first use the first render and the tenth are
  // different questions.
  static void noteRender(const char* activityName, unsigned long durationMs);

  // One page render's phase breakdown, in milliseconds. renderContents already measures these for
  // LOG_DBG; on a device with no serial console that measurement had nowhere to go, which is why the
  // three seconds a page turn costs stayed unattributed through two wrong guesses.
  static void notePageRender(unsigned long prewarmMs, unsigned long drawMs, unsigned long displayMs);

  // The draw phase split one level further: laying the page's blocks down, and the status bar.
  // Needed because the body-cell loops inside renderVertical accounted for 38 ms of a 520 ms draw --
  // the rest is somewhere neither of them covers.
  static void notePageDrawParts(unsigned long blocksMs, unsigned long statusBarMs);

  // Where renderVertical's time went for one page. Vertical drawing measured 2119 ms against
  // horizontal's 76 ms on identical text; this splits that figure into the body cells, the pass that
  // measures each ruby run, and the pass that draws it.
  static void noteVerticalRender(unsigned long bodyMs, unsigned long bodyCells, unsigned long rubyMeasureMs,
                                 unsigned long rubyDrawMs, unsigned long rubyGroups);

  // One Section::buildSomeMore() call: the spine item, the page-count watermark before and
  // after, and how long it took. buildSomeMore has no internal time budget -- it lays out
  // whatever chunk it's given as one atomic call, holding RenderLock (and, from most call
  // sites, HalPowerManager::Lock) for the duration -- so a single pathological page anywhere
  // in a chapter can freeze input for as long as that one call takes. render_max_ms alone
  // can't say *where*: this pins the worst chunk to a spine index and page range so a repeat
  // capture can point at the actual content instead of just the duration.
  static void noteBuildChunk(int spineIndex, uint16_t pageCountBefore, uint16_t pageCountAfter,
                             unsigned long durationMs);

  // Sum of every buildSomeMore() call made to catch one render's target page up, across
  // however many chunks that took. A render can be slow from many cheap chunks in a long
  // loop just as easily as from one expensive one; noteBuildChunk's per-chunk max misses
  // that shape entirely, so this pins the worst render-level total separately.
  static void noteBuildTotal(int spineIndex, unsigned long totalMs, int chunkCount);

  // One UI glyph prewarm that reported failure (see UiGlyphPrewarm). Records the count and the
  // tightest max-alloc seen at such a failure, so a slow list screen can be attributed to the
  // prewarm not landing rather than to the drawing itself.
  static void noteUiPrewarmFailure();

  // Snapshot the RTC log ring into memory, to be written out by the next flush().
  //
  // Copies rather than writing on the spot: this is called from failure paths that may already
  // hold the storage mutex, and the ring is only sixteen lines deep, so waiting for the flush
  // would let routine debug output evict the very error being chased.
  static void captureLogs(const char* reason);

  // Rewrites the snapshot on the SD card, at most every few seconds.
  static void flush(bool inputActive);
};
#else
class InputDiag {
 public:
  static void sample(unsigned long, bool, bool) {}
  static void noteRender(const char*, unsigned long) {}
  static void notePageRender(unsigned long, unsigned long, unsigned long) {}
  static void notePageDrawParts(unsigned long, unsigned long) {}
  static void noteVerticalRender(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) {}
  static void noteBuildChunk(int, uint16_t, uint16_t, unsigned long) {}
  static void noteBuildTotal(int, unsigned long, int) {}
  static void noteUiPrewarmFailure() {}
  static void captureLogs(const char*) {}
  static void flush(bool) {}
};
#endif

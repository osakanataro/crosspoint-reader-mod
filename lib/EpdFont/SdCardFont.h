#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"

class HalFile;

// On-disk binary format version for .cpfont files. Defined as a preprocessor
// macro (rather than a constexpr) so it can be stringified into the SD-fonts
// release URL — see FONT_MANIFEST_URL in FontDownloadActivity.h. No integer
// suffix because stringification would include it (e.g. `4U` → `"4U"`).
//
// The canonical version for the build tooling lives in
// lib/EpdFont/scripts/cpfont_version.py. This firmware-side copy must be
// bumped manually when the firmware is updated to support a new format.
// Reader enforcement: SdCardFont::load().
#define CPFONT_VERSION 4

class SdCardFont {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  // prewarmStyle: the bitmap arena did not fit the largest free block.
  // Distinct from a missed-glyph count so the caller can retry smaller.
  static constexpr int PREWARM_ARENA_TOO_LARGE = -2;
  static constexpr uint8_t MAX_STYLES = 4;

  // Recent frees of a style's mini arena, for INPUT_DIAG (who dropped the page's glyphs, and when).
  struct MiniFreeEvent {
    uint32_t caller = 0;
    uint32_t ms = 0;
    uint32_t freeHeap = 0;
    uint16_t glyphs = 0;
    uint8_t style = 0;
    bool metadataOnly = false;
  };
  static constexpr uint8_t MINI_FREE_EVENTS = 4;
  static const MiniFreeEvent* miniFreeEvents(uint32_t& total);

  SdCardFont() = default;
  ~SdCardFont();
  // Owns raw buffers freed in dtor — no shallow-copy semantics. Make any
  // accidental pass-by-value or move a compile-time error.
  SdCardFont(const SdCardFont&) = delete;
  SdCardFont& operator=(const SdCardFont&) = delete;
  SdCardFont(SdCardFont&&) = delete;
  SdCardFont& operator=(SdCardFont&&) = delete;

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  // isReaderFont: the family's body-text size (its first load), reported by INPUT_DIAG.
  bool load(const char* path, bool isReaderFont = false);

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // styleMask: bitmask of styles to prewarm (bit 0=regular, 1=bold, 2=italic, 3=bolditalic).
  // Default 0x0F = all present styles.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data).
  // Incremental string prewarms accumulate up to MAX_PAGE_GLYPHS so adjacent
  // UI labels do not evict each other.
  // Complete render scans pass accumulate=false: rebuild for this page only,
  // while retaining buffers and allowing a resident subset hit.
  // Returns number of glyphs that couldn't be loaded (0 on full success).
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false, bool loadKernLig = true,
              bool accumulate = true);

  // Multi-string variant: extracts codepoints from `textCount` strings fetched
  // one at a time through `getter` (C-style callback: no std::function bloat,
  // and callers never build a concatenated copy — a heap-tight screen aborting
  // in a bare-new string append is exactly what this avoids). A null getter
  // result skips that index. Unique codepoints cap at MAX_PAGE_GLYPHS.
  // loadKernLig=false skips kern/ligature loading and the mini kern matrix:
  // UI fallback text (CJK titles) has no useful kern pairs, and the ~3KB class
  // tables plus per-rebuild matrix work were enough to OOM the batch on
  // heap-tight screens. Reader-quality paths keep the default.
  using TextGetter = const char* (*)(const void* ctx, uint32_t index);
  int prewarm(TextGetter getter, const void* ctx, uint32_t textCount, uint8_t styleMask = 0x0F,
              bool metadataOnly = false, bool loadKernLig = true, bool accumulate = true);

  // Build a compact advance-only table for layout measurement.
  // Extracts ALL unique codepoints from words (no MAX_PAGE_GLYPHS cap),
  // batch-reads advanceX from SD, stores in a sorted per-style table.
  // extraText: optional additional codepoints to warm in the same SD pass
  // (e.g. shaped Arabic presentation forms the measurement path will look up).
  // Returns number of codepoints not found in font coverage.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F, const char* extraText = nullptr);
  // Packed variant: each segment holds consecutive NUL-terminated words
  // (paragraph word-arena chunks), scanned without per-word string objects.
  int buildAdvanceTablePacked(const char* const* segments, const size_t* segmentLens, size_t segmentCount,
                              bool includeSpace, bool includeHyphen, uint8_t styleMask = 0x0F,
                              const char* extraText = nullptr);

  // Look up advanceX for a codepoint from the advance table.
  // Returns the 12.4 fixed-point advance, or 0 if not found.
  uint16_t getAdvance(uint32_t codepoint, uint8_t style) const;
  // Advance of one codepoint the table does not hold, without loading its bitmap: the resident
  // arena, then the overflow ring, then the 12-byte glyph record straight from the card. 0 when
  // the font has no such glyph (or the read failed), so callers can fall back to a full load.
  uint16_t readAdvanceOnly(uint32_t codepoint, uint8_t styleIdx) const;
  // Close the file readAdvanceOnly keeps open for the duration of a measuring pass.
  void closeMeasureFile() const;

  // Returns true if advance table is populated for at least one style.
  bool hasAdvanceTable() const;

  // Free mini data for all styles and restore stub EpdFontData.
  // Preserves the persistent advance cache so repeated layout passes can reuse
  // previously fetched metrics.
  void clearCache();

  // Drop the persistent advance cache. Call when unloading the SD font or
  // when font/size/family/glyph-table state changes, or to recover a failed bitmap allocation.
  void clearPersistentCache();

  // Release every rebuildable cache while keeping the font loaded and usable:
  // mini glyph/kern arenas, kern/ligature class tables, the overflow ring, and
  // the persistent advance tables. Coverage intervals stay so hasCodepoint()
  // and reloads keep working; glyphs fault back in on demand and the next
  // prewarm rebuilds the arenas. For heap-critical transitions (e.g. starting
  // WiFi + the web server), where retained font data is the difference between
  // a clean start and an OOM abort.
  void releaseResidentCaches();

  // Returns pointer to the managed EpdFont for a given style.
  // Returns nullptr if the style is not present.
  EpdFont* getEpdFont(uint8_t style = 0);

  // Returns true if the given style is present in this font file.
  bool hasStyle(uint8_t style) const;

  // Resolve requested style bits to the closest present style.
  uint8_t resolveStyle(uint8_t style) const;

  // Resolve every requested style bit through fallback and return the actual
  // styles that need cache/advance preparation.
  uint8_t resolveStyleMask(uint8_t styleMask) const;

  // Number of styles present in this font file.
  uint8_t styleCount() const { return styleCount_; }

  // Returns true if the glyph pointer points into the overflow buffer.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;

  // Returns the bitmap for an on-demand-loaded (overflow) glyph.
  const uint8_t* getOverflowBitmap(const EpdGlyph* glyph) const;

  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);

  struct Stats {
    uint32_t prewarmTotalMs = 0;
    uint32_t sdReadTimeMs = 0;
    uint32_t seekCount = 0;
    uint32_t uniqueGlyphs = 0;
    uint32_t bitmapBytes = 0;
  };
  void logStats(const char* label = "SDCF");
  void resetStats();
  const Stats& getStats() const { return stats_; }

  // Counters for INPUT_DIAG. Glyphs read one at a time through the overflow ring because no
  // prewarm covered them; mini-arena rebuilds (the reads go straight into the arena, so no other
  // figure records them) and their time; prewarms that bailed before any style was touched; and
  // the advance-table work a section build spends its font time on.
  uint32_t overflowLoads() const { return overflowLoads_; }
  uint32_t miniRebuilds() const { return miniRebuilds_; }
  uint32_t miniRebuildMs() const { return miniRebuildMs_; }
  uint32_t prewarmEntryFails() const { return prewarmEntryFails_; }
  uint32_t advanceFetchCalls() const { return advanceFetchCalls_; }
  uint32_t advanceFetchMs() const { return advanceFetchMs_; }
  uint32_t advanceFullSkips() const { return advanceFullSkips_; }
  // readAdvanceOnly: calls, the ones that had to read the card, and the time those reads took.
  uint32_t advanceOnlyCalls() const { return advanceOnlyCalls_; }
  uint32_t advanceOnlySdReads() const { return advanceOnlySdReads_; }
  uint32_t advanceOnlyMs() const { return advanceOnlyMs_; }
  uint32_t advanceTableMax() const {
    uint32_t largest = 0;
    for (uint8_t i = 0; i < MAX_STYLES; i++) {
      if (advanceTableSize_[i] > largest) largest = advanceTableSize_[i];
    }
    return largest;
  }
  static constexpr uint32_t advanceTableLimit() { return ADVANCE_CACHE_LIMIT; }

  // Content hash of the file header + style TOC entries (computed during load).
  // Used to generate deterministic font IDs for section cache invalidation.
  uint32_t contentHash() const { return contentHash_; }

 private:
  // Per-style metadata (parsed from file header/TOC)
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  // All per-style data: file offsets, intervals, kern/lig, prewarm cache, EpdFont
  struct PerStyle {
    CpFontHeader header{};

    // File layout offsets for this style's data sections
    uint32_t intervalsFileOffset = 0;
    uint32_t glyphsFileOffset = 0;
    uint32_t kernLeftFileOffset = 0;
    uint32_t kernRightFileOffset = 0;
    uint32_t kernMatrixFileOffset = 0;
    uint32_t ligatureFileOffset = 0;
    uint32_t bitmapFileOffset = 0;

    // Full intervals loaded from file (kept in RAM for codepoint lookup)
    EpdUnicodeInterval* fullIntervals = nullptr;
    EPD_PACKED_BEGIN
    struct BmpInterval16 {
      uint16_t first;
      uint16_t last;
      uint16_t offset;
    } EPD_PACKED_ATTR;
    EPD_PACKED_END
    static_assert(sizeof(BmpInterval16) == 6, "BmpInterval16 must remain compact");
    BmpInterval16* bmpIntervals = nullptr;
    bool intervalsAreBmp16 = false;
    // True when bmpIntervals/fullIntervals above points at another style's table rather than
    // this style's own allocation. Regular/bold/italic weights of the same family almost always
    // cover the identical codepoint set, so a CJK font's multi-KB-per-style table is otherwise
    // paid for once per style. Only the owning style frees it -- see freeStyleAll().
    bool intervalsShared = false;

    // Persistent kern-class + ligature tables (lazy-loaded on first prewarm).
    // The full kern MATRIX is NOT resident — on Literata-class fonts a single
    // style's matrix is ~36-42KB contiguous, and 4 styles' worth won't fit
    // alongside bitmaps + framebuffer on a 380KB device. Only kernLeftClasses
    // and kernRightClasses (small codepoint→classId tables, ~3KB each) stay
    // resident; the matrix is reconstructed per-page as miniKernMatrix.
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    bool kernLigLoaded = false;

    // Stub EpdFontData returned when not prewarmed
    EpdFontData stubData{};

    // Mini EpdFontData built during prewarm. Buffers are kept-if-fits across pages
    // (capacities below track allocated sizes): freeing and reallocating slightly
    // different sizes on every page turn was a primary heap fragmenter — each page's
    // freed hole rarely fit the next page's need, so maxAlloc eroded all session.
    // The per-render PrewarmScope calls clearCache() -> resetStyleMiniData(), which
    // keeps both the allocations AND the loaded data. Buffers: reuse means
    // ensureArrayCapacity early-returns once capacities converge on the book's
    // max, so page turns stop touching the allocator (the free/realloc-per-page
    // pattern was a primary heap fragmenter). Data: the next prewarm
    // subset-checks against the resident tables (see prewarmStyle), so the idle
    // prewarm of page N+1 serves the actual turn with zero SD reads. Retention
    // is bounded two ways in resetStyleMiniData(): a heap floor frees outright
    // under pressure, and sustained underuse (an outlier page's oversized bitmap
    // arena) frees after a few consecutive low-use rebuilds. freeStyleMiniData()
    // remains the full teardown (zeroes capacities) for style eviction / font
    // unload.
    EpdFontData miniData{};
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;
    uint32_t miniIntervalCapacity = 0;
    uint32_t miniGlyphCapacity = 0;
    uint32_t miniBitmapCapacity = 0;
    // Bitmap bytes the current page actually used (set by prewarmStyle), the
    // underuse-hysteresis signal; 0 = no bitmap built this scope (metadata-only
    // prewarm), which leaves the hysteresis counter untouched.
    uint32_t miniBitmapUsed = 0;
    // Exact bitmap bytes per glyph of the last requested set, for the arena retry.
    uint32_t measuredBytesPerGlyph = 0;
    uint8_t miniUnderuseRuns = 0;
    // True when the resident mini was built metadata-only (no bitmaps): it can
    // serve metadata requests but a full render request must rebuild.
    bool miniMetadataOnly = false;
    // Set by a rebuild, consumed by resetStyleMiniData: gates the underuse
    // hysteresis to one evaluation per rebuild (scopes reset twice, and subset
    // hits load nothing new to judge).
    bool miniHysteresisPending = false;

    // Per-page mini kern matrix (built by buildMiniKernMatrix on each full
    // prewarm). miniKernLeftClasses/miniKernRightClasses map ONLY the codepoints
    // used on the current page to renumbered class IDs (1..miniKern*ClassCount).
    // miniKernMatrix is a small miniKernLeftClassCount × miniKernRightClassCount
    // flat matrix. Typical Latin page: ~25×25 matrix = ~625 bytes per style vs
    // ~36KB for the full Literata matrix — ~50× reduction.
    EpdKernClassEntry* miniKernLeftClasses = nullptr;
    EpdKernClassEntry* miniKernRightClasses = nullptr;
    uint16_t miniKernLeftEntryCount = 0;
    uint16_t miniKernRightEntryCount = 0;
    uint8_t miniKernLeftClassCount = 0;
    uint8_t miniKernRightClassCount = 0;
    int8_t* miniKernMatrix = nullptr;
    // Kept-if-fits capacities, same rationale as the mini glyph buffers above.
    uint16_t miniKernLeftCapacity = 0;
    uint16_t miniKernRightCapacity = 0;
    uint32_t miniKernMatrixCapacity = 0;

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // Shared on-demand overflow buffer (ring buffer of glyphs loaded via glyphMissHandler)
  static constexpr uint32_t OVERFLOW_CAPACITY = 8;
  struct OverflowEntry {
    EpdGlyph glyph;
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
  };
  OverflowEntry overflow_[OVERFLOW_CAPACITY] = {};
  uint32_t overflowCount_ = 0;
  uint32_t overflowNext_ = 0;

  // Compact advance-only table for layout measurement (per-style).
  // Built by buildAdvanceTable(), queried by getAdvance().
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
  };
  // Per-style advance table. Sorted by codepoint for binary lookup.
  // Bounded to ADVANCE_CACHE_LIMIT entries; persists across layout passes
  // (across calls to clearCache()) so repeated indexing of the same font
  // amortizes SD reads. Cleared only on font unload or clearPersistentCache().
  static constexpr uint32_t ADVANCE_CACHE_LIMIT = 768;
  AdvanceEntry* advanceTable_[MAX_STYLES] = {};
  uint32_t advanceTableSize_[MAX_STYLES] = {};
  bool advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const;
  // Merge sortedNew (sorted by codepoint, no overlap with existing) into the
  // advance table for styleIdx, preserving sort order; cap-truncates the tail.
  void mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount);

  Stats stats_;
  uint32_t overflowLoads_ = 0;
  uint32_t miniRebuilds_ = 0;
  uint32_t miniRebuildMs_ = 0;
  uint32_t prewarmEntryFails_ = 0;
  uint32_t advanceFetchCalls_ = 0;
  uint32_t advanceFetchMs_ = 0;
  uint32_t advanceFullSkips_ = 0;
  mutable uint32_t advanceOnlyCalls_ = 0;
  mutable uint32_t advanceOnlySdReads_ = 0;
  mutable uint32_t advanceOnlyMs_ = 0;
  // Kept open across readAdvanceOnly calls while a build measures (closed by closeMeasureFile):
  // opening the file by its long name was nearly all of each 3.9 ms read.
  mutable std::unique_ptr<HalFile> measureFile_;
  // Widths readAdvanceOnly already read from the card during this measuring pass, direct-mapped
  // by codepoint (key = codepoint | style << 22 | valid bit). A chapter repeats its characters, so
  // most calls past the full advance table are for a character read moments before. 3 KB, freed
  // with the file.
  static constexpr uint32_t MEASURE_MEMO_SLOTS = 512;
  mutable std::unique_ptr<uint32_t[]> measureMemoKeys_;
  mutable std::unique_ptr<uint16_t[]> measureMemoAdvances_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  // Per-style helpers
  void freeStyleMiniData(PerStyle& s);
  // Per-scope variant: drop the page's data, keep the allocations (see the
  // PerStyle comment). May escalate to freeStyleMiniData under heap pressure
  // or sustained underuse.
  void resetStyleMiniData(PerStyle& s);
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  void freeStyleMiniKern(PerStyle& s);
  bool loadStyleKernLigatureData(PerStyle& s);
  bool buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount);
  void applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const;
  void applyGlyphMissCallback(uint8_t styleIdx);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  int fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask);
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly, bool loadKernLig,
                   bool accumulate);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);

  // Static callback for EpdFontData::coverageHandler: answers hasCodepoint()
  // from the RAM-resident full interval table, without SD I/O.
  static bool onCoverageQuery(void* ctx, uint32_t codepoint);
};

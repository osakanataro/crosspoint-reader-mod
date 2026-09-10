#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  // Release every rebuildable SD-font cache (mini glyph/kern arenas, kern/lig
  // class tables, overflow rings, advance tables) while keeping the fonts
  // loaded. Everything faults back in on demand. For heap-critical transitions
  // (e.g. web-server + WiFi startup); see SdCardFont::releaseResidentCaches().
  void releaseSdFontCaches();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // What the last completed scan pass handed to prewarmCache: total UTF-8
  // bytes and how many font-and-style groups were batched (per-style since the
  // packed-codepoint scan; the old entry model counted per font id). Zero
  // bytes after a page scope means the scan hook never fired -- the draw path
  // bypassed drawText's recording, which no other figure distinguishes from a
  // prewarm that ran and failed. Read by the INPUT_DIAG page hook.
  uint32_t lastScanBytes() const { return lastScanBytes_; }
  uint8_t lastScanFonts() const { return lastScanFonts_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;

  // A render pass touches at most a handful of font ids. Codepoints are packed
  // with a compact font slot and resolved style, then grouped for prewarming.
  // Six slots because a reader page already needs four (the reading face, its CJK fallback,
  // and the two status-bar UI faces) and a heading or a size change adds more; a font past
  // the cap is dropped from the scan silently, and every glyph it draws then loads one at a
  // time from the card. The cost of a slot is four group counters (8 bytes).
  static constexpr uint8_t MAX_SCAN_FONTS = 6;
  static constexpr uint16_t MAX_SCAN_CODEPOINTS = 512;
  static constexpr uint8_t SCAN_STYLE_SHIFT = 21;
  static constexpr uint8_t SCAN_FONT_SHIFT = SCAN_STYLE_SHIFT + 2;
  static constexpr uint32_t SCAN_CODEPOINT_MASK = (1U << SCAN_STYLE_SHIFT) - 1;
  static constexpr uint8_t SCAN_GROUP_COUNT = MAX_SCAN_FONTS * 4;

  uint8_t resolveScanStyle(int fontId, EpdFontFamily::Style style) const;
  int scanFontIds_[MAX_SCAN_FONTS] = {};
  uint32_t scanCodepoints_[MAX_SCAN_CODEPOINTS + 1] = {};
  uint16_t scanGroupCounts_[SCAN_GROUP_COUNT] = {};
  uint16_t scanCodepointCount_ = 0;
  uint8_t scanFontCount_ = 0;
  bool scanOverflowWarned_ = false;
  // Times a draw named a font the scan had no slot left for. Non-zero means the page's
  // prewarm was incomplete by construction.
  uint32_t scanFontOverflow_ = 0;
  uint32_t lastScanBytes_ = 0;
  uint8_t lastScanFonts_ = 0;

 public:
  [[nodiscard]] uint32_t scanFontOverflows() const { return scanFontOverflow_; }
};

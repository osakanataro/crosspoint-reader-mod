#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;
struct SdCardFontFileInfo;

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load the family's .cpfont at `pointSize`, or the nearest size it ships if
  // that exact size is not installed. Only one .cpfont file is loaded; other
  // sizes remain on disk. This keeps resident interval + kern/ligature tables to
  // one size's worth of memory. Returns true on success.
  // preferFlash: use the copy in the inactive OTA slot when it holds the file
  // cacheCandidate() names for this size (possibly a smaller file drawn scaled);
  // otherwise the size's own file is read from the card as usual.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t pointSize,
                  bool preferFlash = false);

  // The file the flash copy should hold to serve `pointSize`, and the scale to
  // draw it at. The size's own file when its regular style fits the slot;
  // otherwise the next smaller size that fits and is within 8/9 of it (only
  // 16 pt for 18 pt among the shipped sizes: a larger upscale visibly thickens
  // the strokes), scaled up by pointSize/its size. file == nullptr when nothing
  // fits. Opens each candidate's header on the card, so not for hot paths.
  struct CacheCandidate {
    const SdCardFontFileInfo* file = nullptr;
    uint8_t scaleNum = 1;
    uint8_t scaleDen = 1;
  };
  static CacheCandidate cacheCandidate(const SdCardFontFamilyInfo& family, uint8_t pointSize);

  // True when the reader-size font reads from the flash copy.
  bool readerFontFromFlash() const;

  // Additively load the .cpfont of `family` at the exact physical `pointSize`
  // (used for size-matched CJK UI fallback alongside the reader-size font).
  // Does not unload anything. If a font of that size is already loaded its id
  // is reused. Returns the font id, or 0 if the family has no file at that size
  // or loading failed.
  int loadFamilyExtraSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t pointSize);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for the loaded family. Returns 0 if nothing loaded
  // or familyName doesn't match.
  int getFontId(const std::string& familyName) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // Point size that was actually loaded.
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);

  // Load+register a single .cpfont file and append it to loaded_.
  // Returns the font id, or 0 on failure (allocation, read, or id collision).
  // idPointSize is the size the font is registered and identified as (it is the file's own
  // size unless the file is loaded scaled, see cacheCandidate()); scaleNum/scaleDen and
  // preferFlash go to SdCardFont::load().
  int loadFile(const SdCardFontFileInfo& file, const char* familyName, GfxRenderer& renderer, uint8_t idPointSize,
               uint8_t scaleNum = 1, uint8_t scaleDen = 1, bool preferFlash = false);

  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  std::vector<LoadedFont> loaded_;
};

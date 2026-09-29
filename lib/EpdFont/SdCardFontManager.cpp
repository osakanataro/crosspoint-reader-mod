#include "SdCardFontManager.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontCache.h>
#include <SdCardFontRegistry.h>

SdCardFontManager::~SdCardFontManager() {
  for (auto& lf : loaded_) {
    delete lf.font;
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
// Produces a deterministic ID that is stable across load/unload cycles and reboots,
// and changes when font content changes (different header/TOC = different contentHash).
int SdCardFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 is reserved as "not found" sentinel
}

int SdCardFontManager::loadFile(const SdCardFontFileInfo& file, const char* familyName, GfxRenderer& renderer,
                                const uint8_t idPointSize, const uint8_t scaleNum, const uint8_t scaleDen,
                                const bool preferFlash) {
  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "Failed to allocate SdCardFont for %s", file.path.c_str());
    return 0;
  }

  // The family's first load is the body-text size; the UI fallback sizes follow it.
  if (!font->load(file.path.c_str(), loaded_.empty(), scaleNum, scaleDen, preferFlash)) {
    LOG_ERR("SDMGR", "Failed to load %s", file.path.c_str());
    delete font;
    return 0;
  }

  // A scaled load carries its own size code, so its section caches and font id never
  // collide with the unscaled file's.
  int fontId = computeFontId(font->contentHash(), familyName, idPointSize);
  // Guard against collision with built-in font IDs (astronomically unlikely
  // with FNV-1a hashes, but provides a safety net)
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, file.path.c_str());
    delete font;
    return 0;
  }
  renderer.registerSdCardFont(fontId, font);
  loaded_.push_back({font, fontId, idPointSize});

  LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u scale=%u/%u source=%s", file.path.c_str(), idPointSize, fontId,
          font->styleCount(), scaleNum, scaleDen, font->usingFlash() ? "flash" : "sd");

  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, fontFamily);
  return fontId;
}

SdCardFontManager::CacheCandidate SdCardFontManager::cacheCandidate(const SdCardFontFamilyInfo& family,
                                                                    const uint8_t pointSize) {
  CacheCandidate out;
  const SdCardFontFileInfo* own = family.findFile(pointSize);
  if (own && SdCardFontCache::sourceFits(own->path.c_str())) {
    out.file = own;
    return out;
  }
  // Largest smaller size within the upscale limit (base * 9 >= pointSize * 8, i.e. at most 9/8).
  const SdCardFontFileInfo* base = nullptr;
  for (const auto& f : family.files) {
    if (f.style != 0 || f.pointSize >= pointSize) continue;
    if (static_cast<unsigned>(f.pointSize) * 9 < static_cast<unsigned>(pointSize) * 8) continue;
    if (!base || f.pointSize > base->pointSize) base = &f;
  }
  if (base && SdCardFontCache::sourceFits(base->path.c_str())) {
    out.file = base;
    out.scaleNum = pointSize;
    out.scaleDen = base->pointSize;
  }
  return out;
}

bool SdCardFontManager::readerFontFromFlash() const { return !loaded_.empty() && loaded_.front().font->usingFlash(); }

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t pointSize,
                                   const bool preferFlash) {
  // Unload any previously loaded family first
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  const SdCardFontFileInfo* selected = family.findNearestSize(pointSize);
  if (!selected) {
    LOG_ERR("SDMGR", "Family %s has no files to load", family.name.c_str());
    return false;
  }
  // The size is the selected file's own; the file read may be a smaller one drawn scaled
  // when that is what the flash copy holds for this size.
  const uint8_t idPointSize = selected->pointSize;
  uint8_t scaleNum = 1;
  uint8_t scaleDen = 1;
  bool fromFlash = false;
  if (preferFlash) {
    const CacheCandidate cand = cacheCandidate(family, idPointSize);
    if (cand.file && SdCardFontCache::isValidFor(cand.file->path.c_str())) {
      selected = cand.file;
      scaleNum = cand.scaleNum;
      scaleDen = cand.scaleDen;
      fromFlash = true;
    }
  }

  if (loadFile(*selected, family.name.c_str(), renderer, idPointSize, scaleNum, scaleDen, fromFlash) == 0) {
    return false;
  }

  loadedFamilyName_ = family.name;
  loadedPointSize_ = idPointSize;
  return true;
}

int SdCardFontManager::loadFamilyExtraSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                           uint8_t pointSize) {
  const SdCardFontFileInfo* file = family.findFile(pointSize);
  if (!file) return 0;  // family has no .cpfont at this exact size

  // Reuse an already-loaded font of the same size (e.g. when a reader size
  // happens to match a UI size) instead of double-loading the file.
  for (const auto& lf : loaded_) {
    if (lf.size == pointSize) return lf.fontId;
  }

  // The copy holds the reader size; a UI size only ever reads the card. Passing preferFlash
  // would open the file once per size just to learn that.
  return loadFile(*file, family.name.c_str(), renderer, file->pointSize);
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  // Drop UI CJK fallbacks before the SD fonts they point at are freed.
  renderer.clearFallbackFonts();
  renderer.clearSdCardFonts();
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
  loadedPointSize_ = 0;
}

int SdCardFontManager::getFontId(const std::string& familyName) const {
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;
  return loaded_.front().fontId;
}

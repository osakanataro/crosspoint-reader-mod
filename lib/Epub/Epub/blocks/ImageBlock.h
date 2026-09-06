#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;
  bool hasValidCache() const;
  bool needsDecode() const;

  // The .pxc path for an extracted image, and whether a usable cache already
  // sits there. Static so the section build can ask before it owns an
  // ImageBlock -- build-time pregeneration decides extract/decode work with
  // exactly these two questions.
  static std::string cachePathFor(const std::string& imagePath);
  static bool hasValidCacheFor(const std::string& imagePath, int width, int height);
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
  static void clearSessionRenderFailures();

  // A page render draws its image up to ~13 times (BW double-refresh plus every
  // grayscale band pass), and each draw streams the whole .pxc off SD. The
  // first draw caches the pixel payload in RAM (chunked, heap-gated, falls back
  // to streaming when it doesn't fit); the reader calls this when the page
  // render completes so nothing stays resident between pages.
  static void releaseRenderCache();

#ifdef INPUT_DIAG
  // Diagnostic only: how a page's draws got their pixels. The two grayscale
  // planes run identical loops, so bracketing each one and comparing the split
  // says whether an uneven pass cost is SD traffic for the image or something
  // else. Reading the stats zeroes them.
  struct CacheRenderStats {
    uint32_t slotHits;     // drawn straight from the RAM slot, no SD access
    uint32_t slotLoads;    // the slot was filled from SD for this draw
    uint32_t streamDraws;  // streamed off SD a row-batch at a time (no slot)
    uint32_t sdMs;         // time inside the draws that touched SD
  };
  static CacheRenderStats takeCacheRenderStats();
#endif

  // Lazy extraction hook: the section build only header-probes images for their
  // dimensions; the file at imagePath is extracted out of the book on first
  // render, via this callback (function pointer + context, not std::function —
  // this is render-loop code). Registered by the reader activity that owns the
  // Epub, cleared on its exit.
  using ExtractFn = bool (*)(void* ctx, const char* srcPath, const char* destPath);
  static void setExtractor(void* ctx, ExtractFn fn);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  std::string srcPath;  // book-internal source href; empty once known-extracted
  int16_t width;
  int16_t height;

  static void* extractCtx;
  static ExtractFn extractFn;
};

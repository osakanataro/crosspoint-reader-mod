#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>

class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  /// forceReload: reload even when the same family and size are resident -- the
  /// flash copy was just written or switched off, so the read source changed.
  void ensureLoaded(GfxRenderer& renderer, bool forceReload = false);

  /// The file the flash copy must hold for the selected family and size (see
  /// SdCardFontManager::cacheCandidate), or nullptr when a built-in family is
  /// selected or nothing of this size fits the slot.
  const SdCardFontFileInfo* cacheCandidate(uint8_t* scaleNum = nullptr, uint8_t* scaleDen = nullptr) const;
  /// True when the flash copy is switched on in settings but the slot does not
  /// hold the candidate file (never built, another size, or wiped by a firmware
  /// update): the reader falls back to the card until it is rebuilt.
  bool cacheRebuildNeeded() const;
  /// True when the battery is too low to start a copy into flash (a copy takes about a
  /// minute of continuous SD reads and flash writes and must not be cut by a dead battery).
  /// USB power lifts the limit. When true, callers switch the setting off and read from the card.
  static bool copyBlockedByBattery();
  static constexpr uint16_t COPY_MIN_BATTERY_PERCENT = 20;
  /// True when the resident reader font reads from the flash copy.
  bool readerFontFromFlash() const { return manager_.readerFontFromFlash(); }

  /// Resolve an SD card font ID from family name + reader point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
  // Load the active SD family at the built-in UI point sizes and register each
  // as a size-matched CJK fallback for the corresponding UI font, so CJK book
  // titles/list rows render at the same size as the surrounding Latin UI text.
  // No-op when no SD family is loaded. Safe to call repeatedly (sizes already
  // loaded are reused).
  void setupUiFallbacks(GfxRenderer& renderer);

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;

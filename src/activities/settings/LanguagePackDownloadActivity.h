#pragma once

#include <I18nKeys.h>

#include "activities/Activity.h"

#ifndef LANG_PACK_BASE_URL
// Packs are published as release assets named <code>.cplang (lowercase); the
// tag carries the pack format version (PACK_VERSION in UiLanguagePackStore.cpp).
#define LANG_PACK_BASE_URL "https://github.com/osakanataro/crosspoint-ost-assets/releases/download/ui-lang-p2/"
#endif

// Downloads the chosen UI language's pack into /lang, then switches to the
// language. When no font on the card draws it, hands over to the font download
// on the same Wi-Fi session.
class LanguagePackDownloadActivity final : public Activity {
 public:
  LanguagePackDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Language lang)
      : Activity("LanguagePackDownload", renderer, mappedInput), lang_(lang) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return state_ == DOWNLOADING; }
  void render(RenderLock&&) override;

  // Downloads `lang`'s pack to `tmpPath` and keeps it only if load() would
  // accept it. On failure sets `error`. Needs Wi-Fi and the SD fonts unloaded.
  static bool fetchPack(Language lang, const char* tmpPath, StrId& error);

 private:
  enum State { WIFI_SELECTION, DOWNLOADING, COMPLETE, FAILED };

  const Language lang_;
  State state_ = WIFI_SELECTION;
  StrId error_ = StrId::STR_DOWNLOAD_FAILED;

  void onWifiSelectionComplete(bool connected);
  bool download();
  void finishDownload();
};

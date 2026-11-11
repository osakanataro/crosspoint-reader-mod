#pragma once

#include <I18nKeys.h>

#include "activities/Activity.h"

#ifndef LANG_PACK_BASE_URL
// Packs are crosspoint-assets release assets named <code>.cplang (lowercase),
// built by scripts/build-language-assets.py, next to the hyphenation packs.
// A pack is matched to the firmware by key name, so the latest release serves
// every firmware that reads its pack format (PACK_VERSION in UiLanguagePackStore.cpp).
#define LANG_PACK_BASE_URL "https://github.com/crosspoint-reader/crosspoint-assets/releases/latest/download/"
#endif

// Downloads the chosen UI language's pack into /lang, then switches to the
// language. When no font on the card draws it, hands over to the font download
// on the same Wi-Fi session.
class LanguagePackDownloadActivity final : public Activity {
 public:
  // `restoreAtBoot`: the saved language has no pack (the firmware no longer
  // builds it in). Asks first; declining saves English so the next boot does
  // not ask again, while a failed download keeps the language and asks again.
  LanguagePackDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Language lang,
                               bool restoreAtBoot = false)
      : Activity("LanguagePackDownload", renderer, mappedInput), lang_(lang), restoreAtBoot_(restoreAtBoot) {}

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
  const bool restoreAtBoot_;
  bool declined_ = false;
  State state_ = WIFI_SELECTION;
  StrId error_ = StrId::STR_DOWNLOAD_FAILED;

  void startWifiSelection();
  void onWifiSelectionComplete(bool connected);
  bool download();
  void finishDownload();
};

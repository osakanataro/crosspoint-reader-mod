#include "LanguagePackDownloadActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "UiLanguagePackStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

void LanguagePackDownloadActivity::onEnter() {
  Activity::onEnter();
  {
    // Free glyph caches before Wi-Fi allocates, so caches refilled later do not
    // split the large free block TLS needs.
    RenderLock lock(*this);
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  }
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void LanguagePackDownloadActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void LanguagePackDownloadActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
  }
  requestUpdate();
}

bool LanguagePackDownloadActivity::download() {
  {
    // TLS needs more heap than the loaded SD fonts leave on X3. The downloading
    // screen is already drawn; finishDownload() reloads them before the next.
    RenderLock lock(*this);
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
    sdFontSystem.unloadAll(renderer);
  }
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("LANG", "Low heap for download (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    error_ = StrId::STR_MEMORY_ERROR;
    return false;
  }

  char path[32];
  UiLanguagePackStore::packPath(lang_, path, sizeof(path));
  const char* name = path + sizeof("/lang/") - 1;
  const std::string url = std::string(LANG_PACK_BASE_URL) + name;
  const std::string tmpPath = std::string(path) + ".tmp";

  Storage.mkdir("/lang");
  // No HTTP downgrade: nothing outside the pack itself vouches for its bytes.
  const auto result = HttpDownloader::downloadToFile(url, tmpPath, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("LANG", "Download of %s failed (%d, HTTP %d)", url.c_str(), static_cast<int>(result),
            HttpDownloader::lastStatus());
    Storage.remove(tmpPath.c_str());
    error_ = StrId::STR_DOWNLOAD_FAILED;
    return false;
  }
  // Only an intact pack replaces the card's copy.
  if (!UiLanguagePackStore::isValidPack(lang_, tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
    error_ = StrId::STR_DOWNLOAD_FAILED;
    return false;
  }
  // Keeps the old pack if the replace fails.
  if (!Storage.replaceFile(tmpPath.c_str(), path)) {
    LOG_ERR("LANG", "Replacing %s failed", path);
    Storage.remove(tmpPath.c_str());
    error_ = StrId::STR_DOWNLOAD_FAILED;
    return false;
  }
  LOG_INF("LANG", "Downloaded %s", path);
  return true;
}

void LanguagePackDownloadActivity::finishDownload() {
  if (!download()) {
    {
      // download() unloaded the SD fonts. Load only the UI family the error
      // screen draws with: a reader family load that fails would clear and save
      // the user's reader font, and the exit reboot loads it anyway.
      RenderLock lock(*this);
      sdFontSystem.applyUiLanguage(renderer);
      state_ = FAILED;
    }
    requestUpdate();
    return;
  }

  bool shown;
  {
    // Only the UI family is loaded: the exit reboots, and the boot loads the
    // reader family. With it unloaded, the reader family is also a candidate.
    RenderLock lock(*this);
    shown = sdFontSystem.switchUiLanguage(renderer, lang_);
    if (shown) state_ = COMPLETE;
  }
  if (shown) {
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  // No font on the card draws the language yet; Wi-Fi is already up for one.
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_UI_FONT_MISSING),
                                                                tr(STR_UI_FONT_MISSING_DOWNLOAD)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             finish();
                             return;
                           }
                           startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput, lang_),
                                                  [this](const ActivityResult&) { finish(); });
                         });
}

void LanguagePackDownloadActivity::loop() {
  if (state_ == DOWNLOADING) {
    // Show the downloading screen before blocking on the network.
    requestUpdateAndWait();
    finishDownload();
    return;
  }
  if (state_ == WIFI_SELECTION) return;

  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void LanguagePackDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int midY = renderer.getScreenHeight() / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LANGUAGE));

  switch (state_) {
    case WIFI_SELECTION:
    case DOWNLOADING:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_DOWNLOADING));
      break;
    case COMPLETE:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_DOWNLOAD_COMPLETE), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, I18N.getLanguageName(lang_));
      break;
    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_DOWNLOAD_FAILED), true, EpdFontFamily::BOLD);
      if (error_ != StrId::STR_DOWNLOAD_FAILED) renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, I18N.get(error_));
      break;
  }

  if (state_ == COMPLETE || state_ == FAILED) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

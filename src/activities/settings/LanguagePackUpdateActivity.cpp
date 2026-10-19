#include "LanguagePackUpdateActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>
#include <iterator>

#include "LanguagePackDownloadActivity.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "UiLanguagePackStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Whether the two files hold the same bytes.
bool sameContents(const char* a, const char* b) {
  HalFile fa;
  HalFile fb;
  if (!Storage.openFileForRead("LANG", a, fa) || !Storage.openFileForRead("LANG", b, fb)) return false;
  if (fa.fileSize() != fb.fileSize()) return false;
  uint8_t bufA[256];
  uint8_t bufB[256];
  while (true) {
    const int n = fa.read(bufA, sizeof(bufA));
    if (n <= 0) return n == 0;
    if (fb.read(bufB, n) != n || memcmp(bufA, bufB, n) != 0) return false;
  }
}

}  // namespace

void LanguagePackUpdateActivity::onEnter() {
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

void LanguagePackUpdateActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    // The boot copies a replaced pack of the shown language into flash.
    silentRestart();
  }
}

void LanguagePackUpdateActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }
  {
    RenderLock lock(*this);
    state_ = CHECKING;
  }
  requestUpdate();
}

bool LanguagePackUpdateActivity::updatePacks() {
  for (const uint8_t index : SORTED_LANGUAGE_INDICES) {
    const auto lang = static_cast<Language>(index);
    if (isLanguageBuiltIn(lang)) continue;
    char path[32];
    UiLanguagePackStore::packPath(lang, path, sizeof(path));
    if (!Storage.exists(path)) continue;

    const std::string tmpPath = std::string(path) + ".tmp";
    if (!LanguagePackDownloadActivity::fetchPack(lang, tmpPath.c_str(), error_)) return false;
    if (sameContents(tmpPath.c_str(), path)) {
      Storage.remove(tmpPath.c_str());
      continue;
    }
    // Keeps the old pack if the replace fails.
    if (!Storage.replaceFile(tmpPath.c_str(), path)) {
      LOG_ERR("LANG", "Replacing %s failed", path);
      Storage.remove(tmpPath.c_str());
      error_ = StrId::STR_UPDATE_FAILED;
      return false;
    }
    LOG_INF("LANG", "Updated %s", path);
    if (!updated_.empty()) updated_ += ", ";
    updated_ += I18N.getLanguageName(lang);
  }
  return true;
}

void LanguagePackUpdateActivity::loop() {
  if (state_ == CHECKING) {
    // Show the checking screen before blocking on the network.
    requestUpdateAndWait();
    {
      // TLS needs more heap than the loaded SD fonts leave on X3.
      RenderLock lock(*this);
      sdFontSystem.unloadAll(renderer);
    }
    const bool ok = updatePacks();
    {
      // Only the UI family the result screen draws with; the exit reboots.
      RenderLock lock(*this);
      sdFontSystem.applyUiLanguage(renderer);
      state_ = ok ? DONE : FAILED;
    }
    requestUpdate();
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

void LanguagePackUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int midY = renderer.getScreenHeight() / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UPDATE_LANGUAGE_FILES));

  switch (state_) {
    case WIFI_SELECTION:
    case CHECKING:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_CHECKING_UPDATE));
      break;
    case DONE:
      if (updated_.empty()) {
        renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_NO_UPDATE), true, EpdFontFamily::BOLD);
      } else {
        renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, updated_.c_str());
      }
      break;
    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
      if (error_ != StrId::STR_UPDATE_FAILED) renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, I18N.get(error_));
      break;
  }

  if (state_ == DONE || state_ == FAILED) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

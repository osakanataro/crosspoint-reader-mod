#include "FontFlashCacheActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <SdCardFontCache.h>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "components/FontFlashCacheView.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/InputDiag.h"

namespace {
constexpr unsigned long FAILURE_DWELL_MS = 3000;
}

void FontFlashCacheActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void FontFlashCacheActivity::loop() {
  switch (state_.load()) {
    case State::Idle:
      state_.store(State::Running);
      requestUpdateAndWait();
      run();
      break;
    case State::Failed:
      if (millis() - failedAtMs_ >= FAILURE_DWELL_MS) onGoHome(HomeMenuItem::NONE);
      break;
    default:
      break;
  }
}

void FontFlashCacheActivity::run() {
  const auto* file = sdFontSystem.cacheCandidate();
  familyName_ = SETTINGS.sdFontFamilyName;
  pointSize_ = file ? file->pointSize : 0;

  SdCardFontCache::Result result = SdCardFontCache::Result::TooLarge;
  if (file) {
    result = SdCardFontCache::preload(
        file->path.c_str(),
        [](size_t completed, size_t total, void* context) {
          auto* self = static_cast<FontFlashCacheActivity*>(context);
          self->completed_.store(completed);
          self->total_.store(total);
          const unsigned percent = total > 0 ? static_cast<unsigned>(completed * 100 / total) : 0;
          if (percent == 100 || percent >= self->lastPercent_ + 5) {
            self->lastPercent_ = percent;
            self->requestUpdate(true);
          }
        },
        this);
  }
  LOG_INF("SDFCACHE", "Boot rebuild for %s: %s", file ? file->path.c_str() : "(none)",
          SdCardFontCache::resultName(result));
  {
    char line[96];
    snprintf(line, sizeof(line), "boot copy result=%s bytes=%u", SdCardFontCache::resultName(result),
             static_cast<unsigned>(total_.load() / 2));
    InputDiag::noteFontCopy(line);
  }
  InputDiag::flushNow();

  if (result == SdCardFontCache::Result::Ok || result == SdCardFontCache::Result::AlreadyCached) {
    {
      RenderLock lock;
      sdFontSystem.ensureLoaded(renderer, true);
      state_.store(State::Ready);
    }
    requestUpdateAndWait();
    onGoHome(HomeMenuItem::NONE);
    return;
  }

  SETTINGS.sdFontFlashCache = 0;
  SETTINGS.saveToFile();
  {
    RenderLock lock;
    tooLarge_ = result == SdCardFontCache::Result::TooLarge;
    failedAtMs_ = millis();
    state_.store(State::Failed);
  }
  requestUpdate();
}

void FontFlashCacheActivity::render(RenderLock&&) {
  const State state = state_.load();
  if (state == State::Failed) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int pageWidth = renderer.getScreenWidth();
    const int pageHeight = renderer.getScreenHeight();
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.clearScreen();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   tr(STR_FONT_FLASH_CACHE_TITLE));
    const int top = (pageHeight - lineHeight) / 2;
    const Rect bounds{metrics.contentSidePadding, top, pageWidth - metrics.contentSidePadding * 2, pageHeight - top};
    UITheme::drawCenteredWrappedText(renderer, bounds, UI_10_FONT_ID,
                                     tooLarge_ ? tr(STR_FONT_FLASH_CACHE_TOO_LARGE) : tr(STR_FONT_FLASH_CACHE_FAILED),
                                     4, true, EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
  } else {
    fontflashcache::draw(renderer, familyName_.c_str(), pointSize_, completed_.load(), total_.load(),
                         state == State::Ready);
  }
  renderer.displayBuffer();
}

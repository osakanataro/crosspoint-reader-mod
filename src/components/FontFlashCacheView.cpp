#include "FontFlashCacheView.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

namespace fontflashcache {

void draw(const GfxRenderer& renderer, const char* familyName, const uint8_t pointSize, const size_t completed,
          const size_t total, const bool ready) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_FONT_FLASH_CACHE_TITLE));

  const int top = (pageHeight - lineHeight) / 2 - lineHeight;
  if (ready) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_FONT_FLASH_CACHE_READY), true, EpdFontFamily::BOLD);
    return;
  }

  char fontLine[48];
  snprintf(fontLine, sizeof(fontLine), "%s, %u pt", familyName, static_cast<unsigned>(pointSize));
  renderer.drawCenteredText(UI_10_FONT_ID, top, fontLine, true, EpdFontFamily::BOLD);

  // The first half of `total` is the copy, the second the read-back check.
  const size_t bounded = std::min(completed, total);
  const bool verifying = total > 1 && bounded > total / 2;
  int y = top + lineHeight + metrics.verticalSpacing;
  renderer.drawCenteredText(UI_10_FONT_ID, y,
                            verifying ? tr(STR_FONT_FLASH_CACHE_VERIFYING) : tr(STR_FONT_FLASH_CACHE_COPYING));
  y += lineHeight + metrics.verticalSpacing;
  GUI.drawProgressBar(
      renderer,
      Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
      bounded, std::max<size_t>(total, 1));
  y += metrics.progressBarHeight + metrics.verticalSpacing + lineHeight + metrics.verticalSpacing;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
}

}  // namespace fontflashcache

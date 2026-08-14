#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "OstVersion.h"
#include "fontIds.h"
#include "images/Logo120.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_BOOTING));
  // Both markers: the upstream release the tree is based on, and the build of this fork on the
  // device. The splash is the one screen every boot passes through, so it is where "which
  // firmware is this?" gets answered without digging through menus.
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 44, CROSSPOINT_VERSION);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 26, OST_VERSION_LABEL, true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
}

#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>
#include <memory>

#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

LanguageSelectActivity::LanguageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("LanguageSelect", renderer, mappedInput) {}

void LanguageSelectActivity::onEnter() {
  UiListActivity::onEnter();

  // Open on the current language, which may sit past the first page; the first
  // screen build pulls the viewport to it (ListNav follow-on-build).
  // The saved choice, which the UI may be showing in English for lack of a font.
  const auto currentLang = SETTINGS.language;
  const auto* begin = std::begin(SORTED_LANGUAGE_INDICES);
  const auto* end = std::end(SORTED_LANGUAGE_INDICES);
  const auto* it = std::find(begin, end, currentLang);
  nav.selected = (it != end) ? static_cast<int>(std::distance(begin, it)) : 0;

  // Built once here rather than every buildScreen() call: labels are static,
  // and the "Selected" marker can't go stale mid-visit since activateIndex()
  // finishes the activity immediately on selection.
  for (int i = 0; i < totalItems; ++i) {
    fui::ListItem item;
    item.label = I18N.getLanguageName(static_cast<Language>(SORTED_LANGUAGE_INDICES[i]));
    if (SORTED_LANGUAGE_INDICES[i] == currentLang) {
      item.value = tr(STR_SELECTED);
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems[i] = item;
  }
}

const char* LanguageSelectActivity::headerTitle() const { return tr(STR_LANGUAGE); }

void LanguageSelectActivity::activateIndex(const int index) {
  // The activated row leaves this screen; a lingering flash would gray an
  // unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  const uint8_t langIndex = SORTED_LANGUAGE_INDICES[index];

  const uint8_t previousLang = SETTINGS.language;
  SETTINGS.language = langIndex;
  {
    // May load or free SD fonts the render task draws with.
    RenderLock lock(*this);
    sdFontSystem.applyUiLanguage(renderer);
    if (!sdFontSystem.uiLanguageUnavailable()) sdFontSystem.adoptUiFamilyForReader(renderer);
  }

  if (!sdFontSystem.uiLanguageUnavailable()) {
    SETTINGS.saveToFile();
    finish();
    return;
  }
  // No font on the card draws the chosen language. Offer the font download;
  // the choice is kept only if a font for it arrives.
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_UI_FONT_MISSING),
                                                                tr(STR_UI_FONT_MISSING_DOWNLOAD)),
                         [this, previousLang](const ActivityResult& result) {
                           if (result.isCancelled) {
                             keepLanguageOrRevert(previousLang);
                             return;
                           }
                           startActivityForResult(
                               std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this, previousLang](const ActivityResult&) { keepLanguageOrRevert(previousLang); });
                         });
}

void LanguageSelectActivity::keepLanguageOrRevert(const uint8_t previousLang) {
  {
    // A downloaded family may draw the chosen language; reverting may free one.
    RenderLock lock(*this);
    sdFontSystem.applyUiLanguage(renderer);
    if (sdFontSystem.uiLanguageUnavailable()) {
      SETTINGS.language = previousLang;
      sdFontSystem.applyUiLanguage(renderer);
      // The previous language may have lost its font too (it was removed from the card).
      if (sdFontSystem.uiLanguageUnavailable()) {
        SETTINGS.language = static_cast<uint8_t>(Language::EN);
        sdFontSystem.applyUiLanguage(renderer);
      }
    } else {
      sdFontSystem.adoptUiFamilyForReader(renderer);
    }
  }
  SETTINGS.saveToFile();
  finish();
}

void LanguageSelectActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems was built once in onEnter() and is reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(totalItems);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

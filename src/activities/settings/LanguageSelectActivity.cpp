#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>
#include <memory>

#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "I18nKeys.h"
#include "LanguagePackDownloadActivity.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "UiLanguagePackStore.h"
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

  // Languages that need a language file or a font from the network are marked.
  Language langs[totalItems];
  bool drawable[totalItems];
  for (int i = 0; i < totalItems; ++i) langs[i] = static_cast<Language>(SORTED_LANGUAGE_INDICES[i]);
  {
    RenderLock lock(*this);
    sdFontSystem.findDrawableLanguages(renderer, langs, totalItems, drawable);
  }

  // Built once here rather than every buildScreen() call: labels are static,
  // and the "Selected" marker can't go stale mid-visit since activateIndex()
  // finishes the activity immediately on selection.
  for (int i = 0; i < totalItems; ++i) {
    fui::ListItem item;
    item.label = I18N.getLanguageName(langs[i]);
    item.actionValue = static_cast<int16_t>(i);
    rowItems[i] = item;
    if (SORTED_LANGUAGE_INDICES[i] == currentLang) {
      rowMarks[i] = RowMark::Selected;
    } else if (!drawable[i] || !UiLanguagePackStore::available(langs[i])) {
      rowMarks[i] = RowMark::NeedsDownload;
    } else {
      rowMarks[i] = RowMark::None;
    }
  }
}

const char* LanguageSelectActivity::headerTitle() const { return tr(STR_LANGUAGE); }

void LanguageSelectActivity::activateIndex(const int index) {
  // The activated row leaves this screen; a lingering flash would gray an
  // unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  const uint8_t langIndex = SORTED_LANGUAGE_INDICES[index];

  const auto chosen = static_cast<Language>(langIndex);
  if (!UiLanguagePackStore::loadable(chosen)) {
    // The language ships as an SD pack (/lang) and the card has none. The
    // download activity switches to the language itself and reboots when it
    // exits, so this screen does not continue.
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_UI_LANGUAGE_PACK_MISSING),
                                               tr(STR_UI_LANGUAGE_PACK_MISSING_BODY)),
        [this, chosen](const ActivityResult& result) {
          if (result.isCancelled) return;
          startActivityForResult(std::make_unique<LanguagePackDownloadActivity>(renderer, mappedInput, chosen),
                                 [this](const ActivityResult&) { finish(); });
        });
    return;
  }
  const bool changed = langIndex != SETTINGS.language;
  bool shown;
  {
    // May load or free SD fonts the render task draws with.
    RenderLock lock(*this);
    shown = sdFontSystem.switchUiLanguage(renderer, chosen);
  }

  if (shown) {
    SETTINGS.saveToFile();
    if (changed) restartOnFreshHeap();
    finish();
    return;
  }
  // No font on the card draws the chosen language. Offer the font download;
  // SETTINGS keeps the previous language until a font for this one arrives.
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_UI_FONT_MISSING),
                                                                tr(STR_UI_FONT_MISSING_DOWNLOAD)),
                         [this, chosen](const ActivityResult& result) {
                           if (result.isCancelled) {
                             keepLanguageOrRevert(chosen);
                             return;
                           }
                           startActivityForResult(
                               std::make_unique<FontDownloadActivity>(renderer, mappedInput, chosen),
                               [this, chosen](const ActivityResult&) { keepLanguageOrRevert(chosen); });
                         });
}

void LanguageSelectActivity::keepLanguageOrRevert(const Language chosen) {
  const uint8_t saved = SETTINGS.language;
  {
    // A downloaded family may draw the chosen language; reverting may free one.
    RenderLock lock(*this);
    const auto previous = static_cast<Language>(SETTINGS.language);
    if (!sdFontSystem.switchUiLanguage(renderer, chosen) && !sdFontSystem.applyUiLanguage(renderer, previous)) {
      // The previous language may have lost its font too (it was removed from the card).
      SETTINGS.language = static_cast<uint8_t>(Language::EN);
      sdFontSystem.applyUiLanguage(renderer, Language::EN);
    }
  }
  SETTINGS.saveToFile();
  if (SETTINGS.language != saved) restartOnFreshHeap();
  finish();
}

void LanguageSelectActivity::restartOnFreshHeap() {
  // Loading and freeing SD fonts for each language leaves the heap in pieces
  // too small for a later Wi-Fi + TLS download on X3, and the pack switch
  // rewrote the strings earlier screens may still point to. Reopen Settings
  // after a reboot instead.
  RenderLock lock(*this);
  silentRestartToSettings();
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
  // Marks are looked up on each repaint: switching to a pack language rewrites
  // the flash copy that earlier tr() pointers point into.
  for (int i = 0; i < totalItems; ++i) {
    rowItems[i].value = rowMarks[i] == RowMark::Selected        ? tr(STR_SELECTED)
                        : rowMarks[i] == RowMark::NeedsDownload ? tr(STR_LANGUAGE_NEEDS_DOWNLOAD)
                                                                : nullptr;
  }
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

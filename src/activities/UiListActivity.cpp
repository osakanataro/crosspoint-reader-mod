#include "UiListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/InputDiag.h"

namespace fui = freeink::ui;

UiListActivity::UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const bool wantsTouchLongPress)
    : Activity(name, renderer, mappedInput), UiAppHost(renderer), wantsTouchLongPress(wantsTouchLongPress) {}

void UiListActivity::onEnter() {
  Activity::onEnter();
  // Hand back every SD-card glyph cache before the first build, the reading font's included: a list
  // opened over the reader has to warm a screenful of CJK out of whatever the book left behind, and
  // what it left behind is not drawn while this screen is up. The prewarm's own record follows the
  // release through SdCardFont's generation counter, so nothing is left believing it is still warm.
  renderer.releaseAllSdGlyphCaches();
  activeNav().reset();
  resetUi();
  app.on(ACTION_ROW, &UiListActivity::rowActionTrampoline, this);
  app.setScreen(&UiListActivity::screenTrampoline, this);
  requestUpdate();
}

void UiListActivity::onExit() {
  // A screenful of CJK glyph bitmaps is tens of KB the next allocation does not
  // get, and picking a row is usually what starts a section build -- which needs
  // one contiguous block for the ZIP inflate.
  UiGlyphPrewarm::release(renderer);
  Activity::onExit();
}

void UiListActivity::prewarmFrame(UiGlyphPrewarm& warm) {
  warm.add(UiGlyphPrewarm::Role::Header, headerTitle());
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  warm.add(UiGlyphPrewarm::Role::ThemeBody, labels.btn1);
  warm.add(UiGlyphPrewarm::Role::ThemeBody, labels.btn2);
  warm.add(UiGlyphPrewarm::Role::ThemeBody, labels.btn3);
  warm.add(UiGlyphPrewarm::Role::ThemeBody, labels.btn4);
}

void UiListActivity::applyFramePrewarm() {
  UiGlyphPrewarm warm;
  prewarmFrame(warm);
  warm.apply(renderer);
}

void UiListActivity::addVisibleRows(UiGlyphPrewarm& warm, const fui::ListItem* items, const int count,
                                    const UiGlyphPrewarm::Role labelRole) {
  if (items == nullptr || count <= 0) return;

  const auto& n = activeNav();
  int rows = n.visibleRows;
  if (rows <= 1) {
    // The band is measured during the build (ListNav::syncToProps), so the
    // first paint of a screen -- the one paint whose glyphs are all cold -- has
    // only the reset value here. Estimate high from the screen instead: a row
    // warmed that turns out to be off-screen costs a few glyph reads, while
    // warming one row of a full screen leaves the rest on the on-demand path.
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int rowHeight = metrics.listRowHeight > 0 ? metrics.listRowHeight : 1;
    const int band = renderer.getScreenHeight() - metrics.topPadding - metrics.headerHeight -
                     metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
    // Bands a screen takes for itself (tab bar, path line, progress line) are
    // not subtracted, so this reads high -- the direction that costs reads
    // rather than leaving rows cold.
    rows = std::max(rows, band / rowHeight);
  }

  const int first = std::clamp(n.top, 0, count);
  const int last = std::min(count, first + rows);
  for (int i = first; i < last; i++) {
    warm.add(labelRole, items[i].label);
    warm.add(UiGlyphPrewarm::Role::ListSmall, items[i].subtitle);
    warm.add(UiGlyphPrewarm::Role::ListSmall, items[i].value);
  }
}

void UiListActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<UiListActivity*>(user)->buildScreen(screen);
}

void UiListActivity::rowActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  if (event.value < 0 || event.value >= self->listCount()) return;
  self->onRowAction(event);
}

void UiListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value;
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  activateIndex(event.value);
}

bool UiListActivity::handleButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const int selected = activeNav().selected;
    if (selected >= 0 && selected < listCount()) activateIndex(selected);
    return true;
  }
  return false;
}

bool UiListActivity::routeListTouch() {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let the action trampoline dispatch.
  const auto route = UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
  // No pressed-state repaint: the render it triggers would drop a slow tap's
  // release inside the uiReady window (tap-to-activate needed two taps), and
  // it costs a second e-ink refresh per tap.
  if (route.routed && app.invalidated()) requestUpdate();
  return static_cast<bool>(route);  // dispatched to the action handler
}

void UiListActivity::moveSelectionTo(const int index) {
  auto& n = activeNav();
  n.selected = index;
  n.follow(listCount());
  requestUpdate();
}

void UiListActivity::loop() {
  if (handleCustomInput()) return;
  if (handleButtons()) return;
  if (routeListTouch()) return;

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    auto& n = activeNav();
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? n.visibleRows : -n.visibleRows;
    if (n.scrollBy(delta, listCount())) requestUpdate();
    return;
  }

  navigateButtons();
}

void UiListActivity::navigateButtons() {
  const int count = listCount();
  auto& n = activeNav();
  buttonNavigator.onNextRelease([this, count, &n] { moveSelectionTo(ButtonNavigator::nextIndex(n.selected, count)); });
  buttonNavigator.onPreviousRelease(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::previousIndex(n.selected, count)); });
  buttonNavigator.onNextContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::nextPageIndex(n.selected, count, n.visibleRows)); });
  buttonNavigator.onPreviousContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::previousPageIndex(n.selected, count, n.visibleRows)); });
}

void UiListActivity::syncListViewport(UiScreen& screen, fui::ListProps& props, const bool hasSubtitle) {
  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    // Non-touch hardware (X3/X4) keeps the original, denser per-theme row
    // height instead of FreeInkUI's touch-target-sized default, so lists fit
    // as many rows per screen as they did before the FreeInkUI migration.
    // props.rowHeight must be set explicitly: screen.list() otherwise falls
    // back to the (touch-friendly) theme token, not this local value.
    const auto& metrics = UITheme::getInstance().getMetrics();
    rowHeight = static_cast<int16_t>(hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight);
    props.rowHeight = rowHeight;
  }
  const fui::Rect band = screen.body();
  activeNav().syncToProps(band, rowHeight, screen.theme().listRowGap, listCount(), props);
  // No-op unless built with INPUT_DIAG. The same arithmetic decides what the list draws and what
  // the selection treats as on screen, so a band taller than the panel shows up as rows that can
  // be selected but never seen.
  InputDiag::noteListBand(band.y, band.height, rowHeight, activeNav().visibleRows, renderer.getScreenHeight());
}

void UiListActivity::drawChrome() {
  const char* title = headerTitle();
  if (!title) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
}

void UiListActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void UiListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  applyFramePrewarm();
  drawChrome();
  renderUi();
  drawFooter();
  renderer.displayBuffer();
}

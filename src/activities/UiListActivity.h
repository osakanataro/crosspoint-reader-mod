#pragma once

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"
#include "util/UiGlyphPrewarm.h"

// Base for activities hosting a single FreeInkUI list screen. UiAppHost owns
// the app-hosting protocol (render target, FreeInkApp, uiReady handshake);
// this base layers the list protocol on top: the touch-routing / swipe-scroll
// / button-navigation loop (swipes scroll the viewport without moving the
// selection; buttons move the selection and pull the viewport along via
// fui::ListNav), and the render skeleton (chrome, app, footer). Subclasses
// supply the data: item count, screen content, and what activating a row does.
//
// Screens that are not a single list (sliders, tab layouts, state machines)
// should NOT derive from this — they use UiAppHost directly.
class UiListActivity : public Activity, protected UiAppHost {
 public:
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 protected:
  // Base-owned row action; subclass-registered actions start at ACTION_USER.
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_USER = 2;

  UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                 bool wantsTouchLongPress = false);

  // --- subclass contract -----------------------------------------------------
  // Current number of list rows (re-read every loop pass; may change).
  virtual int listCount() const = 0;
  // Build the screen: content margin, items, ListProps (call syncListViewport
  // right before screen.list). Runs on the render task via the base trampoline.
  virtual void buildScreen(UiScreen& screen) = 0;
  // Activate a row (touch tap or Confirm on the selection). Handlers that
  // leave this screen should call app.clearTapFlash() so a lingering flash
  // can't gray an unrelated element on the next render.
  virtual void activateIndex(int index) = 0;
  // Touch long-press on a row; only fires when the subclass opted in via the
  // wantsTouchLongPress constructor flag (rows must also carry InputLongPress).
  virtual void onRowLongPress(int index) {}
  // The selection/viewport state the loop, sync, and row dispatch operate on.
  // Default is the single `nav` member; UiTabListActivity redirects it to the
  // active tab's per-tab state.
  virtual freeink::ui::ListNav& activeNav() { return nav; }
  // Bounds-checked ACTION_ROW dispatch. Default: selection follows the tapped
  // row, then long-press/activate. UiTabListActivity remaps row -> ring.
  virtual void onRowAction(const freeink::ui::ActionEvent& event);
  // The button-navigation tail of loop(): release steps the selection, hold
  // jumps by page. UiTabListActivity replaces it with the ring walk.
  virtual void navigateButtons();
  // First hook in loop(); return true when the pass is consumed (popups, extra
  // buttons, gestures). Runs before the base button handling.
  virtual bool handleCustomInput() { return false; }
  // Back/Confirm handling; override wholesale for press/release or hold
  // variants. Return true when a button consumed the pass.
  virtual bool handleButtons();
  virtual void onBackButton() { finish(); }
  // Header band, drawn before the app renders. Default paints GUI.drawHeader
  // with headerTitle(); override either for custom chrome.
  virtual const char* headerTitle() const { return nullptr; }
  virtual void drawChrome();
  // Button hints, drawn after the app renders. Default: Back/Select/Up/Down.
  virtual void drawFooter();

  // --- glyph prewarm ---------------------------------------------------------
  // Add every string this frame is about to draw (see UiGlyphPrewarm). One pass
  // has to cover the whole frame: a prewarm rebuilds that font's cache, so
  // warming the button hints after the rows would drop the rows again, and the
  // rows are the expensive half.
  //
  // The base adds headerTitle() and the default hints. A subclass adds the rows
  // in view (addVisibleRows) plus whatever a drawChrome/drawFooter override
  // paints in place of those defaults, and calls this first.
  virtual void prewarmFrame(UiGlyphPrewarm& warm);

  // Collect the frame's text and load its glyphs. Call from render() before the
  // first draw; a render() override has to call it itself.
  void applyFramePrewarm();

  // Add rows [top, top + visible) of `items` for prewarmFrame. labelRole must
  // match what buildScreen puts in props.labelText: screens that set it to
  // theme().smallText (file names, book titles) pass ListSmall or
  // ListSmallBold, and the ListRow default covers the ones that leave it to the
  // theme. Getting it wrong only leaves that text on the on-demand path.
  void addVisibleRows(UiGlyphPrewarm& warm, const freeink::ui::ListItem* items, int count,
                      UiGlyphPrewarm::Role labelRole = UiGlyphPrewarm::Role::ListRow);

  // --- helpers ---------------------------------------------------------------
  // Measure visibleRows for the screen band, apply follow-on-build, clamp the
  // viewport, and write selection/viewport into props. Call from buildScreen
  // right before screen.list(props).
  // hasSubtitle: rows carry a second (subtitle) text line, so on non-touch
  // hardware the denser override below uses the theme's *-with-subtitle row
  // height instead of its single-line one (see syncListViewport()).
  void syncListViewport(UiScreen& screen, freeink::ui::ListProps& props, bool hasSubtitle = false);
  // Move the selection to index and pull the viewport to it.
  void moveSelectionTo(int index);

  // Page the viewport rather than scrolling it a row at a time, for the row the
  // selection sits on. Call from the screen build, after the viewport sync has
  // measured visibleRows.
  //
  // FreeInkUI pulls the viewport the minimal amount to keep the selection in
  // view, so past the last visible row every button press shifts the window by
  // one and the whole screenful of text changes. Each of those is a fresh glyph
  // prewarm: measured on an X3, a file list costs 518 ms standing still and
  // 850-890 ms per step scrolling that way. Paging pays that once per screenful.
  // It is also what these lists did before the FreeInkUI conversion, and an
  // e-ink panel repaints in full either way, so moving a single row buys nothing.
  //
  // Touch boards keep the smooth behaviour: a swipe there is a scroll, not a
  // page turn, and they are not the ones reading glyphs off a card.
  void snapViewportToPage(int rowIndex);

  // --- shared state ----------------------------------------------------------
  // Selection + viewport (selected/top/visibleRows/followOnBuild). Access via
  // activeNav() in shared code; `nav` is the single-list default storage.
  freeink::ui::ListNav nav;
  ButtonNavigator buttonNavigator;

 private:
  static void screenTrampoline(UiScreen& screen, void* user);
  static void rowActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  // Named apart from UiAppHost::routeTouch so the host overload stays visible
  // (not name-hidden) to subclasses with extra touch surfaces.
  bool routeListTouch();

  const bool wantsTouchLongPress;
};

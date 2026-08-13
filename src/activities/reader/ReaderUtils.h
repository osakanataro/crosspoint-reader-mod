#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <components/bars/tap-zones.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long GO_BACK_OR_HOME_MS = GO_HOME_MS;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;

enum ReaderTouchAction : freeink::ui::ActionId {
  READER_TOUCH_PREV = 1,
  READER_TOUCH_NEXT = 3,
};

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

// `rtlPageProgression` is the book's spine page-progression-direction. In such a book the
// pages advance the way the text runs -- right to left -- so the controls that carry a
// left-right sense have to follow, or every turn is backwards from what the page shows.
// A vertical Japanese book is the common case; a horizontal Arabic or Hebrew one behaves
// the same way.
//
// The two axis-carrying inputs are treated differently on purpose:
//
//   Front Left/Right compose with the orientation flip. Both are transforms of the same
//   axis -- one because the panel is rotated, one because the text runs the other way --
//   so applying both is correct: in an inverted vertical book they cancel out.
//
//   The side buttons override sideButtonLayout instead of composing. That setting is how
//   the reader states which side-button end means "forward" for ordinary books, and letting
//   it compose would mean a reader who had already flipped it by hand to cope with vertical
//   text now gets the old, wrong behaviour back. SIDE_BUTTONS_DISABLED still wins, since
//   that is not a direction but an off switch.
inline PageTurnResult detectPageTurn(const MappedInputManager& input, const bool rtlPageProgression = false) {
  using Button = MappedInputManager::Button;
  const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();

  const bool swapFront = input.isNavDirectionSwapped() != rtlPageProgression;
  const auto prevButton = swapFront ? Button::Right : Button::Left;
  const auto nextButton = swapFront ? Button::Left : Button::Right;

  // Physical Up/Down when overriding, so sideButtonLayout's direction is bypassed;
  // PageBack/PageForward otherwise, which is where that setting is applied.
  const bool sideOverride =
      rtlPageProgression && SETTINGS.sideButtonLayout != CrossPointSettings::SIDE_BUTTONS_DISABLED;
  const auto sidePrevButton = sideOverride ? Button::Down : Button::PageBack;
  const auto sideNextButton = sideOverride ? Button::Up : Button::PageForward;

  const auto triggered = [&](const Button button) {
    return usePress ? input.wasPressed(button) : input.wasReleased(button);
  };

  const bool prev = tiltPrev || triggered(sidePrevButton) || triggered(prevButton);
  const bool powerTurn =
      SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && input.wasReleased(Button::Power);
  const bool next = tiltNext || powerTurn || triggered(sideNextButton) || triggered(nextButton);
  return {prev, next, tiltPrev || tiltNext};
}

struct TouchPageTurn {
  bool prev;
  bool next;
  unsigned long heldMs;
};

// The tap zones carry a left-right sense too, so they follow the page-progression
// direction along with the front buttons (see detectPageTurn).
inline TouchPageTurn detectTouchPageTurn(GfxRenderer& renderer, const MappedInputManager& input,
                                         const bool rtlPageProgression = false) {
  TouchPageTurn result{false, false, 0};
  if (!SETTINGS.touchReaderControls || !input.hasTouch()) {
    return result;
  }

  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) {
    return result;
  }

  const int16_t width = static_cast<int16_t>(renderer.getScreenWidth());
  const int16_t height = static_cast<int16_t>(renderer.getScreenHeight());
  // Outer thirds only: the middle third is the reader-menu tap
  // (isTouchMenuTap below), so it must not double as a page turn. The two ends
  // trade places in an RTL book, so the forward end stays on the side the text
  // advances towards.
  const int16_t zoneWidth = width / 3;
  const auto leadingAction = rtlPageProgression ? READER_TOUCH_NEXT : READER_TOUCH_PREV;
  const auto trailingAction = rtlPageProgression ? READER_TOUCH_PREV : READER_TOUCH_NEXT;
  const freeink::ui::TapZone zones[] = {
      {freeink::ui::Rect{0, 0, zoneWidth, height}, leadingAction},
      {freeink::ui::Rect{static_cast<int16_t>(width - zoneWidth), 0, zoneWidth, height}, trailingAction},
  };

  for (const auto& zone : zones) {
    if (!zone.enabled || !zone.rect.contains(static_cast<int16_t>(x), static_cast<int16_t>(y))) continue;
    result.prev = zone.action == READER_TOUCH_PREV;
    result.next = zone.action == READER_TOUCH_NEXT;
    break;
  }
  result.heldMs = gpio.lastTouchHeldMs();
  return result;
}

// Tap in the middle third of the screen: the tap path into the reader menu on
// every touch board. The page-turn tap zones are the outer thirds, so the
// middle is free in tap mode.
inline bool isTouchMenuTap(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!input.hasTouch()) return false;
  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) return false;
  const int width = renderer.getScreenWidth();
  // Same boundary math as detectTouchPageTurn's outer zones, so the middle
  // band meets them with no dead column when width % 3 != 0.
  const int zoneWidth = width / 3;
  return x >= zoneWidth && x < width - zoneWidth;
}

// Reader menu opens on the menu edge-swipe or a middle-third tap. On home-key
// boards a long press of the capacitive key runs the user-selected long-press
// function instead (SETTINGS.longPressMenuFunction), not the menu.
// With touch reader controls Off the reading surface ignores touch entirely,
// menu included, so a stray brush of the screen can't open it; the menu stays
// reachable via the Confirm button.
inline bool isTouchMenuGesture(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!SETTINGS.touchReaderControls) return false;
  return (input.hasTouch() && input.wasMenuGesture()) || isTouchMenuTap(renderer, input);
}

// One helper, blocking or deferred: the async form starts the refresh and
// returns so the caller can overlap CPU work with the panel's refresh time.
// Async callers must not touch the framebuffer until
// renderer.waitRefreshComplete() and must rebuild the differential baseline
// before the next page turn (the tiled grayscale cleanup does).
inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, bool async = false) {
  const auto mode = (pagesUntilFullRefresh <= 1) ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  if (async) {
    renderer.displayBufferAsync(mode);
  } else {
    renderer.displayBuffer(mode);
  }
  if (pagesUntilFullRefresh <= 1) {
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

struct BackNavCallback {
  void* ctx;
  void (*fn)(void*);
};

// Returns true if the back button was consumed (caller should return).
// Long press (>= GO_BACK_OR_HOME_MS):
// - default: go to file browser
// - with backShortToFileBrowser: go home
// Short press (< GO_BACK_OR_HOME_MS):
// - default: go home
// - with backShortToFileBrowser: go to file browser.
inline bool handleBackNavigation(const MappedInputManager& mappedInput, ActivityManager& activityManager,
                                 const char* filePath, BackNavCallback goHome) {
  // The reading surface deliberately has no swipe-to-exit path on any touch
  // board: the bottom-edge up-swipe already exits, and in swipe page-turn
  // mode a right swipe must page back instead. Back swipes stay available in menus and other activities; only
  // this reader-surface handler ignores them. Physical Back buttons are
  // unaffected: isPressed() is button-only, and this guard skips just the
  // gesture's own release frame.
  if (mappedInput.wasBackGesture()) {
    return false;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_BACK_OR_HOME_MS) {
    if (SETTINGS.backShortToFileBrowser) {
      goHome.fn(goHome.ctx);
    } else {
      activityManager.goToFileBrowser(filePath);
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < GO_BACK_OR_HOME_MS) {
    if (SETTINGS.backShortToFileBrowser) {
      activityManager.goToFileBrowser(filePath);
    } else {
      goHome.fn(goHome.ctx);
    }
    return true;
  }
  return false;
}

}  // namespace ReaderUtils

#pragma once

#include <GfxRenderer.h>
#include <Rtc.h>

// Clock mode's face: "YYYY/MM/DD" over "HH:MM", drawn from the 1bpp bitmaps in
// src/images/ClockDigits.h.
//
// Deliberately free of everything the rest of the UI needs. It takes no
// activity, no theme, no font and no settings, because the path that calls it
// most often is a timer wake that has not mounted the SD card and never will --
// see the clock-mode branch in setup(). Fonts would break that: an SD font is
// on the card, and the built-in ones stop at 18 pt.
namespace ClockFace {

// Draws the face and pushes it to the panel. Sets the orientation itself: the
// bitmaps carry their own rotation (drawImage rotates the origin but not the
// bits), so they are laid out in the panel's native orientation and the caller
// does not get to choose.
//
// `now` is null when the RTC could not be read; the face is then blank rather
// than showing a time that is not the time.
//
// `mode` is the caller's, because the right waveform depends on how often this
// runs rather than on what it draws. See ClockActivity for the policy.
//
// `twelveHour` mirrors the status bar's Clock Format setting: false keeps the
// 24-hour "HH:MM", true switches to "H:MM" with an AM/PM marker beside it.
// Passed as a value so this stays free of CrossPointSettings (see above).
void render(const GfxRenderer& renderer, const Rtc::DateTime* now, HalDisplay::RefreshMode mode, bool twelveHour);

}  // namespace ClockFace

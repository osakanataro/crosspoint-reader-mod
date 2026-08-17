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
// `now` is null when the RTC could not be read; the date and time are then left
// off and only the wake count is drawn. Painting anyway is deliberate: a face
// that never changes cannot say whether the update timer fired, and the count
// is the only thing that separates "never woke" from "woke and had no time to
// show".
//
// wakeCount is drawn small in the corner. See ClockMode::wakeCount().
void render(const GfxRenderer& renderer, const Rtc::DateTime* now, uint32_t wakeCount);

}  // namespace ClockFace

#pragma once

// Serial-free input timing diagnostics.
//
// The USB-write-locked X3 units have no usable serial console, so the numbers that decide whether a
// button press can be seen at all have nowhere to go. This writes them to the SD card instead, which
// is already the only channel in and out of those devices.
//
// The quantity that matters is the interval between consecutive gpio.update() calls. InputManager
// commits a state change only once two consecutive samples agree (DEBOUNCE_DELAY = 5 ms), so a press
// shorter than that interval can land in a single sample and never produce an edge at all. At
// LOW_POWER_FREQ the idle loop delay is 50 ms and the loop body itself runs 16x slower, which is how
// menu presses went missing.
//
// Compiled out unless INPUT_DIAG is defined. Put it in platformio.local.ini (gitignored), never in a
// release build: the calls below become empty inlines the optimizer removes, so the shipped firmware
// pays nothing.

#ifdef INPUT_DIAG
class InputDiag {
 public:
  // Once per main-loop iteration, immediately after gpio.update().
  static void sample(unsigned long nowMs, bool committedEdge, bool debouncePending);

  // Rewrites the snapshot on the SD card, at most every few seconds.
  static void flush(bool inputActive);
};
#else
class InputDiag {
 public:
  static void sample(unsigned long, bool, bool) {}
  static void flush(bool) {}
};
#endif

#pragma once

// Diagnostic build only (-DUILANG_TIMING=1): timings for the UI language
// packs, appended to /uilang-timing.txt on the SD card.
#ifdef UILANG_TIMING
namespace UiLangTiming {
// Any task. Buffered in RAM until flush().
void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Main task.
void flush();
// Main task: time string lookups for the shown language and built-in English.
void benchmarkStrings();
}  // namespace UiLangTiming
#define UILANG_TIMING_NOTE(...) UiLangTiming::note(__VA_ARGS__)
#else
#define UILANG_TIMING_NOTE(...) \
  do {                          \
  } while (0)
#endif

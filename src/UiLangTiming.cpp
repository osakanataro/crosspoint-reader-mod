#include "UiLangTiming.h"

#ifdef UILANG_TIMING

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace UiLangTiming {
namespace {

constexpr size_t BUFFER_SIZE = 2048;
char pending[BUFFER_SIZE];
size_t pendingLen = 0;
char writing[BUFFER_SIZE];
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

const char* languageCode() { return I18N.getLanguageName(I18N.getLanguage()); }

}  // namespace

void note(const char* fmt, ...) {
  char line[160];
  int n = snprintf(line, sizeof(line), "%8lu ", millis());
  va_list args;
  va_start(args, fmt);
  n += vsnprintf(line + n, sizeof(line) - n - 1, fmt, args);
  va_end(args);
  if (n > static_cast<int>(sizeof(line) - 2)) n = sizeof(line) - 2;
  line[n++] = '\n';
  taskENTER_CRITICAL(&lock);
  if (pendingLen + n <= BUFFER_SIZE) {
    memcpy(pending + pendingLen, line, n);
    pendingLen += n;
  }
  taskEXIT_CRITICAL(&lock);
}

void flush() {
  taskENTER_CRITICAL(&lock);
  const size_t n = pendingLen;
  memcpy(writing, pending, n);
  pendingLen = 0;
  taskEXIT_CRITICAL(&lock);
  if (n == 0) return;
  HalFile file = Storage.open("/uilang-timing.txt", O_WRONLY | O_CREAT | O_APPEND);
  if (file) {
    file.write(writing, n);
    file.flush();
  }
}

void benchmarkStrings() {
  constexpr size_t COUNT = static_cast<size_t>(StrId::_COUNT);
  constexpr int REPEATS = 1000;
  const LangStrings en = getLanguageStrings(Language::EN);
  uint32_t sum = 0;

  // First pass after boot: the flash cache may not hold the strings yet.
  int64_t t = esp_timer_get_time();
  for (size_t i = 0; i < COUNT; ++i) sum += static_cast<uint8_t>(I18N.get(static_cast<StrId>(i))[0]);
  const int64_t firstPass = esp_timer_get_time() - t;

  t = esp_timer_get_time();
  for (int r = 0; r < REPEATS; ++r) {
    for (size_t i = 0; i < COUNT; ++i) sum += static_cast<uint8_t>(I18N.get(static_cast<StrId>(i))[0]);
  }
  const int64_t shown = esp_timer_get_time() - t;

  // The same lookup, against the built-in English table.
  t = esp_timer_get_time();
  for (int r = 0; r < REPEATS; ++r) {
    for (size_t i = 0; i < COUNT; ++i) sum += static_cast<uint8_t>(en.data[en.offsets[i]]);
  }
  const int64_t english = esp_timer_get_time() - t;

  // Read every byte of every string (strlen) in the shown language.
  t = esp_timer_get_time();
  size_t bytes = 0;
  for (int r = 0; r < 100; ++r) {
    for (size_t i = 0; i < COUNT; ++i) bytes += strlen(I18N.get(static_cast<StrId>(i)));
  }
  const int64_t readAll = esp_timer_get_time() - t;

  note("bench lang=%s keys=%u first_pass_us=%lld", languageCode(), static_cast<unsigned>(COUNT), firstPass);
  note("bench lang=%s get_ns=%lld (x%d) builtin_en_lookup_ns=%lld", languageCode(),
       shown * 1000 / (static_cast<int64_t>(REPEATS) * COUNT), REPEATS,
       english * 1000 / (static_cast<int64_t>(REPEATS) * COUNT));
  note("bench lang=%s strlen_all_us=%lld bytes=%u (x100) sum=%u", languageCode(), readAll / 100,
       static_cast<unsigned>(bytes / 100), static_cast<unsigned>(sum & 0xFF));
}

}  // namespace UiLangTiming

#endif  // UILANG_TIMING

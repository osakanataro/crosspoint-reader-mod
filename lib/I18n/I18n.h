#pragma once

#include <cstdint>

#include "I18nKeys.h"
/**
 * Internationalization (i18n) system for CrossPoint Reader
 */

class I18n {
 public:
  static I18n& getInstance();

  // Disable copy
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Get localized string by ID
  const char* get(StrId id) const;

  const char* operator[](StrId id) const { return get(id); }

  Language getLanguage() const { return _language; }
  // Shows `lang`, or English when its strings can't be found (returns false).
  bool setLanguage(Language lang);

  // Supplies the strings of a language that is not built in, laid out like a
  // built-in one. The returned memory must stay valid until the next call.
  using PackLoader = bool (*)(Language lang, LangStrings& out);
  void setPackLoader(PackLoader loader) { _packLoader = loader; }
  const char* getLanguageName(Language lang) const;
  static Language languageFromCode(const char* code);

  // Get all unique characters used in a specific language
  // Returns a sorted string of unique characters
  static const char* getCharacterSet(Language lang);

  // Non-ASCII letters (UTF-8, sorted) used by a language's strings: what a UI
  // font must carry to show that language
  static const char* getUiLetters(Language lang);

 private:
  I18n();

  Language _language;
  LangStrings _strings;
  PackLoader _packLoader = nullptr;
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()

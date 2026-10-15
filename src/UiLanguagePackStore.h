#pragma once

#include <I18nKeys.h>

// UI languages that are not built in are read from SD packs
// (/lang/<code>.cplang, written by gen_i18n.py). The chosen one is copied to
// internal flash and read from there like a built-in language, so the card can
// be removed and string lookups cost the same as before.
namespace UiLanguagePackStore {

// I18n::PackLoader: copies the pack to flash when the copy there is missing,
// stale or for another language. Main task only.
bool load(Language lang, LangStrings& out);

// Whether `lang` can be shown: built in, on the card, or already in flash.
bool available(Language lang);

}  // namespace UiLanguagePackStore

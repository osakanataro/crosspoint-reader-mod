#pragma once

#include <I18nKeys.h>

#include <cstddef>

// UI languages that are not built in are read from SD packs
// (/lang/<code>.cplang, written by gen_i18n.py). The chosen one is copied to
// internal flash and read from there like a built-in language, so the card can
// be removed and string lookups cost the same as before.
namespace UiLanguagePackStore {

// Longest _language_code a pack can carry, e.g. "ZH_HANT".
constexpr size_t CODE_LEN = 8;

// I18n::PackLoader: copies the pack to flash when the copy there is missing,
// stale or for another language. Main task only.
bool load(Language lang, LangStrings& out);

// Whether `lang` can be shown: built in, on the card, or already in flash.
bool available(Language lang);

// The card path of `lang`'s pack, e.g. "/lang/ja.cplang" or "/lang/zh_hant.cplang".
void packPath(Language lang, char* path, size_t size);

// Whether the file at `path` is an intact pack for `lang` (header, CRC and
// string ends), as load() would accept it.
bool isValidPack(Language lang, const char* path);

}  // namespace UiLanguagePackStore

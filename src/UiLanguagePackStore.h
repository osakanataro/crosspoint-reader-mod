#pragma once

#include <I18nKeys.h>

#include <cstddef>

// UI languages that are not built in are read from SD packs
// (/lang/<code>.cplang, written by gen_i18n.py). The chosen one is copied to
// internal flash and read from there like a built-in language, so the card can
// be removed and string lookups cost the same as before.
namespace UiLanguagePackStore {

// Room for a pack's _language_code; an 8-character code has no NUL.
constexpr size_t CODE_LEN = 8;
// Room for a pack's _default_font, NUL included.
constexpr size_t FONT_LEN = 32;

// I18n::PackLoader: copies the pack to flash when the copy there is missing,
// stale or for another language. Main task only.
bool load(Language lang, LangStrings& out);

// Whether `lang` can be shown: built in, on the card, or already in flash.
// Reads only the card pack's header.
bool available(Language lang);

// available(), but also checks the card pack's body (CRC), as load() does.
// For the language being chosen or restored at boot; it reads the whole pack
// unless flash already holds it.
bool loadable(Language lang);

// The card path of `lang`'s pack, e.g. "/lang/ja.cplang".
void packPath(Language lang, char* path, size_t size);

// Whether the file at `path` is an intact pack for `lang` (header, CRC and
// string ends), as load() would accept it.
bool isValidPack(Language lang, const char* path);

// Copies the _default_font named by `lang`'s pack on the card into `out`
// ("" for the built-in fonts). False when the card has no valid pack header
// for `lang` or the name does not fit; reads only the header.
bool packDefaultFont(Language lang, char* out, size_t size);

}  // namespace UiLanguagePackStore

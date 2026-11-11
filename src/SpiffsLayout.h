#pragma once

#include <cstddef>

// The `spiffs` partition carries no filesystem. Two stores write it raw:
//   hyphenation banks (HyphenationPackStore) at 0 and size / 2, each
//   size / 2 - UI_LANGUAGE_REGION_SIZE long;
//   the UI language copy (UiLanguagePackStore) in the last
//   UI_LANGUAGE_REGION_SIZE, one 64 KB mapping page.
namespace SpiffsLayout {

constexpr size_t UI_LANGUAGE_REGION_SIZE = 64 * 1024;

}  // namespace SpiffsLayout

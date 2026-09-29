#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// The page shown while the selected SD font is copied into the inactive OTA
// slot: font name, copy/verify stage, a progress bar and the power warning.
// Shared by Text Settings (a change while the copy is switched on) and the
// boot-time rebuild after a firmware update. Draws only; the caller refreshes.
namespace fontflashcache {

void draw(const GfxRenderer& renderer, const char* familyName, uint8_t pointSize, size_t completed, size_t total,
          bool ready);

}  // namespace fontflashcache

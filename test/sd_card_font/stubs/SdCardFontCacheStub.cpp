// Host tests read fonts from the SD stub only: there is no flash copy.
#include <SdCardFontCache.h>

namespace SdCardFontCache {
bool isValidFor(const char*, size_t*) { return false; }
bool readAt(size_t, void*, size_t, size_t) { return false; }
}  // namespace SdCardFontCache

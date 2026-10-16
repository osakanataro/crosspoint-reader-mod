#include "UiLanguagePackStore.h"

#include <HalStorage.h>
#include <I18nStrings.h>
#include <Logging.h>
#include <Memory.h>

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>

#if I18N_LANGUAGE_PACKS
#include <Arduino.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>

#include <algorithm>
#endif

namespace UiLanguagePackStore {

void packPath(const Language lang, char* path, const size_t size) {
  char lower[CODE_LEN + 1] = {};
  const char* code = LANGUAGE_CODES[static_cast<size_t>(lang)];
  for (size_t i = 0; i < CODE_LEN && code[i]; ++i) {
    lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(code[i])));
  }
  snprintf(path, size, "/lang/%s.cplang", lower);
}

#if !I18N_LANGUAGE_PACKS

bool load(Language, LangStrings&) { return false; }
bool available(const Language lang) { return isLanguageBuiltIn(lang); }
bool loadable(const Language lang) { return isLanguageBuiltIn(lang); }
bool isValidPack(Language, const char*) { return false; }

#else

namespace {

// SD pack layout (gen_i18n.py write_language_pack): header, key hashes (sorted),
// string offsets, padding to 4 bytes, strings.
struct PackHeader {
  char magic[4];
  uint8_t version;
  char code[CODE_LEN];
  uint8_t pad[3];
  uint16_t count;
  uint16_t pad2;
  uint32_t dataSize;
  uint32_t crc32;  // of everything after the header
};
static_assert(sizeof(PackHeader) == 28);

// Flash copy: this header, then an offset table in StrId order (built-in
// layout, English offsets flagged with bit 15), then the strings.
struct StoreHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t keyCount;
  uint32_t buildHash;
  uint32_t packCrc;
  char code[CODE_LEN];
  uint32_t dataSize;
  uint32_t crc32;  // of the fields above
};
static_assert(sizeof(StoreHeader) == 32);

constexpr uint32_t STORE_MAGIC = 0x534C5043;  // CPLS
constexpr uint16_t STORE_VERSION = 2;
constexpr uint8_t PACK_VERSION = 2;
constexpr size_t SECTOR_SIZE = 4096;
// The last 64 KB of the spiffs partition; one mapping page.
constexpr size_t REGION_SIZE = 64 * 1024;
constexpr size_t KEY_COUNT = static_cast<size_t>(StrId::_COUNT);
constexpr size_t TABLE_OFFSET = 32;
static_assert(sizeof(StoreHeader) <= TABLE_OFFSET);
constexpr size_t DATA_OFFSET = (TABLE_OFFSET + KEY_COUNT * 2 + 3) & ~size_t{3};
constexpr size_t MAX_DATA = REGION_SIZE - DATA_OFFSET;
constexpr size_t COPY_BUFFER_SIZE = 1024;
constexpr size_t MAX_PACK_KEYS = 4096;

const esp_partition_t* partition = nullptr;
size_t regionOffset = 0;
const uint8_t* mapped = nullptr;
esp_partition_mmap_handle_t mapHandle = 0;

uint32_t storeCrc(const StoreHeader& h) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&h), offsetof(StoreHeader, crc32));
}

void codeOf(const Language lang, char code[CODE_LEN]) {
  memset(code, 0, CODE_LEN);
  strncpy(code, LANGUAGE_CODES[static_cast<size_t>(lang)], CODE_LEN);
}

bool openRegion() {
  if (partition) return true;
  const auto* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!p || p->size < REGION_SIZE) {
    LOG_ERR("LANG", "No flash partition for language packs");
    return false;
  }
  regionOffset = p->size - REGION_SIZE;
  const void* ptr = nullptr;
  if (esp_partition_mmap(p, regionOffset, REGION_SIZE, ESP_PARTITION_MMAP_DATA, &ptr, &mapHandle) != ESP_OK) {
    LOG_ERR("LANG", "Flash mapping failed");
    return false;
  }
  partition = p;
  mapped = static_cast<const uint8_t*>(ptr);
  return true;
}

bool storeHolds(const char code[CODE_LEN], StoreHeader& out) {
  memcpy(&out, mapped, sizeof(out));
  return out.magic == STORE_MAGIC && out.version == STORE_VERSION && out.crc32 == storeCrc(out) &&
         out.keyCount == KEY_COUNT && out.buildHash == I18N_BUILD_HASH && memcmp(out.code, code, CODE_LEN) == 0 &&
         out.dataSize <= MAX_DATA;
}

bool readPackHeader(HalFile& file, const char code[CODE_LEN], PackHeader& h) {
  if (file.read(&h, sizeof(h)) != static_cast<int>(sizeof(h))) return false;
  const size_t tables = (static_cast<size_t>(h.count) * 6 + 3) & ~size_t{3};
  return memcmp(h.magic, "CPLG", 4) == 0 && h.version == PACK_VERSION && memcmp(h.code, code, CODE_LEN) == 0 &&
         h.count <= MAX_PACK_KEYS && h.dataSize <= MAX_DATA && h.dataSize <= 0x7FFF &&
         file.size() == sizeof(h) + tables + h.dataSize;
}

// Reads the body that follows the header; `buffer` holds COPY_BUFFER_SIZE bytes.
bool bodyIsValid(HalFile& file, const PackHeader& pack, const char code[CODE_LEN], uint8_t* buffer) {
  uint32_t crc = 0;
  uint8_t lastByte = 0;
  size_t remaining = file.size() - sizeof(PackHeader);
  while (remaining > 0) {
    const size_t n = std::min(COPY_BUFFER_SIZE, remaining);
    if (file.read(buffer, n) != static_cast<int>(n)) return false;
    crc = esp_rom_crc32_le(crc, buffer, n);
    lastByte = buffer[n - 1];
    remaining -= n;
  }
  // The strings end the body; an unterminated last one would read past the copy.
  if (crc != pack.crc32 || (pack.dataSize > 0 && lastByte != '\0')) {
    LOG_ERR("LANG", "Pack %.8s is damaged", code);
    return false;
  }
  return true;
}

bool install(HalFile& file, const PackHeader& pack, const char code[CODE_LEN]) {
  auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER_SIZE);
  if (!buffer) return false;

  // Check the whole body before erasing the current copy.
  if (!bodyIsValid(file, pack, code, buffer.get())) return false;

  // Map each StrId to the pack's string, or to the English one it lacks.
  auto hashes = makeUniqueNoThrow<uint32_t[]>(pack.count);
  auto offsets = makeUniqueNoThrow<uint16_t[]>(pack.count);
  auto table = makeUniqueNoThrow<uint16_t[]>(KEY_COUNT);
  if (!hashes || !offsets || !table) return false;
  if (!file.seek(sizeof(PackHeader)) || file.read(hashes.get(), pack.count * 4) != static_cast<int>(pack.count * 4) ||
      file.read(offsets.get(), pack.count * 2) != static_cast<int>(pack.count * 2)) {
    return false;
  }
  const uint32_t* first = hashes.get();
  const uint32_t* last = first + pack.count;
  size_t own = 0;
  for (size_t i = 0; i < KEY_COUNT; ++i) {
    const uint32_t* it = std::lower_bound(first, last, I18N_KEY_HASHES[i]);
    if (it != last && *it == I18N_KEY_HASHES[i] && offsets[it - first] < pack.dataSize) {
      table[i] = offsets[it - first];
      ++own;
    } else {
      table[i] = i18n_strings::OFFSETS_EN[i] | 0x8000;
    }
  }
  hashes.reset();
  offsets.reset();

  const size_t used = DATA_OFFSET + pack.dataSize;
  const size_t eraseSize = (used + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
  if (esp_partition_erase_range(partition, regionOffset, eraseSize) != ESP_OK ||
      esp_partition_write(partition, regionOffset + TABLE_OFFSET, table.get(), KEY_COUNT * 2) != ESP_OK) {
    LOG_ERR("LANG", "Flash write failed");
    return false;
  }
  table.reset();

  const size_t dataStart = sizeof(PackHeader) + ((static_cast<size_t>(pack.count) * 6 + 3) & ~size_t{3});
  if (!file.seek(dataStart)) return false;
  for (size_t done = 0; done < pack.dataSize;) {
    const size_t n = std::min(COPY_BUFFER_SIZE, static_cast<size_t>(pack.dataSize) - done);
    if (file.read(buffer.get(), n) != static_cast<int>(n) ||
        esp_partition_write(partition, regionOffset + DATA_OFFSET + done, buffer.get(), n) != ESP_OK) {
      LOG_ERR("LANG", "Flash write failed");
      return false;
    }
    done += n;
  }

  // Written last: a copy cut short by power loss has no valid header.
  StoreHeader header{};
  header.magic = STORE_MAGIC;
  header.version = STORE_VERSION;
  header.keyCount = KEY_COUNT;
  header.buildHash = I18N_BUILD_HASH;
  header.packCrc = pack.crc32;
  memcpy(header.code, code, CODE_LEN);
  header.dataSize = pack.dataSize;
  header.crc32 = storeCrc(header);
  if (esp_partition_write(partition, regionOffset, &header, sizeof(header)) != ESP_OK) {
    LOG_ERR("LANG", "Flash write failed");
    return false;
  }
  LOG_INF("LANG", "Installed %.8s: %u of %u strings, %u B", code, static_cast<unsigned>(own),
          static_cast<unsigned>(KEY_COUNT), static_cast<unsigned>(pack.dataSize));
  return true;
}

}  // namespace

bool load(const Language lang, LangStrings& out) {
  if (lang >= Language::_COUNT || isLanguageBuiltIn(lang) || !openRegion()) return false;
  char code[CODE_LEN];
  codeOf(lang, code);
  char path[32];
  packPath(lang, path, sizeof(path));

  const unsigned long start = millis();
  StoreHeader stored{};
  const bool inFlash = storeHolds(code, stored);
  HalFile file;
  PackHeader pack{};
  const bool onCard =
      Storage.exists(path) && Storage.openFileForRead("LANG", path, file) && readPackHeader(file, code, pack);

  // The card's pack wins when it differs from the copy (an updated translation).
  if (onCard && (!inFlash || stored.packCrc != pack.crc32)) {
    if (install(file, pack, code) && storeHolds(code, stored)) {
      LOG_INF("LANG", "Copied %s to flash in %lu ms", path, millis() - start);
    } else if (!storeHolds(code, stored)) {
      return false;
    } else {
      // install() checks the body before erasing: a damaged pack keeps the copy.
      LOG_ERR("LANG", "Kept the flash copy of %.8s", code);
    }
  } else if (!inFlash) {
    LOG_INF("LANG", "No language pack %s", path);
    return false;
  }
  out.offsets = reinterpret_cast<const uint16_t*>(mapped + TABLE_OFFSET);
  out.data = reinterpret_cast<const char*>(mapped + DATA_OFFSET);
  return true;
}

bool isValidPack(const Language lang, const char* path) {
  if (lang >= Language::_COUNT || isLanguageBuiltIn(lang)) return false;
  char code[CODE_LEN];
  codeOf(lang, code);
  HalFile file;
  PackHeader pack{};
  if (!Storage.openFileForRead("LANG", path, file) || !readPackHeader(file, code, pack)) return false;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER_SIZE);
  return buffer && bodyIsValid(file, pack, code, buffer.get());
}

bool loadable(const Language lang) {
  if (lang >= Language::_COUNT) return false;
  if (isLanguageBuiltIn(lang)) return true;
  char code[CODE_LEN];
  codeOf(lang, code);
  StoreHeader stored{};
  if (openRegion() && storeHolds(code, stored)) return true;
  char path[32];
  packPath(lang, path, sizeof(path));
  return isValidPack(lang, path);
}

bool available(const Language lang) {
  if (lang >= Language::_COUNT) return false;
  if (isLanguageBuiltIn(lang)) return true;
  char code[CODE_LEN];
  codeOf(lang, code);
  StoreHeader stored{};
  if (openRegion() && storeHolds(code, stored)) return true;
  char path[32];
  packPath(lang, path, sizeof(path));
  // A pack in an older format counts as missing, so it is downloaded again.
  HalFile file;
  PackHeader pack{};
  return Storage.exists(path) && Storage.openFileForRead("LANG", path, file) && readPackHeader(file, code, pack);
}

#endif  // I18N_LANGUAGE_PACKS

}  // namespace UiLanguagePackStore

#include "SdCardFontCache.h"

#include <HalOtaSlot.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>

#include <algorithm>
#include <cstring>

#include "SdCardFontCacheFormat.h"

namespace SdCardFontCache {
namespace {

using sd_card_font_cache_format::Header;

constexpr size_t CHUNK_SIZE = 4096;
constexpr size_t ERASE_BLOCK_SIZE = 64 * 1024;
// The 0x640000-byte app slot minus the header sector. Fixed here as well as
// derived from the partition so a larger slot on another board cannot silently
// change the format's assumptions.
constexpr size_t MAX_PAYLOAD_SIZE = 6549504;
constexpr size_t CPFONT_HEADER_SIZE = 32;
constexpr size_t CPFONT_TOC_ENTRY_SIZE = 32;
constexpr uint8_t CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

struct SourceIdentity {
  size_t size = 0;
  uint32_t contentHash = 0;
  // Byte after the regular style's last section: the least a copy must hold.
  size_t regularEnd = 0;
};

uint16_t readU16(const uint8_t* data) { return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8); }
uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t fnv1a(const uint8_t* data, size_t length, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// Reads the .cpfont header and style TOC the same way SdCardFont::load() does,
// so contentHash matches SdCardFont::contentHash() for the same file.
bool identifySource(const char* sourcePath, SourceIdentity& identity) {
  if (!sourcePath || strlen(sourcePath) >= sizeof(Header{}.sourcePath)) return false;

  HalFile file;
  if (!Storage.openFileForRead("SDFCACHE", sourcePath, file)) return false;

  uint8_t data[CPFONT_HEADER_SIZE];
  if (file.read(data, sizeof(data)) != static_cast<int>(sizeof(data)) ||
      memcmp(data, CPFONT_MAGIC, sizeof(CPFONT_MAGIC)) != 0 || readU16(data + 8) != CPFONT_VERSION || data[12] == 0 ||
      data[12] > SdCardFont::MAX_STYLES) {
    return false;
  }

  const uint8_t styleCount = data[12];
  uint32_t hash = fnv1a(data, sizeof(data));
  uint32_t dataOffsets[SdCardFont::MAX_STYLES] = {};
  uint8_t styleIds[SdCardFont::MAX_STYLES] = {};
  for (uint8_t i = 0; i < styleCount; ++i) {
    if (file.read(data, CPFONT_TOC_ENTRY_SIZE) != static_cast<int>(CPFONT_TOC_ENTRY_SIZE)) return false;
    hash = fnv1a(data, CPFONT_TOC_ENTRY_SIZE, hash);
    styleIds[i] = data[0];
    dataOffsets[i] = readU32(data + 24);
  }

  identity.size = file.fileSize();
  identity.contentHash = hash;
  if (identity.size < CPFONT_HEADER_SIZE + static_cast<size_t>(styleCount) * CPFONT_TOC_ENTRY_SIZE) return false;

  // The regular style (id 0) ends where the next style's data begins, or at
  // the end of the file when it is last (or the only style).
  uint32_t regularStart = 0;
  bool haveRegular = false;
  for (uint8_t i = 0; i < styleCount; ++i) {
    if (styleIds[i] == 0) {
      regularStart = dataOffsets[i];
      haveRegular = true;
    }
  }
  if (!haveRegular) return false;
  size_t regularEnd = identity.size;
  for (uint8_t i = 0; i < styleCount; ++i) {
    if (styleIds[i] != 0 && dataOffsets[i] > regularStart && dataOffsets[i] < regularEnd) regularEnd = dataOffsets[i];
  }
  identity.regularEnd = regularEnd;
  return true;
}

size_t payloadCapacity(const HalOtaSlot& slot) {
  return slot.size() > sd_card_font_cache_format::HEADER_AREA_SIZE
             ? std::min(slot.size() - sd_card_font_cache_format::HEADER_AREA_SIZE, MAX_PAYLOAD_SIZE)
             : 0;
}

bool readHeader(const HalOtaSlot& slot, Header& header) {
  return slot.valid() && slot.size() > sd_card_font_cache_format::HEADER_AREA_SIZE &&
         slot.read(0, &header, sizeof(header)) &&
         sd_card_font_cache_format::isHeaderValid(header, payloadCapacity(slot));
}

void report(ProgressCallback progress, size_t completed, size_t total, void* context) {
  if (progress) progress(completed, total, context);
}

size_t roundUp(size_t value, size_t alignment) { return (value + alignment - 1) / alignment * alignment; }

}  // namespace

size_t capacity() { return payloadCapacity(HalOtaSlot::inactive()); }

bool sourceFits(const char* sourcePath, size_t* payloadBytes) {
  if (payloadBytes) *payloadBytes = 0;
  SourceIdentity source{};
  if (!identifySource(sourcePath, source)) return false;
  const size_t cap = capacity();
  if (source.regularEnd > cap) return false;
  if (payloadBytes) *payloadBytes = std::min(source.size, cap);
  return true;
}

bool isValidFor(const char* sourcePath, size_t* payloadBytes) {
  if (payloadBytes) *payloadBytes = 0;

  const HalOtaSlot slot = HalOtaSlot::inactive();
  Header header{};
  SourceIdentity source{};
  const bool valid = readHeader(slot, header) && identifySource(sourcePath, source) &&
                     strcmp(header.sourcePath, sourcePath) == 0 && header.sourceSize == source.size &&
                     header.contentHash == source.contentHash && header.payloadSize >= source.regularEnd;
  if (valid && payloadBytes) *payloadBytes = header.payloadSize;
  return valid;
}

bool readAt(size_t offset, void* data, size_t length, size_t payloadBytes) {
  static const HalOtaSlot slot = HalOtaSlot::inactive();
  if (payloadBytes > payloadCapacity(slot) ||
      !sd_card_font_cache_format::containsPayloadRange(payloadBytes, offset, length)) {
    return false;
  }
  return length == 0 || slot.read(sd_card_font_cache_format::HEADER_AREA_SIZE + offset, data, length);
}

Result preload(const char* sourcePath, ProgressCallback progress, void* context) {
  SourceIdentity source{};
  if (!identifySource(sourcePath, source)) return Result::InvalidFont;
  if (isValidFor(sourcePath)) return Result::AlreadyCached;

  const HalOtaSlot slot = HalOtaSlot::inactive();
  if (!slot.valid() || !slot.safeForScratchWrite()) return Result::NotSafe;
  const size_t cap = payloadCapacity(slot);
  if (source.regularEnd > cap) return Result::TooLarge;
  const size_t payloadSize = std::min(source.size, cap);

  auto buffer = makeUniqueNoThrow<uint8_t[]>(CHUNK_SIZE);
  if (!buffer) return Result::Oom;

  HalFile file;
  if (!Storage.openFileForRead("SDFCACHE", sourcePath, file)) return Result::OpenFailed;
  // Erasing the header sector first invalidates whatever copy was there, so a
  // power loss mid-way leaves an invalid cache rather than a half-new one.
  if (!slot.erase(0, HalOtaSlot::ERASE_SIZE)) return Result::EraseFailed;

  const size_t total = payloadSize * 2;
  uint32_t payloadCrc = UINT32_MAX;
  size_t offset = 0;
  while (offset < payloadSize) {
    const size_t eraseLength = std::min(roundUp(payloadSize - offset, HalOtaSlot::ERASE_SIZE), ERASE_BLOCK_SIZE);
    if (!slot.erase(sd_card_font_cache_format::HEADER_AREA_SIZE + offset, eraseLength)) return Result::EraseFailed;

    const size_t blockEnd = std::min(offset + eraseLength, payloadSize);
    while (offset < blockEnd) {
      const size_t length = std::min(CHUNK_SIZE, blockEnd - offset);
      if (file.read(buffer.get(), length) != static_cast<int>(length)) return Result::ReadFailed;
      payloadCrc = sd_card_font_cache_format::crc32Update(payloadCrc, buffer.get(), length);
      if (!slot.write(sd_card_font_cache_format::HEADER_AREA_SIZE + offset, buffer.get(), length)) {
        return Result::WriteFailed;
      }
      offset += length;
      report(progress, offset, total, context);
    }
  }
  payloadCrc ^= UINT32_MAX;

  uint32_t flashCrc = UINT32_MAX;
  offset = 0;
  while (offset < payloadSize) {
    const size_t length = std::min(CHUNK_SIZE, payloadSize - offset);
    if (!slot.read(sd_card_font_cache_format::HEADER_AREA_SIZE + offset, buffer.get(), length)) {
      return Result::VerifyFailed;
    }
    flashCrc = sd_card_font_cache_format::crc32Update(flashCrc, buffer.get(), length);
    offset += length;
    report(progress, payloadSize + offset, total, context);
  }
  flashCrc ^= UINT32_MAX;
  if (flashCrc != payloadCrc) return Result::VerifyFailed;

  Header header{};
  memcpy(header.magic, sd_card_font_cache_format::MAGIC, sizeof(header.magic));
  header.version = sd_card_font_cache_format::VERSION;
  header.headerSize = sizeof(header);
  header.payloadSize = payloadSize;
  header.sourceSize = source.size;
  header.contentHash = source.contentHash;
  header.payloadCrc = payloadCrc;
  strncpy(header.sourcePath, sourcePath, sizeof(header.sourcePath) - 1);
  header.headerCrc = sd_card_font_cache_format::headerCrc(header);
  if (!slot.write(0, &header, sizeof(header))) return Result::WriteFailed;

  LOG_INF("SDFCACHE", "Cached %s: %u of %u bytes, crc=%08x", sourcePath, static_cast<unsigned>(payloadSize),
          static_cast<unsigned>(source.size), static_cast<unsigned>(payloadCrc));
  return Result::Ok;
}

const char* resultName(Result result) {
  switch (result) {
    case Result::Ok:
      return "ok";
    case Result::AlreadyCached:
      return "already_cached";
    case Result::OpenFailed:
      return "open_failed";
    case Result::InvalidFont:
      return "invalid_font";
    case Result::TooLarge:
      return "too_large";
    case Result::NotSafe:
      return "not_safe";
    case Result::Oom:
      return "oom";
    case Result::EraseFailed:
      return "erase_failed";
    case Result::ReadFailed:
      return "read_failed";
    case Result::WriteFailed:
      return "write_failed";
    case Result::VerifyFailed:
      return "verify_failed";
  }
  return "unknown";
}

}  // namespace SdCardFontCache

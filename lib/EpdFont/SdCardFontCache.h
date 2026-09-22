#pragma once

#include <cstddef>
#include <cstdint>

// A copy of one SD .cpfont in the inactive OTA application slot, read with
// bounded esp_partition_read() calls instead of SD-SPI transactions (about
// 20x less latency per read on the X3). The copy is the leading bytes of the
// file: the whole file when it fits the slot, otherwise as much as fits, which
// must still include the complete regular style. Reads past the copied range
// go to the SD card. Ported and adapted from crossmux (0x1abin/crossmux PR #57).
namespace SdCardFontCache {

enum class Result {
  Ok,
  AlreadyCached,
  OpenFailed,
  InvalidFont,
  TooLarge,
  NotSafe,
  Oom,
  EraseFailed,
  ReadFailed,
  WriteFailed,
  VerifyFailed,
};

using ProgressCallback = void (*)(size_t completed, size_t total, void* context);

// Payload bytes the slot can hold (0 when there is no usable inactive slot).
size_t capacity();
// True when the file's regular style lies within capacity(); `payloadBytes`
// receives the number of leading bytes a copy would hold.
bool sourceFits(const char* sourcePath, size_t* payloadBytes = nullptr);
// True when the slot holds a valid copy of exactly this file (path, size and
// header/TOC hash match); `payloadBytes` receives the copied length.
bool isValidFor(const char* sourcePath, size_t* payloadBytes = nullptr);
// Read `length` bytes at `offset` of the source from the copy. False when the
// range is not within the copied `payloadBytes` or the flash read fails.
bool readAt(size_t offset, void* data, size_t length, size_t payloadBytes);
// Copy the file (or its fitting prefix) into the slot, verify it, then commit
// the header. `progress` runs on the calling task; total = 2 x payload bytes
// (copy, then read-back verification).
Result preload(const char* sourcePath, ProgressCallback progress = nullptr, void* context = nullptr);
const char* resultName(Result result);

}  // namespace SdCardFontCache

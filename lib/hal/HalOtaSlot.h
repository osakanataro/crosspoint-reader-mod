#pragma once

#include <cstddef>
#include <cstdint>

// Bounded access to the OTA application slot that is not currently running.
// The slot doubles as disposable storage for the SD font copy (see
// lib/EpdFont/SdCardFontCache.*); callers never touch ESP-IDF partition APIs
// directly. Ported from crossmux (0x1abin/crossmux PR #57).
class HalOtaSlot {
 public:
  enum class RunningImageState : uint8_t {
    PendingVerify,
    Confirmed,
    Untracked,
    Unsafe,
  };

  static constexpr size_t ERASE_SIZE = 4096;

  HalOtaSlot() = default;

  static HalOtaSlot inactive();
  static RunningImageState runningImageState();

  bool valid() const { return partition_ != nullptr; }
  size_t size() const { return size_; }
  // Erasing the other slot destroys the image the bootloader could roll back
  // to, so it is only allowed once the running image is confirmed (or was
  // never tracked, e.g. flashed over USB).
  bool safeForScratchWrite() const;
  bool read(size_t offset, void* data, size_t length) const;
  bool erase(size_t offset, size_t length) const;
  bool write(size_t offset, const void* data, size_t length) const;

 private:
  explicit HalOtaSlot(const void* partition, size_t size) : partition_(partition), size_(size) {}
  bool contains(size_t offset, size_t length) const;

  const void* partition_ = nullptr;
  size_t size_ = 0;
};

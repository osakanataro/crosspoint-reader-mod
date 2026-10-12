// Cuts the power after every sector write an SdFat operation makes and checks
// that no live entry then names a cluster the FAT marks free (the state that
// lets the next allocation cross-link two files). Lost clusters are allowed.
// Built and run by run_write_order_test.py against the patched SdFat.
#include <FatLib/FatFormatter.h>
#include <FatLib/FatLib.h>

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

constexpr size_t kSectorBytes = 512;
constexpr size_t kSectors = 32768;  // 16 MiB formats as FAT16

struct RamDevice : FsBlockDeviceInterface {
  std::vector<uint8_t> image = std::vector<uint8_t>(kSectors * kSectorBytes);
  bool recording = false;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> writes;

  bool isBusy() override { return false; }
  Sector_t sectorCount() override { return image.size() / kSectorBytes; }
  bool syncDevice() override { return true; }
  bool readSector(Sector_t sector, uint8_t* dst) override {
    memcpy(dst, &image[sector * kSectorBytes], kSectorBytes);
    return true;
  }
  bool readSectors(Sector_t sector, uint8_t* dst, size_t count) override {
    for (size_t i = 0; i < count; i++) readSector(sector + i, dst + i * kSectorBytes);
    return true;
  }
  bool writeSector(Sector_t sector, const uint8_t* src) override {
    if (recording) writes.emplace_back(sector, std::vector<uint8_t>(src, src + kSectorBytes));
    memcpy(&image[sector * kSectorBytes], src, kSectorBytes);
    return true;
  }
  bool writeSectors(Sector_t sector, const uint8_t* src, size_t count) override {
    for (size_t i = 0; i < count; i++) writeSector(sector + i, src + i * kSectorBytes);
    return true;
  }
};

uint16_t fat16Entry(const std::vector<uint8_t>& image, uint32_t fatStart, uint32_t cluster) {
  const size_t at = fatStart * kSectorBytes + cluster * 2;
  return image[at] | (image[at + 1] << 8);
}

// True when every chain of a live root entry runs through allocated clusters.
bool consistent(const std::vector<uint8_t>& image) {
  RamDevice device;
  device.image = image;
  FatVolume volume;
  if (!volume.begin(&device) || volume.fatType() != 16) return false;
  FatFile root;
  FatFile file;
  if (!root.openRoot(&volume)) return false;
  while (file.openNext(&root, O_RDONLY)) {
    uint32_t cluster = file.firstCluster();
    for (uint32_t steps = 0; cluster != 0 && cluster < 0xFFF8; steps++) {
      if (steps > volume.clusterCount()) return false;  // loop in the FAT
      const uint16_t next = fat16Entry(image, volume.fatStartSector(), cluster);
      if (next == 0) return false;  // entry or chain reaches a free cluster
      cluster = next;
    }
    file.close();
  }
  return true;
}

using Operation = void (*)(FatVolume&);

// Returns the number of power-cut points that leave an unsafe card.
int unsafeCuts(const char* name, Operation operation) {
  RamDevice device;
  static uint8_t sectorBuffer[kSectorBytes];
  FatFormatter formatter;
  FatVolume volume;
  FatFile file;
  if (!formatter.format(&device, sectorBuffer) || !volume.begin(&device)) {
    printf("%-14s setup failed\n", name);
    return 1;
  }
  // 300 clusters span two FAT sectors, so freeing them also exercises eviction.
  std::vector<uint8_t> data(volume.bytesPerCluster() * 300, 0xAB);
  if (!file.open(&volume, "victim.bin", O_RDWR | O_CREAT) || file.write(data.data(), data.size()) != data.size() ||
      !file.close()) {
    printf("%-14s setup failed\n", name);
    return 1;
  }
  std::vector<uint8_t> image = device.image;
  device.recording = true;
  operation(volume);
  device.recording = false;

  int unsafe = consistent(image) ? 0 : 1;
  for (const auto& [sector, bytes] : device.writes) {
    memcpy(&image[sector * kSectorBytes], bytes.data(), kSectorBytes);
    if (!consistent(image)) unsafe++;
  }
  printf("%-14s writes=%-4zu unsafe cut points=%d\n", name, device.writes.size(), unsafe);
  return unsafe;
}

}  // namespace

int main() {
  int unsafe = 0;
  unsafe += unsafeCuts("remove", [](FatVolume& v) {
    FatFile f;
    f.open(&v, "victim.bin", O_RDWR);
    f.remove();
  });
  unsafe += unsafeCuts("truncate(0)", [](FatVolume& v) {
    FatFile f;
    f.open(&v, "victim.bin", O_RDWR);
    f.truncate(0);
    f.close();
  });
  unsafe += unsafeCuts("truncate(mid)", [](FatVolume& v) {
    FatFile f;
    f.open(&v, "victim.bin", O_RDWR);
    f.truncate(v.bytesPerCluster() * 10);
    f.close();
  });
  unsafe += unsafeCuts("open O_TRUNC", [](FatVolume& v) {
    FatFile f;
    f.open(&v, "victim.bin", O_RDWR | O_TRUNC);
    f.close();
  });
  unsafe += unsafeCuts("append", [](FatVolume& v) {
    FatFile f;
    f.open(&v, "victim.bin", O_RDWR | O_AT_END);
    std::vector<uint8_t> d(v.bytesPerCluster() * 50, 1);
    f.write(d.data(), d.size());
    f.close();
  });
  unsafe += unsafeCuts("create small", [](FatVolume& v) {
    FatFile f;
    f.open(&v, "small.bin", O_RDWR | O_CREAT);
    std::vector<uint8_t> d(v.bytesPerCluster() * 3, 2);
    f.write(d.data(), d.size());
    f.close();
  });
  unsafe += unsafeCuts("create large", [](FatVolume& v) {
    FatFile f;
    f.open(&v, "large.bin", O_RDWR | O_CREAT);
    std::vector<uint8_t> d(v.bytesPerCluster() * 300, 2);
    f.write(d.data(), d.size());
    f.close();
  });
  return unsafe == 0 ? 0 : 1;
}

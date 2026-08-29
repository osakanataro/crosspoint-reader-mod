#include "PngStreamDecoder.h"

#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "ImageToFramebufferDecoder.h"
#include "PixelCache.h"

namespace {

constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

uint32_t readBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Pulls IDAT payload bytes for InflateStream, walking chunk boundaries as it
// goes: after one IDAT's data it skips the CRC, reads the next chunk header,
// and continues only into another IDAT (the spec requires them consecutive).
struct IdatSource {
  HalFile* file = nullptr;
  uint32_t remainingInChunk = 0;
  bool exhausted = false;
  uint8_t buf[2048];

  static size_t fill(void* ctx, const uint8_t** data) {
    auto* self = static_cast<IdatSource*>(ctx);
    if (self->exhausted) return 0;

    while (self->remainingInChunk == 0) {
      // End of this IDAT: skip its CRC, then look at the next chunk.
      uint8_t header[8];
      if (!self->file->seek(self->file->position() + 4) || self->file->read(header, 8) != 8) {
        self->exhausted = true;
        return 0;
      }
      if (memcmp(header + 4, "IDAT", 4) != 0) {
        self->exhausted = true;  // first non-IDAT chunk ends the stream
        return 0;
      }
      self->remainingInChunk = readBE32(header);
    }

    const size_t want = self->remainingInChunk < sizeof(self->buf) ? self->remainingInChunk : sizeof(self->buf);
    const int got = self->file->read(self->buf, want);
    if (got <= 0) {
      self->exhausted = true;
      return 0;
    }
    self->remainingInChunk -= static_cast<uint32_t>(got);
    *data = self->buf;
    return static_cast<size_t>(got);
  }
};

int paethPredictor(const int a, const int b, const int c) {
  const int p = a + b - c;
  const int pa = p > a ? p - a : a - p;
  const int pb = p > b ? p - b : b - p;
  const int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

// In-place reversal of the per-row filter. prev is the reconstructed previous
// row (all zeros for the first row, as the spec defines).
bool unfilterRow(const uint8_t filter, uint8_t* row, const uint8_t* prev, const size_t rowBytes, const size_t bpp) {
  switch (filter) {
    case 0:  // None
      return true;
    case 1:  // Sub
      for (size_t i = bpp; i < rowBytes; i++) row[i] = uint8_t(row[i] + row[i - bpp]);
      return true;
    case 2:  // Up
      for (size_t i = 0; i < rowBytes; i++) row[i] = uint8_t(row[i] + prev[i]);
      return true;
    case 3:  // Average
      for (size_t i = 0; i < bpp; i++) row[i] = uint8_t(row[i] + prev[i] / 2);
      for (size_t i = bpp; i < rowBytes; i++) row[i] = uint8_t(row[i] + (row[i - bpp] + prev[i]) / 2);
      return true;
    case 4:  // Paeth
      for (size_t i = 0; i < bpp; i++) row[i] = uint8_t(row[i] + prev[i]);
      for (size_t i = bpp; i < rowBytes; i++)
        row[i] = uint8_t(row[i] + paethPredictor(row[i - bpp], prev[i], prev[i - bpp]));
      return true;
    default:
      return false;
  }
}

uint8_t luma(const uint8_t r, const uint8_t g, const uint8_t b) { return uint8_t((r * 77 + g * 150 + b * 29) >> 8); }

uint8_t compositeWhite(const uint8_t gray, const uint8_t alpha) {
  return uint8_t((gray * alpha + 255 * (255 - alpha)) / 255);
}

// Scale factors for expanding sub-byte grayscale samples to 8 bits.
uint8_t expandGray(const uint32_t v, const int bitDepth) {
  switch (bitDepth) {
    case 1:
      return v ? 255 : 0;
    case 2:
      return uint8_t(v * 85);
    case 4:
      return uint8_t(v * 17);
    default:
      return uint8_t(v);
  }
}

}  // namespace

bool PngStreamDecoder::decodeToCache(const std::string& pngPath, const std::string& cachePath, const int dstWidth,
                                     const int dstHeight) {
  if (dstWidth <= 0 || dstHeight <= 0) return false;

  HalFile file;
  if (!Storage.openFileForRead("PNS", pngPath, file)) return false;

  uint8_t sig[8];
  if (file.read(sig, 8) != 8 || memcmp(sig, PNG_SIGNATURE, 8) != 0) return false;

  // --- Chunk walk up to the first IDAT ---------------------------------------
  int srcWidth = 0, srcHeight = 0, bitDepth = 0, colorType = 0;
  uint8_t palette[256 * 3];
  uint8_t paletteAlpha[256];
  int paletteEntries = 0;
  bool haveTrns = false;
  memset(paletteAlpha, 0xFF, sizeof(paletteAlpha));
  // tRNS for gray/RGB names one fully transparent sample value.
  uint32_t transparentGray = 0xFFFFFFFF;
  uint32_t transparentRgb = 0xFFFFFFFF;

  bool haveIhdr = false;
  uint32_t idatLength = 0;
  for (;;) {
    uint8_t header[8];
    if (file.read(header, 8) != 8) return false;
    const uint32_t length = readBE32(header);
    const char* type = reinterpret_cast<const char*>(header + 4);

    if (memcmp(type, "IHDR", 4) == 0) {
      uint8_t ihdr[13];
      if (length != 13 || file.read(ihdr, 13) != 13) return false;
      srcWidth = static_cast<int>(readBE32(ihdr));
      srcHeight = static_cast<int>(readBE32(ihdr + 4));
      bitDepth = ihdr[8];
      colorType = ihdr[9];
      const uint8_t interlace = ihdr[12];
      ImageDimensions dims;
      if (!ImageToFramebufferDecoder::validateAndStoreDimensions(srcWidth, srcHeight, dims, "PNG(stream)"))
        return false;
      if (interlace != 0 || ihdr[10] != 0 || ihdr[11] != 0) {
        LOG_DBG("PNS", "Declining interlaced/nonstandard PNG (interlace=%d)", interlace);
        return false;  // PNGdec fallback handles Adam7 when the heap allows
      }
      const bool depthOk = (colorType == 0 || colorType == 3)
                               ? (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8)
                               : (bitDepth == 8);
      const bool typeOk = colorType == 0 || colorType == 2 || colorType == 3 || colorType == 4 || colorType == 6;
      if (!depthOk || !typeOk) {
        LOG_DBG("PNS", "Declining PNG type=%d depth=%d", colorType, bitDepth);
        return false;
      }
      if (!file.seek(file.position() + 4)) return false;  // CRC
      haveIhdr = true;
    } else if (memcmp(type, "PLTE", 4) == 0) {
      paletteEntries = static_cast<int>(length / 3);
      if (paletteEntries > 256) paletteEntries = 256;
      if (file.read(palette, static_cast<size_t>(paletteEntries) * 3) != paletteEntries * 3) return false;
      if (!file.seek(file.position() + (length - static_cast<uint32_t>(paletteEntries) * 3) + 4)) return false;
    } else if (memcmp(type, "tRNS", 4) == 0) {
      haveTrns = true;
      if (colorType == 3) {
        const uint32_t n = length < 256 ? length : 256;
        if (file.read(paletteAlpha, n) != static_cast<int>(n)) return false;
        if (!file.seek(file.position() + (length - n) + 4)) return false;
      } else if (colorType == 0 && length >= 2) {
        uint8_t v[2];
        if (file.read(v, 2) != 2) return false;
        transparentGray = (uint32_t(v[0]) << 8) | v[1];
        if (!file.seek(file.position() + (length - 2) + 4)) return false;
      } else if (colorType == 2 && length >= 6) {
        uint8_t v[6];
        if (file.read(v, 6) != 6) return false;
        // 8-bit images store the sample in the low byte of each 16-bit field.
        transparentRgb = (uint32_t(v[1]) << 16) | (uint32_t(v[3]) << 8) | v[5];
        if (!file.seek(file.position() + (length - 6) + 4)) return false;
      } else {
        if (!file.seek(file.position() + length + 4)) return false;
      }
    } else if (memcmp(type, "IDAT", 4) == 0) {
      idatLength = length;
      break;  // file is positioned at the IDAT payload
    } else if (memcmp(type, "IEND", 4) == 0) {
      return false;  // no pixel data
    } else {
      if (!file.seek(file.position() + length + 4)) return false;
    }
  }
  if (!haveIhdr) return false;
  if (colorType == 3 && paletteEntries == 0) return false;

  // --- Working buffers -------------------------------------------------------
  const int channels = (colorType == 2) ? 3 : (colorType == 4) ? 2 : (colorType == 6) ? 4 : 1;
  const size_t rowBytes = (static_cast<size_t>(srcWidth) * channels * bitDepth + 7) / 8;
  const size_t bpp = (static_cast<size_t>(channels) * bitDepth + 7) / 8;

  auto curRow = makeUniqueNoThrow<uint8_t[]>(rowBytes);
  auto prevRow = makeUniqueNoThrow<uint8_t[]>(rowBytes);
  auto grayLine = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(srcWidth));
  if (!curRow || !prevRow || !grayLine) {
    LOG_ERR("PNS", "OOM for row buffers (%u bytes/row)", static_cast<unsigned>(rowBytes));
    return false;
  }
  memset(prevRow.get(), 0, rowBytes);

  IdatSource source;
  source.file = &file;
  source.remainingInChunk = idatLength;

  InflateStream inflate;
  if (!inflate.init(/*streaming=*/true)) {
    LOG_ERR("PNS", "OOM for inflate state");
    return false;
  }
  inflate.setZlibWrapped();
  inflate.setFill(&IdatSource::fill, &source);

  PixelCache cache;
  if (!cache.begin(cachePath, dstWidth, dstHeight, 0, 0, 1)) {
    LOG_ERR("PNS", "Failed to start cache stream");
    return false;
  }

  // --- Row loop --------------------------------------------------------------
  uint32_t lastYieldMs = 0;
  int lastDstY = -1;
  bool ok = true;
  for (int srcY = 0; srcY < srcHeight && ok; srcY++) {
    uint8_t filter;
    if (!inflate.read(&filter, 1) || !inflate.read(curRow.get(), rowBytes) ||
        !unfilterRow(filter, curRow.get(), prevRow.get(), rowBytes, bpp)) {
      LOG_ERR("PNS", "Stream error at row %d", srcY);
      ok = false;
      break;
    }

    // Same output-row selection as the PNGdec callback: downscaling picks one
    // destination row for several source rows, upscaling repeats one source
    // row across its whole destination range.
    int firstDstY = (srcY * dstHeight) / srcHeight;
    int endDstY = dstHeight > srcHeight ? ((srcY + 1) * dstHeight) / srcHeight : firstDstY + 1;
    if (firstDstY <= lastDstY) firstDstY = lastDstY + 1;
    if (endDstY > dstHeight) endDstY = dstHeight;

    if (firstDstY < endDstY) {
      // Expand this row to 8-bit gray, alpha composited against white.
      const uint8_t* src = curRow.get();
      for (int x = 0; x < srcWidth; x++) {
        uint8_t g;
        switch (colorType) {
          case 0: {
            uint32_t v;
            if (bitDepth == 8) {
              v = src[x];
            } else {
              const size_t bitPos = static_cast<size_t>(x) * bitDepth;
              v = (src[bitPos >> 3] >> (8 - bitDepth - (bitPos & 7))) & ((1u << bitDepth) - 1);
            }
            g = expandGray(v, bitDepth);
            if (haveTrns && v == transparentGray) g = 255;
            break;
          }
          case 2: {
            const uint8_t* p = src + static_cast<size_t>(x) * 3;
            g = luma(p[0], p[1], p[2]);
            if (haveTrns && ((uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]) == transparentRgb) g = 255;
            break;
          }
          case 3: {
            uint32_t idx;
            if (bitDepth == 8) {
              idx = src[x];
            } else {
              const size_t bitPos = static_cast<size_t>(x) * bitDepth;
              idx = (src[bitPos >> 3] >> (8 - bitDepth - (bitPos & 7))) & ((1u << bitDepth) - 1);
            }
            if (idx >= static_cast<uint32_t>(paletteEntries)) idx = 0;
            const uint8_t* p = palette + idx * 3;
            g = compositeWhite(luma(p[0], p[1], p[2]), paletteAlpha[idx]);
            break;
          }
          case 4: {
            const uint8_t* p = src + static_cast<size_t>(x) * 2;
            g = compositeWhite(p[0], p[1]);
            break;
          }
          default: {  // 6
            const uint8_t* p = src + static_cast<size_t>(x) * 4;
            g = compositeWhite(luma(p[0], p[1], p[2]), p[3]);
            break;
          }
        }
        grayLine[x] = g;
      }

      for (int dstY = firstDstY; dstY < endDstY && ok; dstY++) {
        lastDstY = dstY;
        if (!cache.advanceTo(dstY)) {
          ok = false;
          break;
        }
        DirectCacheWriter cw;
        cw.init(cache.buffer, cache.bytesPerRow, cache.bandRows, cache.originX);
        cw.beginRow(dstY, cache.bandStart);
        int srcX = 0;
        int error = 0;
        for (int dstX = 0; dstX < dstWidth; dstX++) {
          cw.writePixel(dstX, applyBayerDither4Level(grayLine[srcX], dstX, dstY));
          error += srcWidth;
          while (error >= dstWidth) {
            error -= dstWidth;
            if (srcX < srcWidth - 1) srcX++;
          }
        }
      }
    }

    std::swap(curRow, prevRow);
    ImageToFramebufferDecoder::yieldDuringDecode(lastYieldMs);
  }

  if (!ok) {
    cache.abort();
    return false;
  }
  return cache.finalize();
}

#pragma once

#include <string>

// PNG -> .pxc pixel-cache decoder that streams IDAT through InflateStream
// instead of PNGdec.
//
// PNGdec's working set is one ~62KB object (32KB zlib window + inflate state +
// row buffers, allocated as a single new PNG()), and the heap frequently
// cannot produce that hole: a session was measured pinned at a 45,044-byte
// largest block from boot onward, where releasing every font cache moved it by
// zero bytes. Under that ceiling every decode -- build-time and render-time --
// fails, and which chapters of a book get their pictures becomes a lottery
// over heap layout.
//
// This decoder's inflate state comes from InflateStream: ~43KB claimed from
// the lent framebuffer when a FrameBufferLoan is active (build-time path,
// costing the heap nothing), or two separate heap blocks of 11KB + 32KB
// otherwise (render-time path -- 32KB contiguous is available even under the
// measured ceiling). What remains on the heap here is two filter rows, a gray
// line and the cache's single-row band: a few KB, bounded by image width.
//
// Coverage: non-interlaced PNGs, bit depth 1/2/4/8 for grayscale and palette,
// 8-bit for RGB / gray+alpha / RGBA. Alpha (channel or tRNS) is composited
// against white -- the page background -- rather than skipped, so transparent
// regions cache as white instead of the black that skipped pixels leave.
// Anything else (interlace, 16-bit) is declined so the caller can fall back
// to PNGdec, which handles them when the heap allows.
class PngStreamDecoder {
 public:
  // Decode the PNG file at pngPath into a finalized .pxc at cachePath, scaled
  // to exactly dstWidth x dstHeight (the layout's display size). Returns false
  // on unsupported form, OOM, or a corrupt stream; a partial cache file is
  // never left behind.
  static bool decodeToCache(const std::string& pngPath, const std::string& cachePath, int dstWidth, int dstHeight);
};

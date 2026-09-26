#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter;
class PngToFramebufferConverter;

class ImageDecoderFactory {
 public:
  // Returns non-owning pointer - factory owns the decoder lifetime
  static ImageToFramebufferDecoder* getDecoder(const std::string& imagePath);
  static bool isFormatSupported(const std::string& imagePath);

 private:
  // The decoders are file-scope statics in the .cpp: no heap block, so nothing is left behind in
  // the middle of the heap by the first image of a reading session.
};

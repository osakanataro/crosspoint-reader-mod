#include "ImageDecoderFactory.h"

#include <Logging.h>

#include <memory>
#include <string>

#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

namespace {
// Built at startup with the other statics: no heap block, and no first-use guard.
JpegToFramebufferConverter jpegDecoder;
PngToFramebufferConverter pngDecoder;
}  // namespace

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  std::string ext = imagePath;
  size_t dotPos = ext.rfind('.');
  if (dotPos != std::string::npos) {
    ext = ext.substr(dotPos);
    for (auto& c : ext) {
      c = tolower(c);
    }
  } else {
    ext = "";
  }

  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    return &jpegDecoder;
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    return &pngDecoder;
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) { return getDecoder(imagePath) != nullptr; }

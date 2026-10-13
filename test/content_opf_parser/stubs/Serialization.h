#pragma once

#include <string>

#include "Epub.h"

namespace serialization {

inline void writeString(HalFile&, const std::string&) {}
inline bool readString(HalFile&, std::string& out) {
  out.clear();
  return true;
}

}  // namespace serialization

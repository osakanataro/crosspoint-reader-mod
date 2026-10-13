#pragma once

#include <string>
#include <string_view>

namespace FsHelpers {
inline std::string decodeUriEscapes(const std::string& path) { return path; }
inline std::string normalisePath(const std::string& path) { return path; }
inline bool hasJpgExtension(std::string_view) { return false; }
inline bool hasPngExtension(std::string_view) { return false; }
}  // namespace FsHelpers

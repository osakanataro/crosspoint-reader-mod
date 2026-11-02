#pragma once

#include <string>
#include <string_view>

namespace FsHelpers {
inline std::string decodeUriEscapes(const std::string& path) { return path; }
inline std::string normalisePath(const std::string& path) { return path; }
inline bool hasTxtExtension(std::string_view name) { return name.ends_with(".txt"); }
inline bool hasMarkdownExtension(std::string_view name) { return name.ends_with(".md"); }
inline bool hasJpgExtension(std::string_view) { return false; }
inline bool hasPngExtension(std::string_view) { return false; }
inline bool hasGifExtension(std::string_view name) { return name.ends_with(".gif"); }
}  // namespace FsHelpers

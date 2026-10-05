#pragma once
#include <WString.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace FsHelpers {

std::string decodeUriEscapes(const std::string& path);

std::string normalisePath(const std::string& path);

// Numeric-aware, case-insensitive comparison ("2" < "10"). Returns true when str1 orders
// before str2. Same ordering sortFileList applies within the file/directory groups.
bool naturalLess(const std::string& str1, const std::string& str2);

void sortFileList(std::vector<std::string>& strs);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(std::string_view fileName, const char* extension);
inline bool checkFileExtension(const String& fileName, const char* extension) {
  return checkFileExtension(std::string_view{fileName.c_str(), fileName.length()}, extension);
}

// Check for either .jpg or .jpeg extension (case-insensitive)
bool hasJpgExtension(std::string_view fileName);
inline bool hasJpgExtension(const String& fileName) {
  return hasJpgExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .png extension (case-insensitive)
bool hasPngExtension(std::string_view fileName);
inline bool hasPngExtension(const String& fileName) {
  return hasPngExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .bmp extension (case-insensitive)
bool hasBmpExtension(std::string_view fileName);

// Check for .gif extension (case-insensitive)
bool hasGifExtension(std::string_view fileName);
inline bool hasGifExtension(const String& fileName) {
  return hasGifExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .epub extension (case-insensitive)
bool hasEpubExtension(std::string_view fileName);
inline bool hasEpubExtension(const String& fileName) {
  return hasEpubExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for either .xtc or .xtch extension (case-insensitive)
bool hasXtcExtension(std::string_view fileName);

// Check for .txt extension (case-insensitive)
bool hasTxtExtension(std::string_view fileName);
inline bool hasTxtExtension(const String& fileName) {
  return hasTxtExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .md extension (case-insensitive)
bool hasMarkdownExtension(std::string_view fileName);

// Check for .css extension (case-insensitive)
bool hasCssExtension(std::string_view fileName);
inline bool hasCssExtension(const String& fileName) {
  return hasCssExtension(std::string_view{fileName.c_str(), fileName.length()});
}
std::string extractFolderPath(const std::string& filePath);

// Rejects an empty component, one containing '/' or '\', or the exact components
// "." and "..", so a single filename/folder-name argument can never be used to
// escape the directory it is placed into. Names like "volume..2.epub" or
// "notes...txt" that merely contain ".." are accepted.
bool isSafePathComponent(std::string_view name);
inline bool isSafePathComponent(const String& name) {
  return isSafePathComponent(std::string_view{name.c_str(), name.length()});
}

/**
 * Sanitize a filename/path component for FAT32 in a caller-provided buffer.
 * Replaces invalid path characters, spaces, and control characters with '-'.
 */
void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen);

// Hash behind the cache directory names (epub_<n>, xtc_<n>, txt_<n>). On the device it is
// std::hash<std::string> itself. The simulator build replays the 32-bit libstdc++ algorithm
// (MurmurHash2, seed 0xc70f6907) so a card written by the device keeps its cache names on
// the host and the other way round; a 64-bit host's std::hash would name them differently.
#ifdef CROSSPOINT_SIM
inline size_t pathHash(const std::string& s) {
  constexpr uint32_t m = 0x5bd1e995u;
  const size_t len = s.size();
  uint32_t hash = 0xc70f6907u ^ static_cast<uint32_t>(len);
  const auto* buf = reinterpret_cast<const unsigned char*>(s.data());
  size_t remaining = len;
  while (remaining >= 4) {
    uint32_t k = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
                 (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
    k *= m;
    k ^= k >> 24;
    k *= m;
    hash *= m;
    hash ^= k;
    buf += 4;
    remaining -= 4;
  }
  switch (remaining) {
    case 3:
      hash ^= static_cast<uint32_t>(buf[2]) << 16;
      [[fallthrough]];
    case 2:
      hash ^= static_cast<uint32_t>(buf[1]) << 8;
      [[fallthrough]];
    case 1:
      hash ^= static_cast<uint32_t>(buf[0]);
      hash *= m;
  }
  hash ^= hash >> 13;
  hash *= m;
  hash ^= hash >> 15;
  return hash;
}
#else
inline size_t pathHash(const std::string& s) { return std::hash<std::string>{}(s); }
#endif

}  // namespace FsHelpers

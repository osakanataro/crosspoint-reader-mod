#pragma once

#include <HalStorage.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "CssStyle.h"

class CssSelectorUsage;

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */
class CssParser {
 public:
  enum class ParseResult : uint8_t {
    Complete,
    Partial,
    Error,
  };

  enum class CacheStatus : uint8_t {
    Missing,
    Complete,
    Partial,
    Invalid,
  };

  enum class CacheLoadResult : uint8_t {
    Complete,
    LowMemory,
    Invalid,
  };

  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  // v12 (this tree): the style record also carries text-emphasis and max-width / max-height
  //     (three more length/enum fields, defined bits 18-20). Upstream 1.6.0 is v11 with the
  //     shorter record, so the number has to differ or an old cache would decode shifted.
  // v13 (this tree): one more enum byte (text-orientation / text-combine-upright, defined bit
  //     21), and two-part descendant selectors (".vrtl .start-1em") are stored under a key with
  //     a single space, which v12 never wrote.
  static constexpr uint8_t CSS_CACHE_VERSION = 13;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return Complete unless bounded storage stopped rule growth or the source was invalid
   */
  ParseResult loadFromStream(HalFile& source);

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr) const;

  /**
   * Same, with the open ancestors of the element (outermost first) so two-part descendant
   * selectors can match. Each ancestor is its tag name and class attribute. Only ".a .b",
   * ".a tag", ".a tag.b" and the same three with a tag as the first part are stored; the
   * EBPAJ template every surveyed Japanese book carries writes its writing-mode-specific
   * rules that way (".vrtl .h-indent-1em", ".hltr .start-1em"), 956 of its 1,905 rules.
   */
  struct AncestorRef {
    std::string tag;
    std::string classAttr;
  };
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr,
                                      const AncestorRef* ancestors, size_t ancestorCount) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(std::string_view styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const { return entryCount_ == 0; }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const { return entryCount_; }

  /**
   * Clear all loaded rules
   */
  void clear() {
    hasDescendantRules_ = false;
    entries_.reset();
    selectorPool_.reset();
    stylePool_.reset();
    entryCount_ = entryCapacity_ = 0;
    selectorPoolSize_ = selectorPoolCapacity_ = 0;
    styleCount_ = styleCapacity_ = 0;
    ruleGrowthStopped_ = false;
  }

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /** Read the cache header without hydrating its rule map. */
  CacheStatus inspectCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache(bool complete) const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return Complete when loaded, LowMemory when it should be retried, otherwise Invalid
   */
  // usage: keep only rules whose selector can match the scanned chapter (nullptr = all).
  CacheLoadResult loadFromCache(const CssSelectorUsage* usage = nullptr);

  // While set, processRuleBlockWithStyle drops selectors the chapter cannot reference. Used by
  // Epub::parseCssFilesFiltered for templates too large to register whole.
  void setUsageFilter(const CssSelectorUsage* usage) { usageFilter_ = usage; }

  // True when the last successful loadFromCache() read a cache written from a truncated parse
  // (store limit or low heap). Such a set is fine for opening the book but not for building a
  // chapter whose rules may sit past the cut; Section re-parses with the chapter filter then.
  bool lastCacheLoadPartial() const { return lastCacheLoadPartial_; }

 private:
  enum class RuleInsertResult : uint8_t {
    Inserted,
    Merged,
    Limit,
    OutOfMemory,
  };

  enum class PoolResult : uint8_t {
    Ready,
    Limit,
    OutOfMemory,
  };

  struct SelectorEntry {
    uint32_t offset;
    uint16_t styleIndex;
    uint16_t length;
  };
  static_assert(sizeof(SelectorEntry) == 8);

  // Bounded flat storage keeps every growth operation fallible and avoids the
  // throwing node allocations used by std::unordered_map.
  std::unique_ptr<SelectorEntry[]> entries_;
  std::unique_ptr<char[]> selectorPool_;
  std::unique_ptr<CssStyle[]> stylePool_;
  uint16_t entryCount_ = 0;
  uint16_t entryCapacity_ = 0;
  uint32_t selectorPoolSize_ = 0;
  uint32_t selectorPoolCapacity_ = 0;
  uint16_t styleCount_ = 0;
  uint16_t styleCapacity_ = 0;
  bool ruleGrowthStopped_ = false;
  bool lastCacheLoadPartial_ = false;
  // Set once any stored key carries a descendant combinator, so resolveStyle can skip the
  // ancestor lookups entirely for the many stylesheets that have none.
  bool hasDescendantRules_ = false;
  const CssSelectorUsage* usageFilter_ = nullptr;

  std::string cachePath;

  // Internal parsing helpers
  bool restoreCacheBackupIfNeeded() const;
  void processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  [[nodiscard]] int compareEntryToPieces(const SelectorEntry& entry, const std::string_view* pieces,
                                         size_t pieceCount) const;
  [[nodiscard]] size_t lowerBound(const std::string_view* pieces, size_t pieceCount, bool& exact) const;
  [[nodiscard]] size_t lowerBound(std::string_view p0, std::string_view p1, std::string_view p2, bool& exact) const;
  [[nodiscard]] const CssStyle* findStyle(std::string_view p0, std::string_view p1 = {},
                                          std::string_view p2 = {}) const;
  // Descendant key: "<ancestorPart> <p0><p1><p2>", e.g. (".vrtl", "p", ".", "x") -> ".vrtl p.x".
  [[nodiscard]] const CssStyle* findDescendantStyle(std::string_view ancestorPart, std::string_view p0,
                                                    std::string_view p1 = {}, std::string_view p2 = {}) const;
  [[nodiscard]] std::string_view selectorAt(size_t index) const;
  RuleInsertResult insertOrMerge(std::string_view selector, const CssStyle& style);
  PoolResult ensureEntryCapacity(size_t needed);
  PoolResult ensureSelectorPoolCapacity(size_t needed);
  PoolResult ensureStyleCapacity(size_t needed);
  PoolResult internStyle(const CssStyle& style, uint16_t& indexOut);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssTextEmphasis interpretTextEmphasis(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);
};

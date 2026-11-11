#include <gtest/gtest.h>

#include "PrintfConversions.h"

using PrintfConversions::safe;

TEST(PrintfConversions, SameArgumentsAreSafe) {
  EXPECT_TRUE(safe("%d / %d ページ", "Page %d of %d"));
  EXPECT_TRUE(safe("%zu 件のネットワーク", "%zu networks found"));
  EXPECT_TRUE(safe("「%s」を削除", "Delete %s?"));
}

TEST(PrintfConversions, DroppedArgumentsAreSafe) {
  // Unused trailing arguments are never read.
  EXPECT_TRUE(safe("プラグインを削除しますか?", "Uninstall %s and delete all of its files?"));
  EXPECT_TRUE(safe("%d ページ", "Page %d of %d"));
}

TEST(PrintfConversions, OtherArgumentsAreNotSafe) {
  EXPECT_FALSE(safe("%s ページ", "Page %d of %d"));
  EXPECT_FALSE(safe("%d / %d / %d", "Page %d of %d"));
  EXPECT_FALSE(safe("%d", "Battery"));
  EXPECT_FALSE(safe("%ld", "%d"));
  EXPECT_FALSE(safe("%*d", "%d"));
}

TEST(PrintfConversions, ReorderedArgumentsAreNotSafe) { EXPECT_FALSE(safe("%d %s", "%s %d")); }

TEST(PrintfConversions, PercentTextIsNotAConversion) {
  EXPECT_TRUE(safe("Ẩn % pin", "Hide Battery %"));
  EXPECT_TRUE(safe("Ugrás %-ra", "Go to %"));
  EXPECT_TRUE(safe("%'ye git", "Go to %"));
  EXPECT_TRUE(safe("100%% 完了", "100%% done"));
  EXPECT_TRUE(safe("Buchfortschritt in %", "Book Progress Percentage"));
}

TEST(PrintfConversions, FlagsWidthAndPrecisionDoNotMatter) {
  EXPECT_TRUE(safe("%-5.2f", "%f"));
  EXPECT_TRUE(safe("%03d%%", "%d%%"));
}

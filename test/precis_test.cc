// SPDX-License-Identifier: AGPL-3.0-only
import std;
import gtest;
import cairn.precis;
#include "gtest/gtest-macros.h"

TEST(Precis, Keys) {
  EXPECT_EQ(cairn::precis::key("Nick"), std::optional<std::string>("nick"));
  EXPECT_TRUE(cairn::precis::equal("\xef\xbc\xae\xef\xbd\x89\xef\xbd\x83\xef\xbd\x8b", "nick"));  // full width
  EXPECT_TRUE(cairn::precis::equal("\xc3\x89lodie", "\xc3\xa9lodie"));
  EXPECT_TRUE(!cairn::precis::key("two words"));
  EXPECT_TRUE(!cairn::precis::key(""));
}

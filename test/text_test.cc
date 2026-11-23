// SPDX-License-Identifier: AGPL-3.0-only
import std;
import splice;
import gtest;
import cairn;
#include "gtest/gtest-macros.h"

namespace {
using namespace cairn::text;

static_assert(is_utf8("plain") && is_utf8("caf\xc3\xa9") && is_utf8("\xf0\x9f\x98\x80"));
static_assert(!is_utf8("caf\xe9"));          // Latin-1
static_assert(!is_utf8("\xc0\xaf"));         // overlong
static_assert(!is_utf8("\xed\xa0\x80"));     // a surrogate
static_assert(!is_utf8("\xf4\x90\x80\x80"));  // past U+10FFFF
static_assert(!is_utf8("\xe2\x82"));         // cut short

constexpr std::u32string all(std::string_view s) {
  std::u32string got;
  for (char32_t c : decoded(s)) got.push_back(c);
  return got;
}
static_assert(all("caf\xc3\xa9") == U"caf\u00e9");
static_assert(all("caf\xe9") == U"caf\u00e9");  // the same word in Latin-1
static_assert(all("\xf0\x9f\x98\x80!") == U"\U0001F600!");
static_assert(std::ranges::forward_range<decoded_view>);
static_assert([] {
  std::string out;
  write_utf8("caf\xe9", out);
  return out == "caf\xc3\xa9";
}());
}  // namespace

TEST(Text, MixedIsLatin1) { EXPECT_EQ(all("\xc3\xa9 \xe9").size(), 4u); }

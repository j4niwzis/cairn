// SPDX-License-Identifier: AGPL-3.0-only
// Long text split at words and characters; sending paced.
import std;
import splice;
import cairn;
import gtest;

#include "gtest/gtest-macros.h"

namespace {
using namespace cairn;
constexpr std::vector<std::string> split(std::string_view text, std::size_t room) {
  return std::ranges::to<std::vector<std::string>>(pieces(text, room));
}
static_assert(split("short", 10) == std::vector<std::string>{"short"});
static_assert(split("", 10) == std::vector<std::string>{""});
static_assert(split("one two three four", 9) == std::vector<std::string>{"one two", "three", "four"});
static_assert(split("abcdefghij", 4) == std::vector<std::string>{"abcd", "efgh", "ij"});  // no space: cut at the room
static_assert(split("ab\xc3\xa9\xc3\xa9", 4) == std::vector<std::string>{"ab\xc3\xa9", "\xc3\xa9"});
static_assert(split("ab\xc3\xa9\xc3\xa9", 5) == std::vector<std::string>{"ab\xc3\xa9", "\xc3\xa9"});  // never inside é
static_assert(std::ranges::all_of(pieces(std::string(2000, 'x'), text_room("nick!user@host.example", "PRIVMSG", "#mux")),
                                  [](std::string_view p) { return p.size() <= text_room("nick!user@host.example", "PRIVMSG", "#mux"); }));
static_assert(text_room("n!u@h", "PRIVMSG", "#c") == 512 - (1 + 5 + 1 + 7 + 1 + 2 + 2 + 2));
// Views into the text.
static_assert([] {
  constexpr std::string_view text = "aaa bbb";
  return (*pieces(text, 3).begin()).data() == text.data();
}());
// Five at once, then one each half second.
static_assert([] {
  flood f(0.5, 5);
  for (int i = 0; i < 5; ++i)
    if (!f.take(0.0)) return false;
  if (f.take(0.0) || f.wait(0.0) != 0.5) return false;
  return f.take(0.5) && !f.take(0.5) && f.take(1.0);
}());
}  // namespace

TEST(Outgoing, SplitsWhenRun) { EXPECT_EQ(split("aaaa bbbb", 4).size(), 2u); }

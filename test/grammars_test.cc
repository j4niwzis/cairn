// SPDX-License-Identifier: AGPL-3.0-only
// Every grammar reads every line as the one written by hand does: the same
// line, or the same error.
import std;
import splice;
import cairn;
#if CAIRN_SCAN
import cairn.scan;
#endif
import gtest;

#include "gtest/gtest-macros.h"

namespace {
constexpr std::array cases = {
    "foo bar baz asdf", ":coolguy foo bar baz :asdf quux", ":coolguy foo bar baz :", ":coolguy foo bar baz ::asdf",
    ":coolguy foo bar baz :  asdf quux ", "foo bar baz   asdf", ":src JOIN #chan", ":src JOIN :#chan",
    ":nick!user@host PRIVMSG #c :hi\r\n", ":irc.example.com 001 me :Welcome", "@a=b;c=32;k;rt=ql7 foo",
    "@a=b\\\\and\\nk;c=72\\s45;d=gh\\:764 foo", "@tag1=value\\1 foo", "@tag1=value1\\ foo", "@tag1=1;tag1=5 foo",
    "@+example.com/foo=bar foo", "@time=2026-11-23T18:00:00.000Z :n!u@h PRIVMSG #c :x", "", "   ", "@ foo", ": foo",
    "12 foo", "FO-O bar", "1234 x", "  :a!b@c   privmsg   #x   :  spaced  ", "PING", "CAP * LS * :sasl server-time",
    ":srv 005 me CHANTYPES=# PREFIX=(ov)@+ :are supported", "@k :s 433 * nick :in use",
};

template <class Grammar> bool same_as_by_hand(std::string_view text) {
  const auto want = cairn::try_parse<cairn::grammars::by_hand>(text);
  const auto got = cairn::try_parse<Grammar>(text);
  if (want.has_value() != got.has_value()) return false;
  if (!want) return want.error() == got.error();
  return cairn::to_owned(*want) == cairn::to_owned(*got) && *want == *got;  // the views alike too
}
}  // namespace

TEST(Grammars, StdRegexReadsAsByHand) {
  for (std::string_view one : cases) {
    if (!same_as_by_hand<cairn::grammars::std_regex>(one)) {
      std::println("std_regex differs on [{}]", one);
      ADD_FAILURE();
    }
  }
  // Where by hand reads a line, the regular expressions alone read it too --
  // not the walk they fall back on for the error.
  std::size_t alone = 0;
  for (std::string_view one : cases) {
    if (!cairn::try_parse<cairn::grammars::by_hand>(one)) continue;
    const auto got = cairn::grammars::std_regex::read_alone(one);
    if (!got || cairn::to_owned(*got) != cairn::to_owned(*cairn::try_parse<cairn::grammars::by_hand>(one))) {
      std::println("std_regex alone does not read [{}]", one);
      ADD_FAILURE();
    }
    ++alone;
  }
  std::println("regular expression of the line's command part: {}", cairn::grammar::regex<cairn::grammar::expect<cairn::errors::bad_command,
      cairn::grammar::seq<cairn::grammar::capture<&cairn::grammar::raw::command, cairn::grammar::alt<cairn::grammar::many1<cairn::grammar::alpha>,
      cairn::grammar::repeat<cairn::grammar::digit, 3>>>, cairn::grammar::word_end>>>());
  // Read off an input range through it, a line at a time.
  const std::string wire = "PING :a\r\n:n!u@h PRIVMSG #c :x y\r\n";
  std::vector<std::string> commands;
  for (const auto& one : cairn::lines<cairn::grammars::std_regex>(wire))
    if (one) commands.push_back(one->command);
  if (commands != std::vector<std::string>{"PING", "PRIVMSG"}) ADD_FAILURE();
  try {
    (void)cairn::parse<cairn::grammars::std_regex>("12 foo");
    ADD_FAILURE();
  } catch (const cairn::line_failure& e) {
    if (!spl::holds_alternative<cairn::errors::bad_command>(e.error)) ADD_FAILURE();
  }
}

#if CAIRN_SCAN
// scan's automata, made from the same description: the same lines and
// errors, and every line the walk reads read by the automata alone.
TEST(Grammars, ScanReadsAsByHand) {
  for (std::string_view one : cases) {
    EXPECT_TRUE(same_as_by_hand<cairn::grammars::scan>(one)) << one;
    if (const auto want = cairn::try_parse<cairn::grammars::by_hand>(one)) {
      const auto alone = cairn::grammars::scan::read_alone(one);
      EXPECT_TRUE(alone && *alone == *want) << one;
    }
  }
}
#endif

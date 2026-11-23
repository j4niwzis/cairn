// SPDX-License-Identifier: AGPL-3.0-only
// The ircdocs parser tests (msg-split, CC0), every one; and a property
// test: lines made at random read, written and read again come to the same.
import std;
import splice;
import cairn;
import gtest;

#include "gtest/gtest-macros.h"

namespace {
struct split_case {
  std::string_view input;
  bool has_tags;
  std::vector<std::pair<std::string_view, std::string_view>> tags;
  std::optional<std::string_view> source;
  std::string_view verb;
  std::vector<std::string_view> params;
};
const std::vector<split_case> split_cases = {
#include "parser_cases.inc"
};
std::string source_text(const cairn::source_view& s) {
  std::string out(s.nick);
  if (!s.user.empty()) out += "!" + std::string(s.user);
  if (!s.host.empty()) out += "@" + std::string(s.host);
  return out;
}
}  // namespace

TEST(Parser, EveryIrcdocsSplitCase) {
  for (const auto& k : split_cases) {
    const auto got = cairn::try_parse(k.input);
    ASSERT_TRUE(got.has_value()) << k.input;
    EXPECT_EQ(got->command, k.verb) << k.input;
    EXPECT_EQ(std::ranges::to<std::vector<std::string_view>>(got->params()), k.params) << k.input;
    EXPECT_EQ(got->from.has_value(), k.source.has_value()) << k.input;
    if (got->from && k.source) EXPECT_EQ(source_text(*got->from), *k.source) << k.input;
    // Tags as their last value of a key, unescaped.
    const auto owned = cairn::to_owned(*got);
    EXPECT_EQ(owned.tags.size(), k.tags.size()) << k.input;
    for (const auto& [key, value] : k.tags) {
      const auto* one = owned.tag(key);
      EXPECT_TRUE(one != nullptr) << k.input << " " << key;
      if (one) EXPECT_EQ(one->text, value) << k.input << " " << key;
    }
  }
}

// Lines made at random -- tags with every escape, sources, commands,
// parameters with spaces and colons: read, written back, read again, the
// same line; and the owning way the same.
TEST(Parser, RandomLinesComeBackAsTheyWere) {
  std::uint64_t seed = 0x9e3779b97f4a7c15u;
  const auto next = [&](std::uint64_t n) {
    seed = seed * 6364136223846793005u + 1442695040888963407u;
    return (seed >> 33) % n;
  };
  const std::string_view alphabet = "abcXYZ019-_.[]{}|^:;= \\\x01\xc3\xa9";
  const auto word = [&](bool spaces, bool colon_first) {
    std::string out;
    const std::size_t n = 1 + next(8);
    for (std::size_t i = 0; i < n; ++i) {
      char c = alphabet[next(alphabet.size())];
      if (!spaces && (c == ' ' || c == '\\')) c = 'q';
      if (i == 0 && !colon_first && c == ':') c = 'k';
      out += c;
    }
    return out;
  };
  for (int round = 0; round < 20000; ++round) {
    cairn::line l;
    for (std::size_t i = next(3); i > 0; --i) l.tags.push_back({"k" + std::to_string(i) + (next(2) ? "+" : ""), word(true, true)});
    // keys unique, values with every escape
    for (auto& t : l.tags) std::erase_if(t.key, [](char c) { return c == '+'; });
    if (next(2)) l.from = cairn::source{"n" + std::to_string(next(100)), next(2) ? "u" : "", next(2) ? "h.example" : ""};
    l.command = next(4) ? std::string("PRIVMSG") : std::to_string(100 + next(900));
    const std::size_t params = next(6);
    for (std::size_t i = 0; i < params; ++i) {
      const bool last = i + 1 == params;
      std::string p = word(last, last);
      std::erase_if(p, [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
      if (!last) std::erase_if(p, [](char c) { return c == ' '; });
      if (!last && (p.empty() || p.front() == ':')) p = "x" + p;
      l.params.push_back(p);
    }
    const auto wire = cairn::try_write(l);
    ASSERT_TRUE(wire.has_value()) << round;
    const auto back = cairn::try_parse(*wire);
    ASSERT_TRUE(back.has_value()) << *wire;
    EXPECT_EQ(cairn::to_owned(*back), l) << *wire;
    // A view written as it came, and read again: the same view.
    const auto again = cairn::try_write(*back);
    ASSERT_TRUE(again.has_value()) << *wire;
    EXPECT_EQ(*again, *wire);
  }
}

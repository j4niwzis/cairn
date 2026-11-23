// SPDX-License-Identifier: AGPL-3.0-only
// ircdocs/parser-tests msg-join: a line made of atoms -- tags, source,
// command, parameters -- written as one of the lines the suite allows.
import std;
import cairn;
import gtest;

#include "gtest/gtest-macros.h"

namespace {
struct join_case {
  const char* desc;
  std::vector<std::pair<std::string_view, std::string_view>> tags;
  std::optional<std::string_view> source;
  std::string_view verb;
  std::vector<std::string_view> params;
  std::vector<std::string_view> matches;
};
const std::vector<join_case>& cases() {
  static const std::vector<join_case> all{
#include "join_cases.inc"
  };
  return all;
}
cairn::source source_of(std::string_view s) {
  cairn::source out;
  const auto at = s.find('@');
  const auto bang = s.substr(0, at).find('!');
  out.nick = std::string(s.substr(0, std::min(bang, at)));
  if (bang != std::string_view::npos) out.user = std::string(s.substr(bang + 1, at - bang - 1));
  if (at != std::string_view::npos) out.host = std::string(s.substr(at + 1));
  return out;
}
}  // namespace

TEST(Join, IrcdocsCases) {
  int checked = 0;
  for (const auto& c : cases()) {
    cairn::line l;
    for (const auto& [k, v] : c.tags) l.tags.push_back(cairn::tag{std::string(k), std::string(v)});
    if (c.source) l.from = source_of(*c.source);
    l.command = std::string(c.verb);
    for (auto p : c.params) l.params.emplace_back(p);
    std::string out;
    const auto wrote = cairn::try_write(l, out);
    EXPECT_TRUE(wrote.has_value()) << c.desc;
    // The suite's lines have no CR LF; tag order is the atoms' order.
    while (out.ends_with('\n') || out.ends_with('\r')) out.pop_back();
    EXPECT_TRUE(std::ranges::contains(c.matches, std::string_view(out))) << c.desc << ": " << out;
    ++checked;
  }
  EXPECT_GT(checked, 10);
}

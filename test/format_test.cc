// SPDX-License-Identifier: AGPL-3.0-only
// mIRC formatting read into spans and written back: static_asserts, a run.
import std;
import splice;
import cairn;
import gtest;

#include "gtest/gtest-macros.h"

namespace {
using namespace cairn::format;
constexpr std::vector<span> read_all(std::string_view s) { return std::ranges::to<std::vector<span>>(spans(s)); }
constexpr std::string plain_of(std::string_view s) { return std::ranges::to<std::string>(plain(s)); }
constexpr std::string written(const std::vector<span>& all) {
  std::string out;
  write(all, out);
  return out;
}

static_assert(read_all("plain").size() == 1 && read_all("plain")[0].style == style{});
static_assert(read_all("\x02" "bold\x02 not").size() == 2);
static_assert(read_all("\x02" "bold\x02 not")[0].style.bold && !read_all("\x02" "bold\x02 not")[1].style.bold);
static_assert(read_all("\x03" "04red\x03 none")[0].style.foreground.number == 4);
static_assert(read_all("\x03" "04red\x03 none")[1].style.foreground.none());
static_assert(read_all("\x03" "4,12x")[0].style.background.number == 12);
static_assert(read_all("\x03" "4,x")[0].text == ",x");  // a comma with no number after it: text
static_assert(read_all("\x04" "FF8000orange")[0].style.foreground == colour{-1, 0xFF8000, true});
static_assert(read_all("\x1d\x1f" "both\x0f" "reset")[0].style.italic && read_all("\x1d\x1f" "both\x0f" "reset")[0].style.underline);
static_assert(read_all("\x1d\x1f" "both\x0f" "reset")[1].style == style{});
static_assert(read_all("\x11" "code")[0].style.monospace && read_all("\x1e" "gone")[0].style.strikethrough);
static_assert(read_all("\x02\x02").empty());  // codes alone: no span
static_assert(plain_of("\x02" "a\x03" "04,02b\x0f" "c\x1d" "d") == "abcd");
// Views into the text: nothing copied.
static_assert([] {
  constexpr std::string_view text = "x\x02y";
  return read_all(text)[1].text.data() == text.data() + 2;
}());
// Written back and read again: the same spans (the codes may differ --
// ^O where it turns everything off).
static_assert([] {
  constexpr std::string_view text = "\x02" "bold\x02 \x03" "04,12red\x0f" "plain\x04" "00FF00g";
  return read_all(written(read_all(text))) == read_all(text);
}());
static_assert(written({{style{.bold = true}, "a"}, {style{}, "b"}}) == "\x02" "a\x0f" "b");
static_assert(read_all(written({{style{.bold = true, .italic = true}, "a"}, {style{.italic = true}, "b"}, {style{}, "c"}})) ==
              std::vector<span>{{style{.bold = true, .italic = true}, "a"}, {style{.italic = true}, "b"}, {style{}, "c"}});
}  // namespace

TEST(Format, ReadWhenRun) { EXPECT_EQ(plain_of("\x02hi\x02"), "hi"); }

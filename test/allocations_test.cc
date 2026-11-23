// SPDX-License-Identifier: AGPL-3.0-only
// The non-owning API asks nothing of memory: every allocation made while it
// reads, looks up, walks and writes is counted, and there are none.
import std;
import splice;
import cairn;
#if CAIRN_SCAN
import cairn.scan;
#endif
import gtest;

#include "gtest/gtest-macros.h"

namespace {
bool counting = false;
std::size_t allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
  if (counting) ++allocations;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
// What `f` asked of memory.
template <class F> std::size_t asked(F&& f) {
  allocations = 0;
  counting = true;
  f();
  counting = false;
  return allocations;
}
constexpr std::string_view wire = "@time=2026-11-23T18:00:00.000Z;msgid=a\\sb;+draft/reply=x :nick!user@host PRIVMSG #mux :hello there";
}  // namespace

TEST(Allocations, NoneToReadAViewAndLookItsTagsUp) {
  std::size_t chars = 0;
  EXPECT_EQ(asked([&] {
    const auto v = cairn::try_parse(wire);
    if (!v || !v->is("PRIVMSG") || v->param(1) != "hello there") throw 1;
    for (char c : *v->tag_value("msgid")) chars += c == ' ';
    for (const auto one : v->tags()) chars += one.key.size();
    if (!v->has_tag("+draft/reply") || v->from->host != "host") throw 1;
  }), 0u);
  EXPECT_GT(chars, 0u);
}

TEST(Allocations, NoneToFailToRead) {
  EXPECT_EQ(asked([] {
    if (cairn::try_parse("12 foo") || cairn::try_parse("@ x") || cairn::try_parse("")) throw 1;
  }), 0u);
}

TEST(Allocations, NoneToReadOffAForwardRangeIntoRoom) {
  const std::string text = "PING :a\r\n:n!u@h PRIVMSG #c :x y\r\n\r\nNICK new\r\n";
  std::size_t lines = 0;
  EXPECT_EQ(asked([&] {
    auto chars = text | std::views::filter([](char) { return true; });  // not contiguous
    std::array<char, 600> room;
    auto at = std::ranges::begin(chars);
    const auto first = cairn::try_read(at, std::ranges::end(chars), std::span<char>(room));
    if (!first || !first->is("PING")) throw 1;
    for (const auto& one : cairn::lines(text | std::views::filter([](char) { return true; })))
      if (one) ++lines;
  }), 0u);
  EXPECT_EQ(lines, 3u);
}

TEST(Allocations, NoneToWriteIntoRoomOrEscape) {
  cairn::fixed_text<1024> out;
  EXPECT_EQ(asked([&] {
    const auto v = cairn::try_parse(wire);
    if (!v || !cairn::try_write(*v, out)) throw 1;
    cairn::fixed_text<64> escaped;
    escaped.append_range(cairn::escaped("a b;c"));
    if (escaped.view() != "a\\sb\\:c") throw 1;
  }), 0u);
  EXPECT_EQ(out.view(), std::string(wire) + "\r\n");
}

TEST(Allocations, NoneToFoldNames) {
  EXPECT_EQ(asked([] {
    if (!cairn::equal("#Mux[1]", "#mUX{1}", cairn::casemappings::rfc1459{})) throw 1;
  }), 0u);
}

#if CAIRN_SCAN
TEST(Allocations, NoneToReadWithScan) {
  EXPECT_EQ(asked([] {
    const auto v = cairn::try_parse<cairn::grammars::scan>(wire);
    if (!v || v->param(0) != "#mux") throw 1;
  }), 0u);
}
#endif

// The count sees what is asked: the owning API asks, and is counted.
TEST(Allocations, TheOwningWayIsCounted) {
  EXPECT_GT(asked([] {
    const auto owned = cairn::to_owned(*cairn::try_parse(wire));
    if (owned.params.size() != 2) throw 1;
  }), 0u);
}

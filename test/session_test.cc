// SPDX-License-Identifier: AGPL-3.0-only
// cairn::session over a scripted transport: registered, a request answered
// by labeled-response and by its end numeric. Checked while compiling.
import std;
import splice;
import cairn;
import gtest;

#include "gtest/gtest-macros.h"

namespace {
using namespace cairn;
// What the server says, in turn; what was written, kept.
struct scripted {
  std::string script;
  std::size_t at = 0;
  std::string written;
  struct reader : std::ranges::view_interface<reader> {
    scripted* of;
    struct iterator {
      using value_type = char;
      using difference_type = std::ptrdiff_t;
      scripted* of = nullptr;
      constexpr char operator*() const { return of->script[of->at]; }
      constexpr iterator& operator++() { return ++of->at, *this; }
      constexpr void operator++(int) { ++of->at; }
      constexpr bool operator==(std::default_sentinel_t) const { return of->at >= of->script.size(); }
    };
    constexpr iterator begin() const { return {of}; }
    constexpr std::default_sentinel_t end() const { return {}; }
  };
  reader r{{}, this};
  constexpr reader& input() { return r; }
  constexpr void write(std::string_view s) { written += s; }
  constexpr void flush() {}
};
static_assert(transport<scripted>);

static_assert([] {
  scripted wire{":srv CAP * LS :labeled-response batch\r\n:srv CAP * ACK :labeled-response batch\r\n:srv 001 me :Welcome\r\n"
                "@label=c1 :srv BATCH +b labeled-response\r\n@batch=b :srv 352 me #mux u h srv alice H :0 Alice\r\n"
                "@batch=b :srv 315 me #mux :End of WHO\r\n:srv BATCH -b\r\n"};
  std::array<char, 512> room{};
  session s(wire, registration{.nick = "me", .caps = {"labeled-response", "batch"}}, room);
  int events = 0;
  const auto on = [&](const event_t&) { ++events; };
  if (!s.connect(on) || !wire.written.starts_with("CAP LS 302\r\nNICK me\r\n")) return false;
  if (!wire.written.ends_with("CAP REQ :labeled-response batch\r\nCAP END\r\n")) return false;
  const auto got = s.request(msg::who{"#mux", {}}, "WHO", on);
  return got && got->lines.size() == 2 && try_parse(got->lines[0])->number() == 352 &&
         wire.written.ends_with("@label=c1 WHO #mux\r\n");
}());

// No labeled-response: the answer ends with the reply that ends it.
static_assert([] {
  scripted wire{":srv CAP * LS :\r\n:srv 001 me :Welcome\r\n:srv NOTICE me :unrelated\r\n"
                ":srv 301 me bob :away\r\n:srv 353 me = #mux :@alice bob\r\n:srv 366 me #mux :End of NAMES\r\n"};
  std::array<char, 512> room{};
  session s(wire, registration{.nick = "me", .caps = {}}, room);
  int notices = 0;
  const auto on = [&](const event_t& e) {
    if (const auto* r = e.template get_if<events::received>(); r && spl::holds_alternative<msg::notice>(r->what.what)) ++notices;
  };
  if (!s.connect(on)) return false;
  const auto got = s.request(msg::names{commas{"#mux"}}, "NAMES", on);
  return got && got->lines.size() == 2 && notices == 1;  // the 301 not its: handed on, not taken
}());

// An error ends a request too.
static_assert([] {
  scripted wire{":srv CAP * LS :\r\n:srv 001 me :Welcome\r\n:srv 401 me nobody :No such nick\r\n"};
  std::array<char, 512> room{};
  session s(wire, registration{.nick = "me", .caps = {}}, room);
  const auto on = [](const event_t&) {};
  if (!s.connect(on)) return false;
  const auto got = s.request(msg::whois{params_rest{"nobody"}}, "WHOIS", on);
  return got && got->lines.size() == 1 && try_parse(got->lines[0])->number() == 401;
}());
// CHATHISTORY without labeled-response: answered by its batch.
static_assert([] {
  scripted wire{":srv CAP * LS :batch\r\n:srv CAP * ACK :batch\r\n:srv 001 me :Welcome\r\n"
                ":srv NOTICE me :before\r\n:srv BATCH +h1 chathistory #mux\r\n@batch=h1 :a!u@h PRIVMSG #mux :one\r\n"
                "@batch=h1 :b!u@h PRIVMSG #mux :two\r\n:srv BATCH -h1\r\n"};
  std::array<char, 512> room{};
  session s(wire, registration{.nick = "me", .caps = {"batch"}}, room);
  int notices = 0;
  const auto on = [&](const event_t& e) {
    if (const auto* r = e.template get_if<events::received>(); r && r->what.what.template get_if<msg::notice>()) ++notices;
  };
  if (!s.connect(on)) return false;
  const auto got = s.request(msg::chathistory{msg::history::latest{"#mux", msg::history::none{}, 50}}, "CHATHISTORY", on);
  return got && got->lines.size() == 2 && notices == 1 && try_parse(got->lines[1])->param(1) == "two";
}());
}  // namespace

TEST(Session, ConnectsWhenRun) {
  scripted wire{":srv CAP * LS :\r\n:srv 001 me :Welcome\r\n"};
  std::array<char, 512> room{};
  cairn::session s(wire, cairn::registration{.nick = "me", .caps = {}}, room);
  EXPECT_TRUE(s.connect([](const cairn::event_t&) {}));
}

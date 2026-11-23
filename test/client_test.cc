// SPDX-License-Identifier: AGPL-3.0-only
// cairn::client over scripted conversations: what it sends, what it says
// happened. Checked while compiling, and run.
import std;
import splice;
import cairn;
import gtest;

#include "gtest/gtest-macros.h"

namespace {
using namespace cairn;

constexpr std::vector<std::string> sent(const std::vector<line>& lines) {
  std::vector<std::string> out;
  for (const auto& one : lines) out.push_back(*try_write(one));
  return out;
}
constexpr reaction from(client& c, std::string_view text) { return c.feed(to_owned(*try_parse(text))); }
template <class E> constexpr bool happened(const reaction& r) {
  return std::ranges::any_of(r.events, [](const event_t& e) { return spl::holds_alternative<E>(e); });
}

// Registration with SASL PLAIN, capabilities negotiated, a nick in use.
static_assert([] {
  client c(registration{.nick = "alice", .other_nicks = {"alice2"}, .username = "al", .realname = "Alice A",
                        .sasl = sasl::plain{"alice", "secret"}, .caps = {"server-time", "batch"}});
  if (sent(c.start()) != std::vector<std::string>{"CAP LS 302\r\n", "NICK alice\r\n", "USER al 0 * :Alice A\r\n"}) return false;
  // The capabilities in two lines; asked for what it wants and the server has.
  if (!from(c, ":srv CAP * LS * :multi-prefix sasl=PLAIN,EXTERNAL").send.empty()) return false;
  if (sent(from(c, ":srv CAP * LS :server-time batch draft/foo").send) != std::vector<std::string>{"CAP REQ :sasl server-time batch\r\n"}) return false;
  const auto ack = from(c, ":srv CAP * ACK :sasl server-time batch");
  if (!happened<events::caps_enabled>(ack) || sent(ack.send) != std::vector<std::string>{"AUTHENTICATE PLAIN\r\n"}) return false;
  // \0alice\0alice\0secret, base64.
  if (sent(from(c, "AUTHENTICATE +").send) != std::vector<std::string>{"AUTHENTICATE YWxpY2UAYWxpY2UAc2VjcmV0\r\n"}) return false;
  if (!happened<events::logged_in>(from(c, ":srv 900 alice alice!al@h alice :You are now logged in as alice"))) return false;
  if (sent(from(c, ":srv 903 alice :SASL authentication successful").send) != std::vector<std::string>{"CAP END\r\n"}) return false;
  // The nick taken: the next.
  const auto taken = from(c, ":srv 433 * alice :Nickname is already in use");
  if (!happened<events::nick_taken>(taken) || sent(taken.send) != std::vector<std::string>{"NICK alice2\r\n"}) return false;
  if (!happened<events::registered>(from(c, ":srv 001 alice2 :Welcome")) || c.nick() != "alice2" || !c.registered()) return false;
  from(c, ":srv 005 alice2 CASEMAPPING=ascii NETWORK=Example :are supported");
  return c.server().network == "Example" && c.has_cap("batch") && c.has_cap("sasl");
}());

// No capability it wants: CAP END at once. PING answered.
static_assert([] {
  client c(registration{.nick = "bob", .caps = {"server-time"}});
  c.start();
  if (sent(from(c, ":srv CAP * LS :multi-prefix").send) != std::vector<std::string>{"CAP END\r\n"}) return false;
  return sent(from(c, "PING :token123").send) == std::vector<std::string>{"PONG token123\r\n"};
}());

// SASL failing: registration goes on without it.
static_assert([] {
  client c(registration{.nick = "carol", .sasl = sasl::plain{"carol", "wrong"}, .caps = {}});
  c.start();
  from(c, ":srv CAP * LS :sasl");
  from(c, ":srv CAP * ACK :sasl");
  from(c, "AUTHENTICATE +");
  const auto failed = from(c, ":srv 904 carol :SASL authentication failed");
  return happened<events::sasl_failed>(failed) && sent(failed.send) == std::vector<std::string>{"CAP END\r\n"};
}());

// A batch gathered: its messages handed out together at its end.
static_assert([] {
  client c(registration{.nick = "dave", .caps = {}});
  c.start();
  if (!from(c, ":srv BATCH +b1 chathistory #mux").events.empty()) return false;
  if (!from(c, "@batch=b1 :x!x@h PRIVMSG #mux :one").events.empty()) return false;
  if (!from(c, "@batch=b1 :y!y@h PRIVMSG #mux :two").events.empty()) return false;
  const auto done = from(c, ":srv BATCH -b1");
  if (done.events.size() != 1 || !spl::holds_alternative<events::batch>(done.events[0])) return false;
  const auto& b = spl::get<events::batch>(done.events[0]);
  return b.type == "chathistory" && b.params == std::vector<std::string>{"#mux"} && b.messages.size() == 2;
}());

// Capabilities offered and withdrawn after registration (cap-notify).
static_assert([] {
  client c(registration{.nick = "erin", .caps = {"away-notify"}});
  c.start();
  from(c, ":srv CAP * LS :cap-notify");
  from(c, ":srv 001 erin :Welcome");
  const auto offered = from(c, ":srv CAP erin NEW :away-notify");
  if (!happened<events::caps_offered>(offered) || sent(offered.send) != std::vector<std::string>{"CAP REQ away-notify\r\n"}) return false;
  from(c, ":srv CAP erin ACK :away-notify");
  if (!c.has_cap("away-notify")) return false;
  return happened<events::caps_withdrawn>(from(c, ":srv CAP erin DEL :away-notify")) && !c.has_cap("away-notify");
}());
}  // namespace

TEST(Client, RegistersWhenRun) {
  client c(registration{.nick = "frank", .caps = {}});
  EXPECT_EQ(c.start().size(), 3u);
  from(c, ":srv CAP * LS :");
  from(c, ":srv 001 frank :Welcome");
  EXPECT_TRUE(c.registered());
}

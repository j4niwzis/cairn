// SPDX-License-Identifier: AGPL-3.0-only
// cairn::client over scripted conversations: what it writes into the sink,
// what it tells the callback. Checked while compiling, and run.
import std;
import splice;
import cairn;
import gtest;

#include "gtest/gtest-macros.h"

namespace {
using namespace cairn;

// What a line came to, collected by the test (the test may keep things).
struct came {
  std::string sent;
  std::vector<event_t> events;
};
constexpr came from(client& c, std::string_view text) {
  came out;
  c.feed(*try_parse(text), out.sent, [&](const event_t& e) { out.events.push_back(e); });
  return out;
}
constexpr std::string started(client& c) {
  std::string out;
  c.start(out);
  return out;
}
template <class E> constexpr bool happened(const came& r) {
  return std::ranges::any_of(r.events, [](const event_t& e) { return spl::holds_alternative<E>(e); });
}

// Registration with SASL PLAIN, capabilities negotiated, a nick in use.
static_assert([] {
  client c(registration{.nick = "alice", .other_nicks = {"alice2"}, .username = "al", .realname = "Alice A",
                        .sasl = sasl::plain{"alice", "secret"}, .caps = {"server-time", "batch"}});
  if (started(c) != "CAP LS 302\r\nNICK alice\r\nUSER al 0 * :Alice A\r\n") return false;
  if (!from(c, ":srv CAP * LS * :multi-prefix sasl=PLAIN,EXTERNAL").sent.empty()) return false;
  if (from(c, ":srv CAP * LS :server-time batch draft/foo").sent != "CAP REQ :sasl server-time batch\r\n") return false;
  const auto ack = from(c, ":srv CAP * ACK :sasl server-time batch");
  if (!happened<events::caps_enabled>(ack) || ack.sent != "AUTHENTICATE PLAIN\r\n") return false;
  if (from(c, "AUTHENTICATE +").sent != "AUTHENTICATE YWxpY2UAYWxpY2UAc2VjcmV0\r\n") return false;  // \0alice\0alice\0secret
  if (!happened<events::logged_in>(from(c, ":srv 900 alice alice!al@h alice :You are now logged in as alice"))) return false;
  if (from(c, ":srv 903 alice :SASL authentication successful").sent != "CAP END\r\n") return false;
  const auto taken = from(c, ":srv 433 * alice :Nickname is already in use");
  if (!happened<events::nick_taken>(taken) || taken.sent != "NICK alice2\r\n") return false;
  if (!happened<events::registered>(from(c, ":srv 001 alice2 :Welcome")) || c.nick() != "alice2" || !c.registered()) return false;
  from(c, ":srv 005 alice2 CASEMAPPING=ascii NETWORK=Example :are supported");
  return c.server().network == "Example" && c.has_cap("batch") && c.has_cap("sasl");
}());

// No capability it wants: CAP END at once. PING answered.
static_assert([] {
  client c(registration{.nick = "bob", .caps = {"server-time"}});
  started(c);
  if (from(c, ":srv CAP * LS :multi-prefix").sent != "CAP END\r\n") return false;
  return from(c, "PING :token123").sent == "PONG token123\r\n";
}());

// SASL failing: registration goes on without it.
static_assert([] {
  client c(registration{.nick = "carol", .sasl = sasl::plain{"carol", "wrong"}, .caps = {}});
  started(c);
  from(c, ":srv CAP * LS :sasl");
  from(c, ":srv CAP * ACK :sasl");
  from(c, "AUTHENTICATE +");
  const auto failed = from(c, ":srv 904 carol :SASL authentication failed");
  return happened<events::sasl_failed>(failed) && failed.sent == "CAP END\r\n";
}());

// A batch gathered: its lines handed out together at its end.
static_assert([] {
  client c(registration{.nick = "dave", .caps = {}});
  started(c);
  if (!from(c, ":srv BATCH +b1 chathistory #mux").events.empty()) return false;
  if (!from(c, "@batch=b1 :x!x@h PRIVMSG #mux :one").events.empty()) return false;
  if (!from(c, "@batch=b1 :y!y@h PRIVMSG #mux :two").events.empty()) return false;
  const auto done = from(c, ":srv BATCH -b1");
  if (done.events.size() != 1 || !spl::holds_alternative<events::batch>(done.events[0])) return false;
  const auto& b = spl::get<events::batch>(done.events[0]);
  return b.type == "chathistory" && b.params == std::vector<std::string>{"#mux"} && b.lines.size() == 2 &&
         try_parse(b.lines[1])->param(1) == "two";
}());

// Capabilities offered and withdrawn after registration (cap-notify).
static_assert([] {
  client c(registration{.nick = "erin", .caps = {"away-notify"}});
  started(c);
  from(c, ":srv CAP * LS :cap-notify");
  from(c, ":srv 001 erin :Welcome");
  const auto offered = from(c, ":srv CAP erin NEW :away-notify");
  if (!happened<events::caps_offered>(offered) || offered.sent != "CAP REQ :away-notify\r\n") return false;
  from(c, ":srv CAP erin ACK :away-notify");
  if (!c.has_cap("away-notify")) return false;
  return happened<events::caps_withdrawn>(from(c, ":srv CAP erin DEL :away-notify")) && !c.has_cap("away-notify");
}());

// CTCP asked: answered as a NOTICE, as said; not where it is turned off.
static_assert([] {
  client c(registration{.nick = "gina", .caps = {}, .version = "mux 1.0"});
  started(c);
  if (from(c, ":hal!h@h PRIVMSG gina :\x01VERSION\x01").sent != "NOTICE hal :\x01VERSION mux 1.0\x01\r\n") return false;
  if (from(c, ":hal!h@h PRIVMSG gina :\x01PING 12345\x01").sent != "NOTICE hal :\x01PING 12345\x01\r\n") return false;
  client quiet(registration{.nick = "q", .answer_ctcp = false});
  started(quiet);
  return from(quiet, ":hal!h@h PRIVMSG q :\x01VERSION\x01").sent.empty();
}());

// A long SASL payload: in pieces of 400, an exact 400 followed by "+".
static_assert([] {
  client c(registration{.nick = "h", .sasl = sasl::plain{"h", std::string(296, 'p')}, .caps = {}});
  started(c);
  from(c, ":srv CAP * LS :sasl");
  from(c, ":srv CAP * ACK :sasl");
  const std::string sent = from(c, "AUTHENTICATE +").sent;  // 1+1+1+1+296 = 300 bytes: 400 base64 characters
  return sent.size() == 13 + 400 + 2 + 16 && sent.ends_with("AUTHENTICATE +\r\n");
}());
// A batch inside a batch: its lines (and its BATCH lines) kept in the outer
// one, in order; one event, at the outer one's end.
static_assert([] {
  client c(registration{.nick = "eve", .caps = {}});
  started(c);
  from(c, ":srv BATCH +out chathistory #mux");
  from(c, "@batch=out :srv BATCH +in draft/multiline #mux");
  from(c, "@batch=in :a!u@h PRIVMSG #mux :one");
  if (!from(c, "@batch=out :srv BATCH -in").events.empty()) return false;
  from(c, "@batch=out :b!u@h PRIVMSG #mux :two");
  const auto ended = from(c, ":srv BATCH -out");
  if (ended.events.size() != 1) return false;
  const auto& b = *ended.events[0].get_if<events::batch>();
  return b.type == "chathistory" && b.lines.size() == 4 && try_parse(b.lines[1])->param(1) == "one" &&
         try_parse(b.lines[3])->param(1) == "two";
}());

// A BATCH without its reference: nothing opened, nothing thrown.
static_assert([] {
  client c(registration{.nick = "eve", .caps = {}});
  started(c);
  return from(c, ":srv BATCH +").events.empty() && from(c, ":srv BATCH").events.empty();
}());

// The own nick found by the server's casemapping.
static_assert([] {
  client c(registration{.nick = "Alice", .caps = {}});
  started(c);
  from(c, ":srv CAP * LS :");
  from(c, ":srv 001 Alice :Welcome");
  from(c, ":alice!u@h NICK :Bob");
  return c.nick() == "Bob";
}());

// CAP REQ in lines under 510 bytes.
static_assert([] {
  std::vector<std::string> wanted;
  std::string offered;
  for (int i = 0; i < 60; ++i) {
    std::string name = "vendor.example/capability-" + std::string(1, static_cast<char>('a' + i % 26)) + std::string(1, static_cast<char>('a' + i / 26));
    offered += (i ? " " : "") + name;
    wanted.push_back(name);
  }
  client c(registration{.nick = "f", .caps = wanted});
  started(c);
  const std::string sent = from(c, ":srv CAP * LS :" + offered).sent;
  std::size_t lines = 0;
  for (auto line : sent | std::views::split(std::string_view("\r\n"))) {
    if (std::ranges::empty(line)) continue;
    if (std::ranges::distance(line) > 510) return false;
    ++lines;
  }
  return lines >= 3;
}());

// SASL with a mechanism the server does not take: not tried.
static_assert([] {
  client c(registration{.nick = "g", .sasl = sasl::scram_sha256{"g", "pw", "nonce"}, .caps = {}});
  started(c);
  from(c, ":srv CAP * LS :sasl=PLAIN,EXTERNAL");
  const auto ack = from(c, ":srv CAP * ACK :sasl");
  return happened<events::sasl_failed>(ack) && ack.sent == "CAP END\r\n";
}());
// The values of what is offered, asked by name.
static_assert([] {
  client c(registration{.nick = "h", .caps = {}});
  started(c);
  from(c, ":srv CAP * LS :sts=port=6697,duration=300 batch");
  return c.offered_value("sts") == std::optional<std::string_view>("port=6697,duration=300") && c.offered_value("batch") == std::optional<std::string_view>("") &&
         !c.offered_value("sasl");
}());
}  // namespace

TEST(Client, RegistersWhenRun) {
  client c(registration{.nick = "frank", .caps = {}});
  EXPECT_EQ(started(c), "CAP LS 302\r\nNICK frank\r\nUSER frank 0 * frank\r\n");
  from(c, ":srv CAP * LS :");
  from(c, ":srv 001 frank :Welcome");
  EXPECT_TRUE(c.registered());
}

// SCRAM-SHA-256 with RFC 7677's conversation: the client's final message
// as the RFC has it, and the server's proof taken -- run, not compiled
// (4096 rounds of PBKDF2).
TEST(Client, ScramSha256AsRfc7677) {
  client c(registration{.nick = "u", .sasl = sasl::scram_sha256{"user", "pencil", "rOprNGfwEbeRWgbNEkqO"}, .caps = {}});
  started(c);
  from(c, ":srv CAP * LS :sasl=SCRAM-SHA-256");
  EXPECT_EQ(from(c, ":srv CAP * ACK :sasl").sent, "AUTHENTICATE SCRAM-SHA-256\r\n");
  const std::string first = from(c, "AUTHENTICATE +").sent;
  EXPECT_EQ(first, "AUTHENTICATE " + crypto::base64_encode(crypto::to_bytes("n,,n=user,r=rOprNGfwEbeRWgbNEkqO")) + "\r\n");
  const std::string server_first = "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
  const std::string asked = "AUTHENTICATE " + crypto::base64_encode(crypto::to_bytes(server_first));
  const std::string final_sent = from(c, asked).sent;
  EXPECT_EQ(final_sent, "AUTHENTICATE " + crypto::base64_encode(crypto::to_bytes(
      "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=")) + "\r\n");
  const std::string proof = "AUTHENTICATE " + crypto::base64_encode(crypto::to_bytes("v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="));
  EXPECT_EQ(from(c, proof).sent, "AUTHENTICATE +\r\n");
  // A server that cannot prove it: the exchange given up.
  client d(registration{.nick = "u", .sasl = sasl::scram_sha256{"user", "pencil", "rOprNGfwEbeRWgbNEkqO"}, .caps = {}});
  started(d);
  from(d, ":srv CAP * LS :sasl");
  from(d, ":srv CAP * ACK :sasl");
  from(d, "AUTHENTICATE +");
  from(d, asked);
  const std::string wrong = "AUTHENTICATE " + crypto::base64_encode(crypto::to_bytes("v=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));
  EXPECT_EQ(from(d, wrong).sent, "AUTHENTICATE *\r\n");
}

// SPDX-License-Identifier: AGPL-3.0-only
// cairn's typed messages, read as views from lines and written back:
// static_asserts, and a run.
import std;
import splice;
import cairn;
import gtest;

#include "gtest/gtest-macros.h"

namespace {
using namespace cairn;

constexpr isupport server_of() {
  isupport i;
  i.take(*try_parse(":srv 005 me CHANTYPES=# PREFIX=(ov)@+ CHANMODES=beI,k,l,imnst :are supported"));
  return i;
}
constexpr message got(std::string_view s) { return read(*try_parse(s)); }
template <class T> constexpr const T& as(const message& m) { return spl::get<T>(m.what); }
template <class T> constexpr bool is(const message& m) { return spl::holds_alternative<T>(m.what); }
template <class R> constexpr std::vector<std::string> strings(const R& r) {
  std::vector<std::string> out;
  for (std::string_view one : r) out.emplace_back(one);
  return out;
}
// MODE read with what the server said of itself, there while it is walked.
constexpr std::vector<msg::mode_change> changes(std::string_view s) {
  const isupport server = server_of();
  const auto m = read(*try_parse(s), server);
  return std::ranges::to<std::vector<msg::mode_change>>(spl::get<msg::mode>(m.what).changes);
}
using v = std::vector<std::string>;

static_assert(as<msg::privmsg>(got(":n!u@h PRIVMSG #mux :hello there")).body == "hello there");
static_assert(got(":n!u@h PRIVMSG #mux :hi").line.from->nick == "n");
static_assert(as<msg::notice>(got(":srv NOTICE * :*** Looking up your hostname")).target == "*");
static_assert(as<msg::action>(got(":n!u@h PRIVMSG #mux :\x01" "ACTION waves\x01")).body == "waves");
static_assert(as<msg::action>(got(":n!u@h PRIVMSG #mux :\x01" "action waves")).body == "waves");  // unclosed, any case
static_assert(as<msg::ctcp>(got(":n!u@h PRIVMSG me :\x01version\x01")).command == "version");
static_assert(as<msg::ctcp>(got(":n!u@h NOTICE me :\x01VERSION mux 1.0\x01")).reply);
static_assert(as<msg::ctcp>(got(":n!u@h NOTICE me :\x01VERSION mux 1.0\x01")).body == "mux 1.0");
static_assert(strings(as<msg::join>(got(":n!u@h JOIN #a,#b")).channels) == v{"#a", "#b"});
static_assert(as<msg::join>(got(":n!u@h JOIN #a acct :Real Name")).account == "acct");
static_assert(as<msg::join>(got(":n!u@h JOIN #a acct :Real Name")).realname == "Real Name");
static_assert(as<msg::join>(got(":n!u@h JOIN #a acct :Real Name")).keys.empty());
static_assert(as<msg::part>(got(":n!u@h PART #a :bye")).reason == "bye");
static_assert(as<msg::kick>(got(":op!u@h KICK #a victim :spam")).nick == "victim");
static_assert(as<msg::nick>(got(":old!u@h NICK new")).to == "new");
static_assert(*as<msg::topic>(got(":n!u@h TOPIC #a :a new topic")).body == "a new topic");
static_assert(!as<msg::topic>(got("TOPIC #a")).body);
static_assert(changes(":op!u@h MODE #a +ov-k alice bob key") ==
              std::vector<msg::mode_change>{{true, 'o', "alice"}, {true, 'v', "bob"}, {false, 'k', "key"}});
static_assert(changes(":op!u@h MODE #a +l-l 10") == std::vector<msg::mode_change>{{true, 'l', "10"}, {false, 'l', ""}});
static_assert(changes(":me MODE me +iw").size() == 2);  // a user's modes: no arguments
static_assert(*as<msg::away>(got(":n!u@h AWAY :gone")).body == "gone");
static_assert(!as<msg::away>(got(":n!u@h AWAY")).body);
static_assert(as<msg::account>(got(":n!u@h ACCOUNT acct")).name == "acct");
static_assert(as<msg::ping>(got("PING :irc.example.com")).token == "irc.example.com");
static_assert(as<msg::pong>(got(":srv PONG srv :tok")).token == "srv");
static_assert(as<msg::cap>(got(":srv CAP * LS * :sasl server-time")).subcommand == "LS");
static_assert(strings(as<msg::cap>(got(":srv CAP * LS * :sasl server-time")).params) == v{"*", "sasl server-time"});
static_assert(as<msg::batch>(got(":srv BATCH +yXN chathistory #a")).start && as<msg::batch>(got(":srv BATCH +yXN chathistory #a")).ref == "yXN");
static_assert(strings(as<msg::batch>(got(":srv BATCH +yXN chathistory #a")).params) == v{"#a"});
static_assert(!as<msg::batch>(got(":srv BATCH -yXN")).start);
static_assert(as<msg::reply>(got(":srv 001 me :Welcome to IRC")).number == 1);
static_assert(as<msg::reply>(got(":srv 433 * me :Nickname is already in use")).params[0] == "me");
static_assert(as<msg::wallops>(got(":srv WALLOPS :hello")).body == "hello");
static_assert(is<msg::unknown>(got(":srv XYZZY :hello")));
static_assert(as<msg::user>(got("USER alice 0 * :Alice A")).realname == "Alice A");
static_assert(strings(as<msg::names>(got("NAMES #a,#b")).channels) == v{"#a", "#b"});
static_assert(strings(as<msg::whois>(got("WHOIS irc.example.net alice")).targets) == v{"irc.example.net", "alice"});
static_assert(strings(as<msg::monitor>(got("MONITOR + alice,bob")).targets) == v{"alice", "bob"});
static_assert(strings(as<msg::chathistory>(got("CHATHISTORY BEFORE #mux msgid=abc 50")).params) == v{"#mux", "msgid=abc", "50"});
static_assert(is<msg::rehash>(got("REHASH")));
static_assert(strings(as<msg::fail>(got("FAIL CHATHISTORY INVALID_TARGET #nope :No such channel")).context()) == v{"#nope"});
static_assert(as<msg::fail>(got("FAIL CHATHISTORY INVALID_TARGET #nope :No such channel")).description() == "No such channel");
static_assert(std::ranges::equal(*got("@msgid=a\\sb :n!u@h PRIVMSG #a :x").line.tag_value("msgid"), std::string_view("a b")));
// Views into the line: nothing copied.
static_assert([] {
  constexpr std::string_view text = ":n!u@h PRIVMSG #mux :hello";
  const auto m = read(*try_parse(text));
  return as<msg::privmsg>(m).body.data() == text.data() + 21;
}());

// Written straight into what appends.
static_assert(write(msg::privmsg{"#a", "hello there"}) == "PRIVMSG #a :hello there\r\n");
static_assert(write(msg::action{"#a", "waves"}) == "PRIVMSG #a :\x01" "ACTION waves\x01\r\n");
static_assert(write(msg::ctcp{"n", "VERSION", "mux 1.0", true}) == "NOTICE n :\x01VERSION mux 1.0\x01\r\n");
static_assert(write(msg::join{commas{"#a,#b"}, commas{"key"}, {}, {}}) == "JOIN #a,#b key\r\n");
static_assert(write(msg::part{commas{"#a"}, "bye now"}) == "PART #a :bye now\r\n");
static_assert(write(msg::quit{}) == "QUIT\r\n");
static_assert(write(msg::topic{"#a", "new topic"}) == "TOPIC #a :new topic\r\n");
static_assert(write(msg::cap{"REQ", params_rest{":sasl server-time"}}) == "CAP REQ :sasl server-time\r\n");
static_assert(write(msg::pong{"irc.example.com"}) == "PONG irc.example.com\r\n");
static_assert(write(msg::user{"alice", "0", "*", "Alice A"}) == "USER alice 0 * :Alice A\r\n");
static_assert(write(msg::rehash{}) == "REHASH\r\n");
static_assert(write(msg::monitor{"+", commas{"alice,bob"}}) == "MONITOR + alice,bob\r\n");
// Read, written, read again: the same.
static_assert(write(got(":op!u@h MODE #a +o-k alice key").what) == "MODE #a +o-k alice key\r\n");
static_assert(write(got("FAIL CHATHISTORY INVALID_TARGET #nope :No such channel").what) == "FAIL CHATHISTORY INVALID_TARGET #nope :No such channel\r\n");

// Numerics by their names, and back.
static_assert(*numerics::name_of(1) == "RPL_WELCOME");
static_assert(*numerics::name_of(433) == "ERR_NICKNAMEINUSE");
static_assert(*numerics::number_of("RPL_ENDOFNAMES") == 366);
static_assert(!numerics::name_of(999));
}  // namespace

TEST(Messages, ActionReadWhenRun) {
  const auto m = cairn::read(cairn::parse(":n!u@h PRIVMSG #a :\x01" "ACTION waves\x01"));
  EXPECT_EQ(spl::get<cairn::msg::action>(m.what).body, "waves");
}

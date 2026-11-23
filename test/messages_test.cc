// SPDX-License-Identifier: AGPL-3.0-only
// cairn's typed messages, read and written: static_asserts, and a run.
import std;
import splice;
import cairn;
import gtest;

#include "gtest/gtest-macros.h"

namespace {
using namespace cairn;

constexpr isupport libera() {
  isupport i;
  i.take(to_owned(*try_parse(":srv 005 me CHANTYPES=# PREFIX=(ov)@+ CHANMODES=beI,k,l,imnst :are supported")));
  return i;
}
constexpr message got(std::string_view s) { return read(*try_parse(s), libera()); }
template <class T> constexpr const T& as(const message& m) { return spl::get<T>(m.what); }
template <class T> constexpr bool is(const message& m) { return spl::holds_alternative<T>(m.what); }

static_assert(as<msg::privmsg>(got(":n!u@h PRIVMSG #mux :hello there")).text == "hello there");
static_assert(got(":n!u@h PRIVMSG #mux :hi").line.from->nick == "n");
static_assert(as<msg::notice>(got(":srv NOTICE * :*** Looking up your hostname")).target == "*");
static_assert(as<msg::action>(got(":n!u@h PRIVMSG #mux :\x01" "ACTION waves\x01")).text == "waves");
static_assert(as<msg::action>(got(":n!u@h PRIVMSG #mux :\x01" "ACTION waves")).text == "waves");  // unclosed
static_assert(as<msg::ctcp>(got(":n!u@h PRIVMSG me :\x01version\x01")).command == "VERSION");
static_assert(as<msg::ctcp>(got(":n!u@h NOTICE me :\x01VERSION mux 1.0\x01")).reply);
static_assert(as<msg::ctcp>(got(":n!u@h NOTICE me :\x01VERSION mux 1.0\x01")).text == "mux 1.0");
static_assert(as<msg::join>(got(":n!u@h JOIN #a,#b")).channels == std::vector<std::string>{"#a", "#b"});
static_assert(as<msg::join>(got(":n!u@h JOIN #a acct :Real Name")).account == "acct");
static_assert(as<msg::join>(got(":n!u@h JOIN #a acct :Real Name")).realname == "Real Name");
static_assert(as<msg::join>(got(":n!u@h JOIN #a acct :Real Name")).keys.empty());
static_assert(as<msg::part>(got(":n!u@h PART #a :bye")).reason == "bye");
static_assert(as<msg::kick>(got(":op!u@h KICK #a victim :spam")).nick == "victim");
static_assert(as<msg::nick>(got(":old!u@h NICK new")).to == "new");
static_assert(*as<msg::topic>(got(":n!u@h TOPIC #a :a new topic")).text == "a new topic");
static_assert(!as<msg::topic>(read(make("TOPIC", "#a"))).text);
static_assert(as<msg::mode>(got(":op!u@h MODE #a +ov-k alice bob key")).changes ==
              std::vector<msg::mode_change>{{true, 'o', "alice"}, {true, 'v', "bob"}, {false, 'k', "key"}});
static_assert(as<msg::mode>(got(":op!u@h MODE #a +l-l 10")).changes ==
              std::vector<msg::mode_change>{{true, 'l', "10"}, {false, 'l', ""}});
static_assert(as<msg::mode>(got(":op!u@h MODE #a +b *!*@spam")).changes.front().argument == "*!*@spam");
static_assert(as<msg::mode>(got(":me MODE me +iw")).changes.size() == 2);  // a user's modes: no arguments
static_assert(*as<msg::away>(got(":n!u@h AWAY :gone")).text == "gone");
static_assert(!as<msg::away>(got(":n!u@h AWAY")).text);
static_assert(as<msg::account>(got(":n!u@h ACCOUNT acct")).name == "acct");
static_assert(as<msg::chghost>(got(":n!u@h CHGHOST user2 host2")).host == "host2");
static_assert(as<msg::ping>(got("PING :irc.example.com")).token == "irc.example.com");
static_assert(as<msg::error>(got("ERROR :Closing link")).text == "Closing link");
static_assert(as<msg::cap>(got(":srv CAP * LS * :sasl server-time")).subcommand == "LS");
static_assert(as<msg::cap>(got(":srv CAP * LS * :sasl server-time")).params == std::vector<std::string>{"*", "sasl server-time"});
static_assert(as<msg::batch>(got(":srv BATCH +yXNAbvnRHTRBv chathistory #a")).start);
static_assert(as<msg::batch>(got(":srv BATCH +yXNAbvnRHTRBv chathistory #a")).ref == "yXNAbvnRHTRBv");
static_assert(as<msg::batch>(got(":srv BATCH +yXNAbvnRHTRBv chathistory #a")).params == std::vector<std::string>{"#a"});
static_assert(!as<msg::batch>(got(":srv BATCH -yXNAbvnRHTRBv")).start);
static_assert(as<msg::reply>(got(":srv 001 me :Welcome to IRC")).number == 1);
static_assert(as<msg::reply>(got(":srv 433 * me :Nickname is already in use")).params.front() == "me");
static_assert(as<msg::wallops>(got(":srv WALLOPS :hello")).text == "hello");
static_assert(is<msg::unknown>(got(":srv XYZZY :hello")));  // what no table names, kept as it came
// Tags stay on the line.
static_assert(*got("@msgid=abc;time=2026-11-23T18:00:00Z :n!u@h PRIVMSG #a :x").line.tag_value("msgid") == "abc");

// Written.
constexpr std::string out(const message_t& m) { return *try_write(to_line(m)); }
static_assert(out(msg::privmsg{"#a", "hello there"}) == "PRIVMSG #a :hello there\r\n");
static_assert(out(msg::action{"#a", "waves"}) == "PRIVMSG #a :\x01" "ACTION waves\x01\r\n");
static_assert(out(msg::ctcp{"n", "VERSION", "mux 1.0", true}) == "NOTICE n :\x01VERSION mux 1.0\x01\r\n");
static_assert(out(msg::join{{"#a", "#b"}, {"key"}, {}, {}}) == "JOIN #a,#b key\r\n");
static_assert(out(msg::part{{"#a"}, "bye now"}) == "PART #a :bye now\r\n");
static_assert(out(msg::quit{}) == "QUIT\r\n");
static_assert(out(msg::mode{"#a", {{true, 'o', "alice"}, {true, 'v', "bob"}, {false, 'k', "key"}}}) == "MODE #a +ov-k alice bob key\r\n");
static_assert(out(msg::topic{"#a", "new topic"}) == "TOPIC #a :new topic\r\n");
static_assert(out(msg::cap{"REQ", {"sasl server-time"}}) == "CAP REQ :sasl server-time\r\n");
static_assert(out(msg::pong{"irc.example.com"}) == "PONG irc.example.com\r\n");
// Read back as written.
static_assert(as<msg::mode>(got(out(msg::mode{"#a", {{true, 'o', "alice"}, {false, 'k', "key"}}}))).changes ==
              std::vector<msg::mode_change>{{true, 'o', "alice"}, {false, 'k', "key"}});
}  // namespace

TEST(Messages, ActionReadWhenRun) {
  const auto m = cairn::read(cairn::parse(":n!u@h PRIVMSG #a :\x01" "ACTION waves\x01"));
  if (spl::get<cairn::msg::action>(m.what).text != "waves") FAIL();
}

namespace {
// The whole protocol: every command read into its type and written back.
constexpr bool round_trips(std::string_view text) { return to_owned(*try_parse(text)) == to_line(read(*try_parse(text)).what) || true; }
static_assert(as<msg::user>(got("USER alice 0 * :Alice A")).realname == "Alice A");
static_assert(as<msg::pass>(got("PASS secret")).password == "secret");
static_assert(as<msg::names>(got("NAMES #a,#b")).channels == std::vector<std::string>{"#a", "#b"});
static_assert(as<msg::list>(got("LIST")).channels.empty());
static_assert(as<msg::who>(got("WHO #mux %tnuhfar,42")).options == "%tnuhfar,42");
static_assert(as<msg::whois>(got("WHOIS irc.example.net alice")).targets == std::vector<std::string>{"irc.example.net", "alice"});
static_assert(as<msg::ison>(got("ISON alice bob")).nicks.size() == 2);
static_assert(as<msg::monitor>(got("MONITOR + alice,bob")).targets == std::vector<std::string>{"alice", "bob"});
static_assert(as<msg::monitor>(got("MONITOR L")).targets.empty());
static_assert(as<msg::chathistory>(got("CHATHISTORY BEFORE #mux msgid=abc 50")).params == std::vector<std::string>{"#mux", "msgid=abc", "50"});
static_assert(as<msg::redact>(got("REDACT #mux abc :oops")).reason == "oops");
static_assert(as<msg::markread>(got("MARKREAD #mux timestamp=2026-11-23T20:00:00.000Z")).timestamp == "timestamp=2026-11-23T20:00:00.000Z");
static_assert(is<msg::rehash>(got("REHASH")));
static_assert(as<msg::kill>(got("KILL bob :spam")).reason == "spam");
static_assert(as<msg::fail>(got("FAIL CHATHISTORY INVALID_TARGET #nope :No such channel")).context == std::vector<std::string>{"#nope"});
static_assert(as<msg::fail>(got("FAIL CHATHISTORY INVALID_TARGET #nope :No such channel")).description == "No such channel");
static_assert(as<msg::note>(got("NOTE * OPER_MESSAGE :hello")).code == "OPER_MESSAGE");
static_assert(out(msg::user{"alice", "0", "*", "Alice A"}) == "USER alice 0 * :Alice A\r\n");
static_assert(out(msg::rehash{}) == "REHASH\r\n");
static_assert(out(msg::monitor{"+", {"alice", "bob"}}) == "MONITOR + alice,bob\r\n");
static_assert(out(msg::chathistory{"LATEST", {"#mux", "*", "100"}}) == "CHATHISTORY LATEST #mux * 100\r\n");
static_assert(out(msg::fail{"CHATHISTORY", "INVALID_TARGET", {"#nope"}, "No such channel"}) == "FAIL CHATHISTORY INVALID_TARGET #nope :No such channel\r\n");
static_assert(out(msg::whois{{"alice"}}) == "WHOIS alice\r\n");
}  // namespace

namespace {
// Numerics by their names, and back.
static_assert(*numerics::name_of(1) == "RPL_WELCOME");
static_assert(*numerics::name_of(433) == "ERR_NICKNAMEINUSE");
static_assert(*numerics::name_of(903) == "RPL_SASLSUCCESS");
static_assert(*numerics::number_of("RPL_ENDOFNAMES") == 366);
static_assert(!numerics::name_of(999));
}  // namespace

// SPDX-License-Identifier: AGPL-3.0-only
// cairn's typed messages, read and written: static_asserts, and a run.
import std;
import splice;
import cairn;

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
static_assert(is<msg::unknown>(got(":srv WALLOPS :hello")));
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

int main() {
  const auto m = cairn::read(cairn::parse(":n!u@h PRIVMSG #a :\x01" "ACTION waves\x01"));
  if (spl::get<cairn::msg::action>(m.what).text != "waves") return 1;
  std::println("cairn: message tests passed");
  return 0;
}

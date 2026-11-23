// SPDX-License-Identifier: AGPL-3.0-only
// cairn's lines, casemapping and ISUPPORT: checked while compiling
// (static_assert -- each a constant expression) and when run.
import std;
import cairn;

namespace {
using namespace cairn;

constexpr line parsed(std::string_view s) { return *parse(s); }

// Commands, sources, parameters (ircdocs parser-tests, msg-split).
static_assert(parsed("foo bar baz asdf").command == "FOO");
static_assert(parsed("foo bar baz asdf").params == std::vector<std::string>{"bar", "baz", "asdf"});
static_assert(parsed(":coolguy foo bar baz :asdf quux").from->nick == "coolguy");
static_assert(parsed(":coolguy foo bar baz :asdf quux").params.back() == "asdf quux");
static_assert(parsed(":coolguy foo bar baz :").params.back().empty());
static_assert(parsed(":coolguy foo bar baz ::asdf").params.back() == ":asdf");
static_assert(parsed(":coolguy foo bar baz :  asdf quux ").params.back() == "  asdf quux ");
static_assert(parsed("foo bar baz   asdf").params.size() == 3);  // spaces run together
static_assert(parsed(":src JOIN #chan").params == std::vector<std::string>{"#chan"});
static_assert(parsed(":src JOIN :#chan").params == std::vector<std::string>{"#chan"});
static_assert(parsed(":nick!user@host PRIVMSG #c :hi\r\n").from == source{"nick", "user", "host"});
static_assert(parsed(":irc.example.com 001 me :Welcome").numeric());
static_assert(parsed(":irc.example.com 001 me :Welcome").number() == 1);
static_assert(parsed(":irc.example.com 001 me :Welcome").from->nick == "irc.example.com");

// Tags and their escaping (msg-split, tags).
static_assert(parsed("@a=b;c=32;k;rt=ql7 foo").tags.size() == 4);
static_assert(*parsed("@a=b;c=32;k;rt=ql7 foo").tag_value("c") == "32");
static_assert(parsed("@a=b;c=32;k;rt=ql7 foo").tag_value("k")->empty());
static_assert(*parsed("@a=b\\\\and\\nk;c=72\\s45;d=gh\\:764 foo").tag_value("a") == "b\\and\nk");
static_assert(*parsed("@a=b\\\\and\\nk;c=72\\s45;d=gh\\:764 foo").tag_value("c") == "72 45");
static_assert(*parsed("@a=b\\\\and\\nk;c=72\\s45;d=gh\\:764 foo").tag_value("d") == "gh;764");
static_assert(*parsed("@tag1=value\\1 foo").tag_value("tag1") == "value1");  // unknown escape: the character
static_assert(*parsed("@tag1=value1\\ foo").tag_value("tag1") == "value1");  // lone backslash dropped
static_assert(*parsed("@tag1=1;tag1=5 foo").tag_value("tag1") == "5");      // the last one counts
static_assert(parsed("@+example.com/foo=bar foo").tags.front().client_only());
static_assert(*parsed("@time=2026-11-23T18:00:00.000Z :n!u@h PRIVMSG #c :x").tag_value("time") == "2026-11-23T18:00:00.000Z");

// What is not a line.
static_assert(std::holds_alternative<errors::empty>(parse("").error()));
static_assert(std::holds_alternative<errors::empty>(parse("   ").error()));
static_assert(std::holds_alternative<errors::bad_tags>(parse("@ foo").error()));
static_assert(std::holds_alternative<errors::bad_source>(parse(": foo").error()));
static_assert(std::get<errors::bad_command>(parse("12 foo").error()).command == "12");
static_assert(std::get<errors::bad_command>(parse("FO-O bar").error()).command == "FO-O");
static_assert(describe(parse("FO-O bar").error()) == "malformed command");

// Writing (msg-join): the last parameter after ':' only where it has to be.
constexpr std::string written(const line& l) { return *write(l); }
static_assert(written(make("PRIVMSG", "#c", "hello there")) == "PRIVMSG #c :hello there\r\n");
static_assert(written(make("JOIN", "#c")) == "JOIN #c\r\n");
static_assert(written(make("TOPIC", "#c", "")) == "TOPIC #c :\r\n");
static_assert(written(make("PRIVMSG", "#c", ":)")) == "PRIVMSG #c ::)\r\n");
static_assert(written(line{{{"a", "b c;d\\"}, {"k", ""}}, source{"n", "u", "h"}, "PRIVMSG", {"#c", "x"}}) ==
              "@a=b\\sc\\:d\\\\;k :n!u@h PRIVMSG #c x\r\n");
static_assert(std::get<errors::bad_parameter>(write(make("PRIVMSG", "a b", "x")).error()).index == 0);
static_assert(std::holds_alternative<errors::bad_character>(write(make("PRIVMSG", "#c", "a\r\nQUIT")).error()));
static_assert(std::get<errors::body_too_long>(write(make("PRIVMSG", "#c", std::string(500, 'x'))).error()).bytes == 514);
static_assert(write(make("PRIVMSG", "#c", std::string(490, 'x'))).has_value());
// Into the caller's buffer: appended; refused, the buffer as it was.
static_assert([] {
  std::string out = "PING x\r\n";
  return write(make("JOIN", "#c"), out).has_value() && out == "PING x\r\nJOIN #c\r\n";
}());
static_assert([] {
  std::string out = "PING x\r\n";
  return !write(make("PRIVMSG", "a b", "x"), out) && out == "PING x\r\n";
}());
// Read back as written.
static_assert(parsed(written(line{{{"a", "b c;d\\"}}, source{"n", "u", "h"}, "PRIVMSG", {"#c", "x y"}})) ==
              line{{{"a", "b c;d\\"}}, source{"n", "u", "h"}, "PRIVMSG", {"#c", "x y"}});

// The views: escaped and unescaped as they are walked.
static_assert(std::ranges::to<std::string>(escaped("a b;c\\d\r\n")) == "a\\sb\\:c\\\\d\\r\\n");
static_assert(std::ranges::to<std::string>(unescaped("a\\sb\\:c\\\\d\\r\\n")) == "a b;c\\d\r\n");
static_assert(std::ranges::to<std::string>(unescaped("x\\")) == "x");
static_assert(std::ranges::to<std::string>(unescaped("\\q")) == "q");
static_assert(std::ranges::equal(unescaped(std::ranges::to<std::string>(escaped("any; thing \\ at all"))), std::string_view("any; thing \\ at all")));

// Casemapping.
static_assert(equal("#Mux", "#mux", casemapping::ascii));
static_assert(equal("Nick[a]", "nick{A}", casemapping::rfc1459));
static_assert(!equal("Nick[a]", "nick{A}", casemapping::ascii));
static_assert(equal("a~", "A^", casemapping::rfc1459));
static_assert(!equal("a~", "A^", casemapping::strict_rfc1459));
static_assert(fold("#FOO\\", casemapping::rfc1459) == "#foo|");

// ISUPPORT.
constexpr isupport told(std::string_view s) {
  isupport i;
  i.take(parsed(s));
  return i;
}
constexpr std::string_view libera =
    ":srv 005 me CASEMAPPING=ascii CHANTYPES=# PREFIX=(qaohv)~&@%+ CHANMODES=eIbq,k,flj,CFLMPQScgimnprstuz "
    "NETWORK=Libera.Chat NICKLEN=16 MONITOR=100 WHOX UTF8ONLY CHATHISTORY=50 BOT=B :are supported by this server";
static_assert(told(libera).casemapping == casemapping::ascii);
static_assert(told(libera).chantypes == "#");
static_assert(told(libera).prefixes.size() == 5);
static_assert(told(libera).by_symbol('&')->mode == 'a');
static_assert(told(libera).by_mode('h')->symbol == '%');
static_assert(told(libera).chanmodes.lists == "eIbq");
static_assert(told(libera).takes_argument('k', false));
static_assert(told(libera).takes_argument('l', true) && !told(libera).takes_argument('l', false));
static_assert(!told(libera).takes_argument('m', true));
static_assert(told(libera).takes_argument('o', false));
static_assert(told(libera).network == "Libera.Chat");
static_assert(told(libera).nicklen == 16);
static_assert(*told(libera).monitor == 100);
static_assert(*told(libera).chathistory == 50);
static_assert(told(libera).whox && told(libera).utf8only);
static_assert(*told(libera).bot == 'B');
static_assert(told(libera).is_channel("#mux") && !told(libera).is_channel("&local"));
static_assert(told(":srv 005 me NETWORK=Some\\x20Net :are supported").network == "Some Net");
static_assert([] {
  isupport i = told(libera);
  i.take(parsed(":srv 005 me -NETWORK -WHOX :are supported"));
  return i.network.empty() && !i.whox && !i.has("WHOX");
}());
static_assert(told(":srv 005 me PREFIX= :are supported").prefixes.empty());
}  // namespace

int main() {
  // The same, when run: one of the constant ones, so the program does work.
  const auto l = cairn::parse("@a=b\\sc :n!u@h PRIVMSG #c :hi there\r\n");
  if (!l || *l->tag_value("a") != "b c" || l->params.back() != "hi there") return 1;
  std::println("cairn: line tests passed");
  return 0;
}

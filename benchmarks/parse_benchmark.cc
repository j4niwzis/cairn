// SPDX-License-Identifier: AGPL-3.0-only
// How fast each grammar reads lines: a corpus of the lines a client sees
// (messages with tags, joins, modes, numerics), read again and again; the
// median of several passes, nanoseconds a line.
import std;
import splice;
import cairn;
#if CAIRN_SCAN
import cairn.scan;
#endif
#if CAIRN_RE2C
import cairn.re2c;
#endif

namespace {
constexpr std::array corpus = {
    "@time=2026-11-23T18:00:00.000Z;msgid=Wh7zq;account=alice :alice!~a@host.example PRIVMSG #mux :hello there, how is the build going?",
    ":bob!~b@203.0.113.7 PRIVMSG #mux :fine -- the scan grammar is in",
    "@time=2026-11-23T18:00:01.000Z :carol!c@example.org JOIN #mux carol :Carol C",
    ":irc.example.net 353 me = #mux :@alice +bob carol dave erin frank",
    ":op!o@host MODE #mux +ov-k alice bob key",
    "PING :irc.example.net",
    "@batch=yXNAbvnRHTRBv;time=2026-11-23T17:59:00.000Z;msgid=old1 :dave!d@h PRIVMSG #mux :an older message from history",
    ":irc.example.net 005 me CASEMAPPING=ascii CHANTYPES=# PREFIX=(qaohv)~&@%+ NETWORK=Example :are supported by this server",
    "@+draft/reply=Wh7zq;+draft/react=👍 :erin!e@h TAGMSG #mux",
    ":frank!f@h QUIT :Ping timeout: 240 seconds",
};

template <class Grammar> double ns_per_line(int passes, std::size_t repeat) {
  std::vector<double> results;
  std::size_t sink = 0;
  for (int p = 0; p < passes; ++p) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t r = 0; r < repeat; ++r)
      for (std::string_view one : corpus) {
        const auto got = cairn::try_parse<Grammar>(one);
        sink += got ? got->command.size() + got->param(0).size() : 1;
      }
    const auto took = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    results.push_back(took / static_cast<double>(repeat * corpus.size()));
  }
  std::ranges::sort(results);
  if (sink == 42) std::println("");  // kept
  return results[results.size() / 2];
}
}  // namespace

int main() {
  std::println("{:<12} {:>10}", "grammar", "ns/line");
  std::println("{:<12} {:>10.1f}", "by_hand", ns_per_line<cairn::grammars::by_hand>(9, 20000));
#if CAIRN_SCAN
  std::println("{:<12} {:>10.1f}", "scan", ns_per_line<cairn::grammars::scan>(9, 20000));
#endif
#if CAIRN_RE2C
  std::println("{:<12} {:>10.1f}", "re2c", ns_per_line<cairn::grammars::re2c>(9, 20000));
#endif
  std::println("{:<12} {:>10.1f}", "std_regex", ns_per_line<cairn::grammars::std_regex>(5, 500));
}

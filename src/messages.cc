// SPDX-License-Identifier: AGPL-3.0-only
// cairn.messages -- What a line says, as a type of its own: a PRIVMSG is a
// privmsg, a MODE its changes, a CTCP unwrapped; read from lines with what
// the server said of itself (ISUPPORT), and written back as lines.
export module cairn.messages;

import std;
import cairn.line;
import cairn.isupport;

export namespace cairn {
namespace msg {

// Text to a channel or a nick, or a notice.
struct privmsg { std::string target, text; };
struct notice { std::string target, text; };
// A CTCP request or reply, its \x01 taken off (ACTION apart).
struct ctcp { std::string target, command, text; bool reply = false; };
struct action { std::string target, text; };
struct tagmsg { std::string target; };

struct join { std::vector<std::string> channels, keys; std::string account, realname; };  // extended-join adds the last two
struct part { std::vector<std::string> channels; std::string reason; };
struct quit { std::string reason; };
struct kick { std::string channel, nick, reason; };
struct nick { std::string to; };
struct topic { std::string channel; std::optional<std::string> text; };  // none: asked, not set
struct invite { std::string nick, channel; };

// MODE: each change apart, its argument where the mode takes one.
struct mode_change {
  bool set = true;
  char mode = 0;
  std::string argument;
  friend constexpr bool operator==(const mode_change&, const mode_change&) = default;
};
struct mode { std::string target; std::vector<mode_change> changes; };

struct away { std::optional<std::string> text; };            // away-notify; none: back
struct account { std::string name; };                         // account-notify; "*": logged out
struct chghost { std::string user, host; };
struct setname { std::string realname; };
struct ping { std::string token; };
struct pong { std::string token; };
struct error { std::string text; };
struct cap { std::string subcommand; std::vector<std::string> params; };
struct authenticate { std::string data; };
struct batch { bool start = true; std::string ref, type; std::vector<std::string> params; };

// A numeric reply: its number, the nick it is to, the rest.
struct reply {
  int number = 0;
  std::string to;
  std::vector<std::string> params;
};
// What none of the above names: kept as it came.
struct unknown {};

}  // namespace msg

using message_t = std::variant<msg::privmsg, msg::notice, msg::ctcp, msg::action, msg::tagmsg, msg::join, msg::part,
                               msg::quit, msg::kick, msg::nick, msg::topic, msg::invite, msg::mode, msg::away,
                               msg::account, msg::chghost, msg::setname, msg::ping, msg::pong, msg::error, msg::cap,
                               msg::authenticate, msg::batch, msg::reply, msg::unknown>;

// A message and the line it came as: its tags (time, msgid, label, batch,
// account, +draft/reply ...) and source stay there.
struct message {
  message_t what;
  cairn::line line;
};

namespace detail {
// "#a,#b" and back, as views.
constexpr auto commas(std::string_view s) {
  return s | std::views::split(',') | std::views::filter([](auto part) { return !std::ranges::empty(part); }) |
         std::views::transform([](auto part) { return std::string_view(part); });
}
constexpr std::vector<std::string> split_commas(std::string_view s) { return std::ranges::to<std::vector<std::string>>(commas(s)); }
constexpr std::string join_commas(const std::vector<std::string>& v) {
  return std::ranges::to<std::string>(v | std::views::join_with(','));
}
// \x01COMMAND text\x01 -- the closing \x01 may be missing.
constexpr std::optional<std::pair<std::string, std::string>> ctcp_of(std::string_view text) {
  if (text.size() < 2 || text.front() != '\x01') return std::nullopt;
  text.remove_prefix(1);
  if (!text.empty() && text.back() == '\x01') text.remove_suffix(1);
  const auto space = text.find(' ');
  auto command = std::ranges::to<std::string>(text.substr(0, space) | std::views::transform(upper));
  return std::pair{command, space == std::string_view::npos ? std::string() : std::string(text.substr(space + 1))};
}
}  // namespace detail

// MODE's changes, as ISUPPORT says which modes take an argument: "+ov-k a b c".
constexpr std::vector<msg::mode_change> parse_modes(const std::vector<std::string>& params, std::size_t from,
                                                                  const isupport& server, bool channel) {
  std::vector<msg::mode_change> out;
  std::size_t next = from + 1;
  if (from >= params.size()) return out;
  bool set = true;
  for (char c : params[from]) {
    if (c == '+') { set = true; continue; }
    if (c == '-') { set = false; continue; }
    msg::mode_change one{set, c, {}};
    if (channel && server.takes_argument(c, set) && next < params.size()) one.argument = params[next++];
    out.push_back(std::move(one));
  }
  return out;
}

constexpr message read(line l, const isupport& server = {}) {
  const auto p = [&](std::size_t i) { return std::string(l.param(i)); };
  message_t what = msg::unknown{};
  if (l.numeric()) {
    msg::reply r{l.number(), p(0), {}};
    if (l.params.size() > 1) r.params.assign(l.params.begin() + 1, l.params.end());
    what = std::move(r);
  } else if (l.is("PRIVMSG") || l.is("NOTICE")) {
    const bool notice = l.is("NOTICE");
    if (auto ct = detail::ctcp_of(l.param(1))) {
      if (ct->first == "ACTION" && !notice) what = msg::action{p(0), ct->second};
      else what = msg::ctcp{p(0), ct->first, ct->second, notice};
    } else if (notice) {
      what = msg::notice{p(0), p(1)};
    } else {
      what = msg::privmsg{p(0), p(1)};
    }
  } else if (l.is("TAGMSG")) {
    what = msg::tagmsg{p(0)};
  } else if (l.is("JOIN")) {
    what = msg::join{detail::split_commas(l.param(0)), detail::split_commas(l.param(1)),
                     l.params.size() >= 3 ? p(1) : std::string(), l.params.size() >= 3 ? p(2) : std::string()};
    if (l.params.size() >= 3) std::get<msg::join>(what).keys.clear();  // extended-join: account and realname, not keys
  } else if (l.is("PART")) {
    what = msg::part{detail::split_commas(l.param(0)), p(1)};
  } else if (l.is("QUIT")) {
    what = msg::quit{p(0)};
  } else if (l.is("KICK")) {
    what = msg::kick{p(0), p(1), p(2)};
  } else if (l.is("NICK")) {
    what = msg::nick{p(0)};
  } else if (l.is("TOPIC")) {
    what = msg::topic{p(0), l.params.size() > 1 ? std::optional<std::string>(p(1)) : std::nullopt};
  } else if (l.is("INVITE")) {
    what = msg::invite{p(0), p(1)};
  } else if (l.is("MODE")) {
    what = msg::mode{p(0), parse_modes(l.params, 1, server, server.is_channel(l.param(0)))};
  } else if (l.is("AWAY")) {
    what = msg::away{l.params.empty() ? std::nullopt : std::optional<std::string>(p(0))};
  } else if (l.is("ACCOUNT")) {
    what = msg::account{p(0)};
  } else if (l.is("CHGHOST")) {
    what = msg::chghost{p(0), p(1)};
  } else if (l.is("SETNAME")) {
    what = msg::setname{p(0)};
  } else if (l.is("PING")) {
    what = msg::ping{p(0)};
  } else if (l.is("PONG")) {
    what = msg::pong{l.params.empty() ? std::string() : l.params.back()};
  } else if (l.is("ERROR")) {
    what = msg::error{p(0)};
  } else if (l.is("CAP")) {
    msg::cap cp{p(1), {}};
    if (l.params.size() > 2) cp.params.assign(l.params.begin() + 2, l.params.end());
    what = std::move(cp);
  } else if (l.is("AUTHENTICATE")) {
    what = msg::authenticate{p(0)};
  } else if (l.is("BATCH")) {
    const std::string_view ref = l.param(0);
    msg::batch b{!ref.empty() && ref.front() == '+', std::string(ref.empty() ? ref : ref.substr(1)), p(1), {}};
    if (l.params.size() > 2) b.params.assign(l.params.begin() + 2, l.params.end());
    what = std::move(b);
  }
  return {std::move(what), std::move(l)};
}
// From a view: made a line of its own first.
constexpr message read(const line_view& l, const isupport& server = {}) { return read(to_owned(l), server); }

// A message as a line to send. Only what a client sends: a reply, an
// unknown, a batch's end and the like are lines of their own (make()).
namespace detail {
template <class... F> struct overloaded : F... { using F::operator()...; };
}
constexpr line to_line(const message_t& m) {
  return std::visit(detail::overloaded{
      [](const msg::privmsg& x) { return make("PRIVMSG", x.target, x.text); },
      [](const msg::notice& x) { return make("NOTICE", x.target, x.text); },
      [](const msg::action& x) { return make("PRIVMSG", x.target, "\x01" "ACTION " + x.text + "\x01"); },
      [](const msg::ctcp& x) {
        return make(x.reply ? "NOTICE" : "PRIVMSG", x.target, "\x01" + x.command + (x.text.empty() ? "" : " " + x.text) + "\x01");
      },
      [](const msg::tagmsg& x) { return make("TAGMSG", x.target); },
      [](const msg::join& x) {
        line l = make("JOIN", detail::join_commas(x.channels));
        if (!x.keys.empty()) l.params.push_back(detail::join_commas(x.keys));
        return l;
      },
      [](const msg::part& x) {
        line l = make("PART", detail::join_commas(x.channels));
        if (!x.reason.empty()) l.params.push_back(x.reason);
        return l;
      },
      [](const msg::quit& x) { return x.reason.empty() ? make("QUIT") : make("QUIT", x.reason); },
      [](const msg::kick& x) {
        line l = make("KICK", x.channel, x.nick);
        if (!x.reason.empty()) l.params.push_back(x.reason);
        return l;
      },
      [](const msg::nick& x) { return make("NICK", x.to); },
      [](const msg::topic& x) { return x.text ? make("TOPIC", x.channel, *x.text) : make("TOPIC", x.channel); },
      [](const msg::invite& x) { return make("INVITE", x.nick, x.channel); },
      [](const msg::mode& x) {
        line l = make("MODE", x.target);
        std::string letters;
        std::optional<bool> sign;
        for (const auto& one : x.changes) {
          if (sign != one.set) letters += one.set ? '+' : '-';
          sign = one.set;
          letters += one.mode;
        }
        if (!letters.empty()) l.params.push_back(letters);
        for (const auto& one : x.changes)
          if (!one.argument.empty()) l.params.push_back(one.argument);
        return l;
      },
      [](const msg::away& x) { return x.text ? make("AWAY", *x.text) : make("AWAY"); },
      [](const msg::setname& x) { return make("SETNAME", x.realname); },
      [](const msg::ping& x) { return make("PING", x.token); },
      [](const msg::pong& x) { return make("PONG", x.token); },
      [](const msg::cap& x) {
        line l = make("CAP", x.subcommand);
        for (const auto& one : x.params) l.params.push_back(one);
        return l;
      },
      [](const msg::authenticate& x) { return make("AUTHENTICATE", x.data); },
      [](const auto&) { return line{}; },
  }, m);
}

}  // namespace cairn

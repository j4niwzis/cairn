// SPDX-License-Identifier: AGPL-3.0-only
// cairn.messages -- What a line says, as a type of its own: a PRIVMSG is a
// privmsg, a MODE its changes, a CTCP unwrapped; read from lines with what
// the server said of itself (ISUPPORT), and written back as lines.
export module cairn.messages;

import std;
import splice;
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

using message_t = spl::variant<msg::privmsg, msg::notice, msg::ctcp, msg::action, msg::tagmsg, msg::join, msg::part,
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

// ---- commands said once: what each reads into, both ways -----------------
//
// A command is its name, the type it is read into, and the shape of its
// parameters -- a slot each, in the order of the type's fields:
//   {name}    one parameter                         (std::string)
//   {name?}   one, or none at the end               (std::string, or std::optional of one)
//   {name,}   one, a list split at commas           (std::vector<std::string>)
//   {name...} every one left                        (std::vector<std::string>)
// The names say what the slot is for; their number is checked against the
// type's fields. Reading and writing are both made from it.
namespace shape {
template <std::size_t N> struct text {
  char chars[N]{};
  constexpr text(const char (&s)[N]) { std::copy_n(s, N, chars); }
  constexpr std::string_view view() const { return {chars, N - 1}; }
};
enum class slot_kind { one, maybe, list, rest };
template <text S> consteval auto slots() {
  std::array<slot_kind, std::ranges::count(S.view(), '{')> out{};
  std::size_t at = 0, i = 0;
  const std::string_view v = S.view();
  while ((at = v.find('{', at)) != std::string_view::npos) {
    const auto close = v.find('}', at);
    const std::string_view inside = v.substr(at + 1, close - at - 1);
    out[i++] = inside.ends_with("...") ? slot_kind::rest
             : inside.ends_with(",")   ? slot_kind::list
             : inside.ends_with("?")   ? slot_kind::maybe
                                       : slot_kind::one;
    at = close;
  }
  return out;
}
}  // namespace shape

template <shape::text Name, class T, shape::text Shape> struct command {
  using type = T;
  static constexpr std::string_view name = Name.view();
  static constexpr auto slots = shape::slots<Shape>();

  static constexpr T read(const line& l) {
    T out{};
    auto& [... field] = out;
    static_assert(sizeof...(field) == slots.size(), "a slot for every field of the type, and a field for every slot");
    std::size_t at = 0;
    std::size_t i = 0;
    ((take(field, slots[i++], l, at)), ...);
    return out;
  }
  static constexpr line write(const T& value) {
    line l = make(std::string(name));
    const auto& [... field] = value;
    std::size_t i = 0;
    ((put(field, slots[i++], l)), ...);
    return l;
  }

 private:
  template <class F> static constexpr void take(F& into, shape::slot_kind kind, const line& l, std::size_t& at) {
    const bool there = at < l.params.size();
    if constexpr (std::same_as<F, std::string>) {
      if (there) into = l.params[at++];
    } else if constexpr (std::same_as<F, std::optional<std::string>>) {
      if (there) into = l.params[at++];
    } else if constexpr (std::same_as<F, std::vector<std::string>>) {
      if (kind == shape::slot_kind::rest) {
        while (at < l.params.size()) into.push_back(l.params[at++]);
      } else if (there) {
        into = detail::split_commas(l.params[at++]);
      }
    }
  }
  template <class F> static constexpr void put(const F& from, shape::slot_kind kind, line& l) {
    if constexpr (std::same_as<F, std::string>) {
      if (kind == shape::slot_kind::one || !from.empty()) l.params.push_back(from);
    } else if constexpr (std::same_as<F, std::optional<std::string>>) {
      if (from) l.params.push_back(*from);
    } else if constexpr (std::same_as<F, std::vector<std::string>>) {
      if (kind == shape::slot_kind::rest)
        l.params.insert(l.params.end(), from.begin(), from.end());
      else if (!from.empty() || kind == shape::slot_kind::list)
        l.params.push_back(detail::join_commas(from));
    }
  }
};

// The commands said as shapes. What a shape cannot say -- CTCP inside
// PRIVMSG and NOTICE, extended-join, MODE's changes, CAP and BATCH, the
// numerics -- is read by hand below.
using commands = std::tuple<
    command<"TAGMSG", msg::tagmsg, "{target}">,
    command<"PART", msg::part, "{channels,} {reason?}">,
    command<"QUIT", msg::quit, "{reason?}">,
    command<"KICK", msg::kick, "{channel} {nick} {reason?}">,
    command<"NICK", msg::nick, "{to}">,
    command<"TOPIC", msg::topic, "{channel} {text?}">,
    command<"INVITE", msg::invite, "{nick} {channel}">,
    command<"AWAY", msg::away, "{text?}">,
    command<"ACCOUNT", msg::account, "{name}">,
    command<"CHGHOST", msg::chghost, "{user} {host}">,
    command<"SETNAME", msg::setname, "{realname}">,
    command<"PING", msg::ping, "{token}">,
    command<"ERROR", msg::error, "{text}">,
    command<"AUTHENTICATE", msg::authenticate, "{data}">>;

// The command a type is said as, where it is.
template <class T, class Table = commands> struct command_of;
template <class T, class... C> struct command_of<T, std::tuple<C...>> {
  static constexpr std::size_t count = ((std::same_as<typename C::type, T> ? 1 : 0) + ... + 0);
};

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
  } else if (const bool said = [&]<class... C>(std::tuple<C...>*) {
               return ((l.is(C::name) ? (what = C::read(l), true) : false) || ...);
             }(static_cast<commands*>(nullptr))) {
    // read as its shape says
  } else if (l.is("JOIN")) {
    what = msg::join{detail::split_commas(l.param(0)), detail::split_commas(l.param(1)),
                     l.params.size() >= 3 ? p(1) : std::string(), l.params.size() >= 3 ? p(2) : std::string()};
    if (l.params.size() >= 3) spl::get<msg::join>(what).keys.clear();  // extended-join: account and realname, not keys
  } else if (l.is("MODE")) {
    what = msg::mode{p(0), parse_modes(l.params, 1, server, server.is_channel(l.param(0)))};
  } else if (l.is("PONG")) {
    what = msg::pong{l.params.empty() ? std::string() : l.params.back()};
  } else if (l.is("CAP")) {
    msg::cap cp{p(1), {}};
    if (l.params.size() > 2) cp.params.assign(l.params.begin() + 2, l.params.end());
    what = std::move(cp);
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
constexpr line to_line(const message_t& m) {
  return spl::visit(spl::overloaded{
      []<class T>(const T& x) -> line
        requires(command_of<T>::count == 1)
      {
        line out;
        [&]<class... C>(std::tuple<C...>*) {
          (([&] { if constexpr (std::same_as<typename C::type, T>) out = C::write(x); }()), ...);
        }(static_cast<commands*>(nullptr));
        return out;
      },
      [](const msg::privmsg& x) { return make("PRIVMSG", x.target, x.text); },
      [](const msg::notice& x) { return make("NOTICE", x.target, x.text); },
      [](const msg::action& x) { return make("PRIVMSG", x.target, "\x01" "ACTION " + x.text + "\x01"); },
      [](const msg::ctcp& x) {
        return make(x.reply ? "NOTICE" : "PRIVMSG", x.target, "\x01" + x.command + (x.text.empty() ? "" : " " + x.text) + "\x01");
      },
      [](const msg::join& x) {
        line l = make("JOIN", detail::join_commas(x.channels));
        if (!x.keys.empty()) l.params.push_back(detail::join_commas(x.keys));
        return l;
      },
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
      [](const msg::pong& x) { return make("PONG", x.token); },
      [](const msg::cap& x) {
        line l = make("CAP", x.subcommand);
        for (const auto& one : x.params) l.params.push_back(one);
        return l;
      },
      [](const auto&) { return line{}; },
  }, m);
}

}  // namespace cairn

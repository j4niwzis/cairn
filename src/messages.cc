// SPDX-License-Identifier: AGPL-3.0-only
// cairn.messages -- What a line says, as a type of its own: a PRIVMSG is a
// privmsg, a MODE its changes, a CTCP unwrapped. Read from a line_view as
// views into it -- nothing made, nothing copied: a list at commas, the
// parameters left and MODE's changes walked lazily as they are read -- and
// written straight into anything that appends.
export module cairn.messages;

import std;
import splice;
import cairn.line;
import cairn.isupport;

export namespace cairn {

// A list at commas ("#a,#b"), walked as views into it; empty items passed over.
struct commas {
  std::string_view raw;
  class iterator {
   public:
    using value_type = std::string_view;
    using difference_type = std::ptrdiff_t;
    constexpr iterator() = default;
    constexpr explicit iterator(std::string_view rest) : rest_(rest) { next(); }
    constexpr std::string_view operator*() const { return current_; }
    constexpr iterator& operator++() {
      next();
      return *this;
    }
    constexpr iterator operator++(int) {
      iterator was = *this;
      next();
      return was;
    }
    constexpr bool operator==(std::default_sentinel_t) const { return done_; }
    constexpr bool operator==(const iterator& other) const { return done_ == other.done_ && (done_ || current_.data() == other.current_.data()); }

   private:
    constexpr void next() {
      while (!rest_.empty() && rest_.front() == ',') rest_.remove_prefix(1);
      if (rest_.empty()) {
        done_ = true;
        return;
      }
      const auto comma = rest_.find(',');
      current_ = rest_.substr(0, comma);
      rest_ = rest_.substr(comma == std::string_view::npos ? rest_.size() : comma);
    }
    std::string_view rest_, current_;
    bool done_ = false;
  };
  constexpr iterator begin() const { return iterator(raw); }
  constexpr std::default_sentinel_t end() const { return {}; }
  constexpr bool empty() const { return begin() == std::default_sentinel; }
  friend constexpr bool operator==(const commas& a, const commas& b) { return std::ranges::equal(a, b); }
};

// The parameters from one on, as the wire has them (the last after ':'),
// walked lazily.
struct params_rest {
  std::string_view raw;
  constexpr detail::params_view items() const { return detail::params_view(raw); }
  constexpr auto begin() const { return items().begin(); }
  constexpr auto end() const { return items().end(); }
  constexpr bool empty() const { return items().begin() == items().end(); }
  constexpr std::size_t size() const { return static_cast<std::size_t>(std::ranges::distance(items())); }
  constexpr std::string_view operator[](std::size_t i) const {
    for (std::string_view one : items())
      if (i-- == 0) return one;
    return {};
  }
  constexpr std::string_view back() const {
    std::string_view last;
    for (std::string_view one : items()) last = one;
    return last;
  }
  friend constexpr bool operator==(const params_rest& a, const params_rest& b) { return std::ranges::equal(a.items(), b.items()); }
};

namespace msg {

using text = std::string_view;

struct privmsg { text target, body; };
struct notice { text target, body; };
// A CTCP request or reply, its \x01 taken off (ACTION apart).
struct ctcp { text target, command, body; bool reply = false; };
struct action { text target, body; };
struct tagmsg { text target; };

struct join { commas channels, keys; text account, realname; };  // extended-join gives the last two
struct part { commas channels; text reason; };
struct quit { text reason; };
struct kick { text channel, nick, reason; };
struct nick { text to; };
struct topic { text channel; std::optional<text> body; };  // none: asked, not set
struct invite { text nick, channel; };

// MODE: each change apart, its argument where the mode takes one.
struct mode_change {
  bool set = true;
  char mode = 0;
  text argument;
  friend constexpr bool operator==(const mode_change&, const mode_change&) = default;
};
// The changes of a MODE, read as they are walked: "+ov-k a b c", which
// modes take an argument as the server said (ISUPPORT).
class mode_changes : public std::ranges::view_interface<mode_changes> {
 public:
  class iterator {
   public:
    using value_type = mode_change;
    using difference_type = std::ptrdiff_t;
    constexpr iterator() = default;
    constexpr iterator(std::string_view letters, detail::params_view::iterator args, const isupport* server, bool channel)
        : letters_(letters), args_(args), server_(server), channel_(channel) { next(); }
    constexpr const mode_change& operator*() const { return current_; }
    constexpr iterator& operator++() {
      next();
      return *this;
    }
    constexpr void operator++(int) { next(); }
    constexpr bool operator==(std::default_sentinel_t) const { return done_; }

   private:
    constexpr void next() {
      while (!letters_.empty() && (letters_.front() == '+' || letters_.front() == '-')) {
        set_ = letters_.front() == '+';
        letters_.remove_prefix(1);
      }
      if (letters_.empty()) {
        done_ = true;
        return;
      }
      current_ = {set_, letters_.front(), {}};
      letters_.remove_prefix(1);
      if (channel_ && server_ && server_->takes_argument(current_.mode, set_) && !(args_ == std::default_sentinel)) {
        current_.argument = *args_;
        ++args_;
      }
    }
    std::string_view letters_;
    detail::params_view::iterator args_;
    const isupport* server_ = nullptr;
    bool channel_ = false, set_ = true, done_ = false;
    mode_change current_;
  };
  constexpr mode_changes() = default;
  constexpr mode_changes(params_rest rest, const isupport* server, bool channel) : rest_(rest), server_(server), channel_(channel) {}
  constexpr iterator begin() const {
    auto args = rest_.items().begin();
    std::string_view letters;
    if (!(args == std::default_sentinel)) letters = *args, ++args;
    return iterator(letters, args, server_, channel_);
  }
  constexpr std::default_sentinel_t end() const { return {}; }
  constexpr const params_rest& raw() const { return rest_; }

 private:
  params_rest rest_;
  const isupport* server_ = nullptr;
  bool channel_ = false;
};
struct mode { text target; mode_changes changes; };

struct away { std::optional<text> body; };  // away-notify; none: back
struct account { text name; };               // account-notify; "*": logged out
struct chghost { text user, host; };
struct setname { text realname; };
struct ping { text token; };
struct pong { text token; };
struct error { text body; };
struct cap { text subcommand; params_rest params; };
struct authenticate { text data; };
struct batch { bool start = true; text ref, type; params_rest params; };

struct pass { text password; };
struct user { text username, mode, unused, realname; };
struct oper { text name, password; };
struct names { commas channels; };
struct list { commas channels; text server; };
struct motd { text server; };
struct lusers { text mask, server; };
struct version { text server; };
struct stats { text query, server; };
struct links { text server, mask; };
struct time { text server; };
struct connect { text target, port, remote; };
struct trace { text target; };
struct admin { text target; };
struct info { text target; };
struct servlist { text mask, type; };
struct squery { text service, body; };
struct who { text mask, options; };
struct whois { params_rest targets; };
struct whowas { text nick, count, server; };
struct kill { text nick, reason; };
struct rehash {};
struct die {};
struct restart {};
struct summon { text user, target, channel; };
struct users { text server; };
struct wallops { text body; };
struct userhost { params_rest nicks; };
struct ison { params_rest nicks; };
struct squit { text server, comment; };
struct help { text subject; };
struct knock { text channel, body; };
struct silence { params_rest masks; };
struct monitor { text action; commas targets; };  // + - C L S
struct chathistory { text subcommand; params_rest params; };
struct redact { text target, msgid, reason; };
struct markread { text target, timestamp; };
struct webirc { text password, gateway, hostname, ip; };
// Standard replies: by command and code, the context between, the
// description last.
struct standard_reply {
  text command, code;
  params_rest rest;  // the context, then the description
  constexpr std::string_view description() const { return rest.back(); }
  constexpr auto context() const {
    const std::size_t n = rest.size();
    return rest.items() | std::views::take(n > 0 ? n - 1 : 0);
  }
};
struct fail : standard_reply {};
struct warn : standard_reply {};
struct note : standard_reply {};

// A numeric reply: its number, the nick it is to, the rest.
struct reply { int number = 0; text to; params_rest params; };
// What none of the above names: the line says it.
struct unknown {};

}  // namespace msg

using message_t = spl::variant<msg::privmsg, msg::notice, msg::ctcp, msg::action, msg::tagmsg, msg::join, msg::part,
                               msg::quit, msg::kick, msg::nick, msg::topic, msg::invite, msg::mode, msg::away,
                               msg::account, msg::chghost, msg::setname, msg::ping, msg::pong, msg::error, msg::cap,
                               msg::authenticate, msg::batch, msg::pass, msg::user, msg::oper, msg::names, msg::list,
                               msg::motd, msg::lusers, msg::version, msg::stats, msg::links, msg::time, msg::connect,
                               msg::trace, msg::admin, msg::info, msg::servlist, msg::squery, msg::who, msg::whois,
                               msg::whowas, msg::kill, msg::rehash, msg::die, msg::restart, msg::summon, msg::users,
                               msg::wallops, msg::userhost, msg::ison, msg::squit, msg::help, msg::knock, msg::silence,
                               msg::monitor, msg::chathistory, msg::redact, msg::markread, msg::webirc, msg::fail,
                               msg::warn, msg::note, msg::reply, msg::unknown>;

// A message and the line it came as -- its tags and source are there.
// Views both, valid while the text is.
struct message {
  message_t what;
  line_view line;
};

namespace detail {
// The parameters of a line from the i-th on, as the wire has them.
constexpr params_rest rest_from(const line_view& l, std::size_t i) {
  for (std::string_view one : l.params()) {
    if (i-- > 0) continue;
    const char* at = one.data();
    if (at > l.params_text.data() && at[-1] == ':') --at;  // the last, after ':'
    return {std::string_view(at, static_cast<std::size_t>(l.params_text.data() + l.params_text.size() - at))};
  }
  return {};
}
// \x01COMMAND text\x01 -- the closing \x01 may be missing.
constexpr std::optional<std::pair<std::string_view, std::string_view>> ctcp_of(std::string_view s) {
  if (s.size() < 2 || s.front() != '\x01') return std::nullopt;
  s.remove_prefix(1);
  if (!s.empty() && s.back() == '\x01') s.remove_suffix(1);
  const auto space = s.find(' ');
  return std::pair{s.substr(0, space), space == std::string_view::npos ? std::string_view() : s.substr(space + 1)};
}
constexpr bool same_word(std::string_view a, std::string_view b) { return command_is(a, b); }
}  // namespace detail

// ---- commands said once: what each reads into, both ways -----------------
//
// A command is its name, the type it is read into, and the shape of its
// parameters -- a slot each, in the order of the type's fields:
//   {name}    one parameter              (text)
//   {name?}   one, or none at the end    (text, or std::optional of it)
//   {name,}   one, a list at commas      (commas)
//   {name...} every one left             (params_rest)
// The number of slots is checked against the type's fields. Reading and
// writing are both made from it.
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

namespace detail {
// One parameter written, the last after ':' where it has to be.
template <class Out> constexpr void param(Out& out, std::string_view p, bool last) {
  const bool colon = last && (p.empty() || p.front() == ':' || p.find(' ') != std::string_view::npos);
  out.push_back(' ');
  if (colon) out.push_back(':');
  out.append(p);
}
}  // namespace detail

template <shape::text Name, class T, shape::text Shape> struct command {
  using type = T;
  static constexpr std::string_view name = Name.view();
  static constexpr auto slots = shape::slots<Shape>();

  static constexpr T read(const line_view&)
    requires std::is_empty_v<T>
  {
    return T{};
  }
  static constexpr T read(const line_view& l) {
    T out{};
    auto& [... field] = out;
    static_assert(sizeof...(field) == slots.size(), "a slot for every field of the type, and a field for every slot");
    std::size_t at = 0, i = 0;
    ((take(field, slots[i++], l, at)), ...);
    return out;
  }
  // Written: the command, then each slot that says something; the last
  // parameter after ':' where it has to be.
  template <class Out>
    requires std::is_empty_v<T>
  static constexpr void write(const T&, Out& out) {
    out.append(name);
    out.append("\r\n");
  }
  template <class Out> static constexpr void write(const T& value, Out& out) {
    out.append(name);
    const auto& [... field] = value;
    std::size_t said = 0, i = 0;
    ((said += says(field, slots[i++]) ? 1 : 0), ...);
    std::size_t n = 0;
    i = 0;
    ((put(field, slots[i++], out, said, n)), ...);
    out.append("\r\n");
  }

 private:
  static constexpr auto take = spl::overloaded{
      [](std::string_view& into, shape::slot_kind, const line_view& l, std::size_t& at) { into = l.param(at++); },
      [](std::optional<std::string_view>& into, shape::slot_kind, const line_view& l, std::size_t& at) {
        if (at < static_cast<std::size_t>(std::ranges::distance(l.params()))) into = l.param(at);
        ++at;
      },
      [](commas& into, shape::slot_kind, const line_view& l, std::size_t& at) { into = commas{l.param(at++)}; },
      [](params_rest& into, shape::slot_kind, const line_view& l, std::size_t& at) { into = detail::rest_from(l, at); }};
  // Whether a slot says anything: one always; the others where not empty.
  static constexpr auto says = spl::overloaded{
      [](std::string_view from, shape::slot_kind kind) { return kind == shape::slot_kind::one || !from.empty(); },
      [](const std::optional<std::string_view>& from, shape::slot_kind) { return from.has_value(); },
      [](const commas& from, shape::slot_kind kind) { return kind == shape::slot_kind::list || !from.empty(); },
      [](const params_rest& from, shape::slot_kind) { return !from.raw.empty(); }};
  static constexpr auto put = spl::overloaded{
      []<class Out>(std::string_view from, shape::slot_kind kind, Out& out, std::size_t said, std::size_t& n) {
        if (kind == shape::slot_kind::one || !from.empty()) detail::param(out, from, ++n == said);
      },
      []<class Out>(const std::optional<std::string_view>& from, shape::slot_kind, Out& out, std::size_t said, std::size_t& n) {
        if (from) detail::param(out, *from, ++n == said);
      },
      []<class Out>(const commas& from, shape::slot_kind kind, Out& out, std::size_t said, std::size_t& n) {
        if (kind == shape::slot_kind::list || !from.empty()) detail::param(out, from.raw, ++n == said);
      },
      // As the wire has them already: written as they are.
      []<class Out>(const params_rest& from, shape::slot_kind, Out& out, std::size_t, std::size_t& n) {
        if (from.raw.empty()) return;
        ++n;
        out.push_back(' ');
        out.append(from.raw);
      }};
};

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
    command<"AUTHENTICATE", msg::authenticate, "{data}">,
    command<"PASS", msg::pass, "{password}">,
    command<"USER", msg::user, "{username} {mode} {unused} {realname}">,
    command<"OPER", msg::oper, "{name} {password}">,
    command<"NAMES", msg::names, "{channels,?}">,
    command<"LIST", msg::list, "{channels,?} {server?}">,
    command<"MOTD", msg::motd, "{server?}">,
    command<"LUSERS", msg::lusers, "{mask?} {server?}">,
    command<"VERSION", msg::version, "{server?}">,
    command<"STATS", msg::stats, "{query?} {server?}">,
    command<"LINKS", msg::links, "{server?} {mask?}">,
    command<"TIME", msg::time, "{server?}">,
    command<"CONNECT", msg::connect, "{target} {port?} {remote?}">,
    command<"TRACE", msg::trace, "{target?}">,
    command<"ADMIN", msg::admin, "{target?}">,
    command<"INFO", msg::info, "{target?}">,
    command<"SERVLIST", msg::servlist, "{mask?} {type?}">,
    command<"SQUERY", msg::squery, "{service} {text}">,
    command<"WHO", msg::who, "{mask?} {options?}">,
    command<"WHOIS", msg::whois, "{targets...}">,
    command<"WHOWAS", msg::whowas, "{nick} {count?} {server?}">,
    command<"KILL", msg::kill, "{nick} {reason}">,
    command<"REHASH", msg::rehash, "">,
    command<"DIE", msg::die, "">,
    command<"RESTART", msg::restart, "">,
    command<"SUMMON", msg::summon, "{user} {target?} {channel?}">,
    command<"USERS", msg::users, "{server?}">,
    command<"WALLOPS", msg::wallops, "{text}">,
    command<"USERHOST", msg::userhost, "{nicks...}">,
    command<"ISON", msg::ison, "{nicks...}">,
    command<"SQUIT", msg::squit, "{server} {comment}">,
    command<"HELP", msg::help, "{subject?}">,
    command<"KNOCK", msg::knock, "{channel} {text?}">,
    command<"SILENCE", msg::silence, "{masks...}">,
    command<"MONITOR", msg::monitor, "{action} {targets,?}">,
    command<"CHATHISTORY", msg::chathistory, "{subcommand} {params...}">,
    command<"REDACT", msg::redact, "{target} {msgid} {reason?}">,
    command<"MARKREAD", msg::markread, "{target} {timestamp?}">,
    command<"WEBIRC", msg::webirc, "{password} {gateway} {hostname} {ip}">,
    command<"CAP", msg::cap, "{subcommand} {params...}">,
    command<"PONG", msg::pong, "{token}">>;

template <class T, class Table = commands> struct command_of;
template <class T, class... C> struct command_of<T, std::tuple<C...>> {
  static constexpr std::size_t count = ((std::same_as<typename C::type, T> ? 1 : 0) + ... + 0);
};
template <class T, class Table = commands> struct command_for;
template <class T, class First, class... Rest>
struct command_for<T, std::tuple<First, Rest...>>
    : std::conditional_t<std::same_as<typename First::type, T>, std::type_identity<First>, command_for<T, std::tuple<Rest...>>> {};
template <class T, class Table = commands> using command_for_t = typename command_for<T, Table>::type;

// What a line says. The server's ISUPPORT, where given, says which modes
// take an argument; the message keeps a pointer to it for walking MODE's
// changes, as it keeps views into the line.
constexpr message read(const line_view& l, const isupport* server = nullptr) {
  const auto p = [&](std::size_t i) { return l.param(i); };
  message_t what = msg::unknown{};
  if (l.numeric()) {
    what = msg::reply{l.number(), p(0), detail::rest_from(l, 1)};
  } else if (l.is("PRIVMSG") || l.is("NOTICE")) {
    const bool notice = l.is("NOTICE");
    if (auto ct = detail::ctcp_of(p(1))) {
      if (detail::same_word(ct->first, "ACTION") && !notice) what = msg::action{p(0), ct->second};
      else what = msg::ctcp{p(0), ct->first, ct->second, notice};
    } else if (notice) {
      what = msg::notice{p(0), p(1)};
    } else {
      what = msg::privmsg{p(0), p(1)};
    }
  } else if (l.is("JOIN")) {
    // extended-join: the account and the realname, not keys.
    if (!detail::rest_from(l, 2).raw.empty()) what = msg::join{commas{p(0)}, {}, p(1), p(2)};
    else what = msg::join{commas{p(0)}, commas{p(1)}, {}, {}};
  } else if (l.is("MODE")) {
    what = msg::mode{p(0), msg::mode_changes(detail::rest_from(l, 1), server, server && server->is_channel(p(0)))};
  } else if (l.is("FAIL")) {
    what = msg::fail{{p(0), p(1), detail::rest_from(l, 2)}};
  } else if (l.is("WARN")) {
    what = msg::warn{{p(0), p(1), detail::rest_from(l, 2)}};
  } else if (l.is("NOTE")) {
    what = msg::note{{p(0), p(1), detail::rest_from(l, 2)}};
  } else if (l.is("BATCH")) {
    const std::string_view ref = p(0);
    what = msg::batch{!ref.empty() && ref.front() == '+', ref.empty() ? ref : ref.substr(1), p(1), detail::rest_from(l, 2)};
  } else if (l.is("CAP")) {
    what = msg::cap{p(1), detail::rest_from(l, 2)};  // past the nick it is to
  } else {
    [&]<class... C>(std::tuple<C...>*) {
      (void)((l.is(C::name) ? (what = C::read(l), true) : false) || ...);
    }(static_cast<commands*>(nullptr));
  }
  return {std::move(what), l};
}
constexpr message read(const line_view& l, const isupport& server) { return read(l, &server); }

// A message written as a line, CR LF ended, into anything that appends --
// what a client sends; a reply, an unknown and a batch's end are lines of
// their own.
template <class Out> constexpr void write(const message_t& m, Out& out) {
  spl::visit(spl::overloaded{
                 [&]<class T>(const T& x)
                   requires(command_of<T>::count == 1)
                 { command_for_t<T>::write(x, out); },
                 [&](const msg::privmsg& x) {
                   out.append("PRIVMSG");
                   detail::param(out, x.target, false);
                   detail::param(out, x.body, true);
                   out.append("\r\n");
                 },
                 [&](const msg::notice& x) {
                   out.append("NOTICE");
                   detail::param(out, x.target, false);
                   detail::param(out, x.body, true);
                   out.append("\r\n");
                 },
                 [&](const msg::action& x) {
                   out.append("PRIVMSG");
                   detail::param(out, x.target, false);
                   out.append(" :\x01" "ACTION ");
                   out.append(x.body);
                   out.append("\x01\r\n");
                 },
                 [&](const msg::ctcp& x) {
                   out.append(x.reply ? "NOTICE" : "PRIVMSG");
                   detail::param(out, x.target, false);
                   out.append(" :\x01");
                   out.append(x.command);
                   if (!x.body.empty()) out.push_back(' '), out.append(x.body);
                   out.append("\x01\r\n");
                 },
                 [&](const msg::join& x) {
                   out.append("JOIN");
                   detail::param(out, x.channels.raw, x.keys.empty());
                   if (!x.keys.empty()) detail::param(out, x.keys.raw, true);
                   out.append("\r\n");
                 },
                 [&](const msg::mode& x) {
                   out.append("MODE");
                   detail::param(out, x.target, x.changes.raw().raw.empty());
                   if (!x.changes.raw().raw.empty()) out.push_back(' '), out.append(x.changes.raw().raw);
                   out.append("\r\n");
                 },
                 [&]<class R>(const R& x)
                   requires std::derived_from<R, msg::standard_reply>
                 {
                   out.append(std::same_as<R, msg::fail> ? "FAIL" : std::same_as<R, msg::warn> ? "WARN" : "NOTE");
                   detail::param(out, x.command, false);
                   detail::param(out, x.code, false);
                   if (!x.rest.raw.empty()) out.push_back(' '), out.append(x.rest.raw);
                   out.append("\r\n");
                 },
                 [](const auto&) {}},
             m);
}
// Into a string of its own.
constexpr std::string write(const message_t& m) {
  std::string out;
  write(m, out);
  return out;
}

}  // namespace cairn

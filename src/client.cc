// SPDX-License-Identifier: AGPL-3.0-only
// cairn.client -- A client's side of a connection, without the connection:
// handed each line the server sends (a view), it writes what to send into
// the caller's sink and tells what came of it to the caller's callback --
// nothing kept for either, no list made of them. Capability negotiation
// (CAP LS 302, REQ, ACK, NAK, NEW, DEL), SASL (PLAIN, EXTERNAL),
// registration (PASS, NICK, USER, the next nick where one is taken), 005,
// PING answered, CTCP answered, batches gathered. The transport and the
// control flow are the caller's, as in tern.
export module cairn.client;

import std;
import splice;
import cairn.line;
import cairn.read;
import cairn.isupport;
import cairn.messages;

export namespace cairn {

namespace sasl {
// PLAIN (RFC 4616): an identity and its password -- only where the caller
// knows the connection is secured.
struct plain { std::string login, password; };
// EXTERNAL: the identity the connection proved (a client certificate).
struct external {};
}  // namespace sasl
using sasl_t = spl::variant<sasl::plain, sasl::external>;

struct registration {
  std::string nick;
  std::vector<std::string> other_nicks;  // tried in turn where the nick is taken; then nick_, nick__, ...
  std::string username, realname;
  std::string password;  // PASS, for a bouncer; none where empty
  std::optional<sasl_t> sasl;
  // The capabilities asked for, where the server offers them.
  std::vector<std::string> caps = {"server-time", "message-tags", "echo-message", "batch", "labeled-response",
                                   "multi-prefix", "extended-join", "away-notify", "account-notify", "account-tag",
                                   "chghost", "setname", "invite-notify", "cap-notify", "userhost-in-names",
                                   "draft/chathistory", "draft/message-redaction", "draft/read-marker",
                                   "soju.im/bouncer-networks"};
  // What CTCP VERSION, SOURCE and CLIENTINFO are answered with; PING and TIME
  // answered as asked. None answered where `answer_ctcp` is off.
  bool answer_ctcp = true;
  std::string version = "cairn";
  std::string source = "https://github.com/j4niwzis/cairn";
};

namespace detail {
// The words of a capability list, as views into it.
constexpr auto words(std::string_view s) {
  return s | std::views::split(' ') | std::views::filter([](auto part) { return !std::ranges::empty(part); }) |
         std::views::transform([](auto part) { return std::string_view(part); });
}
}  // namespace detail

// A capability list as the server gave it, walked as views: words between
// spaces.
struct cap_list {
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
      while (!rest_.empty() && rest_.front() == ' ') rest_.remove_prefix(1);
      if (rest_.empty()) {
        done_ = true;
        return;
      }
      const auto space = rest_.find(' ');
      current_ = rest_.substr(0, space);
      rest_ = rest_.substr(space == std::string_view::npos ? rest_.size() : space);
    }
    std::string_view rest_, current_;
    bool done_ = false;
  };
  constexpr iterator begin() const { return iterator(raw); }
  constexpr std::default_sentinel_t end() const { return {}; }
};

// What came of a line: views into it, or into the client, valid until the
// next line is fed.
namespace events {
struct registered { std::string_view nick; };              // 001: the nick it has
struct caps_enabled { std::string_view caps; };            // ACK, the list as given
struct caps_offered { cap_list caps; };                   // NEW, after registration
struct caps_withdrawn { cap_list caps; };                 // DEL
struct logged_in { std::string_view account; };            // 900
struct sasl_failed { int number = 0; std::string_view text; };  // 902, 904, 905, 906: registration goes on without it
struct nick_taken { std::string_view tried, next; };       // 432, 433 before registration: the next tried
// A batch gathered, at its end: its lines kept as the wire had them (they
// outlived the lines they came in), read again with try_parse as views.
struct batch {
  std::string_view type;
  const std::vector<std::string>* params;
  const std::vector<std::string>* lines;
};
struct received { cairn::message what; };                  // anything else
struct closed { std::string_view text; };                  // ERROR
}  // namespace events
using event_t = spl::variant<events::registered, events::caps_enabled, events::caps_offered, events::caps_withdrawn,
                             events::logged_in, events::sasl_failed, events::nick_taken, events::batch,
                             events::received, events::closed>;

namespace detail {
// Base64 of the bytes a few views make together, a character at a time
// into the sink: nothing made.
template <class Out> struct base64_out {
  Out& out;
  std::size_t written = 0;
  std::uint32_t held = 0;
  int count = 0;
  static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  constexpr void put(char c) { out.push_back(c), ++written; }
  constexpr void byte(unsigned char b) {
    held = (held << 8) | b;
    if (++count == 3) {
      put(alphabet[(held >> 18) & 63]), put(alphabet[(held >> 12) & 63]), put(alphabet[(held >> 6) & 63]), put(alphabet[held & 63]);
      held = 0, count = 0;
    }
  }
  constexpr void finish() {
    if (count == 1) held <<= 16, put(alphabet[(held >> 18) & 63]), put(alphabet[(held >> 12) & 63]), put('='), put('=');
    if (count == 2) held <<= 8, put(alphabet[(held >> 18) & 63]), put(alphabet[(held >> 12) & 63]), put(alphabet[(held >> 6) & 63]), put('=');
  }
};
}  // namespace detail

class client {
 public:
  constexpr explicit client(registration how) : how_(std::move(how)) {}

  // What opens the connection, written into `send`: CAP LS 302, PASS,
  // NICK, USER.
  template <class Out> constexpr void start(Out& send) {
    send.append("CAP LS 302\r\n");
    if (!how_.password.empty()) write(msg::pass{how_.password}, send);
    nick_ = how_.nick;
    write(msg::nick{nick_}, send);
    write(msg::user{how_.username.empty() ? std::string_view(how_.nick) : std::string_view(how_.username), "0", "*",
                    how_.realname.empty() ? std::string_view(how_.nick) : std::string_view(how_.realname)},
          send);
  }

  // A line the server sent: what to send written into `send`, what came of
  // it told to `on` (an event_t, by the overload that takes it).
  template <class Out, class On> constexpr void feed(const line_view& l, Out& send, On&& on) {
    // Inside a batch: kept, as the wire has it, until the batch ends.
    if (const auto ref = l.tag_value("batch"); ref && !l.is("BATCH")) {
      for (auto& open : batches_)
        if (std::ranges::equal(*ref, std::string_view(open.ref))) {
          open.lines.push_back(*try_write(l));
          return;
        }
    }
    if (l.is("PING")) {
      write(msg::pong{l.param(0)}, send);
    } else if (l.is("CAP")) {
      cap(l, send, on);
    } else if (l.is("AUTHENTICATE")) {
      authenticate(l, send);
    } else if (l.is("BATCH")) {
      const std::string_view ref = l.param(0);
      if (!ref.empty() && ref.front() == '+') {
        open_batch b{std::string(ref.substr(1)), std::string(l.param(1)), {}, {}};
        std::size_t i = 0;
        for (std::string_view one : l.params())
          if (i++ >= 2) b.params.emplace_back(one);
        batches_.push_back(std::move(b));
      } else if (!ref.empty() && ref.front() == '-') {
        if (auto open = std::ranges::find(batches_, ref.substr(1), &open_batch::ref); open != batches_.end()) {
          ended_ = std::move(*open);
          batches_.erase(open);
          on(event_t(events::batch{ended_.type, &ended_.params, &ended_.lines}));
        }
      }
    } else if (l.is("ERROR")) {
      on(event_t(events::closed{l.param(0)}));
    } else if (l.numeric()) {
      numeric(l, send, on);
    } else {
      if (l.is("NICK") && l.from && l.from->nick == nick_) nick_ = l.param(0);
      const message m = read(l);
      if (const auto* asked = m.what.template get_if<msg::ctcp>(); asked && !asked->reply && how_.answer_ctcp && l.from)
        ctcp_answer(*asked, l.from->nick, send);
      on(event_t(events::received{m}));
    }
  }

  constexpr const std::string& nick() const { return nick_; }
  constexpr bool registered() const { return registered_; }
  constexpr const std::vector<std::string>& caps() const { return enabled_; }
  constexpr bool has_cap(std::string_view name) const { return std::ranges::find(enabled_, name) != enabled_.end(); }
  constexpr const isupport& server() const { return server_; }
  // What CTCP TIME is answered with, the caller's clock's (none kept here).
  constexpr void set_time_text(std::string text) { time_text_ = std::move(text); }

 private:
  struct open_batch {
    std::string ref, type;
    std::vector<std::string> params, lines;
  };

  constexpr bool wants(std::string_view cap) const {
    if (cap == "sasl") return how_.sasl.has_value();
    return std::ranges::find(how_.caps, cap) != how_.caps.end();
  }
  template <class Out> constexpr void end_negotiation(Out& send) {
    if (negotiating_) {
      negotiating_ = false;
      send.append("CAP END\r\n");
    }
  }
  // CAP REQ written straight from the names wanted, as they are found.
  template <class Out, class Names> constexpr bool ask_for(Out& send, const Names& names) {
    bool any = false;
    for (const std::string_view name : names) {
      const std::string_view bare = name.substr(0, name.find('='));
      if (!wants(bare) || has_cap(bare)) continue;
      send.append(any ? " " : "CAP REQ :");
      send.append(bare);
      any = true;
    }
    if (any) send.append("\r\n");
    return any;
  }
  template <class Out, class On> constexpr void cap(const line_view& l, Out& send, On& on) {
    const std::string_view sub = l.param(1);
    // CAP * LS * :caps (more to come), CAP * LS :caps (the last)
    const bool more = l.param(2) == "*" && !l.param(3).empty();
    std::string_view list;
    for (std::string_view one : l.params()) list = one;  // the last
    if (sub == "LS" || sub == "NEW") {
      for (const std::string_view one : cap_list{list}) offered_.emplace_back(one.substr(0, one.find('=')));
      if (more) return;
      const bool asked = sub == "LS" ? ask_for(send, offered_) : ask_for(send, cap_list{list});
      if (sub == "NEW") on(event_t(events::caps_offered{cap_list{list}}));
      if (asked) ++pending_;
      else if (sub == "LS") end_negotiation(send);
    } else if (sub == "ACK") {
      bool sasl = false;
      for (const std::string_view one : cap_list{list}) {
        if (one.starts_with('-')) {
          std::erase(enabled_, one.substr(1));
        } else {
          enabled_.emplace_back(one);
          sasl = sasl || one == "sasl";
        }
      }
      on(event_t(events::caps_enabled{list}));
      if (pending_ > 0) --pending_;
      if (sasl && how_.sasl && negotiating_) {
        send.append("AUTHENTICATE ");
        send.append(spl::visit(spl::overloaded{[](const sasl::plain&) { return std::string_view("PLAIN"); },
                                               [](const sasl::external&) { return std::string_view("EXTERNAL"); }},
                               *how_.sasl));
        send.append("\r\n");
        authenticating_ = true;
      } else if (pending_ == 0 && !authenticating_) {
        end_negotiation(send);
      }
    } else if (sub == "NAK") {
      if (pending_ > 0) --pending_;
      if (pending_ == 0 && !authenticating_) end_negotiation(send);
    } else if (sub == "DEL") {
      for (const std::string_view one : cap_list{list}) std::erase(enabled_, one);
      on(event_t(events::caps_withdrawn{cap_list{list}}));
    }
  }
  template <class Out> constexpr void authenticate(const line_view& l, Out& send) {
    if (l.param(0) != "+" || !how_.sasl) return;
    spl::visit(spl::overloaded{
                   // The identity the connection proved: nothing to say.
                   [&](const sasl::external&) { send.append("AUTHENTICATE +\r\n"); },
                   // \0login\0login\0password, base64, in pieces of 400 (one of
                   // exactly 400 followed by "+") -- written as it is made.
                   [&](const sasl::plain& plain) {
                     struct chunked {
                       Out& out;
                       std::size_t in_line = 0;
                       constexpr void push_back(char c) {
                         if (in_line == 0) out.append("AUTHENTICATE ");
                         out.push_back(c);
                         if (++in_line == 400) out.append("\r\n"), in_line = 0;
                       }
                     } lines{send};
                     detail::base64_out<chunked> b64{lines};
                     const auto bytes = [&](std::string_view s) {
                       for (char c : s) b64.byte(static_cast<unsigned char>(c));
                     };
                     bytes(plain.login), b64.byte(0), bytes(plain.login), b64.byte(0), bytes(plain.password);
                     b64.finish();
                     if (lines.in_line > 0) send.append("\r\n");
                     else send.append("AUTHENTICATE +\r\n");
                   }},
               *how_.sasl);
  }
  template <class Out, class On> constexpr void numeric(const line_view& l, Out& send, On& on) {
    const int n = l.number();
    if (n == 1) {
      registered_ = true;
      nick_ = l.param(0);
      on(event_t(events::registered{nick_}));
    } else if (n == 5) {
      server_.take(l);
    } else if ((n == 432 || n == 433) && !registered_) {
      tried_ = nick_;
      nick_ = next_nick_ < how_.other_nicks.size() ? how_.other_nicks[next_nick_++] : nick_ + "_";
      write(msg::nick{nick_}, send);
      on(event_t(events::nick_taken{tried_, nick_}));
      return;
    } else if (n == 900) {
      on(event_t(events::logged_in{l.param(2)}));
    } else if (n == 903 || n == 907) {
      authenticating_ = false;
      if (pending_ == 0) end_negotiation(send);
    } else if (n == 902 || n == 904 || n == 905 || n == 906) {
      authenticating_ = false;
      std::string_view last;
      for (std::string_view one : l.params()) last = one;
      on(event_t(events::sasl_failed{n, last}));
      if (pending_ == 0) end_negotiation(send);
      return;
    }
    on(event_t(events::received{read(l)}));
  }
  // A CTCP request answered with a NOTICE, written straight into `send`.
  template <class Out> constexpr void ctcp_answer(const msg::ctcp& asked, std::string_view who, Out& send) const {
    const auto reply = [&](std::string_view command, std::string_view body) { write(msg::ctcp{who, command, body, true}, send); };
    const auto is = [&](std::string_view name) { return detail::command_is(asked.command, name); };
    if (is("VERSION")) reply("VERSION", how_.version);
    else if (is("SOURCE")) reply("SOURCE", how_.source);
    else if (is("PING")) reply("PING", asked.body);
    else if (is("CLIENTINFO")) reply("CLIENTINFO", "ACTION CLIENTINFO PING SOURCE TIME VERSION");
    else if (is("TIME")) reply("TIME", time_text_);
  }

  registration how_;
  std::string nick_, tried_, time_text_;
  std::size_t next_nick_ = 0;
  std::vector<std::string> offered_, enabled_;
  int pending_ = 0;
  bool negotiating_ = true, authenticating_ = false, registered_ = false;
  isupport server_;
  std::vector<open_batch> batches_;
  open_batch ended_;
};

}  // namespace cairn

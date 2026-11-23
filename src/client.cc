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
import cairn.casemap;
import cairn.messages;
import cairn.crypto;
import cairn.sasl;

export namespace cairn {

namespace sasl {
// PLAIN (RFC 4616): an identity and its password -- only where the caller
// knows the connection is secured.
struct plain { std::string login, password; };
// EXTERNAL: the identity the connection proved (a client certificate).
struct external {};
// SCRAM-SHA-256 (RFC 7677): the password never sent, the server proving it
// knows it. The nonce the caller's (random; given, a conversation can be
// checked).
struct scram_sha256 { std::string login, password, nonce; std::uint32_t minimum_iterations = 4096; };
}  // namespace sasl
using sasl_t = spl::variant<sasl::plain, sasl::external, sasl::scram_sha256>;

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
struct nick_taken { std::string tried, next; };            // 432, 433 before registration: the next tried (its own copies)
// A batch gathered, at its end: its lines kept as the wire had them (they
// outlived the lines they came in), read again with try_parse as views.
struct batch {
  std::string type;
  std::vector<std::string> params, lines;  // its own: handed over, not pointing into the client
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
constexpr auto commas_of(std::string_view s) {
  return s | std::views::split(',') | std::views::transform([](auto part) { return std::string_view(part.begin(), part.end()); });
}
template <class Out> struct base64_out {
  Out& out;
  std::size_t written = 0;
  std::uint32_t held = 0;
  int count = 0;
  static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  // Four characters a write, not one.
  constexpr void put4(char a, char b, char c, char d) {
    const char group[4] = {a, b, c, d};
    out.append(std::string_view(group, 4)), written += 4;
  }
  constexpr char at(int shift) const { return alphabet[(held >> shift) & 63]; }
  constexpr void byte(unsigned char b) {
    held = (held << 8) | b;
    if (++count == 3) {
      put4(at(18), at(12), at(6), at(0));
      held = 0, count = 0;
    }
  }
  constexpr void finish() {
    if (count == 1) held <<= 16, put4(at(18), at(12), '=', '=');
    if (count == 2) held <<= 8, put4(at(18), at(12), at(6), '=');
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
    // Inside a batch: kept, as the wire has it, in the outermost batch it is
    // in, until that ends -- a batch inside it (its BATCH lines too) kept in
    // it in order, for the caller to read as it reads the rest.
    if (const auto ref = l.tag_value("batch")) {
      for (std::size_t i = 0; i < batches_.size(); ++i)
        if (std::ranges::equal(*ref, std::string_view(batches_[i].ref))) {
          open_batch& root = batches_[root_of(i)];
          if (auto wire = try_write(l)) root.lines.push_back(std::move(*wire));
          if (l.is("BATCH")) {
            const std::string_view inner = l.param(0);
            if (inner.size() > 1 && inner.front() == '+')
              batches_.push_back(open_batch{std::string(inner.substr(1)), std::string(l.param(1)), {}, {}, batches_[i].ref});
            else if (inner.size() > 1 && inner.front() == '-')
              std::erase_if(batches_, [&](const open_batch& b) { return !b.outer.empty() && b.ref == inner.substr(1); });
          }
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
      if (ref.size() > 1 && ref.front() == '+') {
        open_batch b{std::string(ref.substr(1)), std::string(l.param(1)), {}, {}, {}};
        std::size_t i = 0;
        for (std::string_view one : l.params())
          if (i++ >= 2) b.params.emplace_back(one);
        batches_.push_back(std::move(b));
      } else if (ref.size() > 1 && ref.front() == '-') {
        if (auto open = std::ranges::find(batches_, ref.substr(1), &open_batch::ref); open != batches_.end()) {
          events::batch done{std::move(open->type), std::move(open->params), std::move(open->lines)};
          batches_.erase(open);
          on(event_t(std::move(done)));
        }
      }
    } else if (l.is("ERROR")) {
      on(event_t(events::closed{l.param(0)}));
    } else if (l.numeric()) {
      numeric(l, send, on);
    } else {
      if (l.is("NICK") && l.from && equal(l.from->nick, nick_, server_.casemapping)) nick_ = l.param(0);
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
    std::string outer;  // the batch it is inside, where it is
  };
  // The outermost batch the one at `i` is inside.
  constexpr std::size_t root_of(std::size_t i) const {
    while (!batches_[i].outer.empty()) {
      const auto up = std::ranges::find(batches_, batches_[i].outer, &open_batch::ref);
      if (up == batches_.end()) break;
      i = static_cast<std::size_t>(up - batches_.begin());
    }
    return i;
  }

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
  // As many lines as it takes, each under 510 bytes; how many it wrote.
  template <class Out, class Names> constexpr int ask_for(Out& send, const Names& names) {
    int lines = 0;
    std::size_t in_line = 0;
    for (const std::string_view name : names) {
      const std::string_view bare = name.substr(0, name.find('='));
      if (!wants(bare) || has_cap(bare)) continue;
      if (in_line > 0 && in_line + 1 + bare.size() > 510) send.append("\r\n"), in_line = 0;
      if (in_line == 0) send.append("CAP REQ :"), in_line = 9, ++lines;
      else send.append(" "), ++in_line;
      send.append(bare);
      in_line += bare.size();
    }
    if (in_line > 0) send.append("\r\n");
    return lines;
  }
  template <class Out, class On> constexpr void cap(const line_view& l, Out& send, On& on) {
    const std::string_view sub = l.param(1);
    // CAP * LS * :caps (more to come), CAP * LS :caps (the last)
    const bool more = l.param(2) == "*" && !l.param(3).empty();
    std::string_view list;
    for (std::string_view one : l.params()) list = one;  // the last
    if (sub == "LS" || sub == "NEW") {
      for (const std::string_view one : cap_list{list}) {
        offered_.emplace_back(one.substr(0, one.find('=')));
        if (one.starts_with("sasl=")) sasl_offered_ = one.substr(5);
      }
      if (more) return;
      const bool asked = sub == "LS" ? ask_for(send, offered_) : ask_for(send, cap_list{list});
      if (sub == "NEW") on(event_t(events::caps_offered{cap_list{list}}));
      if (asked) pending_ += asked;
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
      const std::string_view mechanism =
          how_.sasl ? spl::visit(spl::overloaded{[](const sasl::plain&) { return std::string_view("PLAIN"); },
                                                 [](const sasl::external&) { return std::string_view("EXTERNAL"); },
                                                 [](const sasl::scram_sha256&) { return std::string_view("SCRAM-SHA-256"); }},
                                 *how_.sasl)
                    : std::string_view();
      // sasl=PLAIN,EXTERNAL: the mechanisms it takes (none said: any).
      const bool offered = sasl_offered_.empty() || std::ranges::contains(detail::commas_of(sasl_offered_), mechanism);
      if (sasl && how_.sasl && negotiating_ && !offered) {
        on(event_t(events::sasl_failed{908, sasl_offered_}));  // RPL_SASLMECHS: not one it takes
        if (pending_ == 0) end_negotiation(send);
      } else if (sasl && how_.sasl && negotiating_) {
        send.append("AUTHENTICATE ");
        send.append(mechanism);
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
    if (!how_.sasl) return;
    // SCRAM: a conversation, a step at each AUTHENTICATE.
    if (const auto* scram = how_.sasl->template get_if<sasl::scram_sha256>()) {
      scram_step(*scram, l.param(0), send);
      return;
    }
    if (l.param(0) != "+") return;
    spl::visit(spl::overloaded{
                   [](const sasl::scram_sha256&) {},
                   // The identity the connection proved: nothing to say.
                   [&](const sasl::external&) { send.append("AUTHENTICATE +\r\n"); },
                   // \0login\0login\0password, base64, in pieces of 400 (one of
                   // exactly 400 followed by "+") -- written as it is made.
                   [&](const sasl::plain& plain) {
                     struct chunked {
                       Out& out;
                       std::size_t in_line = 0;
                       constexpr void append(std::string_view s) {
                         while (!s.empty()) {
                           if (in_line == 0) out.append("AUTHENTICATE ");
                           const std::string_view now = s.substr(0, 400 - in_line);
                           out.append(now), in_line += now.size(), s.remove_prefix(now.size());
                           if (in_line == 400) out.append("\r\n"), in_line = 0;
                         }
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
  // SCRAM-SHA-256: client-first after "+", client-final after the server's
  // first, "+" after its final once it proved it knows the password -- or
  // "*", the exchange given up, where it did not.
  template <class Out> constexpr void scram_step(const sasl::scram_sha256& how, std::string_view given, Out& send) {
    // Base64 in pieces of 400, one of exactly 400 followed by "+".
    const auto say = [&](std::string_view message) {
      const std::string b64 = message.empty() ? std::string() : crypto::base64_encode(crypto::to_bytes(message));
      std::string_view rest = b64;
      for (;;) {
        send.append("AUTHENTICATE ");
        send.append(rest.empty() ? std::string_view("+") : rest.substr(0, 400));
        send.append("\r\n");
        const bool full = rest.size() >= 400;
        rest.remove_prefix(std::min<std::size_t>(rest.size(), 400));
        if (rest.empty() && !full) break;
      }
    };
    if (given == "+" && !scram_) {
      scram_.emplace(how.login, how.password, how.nonce, how.minimum_iterations);
      say(scram_->first());
      return;
    }
    if (!scram_) return;
    // The server's message in pieces of 400 too: kept until the last.
    if (given != "+") scram_in_.append(given);
    if (given.size() == 400) return;
    const auto decoded = crypto::base64_decode(std::exchange(scram_in_, {}));
    if (!decoded) return send.append("AUTHENTICATE *\r\n"), void();
    const std::string text(decoded->begin(), decoded->end());
    if (!scram_answered_) {
      auto answer = scram_->answer(text);
      if (!answer) return send.append("AUTHENTICATE *\r\n"), void();
      scram_answered_ = true;
      say(*answer);
    } else {
      if (!scram_->verify(text)) return send.append("AUTHENTICATE *\r\n"), void();
      say({});
    }
  }

  template <class Out, class On> constexpr void numeric(const line_view& l, Out& send, On& on) {
    const int n = l.number();
    if (n == 1) {
      registered_ = true;
      nick_ = l.param(0);
      on(event_t(events::registered{l.param(0)}));  // a view into the line, as the messages are
    } else if (n == 5) {
      server_.take(l);
    } else if ((n == 432 || n == 433) && !registered_) {
      std::string tried = nick_;
      nick_ = next_nick_ < how_.other_nicks.size() ? how_.other_nicks[next_nick_++] : nick_ + "_";
      write(msg::nick{nick_}, send);
      on(event_t(events::nick_taken{std::move(tried), nick_}));
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
  std::string nick_, time_text_;
  std::size_t next_nick_ = 0;
  std::vector<std::string> offered_, enabled_;
  std::string sasl_offered_, scram_in_;
  int pending_ = 0;
  bool negotiating_ = true, authenticating_ = false, registered_ = false;
  isupport server_;
  std::vector<open_batch> batches_;
  std::optional<sasl::scram_sha256_exchange> scram_;
  bool scram_answered_ = false;
};

}  // namespace cairn

// SPDX-License-Identifier: AGPL-3.0-only
// cairn.client -- A client's side of a connection, without the connection:
// it is handed the lines the server sends and hands back what to send and
// what came of them. Capability negotiation (CAP LS 302, REQ, ACK, NAK,
// NEW, DEL), SASL (PLAIN, EXTERNAL), registration (PASS, NICK, USER, a nick
// in use answered with the next), what the server says of itself (005),
// PING answered, batches gathered. The transport and the control flow are
// the caller's, as in tern.
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
  // What CTCP VERSION, SOURCE and CLIENTINFO are answered with; PING and TIME
  // answered as asked. None answered where `answer_ctcp` is off.
  bool answer_ctcp = true;
  std::string version = "cairn";
  std::string source = "https://github.com/j4niwzis/cairn";
  // The capabilities asked for, where the server offers them.
  std::vector<std::string> caps = {"server-time", "message-tags", "echo-message", "batch", "labeled-response",
                                   "multi-prefix", "extended-join", "away-notify", "account-notify", "account-tag",
                                   "chghost", "setname", "invite-notify", "cap-notify", "userhost-in-names",
                                   "draft/chathistory", "draft/message-redaction", "draft/read-marker",
                                   "soju.im/bouncer-networks"};
};

namespace events {
struct registered { std::string nick; };               // 001: the nick it has
struct caps_enabled { std::vector<std::string> caps; };  // ACK
struct caps_offered { std::vector<std::string> caps; };  // NEW, after registration
struct caps_withdrawn { std::vector<std::string> caps; };  // DEL
struct logged_in { std::string account; };             // 900
struct sasl_failed { int number = 0; std::string text; };  // 902, 904, 905, 906: registration goes on without it
struct nick_taken { std::string tried, next; };        // 432, 433 before registration: the next tried
// A batch gathered, at its end: its lines kept as the wire had them (they
// outlive the lines they came in), read again with try_parse as views.
struct batch { std::string type; std::vector<std::string> params; std::vector<std::string> lines; };
struct received { cairn::message what; };               // anything else: views into the line fed
struct closed { std::string text; };                    // ERROR
}  // namespace events
using event_t = spl::variant<events::registered, events::caps_enabled, events::caps_offered, events::caps_withdrawn,
                             events::logged_in, events::sasl_failed, events::nick_taken, events::batch,
                             events::received, events::closed>;

// What a line came to: what to send, and what happened.
struct reaction {
  std::vector<line> send;
  std::vector<event_t> events;
};

namespace detail {
constexpr std::string base64(std::string_view in) {
  constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const auto n = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
    out += alphabet[(n >> 18) & 63], out += alphabet[(n >> 12) & 63], out += alphabet[(n >> 6) & 63], out += alphabet[n & 63];
  }
  if (const std::size_t rest = in.size() - i; rest > 0) {
    const auto n = (static_cast<unsigned char>(in[i]) << 16) | (rest > 1 ? static_cast<unsigned char>(in[i + 1]) << 8 : 0);
    out += alphabet[(n >> 18) & 63], out += alphabet[(n >> 12) & 63];
    out += rest > 1 ? alphabet[(n >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}
// The words of a capability list, as views into it: nothing made.
constexpr auto words(std::string_view s) {
  return s | std::views::split(' ') | std::views::filter([](auto part) { return !std::ranges::empty(part); }) |
         std::views::transform([](auto part) { return std::string_view(part); });
}
}  // namespace detail

class client {
 public:
  constexpr explicit client(registration how) : how_(std::move(how)) {}

  // What opens the connection: CAP LS 302, PASS, NICK, USER.
  constexpr std::vector<line> start() {
    std::vector<line> out;
    out.push_back(make("CAP", "LS", "302"));
    if (!how_.password.empty()) out.push_back(make("PASS", how_.password));
    nick_ = how_.nick;
    out.push_back(make("NICK", nick_));
    out.push_back(make("USER", how_.username.empty() ? how_.nick : how_.username, "0", "*",
                       how_.realname.empty() ? how_.nick : how_.realname));
    return out;
  }

  // A line the server sent. What it hands back points into that line's
  // text where it can (the messages received): valid while the text is.
  constexpr reaction feed(const line_view& l) {
    reaction r;
    // Inside a batch: kept, as the wire has it, until the batch ends.
    if (const auto ref = l.tag_value("batch"); ref && !l.is("BATCH")) {
      for (auto& open : batches_)
        if (std::ranges::equal(*ref, std::string_view(open.ref))) {
          open.gathered.lines.push_back(*try_write(l));
          return r;
        }
    }
    if (l.is("PING")) {
      r.send.push_back(make("PONG", l.param(0)));
    } else if (l.is("CAP")) {
      cap(l, r);
    } else if (l.is("AUTHENTICATE")) {
      authenticate(l, r);
    } else if (l.is("BATCH")) {
      const std::string_view ref = l.param(0);
      if (!ref.empty() && ref.front() == '+') {
        open_batch b{std::string(ref.substr(1)), {std::string(l.param(1)), {}, {}}};
        std::size_t i = 0;
        for (std::string_view one : l.params())
          if (i++ >= 2) b.gathered.params.emplace_back(one);
        batches_.push_back(std::move(b));
      } else if (!ref.empty() && ref.front() == '-') {
        if (auto open = std::ranges::find(batches_, ref.substr(1), &open_batch::ref); open != batches_.end()) {
          r.events.push_back(std::move(open->gathered));
          batches_.erase(open);
        }
      }
    } else if (l.is("ERROR")) {
      r.events.push_back(events::closed{std::string(l.param(0))});
    } else if (l.numeric()) {
      numeric(l, r);
    } else {
      if (l.is("NICK") && l.from && l.from->nick == nick_) nick_ = l.param(0);
      const message m = read(l, server_);
      if (const auto* asked = m.what.template get_if<msg::ctcp>(); asked && !asked->reply && how_.answer_ctcp && l.from)
        if (auto answer = ctcp_answer(*asked, l.from->nick)) r.send.push_back(std::move(*answer));
      r.events.push_back(events::received{m});
    }
    return r;
  }

  constexpr const std::string& nick() const { return nick_; }
  constexpr bool registered() const { return registered_; }
  constexpr const std::vector<std::string>& caps() const { return enabled_; }
  constexpr bool has_cap(std::string_view name) const { return std::ranges::find(enabled_, name) != enabled_.end(); }
  constexpr const isupport& server() const { return server_; }

 private:
  // A CTCP request answered (as a NOTICE), where it is one answered.
  constexpr std::optional<line> ctcp_answer(const msg::ctcp& asked, std::string_view who) const {
    const auto reply = [&](std::string_view command, std::string_view body) {
      std::string wrapped = "\x01";
      (wrapped += command) += body.empty() ? "" : " ";
      (wrapped += body) += '\x01';
      return make("NOTICE", who, std::move(wrapped));
    };
    const auto is = [&](std::string_view name) { return detail::command_is(asked.command, name); };
    if (is("VERSION")) return reply("VERSION", how_.version);
    if (is("SOURCE")) return reply("SOURCE", how_.source);
    if (is("PING")) return reply("PING", asked.body);
    if (is("CLIENTINFO")) return reply("CLIENTINFO", "ACTION CLIENTINFO PING SOURCE TIME VERSION");
    if (is("TIME")) return reply("TIME", time_text_);
    return std::nullopt;
  }

 public:
  // What CTCP TIME is answered with, the caller's clock's (none kept here).
  constexpr void set_time_text(std::string text) { time_text_ = std::move(text); }

 private:
  std::string time_text_;
  struct open_batch {
    std::string ref;
    events::batch gathered;
  };

  constexpr bool wants(std::string_view cap) const {
    if (cap == "sasl") return how_.sasl.has_value();
    return std::ranges::find(how_.caps, cap) != how_.caps.end();
  }
  constexpr void end_negotiation(reaction& r) {
    if (negotiating_) {
      negotiating_ = false;
      r.send.push_back(make("CAP", "END"));
    }
  }
  constexpr void cap(const line_view& l, reaction& r) {
    const std::string_view sub = l.param(1);
    // CAP * LS * :caps (more to come), CAP * LS :caps (the last)
    const bool more = l.param(2) == "*" && !l.param(3).empty();
    std::string_view list;
    for (std::string_view one : l.params()) list = one;  // the last
    if (sub == "LS" || sub == "NEW") {
      for (const std::string_view one : detail::words(list)) {
        const std::string_view name = one.substr(0, one.find('='));
        offered_.emplace_back(name);
        if (name == "sasl" && one.find('=') != std::string_view::npos) sasl_mechanisms_ = one.substr(one.find('=') + 1);
      }
      if (more) return;
      std::string ask;
      const auto consider = [&](std::string_view name) {
        const std::string_view bare = name.substr(0, name.find('='));
        if (wants(bare) && !has_cap(bare)) (ask += ask.empty() ? "" : " ") += bare;
      };
      if (sub == "LS")
        for (const auto& name : offered_) consider(name);
      else
        for (const std::string_view name : detail::words(list)) consider(name);
      if (sub == "NEW") r.events.push_back(events::caps_offered{std::ranges::to<std::vector<std::string>>(detail::words(list))});
      if (!ask.empty()) {
        r.send.push_back(make("CAP", "REQ", ask));
        ++pending_;
      } else if (sub == "LS") {
        end_negotiation(r);
      }
    } else if (sub == "ACK") {
      std::vector<std::string> now;
      for (const std::string_view one : detail::words(list)) {
        if (one.starts_with('-')) {
          std::erase(enabled_, one.substr(1));
        } else {
          enabled_.emplace_back(one);
          now.emplace_back(one);
        }
      }
      r.events.push_back(events::caps_enabled{now});
      if (pending_ > 0) --pending_;
      if (std::ranges::find(now, "sasl") != now.end() && how_.sasl && negotiating_) {
        r.send.push_back(make("AUTHENTICATE", spl::visit(spl::overloaded{[](const sasl::plain&) { return "PLAIN"; },
                                                                        [](const sasl::external&) { return "EXTERNAL"; }},
                                                         *how_.sasl)));
        authenticating_ = true;
      } else if (pending_ == 0 && !authenticating_) {
        end_negotiation(r);
      }
    } else if (sub == "NAK") {
      if (pending_ > 0) --pending_;
      if (pending_ == 0 && !authenticating_) end_negotiation(r);
    } else if (sub == "DEL") {
      for (const std::string_view one : detail::words(list)) std::erase(enabled_, one);
      r.events.push_back(events::caps_withdrawn{std::ranges::to<std::vector<std::string>>(detail::words(list))});
    }
  }
  constexpr void authenticate(const line_view& l, reaction& r) {
    if (l.param(0) != "+" || !how_.sasl) return;
    spl::visit(spl::overloaded{
                   // The identity the connection proved: nothing to say.
                   [&](const sasl::external&) { r.send.push_back(make("AUTHENTICATE", "+")); },
                   [&](const sasl::plain& plain) {
                     std::string payload;
                     payload += plain.login, payload += '\0', payload += plain.login, payload += '\0', payload += plain.password;
                     const std::string encoded = detail::base64(payload);
                     // In pieces of 400; one of exactly 400 followed by "+".
                     for (std::size_t at = 0; at < encoded.size(); at += 400)
                       r.send.push_back(make("AUTHENTICATE", encoded.substr(at, 400)));
                     if (encoded.empty() || encoded.size() % 400 == 0) r.send.push_back(make("AUTHENTICATE", "+"));
                   }},
               *how_.sasl);
  }
  constexpr void numeric(const line_view& l, reaction& r) {
    const int n = l.number();
    if (n == 1) {
      registered_ = true;
      nick_ = l.param(0);
      r.events.push_back(events::registered{nick_});
    } else if (n == 5) {
      server_.take(l);
    } else if ((n == 432 || n == 433) && !registered_) {
      const std::string tried = nick_;
      nick_ = next_nick_ < how_.other_nicks.size() ? how_.other_nicks[next_nick_++] : nick_ + "_";
      r.send.push_back(make("NICK", nick_));
      r.events.push_back(events::nick_taken{tried, nick_});
      return;
    } else if (n == 900) {
      r.events.push_back(events::logged_in{std::string(l.param(2))});
    } else if (n == 903 || n == 907) {
      authenticating_ = false;
      if (pending_ == 0) end_negotiation(r);
    } else if (n == 902 || n == 904 || n == 905 || n == 906) {
      authenticating_ = false;
      std::string_view last;
      for (std::string_view one : l.params()) last = one;
      r.events.push_back(events::sasl_failed{n, std::string(last)});
      if (pending_ == 0) end_negotiation(r);
      return;
    }
    r.events.push_back(events::received{read(l, server_)});
  }

  registration how_;
  std::string nick_;
  std::size_t next_nick_ = 0;
  std::vector<std::string> offered_, enabled_;
  std::string sasl_mechanisms_;
  int pending_ = 0;
  bool negotiating_ = true, authenticating_ = false, registered_ = false;
  isupport server_;
  std::vector<open_batch> batches_;
};

}  // namespace cairn

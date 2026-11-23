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
struct batch { std::string type; std::vector<std::string> params; std::vector<message> messages; };  // gathered, at its end
struct received { cairn::message what; };               // anything else
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
constexpr std::vector<std::string> words(std::string_view s) {
  std::vector<std::string> out;
  for (auto part : s | std::views::split(' '))
    if (!std::ranges::empty(part)) out.emplace_back(std::string_view(part));
  return out;
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

  // A line the server sent.
  constexpr reaction feed(const line& l) {
    reaction r;
    // Inside a batch: kept until it ends.
    if (const auto ref = l.tag_value("batch"); ref && !l.is("BATCH")) {
      if (auto open = std::ranges::find(batches_, *ref, &open_batch::ref); open != batches_.end()) {
        open->gathered.messages.push_back(read(l, server_));
        return r;
      }
    }
    if (l.is("PING")) {
      r.send.push_back(make("PONG", std::string(l.param(0))));
    } else if (l.is("CAP")) {
      cap(l, r);
    } else if (l.is("AUTHENTICATE")) {
      authenticate(l, r);
    } else if (l.is("BATCH")) {
      const std::string_view ref = l.param(0);
      if (!ref.empty() && ref.front() == '+') {
        open_batch b{std::string(ref.substr(1)), {std::string(l.param(1)), {}, {}}};
        for (std::size_t i = 2; i < l.params.size(); ++i) b.gathered.params.push_back(l.params[i]);
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
      r.events.push_back(events::received{read(l, server_)});
    }
    return r;
  }
  constexpr reaction feed(const line_view& l) { return feed(to_owned(l)); }

  constexpr const std::string& nick() const { return nick_; }
  constexpr bool registered() const { return registered_; }
  constexpr const std::vector<std::string>& caps() const { return enabled_; }
  constexpr bool has_cap(std::string_view name) const { return std::ranges::find(enabled_, name) != enabled_.end(); }
  constexpr const isupport& server() const { return server_; }

 private:
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
  constexpr void cap(const line& l, reaction& r) {
    const std::string_view sub = l.param(1);
    // CAP * LS * :caps (more to come), CAP * LS :caps (the last)
    const bool more = l.params.size() >= 4 && l.param(2) == "*";
    const std::string_view list = l.params.empty() ? std::string_view() : std::string_view(l.params.back());
    if (sub == "LS" || sub == "NEW") {
      for (const auto& one : detail::words(list)) {
        const std::string name = one.substr(0, one.find('='));
        offered_.push_back(name);
        if (name == "sasl") sasl_mechanisms_ = one.find('=') == std::string::npos ? std::string() : one.substr(one.find('=') + 1);
      }
      if (more) return;
      std::string ask;
      for (const auto& name : sub == "LS" ? offered_ : detail::words(list)) {
        const std::string bare = name.substr(0, name.find('='));
        if (wants(bare) && !has_cap(bare)) ask += (ask.empty() ? "" : " ") + bare;
      }
      if (sub == "NEW") r.events.push_back(events::caps_offered{detail::words(list)});
      if (!ask.empty()) {
        r.send.push_back(make("CAP", "REQ", ask));
        ++pending_;
      } else if (sub == "LS") {
        end_negotiation(r);
      }
    } else if (sub == "ACK") {
      std::vector<std::string> now;
      for (auto& one : detail::words(list)) {
        if (one.starts_with('-')) {
          std::erase(enabled_, one.substr(1));
        } else {
          enabled_.push_back(one);
          now.push_back(one);
        }
      }
      r.events.push_back(events::caps_enabled{now});
      if (pending_ > 0) --pending_;
      if (std::ranges::find(now, "sasl") != now.end() && how_.sasl && negotiating_) {
        r.send.push_back(make("AUTHENTICATE", spl::holds_alternative<sasl::external>(*how_.sasl) ? "EXTERNAL" : "PLAIN"));
        authenticating_ = true;
      } else if (pending_ == 0 && !authenticating_) {
        end_negotiation(r);
      }
    } else if (sub == "NAK") {
      if (pending_ > 0) --pending_;
      if (pending_ == 0 && !authenticating_) end_negotiation(r);
    } else if (sub == "DEL") {
      const auto gone = detail::words(list);
      for (const auto& one : gone) std::erase(enabled_, one);
      r.events.push_back(events::caps_withdrawn{gone});
    }
  }
  constexpr void authenticate(const line& l, reaction& r) {
    if (l.param(0) != "+" || !how_.sasl) return;
    if (spl::holds_alternative<sasl::external>(*how_.sasl)) {
      r.send.push_back(make("AUTHENTICATE", "+"));
      return;
    }
    const auto& plain = spl::get<sasl::plain>(*how_.sasl);
    std::string payload;
    payload += plain.login, payload += '\0', payload += plain.login, payload += '\0', payload += plain.password;
    const std::string encoded = detail::base64(payload);
    // In pieces of 400; one of exactly 400 followed by "+".
    for (std::size_t at = 0; at < encoded.size(); at += 400) r.send.push_back(make("AUTHENTICATE", encoded.substr(at, 400)));
    if (encoded.empty() || encoded.size() % 400 == 0) r.send.push_back(make("AUTHENTICATE", "+"));
  }
  constexpr void numeric(const line& l, reaction& r) {
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
      r.events.push_back(events::sasl_failed{n, l.params.empty() ? std::string() : l.params.back()});
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

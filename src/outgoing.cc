// SPDX-License-Identifier: AGPL-3.0-only
// cairn.outgoing -- What a client sends, made fit to send: a message too
// long for one line split at words (and never inside a UTF-8 character),
// as a lazy range of pieces; and the pace of sending held to what a server
// takes (a token bucket over the caller's clock).
export module cairn.outgoing;

import std;

export namespace cairn {

namespace detail {
constexpr bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
}  // namespace detail

// The pieces of a text, each at most `room` bytes: cut at the last space
// that fits, else at the last character boundary; spaces at a cut dropped.
// Views into the text, walked lazily.
class pieces_view : public std::ranges::view_interface<pieces_view> {
 public:
  class iterator {
   public:
    using value_type = std::string_view;
    using difference_type = std::ptrdiff_t;
    constexpr iterator() = default;
    constexpr iterator(std::string_view rest, std::size_t room) : rest_(rest), room_(std::max<std::size_t>(room, 1)) { next(); }
    constexpr std::string_view operator*() const { return current_; }
    constexpr iterator& operator++() {
      next();
      return *this;
    }
    constexpr void operator++(int) { next(); }
    constexpr bool operator==(std::default_sentinel_t) const { return done_; }

   private:
    constexpr void next() {
      while (!rest_.empty() && rest_.front() == ' ' && started_) rest_.remove_prefix(1);
      if (rest_.empty()) {
        done_ = started_ || true;
        if (!started_) {  // an empty text is one empty piece
          started_ = true;
          done_ = false;
          current_ = {};
          return;
        }
        return;
      }
      started_ = true;
      if (rest_.size() <= room_) {
        current_ = rest_;
        rest_ = {};
        return;
      }
      std::size_t cut = rest_.substr(0, room_ + 1).rfind(' ');
      if (cut == std::string_view::npos || cut == 0) {
        cut = room_;
        while (cut > 0 && detail::continuation(rest_[cut])) --cut;
        if (cut == 0) cut = room_;
      }
      current_ = rest_.substr(0, cut);
      rest_.remove_prefix(cut);
    }
    std::string_view rest_, current_;
    std::size_t room_ = 0;
    bool done_ = false, started_ = false;
  };
  constexpr pieces_view(std::string_view text, std::size_t room) : text_(text), room_(room) {}
  constexpr iterator begin() const { return iterator(text_, room_); }
  constexpr std::default_sentinel_t end() const { return {}; }

 private:
  std::string_view text_;
  std::size_t room_;
};
constexpr pieces_view pieces(std::string_view text, std::size_t room) { return pieces_view(text, room); }

// The room a PRIVMSG's text has in a line of 512 bytes, as the server will
// send it on: ":nick!user@host PRIVMSG target :" and CR LF taken off.
constexpr std::size_t text_room(std::string_view prefix, std::string_view command, std::string_view target,
                                std::size_t line = 512) {
  const std::size_t used = 1 + prefix.size() + 1 + command.size() + 1 + target.size() + 2 + 2;
  return used < line ? line - used : 0;
}

// The pace of sending: `burst` lines at once, then one each `interval`
// seconds -- times the caller's, in seconds on any clock.
class flood {
 public:
  constexpr flood(double interval = 0.5, double burst = 5) : interval_(interval), burst_(burst), tokens_(burst) {}
  // Whether a line may go now; if so it is counted.
  constexpr bool take(double now) {
    refill(now);
    if (tokens_ < 1) return false;
    tokens_ -= 1;
    return true;
  }
  // How long until the next may go.
  constexpr double wait(double now) {
    refill(now);
    return tokens_ >= 1 ? 0 : (1 - tokens_) * interval_;
  }

 private:
  constexpr void refill(double now) {
    // A clock that went back is not time given back: the last stays.
    if (last_ >= 0 && now > last_) tokens_ = std::min(burst_, tokens_ + (now - last_) / interval_);
    if (last_ < 0 || now > last_) last_ = now;
  }
  double interval_, burst_, tokens_, last_ = -1;
};

}  // namespace cairn

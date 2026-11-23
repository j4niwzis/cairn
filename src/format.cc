// SPDX-License-Identifier: AGPL-3.0-only
// cairn.format -- Text as IRC clients format it (mIRC's codes, as
// modern.ircdocs.horse/formatting gives them): ^B bold, ^] italic, ^_
// underline, ^^ strikethrough, ^Q monospace, ^V reverse, ^C colours
// (0-99, foreground and background), ^D colours in hex, ^O reset. Read as
// a lazy range of spans -- a style and the text it covers, a view into the
// message -- and written back from spans; nothing made on the way.
export module cairn.format;

import std;

export namespace cairn::format {

// A colour: one of the 99 numbered, or one given in hex (^D).
struct colour {
  int number = -1;        // 0-98, -1 none
  std::uint32_t rgb = 0;  // where `hex`
  bool hex = false;
  constexpr bool none() const { return number < 0 && !hex; }
  friend constexpr bool operator==(const colour&, const colour&) = default;
};

struct style {
  bool bold = false, italic = false, underline = false, strikethrough = false, monospace = false, reverse = false;
  colour foreground, background;
  friend constexpr bool operator==(const style&, const style&) = default;
};

struct span {
  cairn::format::style style;
  std::string_view text;
  friend constexpr bool operator==(const span&, const span&) = default;
};

inline constexpr char bold = '\x02', italic = '\x1d', underline = '\x1f', strikethrough = '\x1e', monospace = '\x11',
                      reverse = '\x16', colour_code = '\x03', hex_colour = '\x04', reset = '\x0f';

namespace detail {
constexpr bool digit(char c) { return c >= '0' && c <= '9'; }
constexpr int hexval(char c) {
  return digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}
constexpr bool code(char c) {
  return c == bold || c == italic || c == underline || c == strikethrough || c == monospace || c == reverse ||
         c == colour_code || c == hex_colour || c == reset;
}
// One or two digits: a colour's number. How many were read.
constexpr std::size_t number_at(std::string_view s, int& out) {
  if (s.empty() || !digit(s[0])) return 0;
  out = s[0] - '0';
  if (s.size() > 1 && digit(s[1])) return out = out * 10 + (s[1] - '0'), 2;
  return 1;
}
constexpr std::size_t hex_at(std::string_view s, std::uint32_t& out) {
  if (s.size() < 6) return 0;
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < 6; ++i) {
    const int h = hexval(s[i]);
    if (h < 0) return 0;
    v = v * 16 + static_cast<std::uint32_t>(h);
  }
  return out = v, 6;
}
}  // namespace detail

// The spans of a text, walked lazily: each run of text with the style in
// force over it; codes read and passed over, empty runs left out.
class spans_view : public std::ranges::view_interface<spans_view> {
 public:
  class iterator {
   public:
    using value_type = span;
    using difference_type = std::ptrdiff_t;
    constexpr iterator() = default;
    constexpr explicit iterator(std::string_view rest) : rest_(rest) { next(); }
    constexpr const span& operator*() const { return current_; }
    constexpr iterator& operator++() {
      next();
      return *this;
    }
    constexpr void operator++(int) { next(); }
    constexpr bool operator==(std::default_sentinel_t) const { return done_; }

   private:
    // The codes in front applied; then the text up to the next code.
    constexpr void next() {
      for (;;) {
        while (!rest_.empty() && detail::code(rest_.front())) apply();
        if (rest_.empty()) {
          done_ = true;
          return;
        }
        std::size_t n = 0;
        while (n < rest_.size() && !detail::code(rest_[n])) ++n;
        current_ = {now_, rest_.substr(0, n)};
        rest_.remove_prefix(n);
        return;
      }
    }
    constexpr void apply() {
      const char c = rest_.front();
      rest_.remove_prefix(1);
      switch (c) {
        case bold: now_.bold = !now_.bold; break;
        case italic: now_.italic = !now_.italic; break;
        case underline: now_.underline = !now_.underline; break;
        case strikethrough: now_.strikethrough = !now_.strikethrough; break;
        case monospace: now_.monospace = !now_.monospace; break;
        case reverse: now_.reverse = !now_.reverse; break;
        case reset: now_ = {}; break;
        case colour_code: {
          int fg = -1, bg = -1;
          const std::size_t f = detail::number_at(rest_, fg);
          if (f == 0) {  // ^C alone: colours off
            now_.foreground = now_.background = {};
            break;
          }
          rest_.remove_prefix(f);
          now_.foreground = colour{fg};
          if (rest_.size() > 1 && rest_[0] == ',' && detail::digit(rest_[1])) {
            rest_.remove_prefix(1);
            rest_.remove_prefix(detail::number_at(rest_, bg));
            now_.background = colour{bg};
          }
          break;
        }
        case hex_colour: {
          std::uint32_t fg = 0, bg = 0;
          const std::size_t f = detail::hex_at(rest_, fg);
          if (f == 0) {
            now_.foreground = now_.background = {};
            break;
          }
          rest_.remove_prefix(f);
          now_.foreground = colour{-1, fg, true};
          if (rest_.size() > 6 && rest_[0] == ',' && detail::hex_at(rest_.substr(1), bg)) {
            rest_.remove_prefix(7);
            now_.background = colour{-1, bg, true};
          }
          break;
        }
        default: break;
      }
    }
    std::string_view rest_;
    style now_;
    span current_;
    bool done_ = false;
  };
  constexpr spans_view() = default;
  constexpr explicit spans_view(std::string_view text) : text_(text) {}
  constexpr iterator begin() const { return iterator(text_); }
  constexpr std::default_sentinel_t end() const { return {}; }

 private:
  std::string_view text_;
};
constexpr spans_view spans(std::string_view text) { return spans_view(text); }

// The text without its codes, as a view of its characters.
constexpr auto plain(std::string_view text) {
  return spans(text) | std::views::transform(&span::text) | std::views::join;
}

// Spans written back with the codes that make them: each span's style set
// from the one before (the codes for what changed; ^O where turning things
// off is shorter), into anything that appends.
template <std::ranges::input_range R, class Out>
  requires std::same_as<std::ranges::range_value_t<R>, span>
constexpr void write(R&& all, Out& out) {
  style was;
  const auto toggle = [&](bool from, bool to, char c) {
    if (from != to) out.push_back(c);
  };
  const auto colours = [&](const style& to) {
    if (to.foreground == was.foreground && to.background == was.background) return;
    const auto number = [&](int n) {
      out.push_back(static_cast<char>('0' + n / 10));
      out.push_back(static_cast<char>('0' + n % 10));
    };
    const auto hex = [&](std::uint32_t v) {
      constexpr std::string_view digits = "0123456789ABCDEF";
      for (int shift = 20; shift >= 0; shift -= 4) out.push_back(digits[(v >> shift) & 15]);
    };
    if (to.foreground.none()) {
      out.push_back(colour_code);
      return;
    }
    out.push_back(to.foreground.hex ? hex_colour : colour_code);
    to.foreground.hex ? hex(to.foreground.rgb) : number(to.foreground.number);
    if (!to.background.none()) {
      out.push_back(',');
      to.background.hex ? hex(to.background.rgb) : number(to.background.number);
    }
  };
  for (const span& one : all) {
    if (one.style == style{} && !(was == style{})) {
      out.push_back(reset);
    } else {
      toggle(was.bold, one.style.bold, bold);
      toggle(was.italic, one.style.italic, italic);
      toggle(was.underline, one.style.underline, underline);
      toggle(was.strikethrough, one.style.strikethrough, strikethrough);
      toggle(was.monospace, one.style.monospace, monospace);
      toggle(was.reverse, one.style.reverse, reverse);
      colours(one.style);
    }
    was = one.style;
    for (char c : one.text) out.push_back(c);
  }
}

}  // namespace cairn::format

// SPDX-License-Identifier: AGPL-3.0-only
// cairn.read -- Lines read by walking cairn.grammar's description of a line:
// views into the text and the description's errors; thrown or handed back;
// from text or any input range.
export module cairn.read;

import std;
import splice;
import cairn.line;
import cairn.grammar;

export namespace cairn {

namespace detail {
// What the grammar read, as a line: the tags split, a key given twice
// counting once, the last time.
constexpr line_view assemble(const grammar::raw& got) {
  line_view out;
  if (got.tagged) out.tags_text = got.tags;
  if (got.sourced) out.from = parse_source(got.source);
  out.command = got.command;
  out.params_text = got.params;
  return out;
}
// Every tag has a key: "@=x" or "@a;=x" is no line (it could not be
// written back). Empty parts (";;") are nothing, and allowed.
constexpr bool keys_ok(std::string_view tags) {
  for (auto part : tags | std::views::split(';')) {
    const std::string_view one(part.begin(), part.end());
    if (!one.empty() && one.front() == '=') return false;
  }
  return true;
}
constexpr std::string_view without_line_end(std::string_view in) {
  while (!in.empty() && (in.back() == '\n' || in.back() == '\r')) in.remove_suffix(1);
  return in;
}
}  // namespace detail

namespace grammars {
// The description walked as it is: constexpr, the reference.
struct described {
  static constexpr std::expected<line_view, line_error> try_parse(std::string_view in) {
    in = detail::without_line_end(in);
    if (in.find_first_of(std::string_view("\0\r\n", 3)) != std::string_view::npos) return std::unexpected(errors::bad_character{});
    grammar::walk w{in};
    if (!grammar::walks<grammar::line>(w) || w.error) return std::unexpected(w.error.value_or(errors::empty{}));
    if (!detail::keys_ok(w.got.tags)) return std::unexpected(errors::bad_tags{});
    return detail::assemble(w.got);
  }
};

// The same grammar sliced by hand: a find(' ') a part (memchr), as the
// description reads it, and the same errors where it fails.
struct by_hand {
  static constexpr std::expected<line_view, line_error> try_parse(std::string_view in) {
    in = detail::without_line_end(in);
    // A NUL, a CR or an LF inside the line is no part of one (and could not be
    // written back).
    if (in.find_first_of(std::string_view("\0\r\n", 3)) != std::string_view::npos) return std::unexpected(errors::bad_character{});
    const auto skip_spaces = [&] { in.remove_prefix(std::min(in.find_first_not_of(' '), in.size())); };
    const auto word = [&] {
      const std::string_view got = in.substr(0, in.find(' '));
      in.remove_prefix(got.size());
      return got;
    };
    // Where a part is missing: at the end of the line, empty; else the error.
    const auto fail = [&]<class E>(E) -> std::unexpected<line_error> { return std::unexpected(grammar::error_at<E>(in)); };
    grammar::raw got;
    skip_spaces();
    if (in.starts_with('@')) {
      in.remove_prefix(1);
      got.tagged = true;
      got.tags = word();
      if (got.tags.empty()) return fail(errors::bad_tags{});
      skip_spaces();
    }
    if (in.starts_with(':')) {
      in.remove_prefix(1);
      got.sourced = true;
      got.source = word();
      if (got.source.empty()) return fail(errors::bad_source{});
      skip_spaces();
    }
    const std::string_view at_command = in;
    got.command = word();
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const bool named = !got.command.empty() && std::ranges::all_of(got.command, alpha);
    const bool numeric = got.command.size() == 3 && std::ranges::all_of(got.command, digit);
    if (!named && !numeric) {
      in = at_command;
      return fail(errors::bad_command{});
    }
    // The parameters: up to the end of the last (the trailing one to the end
    // of the line), the spaces after it left out.
    std::size_t end = 0;
    for (std::size_t at = 0;;) {
      at = in.find_first_not_of(' ', at);
      if (at == std::string_view::npos) break;
      if (in[at] == ':') {
        end = in.size();
        break;
      }
      at = std::min(in.find(' ', at), in.size());
      end = at;
    }
    got.params = in.substr(0, end);
    if (!detail::keys_ok(got.tags)) return std::unexpected(errors::bad_tags{});
    return detail::assemble(got);
  }
};

}  // namespace grammars

template <class Grammar = grammars::by_hand>
constexpr std::expected<line_view, line_error> try_parse(std::string_view in) {
  return Grammar::try_parse(in);
}
template <class Grammar = grammars::by_hand>
constexpr line_view parse(std::string_view in) {
  auto got = try_parse<Grammar>(in);
  if (!got) throw line_failure(std::move(got.error()));
  return *std::move(got);
}

// One line from an input range of characters, up to and with its LF: the
// iterator left after it, for the next. At the end of the input with nothing
// read: empty.
//
// Into room of the caller's -- a line_view into it, nothing asked of memory;
// a line longer than the room is too_long_to_read, the rest of it passed over.
template <class Grammar = grammars::by_hand, std::input_iterator I, std::sentinel_for<I> E>
  requires std::convertible_to<std::iter_reference_t<I>, char>
constexpr std::expected<line_view, line_error> try_read(I& at, E end, std::span<char> room) {
  std::size_t size = 0;
  bool fits = true;
  while (at != end) {
    const char c = *at;
    ++at;
    if (c == '\n') break;
    if (size < room.size())
      room[size++] = c;
    else
      fits = false;
  }
  if (!fits) return std::unexpected(errors::too_long_to_read{});
  return try_parse<Grammar>(std::string_view(room.data(), size));
}
template <class Grammar = grammars::by_hand, std::input_iterator I, std::sentinel_for<I> E>
  requires std::convertible_to<std::iter_reference_t<I>, char>
constexpr line_view read(I& at, E end, std::span<char> room) {
  auto got = try_read<Grammar>(at, end, room);
  if (!got) throw line_failure(std::move(got.error()));
  return *std::move(got);
}
// A line of its own (the owning way: memory asked for as it needs).
template <class Grammar = grammars::by_hand, std::input_iterator I, std::sentinel_for<I> E>
  requires std::convertible_to<std::iter_reference_t<I>, char>
constexpr std::expected<line, line_error> try_read(I& at, E end) {
  std::string text;
  while (at != end) {
    const char c = *at;
    ++at;
    if (c == '\n') break;
    text += c;
  }
  auto got = try_parse<Grammar>(text);
  if (!got) return std::unexpected(std::move(got.error()));
  return to_owned(*got);
}
template <class Grammar = grammars::by_hand, std::input_iterator I, std::sentinel_for<I> E>
  requires std::convertible_to<std::iter_reference_t<I>, char>
constexpr line read(I& at, E end) {
  auto got = try_read<Grammar>(at, end);
  if (!got) throw line_failure(std::move(got.error()));
  return *std::move(got);
}

// The lines of characters in a row, one at a time as they are walked: views
// into them, nothing copied and nothing asked of memory. Blank lines between
// are passed over.
template <class Grammar = grammars::by_hand>
class lines_view : public std::ranges::view_interface<lines_view<Grammar>> {
 public:
  constexpr lines_view() = default;
  constexpr explicit lines_view(std::string_view text) : text_(text) {}
  class iterator {
   public:
    using value_type = std::expected<line_view, line_error>;
    using difference_type = std::ptrdiff_t;
    constexpr iterator() = default;
    constexpr explicit iterator(std::string_view rest) : rest_(rest) { next(); }
    constexpr const value_type& operator*() const { return current_; }
    constexpr iterator& operator++() {
      next();
      return *this;
    }
    constexpr void operator++(int) { ++*this; }
    constexpr bool operator==(std::default_sentinel_t) const { return done_; }

   private:
    constexpr void next() {
      for (;;) {
        if (rest_.empty()) {
          done_ = true;
          return;
        }
        const auto lf = rest_.find('\n');
        const std::string_view one = rest_.substr(0, lf);
        rest_ = lf == std::string_view::npos ? std::string_view() : rest_.substr(lf + 1);
        current_ = try_parse<Grammar>(one);
        if (current_ || !spl::holds_alternative<errors::empty>(current_.error())) return;
      }
    }
    std::string_view rest_;
    value_type current_{std::unexpect, errors::empty{}};
    bool done_ = false;
  };
  constexpr iterator begin() const { return iterator(text_); }
  constexpr std::default_sentinel_t end() const { return {}; }

 private:
  std::string_view text_;
};
template <class Grammar = grammars::by_hand, std::ranges::contiguous_range R>
  requires std::same_as<std::ranges::range_value_t<R>, char>
constexpr lines_view<Grammar> lines(const R& text) {
  return lines_view<Grammar>(std::string_view(std::ranges::data(text), std::ranges::size(text)));
}

// The lines of any other input range, each read into room of the caller's
// and handed out as a view into it -- valid until the next. Nothing asked of
// memory, and no room of its own.
template <std::ranges::input_range R, class Grammar = grammars::by_hand>
  requires std::ranges::view<R>
class lines_into_view : public std::ranges::view_interface<lines_into_view<R, Grammar>> {
 public:
  constexpr lines_into_view() = default;
  constexpr lines_into_view(R input, std::span<char> room) : input_(std::move(input)), room_(room) {}
  class iterator {
   public:
    using value_type = std::expected<line_view, line_error>;
    using difference_type = std::ptrdiff_t;
    constexpr iterator() = default;
    constexpr explicit iterator(lines_into_view* of) : of_(of), at_(std::ranges::begin(of->input_)) { next(); }
    constexpr const value_type& operator*() const { return current_; }
    constexpr iterator& operator++() {
      next();
      return *this;
    }
    constexpr void operator++(int) { ++*this; }
    constexpr bool operator==(std::default_sentinel_t) const { return done_; }

   private:
    constexpr void next() {
      for (;;) {
        if (at_ == std::ranges::end(of_->input_)) {
          done_ = true;
          return;
        }
        current_ = try_read<Grammar>(at_, std::ranges::end(of_->input_), of_->room_);
        if (current_ || !spl::holds_alternative<errors::empty>(current_.error())) return;
      }
    }
    lines_into_view* of_ = nullptr;
    std::ranges::iterator_t<R> at_{};
    value_type current_{std::unexpect, errors::empty{}};
    bool done_ = false;
  };
  constexpr iterator begin() { return iterator(this); }
  constexpr std::default_sentinel_t end() const { return {}; }

 private:
  R input_;
  std::span<char> room_;
};
template <class Grammar = grammars::by_hand, std::ranges::viewable_range R>
constexpr auto lines(R&& r, std::span<char> room) {
  return lines_into_view<std::views::all_t<R>, Grammar>(std::views::all(std::forward<R>(r)), room);
}

}  // namespace cairn

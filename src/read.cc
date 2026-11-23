// SPDX-License-Identifier: AGPL-3.0-only
// cairn.read -- Lines read, by a grammar the caller names, each made from
// cairn.grammar's description of a line: by_hand walks it, std_regex is the
// regular expression it comes to. The same lines, views into the text, and
// the same errors; thrown or handed back; from text or any input range.
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
  if (got.tagged) {
    for (auto part : got.tags | std::views::split(';')) {
      const std::string_view one(part);
      if (one.empty()) continue;
      const auto eq = one.find('=');
      tag_view t{one.substr(0, eq), eq == std::string_view::npos ? std::string_view() : one.substr(eq + 1)};
      if (auto same = std::ranges::find(out.tags, t.key, &tag_view::key); same != out.tags.end())
        *same = t;
      else
        out.tags.push_back(t);
    }
  }
  if (got.sourced) out.from = parse_source(got.source);
  out.command = got.command;
  out.params = got.params;
  return out;
}
constexpr std::string_view without_line_end(std::string_view in) {
  while (!in.empty() && (in.back() == '\n' || in.back() == '\r')) in.remove_suffix(1);
  return in;
}
}  // namespace detail

namespace grammars {
// The description walked as it is: constexpr, the reference.
struct by_hand {
  static constexpr std::expected<line_view, line_error> try_parse(std::string_view in) {
    grammar::walk w{detail::without_line_end(in)};
    if (!grammar::walks<grammar::line>(w) || w.error) return std::unexpected(w.error.value_or(errors::empty{}));
    return detail::assemble(w.got);
  }
};

// The description as std::regex: a regular expression for each part of the
// line, made from it while the program compiles (the text) and once when the
// program first reads (the automaton). A list -- the parameters -- read a
// turn at a time, as no group of one keeps more than its last. Where the
// line is not one, the error is the description's, walked.
struct std_regex {
 private:
  template <class Tuple, std::size_t... I>
  static void assign(grammar::raw& got, const std::match_results<std::string_view::const_iterator>& m, std::index_sequence<I...>) {
    (([&] {
       constexpr auto field = std::tuple_element_t<I, Tuple>::value;
       if (!m[I + 1].matched) return;
       const std::string_view text(m[I + 1].first, m[I + 1].second);
       if constexpr (std::same_as<std::remove_cvref_t<decltype(got.*field)>, std::string_view>)
         got.*field = text;
       else
         (got.*field).push_back(text);
     }()),
     ...);
  }
  template <class Tuple, std::size_t... I> static void raise(grammar::raw& got, std::index_sequence<I...>) {
    ((got.*(std::tuple_element_t<I, Tuple>::value) = true), ...);
  }
  // One part, read at `at`: whether it read.
  template <class N> static bool part(std::string_view in, std::string_view::const_iterator& at, grammar::raw& got) {
    static const std::regex expression(grammar::regex<N>());
    std::match_results<std::string_view::const_iterator> m;
    if (!std::regex_search(at, in.end(), m, expression, std::regex_constants::match_continuous)) return false;
    using fields = typename grammar::captures_of<N>::type;
    assign<fields>(got, m, std::make_index_sequence<std::tuple_size_v<fields>>{});
    using flags = typename grammar::flags_of<N>::type;
    raise<flags>(got, std::make_index_sequence<std::tuple_size_v<flags>>{});
    at = m[0].second;
    return true;
  }
  template <class N> struct top { static bool go(std::string_view in, std::string_view::const_iterator& at, grammar::raw& got) { return part<N>(in, at, got); } };
  template <class N> struct top<grammar::opt<N>> {
    static bool go(std::string_view in, std::string_view::const_iterator& at, grammar::raw& got) { return part<N>(in, at, got), true; }
  };
  template <class N> struct top<grammar::many<N>> {
    static bool go(std::string_view in, std::string_view::const_iterator& at, grammar::raw& got) {
      while (at != in.end() && part<N>(in, at, got)) {}
      return true;
    }
  };
  template <class... N> static bool all(std::string_view in, grammar::seq<N...>*, grammar::raw& got) {
    auto at = in.begin();
    return (top<N>::go(in, at, got) && ...) && at == in.end();
  }

 public:
  // The regular expressions alone: a line, or none where they do not read it.
  static std::optional<line_view> read_alone(std::string_view in) {
    in = detail::without_line_end(in);
    grammar::raw got;
    if (!all(in, static_cast<grammar::line*>(nullptr), got)) return std::nullopt;
    return detail::assemble(got);
  }
  static std::expected<line_view, line_error> try_parse(std::string_view in) {
    if (auto got = read_alone(in)) return *std::move(got);
    return by_hand::try_parse(in);
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
// iterator left after it, for the next. A line of its own -- the range need
// not keep what it gave. At the end of the input with nothing read: empty.
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

// The lines of an input range, one at a time as they are walked: each a
// line, or why it is none. Blank lines between are passed over.
template <std::ranges::input_range R, class Grammar = grammars::by_hand>
  requires std::ranges::view<R>
class lines_view : public std::ranges::view_interface<lines_view<R>> {
 public:
  constexpr lines_view() = default;
  constexpr explicit lines_view(R input) : input_(std::move(input)) {}
  class iterator {
   public:
    using value_type = std::expected<line, line_error>;
    using difference_type = std::ptrdiff_t;
    constexpr iterator() = default;
    constexpr explicit iterator(lines_view* of) : of_(of), at_(std::ranges::begin(of->input_)) { next(); }
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
        current_ = try_read<Grammar>(at_, std::ranges::end(of_->input_));
        if (current_ || !spl::holds_alternative<errors::empty>(current_.error())) return;
      }
    }
    lines_view* of_ = nullptr;
    std::ranges::iterator_t<R> at_{};
    value_type current_{std::unexpect, errors::empty{}};
    bool done_ = false;
  };
  constexpr iterator begin() { return iterator(this); }
  constexpr std::default_sentinel_t end() const { return {}; }

 private:
  R input_;
};
template <class Grammar = grammars::by_hand, std::ranges::viewable_range R>
constexpr auto lines(R&& r) { return lines_view<std::views::all_t<R>, Grammar>(std::views::all(std::forward<R>(r))); }

}  // namespace cairn

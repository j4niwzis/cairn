// SPDX-License-Identifier: AGPL-3.0-only
// cairn.scan -- A line read by scan's automaton: each part of the line's
// description written as a scan pattern while the program compiles, and
// built by scan into a tagged deterministic automaton walked state by state.
// Built where CAIRN_SCAN is on; the same lines as the walk, and its errors.
export module cairn.scan;

import std;
import splice;
import scan;
import cairn.line;
import cairn.grammar;
import cairn.read;

export namespace cairn::grammars {

namespace detail {
// A part's pattern, in scan's dialect, as the template argument scan takes.
template <class N> consteval auto pattern_of() {
  constexpr std::size_t size = grammar::regex<N, grammar::scan_dialect>().size() + 1;
  char text[size]{};
  const std::string made = grammar::regex<N, grammar::scan_dialect>();
  std::ranges::copy(made, text);
  return ::scan::fixed_string<size>(text);
}
}  // namespace detail

struct scan {
 private:
  using it = std::string_view::const_iterator;
  template <class Tuple, std::size_t... I, class R>
  static constexpr void assign(grammar::raw& got, const R& m, std::index_sequence<I...>) {
    (([&] {
       constexpr auto field = std::tuple_element_t<I, Tuple>::value;
       const auto& group = m.template get<I + 1>();
       if (!group) return;
       const std::string_view text = group.to_view();
       got.*field = text;
     }()),
     ...);
  }
  template <class Tuple, std::size_t... I> static constexpr void raise(grammar::raw& got, std::index_sequence<I...>) {
    ((got.*(std::tuple_element_t<I, Tuple>::value) = true), ...);
  }
  // One part, read at `at`: whether it read.
  template <class N> static constexpr bool part(std::string_view in, std::size_t& at, grammar::raw& got) {
    const std::string_view here = in.substr(at);
    const auto m = ::scan::starts_with<detail::pattern_of<N>()>(here);
    if (!m) return false;
    const std::size_t read = m.whole().size();
    // What scan has no way to say -- that a space or the end follows.
    if constexpr (grammar::ends_word<N>::value)
      if (read < here.size() && here[read] != ' ') return false;
    using fields = typename grammar::captures_of<N>::type;
    assign<fields>(got, m, std::make_index_sequence<std::tuple_size_v<fields>>{});
    using flags = typename grammar::flags_of<N>::type;
    raise<flags>(got, std::make_index_sequence<std::tuple_size_v<flags>>{});
    at += read;
    return true;
  }
  template <class N> struct top { static constexpr bool go(std::string_view in, std::size_t& at, grammar::raw& got) { return part<N>(in, at, got); } };
  template <class N> struct top<grammar::opt<N>> {
    static constexpr bool go(std::string_view in, std::size_t& at, grammar::raw& got) { return part<N>(in, at, got), true; }
  };
  template <class N> struct top<grammar::many<N>> {
    static constexpr bool go(std::string_view in, std::size_t& at, grammar::raw& got) {
      while (at < in.size() && part<N>(in, at, got)) {}
      return true;
    }
  };
  template <class... N> static constexpr bool all(std::string_view in, grammar::seq<N...>*, grammar::raw& got) {
    std::size_t at = 0;
    return (top<N>::go(in, at, got) && ...) && at == in.size();
  }

 public:
  // The automata alone: a line, or none where they do not read it.
  static constexpr std::optional<line_view> read_alone(std::string_view in) {
    while (!in.empty() && (in.back() == '\n' || in.back() == '\r')) in.remove_suffix(1);
    grammar::raw got;
    if (!all(in, static_cast<grammar::line*>(nullptr), got)) return std::nullopt;
    return cairn::detail::assemble(got);
  }
  static constexpr std::expected<line_view, line_error> try_parse(std::string_view in) {
    if (auto got = read_alone(in)) return *std::move(got);
    return by_hand::try_parse(in);
  }
};

}  // namespace cairn::grammars

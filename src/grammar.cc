// SPDX-License-Identifier: AGPL-3.0-only
// cairn.grammar -- How an IRC line is written, said once, as data: a small
// grammar of types, and the line's grammar in it. Every reader is made from
// it while the program compiles -- walked as it is (by hand), as the
// std::regex it comes to, as the format scan builds its automaton from --
// so all read the same lines and fail with the same errors.
export module cairn.grammar;

import std;
import splice;
import cairn.line;

export namespace cairn::grammar {

// A string as a template argument.
template <std::size_t N> struct fixed_string {
  char chars[N]{};
  constexpr fixed_string(const char (&s)[N]) { std::copy_n(s, N, chars); }
  constexpr std::string_view view() const { return {chars, N - 1}; }
};

// The pieces.
template <char C> struct lit {};                    // this character
template <fixed_string S> struct not_of {};          // any one character but these
struct alpha {};                                     // A-Z a-z
struct digit {};                                     // 0-9
template <class... N> struct seq {};                 // one after another
template <class... N> struct alt {};                 // the first that reads, in order
template <class N> struct opt {};                    // once or not at all
template <class N> struct many {};                   // as often as it reads
template <class N> struct many1 {};                  // once, then as often as it reads
template <class N, std::size_t K> struct repeat {};  // K times
struct rest {};                                      // everything to the end
struct word_end {};                                  // a space next, or the end: nothing read
// What N read, kept in a field of what is read into (a view, or one more of
// a list of views).
template <auto Field, class N> struct capture {};
// N must read here; where it does not, the line is this error -- made from
// the word it stopped at where it takes one. At the end of the line, empty.
template <class Error, class N> struct expect {};

using spaces = many1<lit<' '>>;
using word = many1<not_of<" ">>;

// What the line's grammar reads into: views into the text.
struct raw {
  std::string_view tags;
  std::string_view source;
  std::string_view command;
  std::string_view params;  // the text of them all, read lazily after
  bool tagged = false, sourced = false;
};
template <auto Flag, class N> struct flag {};  // N read: Flag set

// The line (RFC 1459 as it is written now, and IRCv3 message tags).
using line = seq<
    opt<spaces>,
    opt<seq<lit<'@'>, flag<&raw::tagged, expect<errors::bad_tags, capture<&raw::tags, word>>>, opt<spaces>>>,
    opt<seq<lit<':'>, flag<&raw::sourced, expect<errors::bad_source, capture<&raw::source, word>>>, opt<spaces>>>,
    expect<errors::bad_command, seq<capture<&raw::command, alt<many1<alpha>, repeat<digit, 3>>>, word_end>>,
    capture<&raw::params, many<seq<spaces, alt<seq<lit<':'>, rest>, word>>>>,
    opt<spaces>>;

// ---- walked: the grammar read as it is -------------------------------------

struct walk {
  std::string_view in;
  std::size_t at = 0;
  cairn::grammar::raw got;
  std::optional<line_error> error;
};

template <class E> constexpr line_error error_at(std::string_view rest_of_line) {
  if (rest_of_line.empty()) return errors::empty{};
  if constexpr (std::is_constructible_v<E, std::string_view>)
    return E{rest_of_line.substr(0, rest_of_line.find(' '))};
  else
    return E{};
}

template <class N> struct walker;
template <class N> constexpr bool walks(walk& w) { return walker<N>::go(w); }

template <char C> struct walker<lit<C>> {
  static constexpr bool go(walk& w) {
    if (w.at < w.in.size() && w.in[w.at] == C) return ++w.at, true;
    return false;
  }
};
template <fixed_string S> struct walker<not_of<S>> {
  static constexpr bool go(walk& w) {
    if (w.at < w.in.size() && S.view().find(w.in[w.at]) == std::string_view::npos) return ++w.at, true;
    return false;
  }
};
template <> struct walker<alpha> {
  static constexpr bool go(walk& w) {
    if (w.at < w.in.size() && ((w.in[w.at] >= 'A' && w.in[w.at] <= 'Z') || (w.in[w.at] >= 'a' && w.in[w.at] <= 'z'))) return ++w.at, true;
    return false;
  }
};
template <> struct walker<digit> {
  static constexpr bool go(walk& w) {
    if (w.at < w.in.size() && w.in[w.at] >= '0' && w.in[w.at] <= '9') return ++w.at, true;
    return false;
  }
};
template <class... N> struct walker<seq<N...>> {
  static constexpr bool go(walk& w) {
    const std::size_t from = w.at;
    const cairn::grammar::raw kept = w.got;
    if ((walks<N>(w) && ...)) return true;
    if (!w.error) w.at = from, w.got = kept;
    return false;
  }
};
template <class... N> struct walker<alt<N...>> {
  static constexpr bool go(walk& w) { return ((walks<N>(w) || w.error) || ...) && !w.error; }
};
template <class N> struct walker<opt<N>> {
  static constexpr bool go(walk& w) { return walks<N>(w), !w.error; }
};
template <class N> struct walker<many<N>> {
  static constexpr bool go(walk& w) {
    while (walks<N>(w)) {}
    return !w.error;
  }
};
template <class N> struct walker<many1<N>> {
  static constexpr bool go(walk& w) { return walks<N>(w) && walks<many<N>>(w); }
};
template <class N, std::size_t K> struct walker<repeat<N, K>> {
  static constexpr bool go(walk& w) {
    for (std::size_t i = 0; i < K; ++i)
      if (!walks<N>(w)) return false;
    return true;
  }
};
template <> struct walker<rest> {
  static constexpr bool go(walk& w) { return w.at = w.in.size(), true; }
};
template <> struct walker<word_end> {
  static constexpr bool go(walk& w) { return w.at == w.in.size() || w.in[w.at] == ' '; }
};
template <auto Field, class N> struct walker<capture<Field, N>> {
  static constexpr bool go(walk& w) {
    const std::size_t from = w.at;
    if (!walks<N>(w)) return false;
    const std::string_view text = w.in.substr(from, w.at - from);
    w.got.*Field = text;
    return true;
  }
};
template <auto Flag, class N> struct walker<flag<Flag, N>> {
  static constexpr bool go(walk& w) {
    w.got.*Flag = true;
    return walks<N>(w);
  }
};
template <class E, class N> struct walker<expect<E, N>> {
  static constexpr bool go(walk& w) {
    const std::size_t from = w.at;
    if (walks<N>(w)) return true;
    if (!w.error) w.error = error_at<E>(w.in.substr(from));
    return false;
  }
};

// ---- as a regular expression ----------------------------------------------

// The dialects it is written in: what each says for what the other has not.
// std::regex has lookahead; scan has none, and a word's end is checked by
// what reads with it, after the part.
struct ecmascript { static constexpr std::string_view word_end = "(?= |$)"; };
struct scan_dialect { static constexpr std::string_view word_end = ""; };
template <class N, class D> struct regex_of;
template <class N, class D = ecmascript> constexpr std::string regex() { return regex_of<N, D>::text(); }
constexpr std::string escaped_char(char c) {
  return std::string_view("\\^$.|?*+()[]{}/-").find(c) != std::string_view::npos ? std::string{'\\', c} : std::string{c};
}
template <char C, class D> struct regex_of<lit<C>, D> { static constexpr std::string text() { return escaped_char(C); } };
template <fixed_string S, class D> struct regex_of<not_of<S>, D> {
  static constexpr std::string text() {
    std::string out = "[^";
    for (char c : S.view()) out += escaped_char(c);
    return out + "]";
  }
};
template <class D> struct regex_of<alpha, D> { static constexpr std::string text() { return "[A-Za-z]"; } };
template <class D> struct regex_of<digit, D> { static constexpr std::string text() { return "[0-9]"; } };
template <class... N, class D> struct regex_of<seq<N...>, D> { static constexpr std::string text() { return (regex<N, D>() + ...); } };
template <class... N, class D> struct regex_of<alt<N...>, D> {
  static constexpr std::string text() {
    std::string out = "(?:";
    bool first = true;
    ((out += (first ? "" : "|") + regex<N, D>(), first = false), ...);
    return out + ")";
  }
};
template <class N, class D> struct regex_of<opt<N>, D> { static constexpr std::string text() { return "(?:" + regex<N, D>() + ")?"; } };
template <class N, class D> struct regex_of<many<N>, D> { static constexpr std::string text() { return "(?:" + regex<N, D>() + ")*"; } };
template <class N, class D> struct regex_of<many1<N>, D> { static constexpr std::string text() { return "(?:" + regex<N, D>() + ")+"; } };
template <class N, std::size_t K, class D> struct regex_of<repeat<N, K>, D> {
  static constexpr std::string text() {
    std::string out;
    for (std::size_t i = 0; i < K; ++i) out += regex<N, D>();
    return out;
  }
};
template <class D> struct regex_of<rest, D> { static constexpr std::string text() { return ".*"; } };
template <class D> struct regex_of<word_end, D> { static constexpr std::string text() { return std::string(D::word_end); } };
template <auto Field, class N, class D> struct regex_of<capture<Field, N>, D> {
  static constexpr std::string text() { return "(" + regex<N, D>() + ")"; }
};
template <auto Flag, class N, class D> struct regex_of<flag<Flag, N>, D> { static constexpr std::string text() { return regex<N, D>(); } };
template <class E, class N, class D> struct regex_of<expect<E, N>, D> { static constexpr std::string text() { return regex<N, D>(); } };

// Whether a part asks for a word's end after it.
template <class N> struct ends_word : std::false_type {};
template <> struct ends_word<word_end> : std::true_type {};
template <class... N> struct ends_word<seq<N...>> : ends_word<std::tuple_element_t<sizeof...(N) - 1, std::tuple<N...>>> {};
template <class E, class N> struct ends_word<expect<E, N>> : ends_word<N> {};
template <auto F, class N> struct ends_word<flag<F, N>> : ends_word<N> {};

// The fields a piece captures into, in the order of its groups.
template <class N> struct captures_of { using type = std::tuple<>; };
template <class... T> using cat_t = decltype(std::tuple_cat(std::declval<T>()...));
template <class... N> struct captures_of<seq<N...>> { using type = cat_t<typename captures_of<N>::type...>; };
template <class... N> struct captures_of<alt<N...>> { using type = cat_t<typename captures_of<N>::type...>; };
template <class N> struct captures_of<opt<N>> : captures_of<N> {};
template <class N> struct captures_of<many<N>> : captures_of<N> {};
template <class N> struct captures_of<many1<N>> : captures_of<N> {};
template <class N, std::size_t K> struct captures_of<repeat<N, K>> : captures_of<N> {};
template <auto Flag, class N> struct captures_of<flag<Flag, N>> : captures_of<N> {};
template <class E, class N> struct captures_of<expect<E, N>> : captures_of<N> {};
template <auto Field> struct field_tag { static constexpr auto value = Field; };
template <auto Field, class N> struct captures_of<capture<Field, N>> {
  using type = cat_t<std::tuple<field_tag<Field>>, typename captures_of<N>::type>;
};
template <class N> struct flags_of { using type = std::tuple<>; };
template <class... N> struct flags_of<seq<N...>> { using type = cat_t<typename flags_of<N>::type...>; };
template <class N> struct flags_of<opt<N>> : flags_of<N> {};
template <auto Flag, class N> struct flags_of<flag<Flag, N>> { using type = std::tuple<field_tag<Flag>>; };

}  // namespace cairn::grammar

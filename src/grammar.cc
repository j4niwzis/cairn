// SPDX-License-Identifier: AGPL-3.0-only
// cairn.grammar -- How an IRC line is written, said once, as data: a small
// grammar of types, and the line's grammar in it. Every reader is made from
// it -- walked as it is -- so what it reads and the errors it fails with
// are the description's.
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

template <class E>
  requires std::constructible_from<E, std::string_view>
constexpr E made_at(std::string_view rest_of_line) { return E{rest_of_line.substr(0, rest_of_line.find(' '))}; }
template <class E> constexpr E made_at(std::string_view) { return E{}; }
// At the end of the line: empty. Else the error, made from the word it
// stopped at where it takes one.
template <class E> constexpr line_error error_at(std::string_view rest_of_line) {
  if (rest_of_line.empty()) return errors::empty{};
  return made_at<E>(rest_of_line);
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

}  // namespace cairn::grammar

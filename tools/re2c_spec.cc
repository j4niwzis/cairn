// SPDX-License-Identifier: AGPL-3.0-only
// Writes cairn.re2c's source for re2c: the line's description (cairn.grammar)
// as an re2c rule, inside the module that reads with the scanner re2c makes.
// Run where cairn is built; its output goes through re2c, and that into the
// library.
import std;
import cairn;

int main() {
  int tag = 0;
  const std::string rule = cairn::grammar::re2c<cairn::grammar::line>(tag);
  std::string text = R"spec(// Made by tools/re2c_spec.cc and re2c from cairn.grammar's description of
// a line: do not edit.
export module cairn.re2c;

import std;
import cairn.line;
import cairn.grammar;
import cairn.read;

export namespace cairn::grammars {

// A line read by the scanner re2c made of the description, entered once and
// anchored at the line's end: read straight from the text, no copy and no
// sentinel -- its end peeked at as a NUL that is not there.
struct re2c {
  static std::optional<line_view> read_alone(std::string_view in) {
    while (!in.empty() && (in.back() == '\n' || in.back() == '\r')) in.remove_suffix(1);
    constexpr std::size_t none = static_cast<std::size_t>(-1);
    const std::size_t n = in.size();
    std::size_t i = 0, mark = 0;
    /*!stags:re2c format = "std::size_t @@ = none;\n"; */
    std::size_t c0 = none, c1 = none, c2 = none, c3 = none, c4 = none, c5 = none, c6 = none, c7 = none;  // the captures' tags
    const auto piece = [&](std::size_t from, std::size_t to) {
      return from == none || to == none ? std::string_view() : in.substr(from, to - from);
    };
    /*!re2c
      re2c:api = custom;
      re2c:api:style = free-form;
      re2c:define:YYCTYPE = char;
      re2c:define:YYPEEK = "(i < n ? in[i] : char(0))";
      re2c:define:YYSKIP = "++i;";
      re2c:define:YYBACKUP = "mark = i;";
      re2c:define:YYRESTORE = "i = mark;";
      re2c:define:YYSTAGP = "@@{tag} = i;";
      re2c:define:YYSTAGN = "@@{tag} = none;";
      re2c:define:YYSHIFTSTAG = "if (@@{tag} != none) @@{tag} += @@{shift};";
      re2c:yyfill:enable = 0;
      re2c:tags = 1;

      line = @RULE@;

      line "\x00" {
        if (i != n + 1) return std::nullopt;  // a NUL of the text's own
        grammar::raw got;
        got.tags = piece(c0, c1);
        got.source = piece(c2, c3);
        got.command = piece(c4, c5);
        got.params = piece(c6, c7);
        got.tagged = !got.tags.empty();
        got.sourced = !got.source.empty();
        return cairn::detail::assemble(got);
      }
      * { return std::nullopt; }
    */
  }
  static std::expected<line_view, line_error> try_parse(std::string_view in) {
    if (auto got = read_alone(in)) return *std::move(got);
    return by_hand::try_parse(in);
  }
};

}  // namespace cairn::grammars
)spec";
  text.replace(text.find("@RULE@"), 6, rule);
  std::print("{}", text);
  if (tag != 8) {
    std::println(std::cerr, "re2c_spec: the line has {} capture tags, 8 expected", tag);
    return 1;
  }
}

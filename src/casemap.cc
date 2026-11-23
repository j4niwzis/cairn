// SPDX-License-Identifier: AGPL-3.0-only
// cairn.casemap -- Nicks and channels compared as the server says
// (ISUPPORT CASEMAPPING): #Mux and #mux one channel; [ ] \ ~ the lower case
// of { } | ^ under rfc1459.
export module cairn.casemap;

import std;

export namespace cairn {

enum class casemapping {
  ascii,           // A-Z only
  rfc1459,         // A-Z and []\~ as {}|^
  strict_rfc1459,  // A-Z and []\ as {}|, not ~ and ^
};

constexpr std::optional<casemapping> casemapping_named(std::string_view name) {
  if (name == "ascii") return casemapping::ascii;
  if (name == "rfc1459") return casemapping::rfc1459;
  if (name == "strict-rfc1459") return casemapping::strict_rfc1459;
  // rfc7613 (PRECIS) and others: folded as ascii, the part every one shares.
  if (name == "rfc7613") return casemapping::ascii;
  return std::nullopt;
}

constexpr char fold(char c, casemapping m) {
  if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
  if (m == casemapping::ascii) return c;
  switch (c) {
    case '[': return '{';
    case ']': return '}';
    case '\\': return '|';
    case '~': return m == casemapping::rfc1459 ? '^' : c;
    default: return c;
  }
}
constexpr std::string fold(std::string_view s, casemapping m) {
  std::string out(s);
  for (char& c : out) c = fold(c, m);
  return out;
}
constexpr bool equal(std::string_view a, std::string_view b, casemapping m) {
  return a.size() == b.size() && std::ranges::equal(a, b, [m](char x, char y) { return fold(x, m) == fold(y, m); });
}

// A name as a key: folded once, compared as it is.
struct folded {
  std::string key;
  constexpr folded(std::string_view s, casemapping m) : key(fold(s, m)) {}
  friend constexpr auto operator<=>(const folded&, const folded&) = default;
};

}  // namespace cairn

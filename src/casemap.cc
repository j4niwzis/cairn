// SPDX-License-Identifier: AGPL-3.0-only
// cairn.casemap -- Nicks and channels compared as the server says
// (ISUPPORT CASEMAPPING): #Mux and #mux one channel; [ ] \ ~ the lower case
// of { } | ^ under rfc1459. Each mapping a type of its own; folded names are
// views, a character folded as it is walked.
export module cairn.casemap;

import std;
import splice;

export namespace cairn {
namespace casemappings {
// A-Z only.
struct ascii {
  static constexpr std::string_view name = "ascii";
  static constexpr char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
};
// A-Z, and [ ] \ ~ as { } | ^.
struct rfc1459 {
  static constexpr std::string_view name = "rfc1459";
  static constexpr char fold(char c) {
    switch (c) {
      case '[': return '{';
      case ']': return '}';
      case '\\': return '|';
      case '~': return '^';
      default: return ascii::fold(c);
    }
  }
};
// A-Z, and [ ] \ as { } |, not ~ and ^.
struct strict_rfc1459 {
  static constexpr std::string_view name = "strict-rfc1459";
  static constexpr char fold(char c) { return c == '~' ? c : rfc1459::fold(c); }
};
constexpr bool operator==(ascii, ascii) { return true; }
constexpr bool operator==(rfc1459, rfc1459) { return true; }
constexpr bool operator==(strict_rfc1459, strict_rfc1459) { return true; }
}  // namespace casemappings

using casemapping = spl::variant<casemappings::rfc1459, casemappings::ascii, casemappings::strict_rfc1459>;

// The mapping a name stands for; rfc7613 (PRECIS) and the like folded as
// ascii, the part every one shares.
constexpr std::optional<casemapping> casemapping_named(std::string_view name) {
  if (name == casemappings::ascii::name || name == "rfc7613") return casemappings::ascii{};
  if (name == casemappings::rfc1459::name) return casemappings::rfc1459{};
  if (name == casemappings::strict_rfc1459::name) return casemappings::strict_rfc1459{};
  return std::nullopt;
}

constexpr char fold(char c, const casemapping& m) {
  return spl::visit([c](auto one) { return decltype(one)::fold(c); }, m);
}
// A name folded, as a view.
constexpr auto folded(std::string_view s, casemapping m) {
  return s | std::views::transform([m](char c) { return fold(c, m); });
}
constexpr bool equal(std::string_view a, std::string_view b, const casemapping& m) {
  return std::ranges::equal(folded(a, m), folded(b, m));
}

// A name as a key in a container: folded once, compared as it is.
struct key {
  std::string folded_name;
  constexpr key(std::string_view s, const casemapping& m) : folded_name(std::ranges::to<std::string>(folded(s, m))) {}
  friend constexpr auto operator<=>(const key&, const key&) = default;
};

}  // namespace cairn

// SPDX-License-Identifier: AGPL-3.0-only
// cairn.text -- What the bytes of a message mean. IRC carries bytes; most
// clients now send UTF-8, older ones Latin-1 (or CP1252). A message that is
// well-formed UTF-8 is read as UTF-8; one that is not is read as Latin-1, a
// byte a character -- what clients do (modern.ircdocs.horse, "Character
// encodings"). Read as a lazy range of code points over the caller's bytes;
// nothing made on the way.
export module cairn.text;

import std;
import splice;

export namespace cairn::text {

namespace detail {
// The length of the UTF-8 sequence at `at`, or 0 where it is not one
// (overlong, a surrogate, past U+10FFFF, cut short).
constexpr std::size_t sequence(std::string_view s, std::size_t at) {
  const auto b = [&](std::size_t i) { return static_cast<unsigned char>(s[at + i]); };
  const auto tail = [&](std::size_t i) { return at + i < s.size() && (b(i) & 0xC0) == 0x80; };
  const unsigned char c = b(0);
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) return tail(1) ? 2 : 0;
  if (c >= 0xE0 && c <= 0xEF) {
    if (!tail(1) || !tail(2)) return 0;
    if (c == 0xE0 && b(1) < 0xA0) return 0;  // overlong
    if (c == 0xED && b(1) > 0x9F) return 0;  // a surrogate
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (!tail(1) || !tail(2) || !tail(3)) return 0;
    if (c == 0xF0 && b(1) < 0x90) return 0;  // overlong
    if (c == 0xF4 && b(1) > 0x8F) return 0;  // past U+10FFFF
    return 4;
  }
  return 0;
}
constexpr char32_t decode(std::string_view s, std::size_t at, std::size_t n) {
  const auto b = [&](std::size_t i) { return static_cast<char32_t>(static_cast<unsigned char>(s[at + i])); };
  switch (n) {
    case 1: return b(0);
    case 2: return (b(0) & 0x1F) << 6 | (b(1) & 0x3F);
    case 3: return (b(0) & 0x0F) << 12 | (b(1) & 0x3F) << 6 | (b(2) & 0x3F);
    default: return (b(0) & 0x07) << 18 | (b(1) & 0x3F) << 12 | (b(2) & 0x3F) << 6 | (b(3) & 0x3F);
  }
}
}  // namespace detail

// Whether the bytes are well-formed UTF-8.
constexpr bool is_utf8(std::string_view s) {
  for (std::size_t at = 0; at < s.size();) {
    const std::size_t n = detail::sequence(s, at);
    if (n == 0) return false;
    at += n;
  }
  return true;
}

// How a text is read: as UTF-8, or as Latin-1.
namespace encodings {
struct utf8 {};
struct latin1 {};
}  // namespace encodings
using encoding = spl::variant<encodings::utf8, encodings::latin1>;

// What a message is read as: UTF-8 where it is well-formed, else Latin-1.
constexpr encoding encoding_of(std::string_view s) {
  if (is_utf8(s)) return encodings::utf8{};
  return encodings::latin1{};
}

// The code points of a text, read as `how`, lazily: a view over the bytes.
class decoded_view : public std::ranges::view_interface<decoded_view> {
 public:
  class iterator {
   public:
    using value_type = char32_t;
    using difference_type = std::ptrdiff_t;
    constexpr iterator() = default;
    constexpr iterator(std::string_view s, std::size_t at, bool utf8) : s_(s), at_(at), utf8_(utf8) {}
    constexpr char32_t operator*() const {
      if (!utf8_) return static_cast<unsigned char>(s_[at_]);
      return detail::decode(s_, at_, detail::sequence(s_, at_));
    }
    constexpr iterator& operator++() {
      at_ += utf8_ ? detail::sequence(s_, at_) : 1;
      return *this;
    }
    constexpr iterator operator++(int) {
      auto was = *this;
      ++*this;
      return was;
    }
    // Where in the bytes it is.
    constexpr std::size_t offset() const { return at_; }
    friend constexpr bool operator==(const iterator& a, const iterator& b) { return a.at_ == b.at_; }

   private:
    std::string_view s_;
    std::size_t at_ = 0;
    bool utf8_ = true;
  };
  constexpr decoded_view() = default;
  constexpr decoded_view(std::string_view s, const encoding& how)
      : s_(s), utf8_(spl::visit(spl::overloaded{[](encodings::utf8) { return true; }, [](encodings::latin1) { return false; }}, how)) {}
  constexpr iterator begin() const { return {s_, 0, utf8_}; }
  constexpr iterator end() const { return {s_, s_.size(), utf8_}; }

 private:
  std::string_view s_;
  bool utf8_ = true;
};
constexpr decoded_view decoded(std::string_view s, const encoding& how) { return {s, how}; }
// Read as clients read it: UTF-8 if it is that, else Latin-1.
constexpr decoded_view decoded(std::string_view s) { return {s, encoding_of(s)}; }

// One code point written as UTF-8 into a sink (push_back).
template <class Out> constexpr void put_utf8(char32_t c, Out& out) {
  const auto put = [&](char32_t b) { out.push_back(static_cast<char>(b)); };
  if (c < 0x80) {
    put(c);
  } else if (c < 0x800) {
    put(0xC0 | c >> 6), put(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    put(0xE0 | c >> 12), put(0x80 | (c >> 6 & 0x3F)), put(0x80 | (c & 0x3F));
  } else {
    put(0xF0 | c >> 18), put(0x80 | (c >> 12 & 0x3F)), put(0x80 | (c >> 6 & 0x3F)), put(0x80 | (c & 0x3F));
  }
}
// A text as UTF-8, whatever it was read as, into a sink: a Latin-1 one
// made UTF-8, a UTF-8 one as it is.
template <class Out> constexpr void write_utf8(std::string_view s, Out& out) {
  for (char32_t c : decoded(s)) put_utf8(c, out);
}

}  // namespace cairn::text

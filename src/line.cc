// SPDX-License-Identifier: AGPL-3.0-only
// cairn.line -- An IRC line as the wire has it (RFC 1459, its modern form,
// and IRCv3 message tags): its tags, its source, its command and its
// parameters, read and written. Nothing here knows what a command means.
export module cairn.line;

import std;
import splice;

export namespace cairn {

// What reading or writing a line can come to besides a line: a type each,
// with what it knows, and what it says of itself (what).
namespace errors {
struct empty { static constexpr std::string_view what = "empty line"; };  // nothing but spaces, or no command
struct bad_tags { static constexpr std::string_view what = "tags marker with no tags"; };
struct bad_source { static constexpr std::string_view what = "source marker with no source"; };
struct bad_command {  // neither letters nor three digits
  std::string command;
  static constexpr std::string_view what = "malformed command";
  friend constexpr bool operator==(const bad_command&, const bad_command&) = default;
};
struct tags_too_long {  // past 8191 bytes, '@' and the space included
  std::size_t bytes = 0;
  static constexpr std::string_view what = "tags longer than 8191 bytes";
  friend constexpr bool operator==(const tags_too_long&, const tags_too_long&) = default;
};
struct body_too_long {  // past 512 bytes from the source on, CR LF included
  std::size_t bytes = 0;
  static constexpr std::string_view what = "line longer than 512 bytes";
  friend constexpr bool operator==(const body_too_long&, const body_too_long&) = default;
};
struct bad_parameter {  // a middle parameter empty, with a space, or ':' first
  std::size_t index = 0;
  static constexpr std::string_view what = "malformed middle parameter";
  friend constexpr bool operator==(const bad_parameter&, const bad_parameter&) = default;
};
struct bad_character { static constexpr std::string_view what = "CR, LF or NUL in a line"; };
constexpr bool operator==(empty, empty) { return true; }
constexpr bool operator==(bad_tags, bad_tags) { return true; }
constexpr bool operator==(bad_source, bad_source) { return true; }
constexpr bool operator==(bad_character, bad_character) { return true; }
}  // namespace errors
using line_error = spl::variant<errors::empty, errors::bad_tags, errors::bad_source, errors::bad_command,
                                errors::tags_too_long, errors::body_too_long, errors::bad_parameter,
                                errors::bad_character>;
constexpr std::string_view describe(const line_error& e) {
  return spl::visit([](const auto& one) { return std::remove_cvref_t<decltype(one)>::what; }, e);
}

inline constexpr std::size_t max_tags_bytes = 8191;
inline constexpr std::size_t max_body_bytes = 512;

// Reading or writing that came to an error, thrown: the error, as it was.
struct line_failure : std::runtime_error {
  line_error error;
  explicit line_failure(line_error e) : std::runtime_error(std::string(describe(e))), error(std::move(e)) {}
};

// A tag's value as the wire carries it, and back (IRCv3 message-tags), as
// views: read a character at a time as they are walked, nothing made.
namespace detail {
// One character, or the two of its escape.
struct escape_piece {
  std::array<char, 2> chars{};
  std::uint8_t size = 0;
  constexpr const char* begin() const { return chars.data(); }
  constexpr const char* end() const { return chars.data() + size; }
};
constexpr escape_piece escape_one(char c) {
  switch (c) {
    case ';': return {{'\\', ':'}, 2};
    case ' ': return {{'\\', 's'}, 2};
    case '\\': return {{'\\', '\\'}, 2};
    case '\r': return {{'\\', 'r'}, 2};
    case '\n': return {{'\\', 'n'}, 2};
    default: return {{c, 0}, 1};
  }
}
}  // namespace detail

constexpr auto escaped(std::string_view in) { return in | std::views::transform(detail::escape_one) | std::views::join; }

// Unescaped: \: \s \\ \r \n as what they stand for, any other escaped
// character as itself, a lone backslash at the end dropped.
class unescaped_view : public std::ranges::view_interface<unescaped_view> {
 public:
  class iterator {
   public:
    using value_type = char;
    using difference_type = std::ptrdiff_t;
    constexpr iterator() = default;
    constexpr iterator(const char* at, const char* end) : at_(at), end_(end) { settle(); }
    constexpr char operator*() const {
      if (*at_ != '\\') return *at_;
      switch (at_[1]) {
        case ':': return ';';
        case 's': return ' ';
        case 'r': return '\r';
        case 'n': return '\n';
        default: return at_[1];
      }
    }
    constexpr iterator& operator++() {
      at_ += *at_ == '\\' ? 2 : 1;
      settle();
      return *this;
    }
    constexpr iterator operator++(int) {
      iterator was = *this;
      ++*this;
      return was;
    }
    constexpr bool operator==(std::default_sentinel_t) const { return at_ == end_; }
    constexpr bool operator==(const iterator& other) const { return at_ == other.at_; }

   private:
    // A backslash with nothing after it: the end.
    constexpr void settle() {
      if (at_ != end_ && *at_ == '\\' && at_ + 1 == end_) at_ = end_;
    }
    const char* at_ = nullptr;
    const char* end_ = nullptr;
  };
  constexpr unescaped_view() = default;
  constexpr explicit unescaped_view(std::string_view in) : in_(in) {}
  constexpr iterator begin() const { return {in_.data(), in_.data() + in_.size()}; }
  constexpr std::default_sentinel_t end() const { return {}; }

 private:
  std::string_view in_;
};
constexpr unescaped_view unescaped(std::string_view in) { return unescaped_view(in); }

namespace detail {
constexpr char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
}  // namespace detail

// One message tag: its key -- '+' first for a client's own, a vendor's
// prefix as it came -- and its text. A line_view keeps the text as the wire
// has it, escaped, and unescapes it as it is read (value()); a line keeps it
// unescaped.
template <class S> struct basic_tag {
  S key;
  S text;
  constexpr bool client_only() const { return !key.empty() && key.front() == '+'; }
  // Its value, unescaped: a view either way.
  constexpr auto value() const {
    if constexpr (std::same_as<S, std::string_view>)
      return unescaped(text);
    else
      return std::string_view(text);
  }
  friend constexpr bool operator==(const basic_tag&, const basic_tag&) = default;
};

// Who a line is from: a server's name, or a nick with its user and host
// where they are given (nick!user@host).
template <class S> struct basic_source {
  S nick;  // or the server's name
  S user;
  S host;
  constexpr bool empty() const { return nick.empty() && user.empty() && host.empty(); }
  friend constexpr bool operator==(const basic_source&, const basic_source&) = default;
};

// A line: owning (S = std::string, `line`) or a view into what was read
// (S = std::string_view, `line_view`), valid while that is.
template <class S> struct basic_line {
  std::vector<basic_tag<S>> tags;
  std::optional<basic_source<S>> from;
  S command;  // as it came; is() compares without case
  std::vector<S> params;

  constexpr const basic_tag<S>* tag(std::string_view key) const {
    for (const auto& one : tags)
      if (one.key == key) return &one;
    return nullptr;
  }
  constexpr bool has_tag(std::string_view key) const { return tag(key) != nullptr; }
  // A tag's value, unescaped, as a string of its own -- or none.
  constexpr std::optional<std::string> tag_value(std::string_view key) const {
    const auto* one = tag(key);
    if (!one) return std::nullopt;
    return std::ranges::to<std::string>(one->value());
  }
  constexpr bool is(std::string_view name) const {
    return command.size() == name.size() &&
           std::ranges::equal(command, name, [](char a, char b) { return detail::upper(a) == detail::upper(b); });
  }
  constexpr bool numeric() const {
    return command.size() == 3 && std::ranges::all_of(command, [](char c) { return c >= '0' && c <= '9'; });
  }
  constexpr int number() const {
    return numeric() ? (command[0] - '0') * 100 + (command[1] - '0') * 10 + (command[2] - '0') : 0;
  }
  constexpr std::string_view param(std::size_t i) const {
    return i < params.size() ? std::string_view(params[i]) : std::string_view();
  }
  friend constexpr bool operator==(const basic_line&, const basic_line&) = default;
};

using tag = basic_tag<std::string>;
using source = basic_source<std::string>;
using line = basic_line<std::string>;
using tag_view = basic_tag<std::string_view>;
using source_view = basic_source<std::string_view>;
using line_view = basic_line<std::string_view>;

namespace detail {
constexpr bool is_space(char c) { return c == ' '; }
constexpr std::string_view skip_spaces(std::string_view in) {
  while (!in.empty() && is_space(in.front())) in.remove_prefix(1);
  return in;
}
constexpr std::string_view take_word(std::string_view& in) {
  const auto end = in.find(' ');
  const std::string_view word = in.substr(0, end);
  in = end == std::string_view::npos ? std::string_view() : in.substr(end);
  return word;
}
constexpr bool command_ok(std::string_view c) {
  if (c.empty()) return false;
  if (std::ranges::all_of(c, [](char x) { return x >= '0' && x <= '9'; })) return c.size() == 3;
  return std::ranges::all_of(c, [](char x) { return (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z'); });
}
}  // namespace detail

// nick!user@host, or a server's name: views into it.
constexpr source_view parse_source(std::string_view in) {
  source_view out;
  const auto at = in.find('@');
  const std::string_view before = in.substr(0, at);
  if (at != std::string_view::npos) out.host = in.substr(at + 1);
  const auto bang = before.find('!');
  out.nick = before.substr(0, bang);
  if (bang != std::string_view::npos) out.user = before.substr(bang + 1);
  return out;
}

// A line, as views into what is given -- without its CR LF (a trailing CR
// or LF is taken off). Tags and source optional; any number of spaces
// between parts; the last parameter after ':' keeps its spaces.
// A line of its own, from a view: values unescaped, command upper-cased.
constexpr line to_owned(const line_view& v) {
  line out;
  out.tags.reserve(v.tags.size());
  for (const tag_view& t : v.tags) out.tags.push_back({std::string(t.key), std::ranges::to<std::string>(t.value())});
  if (v.from) out.from = source{std::string(v.from->nick), std::string(v.from->user), std::string(v.from->host)};
  out.command = std::ranges::to<std::string>(v.command | std::views::transform(detail::upper));
  out.params.reserve(v.params.size());
  for (std::string_view p : v.params) out.params.emplace_back(p);
  return out;
}
constexpr const line& to_owned(const line& l) { return l; }

// A line as the wire takes it, CR LF ended, appended to `out` -- or why it
// cannot be, `out` then as it was: the limits held before anything is
// written, the last parameter after ':' where it has to be (empty, a space
// in it, or ':' first). A line_view's tag values go as they came, escaped;
// a line's are escaped as they are written.
template <class S>
constexpr std::expected<void, line_error> try_write(const basic_line<S>& l, std::string& out) {
  const std::size_t began = out.size();
  const auto fail = [&](line_error e) -> std::unexpected<line_error> {
    out.resize(began);
    return std::unexpected(std::move(e));
  };
  const auto clean = [](std::string_view s) {
    return std::ranges::none_of(s, [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
  };
  if (!l.tags.empty()) {
    out += '@';
    bool first = true;
    for (const auto& t : l.tags) {
      const std::string_view key = t.key;
      if (key.empty() || !clean(key) || key.find_first_of(" ;=") != std::string_view::npos) return fail(errors::bad_character{});
      if (!first) out += ';';
      first = false;
      out += key;
      if (!t.text.empty()) {
        out += '=';
        if constexpr (std::same_as<S, std::string_view>) {
          if (!clean(t.text) || t.text.find_first_of(" ;") != std::string_view::npos) return fail(errors::bad_character{});
          out += t.text;
        } else {
          out.append_range(escaped(t.text));
        }
      }
    }
    out += ' ';
    if (out.size() - began > max_tags_bytes) return fail(errors::tags_too_long{out.size() - began});
  }
  const std::size_t body_from = out.size();
  if (l.from && !l.from->empty()) {
    const std::size_t at = out.size() + 1;
    out += ':';
    out += l.from->nick;
    if (!l.from->user.empty()) (out += '!') += l.from->user;
    if (!l.from->host.empty()) (out += '@') += l.from->host;
    const std::string_view who = std::string_view(out).substr(at);
    if (!clean(who) || who.find(' ') != std::string_view::npos) return fail(errors::bad_character{});
    out += ' ';
  }
  if (!detail::command_ok(l.command)) return fail(errors::bad_command{std::string(l.command)});
  out += l.command;
  for (std::size_t i = 0; i < l.params.size(); ++i) {
    const std::string_view p = l.params[i];
    if (!clean(p)) return fail(errors::bad_character{});
    const bool last = i + 1 == l.params.size();
    const bool needs_colon = p.empty() || p.front() == ':' || p.find(' ') != std::string_view::npos;
    if (needs_colon && !last) return fail(errors::bad_parameter{i});
    out += ' ';
    if (needs_colon) out += ':';
    out += p;
  }
  out += "\r\n";
  if (out.size() - body_from > max_body_bytes) return fail(errors::body_too_long{out.size() - body_from});
  return {};
}
template <class S> constexpr void write(const basic_line<S>& l, std::string& out) {
  if (auto done = try_write(l, out); !done) throw line_failure(std::move(done.error()));
}
// Into a string of its own.
template <class S> constexpr std::expected<std::string, line_error> try_write(const basic_line<S>& l) {
  std::string out;
  if (auto done = try_write(l, out); !done) return std::unexpected(std::move(done.error()));
  return out;
}
template <class S> constexpr std::string write(const basic_line<S>& l) {
  std::string out;
  write(l, out);
  return out;
}

// A line made from its command and parameters, for writing.
template <class... P>
constexpr line make(std::string command, P&&... params) {
  line l;
  l.command = std::move(command);
  (l.params.emplace_back(std::forward<P>(params)), ...);
  return l;
}

}  // namespace cairn

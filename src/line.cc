// SPDX-License-Identifier: AGPL-3.0-only
// cairn.line -- An IRC line as the wire has it (RFC 1459, its modern form,
// and IRCv3 message tags): its tags, its source, its command and its
// parameters, read and written. Nothing here knows what a command means.
export module cairn.line;

import std;

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
using line_error = std::variant<errors::empty, errors::bad_tags, errors::bad_source, errors::bad_command,
                                errors::tags_too_long, errors::body_too_long, errors::bad_parameter,
                                errors::bad_character>;
constexpr std::string_view describe(const line_error& e) {
  return std::visit([](const auto& one) { return std::remove_cvref_t<decltype(one)>::what; }, e);
}

inline constexpr std::size_t max_tags_bytes = 8191;
inline constexpr std::size_t max_body_bytes = 512;

// One message tag: its key -- '+' first for a client's own, a vendor's
// prefix as it came -- and its value, unescaped (none and empty are one).
struct tag {
  std::string key;
  std::string value;
  constexpr bool client_only() const { return !key.empty() && key.front() == '+'; }
  friend constexpr bool operator==(const tag&, const tag&) = default;
};

// Who a line is from: a server's name, or a nick with its user and host
// where they are given (nick!user@host).
struct source {
  std::string nick;  // or the server's name
  std::string user;
  std::string host;
  constexpr bool empty() const { return nick.empty() && user.empty() && host.empty(); }
  friend constexpr bool operator==(const source&, const source&) = default;
};

struct line {
  std::vector<tag> tags;
  std::optional<cairn::source> from;
  std::string command;  // upper-cased as it came; a numeric as its three digits
  std::vector<std::string> params;

  constexpr const std::string* tag_value(std::string_view key) const {
    for (const tag& one : tags)
      if (one.key == key) return &one.value;
    return nullptr;
  }
  constexpr bool has_tag(std::string_view key) const { return tag_value(key) != nullptr; }
  constexpr bool numeric() const {
    return command.size() == 3 && std::ranges::all_of(command, [](char c) { return c >= '0' && c <= '9'; });
  }
  constexpr int number() const {
    return numeric() ? (command[0] - '0') * 100 + (command[1] - '0') * 10 + (command[2] - '0') : 0;
  }
  // The parameter at `i`, or nothing past the end.
  constexpr std::string_view param(std::size_t i) const {
    return i < params.size() ? std::string_view(params[i]) : std::string_view();
  }
  friend constexpr bool operator==(const line&, const line&) = default;
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

// nick!user@host, or a server's name.
constexpr source parse_source(std::string_view in) {
  source out;
  const auto at = in.find('@');
  const std::string_view before = in.substr(0, at);
  if (at != std::string_view::npos) out.host = std::string(in.substr(at + 1));
  const auto bang = before.find('!');
  out.nick = std::string(before.substr(0, bang));
  if (bang != std::string_view::npos) out.user = std::string(before.substr(bang + 1));
  return out;
}
constexpr void write_source(const source& s, std::string& out) {
  out += s.nick;
  if (!s.user.empty()) (out += '!') += s.user;
  if (!s.host.empty()) (out += '@') += s.host;
}

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
constexpr char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
}  // namespace detail

// A line, without its CR LF (a trailing CR or LF is taken off). Tags and
// source optional; any number of spaces between parts; the last parameter
// after ':' keeps its spaces.
constexpr std::expected<line, line_error> parse(std::string_view in) {
  while (!in.empty() && (in.back() == '\n' || in.back() == '\r')) in.remove_suffix(1);
  line out;
  in = detail::skip_spaces(in);
  if (in.empty()) return std::unexpected(errors::empty{});
  if (in.front() == '@') {
    in.remove_prefix(1);
    const std::string_view all = detail::take_word(in);
    if (all.empty()) return std::unexpected(errors::bad_tags{});
    std::size_t from = 0;
    while (from <= all.size()) {
      const auto end = std::min(all.find(';', from), all.size());
      const std::string_view one = all.substr(from, end - from);
      if (!one.empty()) {
        const auto eq = one.find('=');
        tag t{std::string(one.substr(0, eq)), eq == std::string_view::npos ? std::string() : std::ranges::to<std::string>(unescaped(one.substr(eq + 1)))};
        // A key given twice: the last one counts.
        if (auto same = std::ranges::find(out.tags, t.key, &tag::key); same != out.tags.end())
          *same = std::move(t);
        else
          out.tags.push_back(std::move(t));
      }
      from = end + 1;
    }
    in = detail::skip_spaces(in);
  }
  if (!in.empty() && in.front() == ':') {
    in.remove_prefix(1);
    const std::string_view who = detail::take_word(in);
    if (who.empty()) return std::unexpected(errors::bad_source{});
    out.from = parse_source(who);
    in = detail::skip_spaces(in);
  }
  const std::string_view command = detail::take_word(in);
  if (command.empty()) return std::unexpected(errors::empty{});
  if (!detail::command_ok(command)) return std::unexpected(errors::bad_command{std::string(command)});
  out.command.reserve(command.size());
  for (char c : command) out.command += detail::upper(c);
  for (;;) {
    in = detail::skip_spaces(in);
    if (in.empty()) break;
    if (in.front() == ':') {
      out.params.emplace_back(in.substr(1));
      break;
    }
    out.params.emplace_back(detail::take_word(in));
  }
  return out;
}

// A line as the wire takes it, CR LF ended -- or why it cannot be: the
// limits held before anything is written, the last parameter written after
// ':' where it has to be (empty, a space in it, or ':' first).
constexpr std::expected<void, line_error> write(const line& l, std::string& out) {
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
    for (const tag& t : l.tags) {
      if (t.key.empty() || !clean(t.key) || t.key.find_first_of(" ;=") != std::string::npos)
        return fail(errors::bad_character{});
      if (!first) out += ';';
      first = false;
      out += t.key;
      if (!t.value.empty()) {
        out += '=';
        out.append_range(escaped(t.value));
      }
    }
    out += ' ';
    if (out.size() - began > max_tags_bytes) return fail(errors::tags_too_long{out.size() - began});
  }
  const std::size_t body_from = out.size();
  if (l.from && !l.from->empty()) {
    const std::size_t at = out.size() + 1;
    out += ':';
    write_source(*l.from, out);
    const std::string_view who = std::string_view(out).substr(at);
    if (!clean(who) || who.find(' ') != std::string_view::npos) return fail(errors::bad_character{});
    out += ' ';
  }
  if (!detail::command_ok(l.command)) return fail(errors::bad_command{l.command});
  out += l.command;
  for (std::size_t i = 0; i < l.params.size(); ++i) {
    const std::string& p = l.params[i];
    if (!clean(p)) return fail(errors::bad_character{});
    const bool last = i + 1 == l.params.size();
    const bool needs_colon = p.empty() || p.front() == ':' || p.find(' ') != std::string::npos;
    if (needs_colon && !last) return fail(errors::bad_parameter{i});
    out += ' ';
    if (needs_colon) out += ':';
    out += p;
  }
  out += "\r\n";
  if (out.size() - body_from > max_body_bytes) return fail(errors::body_too_long{out.size() - body_from});
  return {};
}
// The same, into a string of its own.
constexpr std::expected<std::string, line_error> write(const line& l) {
  std::string out;
  if (auto done = write(l, out); !done) return std::unexpected(done.error());
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

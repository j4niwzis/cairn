// SPDX-License-Identifier: AGPL-3.0-only
// cairn.isupport -- What the server says of itself in RPL_ISUPPORT (005):
// read into fields where cairn needs them, every token kept besides.
export module cairn.isupport;

import std;
import cairn.line;
import cairn.casemap;

export namespace cairn {

// A channel member's status: its mode letter and the symbol shown before a
// nick (o, @), highest first as PREFIX gives them.
struct prefix_mode {
  char mode = 0;
  char symbol = 0;
  friend constexpr bool operator==(const prefix_mode&, const prefix_mode&) = default;
};

// CHANMODES: the letters by how they take an argument.
struct channel_modes {
  std::string lists = "beI";  // A: a list, an argument always
  std::string always = "k";   // B: an argument always
  std::string when_set = "l"; // C: an argument when set
  std::string never = "imnpst";  // D: never
};

struct isupport {
  std::vector<std::pair<std::string, std::string>> tokens;  // as given, values unescaped

  cairn::casemapping casemapping = cairn::casemapping::rfc1459;
  std::string chantypes = "#&";
  std::vector<prefix_mode> prefixes{{'o', '@'}, {'v', '+'}};
  cairn::channel_modes chanmodes;
  std::string network;
  std::size_t nicklen = 9;
  std::size_t linelen = 512;
  std::optional<std::size_t> monitor;    // MONITOR, its limit (0: none said)
  std::optional<std::size_t> chathistory;  // CHATHISTORY, most a request may ask
  bool utf8only = false;
  bool whox = false;
  std::optional<char> bot;

  [[nodiscard]] constexpr const std::string* token(std::string_view key) const {
    for (const auto& [k, v] : tokens)
      if (k == key) return &v;
    return nullptr;
  }
  [[nodiscard]] constexpr bool has(std::string_view key) const { return token(key) != nullptr; }
  [[nodiscard]] constexpr bool is_channel(std::string_view name) const {
    return !name.empty() && chantypes.find(name.front()) != std::string::npos;
  }
  // The status a symbol stands for, or none.
  [[nodiscard]] constexpr std::optional<prefix_mode> by_symbol(char symbol) const {
    for (const prefix_mode& p : prefixes)
      if (p.symbol == symbol) return p;
    return std::nullopt;
  }
  [[nodiscard]] constexpr std::optional<prefix_mode> by_mode(char mode) const {
    for (const prefix_mode& p : prefixes)
      if (p.mode == mode) return p;
    return std::nullopt;
  }
  // How a mode letter takes an argument when set (+) or unset (-).
  [[nodiscard]] constexpr bool takes_argument(char mode, bool set) const {
    if (by_mode(mode)) return true;
    if (chanmodes.lists.find(mode) != std::string::npos || chanmodes.always.find(mode) != std::string::npos) return true;
    if (chanmodes.when_set.find(mode) != std::string::npos) return set;
    return false;
  }

  // One 005 line taken in: its tokens between the nick and the trailing
  // text; -KEY takes a token back.
  constexpr void take(const line& l) {
    if (l.params.size() < 2) return;
    for (std::size_t i = 1; i + 1 < l.params.size(); ++i) take_token(l.params[i]);
  }

  constexpr void take_token(std::string_view one) {
    if (one.empty()) return;
    if (one.front() == '-') {
      const std::string key(one.substr(1));
      std::erase_if(tokens, [&](const auto& kv) { return kv.first == key; });
      reset(key);
      return;
    }
    const auto eq = one.find('=');
    std::string key(one.substr(0, eq));
    std::string value = eq == std::string_view::npos ? std::string() : unescape(one.substr(eq + 1));
    if (auto same = std::ranges::find(tokens, key, &std::pair<std::string, std::string>::first); same != tokens.end())
      same->second = value;
    else
      tokens.emplace_back(key, value);
    apply(key, value);
  }

  // \xHH, as ISUPPORT escapes a value.
  [[nodiscard]] static constexpr std::string unescape(std::string_view in) {
    std::string out;
    const auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '\\' && i + 3 < in.size() && in[i + 1] == 'x' && hex(in[i + 2]) >= 0 && hex(in[i + 3]) >= 0) {
        out += static_cast<char>(hex(in[i + 2]) * 16 + hex(in[i + 3]));
        i += 3;
      } else {
        out += in[i];
      }
    }
    return out;
  }

 private:
  static constexpr std::optional<std::size_t> number(std::string_view v) {
    std::size_t n = 0;
    if (v.empty()) return std::nullopt;
    for (char c : v) {
      if (c < '0' || c > '9') return std::nullopt;
      n = n * 10 + static_cast<std::size_t>(c - '0');
    }
    return n;
  }
  constexpr void apply(std::string_view key, std::string_view v) {
    if (key == "CASEMAPPING") {
      if (auto m = casemapping_named(v)) casemapping = *m;
    } else if (key == "CHANTYPES") {
      chantypes = std::string(v);
    } else if (key == "PREFIX") {
      // (ov)@+ -- or empty: no statuses.
      prefixes.clear();
      const auto close = v.find(')');
      if (!v.empty() && v.front() == '(' && close != std::string_view::npos) {
        const std::string_view modes = v.substr(1, close - 1), symbols = v.substr(close + 1);
        for (std::size_t i = 0; i < modes.size() && i < symbols.size(); ++i) prefixes.push_back({modes[i], symbols[i]});
      }
    } else if (key == "CHANMODES") {
      std::array<std::string, 4> groups;
      std::size_t g = 0;
      for (char c : v) {
        if (c == ',') {
          if (++g == groups.size()) break;
        } else {
          groups[g] += c;
        }
      }
      chanmodes = {groups[0], groups[1], groups[2], groups[3]};
    } else if (key == "NETWORK") {
      network = std::string(v);
    } else if (key == "NICKLEN") {
      if (auto n = number(v)) nicklen = *n;
    } else if (key == "LINELEN") {
      if (auto n = number(v)) linelen = *n;
    } else if (key == "MONITOR") {
      monitor = number(v).value_or(0);
    } else if (key == "CHATHISTORY" || key == "draft/CHATHISTORY") {
      chathistory = number(v).value_or(0);
    } else if (key == "UTF8ONLY") {
      utf8only = true;
    } else if (key == "WHOX") {
      whox = true;
    } else if (key == "BOT") {
      if (!v.empty()) bot = v.front();
    }
  }
  constexpr void reset(std::string_view key) {
    const isupport fresh;
    if (key == "CASEMAPPING") casemapping = fresh.casemapping;
    else if (key == "CHANTYPES") chantypes = fresh.chantypes;
    else if (key == "PREFIX") prefixes = fresh.prefixes;
    else if (key == "CHANMODES") chanmodes = fresh.chanmodes;
    else if (key == "NETWORK") network.clear();
    else if (key == "NICKLEN") nicklen = fresh.nicklen;
    else if (key == "LINELEN") linelen = fresh.linelen;
    else if (key == "MONITOR") monitor.reset();
    else if (key == "CHATHISTORY" || key == "draft/CHATHISTORY") chathistory.reset();
    else if (key == "UTF8ONLY") utf8only = false;
    else if (key == "WHOX") whox = false;
    else if (key == "BOT") bot.reset();
  }
};

}  // namespace cairn

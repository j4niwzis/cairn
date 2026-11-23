// SPDX-License-Identifier: AGPL-3.0-only
// cairn.regex -- A line read with std::regex: slow to run, quick to build,
// and nothing more than the standard library. The same lines as by hand.
export module cairn.regex;

import std;
import splice;
import cairn.line;

export namespace cairn::grammars {

struct std_regex {
  static std::expected<line_view, line_error> try_parse(std::string_view in) {
    while (!in.empty() && (in.back() == '\n' || in.back() == '\r')) in.remove_suffix(1);
    using it = std::string_view::const_iterator;
    const auto view = [](const std::sub_match<it>& m) { return std::string_view(m.first, m.second); };
    // Tags, source and command up front; the parameters after, one at a time.
    static const std::regex head(R"(^ *(?:@([^ ]*) *)?(?::([^ ]*) *)?([^ ]*) *)");
    static const std::regex command(R"(^(?:[A-Za-z]+|[0-9]{3})$)");
    static const std::regex param(R"(^(?::(.*)|([^ ]+)) *)");
    std::match_results<it> m;
    if (!std::regex_search(in.begin(), in.end(), m, head)) return std::unexpected(errors::empty{});
    const bool tagged = m[1].matched, sourced = m[2].matched;
    if (tagged && m[1].length() == 0) return std::unexpected(errors::bad_tags{});
    if (sourced && m[2].length() == 0) return std::unexpected(errors::bad_source{});
    if (m[3].length() == 0) return std::unexpected(errors::empty{});
    line_view out;
    if (tagged) {
      for (auto part : view(m[1]) | std::views::split(';')) {
        const std::string_view one(part);
        if (one.empty()) continue;
        const auto eq = one.find('=');
        tag_view t{one.substr(0, eq), eq == std::string_view::npos ? std::string_view() : one.substr(eq + 1)};
        if (auto same = std::ranges::find(out.tags, t.key, &tag_view::key); same != out.tags.end())
          *same = t;
        else
          out.tags.push_back(t);
      }
    }
    if (sourced) out.from = parse_source(view(m[2]));
    out.command = view(m[3]);
    if (!std::regex_match(out.command.begin(), out.command.end(), command))
      return std::unexpected(errors::bad_command{std::string(out.command)});
    for (auto at = m[0].second; at != in.end();) {
      std::match_results<it> p;
      if (!std::regex_search(at, in.end(), p, param, std::regex_constants::match_continuous)) break;
      out.params.push_back(p[1].matched ? view(p[1]) : view(p[2]));
      if (p[1].matched) break;
      at = p[0].second;
    }
    return out;
  }
};

}  // namespace cairn::grammars

// SPDX-License-Identifier: AGPL-3.0-only
// cairn.session -- The client over a transport of the caller's, as tern
// speaks over one: a type, nothing erased, what it can do found by
// concepts. It writes what the client says to the transport, reads lines
// off its input into room of the caller's, and hands what came of them to
// the caller's callback; and it asks the server something and waits for
// the answer to that question alone -- by labeled-response where the server
// has it, else by the reply that ends it (WHO 315, NAMES 366, WHOIS 318,
// LIST 323, MOTD 376 or 422), the answer's lines kept as the wire had them.
export module cairn.session;

import std;
import splice;
import cairn.line;
import cairn.read;
import cairn.messages;
import cairn.client;

export namespace cairn {

// What a transport has to do: an input range of characters it keeps, and
// writing (bytes the transport takes or copies) and flushing.
template <class T>
concept transport = requires(T& t, std::string_view bytes) {
  { t.input() } -> std::ranges::input_range;
  t.write(bytes);
  t.flush();
};

// The answer to a request: its lines, as the wire had them.
struct answer {
  std::vector<std::string> lines;
};

namespace detail {
// What answers a request, without labeled-response: the replies that are
// part of its answer, the ones that end it, and the errors that end it as
// well. Any other numeric is not its, and is handed on.
struct answered_by {
  std::span<const int> parts, ends;
  // Or a batch of one of these types: its lines the answer, its end the end
  // (CHATHISTORY's chathistory and draft/chathistory-targets).
  std::span<const std::string_view> batches = {};
};
inline constexpr std::array<std::string_view, 2> history_batches{"chathistory", "draft/chathistory-targets"};
inline constexpr std::array who_parts{352, 354};
inline constexpr std::array who_ends{315};
inline constexpr std::array names_parts{353};
inline constexpr std::array names_ends{366};
inline constexpr std::array whois_parts{301, 276, 307, 311, 312, 313, 317, 319, 320, 330, 335, 338, 378, 379, 671};
inline constexpr std::array whois_ends{318};
inline constexpr std::array list_parts{321, 322};
inline constexpr std::array list_ends{323};
inline constexpr std::array motd_parts{375, 372};
inline constexpr std::array motd_ends{376, 422};
// What ends any request: the target not there, too few parameters, no
// such command, not allowed, try again.
inline constexpr std::array errors{401, 402, 403, 406, 421, 461, 481, 263};
constexpr std::optional<answered_by> answers_to(std::string_view command) {
  if (command == "CHATHISTORY") return answered_by{{}, {}, history_batches};
  if (command == "WHO") return answered_by{who_parts, who_ends};
  if (command == "NAMES") return answered_by{names_parts, names_ends};
  if (command == "WHOIS") return answered_by{whois_parts, whois_ends};
  if (command == "LIST") return answered_by{list_parts, list_ends};
  if (command == "MOTD") return answered_by{motd_parts, motd_ends};
  return std::nullopt;
}
constexpr bool in(std::span<const int> set, int n) { return std::ranges::find(set, n) != set.end(); }
// The transport, as what a message is written into.
template <class T> struct transport_sink {
  T& t;
  constexpr void append(std::string_view s) { t.write(s); }
  constexpr void push_back(char c) { t.write(std::string_view(&c, 1)); }
};
}  // namespace detail

template <transport T> class session {
 public:
  constexpr session(T& wire, registration how, std::span<char> room) : wire_(wire), client_(std::move(how)), room_(room) {}

  // The registration said, and lines read until it is done (001) or the
  // input ends; what came of them told to `on`.
  template <class On> constexpr bool connect(On&& on) {
    detail::transport_sink<T> out{wire_};
    client_.start(out);
    wire_.flush();
    while (!client_.registered())
      if (!step(on)) return false;
    return true;
  }

  // One line read and handed on; false where the input has ended.
  template <class On> constexpr bool step(On&& on) {
    auto& in = wire_.input();
    auto at = std::ranges::begin(in);
    if (at == std::ranges::end(in)) return false;
    const auto got = try_read(at, std::ranges::end(in), room_);
    if (!got) return true;  // a line that is none: passed over
    detail::transport_sink<T> out{wire_};
    client_.feed(*got, out, on);
    wire_.flush();
    return true;
  }

  // A message sent and its answer waited for; what else comes meanwhile told
  // to `on`. None where the input ends first.
  template <class On> constexpr std::optional<answer> request(const message_t& what, std::string_view command, On&& on) {
    detail::transport_sink<T> out{wire_};
    const bool labeled = client_.has_cap("labeled-response");
    std::string label = "c";
    for (std::uint64_t n = ++labels_, high = 1; high <= n; high *= 10)
      label.insert(1, 1, static_cast<char>('0' + n / high % 10));
    if (labeled) out.append("@label=" + label + " ");
    write(what, out);
    wire_.flush();
    const auto by = detail::answers_to(command);
    answer got;
    // A line kept as the wire has it (the parser takes no line it could not
    // write back; one it could not is left out, never dereferenced).
    const auto keep = [&](const line_view& one) {
      if (auto wire = try_write(one)) got.lines.push_back(std::move(*wire));
    };
    std::string batch;  // the labeled-response batch it came in, where it did
    auto& in = wire_.input();
    for (;;) {
      auto at = std::ranges::begin(in);
      if (at == std::ranges::end(in)) return std::nullopt;
      const auto l = try_read(at, std::ranges::end(in), room_);
      if (!l) continue;
      const auto tagged = [&](std::string_view key, std::string_view want) {
        const auto v = l->tag_value(key);
        return v && std::ranges::equal(*v, want);
      };
      if (labeled) {
        if (l->is("BATCH") && tagged("label", label) && l->param(0).size() > 1 && l->param(0).front() == '+') {
          batch = std::string(l->param(0).substr(1));
          continue;
        }
        if (!batch.empty() && l->is("BATCH") && l->param(0) == "-" + batch) return got;
        if (!batch.empty() && tagged("batch", batch)) {
          keep(*l);
          continue;
        }
        if (tagged("label", label)) {
          if (!l->is("ACK")) keep(*l);
          return got;
        }
      } else if (by && !by->batches.empty()) {
        // Its batch opened, gathered, closed; a FAIL for it ends it too.
        if (batch.empty() && l->is("BATCH") && l->param(0).size() > 1 && l->param(0).front() == '+' &&
            std::ranges::contains(by->batches, l->param(1))) {
          batch = std::string(l->param(0).substr(1));
          continue;
        }
        if (!batch.empty() && l->is("BATCH") && l->param(0).size() > 1 && l->param(0).front() == '-' && l->param(0).substr(1) == batch)
          return got;
        if (!batch.empty() && tagged("batch", batch)) {
          keep(*l);
          continue;
        }
        if (l->is("FAIL") && l->param(0) == command) {
          keep(*l);
          return got;
        }
      } else if (by && l->numeric()) {
        const int n = l->number();
        const bool part = detail::in(by->parts, n), ends = detail::in(by->ends, n) || detail::in(detail::errors, n);
        if (part || ends) {
          keep(*l);
          if (ends) return got;
          continue;
        }
      }
      client_.feed(*l, out, on);
      wire_.flush();
    }
  }

  constexpr client& state() { return client_; }

 private:
  T& wire_;
  client client_;
  std::span<char> room_;
  std::uint64_t labels_ = 0;
};

}  // namespace cairn

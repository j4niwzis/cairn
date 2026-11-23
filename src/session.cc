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
// What a request is answered to the end of, without labeled-response.
constexpr std::optional<int> ends_with(std::string_view command) {
  if (command == "WHO") return 315;
  if (command == "NAMES") return 366;
  if (command == "WHOIS") return 318;
  if (command == "LIST") return 323;
  if (command == "MOTD") return 376;
  return std::nullopt;
}
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
    const auto end = detail::ends_with(command);
    answer got;
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
        if (l->is("BATCH") && tagged("label", label)) {
          batch = std::string(l->param(0).substr(1));
          continue;
        }
        if (!batch.empty() && l->is("BATCH") && l->param(0) == "-" + batch) return got;
        if (!batch.empty() && tagged("batch", batch)) {
          got.lines.push_back(*try_write(*l));
          continue;
        }
        if (tagged("label", label)) {
          if (!l->is("ACK")) got.lines.push_back(*try_write(*l));
          return got;
        }
      } else if (end) {
        const int n = l->number();
        if (n >= 200 && n < 600 && n != 1) {
          got.lines.push_back(*try_write(*l));
          if (n == *end || (*end == 376 && n == 422)) return got;
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

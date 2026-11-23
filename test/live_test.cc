// SPDX-License-Identifier: AGPL-3.0-only
// cairn against a real server (Ergo, in CI; CAIRN_LIVE_TESTS): registered
// with capabilities, a channel joined, NAMES and WHOIS asked and answered.
// The server is at CAIRN_LIVE_SERVER (host:port), 127.0.0.1:6667 by default.
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include "gtest/gtest-macros.h"
import std;
import splice;
import cairn;
import gtest;

namespace {
// A blocking TCP connection as cairn's transport: what it reads walked a
// byte at a time out of what each read() brought.
class tcp {
 public:
  explicit tcp(const char* host, const char* port) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host, port, &hints, &found) != 0) return;
    for (addrinfo* one = found; one; one = one->ai_next) {
      fd_ = socket(one->ai_family, one->ai_socktype, one->ai_protocol);
      if (fd_ >= 0 && ::connect(fd_, one->ai_addr, one->ai_addrlen) == 0) break;
      if (fd_ >= 0) close(fd_), fd_ = -1;
    }
    freeaddrinfo(found);
  }
  ~tcp() {
    if (fd_ >= 0) close(fd_);
  }
  bool open() const { return fd_ >= 0; }

  struct reader : std::ranges::view_interface<reader> {
    tcp* of;
    struct iterator {
      using value_type = char;
      using difference_type = std::ptrdiff_t;
      tcp* of = nullptr;
      char operator*() const { return of->buffer_[of->at_]; }
      iterator& operator++() { return ++of->at_, *this; }
      void operator++(int) { ++of->at_; }
      bool operator==(std::default_sentinel_t) const { return !of->fill(); }
    };
    iterator begin() const { return {of}; }
    std::default_sentinel_t end() const { return {}; }
  };
  reader& input() { return reader_; }
  void write(std::string_view s) { pending_ += s; }
  void flush() {
    std::string_view rest = pending_;
    while (!rest.empty()) {
      const auto n = ::send(fd_, rest.data(), rest.size(), 0);
      if (n <= 0) break;
      rest.remove_prefix(static_cast<std::size_t>(n));
    }
    pending_.clear();
  }

 private:
  // Something left to walk, read when there is not; false once closed.
  bool fill() {
    if (at_ < got_) return true;
    if (fd_ < 0) return false;
    const auto n = ::recv(fd_, buffer_.data(), buffer_.size(), 0);
    if (n <= 0) return false;
    at_ = 0, got_ = static_cast<std::size_t>(n);
    return true;
  }
  int fd_ = -1;
  std::array<char, 4096> buffer_{};  // what one recv() brings: the transport's, not cairn's
  std::size_t at_ = 0, got_ = 0;
  std::string pending_;
  reader reader_{{}, this};
};

std::pair<std::string, std::string> server() {
  const char* said = std::getenv("CAIRN_LIVE_SERVER");
  std::string_view s = said ? said : "127.0.0.1:6667";
  const auto colon = s.rfind(':');
  return {std::string(s.substr(0, colon)), std::string(s.substr(colon + 1))};
}
}  // namespace

TEST(Live, RegistersJoinsAndAsks) {
  const auto [host, port] = server();
  tcp wire(host.c_str(), port.c_str());
  ASSERT_TRUE(wire.open());
  std::array<char, 8192> room{};
  cairn::session s(wire, cairn::registration{.nick = "cairnbot", .caps = {"labeled-response", "batch", "server-time", "message-tags", "draft/chathistory", "chathistory"}}, room);
  bool registered = false;
  const auto on = [&](const cairn::event_t& e) {
    spl::visit(spl::overloaded{[&](const cairn::events::registered&) { registered = true; }, [](const auto&) {}}, e);
  };
  ASSERT_TRUE(s.connect(on));
  EXPECT_TRUE(registered);
  EXPECT_TRUE(s.state().has_cap("labeled-response"));

  s.send(cairn::msg::join{cairn::commas{"#cairn"}, {}, {}, {}});
  const auto names = s.request(cairn::msg::names{cairn::commas{"#cairn"}}, "NAMES", on);
  ASSERT_TRUE(names.has_value());
  const bool listed = std::ranges::any_of(names->lines, [](const std::string& l) {
    const auto got = cairn::try_parse(l);
    return got && got->is("353") && got->param(3).find("cairnbot") != std::string_view::npos;
  });
  EXPECT_TRUE(listed);

  const auto whois = s.request(cairn::msg::whois{cairn::params_rest{"cairnbot"}}, "WHOIS", on);
  ASSERT_TRUE(whois.has_value());
  const bool user = std::ranges::any_of(whois->lines, [](const std::string& l) {
    const auto got = cairn::try_parse(l);
    return got && got->is("311");
  });
  EXPECT_TRUE(user);

  // What was said in the channel, asked back with CHATHISTORY.
  s.send(cairn::msg::privmsg{"#cairn", "hello from cairn"});
  const auto history = s.request(cairn::msg::chathistory{cairn::msg::history::latest{"#cairn", cairn::msg::history::none{}, 10}},
                                 "CHATHISTORY", on);
  ASSERT_TRUE(history.has_value());
  const bool heard = std::ranges::any_of(history->lines, [](const std::string& l) {
    const auto got = cairn::try_parse(l);
    return got && got->is("PRIVMSG") && got->param(1) == "hello from cairn";
  });
  EXPECT_TRUE(heard);
}

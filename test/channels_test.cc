// SPDX-License-Identifier: AGPL-3.0-only
// cairn::channels followed through a conversation, checked while compiling.
import std;
import splice;
import cairn;
import gtest;

#include "gtest/gtest-macros.h"

namespace {
using namespace cairn;
constexpr message got(std::string_view s, const isupport& server = {}) { return read(*try_parse(s), server); }

static_assert([] {
  isupport server;
  server.take(to_owned(*try_parse(":srv 005 me CASEMAPPING=rfc1459 PREFIX=(qov)~@+ CHANMODES=b,k,l,imnst :are supported")));
  channels c("me");
  c.take(server);
  c.take(got(":me!u@h JOIN #Mux", server));
  c.take(got(":srv 353 me = #mux :~alice @bob carol me", server));
  c.take(got(":srv 366 me #mux :End of /NAMES list", server));
  c.take(got(":srv 332 me #mux :the topic", server));
  c.take(got(":srv 333 me #mux bob 1795460000", server));
  if (!c.find("#MUX") || c.find("#mux")->members.size() != 4 || c.find("#mux")->topic != "the topic") return false;
  if (c.member_of("#mux", "Alice")->prefixes != "~") return false;
  c.take(got(":dave!d@h JOIN #mux dave :Dave D", server));                  // extended-join
  if (c.member_of("#mux", "dave")->account != "dave") return false;
  c.take(got(":bob!b@h MODE #mux +v-o+k carol bob key", server));
  if (c.member_of("#mux", "carol")->prefixes != "+" || !c.member_of("#mux", "bob")->prefixes.empty()) return false;
  if (c.find("#mux")->modes.size() != 1 || c.find("#mux")->modes[0].argument != "key") return false;
  c.take(got(":carol!c@h NICK caroline", server));
  if (!c.member_of("#mux", "caroline") || c.member_of("#mux", "carol")) return false;
  c.take(got(":caroline!c@h AWAY :lunch", server));
  if (!c.member_of("#mux", "caroline")->away) return false;
  c.take(got(":bob!b@h KICK #mux dave :bye", server));
  c.take(got(":alice!a@h QUIT :gone", server));
  if (c.find("#mux")->members.size() != 3) return false;  // bob, caroline, me
  c.take(got(":bob!b@h TOPIC #mux :new topic", server));
  if (c.find("#mux")->topic != "new topic" || c.find("#mux")->topic_by != "bob") return false;
  c.take(got(":me!u@h PART #mux", server));
  return c.all().empty();
}());
}  // namespace

TEST(Channels, FollowWhenRun) {
  cairn::channels c("me");
  c.take(cairn::read(*cairn::try_parse(":me!u@h JOIN #a")));
  EXPECT_TRUE(c.find("#a") != nullptr);
}

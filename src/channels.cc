// SPDX-License-Identifier: AGPL-3.0-only
// cairn.channels -- The channels a client is in, followed from what the
// server says: who is in each and with what status (NAMES with
// multi-prefix, JOIN, PART, KICK, QUIT, NICK, MODE), their accounts and
// whether they are away (extended-join, account-notify, away-notify), the
// topic and who set it (TOPIC, 332, 333), the channel's modes. Names
// compared as the server says (CASEMAPPING).
export module cairn.channels;

import std;
import splice;
import cairn.line;
import cairn.casemap;
import cairn.isupport;
import cairn.messages;

export namespace cairn {

struct member {
  std::string key;  // the nick folded as the server says: what it is found by
  std::string nick;
  std::string prefixes;  // the statuses' symbols, highest first: "@+"
  std::string account;   // "" unknown, "*" none
  bool away = false;
  constexpr bool has(char symbol) const { return prefixes.find(symbol) != std::string::npos; }
};

struct channel {
  std::string key;  // the name folded
  std::string name;
  std::vector<member> members;  // in the order of their keys, once sorted
  bool members_sorted = true;
  std::string topic, topic_by;
  std::string topic_at;  // as the server said it: seconds since 1970
  // The channel's modes set, with their arguments: its own copies.
  struct mode_set {
    char mode = 0;
    std::string argument;
    friend constexpr bool operator==(const mode_set&, const mode_set&) = default;
  };
  std::vector<mode_set> modes;
  bool names_complete = false;          // 366 seen since the last JOIN
};

class channels {
 public:
  constexpr explicit channels(std::string me = {}) : me_(std::move(me)) {}

  constexpr void set_nick(std::string me) { me_ = std::move(me); }
  constexpr const std::string& nick() const { return me_; }

  // Found by the folded name, halving: channels kept in the order of their
  // keys, members sorted once a list of names has come in.
  constexpr const channel* find(std::string_view name) const {
    const std::string k = key_of(name);
    const auto at = std::ranges::lower_bound(all_, k, {}, &channel::key);
    return at != all_.end() && at->key == k ? &*at : nullptr;
  }
  constexpr const std::vector<channel>& all() const { return all_; }
  constexpr const member* member_of(std::string_view chan, std::string_view nick) {
    auto* c = find_mut(chan);
    return c ? member_mut(*c, nick) : nullptr;
  }

  // What the server said, taken in.
  constexpr void take(const message& m) {
    const auto who = m.line.from ? std::string_view(m.line.from->nick) : std::string_view();
    spl::visit(spl::overloaded{
                   [&](const msg::join& j) {
                     for (const std::string_view name : j.channels) {
                       if (mine(who)) {
                         if (!find(name)) {
                           channel fresh;
                           fresh.key = key_of(name), fresh.name = std::string(name);
                           all_.insert(std::ranges::upper_bound(all_, fresh.key, {}, &channel::key), std::move(fresh));
                         }
                       } else if (auto* c = find_mut(name)) {
                         add(*c, member{{}, std::string(who), {}, std::string(j.account), false});
                       }
                     }
                   },
                   [&](const msg::part& p) {
                     for (const std::string_view name : p.channels) leave(name, who);
                   },
                   [&](const msg::kick& k) { leave(k.channel, k.nick); },
                   [&](const msg::quit&) {
                     for (auto& c : all_) remove(c, who);
                   },
                   [&](const msg::nick& n) {
                     if (mine(who)) me_ = n.to;
                     for (auto& c : all_)
                       if (auto* mm = member_mut(c, who)) {
                         member renamed = *mm;  // a copy: the one there found by its key, then removed
                         remove(c, who);
                         renamed.nick = std::string(n.to);
                         add(c, std::move(renamed));
                       }
                   },
                   [&](const msg::topic& t) {
                     if (auto* c = find_mut(t.channel); c && t.body) {
                       c->topic = std::string(*t.body);
                       c->topic_by = std::string(who);
                     }
                   },
                   [&](const msg::mode& md) {
                     auto* c = find_mut(md.target);
                     if (!c) return;
                     for (const auto& change : md.changes_with(server_)) {
                       if (const auto status = server_.by_mode(change.mode)) {
                         if (auto* mm = member_mut(*c, change.argument)) set_status(*mm, status->symbol, change.set);
                       } else if (change.set) {
                         std::erase_if(c->modes, [&](const auto& had) { return had.mode == change.mode && !is_list(change.mode); });
                         c->modes.push_back({change.mode, std::string(change.argument)});
                       } else {
                         std::erase_if(c->modes, [&](const auto& had) {
                           return had.mode == change.mode && (!is_list(change.mode) || had.argument == change.argument);
                         });
                       }
                     }
                   },
                   [&](const msg::away& a) {
                     for (auto& c : all_)
                       if (auto* mm = member_mut(c, who)) mm->away = a.body.has_value();
                   },
                   [&](const msg::account& a) {
                     for (auto& c : all_)
                       if (auto* mm = member_mut(c, who)) mm->account = std::string(a.name);
                   },
                   [&](const msg::reply& r) { reply(r); },
                   [](const auto&) {}},
               m.what);
  }
  // What the server says of itself: statuses and casemapping.
  constexpr void take(const isupport& server) { server_ = server; }

 private:
  constexpr bool mine(std::string_view nick) const { return equal(nick, me_, server_.casemapping); }
  constexpr bool is_list(char mode) const { return server_.chanmodes.lists.find(mode) != std::string::npos; }
  constexpr std::string key_of(std::string_view name) const { return std::ranges::to<std::string>(folded(name, server_.casemapping)); }
  constexpr channel* find_mut(std::string_view name) { return const_cast<channel*>(find(name)); }
  // Members sorted by key where a list of names left them in its order:
  // once, then found by halving.
  static constexpr void sort(channel& c) {
    if (c.members_sorted) return;
    std::ranges::stable_sort(c.members, {}, &member::key);
    // The same nick twice (a NAMES list again): the last one kept.
    auto last = std::ranges::unique(c.members.rbegin(), c.members.rend(), {}, &member::key);
    c.members.erase(c.members.begin(), last.begin().base());
    c.members_sorted = true;
  }
  constexpr member* member_mut(channel& c, std::string_view nick) {
    sort(c);
    const std::string k = key_of(nick);
    const auto at = std::ranges::lower_bound(c.members, k, {}, &member::key);
    return at != c.members.end() && at->key == k ? &*at : nullptr;
  }
  constexpr void add(channel& c, member m) {
    m.key = key_of(m.nick);
    if (auto* had = member_mut(c, m.nick)) {
      *had = std::move(m);
      return;
    }
    c.members.insert(std::ranges::upper_bound(c.members, m.key, {}, &member::key), std::move(m));
  }
  // A list of names coming in: appended, sorted once it is all there.
  static constexpr void add_unsorted(channel& c, member m) {
    c.members.push_back(std::move(m));
    c.members_sorted = false;
  }
  constexpr void remove(channel& c, std::string_view nick) {
    sort(c);
    const std::string k = key_of(nick);
    const auto at = std::ranges::lower_bound(c.members, k, {}, &member::key);
    if (at != c.members.end() && at->key == k) c.members.erase(at);
  }
  constexpr void leave(std::string_view name, std::string_view nick) {
    if (mine(nick)) {
      const std::string k = key_of(name);
      const auto at = std::ranges::lower_bound(all_, k, {}, &channel::key);
      if (at != all_.end() && at->key == k) all_.erase(at);
    } else if (auto* c = find_mut(name)) {
      remove(*c, nick);
    }
  }
  // A status set or unset, the symbols kept in the server's order.
  constexpr void set_status(member& m, char symbol, bool on) {
    std::string now;
    for (const auto& p : server_.prefixes)
      if ((p.symbol == symbol) ? on : m.has(p.symbol)) now += p.symbol;
    m.prefixes = std::move(now);
  }
  constexpr void reply(const msg::reply& r) {
    const auto at = [&](std::size_t i) { return i < r.params.size() ? std::string_view(r.params[i]) : std::string_view(); };
    if (r.number == 353) {  // RPL_NAMREPLY: = #chan :@alice +bob carol (with multi-prefix, all statuses)
      auto* c = find_mut(at(1));
      if (!c) return;
      if (c->names_complete) {
        c->members.clear();
        c->names_complete = false;
      }
      for (auto word : at(2) | std::views::split(' ')) {
        std::string_view one(word);
        if (one.empty()) continue;
        member m;
        while (!one.empty() && server_.by_symbol(one.front())) m.prefixes += one.front(), one.remove_prefix(1);
        m.nick = std::string(one.substr(0, one.find('!')));  // userhost-in-names
        m.key = key_of(m.nick);
        add_unsorted(*c, std::move(m));
      }
    } else if (r.number == 366) {  // RPL_ENDOFNAMES
      if (auto* c = find_mut(at(0))) c->names_complete = true, sort(*c);
    } else if (r.number == 332) {  // RPL_TOPIC
      if (auto* c = find_mut(at(0))) c->topic = std::string(at(1));
    } else if (r.number == 333) {  // RPL_TOPICWHOTIME
      if (auto* c = find_mut(at(0))) c->topic_by = std::string(at(1)), c->topic_at = std::string(at(2));
    }
  }

  std::string me_;
  isupport server_;
  std::vector<channel> all_;
};

}  // namespace cairn

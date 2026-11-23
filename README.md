# cairn

An IRC client library for C++26, as modules. As tern does for XMPP, it
speaks over a transport of the caller's -- a type, nothing erased, what it can
do found by concepts -- and runs nothing of its own: the control flow is the
caller's, a blocking socket in a thread or coroutines suspending in the
transport.

Not written yet. What it is to be:

- **Lines** (`cairn.line`): IRCv3 message tags with their escaping, source,
  command or numeric, parameters -- read as views into what was read, without
  copying; the 512-byte body and 8191-byte tag limits held on writes, before
  anything is written. Text that is not UTF-8 replaced, read as Latin-1, or
  refused, as asked.
- **Typed messages** (`cairn.messages`): a type for each command -- PRIVMSG,
  NOTICE, JOIN, PART, QUIT, KICK, MODE (its changes parsed by CHANMODES and
  PREFIX), TOPIC, NICK, INVITE, AWAY, ACCOUNT, CHGHOST, SETNAME, TAGMSG, BATCH,
  PING, ERROR -- the numerics it knows with named fields, CTCP and ACTION
  unwrapped; the tags typed: server-time, msgid, label, batch, account,
  +draft/reply, +draft/react, +typing. A variant of them, told apart with
  `spl::visit`; a protocol extends the set.
- **Negotiation**: CAP LS 302, CAP REQ, SASL (SCRAM-SHA-256, EXTERNAL, PLAIN
  only over TLS unless asked), CAP END, NICK/USER (PASS for bouncers), nicks
  tried in turn, 001 and ISUPPORT; cap-notify followed after.
- **ISUPPORT and casemapping** (`cairn.isupport`, `cairn.casemap`): read into
  fields; channels and nicks compared as the server says (rfc1459, ascii, ...).
- **Requests**: by labeled-response where the server has it, else by the
  replies each request ends with (WHO -> 315, NAMES -> 366, CHATHISTORY -> its
  batch); batches gathered, nested. Several in flight from coroutines through a
  scheduler, `try_` forms returning `std::expected` -- as tern.
- **Receiving**: `lines()`, `receive()`, inboxes by casefolded target over one
  log; PING answered; CTCP answered by handlers given at connect.
- **Writing**: long text split at words and UTF-8, the server's prefix counted
  (or draft/multiline); flood held by a token bucket over the caller's clock;
  every buffer bounded.
- **Channels** (`cairn.channels`, optional): members, prefixes, accounts,
  away, topic, modes, followed purely from the lines.
- **Formatting** (`cairn.format`): mIRC codes to spans and back.
- **Constant evaluation**: the session constexpr along its path, tested as
  tern's are, a whole session over a script at compile time.

## Licence

GNU Affero General Public License, version 3 only (`AGPL-3.0-only`).

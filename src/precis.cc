// SPDX-License-Identifier: AGPL-3.0-only
// cairn.precis -- CASEMAPPING=rfc7613 as RFC 7613 (now 8265) means it: a
// nick prepared by the UsernameCaseMapped profile -- width mapped, lower
// cased, NFC, the Bidi Rule, the IdentifierClass -- by alef. Built where
// CAIRN_UNICODE is on; cairn.casemap without it folds rfc7613 as ascii.
// It allocates: the profile makes a new string (the owning API).
export module cairn.precis;

import std;
import alef.precis;

export namespace cairn::precis {

// The key a nick is compared by under rfc7613, as UTF-8, or nothing where
// the nick is not a username the profile allows.
inline std::optional<std::string> key(std::string_view nick) {
  const auto prepared = alef::prepare_username(nick);
  if (!prepared) return std::nullopt;
  std::string out;
  for (char32_t c : *prepared) {
    const auto put = [&](char32_t b) { out.push_back(static_cast<char>(b)); };
    if (c < 0x80) put(c);
    else if (c < 0x800) put(0xC0 | c >> 6), put(0x80 | (c & 0x3F));
    else if (c < 0x10000) put(0xE0 | c >> 12), put(0x80 | (c >> 6 & 0x3F)), put(0x80 | (c & 0x3F));
    else put(0xF0 | c >> 18), put(0x80 | (c >> 12 & 0x3F)), put(0x80 | (c >> 6 & 0x3F)), put(0x80 | (c & 0x3F));
  }
  return out;
}

// Whether two nicks are the same one under rfc7613.
inline bool equal(std::string_view a, std::string_view b) {
  const auto ka = key(a), kb = key(b);
  return ka && kb && *ka == *kb;
}

}  // namespace cairn::precis

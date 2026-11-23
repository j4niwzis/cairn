// SPDX-License-Identifier: AGPL-3.0-only
// libFuzzer over the parser: any bytes read as a line, by hand and by the
// described grammar alike; a line read written back and read again the
// same; every message read from it written and read again without a crash.
import std;
import cairn;

namespace {
void check(bool ok, int which = __builtin_LINE()) {
  if (!ok) {
    std::println("check at line {} failed", which);
    __builtin_trap();
  }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view in(reinterpret_cast<const char*>(data), size);
  const auto hand = cairn::try_parse<cairn::grammars::by_hand>(in);
  const auto described = cairn::try_parse<cairn::grammars::described>(in);
  check(hand.has_value() == described.has_value());
  if (!hand) {
    check(hand.error().index() == described.error().index());
    return 0;
  }
  check(hand->command == described->command && hand->params_text == described->params_text && hand->tags_text == described->tags_text);
  // Written back and read again: the same line.
  const auto wire = cairn::try_write(*hand);
  check(wire.has_value());
  const auto again = cairn::try_parse(*wire);
  check(again.has_value() && again->command == hand->command &&
        std::ranges::equal(again->params(), hand->params()));
  // As a message, written and read again.
  const cairn::message m = cairn::read(*hand);
  std::string out;
  cairn::write(m.what, out);
  (void)cairn::try_parse(out);
  // Text decoded and formatting walked.
  for (std::string_view p : hand->params()) {
    for (char32_t c : cairn::text::decoded(p)) (void)c;
    for (const auto& span : cairn::format::spans(p)) (void)span;
  }
  return 0;
}

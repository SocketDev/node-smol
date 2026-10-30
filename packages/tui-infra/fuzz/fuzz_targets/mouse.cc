// libFuzzer harness for tui::MouseParser — the terminal's mouse-sequence
// decoder.
//
// Why this boundary: the bytes reaching ParseAll come straight off the
// terminal's stdin, so a hostile or merely broken emulator controls every
// one of them. The decoder walks them with raw pointer arithmetic
// (ParseSgrSequence scans decimal digits until a terminator; the X10 path
// reads three fixed bytes), which is exactly where an off-by-one reads past
// the buffer.
//
// Contract: on ANY byte string the parser must not crash, read out of
// bounds, or hang. Refusing to decode is the expected answer for most
// inputs and is a NON-finding.

#include <cstddef>
#include <cstdint>

#include "tui/mouse.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // One parser across both calls so the button-pressed set carries drag
  // state between sequences — the stateful path a single-shot harness never
  // reaches.
  tui::MouseParser parser;

  // The fast-path predicate a caller runs before committing to a parse. It
  // must agree with the parser on short and truncated input rather than
  // reading past `size`.
  tui::LooksLikeMouseSequence(data, size);

  size_t consumed = 0;
  parser.ParseOne(data, size, &consumed);

  parser.ParseAll(data, size, [](const tui::RawMouseEvent& event) {
    // Touch every field so a sanitizer sees the reads. The scroll slot is
    // only populated for kScroll events; reading it otherwise is the bug
    // this dereference would catch.
    volatile int32_t sink = event.x + event.y + event.button;
    if (event.type == tui::MouseEventType::kScroll && event.scroll) {
      sink += event.scroll->delta;
    }
    (void)sink;
  });

  parser.Reset();
  return 0;
}

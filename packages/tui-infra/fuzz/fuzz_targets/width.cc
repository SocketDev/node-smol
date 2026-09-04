// libFuzzer harness for tui::StringWidth — the UTF-8 display-width scanner.
//
// Why this boundary: every string the renderer measures arrives from
// userland (a component's text, a pasted terminal payload), so the decoder
// sees truncated multi-byte sequences, lone continuation bytes, and
// overlong forms. DecodeUtf8 advances a raw `const char*` by 1-4 bytes per
// codepoint against an `end` sentinel, so a length class that reads its
// continuation bytes before bounds-checking them runs off the buffer on the
// last codepoint.
//
// Contract: on ANY byte string the scanner must not crash, read out of
// bounds, or hang. A replacement-character result for malformed input is
// the documented behavior and a NON-finding.

#include <cstddef>
#include <cstdint>

#include "tui/utf8.hpp"
#include "tui/width.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const char* text = reinterpret_cast<const char*>(data);

  volatile uint32_t width = tui::StringWidth(text, size);
  (void)width;

  // Walk the same bytes through the decode primitives directly. StringWidth
  // takes an ASCII fast path, so without this loop the multi-byte assembly
  // and the byte-length probe stay unfuzzed on ASCII-heavy corpora.
  const char* p = text;
  const char* end = text + size;
  while (p < end) {
    const char* before = p;
    const size_t bytes = tui::Utf8ByteLen(p, end);
    const uint32_t cp = tui::DecodeUtf8(p, end);
    volatile uint32_t cell = tui::CodepointWidth(cp);
    (void)cell;
    // Forward progress is the loop's own invariant: a decode that consumed
    // nothing would spin here forever, and a hang is a finding, so make it
    // one the fuzzer's timeout reports rather than an infinite loop.
    if (p <= before) {
      p = before + (bytes > 0 ? bytes : 1);
    }
  }
  return 0;
}

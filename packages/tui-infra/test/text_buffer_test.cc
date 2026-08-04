// Test for tui::TextBuffer + the display-length / line-count kernels +
// tui::handles::Tag.
//
// Foundation slice (S1). There is no test runner in this package (see the
// tranche port plan); this harness is compiled and run by hand, exactly as
// renderer_test.cc is:
//
//   /usr/bin/clang++ -std=c++20 -Iinclude -o /tmp/tb \
//     test/text_buffer_test.cc \
//     src/socketsecurity/tui/{text_buffer,width,width_data}.cc && /tmp/tb
//
// The DisplayLength / CountLines cases mirror stuie's own text_buffer.rs
// unit tests (display_length_excludes_newlines_and_counts_wide,
// malformed_utf8_bytes_preserved) so the port is checked against the same
// fixtures the Rust source-of-truth asserts. The handle cases mirror
// handles.rs's tag_is_nonzero_and_kind_disjoint /
// renderer_kind_leaves_small_indices_unchanged.

#include "tui/handles.hpp"
#include "tui/text_buffer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace {

int failures = 0;

void Expect(bool ok, const char* label) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", label);
    ++failures;
  }
}

void ExpectEq(uint32_t got, uint32_t want, const char* label) {
  if (got != want) {
    std::fprintf(stderr, "FAIL %s (got %u, want %u)\n", label, got, want);
    ++failures;
  }
}

// DisplayLength / CountLines over a byte-array literal.
template <size_t N>
uint32_t Width(const uint8_t (&bytes)[N]) {
  return tui::DisplayLength(bytes, N);
}
template <size_t N>
uint32_t Lines(const uint8_t (&bytes)[N]) {
  return tui::CountLines(bytes, N);
}

}  // namespace

int main() {
  using namespace tui;

  // ── DisplayLength: newlines excluded, wide scalars counted twice ──
  // Mirrors text_buffer.rs display_length_excludes_newlines_and_counts_wide.
  {
    const uint8_t hello[] = {'H', 'e', 'l', 'l', 'o', ' ',
                             'W', 'o', 'r', 'l', 'd'};
    ExpectEq(Width(hello), 11, "display_length(\"Hello World\") == 11");

    const uint8_t lines[] = {'L', 'i', 'n', 'e', ' ', '1', '\n', 'L', 'i',
                             'n', 'e', ' ', '2', '\n', 'L', 'i', 'n', 'e',
                             ' ', '3'};
    // 18 display columns; the two newlines contribute 0.
    ExpectEq(Width(lines), 18, "display_length(3 lines) excludes newlines");

    // "Hello " (6) + 世(2) + 界(2) + " "(1) + 🌟(2) = 13.
    const uint8_t wide[] = {'H',  'e',  'l',  'l',  'o',  ' ',
                            0xe4, 0xb8, 0x96,              // 世 U+4E16
                            0xe7, 0x95, 0x8c,              // 界 U+754C
                            ' ',
                            0xf0, 0x9f, 0x8c, 0x9f};       // 🌟 U+1F31F
    ExpectEq(Width(wide), 13, "display_length(\"Hello 世界 🌟\") == 13");
  }

  // ── DisplayLength: malformed UTF-8 bytes contribute 0 ──
  // Mirrors text_buffer.rs malformed_utf8_bytes_preserved: the out-of-range
  // 4-byte sequence F4 90 80 80 (would be U+110000) is rejected, so only
  // 'A' and 'B' count.
  {
    const uint8_t raw[] = {0x41, 0xf4, 0x90, 0x80, 0x80, 0x42};
    ExpectEq(Width(raw), 2, "display_length(malformed) counts only scalars");
  }

  // ── DisplayLength: surrogate + overlong are rejected too ──
  {
    const uint8_t surrogate[] = {'A', 0xed, 0xa0, 0x80, 'B'};  // U+D800 form
    ExpectEq(Width(surrogate), 2, "display_length rejects UTF-16 surrogate");
    const uint8_t overlong[] = {'A', 0xc0, 0x80, 'B'};  // overlong NUL
    ExpectEq(Width(overlong), 2, "display_length rejects overlong encoding");
    const uint8_t truncated[] = {'A', 0xe4, 0xb8};  // 世 missing last byte
    ExpectEq(Width(truncated), 1, "display_length rejects truncated tail");
  }

  // ── CountLines: (newline count) + 1 ──
  {
    const uint8_t one[] = {'a', 'b', 'c'};
    ExpectEq(Lines(one), 1, "line_count(no newline) == 1");
    const uint8_t two[] = {'a', 'b', '\n', 'c', 'd'};
    ExpectEq(Lines(two), 2, "line_count(one newline) == 2");
    const uint8_t three[] = {'a', '\n', 'b', '\n', 'c'};
    ExpectEq(Lines(three), 3, "line_count(two newlines) == 3");
  }

  // ── TextBuffer foundation observable state ──
  // A foundation buffer has no content-ingestion path yet (S3), so it stays
  // empty: length 0, byte size 0, one logical line. Clear/Reset are no-op
  // safe on an empty buffer.
  {
    TextBuffer buf;
    ExpectEq(buf.Length(), 0, "empty buffer Length() == 0");
    ExpectEq(buf.ByteSize(), 0, "empty buffer ByteSize() == 0");
    ExpectEq(buf.LineCount(), 1, "empty buffer LineCount() == 1");
    buf.Clear();
    buf.Reset();
    ExpectEq(buf.Length(), 0, "after Clear/Reset Length() == 0");
    ExpectEq(buf.ByteSize(), 0, "after Clear/Reset ByteSize() == 0");
    ExpectEq(buf.LineCount(), 1, "after Clear/Reset LineCount() == 1");
  }

  // ── S2: tab width + presentation defaults (Option semantics) ──
  // Fixtures are stuie's own constants: DEFAULT_TAB_WIDTH == 2
  // (text_buffer.rs:23), default_fg/bg/attrs start None (text_buffer.rs:112-114)
  // and reset_defaults() returns all three to None (text_buffer.rs:437-443).
  {
    TextBuffer buf;
    // A fresh buffer starts at the default tab width and mirrors kDefaultTabWidth.
    ExpectEq(buf.TabWidth(), 2, "fresh TabWidth() == DEFAULT_TAB_WIDTH (2)");
    ExpectEq(buf.TabWidth(), kDefaultTabWidth, "TabWidth() == kDefaultTabWidth");
    buf.SetTabWidth(8);
    ExpectEq(buf.TabWidth(), 8, "SetTabWidth(8) round-trips");
    buf.SetTabWidth(0);
    ExpectEq(buf.TabWidth(), 0, "SetTabWidth(0) round-trips");

    // Defaults start unset (None).
    Expect(!buf.DefaultFg().has_value(), "fresh DefaultFg() is None");
    Expect(!buf.DefaultBg().has_value(), "fresh DefaultBg() is None");
    Expect(!buf.DefaultAttributes().has_value(), "fresh DefaultAttributes None");

    // Some(rgba) sets, and the lanes round-trip verbatim.
    buf.SetDefaultFg(std::array<uint16_t, 4>{255, 255, 255, 255});
    buf.SetDefaultBg(std::array<uint16_t, 4>{10, 20, 30, 255});
    buf.SetDefaultAttributes(uint32_t{7});
    Expect(buf.DefaultFg().has_value() &&
               *buf.DefaultFg() ==
                   (std::array<uint16_t, 4>{255, 255, 255, 255}),
           "SetDefaultFg round-trips lanes");
    Expect(buf.DefaultBg().has_value() &&
               *buf.DefaultBg() == (std::array<uint16_t, 4>{10, 20, 30, 255}),
           "SetDefaultBg round-trips lanes");
    Expect(buf.DefaultAttributes().has_value() &&
               *buf.DefaultAttributes() == 7u,
           "SetDefaultAttributes round-trips");

    // None clears an individual default (null -> None).
    buf.SetDefaultFg(std::nullopt);
    Expect(!buf.DefaultFg().has_value(), "SetDefaultFg(None) clears");
    Expect(buf.DefaultBg().has_value(), "clearing fg leaves bg set");

    // reset_defaults() returns ALL three to None in one shot.
    buf.SetDefaultFg(std::array<uint16_t, 4>{1, 2, 3, 4});
    buf.ResetDefaults();
    Expect(!buf.DefaultFg().has_value(), "ResetDefaults clears fg");
    Expect(!buf.DefaultBg().has_value(), "ResetDefaults clears bg");
    Expect(!buf.DefaultAttributes().has_value(), "ResetDefaults clears attrs");
    // reset_defaults() does NOT touch tab width.
    ExpectEq(buf.TabWidth(), 0, "ResetDefaults leaves tab width untouched");
  }

  // ── handles::Tag: kind-disjoint, nonzero, renderer-transparent ──
  // Mirrors handles.rs tag_is_nonzero_and_kind_disjoint /
  // renderer_kind_leaves_small_indices_unchanged.
  {
    // KIND_RENDERER == 0, so its handles stay the raw small integers.
    ExpectEq(handles::Tag(handles::kKindRenderer, 1), 1, "Tag(renderer,1)==1");
    ExpectEq(handles::Tag(handles::kKindRenderer, 42), 42,
             "Tag(renderer,42)==42");
    // A text-buffer handle carries KIND_TEXT_BUFFER in the top nibble.
    ExpectEq(handles::Tag(handles::kKindTextBuffer, 1),
             (uint32_t{2} << 28) | 1u, "Tag(text_buffer,1) packs kind 2");
    // Same index, different kinds -> different handles (no cross-registry
    // aliasing).
    Expect(handles::Tag(handles::kKindTextBuffer, 1) !=
               handles::Tag(handles::kKindTextBufferView, 1),
           "text_buffer and view handles never alias at the same index");
    Expect(handles::Tag(handles::kKindTextBuffer, 1) != 0,
           "a 1-based text_buffer index never produces the 0 sentinel");
  }

  if (failures == 0) {
    std::printf("TEXT_BUFFER_OK\n");
    return 0;
  }
  std::fprintf(stderr, "%d failure(s)\n", failures);
  return 1;
}

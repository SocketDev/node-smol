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
#include <vector>

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

// Compare `[out, out + n)` against a NUL-terminated ASCII/UTF-8 literal.
void ExpectBytes(const uint8_t* out,
                 uint32_t n,
                 const char* want,
                 const char* label) {
  size_t want_len = 0;
  while (want[want_len] != '\0') {
    ++want_len;
  }
  bool ok = n == want_len;
  for (uint32_t i = 0; ok && i < n; ++i) {
    ok = out[i] == static_cast<uint8_t>(want[i]);
  }
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (got %u bytes \"%.*s\", want \"%s\")\n", label,
                 n, static_cast<int>(n), reinterpret_cast<const char*>(out),
                 want);
    ++failures;
  }
}

// Register `text` as a mem slot and materialize it as content — the C++ mirror
// of stuie's set_content_for_test helper (text_buffer.rs:1109-1113), which is
// the setText path (register_mem + set_from_mem) the range/append fixtures use.
void SetContentViaMem(tui::TextBuffer* buf, const char* text) {
  size_t len = 0;
  while (text[len] != '\0') {
    ++len;
  }
  uint16_t id =
      buf->RegisterMem(reinterpret_cast<const uint8_t*>(text), len);
  Expect(id != tui::kMemRegisterFailed, "SetContentViaMem: register succeeds");
  buf->SetFromMem(id);
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

  // ── S3: CRLF normalization ──
  // stuie's own fixture crlf_normalized (text_buffer.rs:1046-1049):
  // normalize_crlf(b"Line1\r\nLine2\r\n") == b"Line1\nLine2\n".
  {
    const uint8_t crlf[] = {'L', 'i', 'n', 'e', '1', '\r', '\n',
                            'L', 'i', 'n', 'e', '2', '\r', '\n'};
    std::vector<uint8_t> got = NormalizeCrlf(crlf, sizeof(crlf));
    const char* want = "Line1\nLine2\n";
    ExpectBytes(got.data(), static_cast<uint32_t>(got.size()), want,
                "normalize_crlf collapses CRLF to LF");
  }

  // ── S3: mem registry slot recycling across clear ──
  // stuie's own fixture mem_registry_recovers_after_clear
  // (text_buffer.rs:1077-1091), ported onto the class methods the binding
  // registry drives. clear_mem_registry frees every slot, so the old id no
  // longer resolves and register reuses slot 0.
  {
    TextBuffer buf;
    uint16_t id = buf.RegisterMem(reinterpret_cast<const uint8_t*>("Hello World"),
                                  11);
    Expect(id != kMemRegisterFailed, "register_mem id != MEM_REGISTER_FAILED");
    buf.SetFromMem(id);
    ExpectEq(buf.ByteSize(), 11, "set_from_mem gives 11 bytes");
    buf.ClearMemRegistry();
    Expect(!buf.ReplaceMem(id, reinterpret_cast<const uint8_t*>("Recovered"), 9),
           "replace_mem on a cleared slot returns false");
    uint16_t id2 =
        buf.RegisterMem(reinterpret_cast<const uint8_t*>("Recovered"), 9);
    buf.SetFromMem(id2);
    ExpectEq(buf.ByteSize(), 9, "re-registered content is 9 bytes");
    ExpectEq(buf.Length(), 9, "re-registered content display length is 9");
  }

  // ── S3: register_mem recycles the FIRST freed slot ──
  // Direct exercise of the position(Option::is_none) recycle path
  // (text_buffer.rs:160-163). A fresh buffer registers slots 0 and 1
  // monotonically; after ClearMemRegistry frees the whole vector, the next
  // register lands back at index 0.
  {
    TextBuffer buf;
    ExpectEq(buf.RegisterMem(reinterpret_cast<const uint8_t*>("a"), 1), 0,
             "first register -> slot 0");
    ExpectEq(buf.RegisterMem(reinterpret_cast<const uint8_t*>("b"), 1), 1,
             "second register -> slot 1");
    buf.ClearMemRegistry();
    ExpectEq(buf.RegisterMem(reinterpret_cast<const uint8_t*>("c"), 1), 0,
             "register after clear reuses slot 0");
    // replace_mem on a valid occupied slot succeeds and round-trips content.
    Expect(buf.ReplaceMem(0, reinterpret_cast<const uint8_t*>("zz"), 2),
           "replace_mem on an occupied slot returns true");
    buf.SetFromMem(0);
    ExpectEq(buf.ByteSize(), 2, "replaced slot materializes 2 bytes");
    // A missing slot id never resolves.
    Expect(!buf.ReplaceMem(99,
                           reinterpret_cast<const uint8_t*>("x"), 1),
           "replace_mem on an out-of-range slot returns false");
  }

  // ── S3: append + append_from_mem (CRLF-normalized) ──
  // Derived from append/append_from_mem (text_buffer.rs:189-199), which
  // normalize_crlf the incoming bytes then push onto content. Expected values
  // follow directly from that source: "AB" + normalize("\r\nCD") -> "AB\nCD".
  {
    TextBuffer buf;
    SetContentViaMem(&buf, "AB");
    const uint8_t tail[] = {'\r', '\n', 'C', 'D'};
    buf.Append(tail, sizeof(tail));
    ExpectEq(buf.ByteSize(), 5, "append normalizes CRLF then extends content");
    uint8_t out[32];
    uint32_t n = buf.GetPlainText(out, sizeof(out));
    ExpectBytes(out, n, "AB\nCD", "get_plain_text after append");
    // append_from_mem concatenates a registered slot's bytes.
    uint16_t ef =
        buf.RegisterMem(reinterpret_cast<const uint8_t*>("EF"), 2);
    buf.AppendFromMem(ef);
    n = buf.GetPlainText(out, sizeof(out));
    ExpectBytes(out, n, "AB\nCDEF", "get_plain_text after append_from_mem");
  }

  // ── S3: get_plain_text clamps to max_len (copy_slice_out) ──
  // text_buffer.rs:817 copy_slice_out: min(len, max_len), null/zero guard.
  {
    TextBuffer buf;
    SetContentViaMem(&buf, "Hello World");
    uint8_t out[32];
    ExpectEq(buf.GetPlainText(out, 32), 11, "get_plain_text writes all 11 bytes");
    ExpectBytes(out, buf.GetPlainText(out, 32), "Hello World",
                "get_plain_text content round-trips");
    uint32_t clamped = buf.GetPlainText(out, 5);
    ExpectBytes(out, clamped, "Hello", "get_plain_text clamps to max_len 5");
    ExpectEq(buf.GetPlainText(nullptr, 32), 0, "get_plain_text null out -> 0");
    ExpectEq(buf.GetPlainText(out, 0), 0, "get_plain_text max_len 0 -> 0");
  }

  // ── S3: get_text_range by character offsets (char_slice) ──
  // Content is stuie's own char_range fixture "Line 1\nLine 2\nLine 3"
  // (text_buffer.rs:1127). get_text_range slices by scalar offsets: chars
  // [7,13) are exactly "Line 2" (char 6 is the first '\n', 7..12 spell
  // "Line 2", 13 is the next '\n'). Expected values follow char_slice
  // (text_buffer.rs:916).
  {
    TextBuffer buf;
    SetContentViaMem(&buf, "Line 1\nLine 2\nLine 3");
    uint8_t out[64];
    ExpectBytes(out, buf.GetTextRange(7, 13, out, sizeof(out)), "Line 2",
                "get_text_range(7,13) == \"Line 2\"");
    ExpectBytes(out, buf.GetTextRange(0, 6, out, sizeof(out)), "Line 1",
                "get_text_range(0,6) == \"Line 1\"");
    ExpectEq(buf.GetTextRange(5, 5, out, sizeof(out)), 0,
             "get_text_range empty range -> 0");
    // end past the content clamps to the whole 20-scalar buffer.
    ExpectBytes(out, buf.GetTextRange(0, 100, out, sizeof(out)),
                "Line 1\nLine 2\nLine 3",
                "get_text_range end past content clamps to full content");
  }

  // ── S3: get_text_range_by_coords (coord_to_char_offset + char_slice) ──
  // Same fixture content. coord_to_char_offset (text_buffer.rs:924) maps
  // (row,col) to a scalar offset counting newlines: (1,0)->7, (1,6)->13, so
  // [(1,0),(1,6)) is "Line 2"; a whole-buffer coord range round-trips every
  // byte; a row past the content collapses to an empty range.
  {
    TextBuffer buf;
    SetContentViaMem(&buf, "Line 1\nLine 2\nLine 3");
    uint8_t out[64];
    ExpectBytes(out, buf.GetTextRangeByCoords(1, 0, 1, 6, out, sizeof(out)),
                "Line 2", "get_text_range_by_coords((1,0),(1,6)) == \"Line 2\"");
    ExpectBytes(out, buf.GetTextRangeByCoords(0, 0, 2, 6, out, sizeof(out)),
                "Line 1\nLine 2\nLine 3",
                "get_text_range_by_coords whole buffer round-trips");
    ExpectEq(buf.GetTextRangeByCoords(5, 0, 6, 0, out, sizeof(out)), 0,
             "get_text_range_by_coords past content -> 0");
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

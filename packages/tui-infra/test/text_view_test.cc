// Test for tui::TextBufferView lifecycle + geometry/indicator config (slice S5).
//
// There is no test runner in this package; this harness is compiled and run by
// hand, exactly as text_buffer_test.cc is:
//
//   /usr/bin/clang++ -std=c++20 -Iinclude -Wall -Wextra -o /tmp/tv \
//     test/text_view_test.cc \
//     src/socketsecurity/tui/{text_view,text_buffer,width,width_data}.cc \
//     && /tmp/tv
//
// The lifecycle cases mirror stuie's OWN cabi.rs #[test] fixtures
// (borrowed_edit_buffer_text_cannot_own_views cabi.rs:2565,
// owner_destroy_cascades_to_child_views cabi.rs:2581,
// stale_handles_are_safe_noops text_view.rs:2702). The config-setter cases take
// their expected values directly from the setter semantics in
// text_view.rs:632-646 (new) and text_view.rs:997-1036 (tbv_set_*) — e.g.
// set_viewport pins wrap_width to the viewport width, set_wrap_width(0) clears.

#include "tui/handles.hpp"
#include "tui/text_buffer.hpp"
#include "tui/text_view.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

void ExpectEqU64(uint64_t got, uint64_t want, const char* label) {
  if (got != want) {
    std::fprintf(stderr, "FAIL %s (got %llu, want %llu)\n", label,
                 static_cast<unsigned long long>(got),
                 static_cast<unsigned long long>(want));
    ++failures;
  }
}

void ExpectEqVec(const std::vector<uint32_t>& got,
                 const std::vector<uint32_t>& want, const char* label) {
  bool ok = got.size() == want.size();
  for (size_t i = 0; ok && i < got.size(); ++i) {
    ok = got[i] == want[i];
  }
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (got [", label);
    for (size_t i = 0; i < got.size(); ++i) {
      std::fprintf(stderr, "%s%u", i ? "," : "", got[i]);
    }
    std::fprintf(stderr, "], want [");
    for (size_t i = 0; i < want.size(); ++i) {
      std::fprintf(stderr, "%s%u", i ? "," : "", want[i]);
    }
    std::fprintf(stderr, "])\n");
    ++failures;
  }
}

// stuie's text_view.rs #[test] helper `set_text` + text_buffer::content_lines:
// build the logical-line vector the wrap kernel consumes.
std::vector<std::string> LinesOf(const std::string& content) {
  tui::TextBuffer b;
  b.SetContent(reinterpret_cast<const uint8_t*>(content.data()),
               content.size());
  return b.ContentLines();
}

// A BufferResolver over one fixed content (the harness has no binding-side
// TextBuffer registry). Mirrors the binding resolver: ContentLines + LineWidths
// + tab width. Every fixture drives a single buffer, so the handle is ignored.
tui::BufferResolver ResolverFor(const std::string& content,
                                uint32_t tab_width = tui::kDefaultTabWidth) {
  return [content, tab_width](uint32_t /*tb*/) -> tui::BufferData {
    std::vector<std::string> lines = LinesOf(content);
    std::vector<uint32_t> widths = tui::LineWidths(lines, tab_width);
    return tui::BufferData{std::move(lines), std::move(widths), tab_width};
  };
}

// Column widths of every virtual line, for the vector-equality fixtures.
std::vector<uint32_t> WidthsOf(const tui::VirtualLines& vl) {
  std::vector<uint32_t> out;
  for (const tui::VLine& v : vl.vlines) {
    out.push_back(v.width_cols);
  }
  return out;
}

std::vector<uint32_t> SourceLinesOf(const tui::VirtualLines& vl) {
  std::vector<uint32_t> out;
  for (const tui::VLine& v : vl.vlines) {
    out.push_back(v.source_line);
  }
  return out;
}

std::vector<uint32_t> SourceColsOf(const tui::VirtualLines& vl) {
  std::vector<uint32_t> out;
  for (const tui::VLine& v : vl.vlines) {
    out.push_back(v.source_col_offset);
  }
  return out;
}

// ── S7 helpers (selection + extraction + draw) ──

// Load content into a buffer (mirrors stuie's set_text test helper).
void SetText(tui::TextBuffer& b, const std::string& content) {
  b.SetContent(reinterpret_cast<const uint8_t*>(content.data()),
               content.size());
}

// A TextBufferAccessor / BufferResolver pair over ONE live buffer. Every fixture
// drives a single buffer, so the handle argument is ignored. Mirrors the binding
// seam (LookupTextBuffer + ResolveBufferData) but backed by a harness buffer.
tui::TextBufferAccessor AccessorFor(const tui::TextBuffer& buf) {
  return [&buf](uint32_t /*tb*/) -> const tui::TextBuffer* { return &buf; };
}

tui::BufferResolver ResolverForBuffer(const tui::TextBuffer& buf) {
  return [&buf](uint32_t /*tb*/) -> tui::BufferData {
    std::vector<std::string> lines = buf.ContentLines();
    std::vector<uint32_t> widths = tui::LineWidths(lines, buf.TabWidth());
    return tui::BufferData{std::move(lines), std::move(widths),
                           buf.TabWidth()};
  };
}

// The selected text as a string (empty when 0 bytes were written). Mirrors the
// stuie `selected_text` test helper (text_view.rs:2710-2718).
std::string SelectedText(uint32_t view, uint32_t byte_size,
                         const tui::TextBufferAccessor& accessor) {
  std::vector<uint8_t> buf(byte_size, 0);
  const uint32_t n =
      tui::TbvGetSelectedTextBytes(view, buf.data(), byte_size, accessor);
  return std::string(reinterpret_cast<char*>(buf.data()), n);
}

void ExpectStr(const std::string& got, const std::string& want,
               const char* label) {
  if (got != want) {
    std::fprintf(stderr, "FAIL %s (got \"%s\", want \"%s\")\n", label,
                 got.c_str(), want.c_str());
    ++failures;
  }
}

// Write a little-endian u64 at `base` (styled-record field packer).
void WriteU64(std::vector<uint8_t>& buf, size_t base, uint64_t v) {
  for (size_t i = 0; i < 8; ++i) {
    buf[base + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
  }
}

}  // namespace

int main() {
  using namespace tui;

  // ── Fresh view defaults (text_view.rs:632-646 TextBufferView::new) ──
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 7);
    TextBufferView v(tb);
    ExpectEq(v.active_tb, tb, "new: active_tb == creation buffer");
    ExpectEq(v.original_tb, tb, "new: original_tb == creation buffer");
    ExpectEq(v.wrap_mode, kWrapNone, "new: wrap_mode == WRAP_NONE (0)");
    Expect(!v.wrap_width.has_value(), "new: wrap_width is None");
    Expect(!v.viewport.has_value(), "new: viewport is None");
    ExpectEq(v.first_line_offset, 0, "new: first_line_offset == 0");
    Expect(!v.truncate, "new: truncate is false");
    Expect(!v.tab_indicator.has_value(), "new: tab_indicator is None");
    Expect(!v.tab_indicator_color.has_value(), "new: tab_indicator_color None");
  }

  // ── Config setters round-trip (text_view.rs:997-1036 tbv_set_*) ──
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 8);
    TextBufferView v(tb);

    // tbv_set_wrap_mode.
    v.SetWrapMode(1);  // char
    ExpectEq(v.wrap_mode, 1, "set_wrap_mode(1) round-trips");

    // tbv_set_wrap_width: non-zero sets Some, 0 clears to None.
    v.SetWrapWidth(17);
    Expect(v.wrap_width.has_value() && *v.wrap_width == 17,
           "set_wrap_width(17) -> Some(17)");
    v.SetWrapWidth(0);
    Expect(!v.wrap_width.has_value(), "set_wrap_width(0) -> None");

    // tbv_set_first_line_offset.
    v.SetFirstLineOffset(5);
    ExpectEq(v.first_line_offset, 5, "set_first_line_offset(5) round-trips");

    // tbv_set_viewport: stores the rect AND pins wrap_width to the width.
    v.SetViewport(1, 2, 10, 4);
    Expect(v.viewport.has_value() &&
               *v.viewport == (std::array<uint32_t, 4>{1, 2, 10, 4}),
           "set_viewport stores (x,y,w,h)");
    Expect(v.wrap_width.has_value() && *v.wrap_width == 10,
           "set_viewport pins wrap_width to viewport width");

    // tbv_set_viewport_size: keeps the existing (x,y), updates size, pins
    // wrap_width to the new width.
    v.SetViewportSize(20, 6);
    Expect(v.viewport.has_value() &&
               *v.viewport == (std::array<uint32_t, 4>{1, 2, 20, 6}),
           "set_viewport_size keeps offset, updates size");
    Expect(v.wrap_width.has_value() && *v.wrap_width == 20,
           "set_viewport_size pins wrap_width to new width");

    // tbv_set_truncate.
    v.SetTruncate(true);
    Expect(v.truncate, "set_truncate(true) round-trips");

    // tbv_set_tab_indicator.
    v.SetTabIndicator(0x2192);  // RIGHTWARDS ARROW
    Expect(v.tab_indicator.has_value() && *v.tab_indicator == 0x2192,
           "set_tab_indicator(U+2192) -> Some");

    // tbv_set_tab_indicator_color: Some(rgba) sets, None clears.
    v.SetTabIndicatorColor(std::array<uint16_t, 4>{9, 8, 7, 255});
    Expect(v.tab_indicator_color.has_value() &&
               *v.tab_indicator_color ==
                   (std::array<uint16_t, 4>{9, 8, 7, 255}),
           "set_tab_indicator_color(rgba) round-trips lanes");
    v.SetTabIndicatorColor(std::nullopt);
    Expect(!v.tab_indicator_color.has_value(),
           "set_tab_indicator_color(None) clears");
  }

  // ── set_viewport_size with no prior viewport defaults offset to (0,0) ──
  // text_view.rs:1018-1024: v.viewport.map(...).unwrap_or((0,0)).
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 9);
    TextBufferView v(tb);
    v.SetViewportSize(8, 3);
    Expect(v.viewport.has_value() &&
               *v.viewport == (std::array<uint32_t, 4>{0, 0, 8, 3}),
           "set_viewport_size with no prior viewport uses offset (0,0)");
    Expect(v.wrap_width.has_value() && *v.wrap_width == 8,
           "set_viewport_size pins wrap_width even without prior viewport");
  }

  // ── Borrowed-buffer rejection (cabi.rs:2565) ──
  // A borrowed inner buffer cannot own external views: createTextBufferView
  // returns the 0 sentinel. A plainly-owned buffer accepts a view.
  {
    // The binding reads TextBuffer::IsBorrowed() and passes that bit to
    // TbvCreateOwned; exercise the buffer flag directly so the tie-in is real.
    TextBuffer borrowed_buf;
    borrowed_buf.MarkBorrowed();
    Expect(borrowed_buf.IsBorrowed(), "MarkBorrowed -> IsBorrowed() true");
    TextBuffer owned_buf;
    Expect(!owned_buf.IsBorrowed(), "fresh buffer is not borrowed");

    const uint32_t borrowed_tb = handles::Tag(handles::kKindTextBuffer, 10);
    ExpectEq(TbvCreateOwned(borrowed_tb, borrowed_buf.IsBorrowed()), 0,
             "createTextBufferView over a borrowed buffer -> 0");

    const uint32_t owned_tb = handles::Tag(handles::kKindTextBuffer, 11);
    uint32_t view = TbvCreateOwned(owned_tb, owned_buf.IsBorrowed());
    Expect(view != 0, "createTextBufferView over an owned buffer -> non-zero");
    Expect(TbvIsValid(view), "the created view is valid");
    // The handle carries KIND_TEXT_BUFFER_VIEW (3) in its top nibble.
    ExpectEq(view >> handles::kIndexBits, handles::kKindTextBufferView,
             "view handle is kind-tagged KIND_TEXT_BUFFER_VIEW");
    TbvDestroy(view);
    Expect(!TbvIsValid(view), "destroyed view is no longer valid");
  }

  // ── Owner-destroy cascade (cabi.rs:2581) ──
  // Destroying the owning buffer invalidates the child view. The cascade
  // entrypoint is DestroyOwnedChildren — exactly what the binding's
  // DestroyOwnedTextBufferViews calls from destroy() BEFORE erasing the buffer.
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 12);
    uint32_t view = TbvCreateOwned(tb, /*buffer_borrowed=*/false);
    Expect(view != 0, "cascade: view created");
    Expect(TbvIsValid(view), "cascade: view valid before owner destroy");
    DestroyOwnedChildren(tb);  // stuie text_view::destroy_owned_children
    Expect(!TbvIsValid(view), "cascade: view invalid after owner destroy");
  }

  // ── Wrong-kind + stale handles resolve to a miss (text_view.rs:882, 2702) ──
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 13);
    uint32_t view = TbvCreateOwned(tb, false);
    Expect(view != 0, "wrong-kind: view created");
    const uint32_t index = view & handles::kIndexMask;
    // Same index, different kind (a text-buffer handle) must miss the view map.
    const uint32_t wrong_kind =
        handles::Tag(handles::kKindTextBuffer, index);
    Expect(!TbvIsValid(wrong_kind),
           "textBufferViewIsValid rejects a wrong-kind handle at same index");
    // A renderer-kinded handle (small raw integer) also misses.
    Expect(!TbvIsValid(handles::Tag(handles::kKindRenderer, index)),
           "textBufferViewIsValid rejects a renderer-kind handle");
    TbvDestroy(view);

    // Stale handles are safe no-ops.
    const uint32_t stale = handles::Tag(handles::kKindTextBufferView, 999999);
    Expect(!TbvIsValid(stale), "stale view handle is invalid");
    TbvDestroy(stale);                 // no-op, must not crash
    TbvSetWrapMode(stale, 2);          // no-op on a missing view
    TbvSetViewport(stale, 0, 0, 4, 4);
    TbvSetTabIndicatorColor(stale, std::array<uint16_t, 4>{1, 2, 3, 4});
    DestroyOwnedChildren(handles::Tag(handles::kKindTextBuffer, 424242));
    Expect(!TbvIsValid(stale), "stale view handle still invalid after no-ops");
  }

  // ═══════════════════════════════════════════════════════════════════════
  // Slice S6: virtual-line layout / measurement kernel.
  // Every expected value below is lifted DIRECTLY from stuie's own #[test]
  // fixtures in crates/stuie-cabi/src/text_view.rs (cited per block).
  // ═══════════════════════════════════════════════════════════════════════

  // TextBuffer::ContentLines split semantics (from_utf8_lossy + split('\n')).
  {
    ExpectEqVec({static_cast<uint32_t>(LinesOf("").size())}, {1},
                "content_lines: empty buffer is one line");
    Expect(LinesOf("").at(0).empty(), "content_lines: empty buffer -> [\"\"]");
    const std::vector<std::string> abc = LinesOf("a\nb\n");
    Expect(abc.size() == 3 && abc[0] == "a" && abc[1] == "b" && abc[2].empty(),
           "content_lines: trailing newline yields a final empty line");
  }

  // char_wrap_splits_overwide_line (text_view.rs:2488-2506): "1111111111 1111111"
  // (18 cols) char-wraps at width 17 into two vlines [17, 1].
  {
    const VirtualLines vl =
        CalculateVirtualLines(LinesOf("1111111111 1111111"),
                              kDefaultTabWidth, /*char*/ 1, 17, 0);
    ExpectEq(static_cast<uint32_t>(vl.vlines.size()), 2, "char_split: 2 vlines");
    ExpectEq(vl.vlines[0].width_cols, 17, "char_split: vline0 width 17");
    ExpectEq(vl.vlines[1].width_cols, 1, "char_split: vline1 width 1");
    ExpectEq(vl.vlines[1].source_col_offset, 17,
             "char_split: vline1 source_col_offset 17");
    ExpectEq(vl.vlines[0].source_line, 0, "char_split: vline0 source_line 0");
    ExpectEq(vl.vlines[1].source_line, 0, "char_split: vline1 source_line 0");
    ExpectEqVec(vl.first_vline, {0}, "char_split: first_vline == [0]");
    ExpectEqVec(vl.vline_counts, {2}, "char_split: vline_counts == [2]");
  }

  // char_wrap_line_sources_span_multiple_logical_lines (text_view.rs:2531-2542).
  {
    const VirtualLines vl = CalculateVirtualLines(
        LinesOf("1111111111 1111111\n2222222222 2222222\n333\n444\n555"),
        kDefaultTabWidth, /*char*/ 1, 17, 0);
    ExpectEqVec(SourceLinesOf(vl), {0, 0, 1, 1, 2, 3, 4},
                "multi_line: sources");
    ExpectEqVec(vl.first_vline, {0, 2, 4, 5, 6}, "multi_line: first_vline");
    ExpectEqVec(vl.vline_counts, {2, 2, 1, 1, 1}, "multi_line: vline_counts");
  }

  // word_wrap_keeps_words_intact (text_view.rs:2544-2553): "The quick brown fox"
  // at width 10 -> "The quick " (10), "brown fox" (9).
  {
    const VirtualLines vl = CalculateVirtualLines(
        LinesOf("The quick brown fox"), kDefaultTabWidth, /*word*/ 2, 10, 0);
    ExpectEqVec(WidthsOf(vl), {10, 9}, "word_wrap: widths [10,9]");
  }

  // word_wrap_falls_back_to_char_for_long_word (text_view.rs:2555-2563).
  {
    const VirtualLines vl =
        CalculateVirtualLines(LinesOf("ABCDEFGHIJKLMNOPQRSTUVWXYZ"),
                              kDefaultTabWidth, /*word*/ 2, 10, 0);
    ExpectEqVec(WidthsOf(vl), {10, 10, 6}, "word_wrap: long word char-fills");
  }

  // word_overflow_enters_fill_mode_for_rest_of_line (text_view.rs:2565-2579).
  {
    const VirtualLines vl = CalculateVirtualLines(
        LinesOf("Amazon Web Services"), kDefaultTabWidth, /*word*/ 2, 5, 0);
    ExpectEqVec(WidthsOf(vl), {5, 5, 5, 4}, "fill_mode: widths [5,5,5,4]");
    ExpectEqVec(SourceColsOf(vl), {0, 5, 10, 15},
                "fill_mode: source_col_offsets");
  }

  // cjk_run_overflow_fills_width_54_lines (text_view.rs:2581-2597): the
  // TextTable long-cjk-phrase torture row wraps into [54,54,54,54,31] at
  // width 54 — a direct exercise of the char_width/is_wide port.
  {
    const VirtualLines vl = CalculateVirtualLines(
        LinesOf("長文の日本語テキストと中文段落和한국어문장을連続して配置し、"
                "その後に additional English context describing renderer "
                "behavior, border intersection handling, and selection "
                "extraction so that this single cell remains a reliable "
                "wrapping torture test."),
        kDefaultTabWidth, /*word*/ 2, 54, 0);
    ExpectEqVec(WidthsOf(vl), {54, 54, 54, 54, 31}, "cjk_torture: widths");
  }

  // cjk_ascii_transition_is_a_break_but_cjk_run_is_one_unit
  // (text_view.rs:2599-2614): "foo你好bar" packs onto one width-10 line, and at
  // width 7 the trailing "bar" unit wraps whole.
  {
    ExpectEqVec(WidthsOf(CalculateVirtualLines(LinesOf("foo你好bar"),
                                               kDefaultTabWidth, 2, 10, 0)),
                {10}, "cjk_break: width 10 -> [10]");
    ExpectEqVec(WidthsOf(CalculateVirtualLines(LinesOf("foo你好bar"),
                                               kDefaultTabWidth, 2, 7, 0)),
                {7, 3}, "cjk_break: width 7 -> [7,3]");
  }

  // empty_line_between_words_is_kept (text_view.rs:2616-2626).
  {
    const VirtualLines vl = CalculateVirtualLines(
        LinesOf("First line\n\nThird line"), kDefaultTabWidth, 2, 8, 0);
    bool has_line1 = false;
    for (const VLine& v : vl.vlines) {
      has_line1 = has_line1 || v.source_line == 1;
    }
    Expect(has_line1, "empty_line: a vline sources logical line 1");
    ExpectEq(vl.vline_counts[1], 1, "empty_line: empty line -> 1 vline");
  }

  // ── tbv_measure packed u64 (text_view.rs measure fixtures) ──

  // no_wrap_reports_widest_line_and_count (text_view.rs:2462-2473).
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 200);
    const uint32_t view = TbvCreate(tb);
    TbvSetWrapMode(view, kWrapNone);
    const uint64_t packed = TbvMeasure(
        view, 80, 30, ResolverFor("Short\nAVeryLongLineHere\nMedium"));
    ExpectEqU64(packed, (uint64_t{3} << 32) | 17,
                "measure no-wrap: (3 lines, 17 cols)");
    TbvDestroy(view);
  }

  // char_wrap_fitting_content_is_one_line (text_view.rs:2475-2486).
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 201);
    const uint32_t view = TbvCreate(tb);
    TbvSetWrapMode(view, 1);  // char
    const uint64_t packed =
        TbvMeasure(view, 80, 30, ResolverFor("ABCDEFGHIJKLMNOPQRST"));
    ExpectEqU64(packed, (uint64_t{1} << 32) | 20,
                "measure char-fit: (1 line, 20 cols)");
    TbvDestroy(view);
  }

  // measure_honors_inline_first_line_offset (text_view.rs:2508-2529).
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 202);
    const uint32_t view = TbvCreate(tb);
    TbvSetWrapMode(view, 1);   // char
    TbvSetWrapWidth(view, 10);
    const BufferResolver resolve = ResolverFor("abcdef");
    ExpectEqU64(TbvMeasure(view, 10, 30, resolve) >> 32, 1,
                "measure offset=0: 1 line");
    TbvSetFirstLineOffset(view, 5);
    ExpectEqU64(TbvMeasure(view, 10, 30, resolve) >> 32, 2,
                "measure offset=5: 2 lines");
    TbvDestroy(view);
  }

  // empty_buffer_measures_one_by_zero (text_view.rs:2628-2638).
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 203);
    const uint32_t view = TbvCreate(tb);
    TbvSetWrapMode(view, 1);  // char
    ExpectEqU64(TbvMeasure(view, 80, 30, ResolverFor("")),
                (uint64_t{1} << 32) | 0, "measure empty: (1 line, 0 cols)");
    TbvDestroy(view);
  }

  // ── tbv_virtual_line_count + out-buffer serialization contract ──
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 204);
    const uint32_t view = TbvCreate(tb);
    TbvSetWrapMode(view, 1);   // char
    TbvSetWrapWidth(view, 17);
    const BufferResolver resolve = ResolverFor("1111111111 1111111");

    ExpectEq(TbvVirtualLineCount(view, resolve), 2,
             "virtual_line_count: 2 vlines");

    // getLogicalLineInfo: full arrays, widthColsMax = max LOGICAL line width
    // ("1111111111 1111111" is 18 cols wide).
    uint32_t buf[64] = {0};
    const uint32_t n = TbvGetLogicalLineInfo(view, buf, 64, resolve);
    ExpectEq(n, 2, "logical_line_info: count 2");
    ExpectEq(buf[0], 2, "logical_line_info: out[0] count");
    ExpectEq(buf[1], 18, "logical_line_info: out[1] widthColsMax (logical=18)");
    // Layout: starts[2..], widths[2+n..], sources[2+2n..], wraps[2+3n..].
    ExpectEq(buf[2], 0, "logical_line_info: starts[0]");
    ExpectEq(buf[3], 17, "logical_line_info: starts[1]");
    ExpectEq(buf[4], 17, "logical_line_info: widths[0]");
    ExpectEq(buf[5], 1, "logical_line_info: widths[1]");
    ExpectEq(buf[6], 0, "logical_line_info: sources[0]");
    ExpectEq(buf[7], 0, "logical_line_info: sources[1]");
    ExpectEq(buf[8], 0, "logical_line_info: wraps[0]");
    ExpectEq(buf[9], 1, "logical_line_info: wraps[1]");

    // getLineInfo (no viewport): widthColsMax = max over the SLICE (17).
    uint32_t lbuf[64] = {0};
    const uint32_t m = TbvGetLineInfo(view, lbuf, 64, resolve);
    ExpectEq(m, 2, "line_info: count 2");
    ExpectEq(lbuf[1], 17, "line_info: widthColsMax over slice (17)");

    TbvDestroy(view);
  }

  // getLineInfo viewport slicing: viewport (x=0, y=1, w=17, h=1) selects just
  // the second virtual line. tbv_get_line_info slices [y, y+h).
  {
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 205);
    const uint32_t view = TbvCreate(tb);
    TbvSetWrapMode(view, 1);  // char
    TbvSetViewport(view, 0, 1, 17, 1);  // also pins wrap_width to 17
    const BufferResolver resolve = ResolverFor("1111111111 1111111");
    uint32_t buf[64] = {0};
    const uint32_t n = TbvGetLineInfo(view, buf, 64, resolve);
    ExpectEq(n, 1, "line_info viewport: sliced to 1 vline");
    ExpectEq(buf[0], 1, "line_info viewport: out[0] count 1");
    ExpectEq(buf[1], 1, "line_info viewport: widthColsMax 1");
    ExpectEq(buf[2], 17, "line_info viewport: starts[0] == 17 (2nd vline)");
    ExpectEq(buf[3], 1, "line_info viewport: widths[0] == 1");
    ExpectEq(buf[5], 1, "line_info viewport: wraps[0] == 1");
    TbvDestroy(view);
  }

  // ═══ Slice S7: selection + extraction + draw ═══
  // Every expected value below is taken directly from stuie's OWN #[test]
  // fixtures in text_view.rs (the test names are quoted per block).

  // empty_selection_reports_none_and_no_text (text_view.rs:2721).
  {
    TextBuffer buf;
    SetText(buf, "Hello World");
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 300);
    const uint32_t view = TbvCreate(tb);
    ExpectEqU64(TbvGetSelection(view), kNoSelection,
                "empty selection packs as NO_SELECTION");
    ExpectStr(SelectedText(view, 11, AccessorFor(buf)), "",
              "empty selection extracts no text");
    TbvDestroy(view);
  }

  // zero_width_selection_packs_as_none (text_view.rs:2732).
  {
    TextBuffer buf;
    SetText(buf, "Hello World");
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 301);
    const uint32_t view = TbvCreate(tb);
    TbvSetSelection(view, 3, 3, std::nullopt, std::nullopt);
    ExpectEqU64(TbvGetSelection(view), kNoSelection,
                "zero-width selection packs as NO_SELECTION");
    ExpectStr(SelectedText(view, 11, AccessorFor(buf)), "",
              "zero-width selection extracts no text");
    TbvDestroy(view);
  }

  // explicit_selection_packs_and_slices (text_view.rs:2744).
  {
    TextBuffer buf;
    SetText(buf, "Hello World");
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 302);
    const uint32_t view = TbvCreate(tb);
    const TextBufferAccessor acc = AccessorFor(buf);
    TbvSetSelection(view, 0, 5, std::nullopt, std::nullopt);
    ExpectEqU64(TbvGetSelection(view), 5, "explicit [0,5) packs as (0<<32)|5");
    ExpectStr(SelectedText(view, 11, acc), "Hello", "[0,5) extracts \"Hello\"");
    TbvUpdateSelection(view, 11, std::nullopt, std::nullopt);
    ExpectStr(SelectedText(view, 11, acc), "Hello World",
              "updateSelection(11) extracts the whole line");
    TbvResetSelection(view);
    ExpectEqU64(TbvGetSelection(view), kNoSelection,
                "resetSelection clears to NO_SELECTION");
    TbvDestroy(view);
  }

  // width_aware_selection_slices_grapheme_whole (text_view.rs:2760): emoji is 2
  // columns, so [6,8) selects the whole 🌍 cluster.
  {
    TextBuffer buf;
    SetText(buf, "Hello \xF0\x9F\x8C\x8D");  // "Hello 🌍"
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 303);
    const uint32_t view = TbvCreate(tb);
    const TextBufferAccessor acc = AccessorFor(buf);
    TbvSetSelection(view, 6, 8, std::nullopt, std::nullopt);
    ExpectStr(SelectedText(view, 16, acc), "\xF0\x9F\x8C\x8D",
              "[6,8) selects the whole emoji grapheme");
    TbvSetSelection(view, 0, 8, std::nullopt, std::nullopt);
    ExpectStr(SelectedText(view, 16, acc), "Hello \xF0\x9F\x8C\x8D",
              "[0,8) selects the whole string");
    TbvDestroy(view);
  }

  // local_selection_resolves_single_line_coords (text_view.rs:2775).
  {
    TextBuffer buf;
    SetText(buf, "Hello World");
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 304);
    const uint32_t view = TbvCreate(tb);
    const BufferResolver resolve = ResolverForBuffer(buf);
    Expect(TbvSetLocalSelection(view, 0, 0, 11, 0, std::nullopt, std::nullopt,
                                resolve),
           "setLocalSelection(0,0..11,0) reports changed");
    ExpectEqU64(TbvGetSelection(view), 11u, "single-line local selection == 11");
    ExpectStr(SelectedText(view, 11, AccessorFor(buf)), "Hello World",
              "single-line local selection extracts the line");
    TbvDestroy(view);
  }

  // local_selection_spans_multiline_col_offsets (text_view.rs:2787): the newline
  // occupies one column, so [0,20) spans both rows.
  {
    TextBuffer buf;
    SetText(buf, "First row\nSecond row");
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 305);
    const uint32_t view = TbvCreate(tb);
    const BufferResolver resolve = ResolverForBuffer(buf);
    Expect(TbvSetLocalSelection(view, 0, 0, 99, 5, std::nullopt, std::nullopt,
                                resolve),
           "setLocalSelection across rows reports changed");
    ExpectEqU64(TbvGetSelection(view), 20u, "multiline local selection == 20");
    ExpectStr(SelectedText(view, 20, AccessorFor(buf)), "First row\nSecond row",
              "multiline local selection extracts both rows + newline");
    TbvDestroy(view);
  }

  // reverse_focus_extends_end_by_one (text_view.rs:2801).
  {
    TextBuffer buf;
    SetText(buf, "Hello World");
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 306);
    const uint32_t view = TbvCreate(tb);
    const BufferResolver resolve = ResolverForBuffer(buf);
    Expect(TbvSetLocalSelection(view, 5, 0, 5, 0, std::nullopt, std::nullopt,
                                resolve),
           "anchor at col 5 reports changed");
    Expect(TbvUpdateLocalSelection(view, 0, 0, 2, 0, std::nullopt, std::nullopt,
                                   resolve),
           "backward focus reports changed");
    ExpectEqU64(TbvGetSelection(view), (uint64_t{2} << 32) | 6,
                "reverse focus extends end by one -> (2<<32)|6");
    TbvDestroy(view);
  }

  // reset_local_selection_clears_anchor (text_view.rs:2814).
  {
    TextBuffer buf;
    SetText(buf, "Hello World");
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 307);
    const uint32_t view = TbvCreate(tb);
    const BufferResolver resolve = ResolverForBuffer(buf);
    Expect(TbvSetLocalSelection(view, 0, 0, 5, 0, std::nullopt, std::nullopt,
                                resolve),
           "initial local selection reports changed");
    TbvResetLocalSelection(view);
    ExpectEqU64(TbvGetSelection(view), kNoSelection,
                "resetLocalSelection clears to NO_SELECTION");
    // With the anchor cleared, updateLocalSelection falls back to a fresh
    // setLocalSelection: [2,4) -> (2<<32)|4.
    Expect(TbvUpdateLocalSelection(view, 2, 0, 4, 0, std::nullopt, std::nullopt,
                                   resolve),
           "update after reset falls back to setLocalSelection");
    ExpectEqU64(TbvGetSelection(view), (uint64_t{2} << 32) | 4,
                "update after reset -> (2<<32)|4");
    TbvDestroy(view);
  }

  // selection_on_stale_handle_is_safe (text_view.rs:2828).
  {
    TextBuffer buf;  // unused sink for the accessor/resolver
    const BufferResolver resolve = ResolverForBuffer(buf);
    const TextBufferAccessor acc = AccessorFor(buf);
    ExpectEqU64(TbvGetSelection(555555), kNoSelection,
                "stale getSelection -> NO_SELECTION");
    Expect(!TbvSetLocalSelection(555555, 0, 0, 1, 0, std::nullopt, std::nullopt,
                                 resolve),
           "stale setLocalSelection -> false");
    Expect(!TbvUpdateLocalSelection(555555, 0, 0, 1, 0, std::nullopt,
                                    std::nullopt, resolve),
           "stale updateLocalSelection -> false");
    // The remaining setters are safe no-ops on a stale handle.
    TbvSetSelection(555555, 0, 1, std::nullopt, std::nullopt);
    TbvUpdateSelection(555555, 2, std::nullopt, std::nullopt);
    TbvResetSelection(555555);
    TbvResetLocalSelection(555555);
    uint8_t sbuf[8] = {0};
    ExpectEq(TbvGetSelectedTextBytes(555555, sbuf, 8, acc), 0,
             "stale getSelectedTextBytes -> 0");
  }

  // getPlainText copies the active buffer's raw content (tbv_get_plain_text_bytes
  // text_view.rs:1219; exercised by the ev_get_plain_text path).
  {
    TextBuffer buf;
    SetText(buf, "Hello World");
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 308);
    const uint32_t view = TbvCreate(tb);
    uint8_t pbuf[16] = {0};
    const uint32_t n = TbvGetPlainTextBytes(view, pbuf, 16, AccessorFor(buf));
    ExpectStr(std::string(reinterpret_cast<char*>(pbuf), n), "Hello World",
              "getPlainText copies the raw content");
    // max_len == 0 writes nothing.
    ExpectEq(TbvGetPlainTextBytes(view, pbuf, 0, AccessorFor(buf)), 0u,
             "getPlainText(max_len=0) -> 0");
    TbvDestroy(view);
  }

  // truncated_line_draws_prefix_ellipsis_suffix (text_view.rs:2842): the draw
  // path renders "012" + "..." + "GHIJ".
  {
    TextBuffer buf;
    SetText(buf, "0123456789ABCDEFGHIJ");  // 20 cols
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 309);
    const uint32_t view = TbvCreate(tb);
    TbvSetTruncate(view, true);
    TbvSetViewport(view, 0, 0, 10, 1);
    const std::optional<DrawModel> model = TbvDrawModel(view, AccessorFor(buf));
    Expect(model.has_value(), "truncated draw model built");
    if (model.has_value()) {
      // Same truncation geometry as truncation_splits_prefix_ellipsis_suffix
      // (text_view.rs:2826): ellipsis_pos (10-3)/2 == 3, suffix_start 20-4 == 16.
      Expect(!model->vlines.empty() && model->vlines[0].is_truncated,
             "vline[0] is truncated");
      ExpectEq(model->vlines[0].ellipsis_pos, 3, "ellipsis_pos == 3");
      ExpectEq(model->vlines[0].truncation_suffix_start, 16,
               "truncation_suffix_start == 16");
      ExpectEq(model->vlines[0].width_cols, 10, "truncated width_cols == 10");
      std::string chars;
      DrawModelInto(
          *model,
          [&](std::string_view s, int32_t /*cx*/, int32_t /*cy*/,
              const std::array<uint16_t, 4>& /*fg*/,
              const std::array<uint16_t, 4>& /*bg*/, uint32_t /*attrs*/) {
            chars.append(s);
          },
          std::array<uint16_t, 4>{255, 255, 255, 255},
          std::array<uint16_t, 4>{0, 0, 0, 0}, 0, 0);
      ExpectStr(chars, "012...GHIJ", "truncated line draws prefix+...+suffix");
    }
    TbvDestroy(view);
  }

  // draw_composites_styled_fg (text_view.rs:2879): both cells carry the styled
  // red fg (value byte 255,0,0 in the r/g/b lanes).
  {
    TextBuffer buf;
    // One styled chunk "AB" with fg = red {255,0,0,255}, packed as a single
    // kStyledRecordSize StyledChunkStruct (mirror of styled_record
    // text_view.rs:2848). The text/fg pointers borrow stable stack storage.
    static const uint16_t red[4] = {255, 0, 0, 255};
    const char* text = "AB";
    std::vector<uint8_t> records(tui::kStyledRecordSize, 0);
    WriteU64(records, 0, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(text)));
    WriteU64(records, 8, 2);  // text_len
    WriteU64(records, 16,
             static_cast<uint64_t>(reinterpret_cast<uintptr_t>(red)));  // fg ptr
    buf.SetStyled(records.data(), records.size(), 1);
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 310);
    const uint32_t view = TbvCreate(tb);
    const std::optional<DrawModel> model = TbvDrawModel(view, AccessorFor(buf));
    Expect(model.has_value(), "styled draw model built");
    if (model.has_value()) {
      std::vector<std::array<uint16_t, 4>> fgs;
      DrawModelInto(
          *model,
          [&](std::string_view /*s*/, int32_t /*cx*/, int32_t /*cy*/,
              const std::array<uint16_t, 4>& fg,
              const std::array<uint16_t, 4>& /*bg*/, uint32_t /*attrs*/) {
            fgs.push_back(fg);
          },
          std::array<uint16_t, 4>{255, 255, 255, 255},
          std::array<uint16_t, 4>{0, 0, 0, 0}, 0, 0);
      ExpectEq(static_cast<uint32_t>(fgs.size()), 2u,
               "styled draw emits two cells (A, B)");
      for (const std::array<uint16_t, 4>& fg : fgs) {
        Expect((fg[0] & 0xff) == 255 && (fg[1] & 0xff) == 0 &&
                   (fg[2] & 0xff) == 0,
               "styled cell fg is pure red");
      }
    }
    TbvDestroy(view);
  }

  // draw_renders_tab_indicator_and_fill (text_view.rs:2915): "A" then the tab
  // indicator '→' + a fill space, then "B" shifted by the tab width.
  {
    TextBuffer buf;
    SetText(buf, "A\tB");  // tab_width default 2
    const uint32_t tb = handles::Tag(handles::kKindTextBuffer, 311);
    const uint32_t view = TbvCreate(tb);
    TbvSetTabIndicator(view, 0x2192);  // RIGHTWARDS ARROW '→'
    const std::optional<DrawModel> model = TbvDrawModel(view, AccessorFor(buf));
    Expect(model.has_value(), "tab draw model built");
    if (model.has_value()) {
      std::vector<std::pair<std::string, int32_t>> cells;
      DrawModelInto(
          *model,
          [&](std::string_view s, int32_t cx, int32_t /*cy*/,
              const std::array<uint16_t, 4>& /*fg*/,
              const std::array<uint16_t, 4>& /*bg*/, uint32_t /*attrs*/) {
            cells.emplace_back(std::string(s), cx);
          },
          std::array<uint16_t, 4>{255, 255, 255, 255},
          std::array<uint16_t, 4>{0, 0, 0, 0}, 0, 0);
      std::string joined;
      for (const auto& c : cells) {
        joined += c.first;
      }
      ExpectStr(joined, "A\xE2\x86\x92 B", "tab draws A + '→' + fill + B");
      ExpectEq(static_cast<uint32_t>(cells.size()), 4u,
               "tab draw emits four cells");
      if (cells.size() == 4) {
        ExpectEq(static_cast<uint32_t>(cells[1].second), 1u,
                 "indicator glyph at column 1");
        ExpectEq(static_cast<uint32_t>(cells[3].second), 3u,
                 "'B' shifted by the tab width to column 3");
      }
    }
    TbvDestroy(view);
  }

  if (failures == 0) {
    std::printf("TEXT_VIEW_OK\n");
    return 0;
  }
  std::fprintf(stderr, "%d failure(s)\n", failures);
  return 1;
}

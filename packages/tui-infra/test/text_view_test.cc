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

  if (failures == 0) {
    std::printf("TEXT_VIEW_OK\n");
    return 0;
  }
  std::fprintf(stderr, "%d failure(s)\n", failures);
  return 1;
}

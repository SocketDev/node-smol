// Native TextBufferView lifecycle + geometry/indicator config.
//
// Port of the lifecycle + config-setter subset of stuie's
// crates/stuie-cabi/src/text_view.rs — the native measurement/wrapping layer
// OpenTUI's text-buffer-view.ts drives through the FFI RenderLib. A
// TextBufferView is created over a TextBuffer handle and carries the wrap /
// viewport / truncation / tab-indicator configuration the measurement and draw
// paths (later slices) read back.
//
// Slice S5 scope: view lifecycle (create / is-valid / destroy + owner-destroy
// cascade) and the geometry/indicator config setters. The virtual-line
// measurement, selection, and draw machinery in text_view.rs is NOT ported here
// — those grow the struct in later slices.
//
// Handles are kind-tagged u32 registry ids (KIND_TEXT_BUFFER_VIEW, see
// tui/handles.hpp <- stuie handles.rs), so a stale OR wrong-kind handle misses
// the view map and reads as a safe no-op — the "stale AND wrong-kind both
// resolve to a miss" contract the lifecycle suite relies on (text_view.rs:20-21).
//
// Registry placement: unlike the binding-owned TextBuffer registry, the view
// registry + the per-buffer owned-child index live HERE (mirroring stuie's
// module-level TBV_REGISTRY / OWNED_CHILD_VIEWS) so the owner-destroy cascade
// (text_buffer destroy() -> destroy_owned_children) and the borrowed-buffer
// rejection are exercisable by the pure-C++ view harness. The tui_binding.cc
// view family just forwards handles to these free functions; its
// DestroyOwnedTextBufferViews seam calls DestroyOwnedChildren.

#ifndef TUI_INFRA_TEXT_VIEW_HPP_
#define TUI_INFRA_TEXT_VIEW_HPP_

#include <array>
#include <cstdint>
#include <optional>

namespace tui {

// No wrapping (1:1 logical->visual line mapping). text_view.rs:29 WRAP_NONE.
inline constexpr uint8_t kWrapNone = 0;

// A native text-buffer view: the active/original buffer handles plus the wrap /
// viewport / truncation / tab-indicator configuration. Mirrors the lifecycle +
// config fields of text_view.rs:611-629 TextBufferView (the selection and
// derived-model fields are deferred to later slices).
struct TextBufferView {
  // Buffer currently measured/rendered (may be a placeholder an EditorView
  // swaps in). Starts as the creation buffer. text_view.rs:613-615.
  uint32_t active_tb;
  // The buffer this view was created over; switch_to_original restores it.
  // text_view.rs:616-617.
  uint32_t original_tb;

  // Wrap mode code: 0=none, 1=char, 2=word. text_view.rs:617.
  uint8_t wrap_mode = kWrapNone;
  // Wrap width in columns; nullopt disables wrapping. text_view.rs:618.
  std::optional<uint32_t> wrap_width;
  // Viewport (offset_x, offset_y, width, height); nullopt = unclipped.
  // text_view.rs:619.
  std::optional<std::array<uint32_t, 4>> viewport;
  // Inline first-line column offset (narrows the first virtual line).
  // text_view.rs:621.
  uint32_t first_line_offset = 0;
  // Whether overwide virtual lines get a middle "..." ellipsis. text_view.rs:621.
  bool truncate = false;
  // Tab-indicator glyph codepoint (draw path); nullopt = none. text_view.rs:622.
  std::optional<uint32_t> tab_indicator;
  // Tab-indicator color RGBA lanes; nullopt = fall back to the cell foreground.
  // text_view.rs:623.
  std::optional<std::array<uint16_t, 4>> tab_indicator_color;

  // text_view.rs:632-646 TextBufferView::new — active_tb == original_tb == tb,
  // all config at its documented default.
  explicit TextBufferView(uint32_t tb) : active_tb(tb), original_tb(tb) {}

  // ── Config setters (faithful to text_view.rs:997-1036) ──

  // tbv_set_wrap_mode (text_view.rs:997-999).
  void SetWrapMode(uint8_t mode) {
    wrap_mode = mode;
  }

  // tbv_set_wrap_width (text_view.rs:1001-1005): a 0 width clears wrapping.
  void SetWrapWidth(uint32_t width) {
    wrap_width = width == 0 ? std::optional<uint32_t>{} : std::optional<uint32_t>{width};
  }

  // tbv_set_first_line_offset (text_view.rs:1007-1009).
  void SetFirstLineOffset(uint32_t offset) {
    first_line_offset = offset;
  }

  // tbv_set_viewport (text_view.rs:1011-1016): also pins wrap_width to the
  // viewport width.
  void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    viewport = std::array<uint32_t, 4>{x, y, width, height};
    wrap_width = width;
  }

  // tbv_set_viewport_size (text_view.rs:1018-1024): keep the existing offset
  // (0,0 when unset), update size, and pin wrap_width to the new width.
  void SetViewportSize(uint32_t width, uint32_t height) {
    uint32_t x = viewport ? (*viewport)[0] : 0;
    uint32_t y = viewport ? (*viewport)[1] : 0;
    viewport = std::array<uint32_t, 4>{x, y, width, height};
    wrap_width = width;
  }

  // tbv_set_truncate (text_view.rs:1026-1028).
  void SetTruncate(bool value) {
    truncate = value;
  }

  // tbv_set_tab_indicator (text_view.rs:1030-1032).
  void SetTabIndicator(uint32_t code_point) {
    tab_indicator = code_point;
  }

  // tbv_set_tab_indicator_color (text_view.rs:1034-1036): a null color clears.
  void SetTabIndicatorColor(std::optional<std::array<uint16_t, 4>> color) {
    tab_indicator_color = color;
  }
};

// ── View registry (handle -> TextBufferView) ──
// Mirrors stuie's module-level TBV_REGISTRY (text_view.rs:841-874).

// Create a bare view over buffer handle `tb` and return its (kind-tagged)
// handle. No borrow check, no owned-child registration — this is the raw
// tbv_create the EditorView path (later slice) uses. text_view.rs:867-874.
uint32_t TbvCreate(uint32_t tb);

// createTextBufferView (stuie cabi.rs:1391): reject a view over a BORROWED
// buffer by returning the 0 "failed" handle (the shim then throws); otherwise
// create the view AND register it as an owned child of `tb` so destroying the
// buffer cascades to it. `buffer_borrowed` is the buffer's borrowed flag as the
// caller resolved it from the buffer registry (a stale/missing buffer is not
// borrowed, so it falls through to a view over an empty buffer).
uint32_t TbvCreateOwned(uint32_t tb, bool buffer_borrowed);

// Whether `handle` names a live view. Stale AND wrong-kind handles report false
// (the map keys on the full tagged handle). text_view.rs:882-884.
bool TbvIsValid(uint32_t handle);

// Destroy a view (idempotent; a stale/wrong-kind handle is a no-op).
// text_view.rs:876-878.
void TbvDestroy(uint32_t handle);

// ── Owned-child index (buffer handle -> owned view handles) ──
// Mirrors stuie's OWNED_CHILD_VIEWS (text_view.rs:891-918).

// Record `view` as an owned child of buffer `tb`. text_view.rs:902-907.
void RegisterOwnedChild(uint32_t tb, uint32_t view);

// Destroy every owned child view of buffer `tb` (idempotent). Called by the
// binding's DestroyOwnedTextBufferViews from destroy() BEFORE the buffer is
// erased. text_view.rs:911-918 destroy_owned_children.
void DestroyOwnedChildren(uint32_t tb);

// ── Config setters keyed by handle (registry lookup + method call) ──
// Each is a stale-safe no-op for a missing/wrong-kind handle.

void TbvSetWrapMode(uint32_t handle, uint8_t mode);
void TbvSetWrapWidth(uint32_t handle, uint32_t width);
void TbvSetFirstLineOffset(uint32_t handle, uint32_t offset);
void TbvSetViewport(uint32_t handle, uint32_t x, uint32_t y, uint32_t width,
                    uint32_t height);
void TbvSetViewportSize(uint32_t handle, uint32_t width, uint32_t height);
void TbvSetTruncate(uint32_t handle, bool truncate);
void TbvSetTabIndicator(uint32_t handle, uint32_t code_point);
void TbvSetTabIndicatorColor(uint32_t handle,
                             std::optional<std::array<uint16_t, 4>> color);

}  // namespace tui

#endif  // TUI_INFRA_TEXT_VIEW_HPP_

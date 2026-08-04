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
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/text_buffer.hpp"  // SpanRun, TextBuffer (draw model + extraction)

namespace tui {

// No wrapping (1:1 logical->visual line mapping). text_view.rs:29 WRAP_NONE.
inline constexpr uint8_t kWrapNone = 0;

// packSelectionInfo's "no selection" sentinel: all-ones u64. Returned by
// GetSelection for a cleared OR zero-width selection. text_view.rs:33
// NO_SELECTION.
inline constexpr uint64_t kNoSelection = 0xFFFF'FFFF'FFFF'FFFFULL;

// An active text selection over the view's newline-inclusive column space.
// `start`/`end` are COLUMN offsets (each cluster occupies its display width,
// each newline one column); `bg`/`fg` carry the optional selection colors
// (nullopt falls back to the buffer defaults / reverse-video swap at draw time).
// text_view.rs:41-46 Selection.
struct Selection {
  uint32_t start = 0;
  uint32_t end = 0;
  std::optional<std::array<uint16_t, 4>> bg;
  std::optional<std::array<uint16_t, 4>> fg;
};

// Forward declaration: the virtual-line record (defined below with the S6
// measurement kernel). The local-selection resolvers take the view's already-
// materialized virtual lines by reference, so a forward declaration suffices
// here — the .cc sees the complete type.
struct VLine;

// A native text-buffer view: the active/original buffer handles plus the wrap /
// viewport / truncation / tab-indicator configuration and the active selection.
// Mirrors text_view.rs:611-629 TextBufferView.
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

  // The active selection (nullopt when cleared). text_view.rs:625.
  std::optional<Selection> selection;
  // The stored anchor column offset that updateLocalSelection extends the focus
  // against (mirrors text-buffer-view.zig::selection_anchor_offset).
  // text_view.rs:627-628.
  std::optional<uint32_t> selection_anchor_offset;

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

  // ── Selection state (faithful to text_view.rs:1110-1195) ──

  // setSelection: store an explicit [start, end) column-offset selection.
  // text_view.rs:1110-1121.
  void SetSelection(uint32_t start, uint32_t end,
                    std::optional<std::array<uint16_t, 4>> bg,
                    std::optional<std::array<uint16_t, 4>> fg) {
    selection = Selection{start, end, bg, fg};
  }

  // updateSelection: replace the selection end (and colors) keeping start; a
  // no-op when there is no active selection. text_view.rs:1124-1136.
  void UpdateSelection(uint32_t end, std::optional<std::array<uint16_t, 4>> bg,
                       std::optional<std::array<uint16_t, 4>> fg) {
    if (selection.has_value()) {
      selection = Selection{selection->start, end, bg, fg};
    }
  }

  // resetSelection: clear the selection, keeping the stored anchor offset.
  // text_view.rs:1139-1141.
  void ResetSelection() {
    selection = std::nullopt;
  }

  // packSelectionInfo: kNoSelection for none or zero-width, else
  // (start << 32) | end. text_view.rs:1145-1150.
  uint64_t GetSelection() const {
    if (selection.has_value() && selection->start != selection->end) {
      return (static_cast<uint64_t>(selection->start) << 32) |
             static_cast<uint64_t>(selection->end);
    }
    return kNoSelection;
  }

  // resetLocalSelection: clear the selection AND the stored anchor offset.
  // text_view.rs:1185-1190.
  void ResetLocalSelection() {
    selection = std::nullopt;
    selection_anchor_offset = std::nullopt;
  }

  // ── Local (screen-coordinate) selection resolvers (text_view.rs:687-834) ──
  // Each takes the view's already-materialized virtual lines (the caller runs
  // virtual_lines() once) since a selection change never alters wrapping.

  // getTextEndOffset: the end column of the last virtual line, ellipsis-aware
  // for a truncated last line. text_view.rs:674-686.
  uint32_t TextEndOffset(const std::vector<VLine>& vlines) const;

  // coordsToCharOffset: resolve a local (x, y) to a column offset in the
  // newline-inclusive space (viewport scroll + clamp + ellipsis mapping).
  // text_view.rs:692-728.
  uint32_t CoordsToCharOffset(const std::vector<VLine>& vlines, int32_t x,
                              int32_t y) const;

  // setLocalSelectionStyle: resolve anchor+focus screen coords into a selection,
  // returning whether the stored range changed. text_view.rs:729-784.
  bool SetLocalSelection(const std::vector<VLine>& vlines, int32_t ax,
                         int32_t ay, int32_t fx, int32_t fy,
                         std::optional<std::array<uint16_t, 4>> bg,
                         std::optional<std::array<uint16_t, 4>> fg);

  // updateLocalSelectionStyle: focus-only extension when an anchor offset is
  // stored, else delegates to SetLocalSelection. text_view.rs:786-801.
  bool UpdateLocalSelection(const std::vector<VLine>& vlines, int32_t ax,
                            int32_t ay, int32_t fx, int32_t fy,
                            std::optional<std::array<uint16_t, 4>> bg,
                            std::optional<std::array<uint16_t, 4>> fg);

  // updateLocalSelectionFocusOnly: extend the focus against the stored anchor.
  // text_view.rs:803-834.
  bool UpdateLocalSelectionFocusOnly(const std::vector<VLine>& vlines,
                                     int32_t fx, int32_t fy,
                                     std::optional<std::array<uint16_t, 4>> bg,
                                     std::optional<std::array<uint16_t, 4>> fg);
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

// ─────────────────────────────────────────────────────────────────────────
// Virtual-line layout / measurement kernel (slice S6)
//
// Port of the wrapping kernel stuie text_view.rs drives:
// calculate_virtual_lines (VLine / VirtualLines / WrapCursor), measure_lines,
// and serialize_line_info, plus the four public entry points
// (tbv_virtual_line_count / tbv_measure / tbv_get_line_info /
// tbv_get_logical_line_info). Cell widths use stuie's own char_width/is_wide
// ranges (buffer.rs:42-70) rather than the Unicode-17 width tables the rest of
// node-smol uses, so the wrap results are byte-identical to stuie's fixtures.
// ─────────────────────────────────────────────────────────────────────────

// A single virtual (visual) line — the wrapping-aware unit
// calculateVirtualLinesGeneric materializes. text_view.rs:53-68 VLine.
struct VLine {
  uint32_t col_offset = 0;           // global newline-inclusive column offset
  uint32_t width_cols = 0;           // display width of this virtual line
  uint32_t source_line = 0;          // logical line this vline slices
  uint32_t source_col_offset = 0;    // start column within the logical line
  uint32_t wrap_index = 0;           // wrap ordinal within the logical line
  bool is_truncated = false;         // apply_truncation state (draw path)
  uint32_t ellipsis_pos = 0;         // column where the "..." begins
  uint32_t truncation_suffix_start = 0;  // source column the suffix resumes at
};

// The materialized virtual-line model: the per-vline records plus the
// per-logical-line (first_vline, vline_count) index. text_view.rs:94-98
// VirtualLines.
struct VirtualLines {
  std::vector<VLine> vlines;
  std::vector<uint32_t> first_vline;
  std::vector<uint32_t> vline_counts;
};

// The buffer inputs the kernel reads for a given buffer handle, mirroring
// stuie's text_buffer::content_lines / line_widths / tab_width_cols. The
// binding fills this from tui::TextBuffer; a stale/missing buffer yields the
// documented per-helper fallbacks (content_lines empty, line_widths == {0},
// tab_width_cols == kDefaultTabWidth).
struct BufferData {
  std::vector<std::string> content_lines;  // text_buffer.rs:388 content_lines
  std::vector<uint32_t> line_widths;       // text_buffer.rs:480 line_widths
  uint32_t tab_width_cols = 0;             // text_buffer.rs:368 tab_width_cols
};

// Resolve a buffer handle to its BufferData. The binding backs this with its
// TextBuffer registry; the pure-C++ harness backs it with a fixture map. Called
// with the view's active_tb, exactly where stuie calls text_buffer::* helpers.
using BufferResolver = std::function<BufferData(uint32_t tb)>;

// Per-logical-line display-column widths for `content_lines` under `tab_width`,
// using the same cell-width model as the wrap kernel (tabs expand, clusters
// fold). Faithful to text_buffer.rs:480/899 line_widths / display_length_tabs.
std::vector<uint32_t> LineWidths(const std::vector<std::string>& content_lines,
                                 uint32_t tab_width);

// Port of text_view.rs:228-348 calculate_virtual_lines. Produces the per-vline
// model for wrap modes none(0)/char(1)/word(2). `wrap_width` is nullopt (or a
// 0/none-mode value) to disable wrapping (one vline per logical line).
VirtualLines CalculateVirtualLines(
    const std::vector<std::string>& content_lines, uint32_t tab_width,
    uint8_t wrap_mode, std::optional<uint32_t> wrap_width,
    uint32_t first_line_offset);

// ── Public handle entry points (registry lookup + kernel; stale-safe) ──

// tbv_virtual_line_count (text_view.rs:1046-1048): number of virtual lines for
// the view's wrap config. A stale/wrong-kind view handle returns 0.
uint32_t TbvVirtualLineCount(uint32_t handle, const BufferResolver& resolve);

// tbv_measure (text_view.rs:1052-1066): pack (line_count << 32) | width_cols_max
// for the given measure dimensions. A stale view returns (1 << 32) | 0.
uint64_t TbvMeasure(uint32_t handle, uint32_t width, uint32_t height,
                    const BufferResolver& resolve);

// tbv_get_line_info (text_view.rs:1073-1088): serialize the VIEWPORT-SLICED
// virtual lines into the u32 out-buffer [count, widthColsMax, startCols[],
// widthCols[], sources[], wraps[]] (widthColsMax = max over the slice). Returns
// the entry count written. A null `out` or stale view writes nothing.
uint32_t TbvGetLineInfo(uint32_t handle, uint32_t* out, uint32_t max_entries,
                        const BufferResolver& resolve);

// tbv_get_logical_line_info (text_view.rs:1095-1104): same wire layout but the
// FULL (un-sliced) virtual-line arrays, with widthColsMax = the buffer's max
// LOGICAL line width.
uint32_t TbvGetLogicalLineInfo(uint32_t handle, uint32_t* out,
                               uint32_t max_entries,
                               const BufferResolver& resolve);

// ─────────────────────────────────────────────────────────────────────────
// Selection + text extraction + draw (slice S7)
//
// Port of stuie text_view.rs's TextBufferView selection surface (tbv_set/update/
// reset/get_selection + the local screen-coord variants), the width-aware text
// extraction (tbv_get_selected_text_bytes / tbv_get_plain_text_bytes), and the
// draw path (tbv_draw_model + draw_model_into). The draw/extraction paths reach
// the backing TextBuffer through a TextBufferAccessor, the C++ seam for stuie's
// cross-module text_buffer::* calls (content_lines / build_line_spans /
// default_* / get_text_range_by_cols / get_plain_text). A stale/missing buffer
// resolves to nullptr, yielding the same per-helper fallbacks as stuie.
// ─────────────────────────────────────────────────────────────────────────

// Resolve a buffer handle to its live TextBuffer (const — every draw/extraction
// read is const), or nullptr for a stale/missing/wrong-kind handle. The binding
// backs this with its TextBuffer registry; the pure-C++ harness backs it with a
// fixture buffer.
using TextBufferAccessor = std::function<const TextBuffer*(uint32_t tb)>;

// ── Handle-level selection entry points (registry lookup; stale-safe) ──

// tbv_set_selection (text_view.rs:1110): store an explicit [start, end).
void TbvSetSelection(uint32_t handle, uint32_t start, uint32_t end,
                     std::optional<std::array<uint16_t, 4>> bg,
                     std::optional<std::array<uint16_t, 4>> fg);

// tbv_update_selection (text_view.rs:1124): replace end/colors, keep start.
void TbvUpdateSelection(uint32_t handle, uint32_t end,
                        std::optional<std::array<uint16_t, 4>> bg,
                        std::optional<std::array<uint16_t, 4>> fg);

// tbv_reset_selection (text_view.rs:1139): clear the selection.
void TbvResetSelection(uint32_t handle);

// tbv_get_selection (text_view.rs:1145): packed selection info; a stale view
// returns kNoSelection.
uint64_t TbvGetSelection(uint32_t handle);

// tbv_set_local_selection (text_view.rs:1154): resolve screen coords to a
// selection, returning whether the stored range changed. A stale view -> false.
bool TbvSetLocalSelection(uint32_t handle, int32_t ax, int32_t ay, int32_t fx,
                          int32_t fy, std::optional<std::array<uint16_t, 4>> bg,
                          std::optional<std::array<uint16_t, 4>> fg,
                          const BufferResolver& resolve);

// tbv_update_local_selection (text_view.rs:1170): focus-only extension against
// the stored anchor, or a fresh set_local_selection when no anchor is stored.
// A stale view -> false.
bool TbvUpdateLocalSelection(uint32_t handle, int32_t ax, int32_t ay,
                             int32_t fx, int32_t fy,
                             std::optional<std::array<uint16_t, 4>> bg,
                             std::optional<std::array<uint16_t, 4>> fg,
                             const BufferResolver& resolve);

// tbv_reset_local_selection (text_view.rs:1185): clear selection AND anchor.
void TbvResetLocalSelection(uint32_t handle);

// ── Text extraction (stale-safe) ──

// tbv_get_selected_text_bytes (text_view.rs:1198): copy the selected text into
// `out` (0 for no selection or a zero-width one). Column offsets are converted
// to bytes width-aware so graphemes select whole. `accessor` resolves the
// active buffer. A stale view or missing buffer -> 0.
uint32_t TbvGetSelectedTextBytes(uint32_t handle, uint8_t* out, uint32_t max_len,
                                 const TextBufferAccessor& accessor);

// tbv_get_plain_text_bytes (text_view.rs:1219): copy the active buffer's raw
// content into `out`; returns bytes written (0 for empty / max_len == 0 / stale
// view / missing buffer).
uint32_t TbvGetPlainTextBytes(uint32_t handle, uint8_t* out, uint32_t max_len,
                              const TextBufferAccessor& accessor);

// ── Draw model + draw ──

// A view's draw params: the active buffer's logical lines + style spans + tab
// width + resolved default attributes, the full virtual-line model, the
// resolved viewport, the wrap-none flag, the selection, and the tab indicator.
// Mirrors text_view.rs:923-947 DrawModel (content_lines is carried here rather
// than re-fetched inside draw_model_into, since the C++ side has no global
// buffer registry to re-fetch from). prefill_bg is EditorView-only, so it is
// always nullopt for a plain TextBufferView draw.
struct DrawModel {
  // The active buffer handle — the draw caller resolves its default colors from
  // this (stuie's draw_view_model calls text_buffer::default_colors(model.tb)).
  uint32_t tb = 0;
  std::vector<std::string> content_lines;
  std::vector<VLine> vlines;
  uint32_t offset_x = 0;
  uint32_t offset_y = 0;
  std::optional<uint32_t> width;
  std::optional<uint32_t> height;
  bool wrap_none = true;
  std::optional<Selection> selection;
  std::vector<std::vector<SpanRun>> spans_by_line;
  uint32_t default_attrs = 0;
  uint32_t tab_width = 0;
  std::optional<uint32_t> tab_indicator;
  std::optional<std::array<uint16_t, 4>> tab_indicator_color;
  std::optional<std::array<uint16_t, 4>> prefill_bg;
};

// The per-cluster draw sink draw_model_into invokes: (text, cx, cy, fg, bg,
// attrs). fg/bg are raw [u16;4] RGBA lanes (value in the low byte); cx/cy may be
// negative when the view is scrolled off the top/left. Mirrors the FnMut in
// text_view.rs:2236-2238.
using DrawCallback =
    std::function<void(std::string_view, int32_t, int32_t,
                       const std::array<uint16_t, 4>&,
                       const std::array<uint16_t, 4>&, uint32_t)>;

// tbv_draw_model (text_view.rs:949-980): materialize the draw model for the
// view, pulling content/spans/defaults/tab-width from the buffer via `accessor`.
// nullopt only for a stale/wrong-kind VIEW handle; a stale BUFFER resolves to
// the documented empty fallbacks.
std::optional<DrawModel> TbvDrawModel(uint32_t handle,
                                      const TextBufferAccessor& accessor);

// draw_model_into (text_view.rs:2236-2354): draw the model's visible virtual
// lines at (x, y), compositing style spans over the defaults, then the
// selection override, then the reverse-video swap, then tab-indicator fill;
// truncated lines render prefix + "..." + suffix. `def_fg`/`def_bg` are the
// buffer's resolved default colors.
void DrawModelInto(const DrawModel& model, const DrawCallback& draw,
                   std::array<uint16_t, 4> def_fg,
                   std::array<uint16_t, 4> def_bg, int32_t x, int32_t y);

}  // namespace tui

#endif  // TUI_INFRA_TEXT_VIEW_HPP_

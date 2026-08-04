// Native TextBuffer store.
//
// Port of stuie's crates/stuie-cabi/src/text_buffer.rs, the native
// `TextBuffer` OpenTUI's text-buffer.ts drives through the FFI. A text
// buffer holds RAW bytes (malformed UTF-8 is preserved verbatim so a load
// of an out-of-range scalar round-trips its bytes), a default
// fg/bg/attributes triple, a tab width, an owning syntax-style handle, an
// in-memory blob registry, and a flat highlight list (text_buffer.rs:1-15).
//
// This is the FOUNDATION slice: lifecycle + the core content metrics
// (length / byte size / line count) plus clear/reset. Content ingestion
// (S3), presentation defaults + tab width (S2), and highlights / styled
// text (S4) grow the struct in later slices — see the field notes below.
//
// Handles are u32 registry ids (not pointers), so a stale handle is always
// a safe no-op; that lifecycle is owned by the binding-side registry in
// tui_binding.cc, not by this type (text_buffer.rs:14-15).

#ifndef TUI_INFRA_TEXT_BUFFER_HPP_
#define TUI_INFRA_TEXT_BUFFER_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tui {

// Default tab width, matching OpenTUI's native TextBuffer
// (text_buffer.rs:22-23 DEFAULT_TAB_WIDTH).
inline constexpr uint8_t kDefaultTabWidth = 2;

// A packed highlight record — the byte-EXACT 16-byte layout the zig.mts
// `HighlightStruct` pack/unpack agrees on and the FFI reads/writes verbatim
// (text_buffer.rs:27-52 HighlightRepr `#[repr(C)]`). Field order/sizes must
// stay lockstep: start@0, end@4, styleId@8, priority@12(u8), pad@13, hlRef@14.
struct HighlightRepr {
  uint32_t start;
  uint32_t end;
  uint32_t style_id;
  uint8_t priority;
  uint8_t pad;
  uint16_t hl_ref;
};
static_assert(sizeof(HighlightRepr) == 16,
              "HighlightRepr must be exactly 16 bytes (zig HighlightStruct)");
static_assert(alignof(HighlightRepr) == 4, "HighlightRepr aligns to 4");
static_assert(offsetof(HighlightRepr, start) == 0, "start @ 0");
static_assert(offsetof(HighlightRepr, end) == 4, "end @ 4");
static_assert(offsetof(HighlightRepr, style_id) == 8, "styleId @ 8");
static_assert(offsetof(HighlightRepr, priority) == 12, "priority @ 12");
static_assert(offsetof(HighlightRepr, pad) == 13, "pad @ 13");
static_assert(offsetof(HighlightRepr, hl_ref) == 14, "hlRef @ 14");

// Byte size of one packed StyledChunkStruct record the shim builds
// (text_buffer.rs:942 STYLED_RECORD_SIZE): {text:ptr@0, text_len:u64@8,
// fg:ptr@16, bg:ptr@24, attributes:u32@32, link:ptr@40, link_len:u64@48}.
inline constexpr size_t kStyledRecordSize = 56;

// Whether a highlight came from a styled chunk (dropped on clear/setText) or was
// added by the caller (kept across clear/setText). text_buffer.rs:56-60 HlKind.
enum class HlKind { kStyled, kUser };

// A resolved style: (fg, bg, attributes), each color nullopt when unset.
// text_buffer.rs:772 StyleTriple (as consumed by resolve_hl_style).
struct StyleTriple {
  std::optional<std::array<uint16_t, 4>> fg;
  std::optional<std::array<uint16_t, 4>> bg;
  uint32_t attrs = 0;
};

// A resolved style span over a logical line's column range [col, next_col).
// fg/bg nullopt means "keep the default"; attrs OR-combines onto the default.
// Mirrors text_buffer.rs:81-88 SpanRun (the draw path S7 consumes these).
struct SpanRun {
  uint32_t col;
  uint32_t next_col;
  std::optional<std::array<uint16_t, 4>> fg;
  std::optional<std::array<uint16_t, 4>> bg;
  uint32_t attrs;
};

// One stored highlight: its logical line, the packed record, its kind, and (for
// Styled highlights produced by SetStyled) the chunk's inline colors/attributes.
// User highlights resolve their style through the buffer's syntax-style table
// via hl.style_id instead. text_buffer.rs:62-75 HlEntry.
struct HlEntry {
  uint32_t line;
  HighlightRepr hl;
  HlKind kind;
  std::optional<std::array<uint16_t, 4>> fg;
  std::optional<std::array<uint16_t, 4>> bg;
  uint32_t attrs;
};

// ── Minimal internal SyntaxStyle name->id registry (syntax_style.rs) ──
//
// A process-global table keyed by u32 handle: register assigns a monotonic
// 1-based id (0 is the "not found" sentinel), re-registering a name updates it
// in place and returns the existing id, resolve_by_id maps a 1-based id to its
// (fg, bg, attributes). SetStyled leans on this to stamp a non-zero synthetic
// id per colored chunk, and BuildLineSpans resolves User highlights through it.
//
// This is the internal stub the S4 slice needs; the 5 syntaxStyle* cabi symbols
// (create/destroy/register/resolveByName/getStyleCount) stay pending and are
// NOT wired to V8 — a later slice ports those wrappers on top of this store.
uint32_t SyntaxStyleCreate();
void SyntaxStyleDestroy(uint32_t handle);
uint32_t SyntaxStyleRegister(uint32_t handle,
                             const std::string& name,
                             std::optional<std::array<uint16_t, 4>> fg,
                             std::optional<std::array<uint16_t, 4>> bg,
                             uint32_t attrs);
std::optional<StyleTriple> SyntaxStyleResolveById(uint32_t handle, uint32_t id);
uint32_t SyntaxStyleGetStyleCount(uint32_t handle);

// Failure sentinel for RegisterMem (the shim throws on it). Also the ceiling:
// a registry that already holds this many slots refuses to grow.
// text_buffer.rs:24-25 MEM_REGISTER_FAILED.
inline constexpr uint16_t kMemRegisterFailed = 0xffff;

// Normalize CRLF (`\r\n`) to LF (`\n`) at the byte level, preserving malformed
// UTF-8 verbatim (a lone `\r` or `\n` is kept). Faithful port of
// text_buffer.rs:833-846 normalize_crlf; exposed as the testable kernel behind
// SetContent / Append (both normalize before storing).
std::vector<uint8_t> NormalizeCrlf(const uint8_t* bytes, size_t len);

// Display-column count of `[bytes, bytes + len)`: the sum of CodepointWidth
// over the well-formed UTF-8 scalars, newlines excluded (CodepointWidth of a
// control char is 0). Bytes that are not part of a well-formed scalar
// contribute 0 and advance one byte, so a malformed load reports only the
// width of its decodable scalars. Faithful port of text_buffer.rs:850
// display_length (which walks Rust's str::from_utf8 valid-prefix boundaries;
// here the equivalent well-formed-scalar boundaries are decoded directly).
// This is the testable kernel behind TextBuffer::Length.
uint32_t DisplayLength(const uint8_t* bytes, size_t len);

// Newline-delimited logical line count: (count of '\n') + 1, so an empty
// span is one line. Testable kernel behind TextBuffer::LineCount
// (text_buffer.rs:129-132 line_count).
uint32_t CountLines(const uint8_t* bytes, size_t len);

class TextBuffer {
 public:
  TextBuffer() = default;

  // Drop content (and, from S4, the styled-chunk highlights while keeping
  // user highlights). text_buffer.rs:145-148 clear().
  void Clear();

  // Drop content, highlights, and the mem registry. The highlight/mem clears
  // land with S4/S3; for now only content exists. text_buffer.rs:150-154
  // reset().
  void Reset();

  // Display-column count (newlines excluded). text_buffer.rs:125-127 length()
  // -> text_buffer.rs:850 display_length.
  uint32_t Length() const;

  // Raw byte length of the stored content. text_buffer.rs:347-349
  // get_byte_size() -> content.len().
  uint32_t ByteSize() const {
    return static_cast<uint32_t>(content_.size());
  }

  // Newline-delimited logical line count. text_buffer.rs:129-132 line_count().
  uint32_t LineCount() const;

  // ── Presentation defaults + tab width (S2) ──

  // Presentation tab width (display columns a '\t' expands to). A stale handle
  // in the binding falls back to kDefaultTabWidth, matching the registry `with`
  // default, NOT 0. text_buffer.rs:363-365 get_tab_width().
  uint8_t TabWidth() const {
    return tab_width_;
  }

  // Set the presentation tab width. text_buffer.rs:383-385 set_tab_width().
  void SetTabWidth(uint8_t width) {
    tab_width_ = width;
  }

  // Set the default foreground as raw [u16;4] RGBA lanes, or clear it (nullopt).
  // Faithful Option semantics: Some(rgba) sets, None clears.
  // text_buffer.rs:425-427 set_default_fg().
  void SetDefaultFg(std::optional<std::array<uint16_t, 4>> rgba) {
    default_fg_ = rgba;
  }

  // Set the default background, or clear it. text_buffer.rs:429-431
  // set_default_bg().
  void SetDefaultBg(std::optional<std::array<uint16_t, 4>> rgba) {
    default_bg_ = rgba;
  }

  // Set the default attributes bitset, or clear it (null clears).
  // text_buffer.rs:433-435 set_default_attributes().
  void SetDefaultAttributes(std::optional<uint32_t> attrs) {
    default_attrs_ = attrs;
  }

  // Clear all three presentation defaults back to nullopt.
  // text_buffer.rs:437-443 reset_defaults().
  void ResetDefaults() {
    default_fg_ = std::nullopt;
    default_bg_ = std::nullopt;
    default_attrs_ = std::nullopt;
  }

  // Read accessors for the presentation defaults. The S6 draw path resolves
  // these (unset fg -> opaque white, unset bg -> transparent, unset attrs -> 0);
  // exposed now so the Option semantics are checkable. text_buffer.rs:94-96.
  const std::optional<std::array<uint16_t, 4>>& DefaultFg() const {
    return default_fg_;
  }
  const std::optional<std::array<uint16_t, 4>>& DefaultBg() const {
    return default_bg_;
  }
  const std::optional<uint32_t>& DefaultAttributes() const {
    return default_attrs_;
  }

  // ── Content I/O: mem registry + ingestion + readout (S3) ──

  // Register a byte blob in the per-buffer mem registry, recycling the first
  // freed slot when one exists, otherwise appending. Returns the slot id, never
  // kMemRegisterFailed unless the registry is already full (>= 0xffff slots).
  // text_buffer.rs:156-170 register_mem.
  uint16_t RegisterMem(const uint8_t* bytes, size_t len);

  // Replace the blob in an OCCUPIED slot. A missing slot id or a freed (empty)
  // slot returns false without mutating anything. text_buffer.rs:172-180
  // replace_mem.
  bool ReplaceMem(uint16_t id, const uint8_t* bytes, size_t len);

  // Drop every mem slot (frees the registry, so prior ids no longer resolve).
  // text_buffer.rs:453-455 clear_mem_registry.
  void ClearMemRegistry() {
    mem_.clear();
  }

  // Replace content from raw bytes, CRLF-normalized on the way in (styled
  // highlights would be dropped here from S4; user highlights kept). Used by
  // SetFromMem and LoadFile. text_buffer.rs:139-143 set_content ->
  // text_buffer.rs:833 normalize_crlf.
  void SetContent(const uint8_t* bytes, size_t len);

  // Re-materialize content from a registered mem slot (no-op for a missing or
  // freed slot). text_buffer.rs:182-187 set_from_mem.
  void SetFromMem(uint16_t id);

  // Append raw bytes (CRLF-normalized) to the existing content.
  // text_buffer.rs:189-192 append.
  void Append(const uint8_t* bytes, size_t len);

  // Append the content of a registered mem slot (no-op for a missing or freed
  // slot). text_buffer.rs:194-199 append_from_mem.
  void AppendFromMem(uint16_t id);

  // Read `[path, path + path_len)` as a UTF-8 filesystem path, load its bytes as
  // content (CRLF-normalized, malformed UTF-8 preserved verbatim). Returns false
  // when the file cannot be read. text_buffer.rs:498-506 load_file.
  bool LoadFile(const char* path, size_t path_len);

  // Copy the raw content bytes (clamped to `max_len`) into `out`; returns bytes
  // written. A null `out` or zero `max_len` writes nothing and returns 0.
  // text_buffer.rs:520-522 get_plain_text -> text_buffer.rs:817 copy_slice_out.
  uint32_t GetPlainText(uint8_t* out, uint32_t max_len) const;

  // Slice content by CHARACTER (Unicode scalar) offsets `[start, end)` — the
  // content is decoded lossily (malformed bytes -> U+FFFD, matching Rust's
  // from_utf8_lossy) — and copy the sliced UTF-8 bytes into `out`; returns bytes
  // written. An empty range (start >= end) returns 0. text_buffer.rs:525-534
  // get_text_range -> text_buffer.rs:916 char_slice.
  uint32_t GetTextRange(uint32_t start,
                        uint32_t end,
                        uint8_t* out,
                        uint32_t max_len) const;

  // Slice content by (row, col) coordinates, where `col` is a scalar count into
  // the row, and copy the sliced UTF-8 bytes into `out`; returns bytes written.
  // An empty range returns 0. text_buffer.rs:577-596 get_text_range_by_coords ->
  // text_buffer.rs:924 coord_to_char_offset + char_slice.
  uint32_t GetTextRangeByCoords(uint32_t start_row,
                                uint32_t start_col,
                                uint32_t end_row,
                                uint32_t end_col,
                                uint8_t* out,
                                uint32_t max_len) const;

  // ── Highlights + styled text + syntax style (S4) ──

  // Add a User highlight on `line`. An empty range (start >= end) is dropped
  // before storing, mirroring zig addHighlightInternal. text_buffer.rs:598-614
  // add_highlight.
  void AddHighlight(uint32_t line, const HighlightRepr& hl);

  // Split a display-column range [start, end) across every logical line it
  // overlaps, emitting one clamped User highlight per overlapping line (zero-
  // width clamps dropped). text_buffer.rs:623-664 add_highlight_by_char_range.
  void AddHighlightByCharRange(const HighlightRepr& hl);

  // Drop every highlight whose hlRef equals `hl_ref` (User or Styled).
  // text_buffer.rs:666-670 remove_highlights_by_ref.
  void RemoveHighlightsByRef(uint16_t hl_ref);

  // Drop every highlight on `line`. text_buffer.rs:672-676 clear_line_highlights.
  void ClearLineHighlights(uint32_t line);

  // Drop every highlight (User and Styled). text_buffer.rs:678-680
  // clear_all_highlights.
  void ClearAllHighlights();

  // Number of stored highlights. text_buffer.rs:682-684 get_highlight_count.
  uint32_t HighlightCount() const {
    return static_cast<uint32_t>(highlights_.size());
  }

  // Set the owning syntax-style handle (0 detaches). Always returns true for a
  // live buffer (the stale-handle false lives in the binding). text_buffer.rs
  // :512-517 set_syntax_style.
  bool SetSyntaxStyle(uint32_t style) {
    syntax_style_ = style;
    return true;
  }

  uint32_t SyntaxStyle() const {
    return syntax_style_;
  }

  // ── Borrowed-buffer flag (S5) ──

  // Mark this buffer as a BORROWED inner buffer (owned by an EditBuffer). A
  // borrowed buffer may not own external TextBufferViews. text_buffer.rs:315
  // mark_borrowed. (No EditBuffer path exists yet, so nothing marks a buffer
  // borrowed in production; the flag + accessor land now so S5's
  // createTextBufferView gate reads a real value and the view harness can
  // exercise the rejection.)
  void MarkBorrowed() {
    borrowed_ = true;
  }

  // Whether this is a borrowed inner buffer. createTextBufferView rejects a
  // view over a borrowed buffer (returns the 0 sentinel). text_buffer.rs:323
  // is_borrowed (the binding maps a stale/missing handle to false before it
  // ever reaches this accessor).
  bool IsBorrowed() const {
    return borrowed_;
  }

  // Materialize styled chunks: clear (dropping styled highlights + content),
  // concatenate each chunk's normalized text into content, and add one Styled
  // highlight per colored line-segment carrying the chunk's inline colors and a
  // synthetic per-chunk styleId (registered into the buffer's syntax-style table
  // when one is attached, else 0). `records` is `count` packed
  // kStyledRecordSize-byte StyledChunkStructs; text + colors live behind
  // BORROWED process pointers valid for this synchronous call (copied here).
  // text_buffer.rs:204-261 set_styled + parse_styled_chunks.
  void SetStyled(const uint8_t* records, size_t len, uint32_t count);

  // Collect a line's highlights into a heap-allocated array, returning
  // (ptr, count); (nullptr, 0) when the line has none. The caller MUST hand the
  // pair back to FreeLineHighlights. text_buffer.rs:784-800 get_line_highlights.
  std::pair<HighlightRepr*, uint32_t> GetLineHighlights(uint32_t line) const;

  // Free an array produced by GetLineHighlights. text_buffer.rs:806-812
  // free_line_highlights.
  static void FreeLineHighlights(HighlightRepr* ptr, uint32_t count);

  // Build the per-logical-line style spans the draw path composites: for each
  // line the highlights are swept into non-overlapping [col, next_col) runs, the
  // highest-priority active highlight wins each run, and is resolved to colors
  // (Styled inline, User via the syntax-style table). Runs with no active
  // highlight are omitted. Indexed by logical line. text_buffer.rs:694-768
  // build_line_spans.
  std::vector<std::vector<SpanRun>> BuildLineSpans() const;

 private:
  // Raw content bytes (malformed UTF-8 preserved verbatim). text_buffer.rs:92.
  std::vector<uint8_t> content_;

  // Presentation tab width. Read/written by S2's get/setTabWidth (above) and
  // consumed by the S6 measurement path. text_buffer.rs:93.
  uint8_t tab_width_ = kDefaultTabWidth;

  // Presentation defaults (nullopt = unset). The S6 draw path resolves an unset
  // fg to opaque white and an unset bg to transparent; here they hold the raw
  // color/attribute intent. text_buffer.rs:94-96.
  std::optional<std::array<uint16_t, 4>> default_fg_;
  std::optional<std::array<uint16_t, 4>> default_bg_;
  std::optional<uint32_t> default_attrs_;

  // Per-buffer in-memory blob registry. A slot is nullopt when freed, so
  // RegisterMem recycles the first free slot before growing (thousands of
  // setText calls don't leak). text_buffer.rs:98 mem.
  std::vector<std::optional<std::vector<uint8_t>>> mem_;

  // True when this buffer is a BORROWED inner buffer owned by an EditBuffer.
  // A borrowed buffer cannot own external TextBufferViews — S5's
  // createTextBufferView rejects it (read via IsBorrowed()). text_buffer.rs:104.
  bool borrowed_ = false;

  // Owning syntax-style handle (0 = none). Consumed by SetStyled (synthetic
  // chunk-id registration) and BuildLineSpans (User-highlight resolution).
  // text_buffer.rs:97 syntax_style.
  uint32_t syntax_style_ = 0;

  // Flat highlight list, tagged Styled (from SetStyled) or User (from
  // AddHighlight). Clear/SetContent drop the Styled ones and keep the User ones;
  // Reset drops both. text_buffer.rs:99 highlights.
  std::vector<HlEntry> highlights_;

  // Drop styled-chunk highlights, keep user highlights (text_buffer.rs:134-137
  // drop_styled_highlights).
  void DropStyledHighlights();
};

}  // namespace tui

#endif  // TUI_INFRA_TEXT_BUFFER_HPP_

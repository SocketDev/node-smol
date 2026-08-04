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
#include <vector>

namespace tui {

// Default tab width, matching OpenTUI's native TextBuffer
// (text_buffer.rs:22-23 DEFAULT_TAB_WIDTH).
inline constexpr uint8_t kDefaultTabWidth = 2;

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

  // True when this buffer is a BORROWED inner buffer owned by an EditBuffer.
  // A borrowed buffer cannot own external TextBufferViews — S5's
  // createTextBufferView rejects it. Inert in the foundation, hence
  // [[maybe_unused]]. text_buffer.rs:104.
  [[maybe_unused]] bool borrowed_ = false;

  // S3 adds mem; S4 adds highlights/syntax_style.
};

}  // namespace tui

#endif  // TUI_INFRA_TEXT_BUFFER_HPP_

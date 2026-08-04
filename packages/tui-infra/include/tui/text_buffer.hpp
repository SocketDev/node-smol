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

#include <cstddef>
#include <cstdint>
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

 private:
  // Raw content bytes (malformed UTF-8 preserved verbatim). text_buffer.rs:92.
  std::vector<uint8_t> content_;

  // Presentation tab width. Consumed by S2 (get/setTabWidth) and the S6
  // measurement path; unused in the foundation, hence [[maybe_unused]].
  // text_buffer.rs:93.
  [[maybe_unused]] uint8_t tab_width_ = kDefaultTabWidth;

  // True when this buffer is a BORROWED inner buffer owned by an EditBuffer.
  // A borrowed buffer cannot own external TextBufferViews — S5's
  // createTextBufferView rejects it. Inert in the foundation, hence
  // [[maybe_unused]]. text_buffer.rs:104.
  [[maybe_unused]] bool borrowed_ = false;

  // S2 adds default_fg/bg/attrs; S3 adds mem; S4 adds highlights/syntax_style.
};

}  // namespace tui

#endif  // TUI_INFRA_TEXT_BUFFER_HPP_

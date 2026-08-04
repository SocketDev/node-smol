// Native TextBuffer store.
//
// Port of stuie's crates/stuie-cabi/src/text_buffer.rs. Foundation slice:
// lifecycle helpers (clear/reset) plus the content metrics (display length,
// byte size, line count). See tui/text_buffer.hpp for the field-growth plan.

#include "tui/text_buffer.hpp"

#include <cstddef>
#include <cstdint>

#include "tui/width.hpp"  // CodepointWidth

namespace tui {

namespace {

// True for a UTF-8 continuation byte (10xxxxxx).
inline bool IsContinuation(uint8_t b) {
  return (b & 0xc0u) == 0x80u;
}

// Decode one WELL-FORMED UTF-8 scalar at `[p, p + avail)` per Unicode Table
// 3-7 — the exact set Rust's str::from_utf8 accepts, which text_buffer.rs:850
// display_length walks. On a valid scalar, writes it to *cp and returns its
// byte length (1-4). On ANY ill-formed byte — bad lead, bad continuation,
// overlong encoding, UTF-16 surrogate (U+D800..U+DFFF), > U+10FFFF, or a
// truncated trailing sequence — returns 0 so the caller advances one byte
// contributing 0 width, mirroring display_length's invalid-byte skip.
//
// This is stricter than tui/utf8.hpp's DecodeUtf8 (which maps malformed input
// to U+FFFD and accepts overlong / out-of-range forms); display_length's
// malformed contract requires rejecting exactly what Rust rejects, so the
// decode is done here rather than reusing the lenient shared decoder.
size_t NextScalar(const uint8_t* p, size_t avail, uint32_t* cp) {
  const uint8_t b0 = p[0];
  // 1 byte: 00..7F.
  if (b0 < 0x80u) {
    *cp = b0;
    return 1;
  }
  // 2 bytes: C2..DF 80..BF (C0/C1 would be overlong).
  if (b0 >= 0xc2u && b0 <= 0xdfu) {
    if (avail >= 2 && IsContinuation(p[1])) {
      *cp = (static_cast<uint32_t>(b0 & 0x1fu) << 6) | (p[1] & 0x3fu);
      return 2;
    }
    return 0;
  }
  // 3 bytes: E0..EF, with the second byte range narrowed per lead to bar
  // overlong forms (E0 -> A0..BF) and surrogates (ED -> 80..9F).
  if (b0 >= 0xe0u && b0 <= 0xefu) {
    if (avail < 3) {
      return 0;
    }
    const uint8_t b1 = p[1];
    const uint8_t b2 = p[2];
    uint8_t lo = 0x80u;
    uint8_t hi = 0xbfu;
    if (b0 == 0xe0u) {
      lo = 0xa0u;
    } else if (b0 == 0xedu) {
      hi = 0x9fu;
    }
    if (b1 < lo || b1 > hi || !IsContinuation(b2)) {
      return 0;
    }
    *cp = (static_cast<uint32_t>(b0 & 0x0fu) << 12) |
          (static_cast<uint32_t>(b1 & 0x3fu) << 6) | (b2 & 0x3fu);
    return 3;
  }
  // 4 bytes: F0..F4, with the second byte range narrowed per lead to bar
  // overlong forms (F0 -> 90..BF) and > U+10FFFF (F4 -> 80..8F).
  if (b0 >= 0xf0u && b0 <= 0xf4u) {
    if (avail < 4) {
      return 0;
    }
    const uint8_t b1 = p[1];
    const uint8_t b2 = p[2];
    const uint8_t b3 = p[3];
    uint8_t lo = 0x80u;
    uint8_t hi = 0xbfu;
    if (b0 == 0xf0u) {
      lo = 0x90u;
    } else if (b0 == 0xf4u) {
      hi = 0x8fu;
    }
    if (b1 < lo || b1 > hi || !IsContinuation(b2) || !IsContinuation(b3)) {
      return 0;
    }
    *cp = (static_cast<uint32_t>(b0 & 0x07u) << 18) |
          (static_cast<uint32_t>(b1 & 0x3fu) << 12) |
          (static_cast<uint32_t>(b2 & 0x3fu) << 6) | (b3 & 0x3fu);
    return 4;
  }
  // C0, C1, F5..FF, and stray continuation bytes are never valid leads.
  return 0;
}

}  // namespace

uint32_t DisplayLength(const uint8_t* bytes, size_t len) {
  // text_buffer.rs:850 display_length: sum CodepointWidth over well-formed
  // scalars (newlines contribute 0 via CodepointWidth), invalid bytes
  // contribute 0. Column granularity is per-codepoint, matching the shipped
  // StringWidth; grapheme-cluster width (VS16/ZWJ) is a known width-stack
  // limitation carried by the whole node:smol-tui width surface.
  uint32_t width = 0;
  size_t i = 0;
  while (i < len) {
    uint32_t cp = 0;
    const size_t n = NextScalar(bytes + i, len - i, &cp);
    if (n == 0) {
      i += 1;  // invalid byte: 0 width, advance one.
      continue;
    }
    width += CodepointWidth(cp);
    i += n;
  }
  return width;
}

uint32_t CountLines(const uint8_t* bytes, size_t len) {
  // text_buffer.rs:129-132 line_count: (count of '\n') + 1.
  uint32_t newlines = 0;
  for (size_t i = 0; i < len; ++i) {
    if (bytes[i] == static_cast<uint8_t>('\n')) {
      ++newlines;
    }
  }
  return newlines + 1;
}

void TextBuffer::Clear() {
  // text_buffer.rs:145-148 clear(): drop content; drop_styled_highlights()
  // arrives with S4 (no styled highlights exist yet).
  content_.clear();
}

void TextBuffer::Reset() {
  // text_buffer.rs:150-154 reset(): drop content; highlights_.clear() and
  // mem_.clear() arrive with S4/S3.
  content_.clear();
}

uint32_t TextBuffer::Length() const {
  // text_buffer.rs:125-127 length() -> display_length(content).
  return DisplayLength(content_.data(), content_.size());
}

uint32_t TextBuffer::LineCount() const {
  // text_buffer.rs:129-132 line_count().
  return CountLines(content_.data(), content_.size());
}

}  // namespace tui

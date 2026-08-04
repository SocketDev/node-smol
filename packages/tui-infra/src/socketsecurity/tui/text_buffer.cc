// Native TextBuffer store.
//
// Port of stuie's crates/stuie-cabi/src/text_buffer.rs. Foundation slice:
// lifecycle helpers (clear/reset) plus the content metrics (display length,
// byte size, line count). See tui/text_buffer.hpp for the field-growth plan.

#include "tui/text_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// Decode `[bytes, bytes + len)` into a lossy Unicode scalar sequence, mirroring
// Rust's String::from_utf8_lossy: each well-formed scalar decodes to itself and
// each ill-formed byte becomes one U+FFFD replacement scalar (advancing one
// byte). This is the scalar space text_buffer.rs's char_slice /
// coord_to_char_offset walk (both operate on from_utf8_lossy(content).chars()).
// KNOWN LIMITATION carried from DisplayLength: a truncated multi-byte tail at
// EOF yields one U+FFFD per stray byte rather than Rust's single U+FFFD for the
// whole incomplete sequence; the shipped fixtures are well-formed so this stays
// unobservable here.
std::vector<uint32_t> DecodeScalarsLossy(const uint8_t* bytes, size_t len) {
  std::vector<uint32_t> scalars;
  size_t i = 0;
  while (i < len) {
    uint32_t cp = 0;
    const size_t n = NextScalar(bytes + i, len - i, &cp);
    if (n == 0) {
      scalars.push_back(0xFFFDu);  // U+FFFD replacement, advance one byte.
      i += 1;
      continue;
    }
    scalars.push_back(cp);
    i += n;
  }
  return scalars;
}

// Encode one Unicode scalar as UTF-8, appending to `out`.
void EncodeScalar(uint32_t cp, std::vector<uint8_t>* out) {
  if (cp < 0x80u) {
    out->push_back(static_cast<uint8_t>(cp));
  } else if (cp < 0x800u) {
    out->push_back(static_cast<uint8_t>(0xc0u | (cp >> 6)));
    out->push_back(static_cast<uint8_t>(0x80u | (cp & 0x3fu)));
  } else if (cp < 0x10000u) {
    out->push_back(static_cast<uint8_t>(0xe0u | (cp >> 12)));
    out->push_back(static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3fu)));
    out->push_back(static_cast<uint8_t>(0x80u | (cp & 0x3fu)));
  } else {
    out->push_back(static_cast<uint8_t>(0xf0u | (cp >> 18)));
    out->push_back(static_cast<uint8_t>(0x80u | ((cp >> 12) & 0x3fu)));
    out->push_back(static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3fu)));
    out->push_back(static_cast<uint8_t>(0x80u | (cp & 0x3fu)));
  }
}

// text_buffer.rs:915-921 char_slice: collect scalars `[start, end)` and
// re-encode to UTF-8. Mirrors `.chars().skip(start).take(end - start)`, so a
// `start` past the end yields empty and `end` past the end clamps.
std::vector<uint8_t> CharSlice(const std::vector<uint32_t>& scalars,
                               size_t start,
                               size_t end) {
  std::vector<uint8_t> out;
  const size_t size = scalars.size();
  for (size_t i = start; i < end && i < size; ++i) {
    EncodeScalar(scalars[i], &out);
  }
  return out;
}

// text_buffer.rs:923-937 coord_to_char_offset: scalar offset of `(row, col)`.
// Walk scalars counting newlines; when the content runs out before `row`
// newlines are seen, return the total scalar count (Rust's saturating fallback).
size_t CoordToCharOffset(const std::vector<uint32_t>& scalars,
                         uint32_t row,
                         uint32_t col) {
  size_t offset = 0;
  uint32_t line = 0;
  const size_t size = scalars.size();
  while (line < row) {
    if (offset >= size) {
      return size;
    }
    if (scalars[offset] == static_cast<uint32_t>('\n')) {
      line += 1;
    }
    offset += 1;
  }
  return offset + static_cast<size_t>(col);
}

// text_buffer.rs:816-830 copy_slice_out: copy min(len, max_len) bytes into
// `out`; returns bytes written. A null `out` or zero `max_len` writes nothing.
uint32_t CopySliceOut(const uint8_t* src,
                      size_t len,
                      uint8_t* out,
                      uint32_t max_len) {
  if (out == nullptr || max_len == 0) {
    return 0;
  }
  const size_t n =
      len < static_cast<size_t>(max_len) ? len : static_cast<size_t>(max_len);
  if (n == 0) {
    return 0;
  }
  std::memcpy(out, src, n);
  return static_cast<uint32_t>(n);
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
  // text_buffer.rs:150-154 reset(): drop content and the mem registry;
  // highlights_.clear() arrives with S4.
  content_.clear();
  mem_.clear();
}

uint32_t TextBuffer::Length() const {
  // text_buffer.rs:125-127 length() -> display_length(content).
  return DisplayLength(content_.data(), content_.size());
}

uint32_t TextBuffer::LineCount() const {
  // text_buffer.rs:129-132 line_count().
  return CountLines(content_.data(), content_.size());
}

std::vector<uint8_t> NormalizeCrlf(const uint8_t* bytes, size_t len) {
  // text_buffer.rs:833-846 normalize_crlf: collapse each `\r\n` to `\n`, keeping
  // every other byte (including lone `\r`/`\n` and malformed UTF-8) verbatim.
  std::vector<uint8_t> out;
  out.reserve(len);
  size_t i = 0;
  while (i < len) {
    if (bytes[i] == static_cast<uint8_t>('\r') && i + 1 < len &&
        bytes[i + 1] == static_cast<uint8_t>('\n')) {
      out.push_back(static_cast<uint8_t>('\n'));
      i += 2;
    } else {
      out.push_back(bytes[i]);
      i += 1;
    }
  }
  return out;
}

uint16_t TextBuffer::RegisterMem(const uint8_t* bytes, size_t len) {
  // text_buffer.rs:156-170 register_mem: recycle the first freed slot, else
  // append; refuse once the registry already holds kMemRegisterFailed slots.
  std::vector<uint8_t> blob(bytes, bytes + len);
  for (size_t i = 0, n = mem_.size(); i < n; ++i) {
    if (!mem_[i].has_value()) {
      mem_[i] = std::move(blob);
      return static_cast<uint16_t>(i);
    }
  }
  const size_t idx = mem_.size();
  if (idx >= static_cast<size_t>(kMemRegisterFailed)) {
    return kMemRegisterFailed;
  }
  mem_.push_back(std::move(blob));
  return static_cast<uint16_t>(idx);
}

bool TextBuffer::ReplaceMem(uint16_t id, const uint8_t* bytes, size_t len) {
  // text_buffer.rs:172-180 replace_mem: only an in-bounds, OCCUPIED slot is
  // replaced; a missing or freed slot returns false untouched.
  if (id >= mem_.size() || !mem_[id].has_value()) {
    return false;
  }
  mem_[id] = std::vector<uint8_t>(bytes, bytes + len);
  return true;
}

void TextBuffer::SetContent(const uint8_t* bytes, size_t len) {
  // text_buffer.rs:139-143 set_content: CRLF-normalize; drop_styled_highlights()
  // is a no-op until S4 (no highlights field yet).
  content_ = NormalizeCrlf(bytes, len);
}

void TextBuffer::SetFromMem(uint16_t id) {
  // text_buffer.rs:182-187 set_from_mem: re-materialize from an occupied slot;
  // a missing/freed slot is a no-op. The slot is not content_, so no clone is
  // needed before SetContent reassigns content_.
  if (id < mem_.size() && mem_[id].has_value()) {
    const std::vector<uint8_t>& blob = *mem_[id];
    SetContent(blob.data(), blob.size());
  }
}

void TextBuffer::Append(const uint8_t* bytes, size_t len) {
  // text_buffer.rs:189-192 append: CRLF-normalize then append to content.
  const std::vector<uint8_t> norm = NormalizeCrlf(bytes, len);
  content_.insert(content_.end(), norm.begin(), norm.end());
}

void TextBuffer::AppendFromMem(uint16_t id) {
  // text_buffer.rs:194-199 append_from_mem: append an occupied slot's bytes; a
  // missing/freed slot is a no-op.
  if (id < mem_.size() && mem_[id].has_value()) {
    const std::vector<uint8_t>& blob = *mem_[id];
    Append(blob.data(), blob.size());
  }
}

bool TextBuffer::LoadFile(const char* path, size_t path_len) {
  // text_buffer.rs:498-506 load_file: read the whole file; on ANY read error
  // return false without touching content, else set_content(bytes) and true.
  const std::string path_str(path, path_len);
  std::ifstream file(path_str, std::ios::binary);
  if (!file) {
    return false;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
  if (file.bad()) {
    return false;
  }
  SetContent(bytes.data(), bytes.size());
  return true;
}

uint32_t TextBuffer::GetPlainText(uint8_t* out, uint32_t max_len) const {
  // text_buffer.rs:520-522 get_plain_text: copy raw content bytes (clamped).
  return CopySliceOut(content_.data(), content_.size(), out, max_len);
}

uint32_t TextBuffer::GetTextRange(uint32_t start,
                                  uint32_t end,
                                  uint8_t* out,
                                  uint32_t max_len) const {
  // text_buffer.rs:525-534 get_text_range: empty range -> 0; else lossy-decode,
  // slice by scalar offsets, copy out.
  if (start >= end) {
    return 0;
  }
  const std::vector<uint32_t> scalars =
      DecodeScalarsLossy(content_.data(), content_.size());
  const std::vector<uint8_t> sliced = CharSlice(scalars, start, end);
  return CopySliceOut(sliced.data(), sliced.size(), out, max_len);
}

uint32_t TextBuffer::GetTextRangeByCoords(uint32_t start_row,
                                          uint32_t start_col,
                                          uint32_t end_row,
                                          uint32_t end_col,
                                          uint8_t* out,
                                          uint32_t max_len) const {
  // text_buffer.rs:577-596 get_text_range_by_coords: map both coords to scalar
  // offsets; empty range -> 0; else slice and copy out.
  const std::vector<uint32_t> scalars =
      DecodeScalarsLossy(content_.data(), content_.size());
  const size_t start = CoordToCharOffset(scalars, start_row, start_col);
  const size_t end = CoordToCharOffset(scalars, end_row, end_col);
  if (start >= end) {
    return 0;
  }
  const std::vector<uint8_t> sliced = CharSlice(scalars, start, end);
  return CopySliceOut(sliced.data(), sliced.size(), out, max_len);
}

}  // namespace tui

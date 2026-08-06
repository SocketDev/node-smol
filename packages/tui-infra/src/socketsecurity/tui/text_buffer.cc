// doctrine: allow max-file-lines -- 1:1 lockstep port of stuie's crates/stuie-cabi/src/text_buffer.rs; the per-file mirror is the audit surface, so a split would break comparability with the Rust reference.
// Native TextBuffer store.
//
// Port of stuie's crates/stuie-cabi/src/text_buffer.rs. Foundation slice:
// lifecycle helpers (clear/reset) plus the content metrics (display length,
// byte size, line count). See tui/text_buffer.hpp for the field-growth plan.

#include "tui/text_buffer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tui/handles.hpp"  // handles::Tag, kKindSyntaxStyle
#include "tui/width.hpp"    // CodepointWidth

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

// ── Cluster width model for the column-aware slice (get_text_range_by_cols) ──
//
// The TWIN of text_view.cc's IsWide / IsClusterExtender / NextCluster, working
// in the lossy-decoded scalar space DecodeScalarsLossy already produces (rather
// than raw bytes). Duplicated across the TU boundary on purpose: get_text_range
// _by_cols is a text_buffer.rs function, and both copies port the SAME stuie
// ranges (buffer.rs:50-70 is_wide, :95-106 is_cluster_extender, :115-146
// next_cluster) so the column space here is byte-identical to the S6 wrap /
// draw column space — NOT the Unicode-17 width.hpp model DisplayLength uses.

// buffer.rs:84-88: ZWJ folds the FOLLOWING scalar in; VS16 folds in AND promotes
// a width-1 base to width 2.
constexpr uint32_t kClusterZwj = 0x200D;
constexpr uint32_t kClusterVs16 = 0xFE0F;

// buffer.rs:50-70 is_wide: two terminal columns.
bool IsWideScalar(uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2329 && cp <= 0x232A) ||
         (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
         (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xA000 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE19) ||
         (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
         (cp >= 0x1F000 && cp <= 0x1F2FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

// buffer.rs:95-106 is_cluster_extender: zero-width folding scalars.
bool IsClusterExtenderScalar(uint32_t c) {
  return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
         (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
         (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) ||
         (c >= 0xE0100 && c <= 0xE01EF);
}

// buffer.rs:115-146 next_cluster in scalar space: fold the grapheme cluster
// starting at `scalars[i]` — base plus trailing extenders (VS16/combining) and
// ZWJ-joined scalars. Returns (scalar_count, width_cols) with the VS16
// emoji-presentation width-1 -> 2 promotion.
std::pair<size_t, uint32_t> NextClusterScalars(
    const std::vector<uint32_t>& scalars, size_t i) {
  const size_t n = scalars.size();
  if (i >= n) {
    return {0, 0};
  }
  uint32_t width = IsWideScalar(scalars[i]) ? 2u : 1u;
  size_t cluster_end = i + 1;  // one past the last scalar folded into the cluster
  size_t j = i + 1;
  bool join_next = false;
  while (j < n) {
    const uint32_t c = scalars[j];
    if (join_next) {
      cluster_end = j + 1;
      join_next = false;
      j += 1;
      continue;
    }
    if (c == kClusterZwj) {
      cluster_end = j + 1;
      join_next = true;
      j += 1;
      continue;
    }
    if (IsClusterExtenderScalar(c)) {
      if (c == kClusterVs16 && width == 1) {
        width = 2;
      }
      cluster_end = j + 1;
      j += 1;
      continue;
    }
    break;
  }
  return {cluster_end - i, width};
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
  // text_buffer.rs:145-148 clear(): drop content and the styled highlights,
  // keeping the user ones.
  content_.clear();
  DropStyledHighlights();
}

void TextBuffer::Reset() {
  // text_buffer.rs:150-154 reset(): drop content, ALL highlights, and the mem
  // registry.
  content_.clear();
  highlights_.clear();
  mem_.clear();
}

void TextBuffer::DropStyledHighlights() {
  // text_buffer.rs:134-137 drop_styled_highlights: retain only User highlights.
  std::vector<HlEntry> kept;
  kept.reserve(highlights_.size());
  for (HlEntry& e : highlights_) {
    if (e.kind == HlKind::kUser) {
      kept.push_back(std::move(e));
    }
  }
  highlights_ = std::move(kept);
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
  // text_buffer.rs:139-143 set_content: CRLF-normalize, then drop the styled
  // highlights (user highlights survive a content swap).
  content_ = NormalizeCrlf(bytes, len);
  DropStyledHighlights();
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

uint32_t TextBuffer::GetTextRangeByCols(uint32_t start,
                                        uint32_t end,
                                        uint8_t* out,
                                        uint32_t max_len) const {
  // text_buffer.rs:542-575 get_text_range_by_cols: empty range -> 0; else walk
  // the lossy-decoded content in grapheme-cluster space, emitting every cluster
  // whose START column is in [start, end). A newline occupies one column (its
  // '\n' scalar still emitted when in range); every other cluster occupies its
  // display width. Column advances saturating; the loop stops once col >= end.
  if (start >= end) {
    return 0;
  }
  const std::vector<uint32_t> scalars =
      DecodeScalarsLossy(content_.data(), content_.size());
  std::vector<uint8_t> sliced;
  uint32_t col = 0;
  size_t i = 0;
  const size_t n = scalars.size();
  while (i < n) {
    const std::pair<size_t, uint32_t> cluster = NextClusterScalars(scalars, i);
    const uint32_t w =
        (scalars[i] == static_cast<uint32_t>('\n')) ? 1u : cluster.second;
    if (col >= start && col < end) {
      for (size_t k = i; k < i + cluster.first; ++k) {
        EncodeScalar(scalars[k], &sliced);
      }
    }
    // Saturating add so a near-u32::MAX column can't wrap past `end`.
    col = (col > UINT32_MAX - w) ? UINT32_MAX : col + w;
    i += cluster.first;
    if (col >= end) {
      break;
    }
  }
  return CopySliceOut(sliced.data(), sliced.size(), out, max_len);
}

// ── Highlights + styled text + syntax style (S4) ──────────────────────────

namespace {

// Read a little-endian u32 at `bytes[base..]` (unaligned).
// text_buffer.rs:955-962 read_u32.
uint32_t ReadU32Le(const uint8_t* bytes, size_t base) {
  return static_cast<uint32_t>(bytes[base]) |
         (static_cast<uint32_t>(bytes[base + 1]) << 8) |
         (static_cast<uint32_t>(bytes[base + 2]) << 16) |
         (static_cast<uint32_t>(bytes[base + 3]) << 24);
}

// Read a little-endian u64 at `bytes[base..]` (unaligned).
// text_buffer.rs:964-969 read_u64.
uint64_t ReadU64Le(const uint8_t* bytes, size_t base) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(bytes[base + i]) << (8 * i);
  }
  return v;
}

// Dereference a borrowed RGBA buffer pointer (4x u16, low byte per channel). A
// null address reads as "no color". text_buffer.rs:977-985 read_rgba_ptr.
std::optional<std::array<uint16_t, 4>> ReadRgbaPtr(uint64_t addr) {
  if (addr == 0) {
    return std::nullopt;
  }
  const uint16_t* p = reinterpret_cast<const uint16_t*>(
      static_cast<uintptr_t>(addr));
  return std::array<uint16_t, 4>{p[0], p[1], p[2], p[3]};
}

// A decoded styled chunk view into the packed records buffer (borrowed text +
// colors). text_buffer.rs:946-952 StyledChunk.
struct StyledChunk {
  const uint8_t* text;
  size_t text_len;
  bool colored;
  std::optional<std::array<uint16_t, 4>> fg;
  std::optional<std::array<uint16_t, 4>> bg;
  uint32_t attrs;
};

// Parse the packed styled-chunk buffer the shim builds (text_buffer.rs:994-1024
// parse_styled_chunks): `count` x kStyledRecordSize records. Text + fg/bg colors
// live behind BORROWED process pointers valid for this synchronous call; the
// caller (SetStyled) copies before returning. A chunk is "colored" when it
// carries an fg, bg, or non-zero attributes.
std::vector<StyledChunk> ParseStyledChunks(const uint8_t* records,
                                           size_t len,
                                           uint32_t count) {
  std::vector<StyledChunk> out;
  const size_t n = static_cast<size_t>(count);
  if (records == nullptr || len < n * kStyledRecordSize) {
    return out;
  }
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const size_t base = i * kStyledRecordSize;
    const uint64_t text_addr = ReadU64Le(records, base);
    const size_t text_len = static_cast<size_t>(ReadU64Le(records, base + 8));
    std::optional<std::array<uint16_t, 4>> fg =
        ReadRgbaPtr(ReadU64Le(records, base + 16));
    std::optional<std::array<uint16_t, 4>> bg =
        ReadRgbaPtr(ReadU64Le(records, base + 24));
    const uint32_t attrs = ReadU32Le(records, base + 32);
    const uint8_t* text = nullptr;
    size_t tlen = 0;
    if (text_addr != 0 && text_len != 0) {
      text = reinterpret_cast<const uint8_t*>(
          static_cast<uintptr_t>(text_addr));
      tlen = text_len;
    }
    const bool colored = fg.has_value() || bg.has_value() || attrs != 0;
    out.push_back(StyledChunk{text, tlen, colored, fg, bg, attrs});
  }
  return out;
}

// Split lossy-decoded `scalars` into per-line display widths (newline-delimited,
// newlines excluded). Mirrors text_buffer.rs:630-636's split('\n') + width_of
// over the from_utf8_lossy content (width per codepoint, matching the shipped
// width surface). The result length always equals (newline count) + 1.
std::vector<uint32_t> LineWidthsFromScalars(
    const std::vector<uint32_t>& scalars) {
  std::vector<uint32_t> widths;
  uint32_t cur = 0;
  for (uint32_t cp : scalars) {
    if (cp == static_cast<uint32_t>('\n')) {
      widths.push_back(cur);
      cur = 0;
    } else {
      cur += CodepointWidth(cp);
    }
  }
  widths.push_back(cur);
  return widths;
}

// ── Minimal internal SyntaxStyle registry (syntax_style.rs) ──

struct SyntaxStyleEntry {
  std::optional<std::array<uint16_t, 4>> fg;
  std::optional<std::array<uint16_t, 4>> bg;
  uint32_t attrs;
};

struct SyntaxStyleTable {
  std::unordered_map<std::string, uint32_t> by_name;
  std::vector<SyntaxStyleEntry> styles;
};

struct SyntaxStyleRegistryT {
  std::mutex mu;
  uint32_t next = 1;
  std::unordered_map<uint32_t, SyntaxStyleTable> map;
};

SyntaxStyleRegistryT& SyntaxRegistry() {
  static SyntaxStyleRegistryT r;
  return r;
}

}  // namespace

uint32_t SyntaxStyleCreate() {
  // syntax_style.rs:93-99 create: monotonic, never 0.
  SyntaxStyleRegistryT& r = SyntaxRegistry();
  std::lock_guard<std::mutex> lock(r.mu);
  const uint32_t index = r.next;
  r.next += 1;
  if (r.next == 0) {
    r.next = 1;  // wrapping_add(1).max(1)
  }
  const uint32_t handle = handles::Tag(handles::kKindSyntaxStyle, index);
  r.map.emplace(handle, SyntaxStyleTable{});
  return handle;
}

void SyntaxStyleDestroy(uint32_t handle) {
  // syntax_style.rs:101-103 destroy.
  SyntaxStyleRegistryT& r = SyntaxRegistry();
  std::lock_guard<std::mutex> lock(r.mu);
  r.map.erase(handle);
}

uint32_t SyntaxStyleRegister(uint32_t handle,
                             const std::string& name,
                             std::optional<std::array<uint16_t, 4>> fg,
                             std::optional<std::array<uint16_t, 4>> bg,
                             uint32_t attrs) {
  // syntax_style.rs:42-55 register (via 105-116): update-in-place on a known
  // name (returns its 1-based id), else append. A stale handle -> 0.
  SyntaxStyleRegistryT& r = SyntaxRegistry();
  std::lock_guard<std::mutex> lock(r.mu);
  auto it = r.map.find(handle);
  if (it == r.map.end()) {
    return 0;
  }
  SyntaxStyleTable& t = it->second;
  auto found = t.by_name.find(name);
  if (found != t.by_name.end()) {
    const uint32_t id = found->second;
    if (id >= 1 && static_cast<size_t>(id - 1) < t.styles.size()) {
      t.styles[id - 1] = SyntaxStyleEntry{fg, bg, attrs};
    }
    return id;
  }
  t.styles.push_back(SyntaxStyleEntry{fg, bg, attrs});
  const uint32_t id = static_cast<uint32_t>(t.styles.size());
  t.by_name.emplace(name, id);
  return id;
}

std::optional<StyleTriple> SyntaxStyleResolveById(uint32_t handle, uint32_t id) {
  // syntax_style.rs:61-70 resolve_id (via 122-124): id 0 or out-of-range -> None.
  SyntaxStyleRegistryT& r = SyntaxRegistry();
  std::lock_guard<std::mutex> lock(r.mu);
  auto it = r.map.find(handle);
  if (it == r.map.end() || id == 0) {
    return std::nullopt;
  }
  const SyntaxStyleTable& t = it->second;
  if (static_cast<size_t>(id - 1) >= t.styles.size()) {
    return std::nullopt;
  }
  const SyntaxStyleEntry& s = t.styles[id - 1];
  return StyleTriple{s.fg, s.bg, s.attrs};
}

uint32_t SyntaxStyleGetStyleCount(uint32_t handle) {
  // syntax_style.rs:135-140 get_style_count; stale -> 0.
  SyntaxStyleRegistryT& r = SyntaxRegistry();
  std::lock_guard<std::mutex> lock(r.mu);
  auto it = r.map.find(handle);
  return it == r.map.end() ? 0
                           : static_cast<uint32_t>(it->second.styles.size());
}

void TextBuffer::AddHighlight(uint32_t line, const HighlightRepr& hl) {
  // text_buffer.rs:598-614 add_highlight: drop empty ranges, else store as User.
  if (hl.start >= hl.end) {
    return;
  }
  highlights_.push_back(
      HlEntry{line, hl, HlKind::kUser, std::nullopt, std::nullopt, 0});
}

void TextBuffer::AddHighlightByCharRange(const HighlightRepr& hl) {
  // text_buffer.rs:623-664 add_highlight_by_char_range: split [start,end) across
  // every logical line it overlaps, clamping to per-line [col_start,col_end).
  const uint32_t start = hl.start;
  const uint32_t end = hl.end;
  if (start >= end) {
    return;
  }
  const std::vector<uint32_t> scalars =
      DecodeScalarsLossy(content_.data(), content_.size());
  const std::vector<uint32_t> widths = LineWidthsFromScalars(scalars);
  uint32_t col_offset = 0;
  for (size_t line_idx = 0; line_idx < widths.size(); ++line_idx) {
    const uint32_t width = widths[line_idx];
    const uint32_t line_start = col_offset;
    const uint32_t line_end = col_offset + width;
    col_offset = line_end;
    if (line_end <= start) {
      continue;  // line ends before the range begins
    }
    if (line_start >= end) {
      break;  // this line (and every later one) begins after the range
    }
    const uint32_t col_start = start > line_start ? start - line_start : 0;
    const uint32_t col_end = end < line_end ? end - line_start : width;
    if (col_start >= col_end) {
      continue;
    }
    highlights_.push_back(HlEntry{static_cast<uint32_t>(line_idx),
                                  HighlightRepr{col_start, col_end, hl.style_id,
                                                hl.priority, 0, hl.hl_ref},
                                  HlKind::kUser, std::nullopt, std::nullopt, 0});
  }
}

void TextBuffer::RemoveHighlightsByRef(uint16_t hl_ref) {
  // text_buffer.rs:666-670 remove_highlights_by_ref.
  highlights_.erase(
      std::remove_if(highlights_.begin(), highlights_.end(),
                     [hl_ref](const HlEntry& e) {
                       return e.hl.hl_ref == hl_ref;
                     }),
      highlights_.end());
}

void TextBuffer::ClearLineHighlights(uint32_t line) {
  // text_buffer.rs:672-676 clear_line_highlights.
  highlights_.erase(
      std::remove_if(highlights_.begin(), highlights_.end(),
                     [line](const HlEntry& e) { return e.line == line; }),
      highlights_.end());
}

void TextBuffer::ClearAllHighlights() {
  // text_buffer.rs:678-680 clear_all_highlights.
  highlights_.clear();
}

namespace {

// Record one styled-chunk highlight carrying the chunk's inline colors and the
// synthetic per-chunk style_id (text_buffer.rs:265-281 push_styled_hl):
// priority 1, hlRef 0.
HlEntry MakeStyledHl(uint32_t line,
                     uint32_t start,
                     uint32_t end,
                     uint32_t style_id,
                     const StyledChunk& chunk) {
  return HlEntry{line,
                 HighlightRepr{start, end, style_id, 1, 0, 0},
                 HlKind::kStyled,
                 chunk.fg,
                 chunk.bg,
                 chunk.attrs};
}

}  // namespace

void TextBuffer::SetStyled(const uint8_t* records, size_t len, uint32_t count) {
  // text_buffer.rs:204-261 set_styled: clear (drops styled highlights +
  // content), then per chunk register a synthetic style (once per colored chunk)
  // and record one Styled highlight per colored line-segment.
  Clear();
  const std::vector<StyledChunk> chunks = ParseStyledChunks(records, len, count);
  const uint32_t tab_width = static_cast<uint32_t>(tab_width_);
  uint32_t col = 0;
  uint32_t line = 0;
  for (size_t i = 0; i < chunks.size(); ++i) {
    const StyledChunk& chunk = chunks[i];
    // Register a SYNTHETIC per-chunk style ("chunk{i}") ONCE per colored chunk,
    // stamping its 1-based id onto every line segment; buffers with no
    // syntax-style handle keep 0 (text_buffer.rs:218-228).
    uint32_t style_id = 0;
    if (chunk.colored && syntax_style_ != 0) {
      style_id = SyntaxStyleRegister(syntax_style_, "chunk" + std::to_string(i),
                                     chunk.fg, chunk.bg, chunk.attrs);
    }
    const std::vector<uint8_t> norm = NormalizeCrlf(chunk.text, chunk.text_len);
    const std::vector<uint32_t> scalars =
        DecodeScalarsLossy(norm.data(), norm.size());
    uint32_t seg_line = line;
    uint32_t seg_start = col;
    for (uint32_t cp : scalars) {
      if (cp == static_cast<uint32_t>('\n')) {
        if (chunk.colored && col > seg_start) {
          highlights_.push_back(
              MakeStyledHl(seg_line, seg_start, col, style_id, chunk));
        }
        line += 1;
        col = 0;
        seg_line = line;
        seg_start = 0;
      } else if (cp == static_cast<uint32_t>('\t')) {
        col += tab_width;
      } else {
        col += CodepointWidth(cp);
      }
    }
    if (chunk.colored && col > seg_start) {
      highlights_.push_back(
          MakeStyledHl(seg_line, seg_start, col, style_id, chunk));
    }
    content_.insert(content_.end(), norm.begin(), norm.end());
  }
}

std::pair<HighlightRepr*, uint32_t> TextBuffer::GetLineHighlights(
    uint32_t line) const {
  // text_buffer.rs:784-800 get_line_highlights: collect the line's records into
  // a heap array; (nullptr, 0) when the line has none.
  std::vector<HighlightRepr> items;
  for (const HlEntry& e : highlights_) {
    if (e.line == line) {
      items.push_back(e.hl);
    }
  }
  if (items.empty()) {
    return {nullptr, 0};
  }
  const uint32_t count = static_cast<uint32_t>(items.size());
  HighlightRepr* ptr = new HighlightRepr[count];
  std::memcpy(ptr, items.data(), static_cast<size_t>(count) * sizeof(HighlightRepr));
  return {ptr, count};
}

void TextBuffer::FreeLineHighlights(HighlightRepr* ptr, uint32_t count) {
  // text_buffer.rs:806-812 free_line_highlights: free the pair; null/0 is a
  // no-op.
  if (ptr == nullptr || count == 0) {
    return;
  }
  delete[] ptr;
}

std::vector<std::vector<SpanRun>> TextBuffer::BuildLineSpans() const {
  // text_buffer.rs:694-768 build_line_spans: per line, sweep highlights into
  // non-overlapping [col, next_col) runs; the strictly-highest-priority active
  // highlight (first wins ties) resolves each run's colors.
  const size_t line_count = static_cast<size_t>(LineCount());
  const uint32_t syntax = syntax_style_;
  std::vector<std::vector<SpanRun>> out(line_count);

  for (size_t line_idx = 0; line_idx < line_count; ++line_idx) {
    const uint32_t line = static_cast<uint32_t>(line_idx);
    // The line's highlights, kept in stored order (ties resolve to the first).
    std::vector<const HlEntry*> hls;
    for (const HlEntry& e : highlights_) {
      if (e.line == line) {
        hls.push_back(&e);
      }
    }
    if (hls.empty()) {
      continue;
    }

    // Boundary events: at a shared column, ends sort before starts.
    struct Event {
      uint32_t col;
      bool is_start;
      size_t idx;
    };
    std::vector<Event> events;
    events.reserve(hls.size() * 2);
    for (size_t idx = 0; idx < hls.size(); ++idx) {
      events.push_back(Event{hls[idx]->hl.start, true, idx});
      events.push_back(Event{hls[idx]->hl.end, false, idx});
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const Event& a, const Event& b) {
                       if (a.col != b.col) {
                         return a.col < b.col;
                       }
                       if (a.is_start != b.is_start) {
                         return a.is_start < b.is_start;  // false(end) first
                       }
                       return a.idx < b.idx;
                     });

    std::vector<size_t> active;
    uint32_t current_col = 0;
    for (const Event& ev : events) {
      if (ev.col > current_col) {
        // Winner: strictly-highest priority among active (first wins ties).
        bool have_best = false;
        size_t best = 0;
        int best_priority = -1;
        for (size_t idx : active) {
          const int p = static_cast<int>(hls[idx]->hl.priority);
          if (p > best_priority) {
            best_priority = p;
            best = idx;
            have_best = true;
          }
        }
        if (have_best) {
          const HlEntry& e = *hls[best];
          std::optional<std::array<uint16_t, 4>> fg;
          std::optional<std::array<uint16_t, 4>> bg;
          uint32_t attrs = 0;
          if (e.kind == HlKind::kStyled) {
            fg = e.fg;
            bg = e.bg;
            attrs = e.attrs;
          } else {
            std::optional<StyleTriple> resolved =
                SyntaxStyleResolveById(syntax, e.hl.style_id);
            if (resolved.has_value()) {
              fg = resolved->fg;
              bg = resolved->bg;
              attrs = resolved->attrs;
            }
          }
          out[line_idx].push_back(
              SpanRun{current_col, ev.col, fg, bg, attrs});
        }
        current_col = ev.col;
      }
      if (ev.is_start) {
        active.push_back(ev.idx);
      } else {
        active.erase(std::remove(active.begin(), active.end(), ev.idx),
                     active.end());
      }
    }
  }
  return out;
}

std::vector<std::string> TextBuffer::ContentLines() const {
  // text_buffer.rs:388 content_lines: from_utf8_lossy(content).split('\n').
  // split('\n') always yields at least one element, so empty content -> [""].
  // Each segment keeps its raw bytes (the measurement kernel decodes scalars
  // lossily downstream, matching from_utf8_lossy for well-formed input).
  std::vector<std::string> lines;
  size_t start = 0;
  for (size_t i = 0; i <= content_.size(); ++i) {
    if (i == content_.size() || content_[i] == static_cast<uint8_t>('\n')) {
      lines.emplace_back(reinterpret_cast<const char*>(content_.data()) + start,
                         i - start);
      start = i + 1;
    }
  }
  return lines;
}

}  // namespace tui

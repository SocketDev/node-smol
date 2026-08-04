// Native TextBufferView lifecycle + geometry/indicator config.
//
// Port of the lifecycle + config-setter subset of stuie's
// crates/stuie-cabi/src/text_view.rs (slice S5). See tui/text_view.hpp for the
// scope and registry-placement notes. The view registry and the per-buffer
// owned-child index live here (mirroring stuie's TBV_REGISTRY /
// OWNED_CHILD_VIEWS), so the owner-destroy cascade and borrowed-buffer
// rejection are self-contained and testable without the V8 binding.

#include "tui/text_view.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tui/handles.hpp"  // handles::Tag, kKindTextBufferView
#include "tui/utf8.hpp"     // DecodeUtf8

namespace tui {

namespace {

// The view registry: handle -> TextBufferView (text_view.rs:841-858
// TbvRegistry). A function-local static so first use initializes it, matching
// stuie's OnceLock. The mutex guards concurrent access exactly like stuie's
// Mutex<TbvRegistry>.
struct TbvRegistry {
  std::mutex mu;
  // Monotonic 1-based index folded into the kind-tagged handle. Never reused
  // within a process run, so a stale handle never aliases a fresh one.
  uint32_t next = 1;
  std::unordered_map<uint32_t, std::unique_ptr<TextBufferView>> map;
};

TbvRegistry& Registry() {
  static TbvRegistry r;
  return r;
}

// The owned-child index: buffer handle -> the external view handles created
// over it via createTextBufferView (text_view.rs:891-898 OWNED_CHILD_VIEWS).
// Views an EditorView creates internally are owned by the EditorView, not the
// buffer, so they are NOT tracked here.
struct OwnedChildViews {
  std::mutex mu;
  std::unordered_map<uint32_t, std::vector<uint32_t>> map;
};

OwnedChildViews& OwnedChildren() {
  static OwnedChildViews r;
  return r;
}

}  // namespace

uint32_t TbvCreate(uint32_t tb) {
  TbvRegistry& r = Registry();
  std::lock_guard<std::mutex> lock(r.mu);
  uint32_t handle = handles::Tag(handles::kKindTextBufferView, r.next);
  // Advance the monotonic index (wrap to 1, never 0), mirroring
  // reg.next.wrapping_add(1).max(1) (text_view.rs:870).
  r.next += 1;
  if (r.next == 0) {
    r.next = 1;
  }
  r.map.emplace(handle, std::make_unique<TextBufferView>(tb));
  return handle;
}

uint32_t TbvCreateOwned(uint32_t tb, bool buffer_borrowed) {
  // A borrowed text buffer (an EditBuffer's inner buffer) cannot own external
  // views; return the 0 "failed" handle (the shim then throws), mirroring
  // createTextBufferView's is_borrowed gate (cabi.rs:1391-1406).
  if (buffer_borrowed) {
    return 0;
  }
  uint32_t handle = TbvCreate(tb);
  if (handle != 0) {
    RegisterOwnedChild(tb, handle);
  }
  return handle;
}

bool TbvIsValid(uint32_t handle) {
  TbvRegistry& r = Registry();
  std::lock_guard<std::mutex> lock(r.mu);
  return r.map.find(handle) != r.map.end();
}

void TbvDestroy(uint32_t handle) {
  TbvRegistry& r = Registry();
  std::lock_guard<std::mutex> lock(r.mu);
  r.map.erase(handle);
}

void RegisterOwnedChild(uint32_t tb, uint32_t view) {
  OwnedChildViews& r = OwnedChildren();
  std::lock_guard<std::mutex> lock(r.mu);
  r.map[tb].push_back(view);
}

void DestroyOwnedChildren(uint32_t tb) {
  // Take the child list out under the owned-child lock, then destroy each view.
  // TbvDestroy takes the view-registry lock separately, so the two locks are
  // never held at once (no lock-order coupling). text_view.rs:911-918.
  std::vector<uint32_t> children;
  {
    OwnedChildViews& r = OwnedChildren();
    std::lock_guard<std::mutex> lock(r.mu);
    auto it = r.map.find(tb);
    if (it == r.map.end()) {
      return;
    }
    children = std::move(it->second);
    r.map.erase(it);
  }
  for (uint32_t view : children) {
    TbvDestroy(view);
  }
}

namespace {

// with_tbv (text_view.rs:860-865): run `fn` against the live view for `handle`,
// or do nothing for a stale/wrong-kind handle. The lock is held for the call.
template <typename Fn>
void WithTbv(uint32_t handle, Fn&& fn) {
  TbvRegistry& r = Registry();
  std::lock_guard<std::mutex> lock(r.mu);
  auto it = r.map.find(handle);
  if (it != r.map.end()) {
    fn(*it->second);
  }
}

}  // namespace

void TbvSetWrapMode(uint32_t handle, uint8_t mode) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetWrapMode(mode); });
}

void TbvSetWrapWidth(uint32_t handle, uint32_t width) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetWrapWidth(width); });
}

void TbvSetFirstLineOffset(uint32_t handle, uint32_t offset) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetFirstLineOffset(offset); });
}

void TbvSetViewport(uint32_t handle, uint32_t x, uint32_t y, uint32_t width,
                    uint32_t height) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetViewport(x, y, width, height); });
}

void TbvSetViewportSize(uint32_t handle, uint32_t width, uint32_t height) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetViewportSize(width, height); });
}

void TbvSetTruncate(uint32_t handle, bool truncate) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetTruncate(truncate); });
}

void TbvSetTabIndicator(uint32_t handle, uint32_t code_point) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetTabIndicator(code_point); });
}

void TbvSetTabIndicatorColor(uint32_t handle,
                             std::optional<std::array<uint16_t, 4>> color) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetTabIndicatorColor(color); });
}

// ═══════════════════════════════════════════════════════════════════════════
// Virtual-line layout / measurement kernel (slice S6)
//
// Faithful port of stuie text_view.rs's calculate_virtual_lines (VLine /
// VirtualLines / WrapCursor), word_segments, apply_truncation, measure_lines,
// and serialize_line_info. Cell widths use stuie's own char_width/is_wide
// ranges (buffer.rs:42-70) so the wrap results are byte-identical to stuie's
// #[test] fixtures rather than merely close to them.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// A per-cluster (base scalar, display width) pair — one element of line_cells.
// text_view.rs:106-116 line_cells.
struct Cell {
  uint32_t cp;
  uint32_t width;
};

// Zero Width Joiner (folds the FOLLOWING scalar in) and Variation Selector-16
// (folds in AND promotes a width-1 base to width 2). buffer.rs:84-88.
constexpr uint32_t kZwj = 0x200D;
constexpr uint32_t kVs16 = 0xFE0F;

// buffer.rs:50-70 is_wide: whether a code point occupies two terminal columns.
bool IsWide(uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2329 && cp <= 0x232A) ||
         (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
         (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xA000 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE19) ||
         (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
         (cp >= 0x1F000 && cp <= 0x1F2FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

// buffer.rs:42-48 char_width.
uint32_t CharWidth(uint32_t cp) {
  return IsWide(cp) ? 2u : 1u;
}

// buffer.rs:95-106 is_cluster_extender: zero-width scalars that fold into the
// preceding base's cluster (variation selectors + combining-mark blocks).
bool IsClusterExtender(uint32_t c) {
  return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
         (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
         (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) ||
         (c >= 0xE0100 && c <= 0xE01EF);
}

// buffer.rs:115-146 next_cluster: fold the grapheme cluster at [s, end) — base
// scalar plus trailing extenders (VS16/combining) and ZWJ-joined scalars.
// Returns (byte_len, width_cols) with the VS16 emoji-presentation promotion.
std::pair<size_t, uint32_t> NextCluster(const char* s, const char* end) {
  if (s >= end) {
    return {0, 0};
  }
  const char* p = s;
  const uint32_t base = DecodeUtf8(p, end);  // advances p past the base scalar
  uint32_t width = CharWidth(base);
  const char* cluster_end = p;
  bool join_next = false;
  while (p < end) {
    const char* q = p;
    const uint32_t c = DecodeUtf8(q, end);
    if (join_next) {
      cluster_end = q;
      join_next = false;
      p = q;
      continue;
    }
    if (c == kZwj) {
      cluster_end = q;
      join_next = true;
      p = q;
      continue;
    }
    if (IsClusterExtender(c)) {
      if (c == kVs16 && width == 1) {
        width = 2;
      }
      cluster_end = q;
      p = q;
      continue;
    }
    break;
  }
  return {static_cast<size_t>(cluster_end - s), width};
}

// text_view.rs:106-116 line_cells: one Cell per grapheme cluster; a '\t'
// expands to tab_width columns, everything else keeps its cluster width.
std::vector<Cell> LineCells(const std::string& line, uint32_t tab_width) {
  std::vector<Cell> out;
  const char* p = line.data();
  const char* const end = p + line.size();
  while (p < end) {
    const char* bp = p;
    const uint32_t base = DecodeUtf8(bp, end);  // base scalar for '\t' + class
    const std::pair<size_t, uint32_t> cluster = NextCluster(p, end);
    const uint32_t width = (base == static_cast<uint32_t>('\t')) ? tab_width
                                                                 : cluster.second;
    out.push_back(Cell{base, width});
    p += cluster.first;
  }
  return out;
}

// text_view.rs:122-160 is_wrap_break_char: a word may break AFTER such a char.
bool IsWrapBreakChar(uint32_t c) {
  switch (c) {
    case ' ':
    case '\t':
    case '-':
    case '/':
    case '\\':
    case '.':
    case ',':
    case ';':
    case ':':
    case '!':
    case '?':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case 0x00A0:  // NBSP
    case 0x1680:  // OGHAM SPACE MARK
    case 0x202F:  // NARROW NO-BREAK SPACE
    case 0x205F:  // MEDIUM MATHEMATICAL SPACE
    case 0x3000:  // IDEOGRAPHIC SPACE
    case 0x200B:  // ZERO WIDTH SPACE
    case 0x00AD:  // SOFT HYPHEN
    case 0x2010:  // HYPHEN
    case 0x3001:  // IDEOGRAPHIC COMMA
    case 0x3002:  // IDEOGRAPHIC FULL STOP
    case 0xFF01:  // FULLWIDTH EXCLAMATION MARK
    case 0xFF0C:  // FULLWIDTH COMMA
    case 0xFF1A:  // FULLWIDTH COLON
    case 0xFF1F:  // FULLWIDTH QUESTION MARK
      return true;
    default:
      return c >= 0x2000 && c <= 0x200A;  // En quad .. Hair space
  }
}

// text_view.rs:165-215 WordClass / classify_word_class / is_cjk_word_codepoint.
enum class WordClass { kAsciiWord, kCjkWord, kOther };

bool IsCjkWordCodepoint(uint32_t cp) {
  return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2A6DF) ||
         (cp >= 0x2A700 && cp <= 0x2B73F) || (cp >= 0x2B740 && cp <= 0x2B81F) ||
         (cp >= 0x2B820 && cp <= 0x2CEAF) || (cp >= 0x2CEB0 && cp <= 0x2EBEF) ||
         (cp >= 0x2EBF0 && cp <= 0x2EE5D) || (cp >= 0x2F800 && cp <= 0x2FA1F) ||
         (cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
         (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF66 && cp <= 0xFF9D) ||
         (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) ||
         (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
         (cp >= 0xD7B0 && cp <= 0xD7FF);
}

WordClass WordClassOf(uint32_t cp) {
  if (cp <= 0x7F) {
    const bool alnum = (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
                       (cp >= 'a' && cp <= 'z') || cp == '_';
    return alnum ? WordClass::kAsciiWord : WordClass::kOther;
  }
  return IsCjkWordCodepoint(cp) ? WordClass::kCjkWord : WordClass::kOther;
}

// text_view.rs:219-222 is_cjk_ascii_transition.
bool IsCjkAsciiTransition(WordClass prev, WordClass curr) {
  return (prev == WordClass::kCjkWord && curr == WordClass::kAsciiWord) ||
         (prev == WordClass::kAsciiWord && curr == WordClass::kCjkWord);
}

// text_view.rs:465-486 word_segments: group cells into break units — a unit
// ends AFTER a wrap-break char and BETWEEN a CJK<->ASCII transition.
std::vector<std::vector<Cell>> WordSegments(const std::vector<Cell>& cells) {
  std::vector<std::vector<Cell>> out;
  std::vector<Cell> cur;
  std::optional<WordClass> prev_class;
  for (const Cell& cell : cells) {
    const WordClass cls = WordClassOf(cell.cp);
    if (prev_class.has_value() && !cur.empty() &&
        IsCjkAsciiTransition(*prev_class, cls)) {
      out.push_back(std::move(cur));
      cur.clear();
    }
    cur.push_back(cell);
    if (IsWrapBreakChar(cell.cp)) {
      out.push_back(std::move(cur));
      cur.clear();
    }
    prev_class = cls;
  }
  if (!cur.empty()) {
    out.push_back(std::move(cur));
  }
  return out;
}

VLine MakeVLine(uint32_t col_offset, uint32_t width_cols, uint32_t source_line,
                uint32_t source_col_offset, uint32_t wrap_index) {
  VLine v;
  v.col_offset = col_offset;
  v.width_cols = width_cols;
  v.source_line = source_line;
  v.source_col_offset = source_col_offset;
  v.wrap_index = wrap_index;
  return v;
}

// text_view.rs:354-458 WrapCursor: the per-logical-line wrap emitter shared by
// the char/word arms. Points at the accumulating vline vector, count, and the
// global/first-line-pending cursors owned by CalculateVirtualLines.
struct WrapCursor {
  std::vector<VLine>* vlines;
  uint32_t* count;
  uint32_t line_idx;
  uint32_t ww;
  uint32_t first_line_offset;
  bool* first_line_pending;
  uint32_t* global;
  uint32_t source_col;
  uint32_t cur_w;
  uint32_t cur_col_offset;
  uint32_t cur_source_col;
  uint32_t wrap_index;
  bool any_in_line;

  // text_view.rs:374-383 line_wrap_w: the first vline of the first logical line
  // is narrowed by first_line_offset when set.
  uint32_t LineWrapW() const {
    if (*first_line_pending && first_line_offset > 0 && first_line_offset < ww) {
      return ww - first_line_offset;
    }
    return ww;
  }

  // text_view.rs:386-400 flush.
  void Flush() {
    vlines->push_back(
        MakeVLine(cur_col_offset, cur_w, line_idx, cur_source_col, wrap_index));
    ++(*count);
    ++wrap_index;
    cur_col_offset = *global;
    cur_w = 0;
    *first_line_pending = false;
    cur_source_col = source_col;
  }

  // text_view.rs:403-411 push_cols.
  void PushCols(uint32_t w) {
    if (cur_w == 0) {
      cur_source_col = source_col;
    }
    cur_w += w;
    *global += w;
    source_col += w;
    any_in_line = true;
  }

  // text_view.rs:415-423 push_char.
  void PushChar(uint32_t cw) {
    if (cur_w > 0 && cur_w + cw > LineWrapW()) {
      Flush();
    }
    cur_w += cw;
    *global += cw;
    source_col += cw;
    any_in_line = true;
  }

  // text_view.rs:429-442 push_segment_char_mode.
  void PushSegmentCharMode(const std::vector<Cell>& seg) {
    uint32_t seg_w = 0;
    for (const Cell& c : seg) {
      seg_w += c.width;
    }
    const uint32_t line_wrap_w = LineWrapW();
    if (cur_w > 0 && cur_w + seg_w > line_wrap_w) {
      Flush();
    }
    if (seg_w > line_wrap_w) {
      for (const Cell& c : seg) {
        PushChar(c.width);
      }
    } else {
      PushCols(seg_w);
    }
  }

  // text_view.rs:446-457 finish.
  void Finish() {
    if (cur_w > 0 || !any_in_line) {
      vlines->push_back(MakeVLine(cur_col_offset, cur_w, line_idx,
                                  cur_source_col, wrap_index));
      ++(*count);
    }
  }
};

// text_view.rs:522-547 apply_truncation.
void ApplyTruncation(std::vector<VLine>& vlines, uint32_t vp_width) {
  if (vp_width == 0) {
    return;
  }
  const uint32_t ellipsis_width = 3;
  for (VLine& v : vlines) {
    if (v.width_cols <= vp_width) {
      continue;
    }
    if (vp_width <= ellipsis_width) {
      v.width_cols = 0;
      v.is_truncated = true;
      v.ellipsis_pos = 0;
      v.truncation_suffix_start = 0;
      continue;
    }
    const uint32_t available = vp_width - ellipsis_width;
    const uint32_t prefix_width = available / 2;
    const uint32_t suffix_width = available - prefix_width;
    const uint32_t suffix_start_pos = v.width_cols - suffix_width;
    v.width_cols = vp_width;
    v.is_truncated = true;
    v.ellipsis_pos = prefix_width;
    v.truncation_suffix_start = suffix_start_pos;
  }
}

// text_view.rs:553-572 measure_lines: (line_count, width_cols_max). The
// wrapping path counts virtual lines; the no-wrap / zero-width path reports the
// logical line count and widest logical line.
std::pair<uint32_t, uint32_t> MeasureLines(const BufferData& bd,
                                           uint8_t wrap_mode,
                                           std::optional<uint32_t> wrap_width,
                                           uint32_t first_line_offset) {
  if (wrap_width.has_value() && wrap_mode != kWrapNone && *wrap_width > 0) {
    const VirtualLines vl = CalculateVirtualLines(
        bd.content_lines, bd.tab_width_cols, wrap_mode, wrap_width,
        first_line_offset);
    uint32_t max_w = 0;
    for (const VLine& v : vl.vlines) {
      max_w = std::max(max_w, v.width_cols);
    }
    return {static_cast<uint32_t>(vl.vlines.size()), max_w};
  }
  const uint32_t line_count = static_cast<uint32_t>(bd.line_widths.size());
  uint32_t max_w = 0;
  for (uint32_t w : bd.line_widths) {
    max_w = std::max(max_w, w);
  }
  return {line_count, max_w};
}

// text_view.rs:581-605 serialize_line_info: [count, widthColsMax, startCols[],
// widthCols[], sources[], wraps[]] into the u32 out-buffer.
uint32_t SerializeLineInfo(const VLine* slice, size_t slice_len,
                           uint32_t width_cols_max, uint32_t* out,
                           uint32_t max_entries) {
  if (out == nullptr) {
    return 0;
  }
  const uint32_t count =
      std::min(static_cast<uint32_t>(slice_len), max_entries);
  const size_t n = count;
  out[0] = count;
  out[1] = width_cols_max;
  uint32_t* starts = out + 2;
  uint32_t* widths = out + 2 + n;
  uint32_t* sources = out + 2 + 2 * n;
  uint32_t* wraps = out + 2 + 3 * n;
  for (size_t i = 0; i < n; ++i) {
    starts[i] = slice[i].col_offset;
    widths[i] = slice[i].width_cols;
    sources[i] = slice[i].source_line;
    wraps[i] = slice[i].wrap_index;
  }
  return count;
}

// with_tbv (text_view.rs:860-865) with a return value: run `fn` against the
// live view for `handle`, or return `fallback` for a stale/wrong-kind handle.
// Holds the view-registry lock for the call — `fn` may take the SEPARATE
// buffer-registry lock (via the resolver), matching stuie's nested
// TBV_REGISTRY -> TEXT_BUFFER lock order.
template <typename T, typename Fn>
T WithTbvRet(uint32_t handle, T fallback, Fn&& fn) {
  TbvRegistry& r = Registry();
  std::lock_guard<std::mutex> lock(r.mu);
  auto it = r.map.find(handle);
  if (it == r.map.end()) {
    return fallback;
  }
  return fn(*it->second);
}

// text_view.rs:648-671 wrap_width_for_calc + virtual_lines: materialize the
// view's virtual-line model, applying truncation when truncate && viewport.
VirtualLines VirtualLinesFor(const TextBufferView& v,
                             const BufferResolver& resolve) {
  const BufferData bd = resolve(v.active_tb);
  const std::optional<uint32_t> ww =
      (v.wrap_mode != kWrapNone) ? v.wrap_width : std::optional<uint32_t>{};
  VirtualLines vl = CalculateVirtualLines(bd.content_lines, bd.tab_width_cols,
                                          v.wrap_mode, ww, v.first_line_offset);
  if (v.truncate && v.viewport.has_value()) {
    ApplyTruncation(vl.vlines, (*v.viewport)[2]);
  }
  return vl;
}

}  // namespace

std::vector<uint32_t> LineWidths(const std::vector<std::string>& content_lines,
                                 uint32_t tab_width) {
  std::vector<uint32_t> out;
  out.reserve(content_lines.size());
  for (const std::string& line : content_lines) {
    uint32_t width = 0;
    for (const Cell& c : LineCells(line, tab_width)) {
      width += c.width;
    }
    out.push_back(width);
  }
  return out;
}

VirtualLines CalculateVirtualLines(
    const std::vector<std::string>& content_lines, uint32_t tab_width,
    uint8_t wrap_mode, std::optional<uint32_t> wrap_width,
    uint32_t first_line_offset) {
  VirtualLines result;
  std::vector<VLine>& vlines = result.vlines;
  std::vector<uint32_t>& first_vline = result.first_vline;
  std::vector<uint32_t>& vline_counts = result.vline_counts;
  vlines.reserve(content_lines.size());
  first_vline.reserve(content_lines.size());
  vline_counts.reserve(content_lines.size());

  // text_view.rs:240-243: wrapping is active only for a non-none mode with a
  // positive width.
  std::optional<uint32_t> wrap_w;
  if (wrap_mode != kWrapNone && wrap_width.has_value() && *wrap_width > 0) {
    wrap_w = *wrap_width;
  }

  uint32_t global = 0;
  bool first_line_pending = first_line_offset > 0;

  for (size_t li = 0; li < content_lines.size(); ++li) {
    const uint32_t line_idx = static_cast<uint32_t>(li);
    const std::vector<Cell> cells = LineCells(content_lines[li], tab_width);
    const uint32_t first_idx = static_cast<uint32_t>(vlines.size());
    uint32_t count = 0;

    if (!wrap_w.has_value()) {
      // Wrapping disabled: one vline per logical line.
      uint32_t width = 0;
      for (const Cell& c : cells) {
        width += c.width;
      }
      vlines.push_back(MakeVLine(global, width, line_idx, 0, 0));
      global += width;
      count = 1;
    } else {
      const uint32_t ww = *wrap_w;
      if (cells.empty()) {
        // Empty logical line still occupies one virtual line.
        vlines.push_back(MakeVLine(global, 0, line_idx, 0, 0));
        count = 1;
      } else {
        // char mode wraps every cell as its own breakable unit; word mode
        // groups cells into break units.
        std::vector<std::vector<Cell>> segments;
        if (wrap_mode == 1) {
          segments.reserve(cells.size());
          for (const Cell& c : cells) {
            segments.push_back({c});
          }
        } else {
          segments = WordSegments(cells);
        }

        const uint32_t line_start = global;
        WrapCursor cur{&vlines,
                       &count,
                       line_idx,
                       ww,
                       first_line_offset,
                       &first_line_pending,
                       &global,
                       /*source_col=*/0,
                       /*cur_w=*/0,
                       /*cur_col_offset=*/line_start,
                       /*cur_source_col=*/0,
                       /*wrap_index=*/0,
                       /*any_in_line=*/false};

        if (wrap_mode == 1) {
          for (const std::vector<Cell>& seg : segments) {
            cur.PushSegmentCharMode(seg);
          }
        } else {
          // word-wrap: break units flow whole; a unit overflowing an EMPTY
          // line enters fill-mode, which char-fills the rest of the logical
          // line to the wrap width ignoring later word boundaries.
          bool fill_mode = false;
          for (const std::vector<Cell>& seg : segments) {
            uint32_t seg_w = 0;
            for (const Cell& c : seg) {
              seg_w += c.width;
            }
            if (!fill_mode) {
              const uint32_t line_wrap_w = cur.LineWrapW();
              if (cur.cur_w > 0 && cur.cur_w + seg_w > line_wrap_w) {
                cur.Flush();
              }
              if (seg_w <= cur.LineWrapW()) {
                cur.PushCols(seg_w);
                continue;
              }
              fill_mode = true;
            }
            for (const Cell& c : seg) {
              cur.PushChar(c.width);
            }
          }
        }
        cur.Finish();
      }
    }

    first_vline.push_back(first_idx);
    vline_counts.push_back(count);
    // Newline occupies one global column.
    global += 1;
    first_line_pending = false;
  }

  return result;
}

uint32_t TbvVirtualLineCount(uint32_t handle, const BufferResolver& resolve) {
  return WithTbvRet<uint32_t>(handle, 0, [&](const TextBufferView& v) {
    return static_cast<uint32_t>(VirtualLinesFor(v, resolve).vlines.size());
  });
}

uint64_t TbvMeasure(uint32_t handle, uint32_t width, uint32_t /*height*/,
                    const BufferResolver& resolve) {
  const std::pair<uint32_t, uint32_t> r =
      WithTbvRet<std::pair<uint32_t, uint32_t>>(
          handle, std::pair<uint32_t, uint32_t>{1u, 0u},
          [&](const TextBufferView& v) {
            const std::optional<uint32_t> wrap_width =
                (v.wrap_mode != kWrapNone && width > 0)
                    ? std::optional<uint32_t>{width}
                    : std::optional<uint32_t>{};
            const BufferData bd = resolve(v.active_tb);
            return MeasureLines(bd, v.wrap_mode, wrap_width,
                                v.first_line_offset);
          });
  return (static_cast<uint64_t>(r.first) << 32) |
         static_cast<uint64_t>(r.second);
}

uint32_t TbvGetLineInfo(uint32_t handle, uint32_t* out, uint32_t max_entries,
                        const BufferResolver& resolve) {
  return WithTbvRet<uint32_t>(handle, 0, [&](const TextBufferView& v) {
    const VirtualLines vl = VirtualLinesFor(v, resolve);
    size_t start = 0;
    size_t end = vl.vlines.size();
    if (v.viewport.has_value()) {
      const uint32_t y = (*v.viewport)[1];
      const uint32_t h = (*v.viewport)[3];
      start = std::min(static_cast<size_t>(y), vl.vlines.size());
      end = std::min(start + static_cast<size_t>(h), vl.vlines.size());
    }
    uint32_t max_w = 0;
    for (size_t i = start; i < end; ++i) {
      max_w = std::max(max_w, vl.vlines[i].width_cols);
    }
    return SerializeLineInfo(vl.vlines.data() + start, end - start, max_w, out,
                             max_entries);
  });
}

uint32_t TbvGetLogicalLineInfo(uint32_t handle, uint32_t* out,
                               uint32_t max_entries,
                               const BufferResolver& resolve) {
  return WithTbvRet<uint32_t>(handle, 0, [&](const TextBufferView& v) {
    const VirtualLines vl = VirtualLinesFor(v, resolve);
    const BufferData bd = resolve(v.active_tb);
    uint32_t max_w = 0;
    for (uint32_t w : bd.line_widths) {
      max_w = std::max(max_w, w);
    }
    return SerializeLineInfo(vl.vlines.data(), vl.vlines.size(), max_w, out,
                             max_entries);
  });
}

}  // namespace tui

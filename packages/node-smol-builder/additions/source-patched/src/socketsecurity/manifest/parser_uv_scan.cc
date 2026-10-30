// node:smol-manifest — uv.lock lexical scanners implementation.
//
// See parser_uv_scan.h for the per-function contracts and
// parser_uv.h for the source-material register. Everything here is
// line-local: canonical uv output never splits a string or an inline
// table across lines (uv's own reader relies on the same property —
// https://github.com/astral-sh/uv/blob/0.12.1/crates/uv-resolver/src/lock/deserialize.rs
// scans values with byte cursors, not a TOML tree), so per-line
// quote/bracket tracking is complete.

#include "parser_uv_scan.h"

#include <string>
#include <string_view>
#include <vector>

namespace node {
namespace socketsecurity {
namespace manifest {
namespace uvscan {

std::string_view TrimAscii(std::string_view s) {
  size_t lo = 0;
  while (lo < s.size() && (s[lo] == ' ' || s[lo] == '\t')) {
    ++lo;
  }
  size_t hi = s.size();
  while (hi > lo &&
         (s[hi - 1] == ' ' || s[hi - 1] == '\t' || s[hi - 1] == '\r')) {
    --hi;
  }
  return s.substr(lo, hi - lo);
}

size_t NextLf(std::string_view content, size_t from) {
  while (from < content.size() && content[from] != '\n') {
    ++from;
  }
  return from;
}

std::string_view StripQuotes(std::string_view s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

std::string_view KeyOf(std::string_view line) {
  size_t eq = line.find('=');
  if (eq == std::string_view::npos) return std::string_view{};
  return TrimAscii(line.substr(0, eq));
}

std::string_view ValueAfterEquals(std::string_view line) {
  size_t eq = line.find('=');
  if (eq == std::string_view::npos) return std::string_view{};
  return TrimAscii(line.substr(eq + 1));
}

int NetBrackets(std::string_view line) {
  int net = 0;
  bool in_str = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_str) {
      if (c == '\\') {
        ++i;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
    } else if (c == '[') {
      ++net;
    } else if (c == ']') {
      --net;
    }
  }
  return net;
}

// Lock-step with sdxgen's normalizePep503
// (src/parsers/pypi/index.mts:288-290): lowercase + collapse `[-_.]`
// runs to one `-`. The fast path interns the borrowed view directly;
// the slow path builds the normalized copy, and Intern() copies it
// into the arena so the returned view outlives this scratch string.
std::string_view NormalizePep503(std::string_view s, ParseContext* ctx) {
  bool canonical = true;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if ((c >= 'A' && c <= 'Z') || c == '_' || c == '.' ||
        (c == '-' && i + 1 < s.size() && s[i + 1] == '-')) {
      canonical = false;
      break;
    }
  }
  if (canonical) {
    return ctx->intern.Intern(s);
  }
  std::string buf;
  buf.reserve(s.size());
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '_' || c == '.') c = '-';
    if (c == '-' && !buf.empty() && buf.back() == '-') continue;
    buf.push_back(c);
  }
  return ctx->intern.Intern(std::string_view(buf));
}

// Key must sit on a token boundary ('{', ',' or space before; space
// or '=' after). Quoted values end at the closing quote, so commas
// inside `marker = "..."` strings can't truncate the value.
bool InlineTableString(std::string_view table, std::string_view key,
                       std::string_view* out_value) {
  size_t pos = 0;
  while ((pos = table.find(key, pos)) != std::string_view::npos) {
    const bool boundary_before =
        pos > 0 && (table[pos - 1] == '{' || table[pos - 1] == ',' ||
                    table[pos - 1] == ' ');
    size_t after = pos + key.size();
    while (after < table.size() && table[after] == ' ') {
      ++after;
    }
    if (!boundary_before || after >= table.size() || table[after] != '=') {
      pos += key.size();
      continue;
    }
    ++after;
    while (after < table.size() && table[after] == ' ') {
      ++after;
    }
    if (after < table.size() && table[after] == '"') {
      size_t close = table.find('"', after + 1);
      if (close == std::string_view::npos) return false;
      *out_value = table.substr(after + 1, close - after - 1);
      return true;
    }
    size_t end = after;
    while (end < table.size() && table[end] != ',' && table[end] != '}') {
      ++end;
    }
    *out_value = TrimAscii(table.substr(after, end - after));
    return true;
  }
  return false;
}

// Entries are `{ name = "x", ... }` inline tables (canonical) or
// bare quoted strings. Empty view when no name is extractable.
std::string_view ExtractUvDepName(std::string_view entry, ParseContext* ctx) {
  std::string_view s = TrimAscii(entry);
  if (!s.empty() && s.back() == ',') {
    s.remove_suffix(1);
    s = TrimAscii(s);
  }
  if (s.empty()) return std::string_view{};
  if (s.front() == '{') {
    std::string_view name;
    if (!InlineTableString(s, "name", &name) || name.empty()) {
      return std::string_view{};
    }
    return NormalizePep503(name, ctx);
  }
  std::string_view bare = StripQuotes(s);
  if (bare.empty()) return std::string_view{};
  return NormalizePep503(bare, ctx);
}

// Splits on commas at depth 0 (outside braces/brackets/quotes), so
// nested tables like `{ name = "x", source = { registry = "..." } }`
// and nested arrays like `extra = ["http2"]` stay whole.
void ParseInlineEntries(std::string_view value, ParseContext* ctx,
                        std::vector<std::string_view>* out) {
  size_t lb = value.find('[');
  size_t rb = value.rfind(']');
  if (lb == std::string_view::npos || rb == std::string_view::npos ||
      rb <= lb) {
    return;
  }
  std::string_view inner = value.substr(lb + 1, rb - lb - 1);
  int depth = 0;
  bool in_str = false;
  size_t start = 0;
  for (size_t i = 0; i <= inner.size(); ++i) {
    if (i < inner.size() && in_str) {
      if (inner[i] == '\\') {
        ++i;
      } else if (inner[i] == '"') {
        in_str = false;
      }
      continue;
    }
    if (i < inner.size() && inner[i] == '"') {
      in_str = true;
      continue;
    }
    if (i < inner.size() && (inner[i] == '{' || inner[i] == '[')) {
      ++depth;
      continue;
    }
    if (i < inner.size() && (inner[i] == '}' || inner[i] == ']')) {
      --depth;
      continue;
    }
    if (i == inner.size() || (inner[i] == ',' && depth == 0)) {
      std::string_view name =
          ExtractUvDepName(inner.substr(start, i - start), ctx);
      if (!name.empty()) {
        out->push_back(name);
      }
      start = i + 1;
    }
  }
}

}  // namespace uvscan
}  // namespace manifest
}  // namespace socketsecurity
}  // namespace node

// node:smol-manifest — uv.lock lexical scanners.
//
// The line/token half of the uv.lock parser: ASCII trimming, TOML
// `key = value` splitting, quote-aware bracket depth, inline-table
// probing, and PEP 503 name normalization. parser_uv.cc owns the
// section walker; this file owns everything that looks at one line
// (or one inline table) at a time. Split from parser_uv.cc to keep
// each file under the 500-line cap.
//
// Lock-step with the walker: parser_uv.cc (canonical-layout section
// dispatch). Shared upstream pin: astral-sh/uv tag 0.12.1 —
// https://github.com/astral-sh/uv/blob/0.12.1/crates/uv-resolver/src/lock/deserialize.rs
// (see parser_uv.h for the full source-material register).

#ifndef SRC_SOCKETSECURITY_MANIFEST_PARSER_UV_SCAN_H_
#define SRC_SOCKETSECURITY_MANIFEST_PARSER_UV_SCAN_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <string_view>
#include <vector>

#include "manifest.h"

namespace node {
namespace socketsecurity {
namespace manifest {
namespace uvscan {

// Trim spaces/tabs (and a trailing '\r') from both ends.
std::string_view TrimAscii(std::string_view s);

// Index of the next '\n' at or after `from` (content.size() if none).
size_t NextLf(std::string_view content, size_t from);

// Strip one layer of surrounding double quotes.
std::string_view StripQuotes(std::string_view s);

// The key half of `key = value` (exact token, trimmed). Empty view
// when the line has no `=`.
std::string_view KeyOf(std::string_view line);

// The value half of `key = value`, trimmed, quotes kept.
std::string_view ValueAfterEquals(std::string_view line);

// Net `[` vs `]` depth change across one line, ignoring brackets
// inside double-quoted strings.
int NetBrackets(std::string_view line);

// PEP 503 name normalization → interned view. Lock-step with
// sdxgen's normalizePep503 (src/parsers/pypi/index.mts:288-290).
std::string_view NormalizePep503(std::string_view s, ParseContext* ctx);

// Look up `key = <string>` inside a one-line inline table `{ ... }`.
bool InlineTableString(std::string_view table, std::string_view key,
                       std::string_view* out_value);

// Dependency-array entry → PEP 503-normalized interned name.
std::string_view ExtractUvDepName(std::string_view entry, ParseContext* ctx);

// Split a single-line array on top-level commas and collect each
// entry's name via ExtractUvDepName.
void ParseInlineEntries(std::string_view value, ParseContext* ctx,
                        std::vector<std::string_view>* out);

}  // namespace uvscan
}  // namespace manifest
}  // namespace socketsecurity
}  // namespace node

#endif  // NODE_WANT_INTERNALS
#endif  // SRC_SOCKETSECURITY_MANIFEST_PARSER_UV_SCAN_H_

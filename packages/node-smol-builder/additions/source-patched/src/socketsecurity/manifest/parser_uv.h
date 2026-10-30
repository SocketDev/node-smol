// node:smol-manifest — uv.lock parser (Python / pypi).
//
// Handles the canonical TOML layout uv's lockfile serializer emits.
// Line-walk impl — no toml++ vendoring, and deliberately NO
// general-TOML fallback: uv's own canonical-format reader (PR
// astral-sh/uv#20648) falls back to a full TOML parse on unsupported
// syntax; this port instead fails loud with ERR_INVALID_LOCKFILE
// naming the first unrecognized construct, per repo doctrine
// (code-first-then-ai: a reconcile path that can't resolve exits
// loud, never false-green).
//
// Algorithm oracle (in lock-step order, newest → oldest):
//   1. socket-sdxgen/src/parsers/pypi/index.mts — parseUvLock +
//      normalizePep503, the TS public contract this impl matches
//   2. uv's canonical lockfile deserializer (pinned tag 0.12.1):
//        https://github.com/astral-sh/uv/blob/0.12.1/crates/uv-resolver/src/lock/deserialize.rs
//      introduced by https://github.com/astral-sh/uv/pull/20648
//   3. uv's lockfile writer — the source of truth for the format:
//        https://github.com/astral-sh/uv/blob/0.12.1/crates/uv-resolver/src/lock/mod.rs

#ifndef SRC_SOCKETSECURITY_MANIFEST_PARSER_UV_H_
#define SRC_SOCKETSECURITY_MANIFEST_PARSER_UV_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <string_view>

#include "manifest.h"

namespace node {
namespace socketsecurity {
namespace manifest {

bool ParseUvLock(std::string_view content,
                 ParseContext* ctx,
                 ParsedLockfile* out,
                 ParseError* err);

}  // namespace manifest
}  // namespace socketsecurity
}  // namespace node

#endif  // NODE_WANT_INTERNALS
#endif  // SRC_SOCKETSECURITY_MANIFEST_PARSER_UV_H_

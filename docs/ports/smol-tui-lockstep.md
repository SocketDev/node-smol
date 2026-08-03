# node:smol-tui lockstep tracker

## The contract

`node:smol-tui` is the C++ port of **stuie's Rust implementation**, not an
independent OpenTUI port. The chain has three links, and each has a pinned
oracle:

1. **Upstream**: `anomalyco/opentui` at `v0.4.5` (`0c8c4f7cff29`), whose Zig
   manifest pins its own layout engine at `facebook/yoga` `v3.2.1`
   (`042f5013152e`). stuie tracks both — sparse cone `packages/core/src` —
   and verifies the archive content-hashes in its `.gitmodules`.
2. **Reference implementation**: stuie's `crates/stuie-cabi` — a pure-Rust
   drop-in for OpenTUI's native `OptimizedBuffer` C ABI (198 `extern "C"`
   symbols — 193 in `cabi.rs` plus 5 in `audio_decoder.rs` — ~12.5k lines). It
   is proven against upstream's own
   `buffer.test.ts` through `conformance/shims/zig.mts`, and stuie's lockstep
   audit holds its file-forks at ok against the v0.4.5 pin.
3. **This port**: `packages/tui-infra` (C++ primitives: ANSI emit,
   cell-buffer diff render loop, Yoga direct binding, mouse parser) plus the
   binding glue at
   `packages/node-smol-builder/additions/source-patched/src/socketsecurity/tui/tui_binding.cc`,
   surfaced as the `node:smol-tui` builtin via
   `additions/source-patched/lib/smol-tui.js`.

## How to keep this lockstep

1. **stuie's Rust is the semantic reference.** When porting or reviewing a
   routine, compare against `stuie/crates/stuie-cabi/src/` — `buffer.rs`,
   `edit_buffer.rs`, `text_buffer.rs`, `handles.rs` — and cite the Rust
   `file:line` in a comment so audits have a fixed point. Do not port from
   OpenTUI's Zig directly: stuie already did that translation and carries the
   conformance proof for it.
2. **Version moves start upstream and flow down.** The trigger for a yoga
   bump is opentui bumping its own yoga pin in `build.zig.zon` — never yoga
   activity alone. The trigger for an opentui bump is a tagged upstream
   release; stuie moves first (its conformance suite revalidates
   `stuie-cabi`), then this port follows.
3. **The ABI shape is stuie-cabi's.** Symbol names, argument order, and the
   `u32`-handle convention match OpenTUI's FFI because stuie-cabi matches it;
   divergence here must be a deliberate, documented decision, not drift.
4. **audio_decoder is out of scope.** stuie-cabi carries a symphonia-backed
   mp3/flac decoder; `node:smol-tui` does not ship audio. If that changes it
   becomes its own builtin, not a tui-infra module.

## Snapshot

| Concern                                                      | Status                                                                                                               |
| ------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------- |
| Tier 1 — ANSI emit                                           | ported, live in `node:smol-tui`                                                                                      |
| Tier 2 — cell-buffer diff render loop                        | ported, live in `node:smol-tui`                                                                                      |
| Tier 3 — Yoga direct binding + mouse parser                  | ported, live in `node:smol-tui`                                                                                      |
| Higher-level surfaces (`react`, `keymap`, `qrcode`, `solid`) | planned as `node:smol-tui/<surface>` siblings                                                                        |
| Rust↔C++ symbol audit against stuie-cabi                     | enforced by `scripts/repo/check/smol-tui-cabi-symbols-are-mapped.mts` over `packages/tui-infra/cabi-symbol-map.json` |

That last row used to read "not yet run": `stuie-cabi` grew its 198-symbol
surface — 193 in `cabi.rs` plus 5 in `audio_decoder.rs` — and its conformance
proof after tui-infra's tiers were ported, so for a while the two lineages had
never been diffed symbol-by-symbol. They are now. Every `extern "C"` symbol is
enumerated into `packages/tui-infra/cabi-symbols.snapshot.json` and must carry
a row in `packages/tui-infra/cabi-symbol-map.json` that is either `ported`
with a counterpart file plus identifier the check verifies, `out-of-scope`
with a reason such as the audio rule above, or `pending` with the tier it
waits on, held under a `pendingCeiling` that only ratchets down. Refresh the
snapshot against a stuie checkout with `node
scripts/repo/check/smol-tui-cabi-symbols-are-mapped.mts --update`; the map
itself stays hand-curated, because a claim that a symbol is ported is a
judgement a human makes and the check only verifies.

// Kind-tagged u32 object handles.
//
// 1:1 port of stuie's crates/stuie-cabi/src/handles.rs. A handle's top 4
// bits encode its object kind, so a handle minted by one registry can never
// be mistaken for a live entry of a different kind — a wrong-kind handle
// just misses that registry's map. Each per-kind registry keeps its own
// monotonic index in the low 28 bits (never reused within a process run),
// so a stale handle also never aliases a fresh one. Together these give the
// "stale AND wrong-kind handles both resolve to a miss" contract that the
// text-buffer / text-view lifecycle suite relies on (handles.rs:1-15).
//
// The registries key their maps by the full tagged handle, so ordinary
// lookups need no explicit kind check — a wrong-kind handle just misses
// (handles.rs:13-14).

#ifndef TUI_INFRA_HANDLES_HPP_
#define TUI_INFRA_HANDLES_HPP_

#include <cstdint>

namespace tui {
namespace handles {

// Object-kind ordinals. Where they overlap the upstream Zig `ObjectKind`
// enum the values line up so a tagged handle packs like upstream's
// (handles.rs:18-25).
inline constexpr uint32_t kKindRenderer = 0;         // handles.rs:18
inline constexpr uint32_t kKindOptimizedBuffer = 1;  // handles.rs:19
inline constexpr uint32_t kKindTextBuffer = 2;       // handles.rs:20
inline constexpr uint32_t kKindTextBufferView = 3;   // handles.rs:21
inline constexpr uint32_t kKindEditBuffer = 4;       // handles.rs:22
inline constexpr uint32_t kKindEditorView = 5;       // handles.rs:23
inline constexpr uint32_t kKindSyntaxStyle = 6;      // handles.rs:24
inline constexpr uint32_t kKindAudioDecoder = 7;     // handles.rs:25

// Bits reserved for the per-kind monotonic index (the low 28 bits)
// (handles.rs:27-30).
inline constexpr uint32_t kIndexBits = 28;
inline constexpr uint32_t kIndexMask = (uint32_t{1} << kIndexBits) - 1;

// Fold a per-registry monotonic `index` (1-based, never 0) together with
// its `kind` nibble into an opaque handle. `index` is masked to 28 bits; a
// single registry would have to mint 2^28 (268M) objects in one process for
// this to wrap into the kind nibble, which no conformance run approaches
// (handles.rs:36-39 `tag`).
inline constexpr uint32_t Tag(uint32_t kind, uint32_t index) {
  return (kind << kIndexBits) | (index & kIndexMask);
}

}  // namespace handles
}  // namespace tui

#endif  // TUI_INFRA_HANDLES_HPP_

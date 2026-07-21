// smol-decmpfs — C++ inverse reader for a napi `--compress` hybrid.
//
// LOCK-STEP PORT of the canonical Rust reference
// `decmpfs::addon` (upstream/decmpfs/crates/decmpfs/src/addon.rs). The behavior
// must match the Rust byte-for-byte: same accept/reject, same decoded output,
// same size cap, same None conditions. Any divergence is a `divergence`
// finding, never a silent fixup.
//
// A "binpress hybrid" is a Mach-O / ELF / PE binary carrying the original
// `.node` addon zstd-compressed inside a PRESSED_DATA section (Mach-O
// `__PRESSED_DATA` in segment `SMOL`, ELF `.PRESSED_DATA`, PE `.PRESSED`),
// located via SECTION HEADERS — never an EOF scan.
//
// Pressed-data blob layout (little-endian):
//   [magic 32B "__SMOL_PRESSED_DATA_MAGIC_MARKER"]
//   [compressed   u64 LE]  zstd payload length
//   [uncompressed u64 LE]  raw addon length
//   [cache key   16B]      ignored
//   [platform     3B]      ignored
//   [integrity   64B]      SHA-512 of the zstd payload
//   [has_config   1B]      0/1
//   [config    1192B]      only if has_config == 1
//   [payload      ...]     zstd frame
#ifndef SMOL_DECMPFS_H
#define SMOL_DECMPFS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace smol_decmpfs {

// Fixed leading magic marker, 32 bytes.
inline constexpr char kMagicMarker[] = "__SMOL_PRESSED_DATA_MAGIC_MARKER";
inline constexpr size_t kMagicLen = 32;  // sizeof - 1 (no NUL)
inline constexpr size_t kSizeHeaderLen = 16;
inline constexpr size_t kCacheKeyLen = 16;
inline constexpr size_t kPlatformMetadataLen = 3;
inline constexpr size_t kIntegrityHashLen = 64;
inline constexpr size_t kConfigFlagLen = 1;
inline constexpr size_t kConfigBinaryLen = 1192;
// Fixed header length up to and including the has-config flag = 132.
inline constexpr size_t kHeaderLen = kMagicLen + kSizeHeaderLen + kCacheKeyLen +
                                     kPlatformMetadataLen + kIntegrityHashLen +
                                     kConfigFlagLen;
// DoS guard, matches bin-infra / the Rust reference: 512 MiB.
inline constexpr uint64_t kMaxDecompressed = 512ULL * 1024 * 1024;

// If `content` is a napi `--compress` hybrid, decode its embedded addon and
// return the raw `.node` bytes; otherwise std::nullopt. Integrity-checked
// (SHA-512 over the zstd payload) — a hybrid that fails verification returns
// nullopt, never partial bytes.
std::optional<std::vector<uint8_t>> unwrap_if_hybrid(const uint8_t* content,
                                                     size_t len);

// Parse the bin-infra pressed-data blob (magic + header + zstd payload) into
// the raw addon. Split from section-finding so the format round-trips without
// synthesizing a whole Mach-O/ELF/PE.
std::optional<std::vector<uint8_t>> decode_pressed_data(const uint8_t* section,
                                                        size_t len);

}  // namespace smol_decmpfs

#endif  // SMOL_DECMPFS_H

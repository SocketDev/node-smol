// Type declarations for the `node:smol-decmpfs` inverse reader addon.
// Hand-maintained in lockstep with the N-API surface in src/napi_addon.cpp,
// itself a lock-step port of the Rust `decmpfs::addon` reference.

/**
 * If `input` is a napi `--compress` hybrid (a Mach-O / ELF / PE binary carrying
 * the original addon zstd-compressed in its PRESSED_DATA section), decode and
 * return the raw `.node` bytes; otherwise `null`. Integrity-checked (SHA-512
 * over the zstd payload) — a hybrid that fails verification returns `null`,
 * never partial bytes.
 */
export function unwrapIfHybrid(input: Uint8Array): Buffer | null

/**
 * Decode a bare bin-infra pressed-data blob (magic + header + zstd payload)
 * into the raw addon, or `null` on any reject condition (bad magic, short
 * buffer, size-guard trip, SHA-512 mismatch, malformed zstd frame). Split from
 * {@link unwrapIfHybrid} so the blob format round-trips without a whole binary.
 */
export function decodePressedData(input: Uint8Array): Buffer | null

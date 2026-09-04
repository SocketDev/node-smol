/**
 * Compressed self-extracting binary format constants — split out of
 * constants.mts (soft file-size cap) since this block is self-contained.
 */

/**
 * Magic marker to identify the start of compressed data in self-extracting
 * binaries. The marker is 32 bytes long and must match EXACTLY with the C++
 * stub code.
 *
 * C++ equivalent (split to prevent self-reference):
 * MAGIC_MARKER_PART1 = "__SMOL"
 * MAGIC_MARKER_PART2 = "_PRESSED_DATA"
 * MAGIC_MARKER_PART3 = "_MAGIC_MARKER"
 * MAGIC_MARKER_LEN = 32.
 *
 * @type {string}
 */
export const SMOL_PRESSED_DATA_MAGIC_MARKER = '__SMOL_PRESSED_DATA_MAGIC_MARKER'

/**
 * Header field sizes in bytes, in on-disk order: magic marker, compressed
 * size, uncompressed size, cache key, platform metadata, integrity hash,
 * smol config flag, optional smol config binary, then the zstd-compressed
 * payload. The C++ stub decompressor reads these fields positionally, so
 * every size must stay in lock-step with compression_constants.h in
 * packages/bin-infra.
 *
 * Full layout: docs/agents.md/repo/compressed-binary-format.md.
 */
export const HEADER_SIZES = {
  CACHE_KEY: 16,
  COMPRESSED_SIZE: 8,
  // SHA-512 of compressed data. Mirrors INTEGRITY_HASH_LEN in
  // packages/bin-infra/src/socketsecurity/bin-infra/compression_constants.h.
  // Must stay in lock-step with the C side: the decompressor reads exactly
  // this many bytes after PLATFORM_METADATA and before SMOL_CONFIG_FLAG.
  INTEGRITY_HASH: 64,
  MAGIC_MARKER: 32,
  PLATFORM_METADATA: 3,
  SMOL_CONFIG_BINARY: 1192,
  SMOL_CONFIG_FLAG: 1,
  UNCOMPRESSED_SIZE: 8,
}

/**
 * Platform metadata byte values.
 */
export const PLATFORM_VALUES = {
  darwin: 1,
  linux: 0,
  win32: 2,
}

export const ARCH_VALUES = {
  arm: 3,
  arm64: 1,
  ia32: 2,
  x64: 0,
}

export const LIBC_VALUES = {
  glibc: 0,
  musl: 1,
  na: 255,
}

/**
 * Compression algorithm byte values.
 */
export const COMPRESSION_VALUES = {
  lzma: 1,
  lzms: 2,
  zstd: 0,
}

/**
 * Metadata header size (excluding magic marker, smol config, and compressed
 * data). compressed_size (8) + uncompressed_size (8) + cache_key (16) +
 * platform_metadata (3) + integrity_hash (64) + smol_config_flag (1) = 100
 * bytes.
 */
export const METADATA_HEADER_SIZE =
  HEADER_SIZES.COMPRESSED_SIZE +
  HEADER_SIZES.UNCOMPRESSED_SIZE +
  HEADER_SIZES.CACHE_KEY +
  HEADER_SIZES.PLATFORM_METADATA +
  HEADER_SIZES.INTEGRITY_HASH +
  HEADER_SIZES.SMOL_CONFIG_FLAG

/**
 * Total header size without smol config (excluding compressed data).
 * marker (32) + metadata (100) = 132 bytes.
 */
export const TOTAL_HEADER_SIZE_WITHOUT_SMOL_CONFIG =
  HEADER_SIZES.MAGIC_MARKER + METADATA_HEADER_SIZE

/**
 * Total header size with smol config (excluding compressed data).
 * marker (32) + metadata (100) + smol_config (1192) = 1324 bytes.
 */
export const TOTAL_HEADER_SIZE_WITH_SMOL_CONFIG =
  TOTAL_HEADER_SIZE_WITHOUT_SMOL_CONFIG + HEADER_SIZES.SMOL_CONFIG_BINARY

/**
 * Smol config binary size.
 */
export const SMOL_CONFIG_BINARY_SIZE = HEADER_SIZES.SMOL_CONFIG_BINARY

/**
 * Smol config magic number (ASCII "SMFG").
 */
export const SMOL_CONFIG_MAGIC = 0x53_4d_46_47

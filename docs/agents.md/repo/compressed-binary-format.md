# Compressed self-extracting binary format

The header layout for compressed self-extracting binaries. The TypeScript
constants live in
`packages/build-infra/lib/compressed-binary-format-constants.mts` and must
match the C side in
`packages/bin-infra/src/socketsecurity/bin-infra/compression_constants.h`
byte for byte, because the stub decompressor reads every field positionally.
Change one side and you must change the other in the same commit.

## Magic marker

The compressed region starts with a 32-byte magic marker,
`__SMOL_PRESSED_DATA_MAGIC_MARKER`. The C++ stub builds the marker from three
parts, `__SMOL` + `_PRESSED_DATA` + `_MAGIC_MARKER`, so the assembled string
never appears literally in the stub's own binary. Without the split, the stub
would find its own embedded string constant when scanning for the marker.

## Header layout

Fields appear in this on-disk order:

| Field              | Size in bytes | Contents                                             |
| ------------------ | ------------- | ---------------------------------------------------- |
| Magic marker       | 32            | `__SMOL_PRESSED_DATA_MAGIC_MARKER`                   |
| Compressed size    | 8             | uint64, little-endian                                |
| Uncompressed size  | 8             | uint64, little-endian                                |
| Cache key          | 16            | Hex string                                           |
| Platform metadata  | 3             | One byte each for platform, arch, libc; values below |
| Integrity hash     | 64            | SHA-512 of the compressed data                       |
| Smol config flag   | 1             | 0 means no config, 1 means config present            |
| Smol config binary | 1192          | Present only when the flag is 1; layout below        |
| Compressed data    | variable      | zstd stream                                          |

The decompressor reads the integrity hash directly after the platform metadata
and before the smol config flag, so `INTEGRITY_HASH` in `HEADER_SIZES` must
stay in lock-step with `INTEGRITY_HASH_LEN` on the C side.

The smol config binary is itself structured:

| Field       | Size in bytes | Contents                                                             |
| ----------- | ------------- | -------------------------------------------------------------------- |
| Magic       | 4             | `0x534D4647`, ASCII `SMFG`                                           |
| Version     | 2             | Currently 2                                                          |
| Config data | 1186          | Update config, fakeArgvEnv, and nodeVersion, validated at build time |

## Platform metadata values

| Platform byte | Value | Arch byte | Value | Libc byte                 | Value |
| ------------- | ----- | --------- | ----- | ------------------------- | ----- |
| linux         | 0     | x64       | 0     | glibc                     | 0     |
| darwin        | 1     | arm64     | 1     | musl                      | 1     |
| win32         | 2     | ia32      | 2     | not applicable, non-Linux | 255   |
|               |       | arm       | 3     |                           |       |

## Derived sizes

- Metadata header, everything between the magic marker and the optional smol
  config binary: 8 + 8 + 16 + 3 + 64 + 1 = 100 bytes.
- Total header without smol config: 32 + 100 = 132 bytes.
- Total header with smol config: 132 + 1192 = 1324 bytes.

## Compression algorithm

All platforms use zstd exclusively for new binaries. The
`COMPRESSION_VALUES` table still defines byte values for lzma and lzms so
older readers and tooling can identify legacy streams.

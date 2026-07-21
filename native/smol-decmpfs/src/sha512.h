// Minimal, self-contained SHA-512 (FIPS 180-4). Public-domain-style
// implementation vendored so the decmpfs inverse reader carries no crypto
// dependency: it needs exactly one hash (verify the 64-byte integrity field
// over a zstd payload), and pulling in OpenSSL/BoringSSL for that would violate
// node-smol's "prefer std, justify every dep" policy. This is not a
// general-purpose crypto library — it is the one primitive the reader needs.
#ifndef SMOL_DECMPFS_SHA512_H
#define SMOL_DECMPFS_SHA512_H

#include <cstddef>
#include <cstdint>

namespace smol_decmpfs {

// Compute SHA-512 of `data[0..len)` into the 64-byte `out` buffer.
void sha512(const uint8_t* data, size_t len, uint8_t out[64]);

}  // namespace smol_decmpfs

#endif  // SMOL_DECMPFS_SHA512_H

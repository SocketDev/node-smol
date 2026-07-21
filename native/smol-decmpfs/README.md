# smol-decmpfs — C++ inverse reader (`node:smol-decmpfs`)

A portable C++ **lock-step port** of the canonical Rust reference
`decmpfs::addon` (`upstream/decmpfs/crates/decmpfs/src/addon.rs`): unwrap a napi
`--compress` hybrid back to the raw `.node`. The port matches the Rust
byte-for-byte — same accept/reject, same decoded output, same 512 MiB cap, same
`None` conditions. Any divergence is a `divergence` finding, never a silent
fixup (see `scripts/differential.mts`).

## Surface

```ts
import { unwrapIfHybrid, decodePressedData } from 'node:smol-decmpfs'
unwrapIfHybrid(bytes: Uint8Array): Buffer | null      // whole Mach-O/ELF/PE
decodePressedData(bytes: Uint8Array): Buffer | null   // bare pressed-data blob
```

A graceful reject returns `null`; only a misuse (non-bytes argument) throws.

## Layout

| Path                       | Role                                                          |
| -------------------------- | ------------------------------------------------------------- |
| `include/smol_decmpfs.h`   | Reader API + format constants.                                |
| `src/smol_decmpfs.cpp`     | The port: Mach-O/ELF/PE section walk + pressed-data decode.   |
| `src/sha512.{h,cpp}`       | Vendored SHA-512 (the one integrity primitive; no crypto dep).|
| `src/napi_addon.cpp`       | N-API wrapper (C ABI `<node_api.h>`, no node-addon-api dep).  |
| `tool/decode_cli.cpp`      | Differential oracle CLI (test-only, not shipped).             |
| `binding.gyp`              | node-gyp build for the `.node`.                               |
| `fuzz/`                    | libFuzzer harness (see `fuzz/README.md`).                     |
| `scripts/`                 | `differential.mts`, `fuzz.mts`, `paths.mts`.                  |

## Dependency choices

- **zstd** — links the system/Homebrew **libzstd** (`/opt/homebrew/opt/zstd`,
  override with `SMOL_ZSTD_PREFIX`). The Rust reference uses the `zstd` crate
  (same upstream C library). A release matrix should vendor/pin libzstd per
  target; the local build links the static `libzstd.a`.
- **SHA-512** — a small vendored implementation (`src/sha512.cpp`), not a crypto
  library: the reader needs exactly one hash (verify the 64-byte integrity field
  over the zstd payload), so pulling in OpenSSL would violate node-smol's
  "prefer std, justify every dep" policy.

## Build

Canonical path (node-gyp):

```bash
node-gyp rebuild            # -> build/Release/smol_decmpfs.node
```

`node-gyp` is not installed in every environment. The equivalent direct build
(what node-gyp does under the hood — Node headers + `-undefined
dynamic_lookup`) produces the same loadable `.node`:

```bash
NODE_INC=$(node -p "path.join(path.dirname(path.dirname(process.execPath)),'include','node')")
ZSTD=${SMOL_ZSTD_PREFIX:-/opt/homebrew/opt/zstd}
xcrun clang++ -std=c++17 -O2 -fno-rtti -shared -undefined dynamic_lookup \
  -DNAPI_VERSION=8 -I"$NODE_INC" -Iinclude -Isrc -I"$ZSTD/include" \
  src/sha512.cpp src/smol_decmpfs.cpp src/napi_addon.cpp \
  "$ZSTD/lib/libzstd.a" -o build/Release/smol_decmpfs.node
```

## Verify

```bash
node scripts/differential.mts          # C++ reader vs the Rust golden (lock-step)
node scripts/differential.mts --update-golden   # regenerate golden from Rust
node scripts/fuzz.mts run read_hybrid --time 20 # ASan/UBSan/libFuzzer smoke
```

The differential proves the reader reproduces the Rust reference on every input
in the shared corpus; the golden (`fuzz/decmpfs/verdicts.golden.json`) is minted
from the Rust reference itself (a throwaway crate that path-depends on
`upstream/decmpfs/crates/decmpfs` with the `addon` feature).

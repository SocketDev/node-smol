# node-smol

node-smol is Socket's customized Node.js distribution. It owns the Node source
patches, builtins, SEA packaging, platform artifacts, and release assembly.

Reusable native capabilities remain Rust-canonical in their owner repositories.
`upstream/` holds shallow, pinned source references; adapters consume those
references through a `.node` addon or a narrow `node:smol-*` builtin contract.

## Upstream contracts

- `upstream/stuie` — terminal UI and ANSI mouse parsing. The Rust crate is the
  canonical implementation; future `node:smol-tui` integration must run its
  shared fixture corpus before a pin advances.
- `upstream/decmpfs` — transparent filesystem compression and executable
  packing. The existing Rust/N-API addon is the first consumer. Its precondition
  — a shared fuzz corpus both lanes read — now exists at `fuzz/decmpfs/`, so the
  C++ inverse reader is no longer out of scope: `native/smol-decmpfs/` is a
  lock-step port of the Rust `decmpfs::addon` (`unwrap_if_hybrid` /
  `decode_pressed_data`), wrapped as the `node:smol-decmpfs` addon. A
  differential check (`native/smol-decmpfs/scripts/differential.mts`) holds the
  two lanes byte-for-byte on the shared corpus, and a libFuzzer + ASan/UBSan
  harness (`native/smol-decmpfs/fuzz/`) seeds from it.

`upstream/contracts.mts` is the code-as-law record for those pins: Rust crate,
N-API addon crate, fixture corpus, ABI schema version, and supported target
triples. Verify it after initializing submodules and whenever an upstream pin
changes:

```sh
node scripts/repo/check-upstream-contracts.mts
```

## Development

```sh
git submodule update --init --depth 1
```

The first extraction wave moves the Node-specific builder and release surfaces
from socket-btm without changing public package names.

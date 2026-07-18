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

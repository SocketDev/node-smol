# node-smol

[![Follow @SocketSecurity](https://img.shields.io/twitter/follow/SocketSecurity?style=social)](https://twitter.com/SocketSecurity)
[![Follow @socket.dev on Bluesky](https://img.shields.io/badge/Follow-@socket.dev-1DA1F2?style=social&logo=bluesky)](https://bsky.app/profile/socket.dev)

Socket's customized Node.js distribution — source patches, builtins, SEA packaging, platform artifacts, and release assembly.

node-smol owns the Node source patches, builtins, SEA packaging, platform
artifacts, and release assembly for Socket's customized Node.js distribution.
Reusable native capabilities remain Rust-canonical in their owner repositories;
`upstream/` holds shallow, pinned source references, and adapters consume those
references through a `.node` addon or a narrow `node:smol-*` builtin contract.

## Install

**There are no releases yet** — this repo has published zero GitHub releases,
so there is nothing to download today. Once the first release is cut, binaries
will ship as GitHub release assets:

```sh
# Not functional yet: SocketDev/node-smol has no releases (checked 2026-07-28).
gh release download --repo SocketDev/node-smol
```

Until then, the build lane is `scripts/repo/build.mts`:

```sh
node scripts/repo/build.mts --dry-run          # print the exact build plan
node scripts/repo/build.mts                    # bake the compile-environment image
node scripts/repo/build.mts --target binary    # fails loud: the binary lane is not ported yet
```

Release assembly lives in `scripts/repo/release.mts` (dry-run by default;
`--publish` cuts a draft release via `gh release create --draft`).

## Usage

Run the downloaded binary in place of `node`:

```sh
./node-smol --version
```

### Upstream contracts

- `upstream/stuie` — terminal UI and ANSI mouse parsing. The Rust crate is the
  canonical implementation; future `node:smol-tui` integration must run its
  shared fixture corpus before a pin advances.

## Development

<details>
<summary>Contributor commands</summary>

```sh
git submodule update --init --depth 1
pnpm install
pnpm run check
pnpm run test
```

</details>

The first extraction wave moves the Node-specific builder and release surfaces
from socket-btm without changing public package names.

## License

MIT

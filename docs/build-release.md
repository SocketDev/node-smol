# Build and release

node-smol is configured to use GHCR for runtime binaries. It does not publish
binaries to npm or GitHub Releases. The current workflow uploads Actions
artifacts; the GHCR publisher and Socket CLI download path are not implemented.

## Build targets

The base target prepares the compile-environment image:

```sh
node scripts/repo/build.mts --dry-run
node scripts/repo/build.mts
```

The binary target builds the native runtime and runs its native checks:

```sh
pnpm run build:repo --target binary --prod
```

## Publish

GHCR is configured as the private binary channel in
`.config/repo/socket-wheelhouse.json`. Publishing is not available yet: the
workflow must publish and verify OCI artifacts, then Socket CLI must read them
before this channel is usable.

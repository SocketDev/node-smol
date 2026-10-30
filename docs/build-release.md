# Build and release

node-smol owns the Node source patches, builtins, single-executable packaging,
platform artifacts, and release assembly. The build lane is still in
development; no GitHub releases have been published. Do not treat the
download example in older material as an available artifact.

## Build targets

The base target prepares the compile-environment image and is the default:

```sh
node scripts/repo/build.mts --dry-run
node scripts/repo/build.mts
```

The binary target is not yet ported and currently fails with an explicit
diagnostic. A dry run reports that status without attempting a build:

```sh
node scripts/repo/build.mts --target binary --dry-run
```

The build planner can select platforms and push an image where configured; use
`--help` in the script for current flags. Build plans are printed before
execution, and the dry-run mode makes no changes.

## Release assembly

`scripts/repo/release.mts` assembles checksums and a draft GitHub release from
already-built assets. It defaults to dry-run. `--publish` invokes `gh release
create --draft`; it does not publish the release as a final public release.
Release assembly refuses to proceed when the asset directory is absent or
empty. The full platform binary lane and native checks remain prerequisites.

## Upstream source and development

The `upstream/` directory contains shallow, pinned source references. Initialize
them before repository checks or development work:

```sh
git submodule update --init --depth 1
pnpm install
pnpm run check
pnpm run test
```

The full platform binary lane and native checks remain prerequisites before
release assets can be assembled.

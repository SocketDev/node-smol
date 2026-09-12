# node-smol build: flags, compression strategy, size budget

Detail for `packages/node-smol-builder/scripts/common/shared/build.mts`, the
custom Node.js builder for Socket CLI distribution (smol builds) and Single
Executable Applications (SEA) with automatic Brotli compression.

## Directory structure

Fully isolated by mode + platform-arch, so concurrent builds don't collide:

- `build/shared/` - shared pristine artifacts (cloned source, extracted to
  dev/prod).
  - `build/shared/source/` - pristine Node.js source (archived in checkpoint).
  - `build/shared/checkpoints/` - source-cloned checkpoint (shared across
    dev/prod).
- `build/<mode>/<platform-arch>/` - build workspace for one mode on one
  target.
  - `build/<mode>/<platform-arch>/source/` - Node.js source (extracted from
    the shared checkpoint).
  - `build/<mode>/<platform-arch>/out/` - build outputs (Release, Stripped,
    Compressed, Final, …).
  - `build/<mode>/<platform-arch>/.cache/` - compiled binary cache +
    `cache-validation.hash`.
  - `build/<mode>/<platform-arch>/checkpoints/` - build checkpoints
    (source-patched, binary-released, …).

## Dual compression strategy

- **Layer 1 - SEA blob compression (Brotli on JavaScript).** Enabled by
  default during `--experimental-sea-config`. 70-80% size reduction
  (10-50MB → 2-10MB). Opt out with `"useCompression": false` in
  `sea-config.json`. Decompression: ~50-100ms at startup.
- **Layer 2 - binary compression (platform-specific on the whole binary).**
  Always enabled during build. 75-79% size reduction (27MB → 8-12MB).
  Decompression: ~100ms on first run, then cached.

## Build flags

- `--clean` - force clean build (ignore cache).
- `--prod` - production optimizations (V8 Lite, LTO).
- `--dev` - development mode (faster builds).
- `--with-dawn` - link against dawn-builder's `libwebgpu_dawn.a` (requires
  `pnpm --filter dawn-builder build` first; hard-fails if the artifact is
  missing).
- `--with-lief` - enable LIEF support (enables `--build-sea` flag, +5MB
  binary size).
- `--from-checkpoint=<name>` - skip to a specific build phase (resume from
  an existing artifact). Valid: `binary-released`, `binary-stripped`,
  `binary-compressed`, `finalized`.
- `--stop-at=<name>` - stop after a specific build phase (creates a
  checkpoint). Same valid set as above.
- `--build-only=<name>` - build to a stage but skip checkpoint creation
  (for Depot CI). Same valid set as above.

## Linker choice

Linux release links with **gold**, and that is a size decision rather than a
speed one. `001-common-gypi-lto.patch` passes `-fuse-ld=gold` with
`-Wl,--icf=safe`, so byte-identical functions fold into one copy. `safe` is the
mode that preserves function-pointer identity, which V8 depends on because it
compares code pointers.

Do not swap this linker without reading entry 1 of the
[build performance journal](../../perf/build-journal.md). mold was
evaluated and rejected there: its `--icf=safe` cannot fold under GCC, so the swap
trades binary size for link speed that the LTO link already masks. The entry
carries the reversal condition and the one measurement that would reopen it.

## Usage patterns

The binary build lives in the builder package, so it is invoked through that
package rather than from the root. Root `pnpm build` builds the fleet hook
bundle, not the binary.

```text
# Binary build. The package is named local-node-smol-builder, per the fleet's
# `local-<dir>` rule for a private package.
pnpm --filter local-node-smol-builder run build           # dev build
pnpm --filter local-node-smol-builder run build --prod    # production, LTO
pnpm --filter local-node-smol-builder run build --dev     # development mode
pnpm --filter local-node-smol-builder run clean           # drop the cache first

# Resume or stop at a phase (same valid set as the flags above).
pnpm --filter local-node-smol-builder run build --from-checkpoint=binary-stripped
pnpm --filter local-node-smol-builder run build --stop-at=binary-released

postject smol-binary NODE_SEA_BLOB app.blob               # smol + SEA
```

The usage flags beyond mode selection:

- `--clean` wipes the checkpoint cache before building, so a stale intermediate
  can never ride into a release. Without it, the build resumes from the newest
  checkpoint on disk.
- `--verify` runs the checksum verification pass over the downloaded Node
  source and the checkpoint chain before compiling, and fails the build on a
  mismatch rather than compiling bytes nobody checked.
- `--test` runs the binary's smoke tests after the link; `--test-full` runs
  the extended suite. A build without either flag stops at the Final copy.

## Binary size optimization strategy

<details><summary>Binary size optimization strategy</summary>

Starting size: ~49 MB (default Node.js v25 build).

**Stage 1 - configure flags (applied):**

- `--with-intl=small-icu`: ~44 MB (-5 MB, English-only ICU) - used.
- `--without-*` flags: ~27 MB (-22 MB, removes npm, amaro, etc.) - used.
- `--experimental-enable-pointer-compression`: reduced memory usage - used.

Additional options considered but not used:

- `--with-intl=none`: ~41 MB (-8 MB, no ICU, breaks Unicode).
- `--v8-lite-mode`: ~29 MB (-20 MB, disables JIT, 5-10x slower).

**Stage 2 - binary stripping** (platform-specific strip): ~25 MB (-24 MB,
removes debug symbols).

**Stage 3 - compression** (this script) + pkg Brotli (VFS): ~23 MB (-26 MB,
compresses Socket CLI code). Node.js `lib/` minify+Brotli: ~21 MB (-28 MB,
compresses built-in modules).

Target: ~21 MB (small-icu + full V8 JIT for performance).

**Size breakdown:**

- Node.js `lib/` (compressed): ~2.5 MB (minified + Brotli).
- Socket CLI (VFS): ~13 MB (pkg Brotli).
- Native code (V8, libuv): ~2.5 MB (stripped).

**Compression approach:**

1. Node.js built-in modules: esbuild minify → Brotli quality 11.
2. Socket CLI application: pkg automatic Brotli compression.

**Performance impact:**

- Startup overhead: ~50-100 ms (one-time decompression).
- Runtime performance: ~5-10x slower JS (V8 Lite mode).
- WASM performance: unaffected (Liftoff baseline compiler).

</details>

## Hierarchical source layout and cache-key paths

Detail for `packages/node-smol-builder/scripts/paths.mts`, which resolves
where the builder's scripts, patches, and additions live and which of those
directories feed each build phase's cache key.

### Priority order

Scripts, patches, and additions are organized by phase and then by
specificity. For a category and phase, `getHierarchicalPaths` returns three
candidate directories in priority order, from general to specific:

1. `shared` applies to all platforms.
2. `<platform>/shared` applies to every arch of one platform.
3. `<platform>/<arch>` applies to one specific platform and arch.

For example, `getHierarchicalPaths('scripts', 'stripped', 'darwin', 'arm64')`
returns:

```text
packages/node-smol-builder/scripts/stripped/shared
packages/node-smol-builder/scripts/stripped/darwin/shared
packages/node-smol-builder/scripts/stripped/darwin/arm64
```

All candidate paths are returned whether or not the directory exists on disk.
Non-existent paths are safe to pass to find and glob operations, which simply
skip them.

### Per-phase versus cumulative collection

`getBuildSourcePaths(phase, platform, arch)` returns the paths that affect
one build phase, for cache-key generation. Scripts are collected for the
current phase only. Patches and additions are cumulative: each phase includes
its own files plus the files of every phase before it, because a change to an
earlier phase's patches changes what every later phase builds on.

For example, `getBuildSourcePaths('stripped', 'darwin', 'arm64')` returns an
object where `common` holds the shared script paths, `scripts` holds only the
stripped-phase script paths, and `patches` and `additions` hold the
cumulative paths for release plus stripped.

`getCumulativeBuildSourcePaths` is the stricter variant used for cache
validation. It collects scripts cumulatively too, so the resulting hash
covers every file from the current phase and all previous phases.

`getCumulativeHierarchicalPaths(category, phases, platform, arch)` is the
underlying helper for the cumulative cases. For example, calling it with
`'patches'` and phases `['release', 'stripped']` on linux-x64 returns:

```text
patches/release/shared
patches/release/linux/shared
patches/release/linux/x64
patches/stripped/shared
patches/stripped/linux/shared
patches/stripped/linux/x64
```

### Filtering to existing paths

`getExistingPaths(paths)` filters a candidate list down to the directories
that actually exist. Local build operations need this because they walk paths
with `readdirSync` and `statSync`, which throw on non-existent directories.
CI workflows do not need it, because `find` handles missing directories
gracefully. For example, filtering the stripped-phase script candidates on a
macOS checkout typically leaves only
`packages/node-smol-builder/scripts/stripped/darwin/shared`.

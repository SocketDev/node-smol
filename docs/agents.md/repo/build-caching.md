# Build caching: cache keys, checkpoints, extraction cache

Detail for the caching layers in `packages/build-infra/lib/` — `cache-key.mts`,
`checkpoint-manager.mts`, and `extraction-cache.mts`. The short comments in
those files carry the invariants; this page carries the discussion.

## Cache keys

`generateCacheKey` in `cache-key.mts` builds the cache directory name in the
format `v{nodeVersion}-{platform}-{arch}-{contentHash}-{pkgVersion}`, for
example `v24.12.0-darwin-arm64-b71671ba-2.1.5`.

Platform and arch should be specified explicitly for cross-compilation builds.
They default to the current system values, and a host default silently mis-tags
a cache entry that was actually built for another target.

Content files are hashed by content only, never by mtime. Git operations such
as squash and rebase rewrite mtimes, so an mtime-based key would miss the cache
even when the file content is identical. A missing content file contributes its
filename to the hash, because deletion is an intentional state that should
produce a different key. Any other read error fails the build instead of being
hashed around.

Each package also has cache-busting dependencies. When one of these packages
updates, the cache key changes and caches rebuild:

| Package         | Cache-busting dependencies                                                                                |
| --------------- | --------------------------------------------------------------------------------------------------------- |
| bootstrap       | `@socketsecurity/lib`, `@socketsecurity/packageurl-js`                                                    |
| cli             | `@socketsecurity/lib`, `@socketsecurity/packageurl-js`, `@socketsecurity/sdk`, `@socketsecurity/registry` |
| cli-with-sentry | `@socketsecurity/lib`, `@socketsecurity/packageurl-js`                                                    |

## Workflow checkpoints

`checkpoint-manager.mts` saves and restores build state so GitHub Actions
workflows can cache each build phase and resume interrupted builds.

### Creating checkpoints

`createCheckpoint` enforces the pattern build → smoke test → checkpoint. The
`smokeTest` callback is a required parameter, so a checkpoint can only be
written after the build artifacts have been validated. A checkpoint created
from unvalidated artifacts would poison every later run that restores it.

Storage layout:

- Metadata JSON: `build/{mode}/checkpoints/{package}/{phase}.json`
- Artifact tarball: `build/{mode}/checkpoints/{package}/{phase}.tar.gz`,
  written only when `artifactPath` is provided.

Cache invalidation works per checkpoint. When `sourcePaths` is provided, the
function computes a source hash and stores it in the checkpoint metadata. The
next build compares the current hash against the stored one to decide whether
the stage needs a rebuild. Each checkpoint tracks its own source dependencies
independently, so invalidating one stage does not invalidate its siblings.

Artifact storage works for a single file or a whole directory: the path is
archived into `checkpoint.tar.gz`, the metadata records `artifactPath`, and
`restoreCheckpoint` extracts the tarball back to that location. The
`tarExcludes` option takes tar glob patterns interpreted relative to the
archive root, useful for dropping build outputs a previous run left inside the
artifact directory.

Binary-stage checkpoints must pass the target `platform` and `arch` explicitly.
The host-platform fallback was removed because it silently mis-tagged cache
entries under cross-compilation, where a darwin host builds a linux target.
Source-stage checkpoints listed in `PLATFORM_AGNOSTIC_CHECKPOINTS` skip
platform metadata entirely. On macOS, a provided `binaryPath` is ad-hoc signed
before the smoke test runs, because an unsigned binary cannot execute.

Example calls:

```js
// Minimal usage.
await createCheckpoint(BUILD_DIR, CHECKPOINTS.FINALIZED, async () => {
  await spawn(binaryPath, ['--version'])
})

// With options.
await createCheckpoint(
  BUILD_DIR,
  CHECKPOINTS.BINARY_RELEASED,
  async () => {
    await spawn(binaryPath, ['--version'])
  },
  {
    artifactPath: './out/Release/node',
    sourcePaths: ['build.mts', 'patches/*.patch'],
    packageRoot: PACKAGE_ROOT,
    platform: 'linux',
    arch: 'x64',
    libc: 'glibc',
  },
)
```

Option reference for `createCheckpoint`:

| Option                       | Meaning                                                                     |
| ---------------------------- | --------------------------------------------------------------------------- |
| `packageName`                | Package name; defaults to an empty string for a flat checkpoint layout.     |
| `artifactPath`               | File or directory to archive in the checkpoint tarball.                     |
| `sourcePaths`                | Source paths hashed for cache validation.                                   |
| `packageRoot`                | Package root used for relative path display in logs.                        |
| `platform` / `arch` / `libc` | Target triple; required for binary stages, ignored for source stages.       |
| `binaryPath`                 | Binary to ad-hoc sign on macOS before the smoke test.                       |
| `tarExcludes`                | Tar glob patterns, relative to the archive root, excluded from the tarball. |

### Restoring checkpoints

`restoreCheckpoint` extracts the tarball back to the `artifactPath` recorded in
the checkpoint metadata, or to `options.destDir` when that override is set. It
deletes the existing target file or directory before extraction, which
guarantees a clean state and prevents a corrupt mix of old and new files left
behind by a previous failed build.

Validation is strict. When the caller passes an expected `platform`, `arch`, or
`libc` and the checkpoint metadata disagrees, the function throws instead of
warning, to prevent cross-target checkpoint pollution. Linux restores of
binary-stage checkpoints additionally require `libc` on both sides: the caller
must pass `glibc` or `musl`, and a Linux checkpoint without libc metadata is
refused. Source-stage checkpoints in `PLATFORM_AGNOSTIC_CHECKPOINTS` skip all
of this validation, matching what `createCheckpoint` records.

The return value is a boolean. It is `false` when the checkpoint does not exist
or fails an integrity check, such as an empty or oversized tarball. Mismatched
target metadata throws rather than returning `false`, because that case is a
caller bug, not a cache miss.

## Source hashing for extraction caches

`computeSourceHash` in `extraction-cache.mts` matches the GitHub Actions cache
key algorithm so local and CI cache keys agree:

1. Hash each file individually with SHA256.
2. Join the hex digests with newlines.
3. Hash the combined result with SHA256.

Files are sorted before hashing so the result is stable regardless of input
order. Directory paths are expanded recursively to the files inside them.
Symlinks are skipped to avoid loops and traversal issues, and a missing path
contributes a sentinel so deletion changes the hash. The optional
`platformMetadata` string, for example `linux-x64-glibc`, is mixed into the
hash so the same sources produce different keys per target.

The `options.relativeTo` switch selects which path form goes into the hash:

- Omitted: each path is hashed as-is, absolute. Use this for source-cache
  hashes where rename detection across source roots matters. Moving `foo.ts`
  between two source roots with unchanged content must change the hash.
- Provided: each path is hashed relative to `relativeTo`. Use this for
  artifact-integrity hashes, where the tarball is extracted to a temp
  directory at creation time and a different final directory at restore time.
  The hash must stay stable across those absolute-path changes, so only file
  content and intra-artifact layout contribute.

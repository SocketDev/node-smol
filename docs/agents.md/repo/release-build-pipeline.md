# Release build pipeline

Detail for the binary-released phase of node-smol-builder: the `buildRelease`
entry point, how monorepo sources are wired into the patched Node.js tree,
where the compression tools come from, and how binject is driven in tests.

## buildRelease config reference

<details><summary>buildRelease config reference</summary>

`packages/node-smol-builder/scripts/binary-released/shared/build-released.mts`
exports `buildRelease(config, buildOptions)`. The `config` object carries the
whole build context for the phase. Required properties are validated at
runtime by the `requiredProps` check at the top of the function, so a missing
property fails fast with a named error instead of surfacing later as an
undefined path.

| Field                     | Meaning                                                                                                                                                                                                                                                                                                           |
| ------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `nodeVersion`             | Node.js version to build.                                                                                                                                                                                                                                                                                         |
| `nodeSha`                 | Node.js commit SHA.                                                                                                                                                                                                                                                                                               |
| `nodeRepo`                | Node.js repository URL.                                                                                                                                                                                                                                                                                           |
| `buildDir`                | Build directory.                                                                                                                                                                                                                                                                                                  |
| `packageName`             | Package name.                                                                                                                                                                                                                                                                                                     |
| `packageRoot`             | Package root directory.                                                                                                                                                                                                                                                                                           |
| `sharedBuildDir`          | Shared build directory.                                                                                                                                                                                                                                                                                           |
| `sharedSourceDir`         | Shared source directory.                                                                                                                                                                                                                                                                                          |
| `modeSourceDir`           | Mode-specific source directory.                                                                                                                                                                                                                                                                                   |
| `buildPatchesDir`         | Build patches directory.                                                                                                                                                                                                                                                                                          |
| `outDir`                  | Build out directory.                                                                                                                                                                                                                                                                                              |
| `nodeBinary`              | Node binary path.                                                                                                                                                                                                                                                                                                 |
| `outputReleaseDir`        | Release output directory.                                                                                                                                                                                                                                                                                         |
| `outputReleaseBinary`     | Release binary path.                                                                                                                                                                                                                                                                                              |
| `cacheDir`                | Cache directory.                                                                                                                                                                                                                                                                                                  |
| `testFile`                | Test file path used to validate the source tree.                                                                                                                                                                                                                                                                  |
| `bootstrapFile`           | Bootstrap file path used to validate the source tree.                                                                                                                                                                                                                                                             |
| `patchedFile`             | Patched file path used to validate patch application.                                                                                                                                                                                                                                                             |
| `platform`                | Target platform.                                                                                                                                                                                                                                                                                                  |
| `arch`                    | Target architecture.                                                                                                                                                                                                                                                                                              |
| `libc`                    | Target libc variant, such as `musl`.                                                                                                                                                                                                                                                                              |
| `buildMode`               | Build mode, `dev` or `prod`.                                                                                                                                                                                                                                                                                      |
| `cleanBuild`              | Whether this is a clean build.                                                                                                                                                                                                                                                                                    |
| `autoYes`                 | Auto-yes to prompts.                                                                                                                                                                                                                                                                                              |
| `isCI`                    | Whether the build is running in CI.                                                                                                                                                                                                                                                                               |
| `isProdBuild`             | Whether this is a production build.                                                                                                                                                                                                                                                                               |
| `allowCross`              | Allow cross-compilation. Experimental.                                                                                                                                                                                                                                                                            |
| `withDawn`                | Link against dawn-builder's prebuilt WebGPU library.                                                                                                                                                                                                                                                              |
| `withLief`                | Enable LIEF support.                                                                                                                                                                                                                                                                                              |
| `collectBuildSourceFiles` | Function that collects build source files for cache validation.                                                                                                                                                                                                                                                   |
| `extraConfigureFlags`     | Extra `./configure` flags appended verbatim after the standard set. The bundle-driven compile in detect-bundle-features.mts and compile-for-bundle.mts passes the detector's `--without-smol-*` and `--without-sqlite` flags here to drop subsystems a given SEA bundle does not use. Optional, defaults to none. |

`buildOptions.skipCheckpoint` skips checkpoint creation after the build.

</details>

## Monorepo-package source mappings

`packages/node-smol-builder/scripts/binary-released/shared/prepare-external-sources.mts`
exports `MONOREPO_PACKAGE_SOURCES`, the manifest of monorepo packages whose
sources are copied into the patched Node.js tree.

The invariant that matters is that `from` is the cache-key authority.
apply-patches.mts imports this list and feeds each `from` directory to
`computeSourceHash` for the SOURCE_PATCHED cache key. `relativeTo` is purely a
copy-routing detail. It tells `copyBuildAdditions` where to land the tree
under `modeSourceDir`, and it has no role in cache invalidation. Editing the
upstream package contents invalidates SOURCE_PATCHED automatically. Changing
`relativeTo` does not, and it should not, because that is a path rewrite
rather than a content change.

The flow is direct: each `from` directory in the manifest is copied by
`copyBuildAdditions` to `modeSourceDir/<relativeTo>`, with no intermediate
stop in `additions/`. Adding a new package to this manifest is the only edit
needed to wire it into both the cache and the source tree.

Hand-maintained sources under `additions/source-patched/src/socketsecurity/`,
such as sea-smol, vfs, ffi, and http, are deliberately not in this list. They
live only in `additions/` as authoritative sources and are picked up by
`copyBuildAdditions`' directory walk over `ADDITIONS_SOURCE_PATCHED_DIR`.

## Compression tool sourcing

`packages/node-smol-builder/scripts/common/shared/compression-tools.mts`
resolves the binpress compression tool, downloading it when missing. Note
that binflate, the decompressor, is no longer downloaded here because the
self-extracting stub has built-in decompression.

Three environment variables switch tool sourcing from release downloads to
local source builds:

- `BUILD_TOOLS_FROM_SOURCE=true` builds the binsuite tools binpress,
  binflate, and binject from source instead of downloading them from
  releases. Docker builds use this to test local changes.
- `BUILD_DEPS_FROM_SOURCE=true` builds LIEF from source during binsuite
  builds. LIEF must be pre-installed on the system. This part is implemented
  in the CMake build scripts.
- `BUILD_ALL_FROM_SOURCE=true` is a shortcut that enables both of the above.

Local builds under `packages/binpress/build/*/<platformArch>/out/Final/` are
preferred whenever they exist and the runtime platform matches the target
platform. The platform check prevents using macOS-built binaries when running
inside Linux Docker.

## binject in tests

`packages/node-smol-builder/test/helpers/binject.mts` wraps binject CLI calls
for tests. `runBinject` resolves the binject binary from the binject
package's build output, makes the target binary path absolute, and keeps the
resource path relative to the test directory.

binject automatically handles compressed self-extracting stubs. It detects
the stub's cache key and uses the extracted binary from a test-specific cache
directory, which keeps tests isolated from each other. The resource name can
be `NODE_SEA_BLOB`, a VFS resource name from
`bin-infra/test/helpers/segment-names`, or `BOTH` for dual SEA plus VFS
injection, in which case the resource path is an object with `sea` and `vfs`
keys.

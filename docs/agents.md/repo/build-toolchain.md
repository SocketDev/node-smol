# Build toolchain: tools, checksums, tarballs, submodule versions

Detail for the toolchain helpers in `packages/build-infra/lib/` -
`tool-installer.mts`, `node-checksum.mts`, `submodule-version.mts`,
`tarball-utils.mts`, and `model-build-helpers.mts`. The short comments in
those files carry the invariants; this page carries the discussion.

## Tool installation

`ensureToolInstalled` in `tool-installer.mts` checks one tool and installs it
when missing. Tools fall into two categories:

- Pinned tools, category `pinned` in `external-tools.json`, require an exact
  version. They are auto-downloaded with checksum verification and cached. A
  tool counts as pinned when its config carries a version and a matching
  `tool-checksums/<tool>-<version>.json` artifact exists.
- All other tools accept any recent version and install through the platform
  package manager, for example brew, apt, or choco.

For non-pinned tools the flow is: find the binary on PATH, then verify it
actually runs, which catches binaries with broken dependencies. When a tool
exists but fails to run and its config lists dependencies, each dependency is
installed first and the verification is retried before giving up.

The resolved result is a plain object with `available` and `installed`
booleans plus optional `path`, `packageManager`, and `error` fields. Callers
branch on `available` and surface `error` when it is set.

`ensureAllToolsInstalled` runs the same check over a list, sequentially on
purpose so package manager invocations never race each other.

## Node.js release checksums

<details><summary>Node.js release checksums</summary>

`node-checksum.mts` ties the Node.js source submodule to an authentic upstream
release.

`fetchNodeChecksum` downloads `SHASUMS256.txt` from the official Node.js
distribution and extracts the checksum for `node-vX.Y.Z.tar.gz`. It returns
the digest as lowercase hex, which is the format both consumers want: the
update-node skill writes `sha256:<hex>` into `.gitmodules` during version
updates, and `verifyNodeChecksum` compares hex against hex. On failure it
returns an object with an `error` string instead of throwing, so callers can
report and decide.

```js
const result = await fetchNodeChecksum('1.2.3')
if ('hash' in result) {
  // Write to .gitmodules: # node-1.2.3 sha256:<result.hash>
}
```

`verifyNodeChecksum` fetches the official checksum and compares it against the
checksum stored in `.gitmodules` for the node submodule. The version defaults
to the repo's `.node-version`. Like the fetch helper it reports failure
through its return value, `valid: false` plus an `error` message, rather than
throwing, so the caller decides how hard to fail.

```js
const result = await verifyNodeChecksum()
if (!result.valid) {
  throw new Error(`Checksum mismatch: ${result.expected} !== ${result.actual}`)
}
```

Both helpers accept a `timeout` option in milliseconds, defaulting to 10
seconds.

</details>

## Submodule version comments

`.gitmodules` carries a version comment directly above each submodule section
in the format `# <package>-<version>`, optionally followed by a checksum as
`<algorithm>:<hash>` on the same line. The version may be semver or any other
scheme the upstream uses. Other annotation comment lines, for example
`# full-checkout: …`, may sit between the version comment and the
`[submodule "<path>"]` header; the parsers tolerate any contiguous comment
block there.

`getSubmoduleVersion` in `submodule-version.mts` extracts the version string
and throws when the comment is missing or malformed, since a missing pin is a
repo-integrity problem, not a soft miss.

```js
const version = getSubmoduleVersion(
  'packages/lief-builder/upstream/lief',
  'lief',
)
// Returns: '0.17.0'
```

`getSubmoduleChecksum` reads the optional `<algorithm>:<hash>` pair from the
same comment line and returns `undefined` when no checksum is recorded.

## Tarball extraction

`extractTarball` in `tarball-utils.mts` extracts with system tar after
validating the archive. `validateTarballPaths` lists the archive and rejects
null bytes, absolute paths, and parent-directory traversal, including the
Windows edge case where `path.isAbsolute('/etc/passwd')` is false but some tar
implementations still write the entry from the extraction root. On platforms
whose tar supports it, `--no-absolute-names` is added as defense in depth, and
`--overwrite` is added on GNU tar to avoid ENOTEMPTY when re-extracting over
an existing directory.

Options:

| Option            | Default    | Meaning                                       |
| ----------------- | ---------- | --------------------------------------------- |
| `validate`        | `true`     | Pre-scan archive paths before extraction.     |
| `createDir`       | `true`     | Create the extraction directory when missing. |
| `stdio`           | `'ignore'` | Stdio mode for the tar child process.         |
| `stripComponents` | `0`        | Leading path components to strip.             |
| `overwrite`       | `true`     | Overwrite existing files and directories.     |

The resolved value is the list of file paths reported by the validation pass.
When `validate` is `false` the list is empty.

## Model build preflight

`checkModelBuildPrerequisites` in `model-build-helpers.mts` centralizes the
prerequisite checks shared by the ML model builders: minilm-builder,
codet5-models-builder, and the models package. In order it frees disk space in
CI environments, checks available disk space against `requiredDiskGB`,
installs and verifies Python 3 at the fleet minimum version, loads the tool
config from `external-tools.json` with extends support, and installs the
Python packages that config requires. It throws when Python or a required
package cannot be provided, and returns the loaded `externalTools` config
together with the extracted `pythonPackages` list for the caller to reuse.

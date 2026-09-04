# uv.lock — canonical-layout slice (headroom corpus)

## Input

`input.uv.lock` is a trimmed slice of the repo's own headroom pin lockfile
(`.claude/hooks/fleet/setup-security-tools/headroom/uv.lock`, uv canonical
layout, `version = 1` / `revision = 3`). Trimming kept one representative of
every construct the native walker handles:

- top-level scalars + the multi-line `resolution-markers` string array
- `[options]` with `exclude-newer`, and `[options.exclude-newer-package]`
- `[manifest]` with an `overrides` inline-table array
- `[[package]]` rows: registry source, multi-line `dependencies` inline-table
  arrays (with `marker` keys), `sdist` inline table, `wheels` arrays
- `[package.optional-dependencies]` group tables, including entries that pin
  `version`/`source` per marker (the duplicated `onnxruntime` pair)
- a `source = { virtual = "." }` root package with `[package.metadata]` +
  `requires-dist`

## Expected behavior

`canonical-headroom-slice.golden.json` is the exact
`parseLockfile(content, 'pypi', 'uv')` output of the smol binding
(`JSON.stringify` equality in test/smol-manifest-binding-live.mjs):

- Six rows, file order; names PEP 503-normalized; `_index` maps each name to
  its row.
- `integrity` carries the sdist `hash` verbatim (`sha256:` prefix kept).
- `h2` and `httpx` are `depType: "optional"` / `isOptional: true` — each is
  referenced by a `[package.optional-dependencies]` group (httpx by
  headroom-ai's `proxy` group, h2 by httpx's own `http2` group).
- The virtual root row has no `resolved` (local source kinds carry no
  resolution URL).

## Reference

C++ walker: `additions/source-patched/src/socketsecurity/manifest/parser_uv.cc`
(+ `parser_uv_scan.cc`), lock-step with sdxgen's `parseUvLock` and uv's
canonical deserializer pinned at tag 0.12.1
(<https://github.com/astral-sh/uv/blob/0.12.1/crates/uv-resolver/src/lock/deserialize.rs>,
introduced by <https://github.com/astral-sh/uv/pull/20648>).

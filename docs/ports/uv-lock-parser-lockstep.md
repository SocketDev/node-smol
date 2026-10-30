# uv.lock parser lockstep tracker

## The contract

`parser_uv.cc` in `node:smol-manifest` is a canonical-format C++ reader for
`uv.lock`, modeled on **astral-sh/uv pull request 20648** ("Parse uv
lockfiles directly"): uv writes lockfiles in a predictable canonical shape,
and this parser reads that shape with a single line walk. One deliberate
deviation from the upstream design: uv's own reader falls back to a general
TOML parser on non-canonical syntax; this parser has NO fallback, per the
repo's no-TOML-library doctrine, and fails loud with `ERR_INVALID_LOCKFILE`
naming the first unrecognized construct.

## Pins

1. **Upstream**: `astral-sh/uv` at `0.12.1`
   (`329541a503de8a4d9bb021814f9c0875efe033c8`), the first release carrying
   the direct-parse deserializer. Reference cone in `.gitmodules` as
   `upstream/uv`, sparse to `crates/uv-resolver/src/lock/`; the oracle file
   is `crates/uv-resolver/src/lock/deserialize.rs`.
2. **Name normalization**: PEP 503, in lockstep with sdxgen's
   `normalizePep503` (`socket-sdxgen src/parsers/pypi`).

## Re-review procedure on a pin bump

Fetch the new uv tag, diff `deserialize.rs` and the lock schema
(`version`/`revision` handling, new table forms), extend the walker for any
new canonical shape, regenerate the golden fixture with the standalone
harness, and re-run the two in-repo corpora plus the fixture registers.
Advance the `.gitmodules` ref via `gen/gitmodules-hash.mts --set` and this
tracker's pin line in the same change.

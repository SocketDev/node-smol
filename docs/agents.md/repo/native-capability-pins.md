# Native-capability pins

Reusable native capabilities stay Rust-canonical in their owner repos.
`upstream/<name>` holds shallow single-branch source references, and adapters
consume them through a `.node` addon or a narrow `node:smol-*` builtin
contract.

## The actual pin shape today

`.gitmodules` carries `branch = main` plus `shallow = true` only for the stuie
reference: stuie publishes no release tags, so there is no `ref` and no
`sha256:` yet. Do not describe those pins as content-addressed — they are not,
and a doc that says otherwise teaches the wrong trust model.

## The intended contract

Adopt the `ref` + `sha256:` pin shape, set via `gen/gitmodules-hash --set`,
once stuie ships a taggable release. Every other upstream in `.gitmodules`
already carries that shape; the stuie row is the one deliberate exception, and
this page is its tracking note.

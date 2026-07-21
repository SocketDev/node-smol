# decmpfs fuzzing — shared corpus (lock-step contract)

This directory is the **shared decmpfs fuzz corpus** every lane reads. It mirrors
the acorn shared-substrate philosophy (`packages/acorn/fuzz/README.md`): the
corpus, the dictionary, and the target/classification vocabulary are defined
ONCE and both lanes conform to them. Fuzzing is lock-step, exactly like the
readers are.

Rust is the canonical lane and the source of truth. The C++ inverse reader
(`native/smol-decmpfs/`) is a byte-for-byte port of the Rust
`decmpfs::addon::unwrap_if_hybrid` / `decode_pressed_data`, and its libFuzzer
harness seeds from THIS corpus. Any accept-here / reject-there split between the
two lanes on a shared input is a `divergence` finding, not a silent fixup.

## Provenance

Seeded verbatim (byte-identical) from the canonical Rust lane's committed corpus
at `decmpfs/fuzz/corpus/` (the crate pinned by `upstream/contracts.mts`). Do not
mutate an existing seed in place — a seed is a frozen input. Add new seeds as new
files. Re-sync from the Rust lane by copying; the two dirs must diff clean.

## Layout

| Path                             | Role                                                            |
| -------------------------------- | --------------------------------------------------------------- |
| `corpus/<target>/`               | Committed, curated seed inputs — one raw input per file.        |
| `decmpfs.dict`                   | libFuzzer `-dict` tokens (blob framing + container magics).     |

## Targets (the shape of an input)

Two canonical targets. Both lanes implement both; the wire input is **raw,
attacker-controllable bytes** in each case.

| Corpus dir (canonical) | Rust target          | C++ harness target | Input bytes are…                                            |
| ---------------------- | -------------------- | ------------------ | ----------------------------------------------------------- |
| `unwrap_if_hybrid`     | `unwrap_if_hybrid`   | `read_hybrid`      | a whole Mach-O / ELF / PE binary → the section walk + decode |
| `decode_pressed_data`  | `decode_pressed_data`| `decode_pressed`   | a bare pressed-data blob → the header parse + zstd decode    |

The corpus directory keeps the **canonical Rust target name** so the two lanes'
corpora diff clean; the C++ harness executables use the shorter `read_hybrid` /
`decode_pressed` names and map onto these dirs 1:1 (see
`native/smol-decmpfs/fuzz/README.md`).

## Classification

A single-lane fuzzer classifies each input as `ok` (a value was returned — for
this reader `Some`/`None` are BOTH `ok`, a graceful reject is a non-finding) or
one of the buggy outcomes `crash` / `hang` / `oom`. A graceful `None`
(bad magic, short buffer, size-guard trip, SHA-512 mismatch, malformed zstd
frame) is explicitly a NON-finding — the reader's whole job is to reject
untrusted bytes without crashing. `divergence` is emitted only by the
differential check (`native/smol-decmpfs/scripts/differential.mts`), which runs
BOTH lanes over this corpus and compares accept/reject + decoded output.

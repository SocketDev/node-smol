# smol-decmpfs C++ libFuzzer + ASan/UBSan harness

Lock-step port of the Rust reference fuzz lane (`decmpfs/fuzz/`). The two
targets mirror the Rust targets and seed from the SHARED corpus at
`fuzz/decmpfs/corpus/` — read the cross-lane contract there first:
[`fuzz/decmpfs/README.md`](../../../fuzz/decmpfs/README.md).

| C++ target       | Rust target           | Shared corpus dir                        | Under test                             |
| ---------------- | --------------------- | ---------------------------------------- | -------------------------------------- |
| `read_hybrid`    | `unwrap_if_hybrid`    | `fuzz/decmpfs/corpus/unwrap_if_hybrid`   | the Mach-O/ELF/PE section walk         |
| `decode_pressed` | `decode_pressed_data` | `fuzz/decmpfs/corpus/decode_pressed_data`| the pressed-data blob parse + zstd     |

## Toolchain requirement

Apple's Xcode clang carries NO libFuzzer runtime (`-fsanitize=fuzzer` fails at
link). The runner probes for a full-LLVM clang++ by compiling + linking a
trivial `LLVMFuzzerTestOneInput` TU and uses the first that works:

```bash
brew install llvm          # macOS
apt install clang lld      # Linux
```

On macOS the full-LLVM clang needs the SDK sysroot passed explicitly; the runner
resolves it via `xcrun --show-sdk-path`. Set `SMOL_FUZZER_CXX=/path/to/clang++`
to override compiler detection. When none links, the runner prints
`smoke-blocked: libFuzzer-capable clang++ absent` with the install command and
exits non-zero — never a fabricated pass.

## Run

```bash
# from native/smol-decmpfs:
node scripts/fuzz.mts list
node scripts/fuzz.mts build read_hybrid
node scripts/fuzz.mts run   read_hybrid --time 20
node scripts/fuzz.mts run   decode_pressed --time 20
```

Built `-fsanitize=address,fuzzer,undefined -fno-sanitize-recover=undefined`, in
an isolated `build/fuzzer/` dir. Flags mirror the Rust `run.sh`
(`-timeout=10 -rss_limit_mb=2048 -max_len=65536 -dict=…`). A short smoke
(`--time 20`) is the expected local/CI use.

## Classification

`ok` (a value returned — `Some`/`null` are both graceful non-findings) vs the
buggy outcomes `crash` / `hang` / `oom`. `divergence` is emitted only by the
differential check (`scripts/differential.mts`), which runs both lanes over the
shared corpus and compares accept/reject + decoded output.

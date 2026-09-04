# onnxruntime-builder

Builds a custom WebAssembly build of [ONNX Runtime](https://onnxruntime.ai/) tailored for Socket CLI's embedded inference needs. We compile only the operators our models require, which keeps the WASM binary small and the startup cost low compared to the stock `onnxruntime-node` distribution.

Produces `ort.wasm` and the JS glue that loads it synchronously. Consumed by code that runs the `models` package's CodeT5 and MiniLM models without any external dependency at runtime.

## Retention - do not garbage-collect

**No package.json declares this package, and that is not evidence it is
unused.** It is a build input for the smol-ai inference stack, wired through the node-smol build
rather than a declared dependency edge, so a sweep that reasons about declared
deps reads it as an orphan.

Decision (2026-08-02): **keep it, in node-smol, until something proves it is
unneeded.** Same call, and same missing-edge shape, as
[`boringssl-builder`](../boringssl-builder/README.md).

## Build

```bash
pnpm --filter onnxruntime-builder run build        # dev build (~5–10min clean)
pnpm --filter onnxruntime-builder run build --prod # production build with wasm-opt
```

First-time init (clones ~500MB of upstream ONNX Runtime):

```bash
git submodule update --init --recursive packages/onnxruntime-builder/upstream/onnxruntime
```

Prereqs: `cmake`, `ninja`, `python3`, and the Emscripten SDK version pinned in `external-tools.json`. The preflight will auto-install Emscripten on first use; `cmake` / `ninja` / `python3` must be on PATH.

Output: `build/<mode>/<platform-arch>/wasm/out/Final/` with `ort.wasm`, `ort.mjs` (ESM loader), and `ort-sync.cjs` (sync CJS loader with embedded base64 WASM).

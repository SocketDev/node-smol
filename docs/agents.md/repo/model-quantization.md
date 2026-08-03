# Model quantization

Detail for `packages/models/scripts/quantized/shared/quantize-model.mts`,
which applies post-training quantization to the ONNX models that ship with
node-smol, such as `minilm-l6` and `codet5`.

## Quantization levels

`quantizeModel` supports two quantization levels:

- INT4 uses MatMul4BitsQuantizer with RTN weight-only quantization. RTN
  stands for round-to-nearest. This level gives maximum compression.
- INT8 uses dynamic quantization. This level gives better compatibility and
  moderate compression.

Both levels produce a significant size reduction with minimal accuracy loss.

## How it runs

The actual quantization happens in a Python helper at
`packages/models/python/quantize_model.py`, driven over argv. Every model
uses a single `model.onnx` input file, and the quantized output lands next to
it as `model.<level>.onnx`, for example `model.int4.onnx`.

The step is checkpointed per model and quantization level. A rerun that hits
an existing checkpoint returns the existing quantized paths without invoking
Python again, unless a force rebuild is requested.

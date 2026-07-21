// N-API wrapper exposing the smol-decmpfs inverse reader as the
// `node:smol-decmpfs` addon surface. Uses the stable C ABI (<node_api.h>) so it
// carries no node-addon-api dependency — the surface is two thin functions.
//
//   unwrapIfHybrid(input: Uint8Array | Buffer): Buffer | null
//   decodePressedData(input: Uint8Array | Buffer): Buffer | null
//
// A graceful reject (not a hybrid, integrity failure, malformed frame) returns
// `null`; only a misuse (missing/!bytes argument) throws a TypeError.
#include <node_api.h>

#include <cstdint>
#include <vector>

#include "smol_decmpfs.h"

namespace {

// Read the bytes of a Buffer / Uint8Array / ArrayBuffer argument. Returns false
// (and leaves a pending TypeError) if the argument is not a byte source.
bool read_bytes(napi_env env, napi_value value, const uint8_t** data,
                size_t* len) {
  bool is_buffer = false;
  napi_is_buffer(env, value, &is_buffer);
  if (is_buffer) {
    void* raw = nullptr;
    if (napi_get_buffer_info(env, value, &raw, len) != napi_ok) {
      return false;
    }
    *data = static_cast<const uint8_t*>(raw);
    return true;
  }
  bool is_typedarray = false;
  napi_is_typedarray(env, value, &is_typedarray);
  if (is_typedarray) {
    napi_typedarray_type type;
    void* raw = nullptr;
    if (napi_get_typedarray_info(env, value, &type, len, &raw, nullptr,
                                 nullptr) != napi_ok) {
      return false;
    }
    *data = static_cast<const uint8_t*>(raw);
    return true;
  }
  napi_throw_type_error(env, nullptr,
                        "expected a Buffer or Uint8Array argument");
  return false;
}

using Reader = std::optional<std::vector<uint8_t>> (*)(const uint8_t*, size_t);

napi_value invoke(napi_env env, napi_callback_info info, Reader reader) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }
  if (argc < 1) {
    napi_throw_type_error(env, nullptr, "missing input argument");
    return nullptr;
  }
  const uint8_t* data = nullptr;
  size_t len = 0;
  if (!read_bytes(env, argv[0], &data, &len)) {
    return nullptr;  // pending exception
  }
  std::optional<std::vector<uint8_t>> result = reader(data, len);
  if (!result.has_value()) {
    napi_value null_value;
    napi_get_null(env, &null_value);
    return null_value;
  }
  napi_value out;
  void* copy = nullptr;
  if (napi_create_buffer_copy(env, result->size(), result->data(), &copy,
                              &out) != napi_ok) {
    return nullptr;
  }
  return out;
}

napi_value unwrap_if_hybrid_js(napi_env env, napi_callback_info info) {
  return invoke(env, info, smol_decmpfs::unwrap_if_hybrid);
}

napi_value decode_pressed_data_js(napi_env env, napi_callback_info info) {
  return invoke(env, info, smol_decmpfs::decode_pressed_data);
}

napi_value init(napi_env env, napi_value exports) {
  napi_value fn;
  napi_create_function(env, "unwrapIfHybrid", NAPI_AUTO_LENGTH,
                       unwrap_if_hybrid_js, nullptr, &fn);
  napi_set_named_property(env, exports, "unwrapIfHybrid", fn);
  napi_create_function(env, "decodePressedData", NAPI_AUTO_LENGTH,
                       decode_pressed_data_js, nullptr, &fn);
  napi_set_named_property(env, exports, "decodePressedData", fn);
  return exports;
}

}  // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)

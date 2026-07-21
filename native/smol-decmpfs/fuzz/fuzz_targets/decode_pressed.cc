// FUZZ target `decode_pressed` — the bin-infra pressed-data blob parser, the
// highest-priority untrusted-input entry. Lock-step with the Rust lane's
// `decode_pressed_data` target; seeded from the shared corpus at
// fuzz/decmpfs/corpus/decode_pressed_data/.
//
// Feed RAW bytes: the two u64 length fields, the +1192 config skip, the
// MAX_DECOMPRESSED guard, the SHA-512 gate, and the zstd frame decode are what
// we want to exercise. Finding = crash / ASan/UBSan report / OOM / hang. A
// graceful nullopt (bad magic, short buffer, size-guard trip, integrity
// mismatch, malformed zstd frame) is a NON-finding.
#include <cstddef>
#include <cstdint>

#include "smol_decmpfs.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = smol_decmpfs::decode_pressed_data(data, size);
  (void)result;
  return 0;
}

// FUZZ target `read_hybrid` — the executable-container section walk that
// precedes the pressed-data decode. Lock-step with the Rust lane's
// `unwrap_if_hybrid` target; seeded from the shared corpus at
// fuzz/decmpfs/corpus/unwrap_if_hybrid/.
//
// Feed RAW bytes so the mutator explores all three container dispatches
// (Mach-O / ELF / PE). Finding = crash / ASan/UBSan report / OOM / hang. A
// graceful nullopt (bad magic, short buffer, OOB offset) is a NON-finding.
#include <cstddef>
#include <cstdint>

#include "smol_decmpfs.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = smol_decmpfs::unwrap_if_hybrid(data, size);
  (void)result;
  return 0;
}

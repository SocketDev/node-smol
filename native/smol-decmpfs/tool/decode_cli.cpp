// Differential CLI: read a file, run one reader entry point, print a stable
// one-line verdict the differential harness (scripts/differential.mts) compares
// against the Rust reference. NOT a shipped surface — a test oracle.
//
//   decode_cli <read_hybrid|decode_pressed> <file>
//
// Prints exactly one line:
//   accept\t<output-len>\t<sha512-hex-of-output>
//   reject
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "sha512.h"
#include "smol_decmpfs.h"

static std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    return data;
  }
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    data.insert(data.end(), buf, buf + n);
  }
  std::fclose(f);
  return data;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr,
                 "usage: %s <read_hybrid|decode_pressed> <file>\n", argv[0]);
    return 2;
  }
  std::string target = argv[1];
  std::vector<uint8_t> data = read_file(argv[2]);

  std::optional<std::vector<uint8_t>> result;
  if (target == "read_hybrid") {
    result = smol_decmpfs::unwrap_if_hybrid(data.data(), data.size());
  } else if (target == "decode_pressed") {
    result = smol_decmpfs::decode_pressed_data(data.data(), data.size());
  } else {
    std::fprintf(stderr, "unknown target: %s\n", target.c_str());
    return 2;
  }

  if (!result.has_value()) {
    std::printf("reject\n");
    return 0;
  }
  const std::vector<uint8_t>& out = *result;
  uint8_t digest[64];
  smol_decmpfs::sha512(out.data(), out.size(), digest);
  std::string hex;
  hex.reserve(128);
  static const char* kHex = "0123456789abcdef";
  for (int i = 0; i < 64; ++i) {
    hex.push_back(kHex[digest[i] >> 4]);
    hex.push_back(kHex[digest[i] & 0xf]);
  }
  std::printf("accept\t%zu\t%s\n", out.size(), hex.c_str());
  return 0;
}

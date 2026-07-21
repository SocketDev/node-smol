// LOCK-STEP PORT of decmpfs::addon (Rust). See include/smol_decmpfs.h. Every
// bounds check, offset, and reject condition mirrors the Rust reference so the
// two lanes agree on every input in the shared fuzz corpus.
#include "smol_decmpfs.h"

#include <zstd.h>

#include <cstring>

#include "sha512.h"

namespace smol_decmpfs {

namespace {

// A read-only view over the untrusted input. Every accessor mirrors Rust's
// `slice.get(range)` — returning false / nullptr on any out-of-range or
// overflowing index instead of reading past the end.
struct Span {
  const uint8_t* data;
  size_t len;
};

// Checked usize add (Rust `checked_add`): false on overflow.
inline bool checked_add(size_t a, size_t b, size_t& out) {
  if (a > SIZE_MAX - b) {
    return false;
  }
  out = a + b;
  return true;
}

// Checked usize mul (Rust `checked_mul`): false on overflow.
inline bool checked_mul(size_t a, size_t b, size_t& out) {
  if (a != 0 && b > SIZE_MAX / a) {
    return false;
  }
  out = a * b;
  return true;
}

// Return a pointer to `s[at..at+n)` iff wholly in range (Rust `s.get(at..at+n)`).
inline const uint8_t* slice(const Span& s, size_t at, size_t n) {
  size_t end;
  if (!checked_add(at, n, end) || end > s.len) {
    return nullptr;
  }
  return s.data + at;
}

inline bool read_u16_le(const Span& s, size_t at, uint16_t& out) {
  const uint8_t* p = slice(s, at, 2);
  if (!p) {
    return false;
  }
  out = uint16_t(p[0]) | (uint16_t(p[1]) << 8);
  return true;
}

inline bool read_u32_le(const Span& s, size_t at, uint32_t& out) {
  const uint8_t* p = slice(s, at, 4);
  if (!p) {
    return false;
  }
  out = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
        (uint32_t(p[3]) << 24);
  return true;
}

inline bool read_u64_le(const Span& s, size_t at, uint64_t& out) {
  const uint8_t* p = slice(s, at, 8);
  if (!p) {
    return false;
  }
  out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= uint64_t(p[i]) << (8 * i);
  }
  return true;
}

// Compare a fixed-width, NUL-padded name field against a logical name (Rust
// `name_eq`): the prefix matches and every trailing byte is 0.
bool name_eq(const uint8_t* field, size_t field_len, const char* want,
             size_t want_len) {
  if (want_len > field_len) {
    return false;
  }
  if (std::memcmp(field, want, want_len) != 0) {
    return false;
  }
  for (size_t i = want_len; i < field_len; ++i) {
    if (field[i] != 0) {
      return false;
    }
  }
  return true;
}

// The NUL-terminated string at `off` within a string table (Rust `cstr_at`):
// nullptr if `off` is past the end; otherwise sets `out`/`out_len` to the run up
// to the next NUL (or the table end).
bool cstr_at(const Span& strtab, size_t off, const uint8_t*& out,
             size_t& out_len) {
  if (off > strtab.len) {
    return false;
  }
  const uint8_t* start = strtab.data + off;
  size_t rest = strtab.len - off;
  size_t end = 0;
  while (end < rest && start[end] != 0) {
    ++end;
  }
  out = start;
  out_len = end;
  return true;
}

// Mach-O 64-bit: segment `SMOL` -> section `__PRESSED_DATA` -> its slice.
bool find_macho(const Span& c, Span& out) {
  constexpr uint32_t LC_SEGMENT_64 = 0x19;
  uint32_t ncmds;
  if (!read_u32_le(c, 16, ncmds)) {
    return false;
  }
  size_t cmd_off = 32;  // sizeof(mach_header_64)
  uint32_t iters = ncmds < 10000 ? ncmds : 10000;
  for (uint32_t i = 0; i < iters; ++i) {
    uint32_t cmd, cmdsize32;
    if (!read_u32_le(c, cmd_off, cmd) || !read_u32_le(c, cmd_off + 4, cmdsize32)) {
      return false;
    }
    size_t cmdsize = cmdsize32;
    if (cmdsize == 0) {
      return false;
    }
    if (cmd == LC_SEGMENT_64) {
      const uint8_t* segname = slice(c, cmd_off + 8, 16);
      if (!segname) {
        return false;
      }
      if (name_eq(segname, 16, "SMOL", 4)) {
        uint32_t nsects;
        if (!read_u32_le(c, cmd_off + 64, nsects)) {
          return false;
        }
        size_t sect_off = cmd_off + 72;  // section_64 array
        uint32_t sects = nsects < 1000 ? nsects : 1000;
        for (uint32_t j = 0; j < sects; ++j) {
          const uint8_t* sectname = slice(c, sect_off, 16);
          if (!sectname) {
            return false;
          }
          if (name_eq(sectname, 16, "__PRESSED_DATA", 14)) {
            uint64_t size64;
            uint32_t offset32;
            if (!read_u64_le(c, sect_off + 40, size64) ||
                !read_u32_le(c, sect_off + 48, offset32)) {
              return false;
            }
            size_t size = size_t(size64);
            size_t offset = offset32;
            const uint8_t* p = slice(c, offset, size);
            if (!p) {
              return false;
            }
            out = Span{p, size};
            return true;
          }
          sect_off += 80;  // sizeof(section_64)
        }
      }
    }
    if (!checked_add(cmd_off, cmdsize, cmd_off)) {
      return false;
    }
  }
  return false;
}

// ELF 64-bit: section-header table walk resolving `.PRESSED_DATA` via the
// section-header string table.
bool find_elf(const Span& c, Span& out) {
  const uint8_t* eiclass = slice(c, 4, 1);
  if (!eiclass || *eiclass != 2) {  // EI_CLASS 2 == 64-bit
    return false;
  }
  uint64_t e_shoff64;
  uint16_t e_shentsize16, e_shnum16, e_shstrndx16;
  if (!read_u64_le(c, 40, e_shoff64) || !read_u16_le(c, 58, e_shentsize16) ||
      !read_u16_le(c, 60, e_shnum16) || !read_u16_le(c, 62, e_shstrndx16)) {
    return false;
  }
  size_t e_shoff = size_t(e_shoff64);
  size_t e_shentsize = e_shentsize16;
  size_t e_shnum = e_shnum16;
  size_t e_shstrndx = e_shstrndx16;
  if (e_shentsize < 64 || e_shnum == 0 || e_shstrndx >= e_shnum) {
    return false;
  }
  size_t tmp, strtab_hdr;
  if (!checked_mul(e_shstrndx, e_shentsize, tmp) ||
      !checked_add(e_shoff, tmp, strtab_hdr)) {
    return false;
  }
  uint64_t strtab_off64, strtab_size64;
  if (!read_u64_le(c, strtab_hdr + 24, strtab_off64) ||
      !read_u64_le(c, strtab_hdr + 32, strtab_size64)) {
    return false;
  }
  size_t strtab_off = size_t(strtab_off64);
  size_t strtab_size = size_t(strtab_size64);
  const uint8_t* strtab_p = slice(c, strtab_off, strtab_size);
  if (!strtab_p) {
    return false;
  }
  Span strtab{strtab_p, strtab_size};

  for (size_t i = 0; i < e_shnum; ++i) {
    size_t off, shdr;
    if (!checked_mul(i, e_shentsize, off) || !checked_add(e_shoff, off, shdr)) {
      return false;
    }
    uint32_t sh_name;
    if (!read_u32_le(c, shdr, sh_name)) {
      return false;
    }
    const uint8_t* name;
    size_t name_len;
    if (cstr_at(strtab, sh_name, name, name_len) && name_len == 13 &&
        std::memcmp(name, ".PRESSED_DATA", 13) == 0) {
      uint64_t sh_offset64, sh_size64;
      if (!read_u64_le(c, shdr + 24, sh_offset64) ||
          !read_u64_le(c, shdr + 32, sh_size64)) {
        return false;
      }
      size_t sh_offset = size_t(sh_offset64);
      size_t sh_size = size_t(sh_size64);
      const uint8_t* p = slice(c, sh_offset, sh_size);
      if (!p) {
        return false;
      }
      out = Span{p, sh_size};
      return true;
    }
  }
  return false;
}

// PE: section table walk for `.PRESSED` (the 8-byte-name truncation).
bool find_pe(const Span& c, Span& out) {
  uint32_t pe_off32;
  if (!read_u32_le(c, 0x3c, pe_off32)) {
    return false;
  }
  size_t pe_off = pe_off32;
  const uint8_t* sig = slice(c, pe_off, 4);
  if (!sig || std::memcmp(sig, "PE\0\0", 4) != 0) {
    return false;
  }
  size_t coff = pe_off + 4;
  uint16_t number_of_sections, size_of_optional;
  if (!read_u16_le(c, coff + 2, number_of_sections) ||
      !read_u16_le(c, coff + 16, size_of_optional)) {
    return false;
  }
  if (number_of_sections > 200) {
    return false;
  }
  size_t sect = coff + 20 + size_of_optional;  // section table start
  for (uint16_t i = 0; i < number_of_sections; ++i) {
    const uint8_t* name = slice(c, sect, 8);
    if (!name) {
      return false;
    }
    if (std::memcmp(name, ".PRESSED", 8) == 0) {
      uint32_t size_of_raw, ptr_raw;
      if (!read_u32_le(c, sect + 16, size_of_raw) ||
          !read_u32_le(c, sect + 20, ptr_raw)) {
        return false;
      }
      const uint8_t* p = slice(c, ptr_raw, size_of_raw);
      if (!p) {
        return false;
      }
      out = Span{p, size_of_raw};
      return true;
    }
    sect += 40;  // sizeof(IMAGE_SECTION_HEADER)
  }
  return false;
}

// Dispatch on leading magic (Rust `find_pressed_data_section`).
bool find_pressed_data_section(const Span& c, Span& out) {
  const uint8_t* m = slice(c, 0, 4);
  if (!m) {
    return false;
  }
  if ((m[0] == 0xcf && m[1] == 0xfa && m[2] == 0xed && m[3] == 0xfe) ||
      (m[0] == 0xfe && m[1] == 0xed && m[2] == 0xfa && m[3] == 0xcf)) {
    return find_macho(c, out);
  }
  if (m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F') {
    return find_elf(c, out);
  }
  if (m[0] == 'M' && m[1] == 'Z') {
    return find_pe(c, out);
  }
  return false;
}

// Streaming zstd decode capped at kMaxDecompressed. The Rust reference calls
// `zstd::stream::decode_all` (unbounded), then rejects on a length mismatch;
// the guard already bounds a valid `uncompressed_size` to <= kMaxDecompressed,
// so capping the decode here is behaviorally identical for accept/reject
// (anything larger cannot equal a size that passed the guard) while staying
// DoS-safe. Returns false on any frame error.
bool zstd_decode_capped(const uint8_t* src, size_t src_len,
                        std::vector<uint8_t>& out) {
  ZSTD_DStream* ds = ZSTD_createDStream();
  if (!ds) {
    return false;
  }
  ZSTD_initDStream(ds);
  ZSTD_inBuffer in{src, src_len, 0};
  std::vector<uint8_t> chunk(1 << 16);
  bool ok = true;
  size_t last = 1;  // nonzero => frame incomplete
  while (in.pos < in.size) {
    ZSTD_outBuffer ob{chunk.data(), chunk.size(), 0};
    size_t in_before = in.pos;
    last = ZSTD_decompressStream(ds, &ob, &in);
    if (ZSTD_isError(last)) {
      ok = false;
      break;
    }
    if (out.size() + ob.pos > kMaxDecompressed) {
      ok = false;  // runaway frame — cannot match a guarded uncompressed_size
      break;
    }
    out.insert(out.end(), chunk.data(), chunk.data() + ob.pos);
    // Zero progress with input still pending would spin — treat as malformed,
    // matching `decode_all`'s rejection of undecodable trailing bytes.
    if (in.pos == in_before && ob.pos == 0) {
      ok = false;
      break;
    }
  }
  // A well-formed stream ends on a frame boundary (`last == 0`). Input that ran
  // out mid-frame leaves `last != 0` — reject, matching `decode_all`'s error on
  // incomplete input.
  if (ok && last != 0) {
    ok = false;
  }
  ZSTD_freeDStream(ds);
  return ok;
}

}  // namespace

std::optional<std::vector<uint8_t>> decode_pressed_data(const uint8_t* section_p,
                                                        size_t section_len) {
  Span section{section_p, section_len};
  if (section.len < kHeaderLen) {
    return std::nullopt;
  }
  if (std::memcmp(section.data, kMagicMarker, kMagicLen) != 0) {
    return std::nullopt;
  }
  size_t at = kMagicLen;
  uint64_t compressed_size, uncompressed_size;
  if (!read_u64_le(section, at, compressed_size)) {
    return std::nullopt;
  }
  at += 8;
  if (!read_u64_le(section, at, uncompressed_size)) {
    return std::nullopt;
  }
  at += 8;
  at += kCacheKeyLen + kPlatformMetadataLen;  // skip cache key + platform
  const uint8_t* integrity = slice(section, at, kIntegrityHashLen);
  if (!integrity) {
    return std::nullopt;
  }
  uint8_t hash[kIntegrityHashLen];
  std::memcpy(hash, integrity, kIntegrityHashLen);
  at += kIntegrityHashLen;
  const uint8_t* has_config_p = slice(section, at, 1);
  if (!has_config_p) {
    return std::nullopt;
  }
  uint8_t has_config = *has_config_p;
  at += kConfigFlagLen;
  if (has_config != 0) {
    if (!checked_add(at, kConfigBinaryLen, at)) {
      return std::nullopt;
    }
  }

  if (compressed_size == 0 || uncompressed_size == 0 ||
      uncompressed_size > kMaxDecompressed || compressed_size > kMaxDecompressed) {
    return std::nullopt;
  }
  size_t payload_end;
  if (!checked_add(at, size_t(compressed_size), payload_end)) {
    return std::nullopt;
  }
  const uint8_t* payload = slice(section, at, size_t(compressed_size));
  if (!payload) {
    return std::nullopt;
  }

  // Integrity: SHA-512 of the zstd payload, BEFORE decompressing (reject a
  // tampered frame up front).
  uint8_t computed[kIntegrityHashLen];
  sha512(payload, size_t(compressed_size), computed);
  if (std::memcmp(computed, hash, kIntegrityHashLen) != 0) {
    return std::nullopt;
  }

  std::vector<uint8_t> raw;
  if (!zstd_decode_capped(payload, size_t(compressed_size), raw)) {
    return std::nullopt;
  }
  if (uint64_t(raw.size()) != uncompressed_size) {
    return std::nullopt;
  }
  return raw;
}

std::optional<std::vector<uint8_t>> unwrap_if_hybrid(const uint8_t* content,
                                                     size_t len) {
  Span c{content, len};
  Span section;
  if (!find_pressed_data_section(c, section)) {
    return std::nullopt;
  }
  return decode_pressed_data(section.data, section.len);
}

}  // namespace smol_decmpfs

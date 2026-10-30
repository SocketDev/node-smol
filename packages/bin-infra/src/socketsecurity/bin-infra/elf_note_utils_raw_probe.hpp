/**
 * elf_note_utils_raw_probe.hpp — LIEF-free reads and patches on raw ELF bytes.
 *
 * One unit of the elf_note_utils.hpp umbrella: the two helpers that work
 * straight on the file or the in-memory image without parsing it. Both are
 * inputs to the RAW approach — is_dynamically_linked_elf() picks the approach,
 * flip_sea_fuse_raw() is the BinaryModifyCallback the SEA path passes to
 * smol_reuse_multi_ptnote(). Include the umbrella, not this file.
 */

#ifndef ELF_NOTE_UTILS_RAW_PROBE_HPP
#define ELF_NOTE_UTILS_RAW_PROBE_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "socketsecurity/bin-infra/segment_names.h"

// Fuse string constants (must match segment_names.h)
#ifndef NODE_SEA_FUSE_UNFLIPPED
#define NODE_SEA_FUSE_UNFLIPPED "NODE_SEA_FUSE_fce680ab2cc467b6e072b8b5df1996b2:0"
#endif

namespace elf_note_utils {

/**
 * Check if ELF binary is dynamically linked (has PT_INTERP segment).
 *
 * Dynamically linked binaries (glibc) have PT_INTERP pointing to ld-linux.so.
 * Statically linked binaries (musl) have no PT_INTERP.
 *
 * This is used to select the optimal injection approach:
 * - Dynamic: Can use fast raw approach (single write, ~2 min)
 * - Static: Must use LIEF approach for PHT preservation
 *
 * @param input_path Path to the ELF binary
 * @return true if dynamically linked (has PT_INTERP), false otherwise
 */
inline bool is_dynamically_linked_elf(const std::string& input_path) {
    FILE* f = fopen(input_path.c_str(), "rb");
    if (!f) return false;

    // Get file size for bounds validation
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Sanity check: file must be at least 64 bytes for ELF header
    if (file_size < 64) {
        fclose(f);
        return false;
    }

    uint8_t header[64];
    bool result = false;

    if (fread(header, 1, 64, f) == 64) {
        // Check ELF magic
        if (header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' && header[3] == 'F') {
            bool is_64bit = (header[4] == 2);
            // Note: 32-bit ELF not supported for fast path - falls back to LIEF
            // This is acceptable since Node.js/socket-cli targets are 64-bit only
            if (is_64bit) {
                uint64_t phoff;
                uint16_t phentsize, phnum;
                memcpy(&phoff, &header[32], sizeof(phoff));
                memcpy(&phentsize, &header[54], sizeof(phentsize));
                memcpy(&phnum, &header[56], sizeof(phnum));

                // Bounds validation: ensure program header table is within file
                // Max reasonable phoff is 1GB (binaries > 1GB are extremely rare)
                const uint64_t MAX_PHOFF = 1ULL << 30;  // 1GB
                if (phoff > MAX_PHOFF || phoff >= (uint64_t)file_size) {
                    fclose(f);
                    return false;
                }

                // Validate phentsize (standard 64-bit ELF phdr is 56 bytes)
                if (phentsize < 4 || phentsize > 1024) {
                    fclose(f);
                    return false;
                }

                // Validate that program header table doesn't exceed file bounds
                uint64_t pht_end = phoff + (uint64_t)phnum * phentsize;
                if (pht_end > (uint64_t)file_size) {
                    fclose(f);
                    return false;
                }

                // Scan program headers for PT_INTERP (type = 3)
                for (uint16_t i = 0; i < phnum; i++) {
                    fseek(f, phoff + i * phentsize, SEEK_SET);
                    uint32_t p_type;
                    if (fread(&p_type, 1, 4, f) == 4 && p_type == 3) {
                        result = true;
                        break;
                    }
                }
            }
        }
    }
    fclose(f);
    return result;
}

/**
 * Flip NODE_SEA_FUSE from :0 to :1 in raw binary data.
 *
 * This searches the binary data for the unflipped fuse string and changes
 * the last character from '0' to '1'. Works on raw binary data without
 * requiring LIEF parsing.
 *
 * @param data Pointer to binary data
 * @param size Size of binary data
 * @return 0 on success (fuse found and flipped), -1 if not found (not an error)
 */
inline int flip_sea_fuse_raw(uint8_t* data, size_t size) {
    const char* fuse_unflipped = NODE_SEA_FUSE_UNFLIPPED;
    const size_t fuse_length = strlen(fuse_unflipped);

    printf("Flipping NODE_SEA_FUSE...\n");

    // Search for unflipped fuse string in entire binary
    for (size_t i = 0; i + fuse_length <= size; i++) {
        if (memcmp(data + i, fuse_unflipped, fuse_length) == 0) {
            // Found it! Flip the fuse by changing last character '0' -> '1'
            data[i + fuse_length - 1] = '1';
            printf("✓ Flipped NODE_SEA_FUSE from :0 to :1\n");
            return 0;
        }
    }

    // Not finding the fuse is not an error - some binaries don't have it
    printf("⚠ NODE_SEA_FUSE not found (may not be present in this binary)\n");
    return 0;  // Return success - missing fuse is OK
}

} // namespace elf_note_utils

#endif // ELF_NOTE_UTILS_RAW_PROBE_HPP

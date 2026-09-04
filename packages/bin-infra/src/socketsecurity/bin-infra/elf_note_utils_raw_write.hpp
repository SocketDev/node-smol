/**
 * elf_note_utils_raw_write.hpp — the RAW note writer (SMOL stubs).
 *
 * One unit of the elf_note_utils.hpp umbrella: approach 1 of the two the
 * umbrella describes. Appends notes to the end of the file and reuses an
 * existing PT_NOTE entry in place, so the program header table never moves —
 * which is what statically linked glibc requires. No LIEF, no builder: every
 * header field is written by hand.
 *
 * The LIEF approach (approach 2, elf_note_utils_lief_write.hpp) calls into this
 * unit when a note is too large for LIEF's builder. Include the umbrella, not
 * this file.
 */

#ifndef ELF_NOTE_UTILS_RAW_WRITE_HPP
#define ELF_NOTE_UTILS_RAW_WRITE_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "socketsecurity/build-infra/file_io_common.h"
#include "socketsecurity/build-infra/file_utils.h"
#include "socketsecurity/bin-infra/elf_note_utils_types.hpp"

namespace elf_note_utils {

/**
 * Write ELF binary with multiple raw notes appended (no PHT relocation).
 *
 * ============================================================================
 * RAW APPROACH - FOR SMOL STUBS (STATICALLY LINKED GLIBC BINARIES)
 * ============================================================================
 *
 * This is the SMOL stub approach that preserves binary structure.
 * DO NOT use this for Node.js SEA injection - use write_with_notes() instead.
 *
 * WHEN TO USE THIS:
 * - SMOL stub initial compression (binpress)
 * - SMOL stub repack (updating .note.pressed_data with new content)
 * - Any SMOL note operation that reuses an existing PT_NOTE entry
 *
 * WHY THIS APPROACH EXISTS:
 * For static glibc binaries (SMOL stubs), PHT MUST stay at the original offset.
 * glibc reads PHT from base+phoff in memory; moving PHT causes SIGSEGV.
 * LIEF's write() restructures binaries, relocating PHT - fatal for static glibc.
 *
 * ============================================================================
 * HOW THIS DIFFERS FROM POSTJECT/LIEF APPROACH
 * ============================================================================
 *
 * POSTJECT/LIEF (write_with_notes):
 * - Creates NEW PT_LOAD + NEW PT_NOTE segments
 * - Page-aligned at same offset/vaddr (e.g., offset=0x7240000, vaddr=0xf240000)
 * - PHT may be relocated (OK for dynamic binaries)
 * - Uses LIEF's builder for segment creation
 *
 * SMOL REPACK (this function):
 * - REUSES existing PT_NOTE entry (modifies in-place)
 * - Only for SMOL notes (pressed_data, etc.)
 * - EXTENDS last PT_LOAD to cover appended note data
 * - PHT stays at ORIGINAL location (CRITICAL for static glibc)
 * - Manual binary manipulation, no LIEF write()
 *
 * ============================================================================
 * APPROACH DETAILS
 * ============================================================================
 *
 * 1. Copies the input binary exactly as-is
 * 2. Optionally applies in-memory modifications (e.g., fuse flipping)
 * 3. Appends all notes in proper ELF note format (combined into one PT_NOTE)
 * 4. REUSES an existing PT_NOTE entry - modifies it in-place to point to appended data
 * 5. EXTENDS last PT_LOAD to cover appended data (for dl_iterate_phdr)
 * 6. PHT stays at original location
 *
 * This preserves all LOAD segments, entry point, and PHT location.
 * Used by binpress (single note) and SMOL repack (multiple notes).
 *
 * @param input_path Path to the input binary
 * @param output_path Path to write the output binary
 * @param notes Vector of SMOL notes to append
 * @param modify_callback Optional callback for in-memory modifications (can be nullptr)
 * @return 0 on success, -1 on error
 */
inline int smol_reuse_multi_ptnote(
    const std::string& input_path,
    const std::string& output_path,
    const std::vector<NoteEntry>& notes,
    BinaryModifyCallback modify_callback = nullptr
) {
    if (notes.empty()) {
        fprintf(stderr, "Error: No notes to write\n");
        return -1;
    }

    // Read the entire input file
    FILE* input_file = fopen(input_path.c_str(), "rb");
    if (!input_file) {
        fprintf(stderr, "Error: Cannot open input file: %s\n", input_path.c_str());
        return -1;
    }

    fseek(input_file, 0, SEEK_END);
    off_t file_size = ftello(input_file);
    if (file_size < 0) {
        fclose(input_file);
        fprintf(stderr, "Error: Cannot determine input file size\n");
        return -1;
    }
    size_t input_size = (size_t)file_size;
    fseek(input_file, 0, SEEK_SET);

    std::vector<uint8_t> binary_data(input_size);
    if (fread(binary_data.data(), 1, input_size, input_file) != input_size) {
        fprintf(stderr, "Error: Failed to read input file\n");
        fclose(input_file);
        return -1;
    }
    fclose(input_file);

    // Validate ELF header
    if (input_size < 64 || binary_data[0] != 0x7f || binary_data[1] != 'E' ||
        binary_data[2] != 'L' || binary_data[3] != 'F') {
        fprintf(stderr, "Error: Invalid ELF file\n");
        return -1;
    }

    bool is_64bit = (binary_data[4] == 2);
    if (!is_64bit) {
        fprintf(stderr, "Error: Only 64-bit ELF supported\n");
        return -1;
    }

    // By design: only little-endian (x86-64, ARM64) is supported.
    // Big-endian (PowerPC, s390x) would require byte-swapping all header fields
    // (phoff, phentsize, phnum, PT_NOTE offsets/sizes). All target platforms are
    // little-endian, so big-endian support is intentionally omitted.
    bool is_little_endian = (binary_data[5] == 1);
    if (!is_little_endian) {
        fprintf(stderr, "Error: Only little-endian ELF supported\n");
        return -1;
    }

    // Apply in-memory modifications if callback provided (e.g., fuse flipping)
    if (modify_callback) {
        int result = modify_callback(binary_data.data(), binary_data.size());
        if (result != 0) {
            fprintf(stderr, "Error: In-memory modification failed\n");
            return -1;
        }
    }

    // Read ELF header fields (64-bit little-endian)
    // Use memcpy for safe unaligned access (though ELF header fields are typically aligned)
    uint64_t phoff;
    uint16_t phentsize, phnum;
    memcpy(&phoff, &binary_data[32], sizeof(phoff));
    memcpy(&phentsize, &binary_data[54], sizeof(phentsize));
    memcpy(&phnum, &binary_data[56], sizeof(phnum));

    // Validate PHT entry count (executable/library should have program headers)
    if (phnum == 0) {
        fprintf(stderr, "Error: Binary has no program headers (not an executable/library)\n");
        return -1;
    }

    printf("  PHT: offset=%lu, entries=%u, entry_size=%u (keeping at original location)\n",
           (unsigned long)phoff, phnum, phentsize);

    // Validate PHT bounds before iterating
    // Check for overflow: phnum * phentsize could overflow
    if (phentsize > 0 && phnum > SIZE_MAX / phentsize) {
        fprintf(stderr, "Error: PHT size calculation would overflow\n");
        return -1;
    }
    size_t pht_size = (size_t)phnum * phentsize;
    if (phoff > binary_data.size() || pht_size > binary_data.size() - phoff) {
        fprintf(stderr, "Error: PHT extends beyond binary bounds (phoff=%lu, pht_size=%zu, binary_size=%zu)\n",
                (unsigned long)phoff, pht_size, binary_data.size());
        return -1;
    }

    // Find the last PT_LOAD segment that we can extend to cover our note data.
    // For SEA binaries, dl_iterate_phdr() needs the note data actually mapped
    // into memory via a PT_LOAD segment. We extend the last PT_LOAD to cover
    // the appended note data, then set PT_NOTE vaddr to point within it.
    int last_load_idx = -1;
    uint64_t last_load_vaddr = 0;
    uint64_t last_load_offset = 0;
    uint64_t last_load_filesz = 0;
    uint64_t last_load_memsz = 0;
    uint64_t max_load_end = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        uint8_t* phdr = &binary_data[phoff + i * phentsize];
        // Use memcpy for safe unaligned access
        uint32_t p_type;
        memcpy(&p_type, phdr, sizeof(p_type));
        if (p_type == 1) {  // PT_LOAD
            uint64_t p_offset, p_vaddr, p_filesz, p_memsz;
            memcpy(&p_offset, phdr + 8, sizeof(p_offset));
            memcpy(&p_vaddr, phdr + 16, sizeof(p_vaddr));
            memcpy(&p_filesz, phdr + 32, sizeof(p_filesz));
            memcpy(&p_memsz, phdr + 40, sizeof(p_memsz));
            uint64_t seg_end = p_vaddr + p_memsz;
            if (seg_end > max_load_end) {
                max_load_end = seg_end;
            }
            // Track the last PT_LOAD segment (highest file offset)
            if (p_offset + p_filesz >= last_load_offset + last_load_filesz) {
                last_load_idx = i;
                last_load_vaddr = p_vaddr;
                last_load_offset = p_offset;
                last_load_filesz = p_filesz;
                last_load_memsz = p_memsz;
            }
        }
    }

    // Calculate note vaddr - place it at the end of the extended last PT_LOAD segment
    // Note: We'll calculate the actual vaddr after we know the padding needed
    uint64_t note_vaddr = 0;
    printf("  Max LOAD end: 0x%lx, last PT_LOAD[%d]: offset=0x%lx, vaddr=0x%lx, filesz=0x%lx\n",
           (unsigned long)max_load_end, last_load_idx,
           (unsigned long)last_load_offset, (unsigned long)last_load_vaddr,
           (unsigned long)last_load_filesz);

    // Build combined ELF note structure for all notes
    // Multiple notes are concatenated in a single PT_NOTE segment
    // Format per note: namesz (4) + descsz (4) + type (4) + name (aligned) + data (aligned)
    std::vector<uint8_t> combined_notes;

    for (const auto& note : notes) {
        size_t name_len = strlen(note.name) + 1;  // Include null terminator
        size_t name_aligned = align_up(name_len, 4);
        size_t data_aligned = align_up(note.data.size(), 4);
        size_t note_size = 12 + name_aligned + data_aligned;  // 12 = 3 * uint32_t

        size_t offset = combined_notes.size();
        combined_notes.resize(offset + note_size, 0);

        uint32_t namesz = name_len;
        uint32_t descsz = note.data.size();
        uint32_t type = 0;  // Custom note type

        memcpy(&combined_notes[offset + 0], &namesz, 4);
        memcpy(&combined_notes[offset + 4], &descsz, 4);
        memcpy(&combined_notes[offset + 8], &type, 4);
        memcpy(&combined_notes[offset + 12], note.name, name_len);
        if (!note.data.empty()) {
            memcpy(&combined_notes[offset + 12 + name_aligned], note.data.data(), note.data.size());
        }

        printf("  Note '%s': %zu bytes data, %zu bytes total\n",
               note.name, note.data.size(), note_size);
    }

    // Find a PT_NOTE entry (we'll use the LAST PT_NOTE found)
    // SAFETY: Reusing an existing PT_NOTE entry is safe because:
    // 1. We set p_vaddr to a high address past LOAD segments (visible to dl_iterate_phdr)
    // 2. We preserve original note content via deduplication (below)
    // 3. Kernel supports multiple notes within a single PT_NOTE segment
    // 4. Stub binaries typically have .note.gnu.build-id as the last PT_NOTE
    // 5. Node.js SEA needs proper p_vaddr for postject_find_resource() to work
    int last_note_idx = -1;
    for (uint16_t i = 0; i < phnum; i++) {
        uint8_t* phdr = &binary_data[phoff + i * phentsize];
        // Use memcpy for safe unaligned access
        uint32_t p_type;
        memcpy(&p_type, phdr, sizeof(p_type));
        if (p_type == 4) {  // PT_NOTE
            last_note_idx = i;
        }
    }

    if (last_note_idx < 0) {
        fprintf(stderr, "Error: No PT_NOTE entry found in binary\n");
        return -1;
    }

    printf("  Using PT_NOTE entry at index %d\n", last_note_idx);

    // Get the existing PT_NOTE segment info
    uint8_t* target_phdr = &binary_data[phoff + last_note_idx * phentsize];
    // Use memcpy for safe unaligned access
    uint64_t orig_offset, orig_vaddr, orig_filesz;
    memcpy(&orig_offset, target_phdr + 8, sizeof(orig_offset));
    memcpy(&orig_vaddr, target_phdr + 16, sizeof(orig_vaddr));
    memcpy(&orig_filesz, target_phdr + 32, sizeof(orig_filesz));
    printf("  Original PT_NOTE[%d]: offset=0x%lx, vaddr=0x%lx, filesz=0x%lx\n",
           last_note_idx, (unsigned long)orig_offset, (unsigned long)orig_vaddr,
           (unsigned long)orig_filesz);

    // Build a set of note names we're adding (for deduplication)
    std::set<std::string> new_note_names;
    for (const auto& note : notes) {
        new_note_names.insert(note.name);
    }

    // Read existing notes from the PT_NOTE segment and preserve any that
    // don't conflict with our new notes
    std::vector<uint8_t> preserved_notes;
    if (orig_filesz > 0 && orig_offset + orig_filesz <= input_size) {
        const uint8_t* existing_data = binary_data.data() + orig_offset;
        size_t pos = 0;

        printf("  Scanning existing notes for preservation...\n");
        while (pos + 12 <= orig_filesz) {
            // Use memcpy for safe unaligned access (compiler optimizes to efficient code)
            uint32_t namesz, descsz;
            memcpy(&namesz, existing_data + pos, sizeof(namesz));
            memcpy(&descsz, existing_data + pos + 4, sizeof(descsz));
            // uint32_t type; memcpy(&type, existing_data + pos + 8, sizeof(type));

            size_t name_aligned = align_up(namesz, 4);
            size_t desc_aligned = align_up(descsz, 4);
            size_t note_total = 12 + name_aligned + desc_aligned;

            if (pos + note_total > orig_filesz) break;

            // Get note name (may not be null-terminated in struct, but namesz includes null)
            std::string existing_name;
            if (namesz > 0) {
                existing_name.assign(reinterpret_cast<const char*>(existing_data + pos + 12),
                                     namesz > 0 ? namesz - 1 : 0);  // Exclude null terminator
            }

            // Check if this note conflicts with one we're adding
            if (new_note_names.find(existing_name) == new_note_names.end()) {
                // Preserve this note - it doesn't conflict
                printf("    Preserving existing note '%s' (%u bytes)\n",
                       existing_name.c_str(), descsz);
                // Use insert with iterators for better readability and idiomatic C++
                preserved_notes.insert(preserved_notes.end(),
                                      existing_data + pos,
                                      existing_data + pos + note_total);
            } else {
                printf("    Replacing existing note '%s'\n", existing_name.c_str());
            }

            pos += note_total;
        }
    }

    // Combine preserved notes with our new notes
    std::vector<uint8_t> all_notes;
    if (!preserved_notes.empty()) {
        all_notes = std::move(preserved_notes);
    }
    // Append our new notes
    size_t prev_size = all_notes.size();
    all_notes.resize(prev_size + combined_notes.size());
    memcpy(all_notes.data() + prev_size, combined_notes.data(), combined_notes.size());

    // Notes will be appended at end of binary
    size_t notes_total_size = all_notes.size();

    printf("  Combined notes: offset=%lu, size=%zu (preserved + new)\n",
           (unsigned long)input_size, notes_total_size);

    // Check if this is a SMOL compression operation (pressed_data note).
    // SMOL stubs should NEVER extend PT_LOAD - they need unmapped vaddr.
    // Even if the stub has PT_DYNAMIC (e.g., PIE musl stubs), extending PT_LOAD
    // causes the loader to try mapping 26MB at a low address, causing SIGSEGV.
    bool is_smol_compression = false;
    for (const auto& note : notes) {
        if (strcmp(note.name, "pressed_data") == 0) {
            is_smol_compression = true;
            break;
        }
    }

    // Check if binary is dynamically linked (needs PT_LOAD for SEA/dl_iterate_phdr).
    // IMPORTANT: Only check for PT_INTERP, not PT_DYNAMIC!
    // - Static-PIE binaries have PT_DYNAMIC (for TLS/relocations) but NO PT_INTERP
    // - Dynamically linked binaries have BOTH PT_DYNAMIC AND PT_INTERP
    // - Only binaries with PT_INTERP use dl_iterate_phdr() at runtime
    // Also: For SMOL compression, always treat as static regardless
    bool is_dynamic = false;
    if (!is_smol_compression) {
        for (uint16_t i = 0; i < phnum; i++) {
            uint8_t* phdr = &binary_data[phoff + i * phentsize];
            // Use memcpy for safe unaligned access
            uint32_t p_type;
            memcpy(&p_type, phdr, sizeof(p_type));
            if (p_type == 3) {  // PT_INTERP only (not PT_DYNAMIC)
                is_dynamic = true;
                break;
            }
        }
    }

    // For SEA binaries (dynamically linked), dl_iterate_phdr() needs the note data
    // mapped into memory. We extend the last PT_LOAD segment to cover the appended
    // note data, making it accessible at runtime.
    //
    // Strategy:
    // 1. Calculate how far past the last PT_LOAD's file content our notes start
    // 2. Extend that PT_LOAD's filesz/memsz to cover the notes
    // 3. Set PT_NOTE vaddr to point within the extended PT_LOAD region

    // Calculate the file offset where notes will be written (end of original file)
    // and the corresponding virtual address in the extended PT_LOAD
    uint64_t notes_file_offset = input_size;

    // The gap between the last PT_LOAD's file content and our notes
    uint64_t gap_from_load_end = notes_file_offset - (last_load_offset + last_load_filesz);

    // Calculate note_vaddr: it's the last PT_LOAD's vaddr + its original filesz + gap
    // This places the notes at the correct virtual address within the extended segment
    note_vaddr = last_load_vaddr + last_load_filesz + gap_from_load_end;

    printf("  Extending PT_LOAD[%d] to cover note data (SEA compatibility)\n", last_load_idx);
    printf("  Gap from LOAD end to notes: 0x%lx bytes\n", (unsigned long)gap_from_load_end);
    printf("  Note vaddr within extended LOAD: 0x%lx\n", (unsigned long)note_vaddr);

    // Extend the last PT_LOAD segment to cover the appended notes
    if (last_load_idx >= 0 && is_dynamic) {
        uint8_t* load_phdr = &binary_data[phoff + last_load_idx * phentsize];

        // New sizes: original size + gap + notes
        uint64_t new_load_filesz = last_load_filesz + gap_from_load_end + notes_total_size;
        uint64_t new_load_memsz = last_load_memsz + gap_from_load_end + notes_total_size;

        // Update PT_LOAD filesz and memsz
        memcpy(load_phdr + 32, &new_load_filesz, 8);  // p_filesz
        memcpy(load_phdr + 40, &new_load_memsz, 8);   // p_memsz

        printf("  Extended PT_LOAD[%d]: filesz 0x%lx -> 0x%lx, memsz 0x%lx -> 0x%lx\n",
               last_load_idx,
               (unsigned long)last_load_filesz, (unsigned long)new_load_filesz,
               (unsigned long)last_load_memsz, (unsigned long)new_load_memsz);
    } else if (!is_dynamic) {
        // For static binaries or SMOL stubs, use a high virtual address
        // that's past all LOAD segments but doesn't need actual mapping.
        // SMOL stubs (even with PT_DYNAMIC) must use this path because extending
        // PT_LOAD would cause the loader to map 26MB+ at a low address -> SIGSEGV.
        note_vaddr = 0x10000000 + align_up(input_size, 0x1000);
        if (is_smol_compression) {
            printf("  SMOL compression - using unmapped vaddr: 0x%lx (no PT_LOAD extension)\n",
                   (unsigned long)note_vaddr);
        } else {
            printf("  Static binary - using unmapped vaddr: 0x%lx\n", (unsigned long)note_vaddr);
        }
    }

    // Update PT_NOTE segment to point to our appended notes
    {
        uint32_t new_flags = 4;    // PF_R (readable)
        uint64_t new_offset = notes_file_offset;
        uint64_t new_paddr = note_vaddr;
        uint64_t new_filesz = notes_total_size;
        uint64_t new_memsz = notes_total_size;
        uint64_t new_align = 4;

        memcpy(target_phdr + 4, &new_flags, 4);
        memcpy(target_phdr + 8, &new_offset, 8);
        memcpy(target_phdr + 16, &note_vaddr, 8);
        memcpy(target_phdr + 24, &new_paddr, 8);
        memcpy(target_phdr + 32, &new_filesz, 8);
        memcpy(target_phdr + 40, &new_memsz, 8);
        memcpy(target_phdr + 48, &new_align, 8);

        printf("  Modified PT_NOTE[%d]: offset=0x%lx, vaddr=0x%lx, filesz=0x%lx\n",
               last_note_idx, (unsigned long)new_offset, (unsigned long)note_vaddr,
               (unsigned long)new_filesz);

        // Combine binary data and notes into single buffer for atomic write
        size_t total_size = input_size + notes_total_size;
        std::vector<uint8_t> combined_data;
        combined_data.reserve(total_size);
        combined_data.insert(combined_data.end(), binary_data.begin(), binary_data.begin() + input_size);
        combined_data.insert(combined_data.end(), all_notes.begin(), all_notes.end());

        // Write combined data atomically using cross-platform helper with detailed error logging
        if (write_file_atomically(output_path.c_str(), combined_data.data(), combined_data.size(), 0755) == -1) {
            return -1;
        }
        set_executable_permissions(output_path.c_str());

        printf("  Successfully wrote binary with %zu notes (PHT unchanged at offset %lu)\n",
               notes.size(), (unsigned long)phoff);
        printf("  Output size: %lu bytes\n",
               (unsigned long)(input_size + notes_total_size));
        if (is_dynamic) {
            printf("  Note data mapped via extended PT_LOAD (SEA compatible)\n");
        }
    }

    return 0;
}

/**
 * Write ELF binary with raw note appending (no PHT relocation) - single note.
 *
 * ============================================================================
 * SMOL REPACK SINGLE NOTE - CONVENIENCE WRAPPER
 * ============================================================================
 *
 * This is a convenience wrapper around smol_reuse_multi_ptnote() for
 * single-note use (e.g., binpress initial compression).
 *
 * REUSES an existing PT_NOTE entry - only for SMOL notes.
 * SMOL compressed data storage:
 *   Mach-O: SMOL/__PRESSED_DATA        (segment/section)
 *   ELF:    PT_NOTE with owner "pressed_data" (LIEF creates .note.pressed_data section)
 *   PE:     .pressed_data              (section only - no segments in PE)
 *
 * For static glibc binaries, PHT MUST stay at the original offset because glibc
 * reads PHT from base+phoff in memory, and moving it causes SIGSEGV.
 *
 * @param stub_path Path to the original stub binary
 * @param output_path Path to write the output binary
 * @param note_name SMOL note owner name (e.g., "pressed_data")
 * @param note_data Note data to append
 * @return 0 on success, -1 on error
 */
inline int smol_reuse_single_ptnote(
    const std::string& stub_path,
    const std::string& output_path,
    const char* note_name,
    const std::vector<uint8_t>& note_data
) {
    std::vector<NoteEntry> notes;
    notes.emplace_back(note_name, note_data);
    return smol_reuse_multi_ptnote(stub_path, output_path, notes, nullptr);
}

} // namespace elf_note_utils

#endif // ELF_NOTE_UTILS_RAW_WRITE_HPP

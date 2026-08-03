/**
 * elf_note_utils_lief_write.hpp — the LIEF/postject note writer.
 *
 * One unit of the elf_note_utils.hpp umbrella: approach 2 of the two the
 * umbrella describes. Creates NEW page-aligned PT_LOAD + PT_NOTE segments the
 * way postject does, so dl_iterate_phdr() (and therefore
 * postject_find_resource()) can reach the note at runtime. The program header
 * table may be relocated, which is fine for dynamically linked binaries and
 * fatal for static glibc — that case belongs to
 * elf_note_utils_raw_write.hpp.
 *
 * The two segment fixups live here rather than with the note CRUD because both
 * exist purely to survive a LIEF write: LIEF emits PT_NOTE with p_vaddr=0 and
 * no matching PT_LOAD. write_with_notes_raw() is the bridge back to the RAW
 * approach for notes larger than LIEF's builder handles. Include the umbrella,
 * not this file.
 */

#ifndef ELF_NOTE_UTILS_LIEF_WRITE_HPP
#define ELF_NOTE_UTILS_LIEF_WRITE_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <LIEF/LIEF.hpp>

#include "socketsecurity/build-infra/file_io_common.h"
#include "socketsecurity/bin-infra/elf_note_utils_raw_probe.hpp"
#include "socketsecurity/bin-infra/elf_note_utils_raw_write.hpp"
#include "socketsecurity/bin-infra/elf_note_utils_types.hpp"

namespace elf_note_utils {

// Forward declaration for write_with_notes_raw
inline void write_with_notes(LIEF::ELF::Binary* binary, const std::string& output_path);

/**
 * Create matching PT_LOAD segment for PT_NOTE (postject compatibility).
 *
 * ============================================================================
 * PART OF LIEF/POSTJECT APPROACH - NOT USED BY RAW/SMOL APPROACH
 * ============================================================================
 *
 * This is called by write_with_notes() for postject-compatible injection.
 * The SMOL approach (smol_reuse_multi_ptnote) extends an existing PT_LOAD
 * instead of creating a new one.
 *
 * WHY THIS IS NEEDED:
 * Postject creates BOTH PT_LOAD and PT_NOTE segments pointing to the same region:
 * - PT_LOAD: Provides memory mapping so note data is accessible at runtime
 * - PT_NOTE: Provides dl_iterate_phdr() access for postject_find_resource()
 *
 * This function mimics postject's behavior by adding a PT_LOAD segment that
 * covers the same region as the PT_NOTE segment containing our injected notes.
 *
 * @param binary ELF binary to fix
 */
inline void add_matching_load_for_notes(LIEF::ELF::Binary* binary) {
    // Find the highest file offset and vaddr used by LOAD segments
    uint64_t max_load_file_end = 0;
    uint64_t max_load_vaddr_end = 0;
    for (auto& seg : binary->segments()) {
        if (seg.type() == LIEF::ELF::Segment::TYPE::LOAD) {
            uint64_t file_end = seg.file_offset() + seg.physical_size();
            uint64_t vaddr_end = seg.virtual_address() + seg.virtual_size();
            if (file_end > max_load_file_end) {
                max_load_file_end = file_end;
            }
            if (vaddr_end > max_load_vaddr_end) {
                max_load_vaddr_end = vaddr_end;
            }
        }
    }

    // Find PT_NOTE segments that contain our SEA/VFS notes
    for (auto& seg : binary->segments()) {
        if (seg.type() == LIEF::ELF::Segment::TYPE::NOTE) {
            // Check if this NOTE segment contains our custom notes
            bool has_sea_note = false;
            for (const auto& note : binary->notes()) {
                if (note.name() == "NODE_SEA_BLOB" || note.name() == "SMOL_VFS_BLOB") {
                    has_sea_note = true;
                    break;
                }
            }

            if (has_sea_note && seg.virtual_address() != 0) {
                // Create a matching PT_LOAD segment for memory mapping
                // Use page-aligned size to ensure proper loading
                uint64_t load_offset = seg.file_offset();
                uint64_t load_vaddr = seg.virtual_address();
                uint64_t load_size = align_up(seg.physical_size(), 0x1000);

                LIEF::ELF::Segment load_seg;
                load_seg.type(LIEF::ELF::Segment::TYPE::LOAD);
                load_seg.flags(LIEF::ELF::Segment::FLAGS::R);  // Read-only
                load_seg.file_offset(load_offset);
                load_seg.virtual_address(load_vaddr);
                load_seg.physical_address(load_vaddr);
                load_seg.physical_size(load_size);
                load_seg.virtual_size(load_size);
                load_seg.alignment(0x1000);  // Page alignment

                binary->add(load_seg);
                printf("  Added PT_LOAD for notes: offset=0x%lx, vaddr=0x%lx, size=0x%lx\n",
                       (unsigned long)load_offset, (unsigned long)load_vaddr,
                       (unsigned long)load_size);
                break;  // Only need one LOAD for notes
            }
        }
    }
}

/**
 * Fix PT_NOTE segment virtual addresses to make them visible to dl_iterate_phdr().
 *
 * LIEF creates PT_NOTE segments with p_vaddr=0, which makes them invisible to
 * dl_iterate_phdr() that Node.js SEA uses (postject_find_resource).
 * This function sets proper non-zero virtual addresses for PT_NOTE segments.
 *
 * Strategy:
 * - Find the highest LOAD segment end address
 * - Place PT_NOTE segments starting after that, with 4KB page alignment
 *
 * @param binary ELF binary to fix
 */
inline void fix_note_segment_vaddrs(LIEF::ELF::Binary* binary) {
    // Find the highest address used by LOAD segments
    uint64_t max_load_end = 0;
    for (auto& seg : binary->segments()) {
        if (seg.type() == LIEF::ELF::Segment::TYPE::LOAD) {
            uint64_t seg_end = seg.virtual_address() + seg.virtual_size();
            if (seg_end > max_load_end) {
                max_load_end = seg_end;
            }
        }
    }

    // Start placing PT_NOTE segments after LOAD segments, with 4KB page alignment
    uint64_t next_vaddr = align_up(max_load_end, 0x1000);

    // Fix PT_NOTE segments that have p_vaddr=0
    int fixed_count = 0;
    for (auto& seg : binary->segments()) {
        if (seg.type() == LIEF::ELF::Segment::TYPE::NOTE && seg.virtual_address() == 0) {
            seg.virtual_address(next_vaddr);
            seg.physical_address(next_vaddr);
            printf("  Fixed PT_NOTE segment: set p_vaddr=0x%lx (was 0x0)\n",
                   (unsigned long)next_vaddr);
            next_vaddr = align_up(next_vaddr + seg.physical_size(), 0x1000);
            fixed_count++;
        }
    }

    if (fixed_count > 0) {
        printf("  Fixed %d PT_NOTE segment(s) to be visible to dl_iterate_phdr()\n", fixed_count);
    }
}

/**
 * Write ELF binary using raw binary manipulation (preferred for large notes).
 *
 * This function appends notes to the end of the file and updates the PT_NOTE
 * segment to point to them. For SEA binaries (dynamically linked), postject
 * adds both PT_LOAD and PT_NOTE segments - we currently just update PT_NOTE.
 *
 * NOTE: This may not work for all SEA use cases since the note data won't
 * be mapped into memory. For full SEA compatibility with dl_iterate_phdr,
 * consider using postject which properly handles PT_LOAD segments.
 *
 * @param binary ELF binary with notes added via LIEF API
 * @param input_path Path to the original input binary
 * @param output_path Path to write the output binary
 * @return 0 on success, -1 on error
 */
inline int write_with_notes_raw(
    LIEF::ELF::Binary* binary,
    const std::string& input_path,
    const std::string& output_path
) {
    // Extract notes from LIEF binary and write using raw approach
    std::vector<NoteEntry> notes;

    for (const auto& note : binary->notes()) {
        const std::string& name = note.name();
        const auto& desc = note.description();

        // Only include custom notes we care about (SEA/VFS)
        if (name == "NODE_SEA_BLOB" || name == "SMOL_VFS_BLOB") {
            printf("  Extracting note '%s' (%zu bytes) for raw write...\n",
                   name.c_str(), desc.size());
            // Convert span to vector
            std::vector<uint8_t> desc_vec(desc.begin(), desc.end());
            notes.emplace_back(name.c_str(), desc_vec);
        }
    }

    if (notes.empty()) {
        fprintf(stderr, "Error: No notes found in binary for raw write\n");
        return -1;
    }

    // Use flip_sea_fuse_raw callback if we have a SEA blob
    BinaryModifyCallback fuse_callback = nullptr;
    for (const auto& note : notes) {
        if (strcmp(note.name, "NODE_SEA_BLOB") == 0 && !note.data.empty()) {
            fuse_callback = flip_sea_fuse_raw;
            break;
        }
    }

    return smol_reuse_multi_ptnote(input_path, output_path, notes, fuse_callback);
}

/**
 * Write ELF binary with minimal config for PT_NOTE operations (LIEF builder).
 *
 * ============================================================================
 * LIEF APPROACH - POSTJECT-COMPATIBLE (DYNAMICALLY LINKED BINARIES)
 * ============================================================================
 *
 * This is the postject-compatible approach for Node.js SEA injection.
 * DO NOT use this for SMOL stubs - use smol_reuse_multi_ptnote() instead.
 *
 * WHY THIS APPROACH EXISTS:
 * Postject uses LIEF's Note API to create both PT_LOAD and PT_NOTE segments
 * pointing to the same location. dl_iterate_phdr() needs notes mapped via
 * PT_LOAD to access them at runtime (postject_find_resource).
 *
 * ============================================================================
 * HOW THIS ALIGNS WITH POSTJECT
 * ============================================================================
 *
 * POSTJECT BEHAVIOR:
 *   LIEF::ELF::Note note;
 *   note.name(note_name);
 *   note.description(data);
 *   binary->add(note);
 *   binary->raw();  // Creates PT_LOAD + PT_NOTE at same offset
 *
 * BINJECT BEHAVIOR (this function):
 *   Note::create() + binary->add()  // Same as postject
 *   write_with_notes()              // LIEF builder with proper config
 *
 * RESULTING STRUCTURE:
 * - NEW PT_LOAD segment created (not reusing existing)
 * - PT_LOAD: offset=0x7240000, vaddr=0xf240000, size=0x10000 (page-aligned)
 * - PT_NOTE: offset=0x7240000, vaddr=0xf240000, size=actual_note_size
 * - Both point to same location, allowing dl_iterate_phdr() access
 *
 * ============================================================================
 * HOW THIS DIFFERS FROM SMOL REPACK APPROACH
 * ============================================================================
 *
 * SMOL REPACK (smol_reuse_multi_ptnote):
 * - REUSES existing PT_NOTE entry (modifies in-place)
 * - Only for SMOL notes (pressed_data, etc.)
 * - EXTENDS last PT_LOAD to cover appended note data
 * - PHT stays at ORIGINAL location (CRITICAL for static glibc)
 *
 * LIEF/POSTJECT (this function):
 * - Creates NEW PT_LOAD + NEW PT_NOTE segments
 * - Page-aligned at same offset/vaddr
 * - PHT may be relocated (OK for dynamic binaries)
 *
 * ============================================================================
 * IMPLEMENTATION DETAILS
 * ============================================================================
 *
 * WARNING: LIEF's builder may truncate large notes (~1MB limit observed).
 * For notes larger than 1MB, use write_with_notes_raw() instead.
 *
 * CRITICAL: This is the correct way to write ELF binaries after modifying
 * PT_NOTE segments. Using binary->raw() or write() without proper config
 * causes VirtAddr=0 on note sections, leading to segfaults.
 *
 * The config disables all Builder processing except notes, which ensures:
 * 1. PT_NOTE segments are properly constructed in the PHT
 * 2. Section VirtAddr is correctly set (not 0)
 * 3. Other binary structures remain untouched
 *
 * TRIPLE-WRITE PATTERN:
 * This function performs three writes to work around LIEF quirks:
 * 1. First write: Properly constructs PT_NOTE segments (config.notes=true)
 * 2. Re-parse and fix: Removes ALLOC flag from sections with VirtAddr=0
 *    (LIEF adds ALLOC+VirtAddr=0, which causes kernel crashes - see Rust #26764)
 * 3. Fix PT_NOTE p_vaddr: Sets non-zero virtual addresses for PT_NOTE segments
 *    (LIEF creates p_vaddr=0, making them invisible to dl_iterate_phdr())
 * 4. Third write: Preserves all fixes while maintaining PT_NOTE integrity
 *    (MUST use config.notes=true to prevent PT_NOTE corruption/segfaults)
 *
 * LIEF VERSION NOTE:
 * This code has been tested with LIEF 0.14.x. When upgrading LIEF, verify:
 * 1. Builder respects section flags during write (ALLOC removal preserved)
 * 2. config.notes=true still properly constructs PT_NOTE segments
 * 3. The double-write pattern still prevents ALLOC+VirtAddr=0 crashes
 * 4. Second write with notes=true does NOT undo the ALLOC flag fixes
 * 5. PT_NOTE segment p_vaddr modifications are preserved
 *
 * @param binary ELF binary to write
 * @param output_path Path to write the binary to
 */
inline void write_with_notes(
    LIEF::ELF::Binary* binary,
    const std::string& output_path
) {
    // FIRST: Fix PT_NOTE segment virtual addresses before first write
    // This must happen before any writes because LIEF creates segments with p_vaddr=0
    fix_note_segment_vaddrs(binary);

    // SECOND: Add matching PT_LOAD segment for SEA/VFS notes (postject compatibility)
    // This allows dl_iterate_phdr() to access the note data at runtime
    add_matching_load_for_notes(binary);

    LIEF::ELF::Builder::config_t config;
    config.dt_hash = false;
    config.dyn_str = false;
    config.dynamic_section = false;
    config.fini_array = false;
    config.gnu_hash = false;
    config.init_array = false;
    config.interpreter = false;
    config.jmprel = false;
    config.notes = true;  // MUST be true to properly write PT_NOTE segments
    config.preinit_array = false;
    config.relr = false;
    config.android_rela = false;
    config.rela = false;
    config.static_symtab = false;
    config.sym_verdef = false;
    config.sym_verneed = false;
    config.sym_versym = false;
    config.symtab = false;
    config.coredump_notes = false;
    config.force_relocate = false;
    config.skip_dynamic = true;

    binary->write(output_path, config);

    // Sync to disk (LIEF doesn't fsync internally)
    if (fsync_file_by_path(output_path.c_str()) != FILE_IO_OK) {
        fprintf(stderr, "Error: Failed to sync LIEF output to disk: %s\n", output_path.c_str());
        return;
    }

    // Fix: Re-parse the written binary and remove ALLOC flag from note sections
    // that have VirtAddr=0. LIEF creates these sections with ALLOC but VirtAddr=0,
    // which causes the loader to crash when trying to map them to address 0.
    // See: https://github.com/rust-lang/rust/issues/26764
    auto fixed = LIEF::ELF::Parser::parse(output_path);
    if (fixed) {
        bool modified = false;
        for (auto& sec : fixed->sections()) {
            // Check for note sections with ALLOC flag and VirtAddr=0
            if (sec.type() == LIEF::ELF::Section::TYPE::NOTE &&
                sec.virtual_address() == 0) {
                auto flags = sec.flags();
                if (static_cast<uint64_t>(flags) & static_cast<uint64_t>(LIEF::ELF::Section::FLAGS::ALLOC)) {
                    sec.remove(LIEF::ELF::Section::FLAGS::ALLOC);
                    printf("  Fixed: Removed ALLOC flag from %s (VirtAddr=0)\n", sec.name().c_str());
                    modified = true;
                }
            }
        }
        if (modified) {
            // Fix PT_NOTE segment virtual addresses again (re-parsed binary loses the fix)
            fix_note_segment_vaddrs(fixed.get());

            // Re-add matching PT_LOAD for SEA/VFS notes (postject compatibility)
            add_matching_load_for_notes(fixed.get());

            // Write with minimal config to preserve the binary structure.
            // CRITICAL: Third write to apply both ALLOC flag fixes AND p_vaddr fixes.
            LIEF::ELF::Builder::config_t fix_config;
            fix_config.dt_hash = false;
            fix_config.dyn_str = false;
            fix_config.dynamic_section = false;
            fix_config.fini_array = false;
            fix_config.gnu_hash = false;
            fix_config.init_array = false;
            fix_config.interpreter = false;
            fix_config.jmprel = false;
            // CRITICAL: notes MUST be true here! If false, LIEF skips PT_NOTE
            // segment construction, corrupting the Program Header Table and
            // causing SIGSEGV (exit 139) when the binary executes.
            // Setting to true ensures:
            // 1. PT_NOTE segments are properly serialized to the output
            // 2. PHT entries for PT_NOTE are valid and complete
            // 3. ALLOC flag fixes (from above) are preserved (orthogonal operation)
            // 4. PT_NOTE p_vaddr fixes make segments visible to dl_iterate_phdr()
            fix_config.notes = true;
            fix_config.preinit_array = false;
            fix_config.relr = false;
            fix_config.android_rela = false;
            fix_config.rela = false;
            fix_config.static_symtab = false;
            fix_config.sym_verdef = false;
            fix_config.sym_verneed = false;
            fix_config.sym_versym = false;
            fix_config.symtab = false;
            fix_config.coredump_notes = false;
            fix_config.force_relocate = false;
            fix_config.skip_dynamic = true;
            fixed->write(output_path, fix_config);

            // Sync to disk (LIEF doesn't fsync internally)
            if (fsync_file_by_path(output_path.c_str()) != FILE_IO_OK) {
                fprintf(stderr, "Error: Failed to sync LIEF output to disk: %s\n", output_path.c_str());
                return;
            }
        }
    }
}

} // namespace elf_note_utils

#endif // ELF_NOTE_UTILS_LIEF_WRITE_HPP

/**
 * elf_note_utils_lief_notes.hpp — PT_NOTE create/remove/exists/replace via LIEF.
 *
 * One unit of the elf_note_utils.hpp umbrella: the note-level CRUD on an
 * already-parsed LIEF::ELF::Binary. These only mutate the in-memory model —
 * getting the result onto disk is elf_note_utils_lief_write.hpp's job. Include
 * the umbrella, not this file.
 */

#ifndef ELF_NOTE_UTILS_LIEF_NOTES_HPP
#define ELF_NOTE_UTILS_LIEF_NOTES_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <LIEF/LIEF.hpp>

namespace elf_note_utils {

/**
 * Create and add a PT_NOTE to an ELF binary.
 *
 * This handles the LIEF #1026 fix where section_name must be specified
 * in the format ".note.<owner_name>" for custom notes to serialize.
 *
 * @param binary ELF binary to add note to
 * @param note_name Note owner name (e.g., "NODE_SEA_BLOB")
 * @param data Note data/description
 * @return 0 on success, -1 on error
 */
inline int create_and_add(
    LIEF::ELF::Binary* binary,
    const char* note_name,
    const std::vector<uint8_t>& data
) {
    if (!binary || !note_name) {
        fprintf(stderr, "Error: Invalid arguments to create_and_add\n");
        return -1;
    }

    // CRITICAL: Must specify section_name for LIEF serialization
    // Format: .note.<owner_name> (e.g., ".note.NODE_SEA_BLOB")
    std::string section_name = std::string(".note.") + note_name;

    // Create note using factory method
    auto note = LIEF::ELF::Note::create(
        note_name,                              // name (owner)
        uint32_t(0),                            // type (0 for custom notes)
        data,                                   // description (data)
        section_name,                           // section_name (required)
        LIEF::ELF::Header::FILE_TYPE::NONE,     // ftype
        LIEF::ELF::ARCH::NONE,                  // arch
        LIEF::ELF::Header::CLASS::NONE          // cls
    );

    if (!note) {
        fprintf(stderr, "Error: Failed to create PT_NOTE for '%s'\n", note_name);
        fprintf(stderr, "  Note owner: %s\n", note_name);
        fprintf(stderr, "  Data size: %zu bytes\n", data.size());
        fprintf(stderr, "  Section: %s\n", section_name.c_str());
        fprintf(stderr, "  This indicates LIEF Note::create() failed\n");
        return -1;
    }

    binary->add(*note);

    // Remove ALLOC flag from the new note section.
    // LIEF creates note sections with SHF_ALLOC and VirtAddr=0, which causes
    // the kernel to try mapping the section to address 0, resulting in SIGSEGV.
    // Since we read the note data using file offsets (not virtual addresses),
    // the ALLOC flag is unnecessary. Removing it prevents the loader crash.
    // See: https://github.com/rust-lang/rust/issues/26764
    //
    // Note: We search for the section by checking if its name contains our note name,
    // because LIEF may store the name differently.
    bool found = false;
    for (auto& sec : binary->sections()) {
        // Check if this is our note section (by substring match or exact match)
        if (sec.name() == section_name ||
            sec.name().find(note_name) != std::string::npos) {
            auto flags = sec.flags();
            if (static_cast<uint64_t>(flags) & static_cast<uint64_t>(LIEF::ELF::Section::FLAGS::ALLOC)) {
                sec.remove(LIEF::ELF::Section::FLAGS::ALLOC);
                printf("  Removed ALLOC flag from %s section\n", sec.name().c_str());
                found = true;
            }
            break;
        }
    }
    if (!found) {
        printf("  Warning: Could not find section %s to remove ALLOC flag\n", section_name.c_str());
        printf("  Sections in binary:\n");
        for (const auto& sec : binary->sections()) {
            if (sec.name().find("note") != std::string::npos ||
                sec.name().find("pressed") != std::string::npos) {
                printf("    - %s\n", sec.name().c_str());
            }
        }
    }

    return 0;
}

/**
 * Remove all PT_NOTE segments with matching name.
 * Safe against iterator invalidation.
 *
 * @param binary ELF binary to remove notes from
 * @param note_name Note owner name to match
 */
inline void remove_all(
    LIEF::ELF::Binary* binary,
    const char* note_name
) {
    if (!binary || !note_name) {
        return;
    }

    // Safe iterator removal pattern - restart after each removal
    bool found;
    do {
        found = false;
        for (auto& note : binary->notes()) {
            if (note.name() == note_name) {
                binary->remove(note);
                found = true;
                break;  // Restart iteration after removal
            }
        }
    } while (found);
}

/**
 * Check if PT_NOTE with given name exists.
 *
 * @param binary ELF binary to check
 * @param note_name Note owner name to find
 * @return true if note exists, false otherwise
 */
inline bool exists(
    LIEF::ELF::Binary* binary,
    const char* note_name
) {
    if (!binary || !note_name) {
        return false;
    }

    for (const auto& note : binary->notes()) {
        if (note.name() == note_name) {
            return true;
        }
    }
    return false;
}

/**
 * Remove and replace (or just add if not exists) a PT_NOTE.
 *
 * This is the common pattern for updating PT_NOTE content:
 * 1. Check if note exists
 * 2. Remove if exists
 * 3. Add new note with updated content
 *
 * @param binary ELF binary to update
 * @param note_name Note owner name
 * @param data New note data
 * @return 0 on success, -1 on error
 */
inline int replace_or_add(
    LIEF::ELF::Binary* binary,
    const char* note_name,
    const std::vector<uint8_t>& data
) {
    if (!binary || !note_name) {
        fprintf(stderr, "Error: Invalid arguments to replace_or_add\n");
        return -1;
    }

    // Check if note exists
    bool note_exists = exists(binary, note_name);

    if (note_exists) {
        printf("  Found existing %s PT_NOTE, removing and recreating...\n", note_name);
        remove_all(binary, note_name);
        printf("  Removed old %s PT_NOTE\n", note_name);
    } else {
        printf("  No existing %s PT_NOTE found, creating new one...\n", note_name);
    }

    // Add new note
    return create_and_add(binary, note_name, data);
}

} // namespace elf_note_utils

#endif // ELF_NOTE_UTILS_LIEF_NOTES_HPP

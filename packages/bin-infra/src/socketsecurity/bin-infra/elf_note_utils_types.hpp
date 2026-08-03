/**
 * elf_note_utils_types.hpp — shared vocabulary for the ELF note helpers.
 *
 * One unit of the elf_note_utils.hpp umbrella: the alignment helper, the note
 * record every approach passes around, and the in-memory modify hook. Nothing
 * here touches a file or LIEF — include the umbrella, not this file.
 */

#ifndef ELF_NOTE_UTILS_TYPES_HPP
#define ELF_NOTE_UTILS_TYPES_HPP

#include <cstdint>
#include <vector>

namespace elf_note_utils {

/**
 * Align a value up to the specified alignment.
 */
inline uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

/**
 * Structure representing a note to be added to an ELF binary.
 */
struct NoteEntry {
    const char* name;
    std::vector<uint8_t> data;

    NoteEntry(const char* n, const std::vector<uint8_t>& d) : name(n), data(d) {}
    NoteEntry(const char* n, const uint8_t* d, size_t sz) : name(n), data(d, d + sz) {}
};

/**
 * Callback for in-memory binary modifications (e.g., fuse flipping).
 * Called after reading the binary into memory, before writing.
 *
 * @param data Pointer to binary data in memory
 * @param size Size of binary data
 * @return 0 on success, -1 on error
 */
using BinaryModifyCallback = int (*)(uint8_t* data, size_t size);

} // namespace elf_note_utils

#endif // ELF_NOTE_UTILS_TYPES_HPP

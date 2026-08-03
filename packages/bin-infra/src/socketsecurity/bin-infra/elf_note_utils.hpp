/**
 * ELF PT_NOTE utilities for DRY code
 *
 * Shared helpers for creating, removing, and replacing PT_NOTE segments
 * across binpress, binject, and bin-infra.
 *
 * ============================================================================
 * TWO APPROACHES FOR ELF NOTE INJECTION
 * ============================================================================
 *
 * This header provides two distinct approaches for injecting PT_NOTE segments:
 *
 * 1. RAW APPROACH (SMOL STUBS) - smol_reuse_multi_ptnote()
 *    - For STATICALLY LINKED glibc binaries (SMOL stubs)
 *    - Preserves PHT at original offset (CRITICAL for static glibc)
 *    - SMOL compressed data storage:
 *        Mach-O: SMOL/__PRESSED_DATA        (segment/section)
 *        ELF:    PT_NOTE with owner "pressed_data" (LIEF creates .note.pressed_data section)
 *        PE:     .pressed_data              (section only - no segments in PE)
 *    - Appends notes to end of file, modifies existing PT_NOTE in-place
 *    - Extends last PT_LOAD to cover appended note data
 *    - glibc reads PHT from base+phoff; moving PHT causes SIGSEGV
 *
 * 2. LIEF APPROACH (POSTJECT-COMPATIBLE) - write_with_notes()
 *    - For DYNAMICALLY LINKED binaries (Node.js SEA, etc.)
 *    - Matches postject's behavior: creates NEW PT_LOAD + PT_NOTE segments
 *    - LIEF creates both segments at same offset/vaddr (page-aligned)
 *    - Required for dl_iterate_phdr() / postject_find_resource()
 *    - PHT may be relocated (acceptable for dynamic binaries)
 *
 * ============================================================================
 * WHEN TO USE EACH APPROACH
 * ============================================================================
 *
 * USE RAW APPROACH FOR:
 * - SMOL stubs (statically linked with glibc)
 * - Binaries where PHT MUST stay at original offset
 * - binpress single-file executables
 *
 * USE LIEF APPROACH FOR:
 * - Node.js SEA injection
 * - Dynamically linked binaries
 * - Postject compatibility requirements
 * - binject SEA/VFS injection
 *
 * ============================================================================
 * SPLIT LAYOUT — ONE DOMAIN OR PHASE PER UNIT
 * ============================================================================
 *
 * This file is the umbrella. Include it — the units below are an internal
 * layout, not five APIs, and every name in namespace elf_note_utils keeps this
 * header as its include location:
 *
 *   elf_note_utils_types.hpp       align_up(), NoteEntry,
 *                                  BinaryModifyCallback — the shared
 *                                  vocabulary, no I/O.
 *   elf_note_utils_raw_probe.hpp   LIEF-free work on raw bytes:
 *                                  is_dynamically_linked_elf() (picks the
 *                                  approach), flip_sea_fuse_raw().
 *   elf_note_utils_raw_write.hpp   Approach 1 above: smol_reuse_multi_ptnote()
 *                                  and its single-note wrapper.
 *   elf_note_utils_lief_notes.hpp  PT_NOTE CRUD on a parsed binary:
 *                                  create_and_add(), remove_all(), exists(),
 *                                  replace_or_add().
 *   elf_note_utils_lief_write.hpp  Approach 2 above: the segment fixups LIEF
 *                                  writes need, plus write_with_notes() and
 *                                  write_with_notes_raw().
 *
 * Include order is load-bearing: the LIEF writer calls into the raw writer and
 * the raw probe, so both must be included before it.
 * ============================================================================
 */

#ifndef ELF_NOTE_UTILS_HPP
#define ELF_NOTE_UTILS_HPP

#include <LIEF/LIEF.hpp>
#include <set>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "socketsecurity/build-infra/file_io_common.h"
#include "socketsecurity/build-infra/file_utils.h"
#include "socketsecurity/bin-infra/segment_names.h"

// Fuse string constants (must match segment_names.h)
#ifndef NODE_SEA_FUSE_UNFLIPPED
#define NODE_SEA_FUSE_UNFLIPPED "NODE_SEA_FUSE_fce680ab2cc467b6e072b8b5df1996b2:0"
#endif

#include "socketsecurity/bin-infra/elf_note_utils_types.hpp"
#include "socketsecurity/bin-infra/elf_note_utils_raw_probe.hpp"
#include "socketsecurity/bin-infra/elf_note_utils_raw_write.hpp"
#include "socketsecurity/bin-infra/elf_note_utils_lief_notes.hpp"
#include "socketsecurity/bin-infra/elf_note_utils_lief_write.hpp"

#endif // ELF_NOTE_UTILS_HPP

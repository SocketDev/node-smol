/**
 * @file smol_segment_reader.c
 * @brief Shared SMOL segment reading utilities implementation.
 *
 * Cross-platform support for Windows, macOS, and Linux.
 *
 * ============================================================================
 * SPLIT LAYOUT — ONE FORMAT OR PHASE PER PART
 * ============================================================================
 *
 * The implementation lives in sibling part files, each owning one executable
 * format or one phase of the read:
 *
 *   smol_segment_reader_internal.h  Includes shared by every part.
 *   smol_segment_reader_metadata.c  Format-agnostic metadata record: read,
 *                                   validate, find the marker in a buffer.
 *   smol_segment_reader_macho.c     Mach-O header walks (SMOL/__PRESSED_DATA,
 *                                   __DATA/__smol_node_ver).
 *   smol_segment_reader_elf.c       ELF header walks (PT_NOTE marker,
 *                                   SMOL_NODE_VER section).
 *   smol_segment_reader_pe.c        PE header walks (.PRESSED_DATA,
 *                                   SMOL_NODE_VER, VS_VERSION_INFO resource).
 *   smol_segment_reader_detect.c    LIEF-free PRESSED_DATA presence checks.
 *   smol_segment_reader_version.c   Node.js version extraction ladder.
 *
 * Include order is load-bearing: the version part calls the per-format section
 * finders, so the three format parts must be included before it.
 *
 * ============================================================================
 * WHY THE PARTS ARE BODY INCLUDES, NOT SIBLING TRANSLATION UNITS
 * ============================================================================
 *
 * This file's path is hardcoded as a compiled source in build definitions that
 * live OUTSIDE this package: bin-stub-builder, binflate, binject, binpress and
 * node-smol-builder each name it in their per-platform Makefiles, and
 * node-smol-builder's node.gyp patch (004-node-gyp-smol-sources.patch) names it
 * in node's own source list. None of those lists globs, so promoting the parts
 * to standalone translation units would silently drop their symbols from five
 * sibling packages plus the node build.
 *
 * Keeping the parts as body includes of this one translation unit means the
 * object file, its symbol set, and every build definition stay exactly as they
 * were, while no single file exceeds the native-source line cap. Statics stay
 * file-local because there is still only one translation unit. This mirrors
 * binpress's binpress_main.c, which the three platform binaries include the
 * same way.
 *
 * Do NOT add a part file to any build definition's source list — doing so
 * compiles it twice and the link fails on duplicate symbols.
 */

#include "socketsecurity/bin-infra/smol_segment_reader_internal.h"

/* Format-agnostic metadata record. */
#include "smol_segment_reader_metadata.c"

/* Per-format header parsing. Must precede the version part. */
#include "smol_segment_reader_macho.c"
#include "smol_segment_reader_elf.c"
#include "smol_segment_reader_pe.c"

/* Detection and version phases, built on the parsers above. */
#include "smol_segment_reader_detect.c"
#include "smol_segment_reader_version.c"

/**
 * @file smol_segment_reader_internal.h
 * @brief Internal includes shared by the smol_segment_reader.c parts.
 *
 * NOT a public API. Nothing outside the smol_segment_reader.c parts should
 * include this header — public declarations live in smol_segment_reader.h
 * (metadata readers) and smol_detect.h (PRESSED_DATA detection).
 *
 * This header exists so each part file (smol_segment_reader_metadata.c,
 * _macho.c, _elf.c, _pe.c, _detect.c, _version.c) states its own dependencies
 * instead of inheriting them from whichever file included it. The include
 * guard makes the repeated inclusion a no-op.
 */

#ifndef SOCKETSECURITY_BIN_INFRA_SMOL_SEGMENT_READER_INTERNAL_H
#define SOCKETSECURITY_BIN_INFRA_SMOL_SEGMENT_READER_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include "socketsecurity/bin-infra/smol_segment_reader.h"
#include "socketsecurity/bin-infra/marker_finder.h"
#include "socketsecurity/bin-infra/compression_constants.h"
#include "socketsecurity/bin-infra/segment_names.h"
#include "socketsecurity/build-infra/binary_format_finder.h"
#include "socketsecurity/build-infra/path_utils.h"
#include "socketsecurity/build-infra/file_io_common.h"

/**
 * Windows compatibility.
 * posix_compat.h provides POSIX function mappings (read, lseek, etc.)
 * and types (ssize_t, off_t) for Windows.
 */
#ifdef _WIN32
    #include "socketsecurity/build-infra/posix_compat.h"
#else
    #include <unistd.h>
    #include <fcntl.h>  /* For O_RDONLY */
#endif

#endif /* SOCKETSECURITY_BIN_INFRA_SMOL_SEGMENT_READER_INTERNAL_H */

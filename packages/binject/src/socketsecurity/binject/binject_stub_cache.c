// ============================================================================
// binject_stub_cache.c — Compressed self-extracting stub handling
// ============================================================================
//
// WHAT THIS FILE DOES
// Recognizes a compressed self-extracting stub, decompresses one into the
// shared DLX cache directory, and reports the cache path of the extracted
// binary so the injection phase has a plain executable to work with.
//
// WHY IT EXISTS
// A node-smol stub carries the real node binary as a ZSTD blob, so a resource
// cannot be injected into it directly — it has to be unpacked first. This is
// the middle of binject's three phases (core primitives in binject.c → stub
// cache here → command orchestration in binject_commands.c); it was split out
// of binject.c when that file outgrew the 1000-line source cap.
// ============================================================================

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  // For O_CLOEXEC, lstat, fdopen
#define _XOPEN_SOURCE 700        // For additional POSIX features
#ifdef __APPLE__
#define _DARWIN_C_SOURCE         // For O_NOFOLLOW on macOS
#endif
#endif  // !_WIN32

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <process.h>
// Windows doesn't have O_NOFOLLOW, define to 0 (no-op, symlink behavior different)
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#endif
#include "socketsecurity/binject/binject.h"
#include "socketsecurity/binject/binject_internal.h"

/* Shared compression library from bin-infra */
#include "socketsecurity/bin-infra/binary_format.h"
#include "socketsecurity/bin-infra/buffer_constants.h"
#include "socketsecurity/bin-infra/compression_common.h"
#include "socketsecurity/bin-infra/compression_constants.h"
#include "socketsecurity/bin-infra/decompressor_limits.h"
#include "socketsecurity/bin-infra/smol_detect.h"
#include "socketsecurity/bin-infra/smol_segment_reader.h"

/* Shared file utilities from build-infra */
#include "socketsecurity/build-infra/dlx_cache_common.h"
#include "socketsecurity/build-infra/file_io_common.h"
#include "socketsecurity/build-infra/file_utils.h"
#include "socketsecurity/build-infra/posix_compat.h"

/* Compressed binary cache support */

/* Constants for compressed stub detection */
/* Format-specific search sizes optimized for each binary format's section layout */
/* Marker appears at end of stub binary; search sizes must exceed stub size */
#define SEARCH_SIZE_MACHO  (1536 * 1024)  /* macOS Mach-O: stub ~1MB, marker at ~1MB */
#define SEARCH_SIZE_PE     (128 * 1024)   /* Windows PE: marker at ~59KB */
#define SEARCH_SIZE_ELF    (1408 * 1024)  /* Linux ELF: marker at ~1052KB */
#define SEARCH_SIZE_MAX    SEARCH_SIZE_MACHO /* Maximum for unknown formats */

/**
 * Get the appropriate search size for a binary format.
 * Uses runtime format detection to support cross-platform operations
 * (e.g., inspecting Linux binaries on macOS).
 */
static size_t get_search_size_for_format(binject_format_t format) {
    switch (format) {
        case BINJECT_FORMAT_MACHO:
            return SEARCH_SIZE_MACHO;
        case BINJECT_FORMAT_PE:
            return SEARCH_SIZE_PE;
        case BINJECT_FORMAT_ELF:
            return SEARCH_SIZE_ELF;
        default:
            return SEARCH_SIZE_MAX;  /* Unknown format: search entire range */
    }
}

/* Check if executable is a compressed self-extracting stub using LIEF.
 * Uses LIEF to read section content directly, eliminating arbitrary size limits. */
int binject_is_compressed_stub_lief(const char *executable) {
    binject_format_t format = binject_detect_format(executable);

    /* Use LIEF-based section reading for each format.
     * Pass marker parts separately to avoid marker appearing in binary. */
    switch (format) {
        case BINJECT_FORMAT_MACHO:
            return smol_has_compressed_data_macho_lief(executable,
                MAGIC_MARKER_PART1, MAGIC_MARKER_PART2, MAGIC_MARKER_PART3) == 1;
        case BINJECT_FORMAT_ELF:
            return smol_has_compressed_data_elf_lief(executable,
                MAGIC_MARKER_PART1, MAGIC_MARKER_PART2, MAGIC_MARKER_PART3) == 1;
        case BINJECT_FORMAT_PE:
            return smol_has_compressed_data_pe_lief(executable,
                MAGIC_MARKER_PART1, MAGIC_MARKER_PART2, MAGIC_MARKER_PART3) == 1;
        default:
            return 0;
    }
}

/* Check if executable is a compressed self-extracting stub (buffer-based fallback).
 * Kept for compatibility but binject_is_compressed_stub_lief is preferred. */
int binject_is_compressed_stub(const char *executable) {
    /* Use LIEF-based detection which reads section content directly */
    return binject_is_compressed_stub_lief(executable);
}

/**
 * Extract compressed stub to cache directory (cross-platform).
 * This function manually decompresses the stub without needing to execute it,
 * enabling cross-platform builds (e.g., extracting Linux stubs on macOS).
 *
 * @param compressed_stub Path to compressed stub binary
 * @param extracted_path Path where extracted binary should be written
 * @return BINJECT_OK on success, error code on failure
 */
int binject_extract_stub_to_cache(const char *compressed_stub, const char *extracted_path) {
    FILE *fp = NULL;
    uint8_t *buffer = NULL;
    uint8_t *compressed_data = NULL;
    uint8_t *decompressed_data = NULL;
    int result = BINJECT_ERROR;

    printf("Extracting compressed stub manually...\n");

    /* Open compressed stub. */
    fp = fopen(compressed_stub, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open compressed stub: %s\n", compressed_stub);
        goto cleanup;
    }

    /* Get search size based on binary format (runtime detection for cross-platform support). */
    binject_format_t format = binject_detect_format(compressed_stub);
    size_t search_size = get_search_size_for_format(format);
    buffer = malloc(search_size);
    if (!buffer) {
        fprintf(stderr, "Error: Out of memory\n");
        goto cleanup;
    }

    size_t read_size = fread(buffer, 1, search_size, fp);

    /* Find magic marker. */
    size_t marker_offset;
    if (smol_find_marker_in_buffer(buffer, read_size, &marker_offset) != 0) {
        fprintf(stderr, "Error: Magic marker not found\n");
        goto cleanup;
    }

    /* Read metadata: [marker][8-byte: compressed_size][8-byte: uncompressed_size][16-byte: cache_key][3-byte: platform_metadata]. */
    size_t metadata_offset = marker_offset + MAGIC_MARKER_LEN;
    if (metadata_offset + METADATA_HEADER_LEN > read_size) {
        fprintf(stderr, "Error: Metadata truncated\n");
        goto cleanup;
    }

    uint64_t compressed_size;
    uint64_t uncompressed_size;
    memcpy(&compressed_size, buffer + metadata_offset, sizeof(uint64_t));
    memcpy(&uncompressed_size, buffer + metadata_offset + sizeof(uint64_t), sizeof(uint64_t));

    /* Fast-fail: Validate sizes BEFORE allocation and decompression. */
    if (compressed_size == 0 || uncompressed_size == 0) {
        fprintf(stderr, "Error: Invalid metadata (zero size)\n");
        goto cleanup;
    }
    if (uncompressed_size > DECOMPRESSOR_MAX_UNCOMPRESSED_SIZE) {
        fprintf(stderr, "Error: Uncompressed size %llu exceeds limit %d MB\n",
                (unsigned long long)uncompressed_size,
                DECOMPRESSOR_MAX_UNCOMPRESSED_SIZE / (1024 * 1024));
        goto cleanup;
    }
    if (compressed_size > uncompressed_size) {
        fprintf(stderr, "Error: Compressed size %llu > uncompressed size %llu (invalid)\n",
                (unsigned long long)compressed_size, (unsigned long long)uncompressed_size);
        goto cleanup;
    }

    /* All platforms use ZSTD compression exclusively. */
    (void)0;

    /* Check has_smol_config flag to determine if SMFG config is present.
     * Layout: [marker][size_header][cache_key][platform_metadata][has_smol_config][smol_config?][data]
     * has_smol_config is at offset: marker + 32 + 16 + 16 + 3 = marker + 67 */
    size_t has_smol_config_offset = marker_offset + MAGIC_MARKER_LEN + SIZE_HEADER_LEN + CACHE_KEY_LEN + PLATFORM_METADATA_LEN;
    if (has_smol_config_offset >= read_size) {
        fprintf(stderr, "Error: has_smol_config flag truncated\n");
        goto cleanup;
    }
    uint8_t has_smol_config = buffer[has_smol_config_offset];

    /* Compressed data starts after: marker + metadata header + optional smol config. */
    size_t data_offset = marker_offset + MAGIC_MARKER_LEN + METADATA_HEADER_LEN;
    if (has_smol_config != 0) {
        data_offset += SMOL_CONFIG_BINARY_LEN;
    }

    printf("  Compressed size: %llu bytes\n", (unsigned long long)compressed_size);
    printf("  Uncompressed size: %llu bytes\n", (unsigned long long)uncompressed_size);
    printf("  Has SMFG config: %s\n", has_smol_config ? "yes" : "no");
    printf("  Data offset: %zu bytes\n", data_offset);

    /* Allocate buffer for compressed data. */
    compressed_data = malloc(compressed_size);
    if (!compressed_data) {
        fprintf(stderr, "Error: Out of memory for compressed data\n");
        goto cleanup;
    }

    /* Optimization: If compressed data fits in already-read buffer, copy directly.
     * This avoids an extra fseek + fread syscall for small/medium compressed stubs. */
    if (data_offset + compressed_size <= read_size) {
        /* Data already in buffer - copy directly (avoids file I/O) */
        memcpy(compressed_data, buffer + data_offset, compressed_size);
    } else {
        /* Data extends beyond buffer - need to read from file */
#ifdef _WIN32
        if (_fseeki64(fp, (int64_t)data_offset, SEEK_SET) != 0) {
#else
        if (fseeko(fp, (off_t)data_offset, SEEK_SET) != 0) {
#endif
            fprintf(stderr, "Error: Failed to seek to compressed data\n");
            goto cleanup;
        }
        if (fread(compressed_data, 1, compressed_size, fp) != compressed_size) {
            fprintf(stderr, "Error: Failed to read compressed data\n");
            goto cleanup;
        }
    }

    fclose(fp);
    fp = NULL;

    /* Allocate decompression buffer. */
    decompressed_data = malloc(uncompressed_size);
    if (!decompressed_data) {
        fprintf(stderr, "Error: Out of memory for decompressed data\n");
        goto cleanup;
    }

    /* Decompress using ZSTD. */
    printf("  Decompressing... (algorithm: ZSTD)\n");
    int decompress_result = decompress_buffer_sized(
        compressed_data, compressed_size,
        decompressed_data, uncompressed_size
    );

    if (decompress_result != COMPRESS_OK) {
        fprintf(stderr, "Error: Decompression failed (code: %d = %s)\n",
                decompress_result, compress_error_string(decompress_result));
        fprintf(stderr, "  Compressed size: %" PRIu64 " bytes, expected uncompressed: %" PRIu64 " bytes\n",
                compressed_size, uncompressed_size);

        /* Provide context-aware diagnostics for common failure scenarios */
        if (decompress_result == COMPRESS_ERROR_DECOMPRESS_FAILED && compressed_size >= 4) {
            uint32_t first_word;
            memcpy(&first_word, compressed_data, sizeof(first_word));
            if (first_word == 0x534D4647) {  /* "SMFG" magic */
                fprintf(stderr, "  Cause: Data offset points to SMFG config instead of compressed data.\n");
                fprintf(stderr, "  The decompressor tried to decompress config data instead of actual compressed data.\n");
            } else {
                fprintf(stderr, "  Cause: Data at offset does not appear to be ZSTD-compressed.\n");
                fprintf(stderr, "  First 4 bytes: 0x%08X\n", first_word);
            }
        }

        goto cleanup;
    }

    /* Create parent directories if needed. */
    if (create_parent_directories(extracted_path) != 0) {
        fprintf(stderr, "Error: Failed to create parent directories for output path: %s\n", extracted_path);
        goto cleanup;
    }

    /* Write decompressed binary using cross-platform helper. */
    if (write_file_atomically(extracted_path, decompressed_data, uncompressed_size, 0755) == -1) {
        fprintf(stderr, "Error: Failed to write decompressed data\n");
        goto cleanup;
    }

    /* Make executable on Unix. */
    if (set_executable_permissions(extracted_path) != 0) {
        fprintf(stderr, "Error: Failed to set executable permissions\n");
        goto cleanup;
    }

    printf("✓ Extraction complete: %s\n", extracted_path);
    result = BINJECT_OK;

cleanup:
    if (fp) fclose(fp);
    if (buffer) free(buffer);
    if (compressed_data) free(compressed_data);
    if (decompressed_data) free(decompressed_data);
    return result;
}

/* Get path to extracted binary from compressed stub */
int binject_get_extracted_path(const char *compressed_stub, char *extracted_path, size_t path_size) {
    /* Validate compressed_stub path to prevent command injection */
    if (!compressed_stub || strlen(compressed_stub) == 0) {
        fprintf(stderr, "Error: Compressed stub path is empty\n");
        return BINJECT_ERROR;
    }

    /* Check for path traversal attempts */
    if (strstr(compressed_stub, "..") != NULL) {
        fprintf(stderr, "Error: Path traversal detected in stub path\n");
        return BINJECT_ERROR;
    }

    /* Validate file exists */
    BINJECT_STAT_STRUCT stub_st;
    if (binject_stat(compressed_stub, &stub_st) != 0) {
        fprintf(stderr, "Error: Compressed stub not found: %s\n", compressed_stub);
        return BINJECT_ERROR_FILE_NOT_FOUND;
    }

    /* Validate it's a regular file */
    if (!S_ISREG(stub_st.st_mode)) {
        fprintf(stderr, "Error: Compressed stub is not a regular file\n");
        return BINJECT_ERROR;
    }

    /* Read SMOL metadata to get cache key using LIEF-based reader */
    smol_metadata_t metadata;
    int fd = open(compressed_stub, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to open stub: %s\n", strerror(errno));
        return BINJECT_ERROR_FILE_NOT_FOUND;
    }

    if (smol_read_metadata(fd, &metadata) != 0) {
        fprintf(stderr, "Error: Failed to read SMOL metadata\n");
        close(fd);
        return BINJECT_ERROR;
    }
    close(fd);

    /* Use cache key from metadata */
    char cache_key[CACHE_KEY_LEN + 1];
    memcpy(cache_key, metadata.cache_key, CACHE_KEY_LEN);
    cache_key[CACHE_KEY_LEN] = '\0';

    /* Validate cache key is exactly 16 hex bytes with proper null termination */
    for (int i = 0; i < CACHE_KEY_LEN; i++) {
        char c = cache_key[i];
        if (c == '\0') {
            fprintf(stderr, "Error: Cache key contains null byte at position %d\n", i);
            return BINJECT_ERROR;
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            fprintf(stderr, "Error: Invalid cache key format (must be hex): %s\n", cache_key);
            return BINJECT_ERROR;
        }
    }

    /* Verify null termination at position 16 */
    if (cache_key[CACHE_KEY_LEN] != '\0') {
        fprintf(stderr, "Error: Cache key not properly null-terminated\n");
        return BINJECT_ERROR;
    }

    /* Build extracted binary path: <base_dir>/<cache_key>/node-smol-<platform>-<arch> */
    /* Respects SOCKET_DLX_DIR and SOCKET_HOME environment variables */
    char base_dir[CACHE_DIR_BUFFER_SIZE];
    if (dlx_get_cache_base_dir(base_dir, sizeof(base_dir)) != 0) {
        fprintf(stderr, "Error: Failed to get cache base directory\n");
        return BINJECT_ERROR;
    }

    /* Validate cache directory is accessible.
     * POSIX_LSTAT uses lstat() on Unix (detects symlinks) and stat() on Windows
     * (symlinks require elevated privileges, so stat() is sufficient). */
    BINJECT_STAT_STRUCT base_st;
    if (POSIX_LSTAT(base_dir, &base_st) != 0) {
        fprintf(stderr, "Error: Cache directory not accessible: %s\n", base_dir);
        return BINJECT_ERROR;
    }

    /* Reject symlinks to prevent TOCTOU attacks.
     * POSIX_S_ISLNK returns false on Windows (no-op since symlinks need admin). */
    if (POSIX_S_ISLNK(base_st.st_mode)) {
        fprintf(stderr, "Error: Cache directory cannot be a symbolic link: %s\n", base_dir);
        return BINJECT_ERROR;
    }

    if (!S_ISDIR(base_st.st_mode)) {
        fprintf(stderr, "Error: Cache path is not a directory: %s\n", base_dir);
        return BINJECT_ERROR;
    }

    /* Build extracted binary path using shared helper. */
    /* Path format: <base_dir>/<cache_key>/node (or node.exe on Windows). */
    if (dlx_get_extracted_binary_path(cache_key, extracted_path, path_size) != 0) {
        fprintf(stderr, "Error: Failed to construct extracted binary path\n");
        return BINJECT_ERROR;
    }

    /* Check if extracted binary exists */
    BINJECT_STAT_STRUCT st;
    if (binject_stat(extracted_path, &st) != 0) {
        fprintf(stderr, "Extracted binary not found in cache\n");

        /* Use LIEF-based extraction which reads section data directly.
         * This enables cross-platform builds (e.g., extracting Linux stubs on macOS). */
        int extract_result = smol_extract_binary_lief(compressed_stub, extracted_path);
        if (extract_result != 0) {
            fprintf(stderr, "Error: Failed to extract compressed stub\n");
            return BINJECT_ERROR;
        }

        /* Verify extraction succeeded by opening and checking file format */
        /* Use O_NOFOLLOW to prevent TOCTOU via symlink attacks */
        FILE *verify_fp = NULL;
#ifndef _WIN32
        int verify_fd = open(extracted_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (verify_fd < 0) {
            fprintf(stderr, "Error: Cannot open extracted binary (may be symlink): %s\n", extracted_path);
            return BINJECT_ERROR_FILE_NOT_FOUND;
        }
        verify_fp = fdopen(verify_fd, "rb");
        if (!verify_fp) {
            close(verify_fd);
            fprintf(stderr, "Error: Cannot fdopen extracted binary: %s\n", extracted_path);
            return BINJECT_ERROR_FILE_NOT_FOUND;
        }
#else
        /* Windows: Check for symlinks/reparse points before opening */
        DWORD attrs = GetFileAttributesA(extracted_path);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "Error: Cannot get file attributes: %s\n", extracted_path);
            return BINJECT_ERROR_FILE_NOT_FOUND;
        }

        /* Reject reparse points (symlinks, mount points, etc.) */
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            fprintf(stderr, "Error: File is a reparse point (symlink): %s\n", extracted_path);
            return BINJECT_ERROR_FILE_NOT_FOUND;
        }

        verify_fp = fopen(extracted_path, "rb");
        if (!verify_fp) {
            fprintf(stderr, "Error: Extracted binary not found at: %s\n", extracted_path);
            return BINJECT_ERROR_FILE_NOT_FOUND;
        }
#endif

        /* Verify it's a valid binary format by reading magic bytes.
         * Use the already-read bytes directly with detect_binary_format() instead
         * of calling binject_detect_format() which would:
         * 1. Reopen the file (wasting a syscall)
         * 2. Open WITHOUT O_NOFOLLOW (creating a TOCTOU vulnerability)
         * 3. Re-read the same 4 bytes we already have */
        uint8_t magic[4];
        size_t bytes_read = fread(magic, 1, 4, verify_fp);
        fclose(verify_fp);

        if (bytes_read != 4) {
            fprintf(stderr, "Error: Extracted binary is invalid (too small)\n");
            return BINJECT_ERROR_INVALID_FORMAT;
        }

        /* Use already-read magic bytes for format validation (no second file open) */
        binary_format_t extracted_format = detect_binary_format(magic);
        if (extracted_format == BINARY_FORMAT_UNKNOWN) {
            fprintf(stderr, "Error: Extracted binary has invalid format\n");
            return BINJECT_ERROR_INVALID_FORMAT;
        }

        fprintf(stderr, "✓ Extraction complete: %s\n", extracted_path);
    }

    return BINJECT_OK;
}

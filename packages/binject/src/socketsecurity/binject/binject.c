// ============================================================================
// binject.c — Core primitives shared by every binject operation
// ============================================================================
//
// WHAT THIS FILE DOES
// Implements binject's format-agnostic primitives: detecting the executable
// format (ELF, Mach-O, PE) with a small mtime-keyed cache, slurping a resource
// file into memory, the CRC32 checksum, and the compress/decompress wrappers.
//
// WHY IT EXISTS
// This is the bottom layer of the "engine" behind binject — the pieces that
// have no opinion about which command is running. The two layers above it live
// in sibling files: binject_stub_cache.c (compressed self-extracting stubs)
// and binject_commands.c (the inject/list/extract/verify orchestration). They
// were split out of this file when it outgrew the 1000-line source cap; the
// public API in binject.h is unchanged.
// ============================================================================

/**
 * binject - Core implementation
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  // For O_CLOEXEC, lstat, fdopen
#define _XOPEN_SOURCE 700        // For additional POSIX features
#ifdef __APPLE__
#define _DARWIN_C_SOURCE         // For O_NOFOLLOW on macOS
#endif
#endif  // !_WIN32

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
/* Windows compat shims (PATH_MAX, S_ISREG/S_ISDIR, 64-bit fseek/ftell). */
#include "socketsecurity/build-infra/file_io_common.h"
#include "socketsecurity/binject/binject.h"
#include "socketsecurity/binject/binject_internal.h"

/* Shared compression library from bin-infra */
#include "socketsecurity/bin-infra/binary_format.h"
#include "socketsecurity/bin-infra/compression_common.h"

/* Shared file utilities from build-infra */
#include "socketsecurity/build-infra/path_utils.h"

/* Format detection cache to avoid redundant file reads.
 * This significantly improves performance when processing the same file
 * multiple times (e.g., during batch injection which calls detect_format 3+ times).
 * Size of 32 handles typical batch operations without excessive memory (~128KB). */
#define FORMAT_CACHE_SIZE 32
static struct {
    char path[PATH_MAX];
    binject_format_t format;
    time_t mtime;  /* File modification time for cache invalidation */
} format_cache[FORMAT_CACHE_SIZE];
static int format_cache_index = 0;

/* Cross-platform stat wrapper — avoids #define stat _stat which breaks
 * struct stat declarations on Windows. Declared in binject_internal.h because
 * binject_stub_cache.c and binject_commands.c need the same wrapper. */
int binject_stat(const char* path, struct stat* st) {
    return stat(path, st);
}

/* Detect binary format by magic bytes (with caching) */
binject_format_t binject_detect_format(const char *executable) {
    /* Resolve relative paths to absolute paths to avoid fopen() issues with relative paths. */
    char resolved_path[PATH_MAX];
    const char *path_to_open = resolve_absolute_path(executable, resolved_path);

    /* Get file stat once for both cache check and cache update */
    BINJECT_STAT_STRUCT st;
    int stat_valid = (binject_stat(path_to_open, &st) == 0);

    /* Check cache first */
    if (stat_valid) {
        for (int i = 0; i < FORMAT_CACHE_SIZE; i++) {
            if (format_cache[i].path[0] != '\0' &&
                strcmp(format_cache[i].path, path_to_open) == 0 &&
                format_cache[i].mtime == st.st_mtime) {
                return format_cache[i].format;
            }
        }
    }

    FILE *fp = fopen(path_to_open, "rb");
    if (!fp) {
        return BINJECT_FORMAT_UNKNOWN;
    }

    uint8_t magic[4];
    if (fread(magic, 1, 4, fp) != 4) {
        fclose(fp);
        return BINJECT_FORMAT_UNKNOWN;
    }
    fclose(fp);

    /* Use shared binary format detection. */
    binary_format_t format = detect_binary_format(magic);

    /* Convert from shared format enum to binject format enum. */
    binject_format_t result;
    switch (format) {
        case BINARY_FORMAT_MACHO:
            result = BINJECT_FORMAT_MACHO;
            break;
        case BINARY_FORMAT_ELF:
            result = BINJECT_FORMAT_ELF;
            break;
        case BINARY_FORMAT_PE:
            result = BINJECT_FORMAT_PE;
            break;
        default:
            result = BINJECT_FORMAT_UNKNOWN;
            break;
    }

    /* Cache the result (reuse stat from earlier) */
    if (stat_valid) {
        strncpy(format_cache[format_cache_index].path, path_to_open, PATH_MAX - 1);
        format_cache[format_cache_index].path[PATH_MAX - 1] = '\0';
        format_cache[format_cache_index].format = result;
        format_cache[format_cache_index].mtime = st.st_mtime;
        format_cache_index = (format_cache_index + 1) % FORMAT_CACHE_SIZE;
    }

    return result;
}

/* Maximum resource file size: 500MB to accommodate universal binaries (arm64+x86_64) */
#define MAX_RESOURCE_SIZE (500 * 1024 * 1024)

/* Read resource file into memory */
int binject_read_resource(const char *resource_file, uint8_t **data, size_t *size) {
    FILE *fp = fopen(resource_file, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open resource file: %s\n", resource_file);
        return BINJECT_ERROR_FILE_NOT_FOUND;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        fprintf(stderr, "Error: Cannot seek resource file\n");
        return BINJECT_ERROR;
    }

    /* Use platform-appropriate 64-bit file position function */
#ifdef _WIN32
    __int64 file_size = _ftelli64(fp);
#else
    off_t file_size = ftello(fp);
#endif
    if (file_size < 0) {
        fclose(fp);
        fprintf(stderr, "Error: Cannot determine resource file size\n");
        return BINJECT_ERROR;
    }

    /* Validate file_size fits in size_t before casting (prevents truncation on 32-bit) */
    if ((uint64_t)file_size > (uint64_t)SIZE_MAX) {
        fclose(fp);
        fprintf(stderr, "Error: Resource file size exceeds addressable memory\n");
        return BINJECT_ERROR;
    }

    if ((size_t)file_size > MAX_RESOURCE_SIZE) {
        fclose(fp);
        fprintf(stderr, "Error: Resource file too large (max %d MB)\n", MAX_RESOURCE_SIZE / (1024 * 1024));
        return BINJECT_ERROR;
    }

    /* Reject empty resources: a 0-byte SEA blob or VFS archive is never valid
     * payload. (The 0-byte VFS compatibility section is created via an empty
     * --vfs PATH argument and never reaches this reader.) */
    if (file_size == 0) {
        fclose(fp);
        fprintf(stderr, "Error: Resource file is empty: %s\n", resource_file);
        return BINJECT_ERROR;
    }

    *size = (size_t)file_size;

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        fprintf(stderr, "Error: Cannot seek resource file\n");
        return BINJECT_ERROR;
    }

    /* Handle empty files: malloc(0) behavior is implementation-defined.
     * Always allocate at least 1 byte to ensure a valid non-NULL pointer. */
    *data = malloc(*size > 0 ? *size : 1);
    if (!*data) {
        fclose(fp);
        fprintf(stderr, "Error: Out of memory\n");
        return BINJECT_ERROR;
    }

    /* Only read if there's data to read (empty files have size 0) */
    if (*size > 0 && fread(*data, 1, *size, fp) != *size) {
        free(*data);
        fclose(fp);
        fprintf(stderr, "Error: Failed to read resource file\n");
        return BINJECT_ERROR;
    }

    fclose(fp);
    return BINJECT_OK;
}

/* CRC32 checksum with proper polynomial */
uint32_t binject_checksum(const uint8_t *data, size_t size) {
    /* CRC32 polynomial (IEEE 802.3) */
    static const uint32_t crc32_table[256] = {
        0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
        0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
        0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
        0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
        0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
        0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
        0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
        0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
        0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
        0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
        0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
        0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
        0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
        0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
        0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
        0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
        0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
        0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
        0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
        0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
        0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
        0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
        0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
        0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
        0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
        0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
        0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
        0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
        0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
        0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
        0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
        0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
    };

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        uint8_t byte = data[i];
        crc = (crc >> 8) ^ crc32_table[(crc ^ byte) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

/* Compress data using shared compression library */
int binject_compress(const uint8_t *input, size_t input_size,
                    uint8_t **output, size_t *output_size) {
    int result = compress_buffer(input, input_size, output, output_size);

    /* Map compression_common error codes to binject error codes */
    if (result == COMPRESS_OK) {
        return BINJECT_OK;
    } else {
        return BINJECT_ERROR_COMPRESSION_FAILED;
    }
}

/* Decompress data using shared compression library */
int binject_decompress(const uint8_t *input, size_t input_size,
                      uint8_t **output, size_t *output_size) {
    int result = decompress_buffer(input, input_size, output, output_size);

    /* Map compression_common error codes to binject error codes */
    if (result == COMPRESS_OK) {
        return BINJECT_OK;
    } else {
        return BINJECT_ERROR_DECOMPRESSION_FAILED;
    }
}

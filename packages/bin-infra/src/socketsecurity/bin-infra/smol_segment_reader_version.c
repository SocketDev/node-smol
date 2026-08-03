/**
 * @file smol_segment_reader_version.c
 * @brief Node.js version extraction from SMOL binaries.
 *
 * Part of the smol_segment_reader.c translation unit (see that file's header
 * for the split layout). This part owns the version-extraction phase and the
 * fallback ladder behind smol_extract_node_version_fast(): SMFG config in
 * PRESSED_DATA, then the raw __smol_node_ver/SMOL_NODE_VER section, then the
 * PE VS_VERSION_INFO resource. The per-format section finders it calls live in
 * the _macho.c / _elf.c / _pe.c parts, which the umbrella includes first.
 */

#include "socketsecurity/bin-infra/smol_segment_reader_internal.h"

/**
 * SMFG config offsets and constants.
 * nodeVersion is at offset 1176 in the 1192-byte SMFG v2 binary.
 * Format: length byte (1) + version string (up to 15 chars).
 */
#define SMFG_MAGIC 0x534D4647  /* "SMFG" */
#define SMFG_VERSION_OFFSET 4
#define SMFG_NODE_VERSION_OFFSET 1176
#define SMFG_NODE_VERSION_MAX_LEN 15

/*
 * ============================================================================
 * Native __smol_node_ver Section Readers
 * ============================================================================
 *
 * These functions read the Node.js version string directly from the
 * __smol_node_ver section without using LIEF. This is much faster and
 * works on raw Node.js binaries before compression.
 *
 * The __smol_node_ver section is created by smol_node_version.c during
 * Node.js compilation and contains just the version string (e.g., "1.2.3").
 *
 * Section locations:
 * - Mach-O: __DATA segment, __smol_node_ver section
 * - ELF: SMOL_NODE_VER section
 * - PE: SMOL_NODE_VER section
 */

/**
 * Extract version string from section content.
 * The section contains a null-terminated version string (e.g., "1.2.3").
 *
 * @param data Section content
 * @param size Section size
 * @return Version string (caller must free), or NULL if invalid
 */
static char* extract_version_from_section_content(const uint8_t *data, size_t size) {
    if (!data || size == 0 || size > 32) {
        return NULL;
    }

    /* Find null terminator or use full length. */
    size_t len = 0;
    while (len < size && data[len] != 0) {
        len++;
    }

    /* Validate version string length (e.g., "1.2.3" = 5 chars). */
    if (len == 0 || len > 15) {
        return NULL;
    }

    char *result = (char *)malloc(len + 1);
    if (!result) {
        return NULL;
    }

    memcpy(result, data, len);
    result[len] = '\0';

    return result;
}

/**
 * Extract Node.js version from raw binary's __smol_node_ver section.
 * This works on uncompressed Node.js binaries built with smol_node_version.c.
 *
 * @param fd Open file descriptor
 * @return Version string (caller must free), or NULL if not found
 */
static char* extract_node_ver_from_section(int fd) {
    int64_t section_offset = 0;
    size_t section_size = 0;
    int found = -1;

#if defined(__APPLE__)
    found = smol_find_node_ver_section_macho(fd, &section_offset, &section_size);
#elif defined(__linux__) || (!defined(__APPLE__) && !defined(_WIN32))
    found = smol_find_node_ver_section_elf(fd, &section_offset, &section_size);
#elif defined(_WIN32)
    found = smol_find_node_ver_section_pe(fd, &section_offset, &section_size);
#endif

    if (found != 0 || section_offset <= 0 || section_size == 0 || section_size > 32) {
        return NULL;
    }

    /* Read section content. */
    if (lseek(fd, section_offset, SEEK_SET) == -1) {
        return NULL;
    }

    uint8_t *content = (uint8_t *)malloc(section_size);
    if (!content) {
        return NULL;
    }

    if (read_eintr(fd, content, section_size) != (ssize_t)section_size) {
        free(content);
        return NULL;
    }

    char *version = extract_version_from_section_content(content, section_size);
    free(content);
    return version;
}

/**
 * Extract Node.js version from SMFG config binary.
 *
 * @param smfg_config 1192-byte SMFG config binary
 * @return Version string (caller must free), or NULL if not found/invalid
 */
static char* extract_version_from_smfg(const uint8_t *smfg_config) {
    if (!smfg_config) {
        return NULL;
    }

    /* Verify SMFG magic. */
    uint32_t magic;
    memcpy(&magic, smfg_config, sizeof(magic));
    if (magic != SMFG_MAGIC) {
        return NULL;
    }

    /* Verify SMFG version >= 2 (v2 added nodeVersion field). */
    uint16_t version;
    memcpy(&version, smfg_config + SMFG_VERSION_OFFSET, sizeof(version));
    if (version < 2) {
        return NULL;
    }

    /* Read nodeVersion: length byte + string. */
    uint8_t len = smfg_config[SMFG_NODE_VERSION_OFFSET];
    if (len == 0 || len > SMFG_NODE_VERSION_MAX_LEN) {
        return NULL;
    }

    char *result = (char *)malloc(len + 1);
    if (!result) {
        return NULL;
    }

    memcpy(result, smfg_config + SMFG_NODE_VERSION_OFFSET + 1, len);
    result[len] = '\0';

    return result;
}

/**
 * Read SMOL metadata and SMFG config from file descriptor.
 *
 * Similar to smol_read_metadata_after_marker but also reads the SMFG config
 * binary (if present) instead of skipping it.
 *
 * @param fd File descriptor positioned immediately after magic marker
 * @param metadata Output: metadata structure (caller-allocated)
 * @param smfg_config_out Output: SMFG config buffer (caller must free), or NULL if not present
 * @return 0 on success, -1 on error
 */
static int smol_read_metadata_with_config(int fd, smol_metadata_t *metadata, uint8_t **smfg_config_out) {
    if (fd < 0 || !metadata || !smfg_config_out) {
        return -1;
    }

    *smfg_config_out = NULL;

    /* Initialize metadata structure. */
    memset(metadata, 0, sizeof(smol_metadata_t));

    /* Read compressed size (8 bytes). */
    if (read_eintr(fd, &metadata->compressed_size, sizeof(metadata->compressed_size))
        != sizeof(metadata->compressed_size)) {
        return -1;
    }

    /* Read uncompressed size (8 bytes). */
    if (read_eintr(fd, &metadata->uncompressed_size, sizeof(metadata->uncompressed_size))
        != sizeof(metadata->uncompressed_size)) {
        return -1;
    }

    /* Read cache key (16 bytes). */
    char cache_key_raw[CACHE_KEY_LEN];
    if (read_eintr(fd, cache_key_raw, CACHE_KEY_LEN) != CACHE_KEY_LEN) {
        return -1;
    }
    memcpy(metadata->cache_key, cache_key_raw, CACHE_KEY_LEN);
    metadata->cache_key[CACHE_KEY_LEN] = '\0';

    /* Read platform metadata (3 bytes). */
    if (read_eintr(fd, metadata->platform_metadata, PLATFORM_METADATA_LEN) != PLATFORM_METADATA_LEN) {
        return -1;
    }

    /* Read integrity hash (32 bytes: SHA-256 of compressed data). */
    if (read_eintr(fd, metadata->integrity_hash, INTEGRITY_HASH_LEN) != INTEGRITY_HASH_LEN) {
        return -1;
    }

    /* Read has_smol_config flag (1 byte). */
    uint8_t has_smol_config;
    if (read_eintr(fd, &has_smol_config, SMOL_CONFIG_FLAG_LEN) != SMOL_CONFIG_FLAG_LEN) {
        return -1;
    }

    /* Read SMFG config binary if present. */
    if (has_smol_config != 0) {
        uint8_t *config = (uint8_t *)malloc(SMOL_CONFIG_BINARY_LEN);
        if (!config) {
            return -1;
        }
        if (read_eintr(fd, config, SMOL_CONFIG_BINARY_LEN) != SMOL_CONFIG_BINARY_LEN) {
            free(config);
            return -1;
        }
        *smfg_config_out = config;
    }

    /* Record offset to compressed data. */
    metadata->data_offset = lseek(fd, 0, SEEK_CUR);
    if (metadata->data_offset == -1) {
        if (*smfg_config_out) {
            free(*smfg_config_out);
            *smfg_config_out = NULL;
        }
        return -1;
    }

    return 0;
}

/**
 * Extract Node.js version from binary using fast native parsing.
 *
 * This is much faster than LIEF-based parsing because it uses direct file I/O
 * and platform-specific header parsing instead of full binary analysis.
 *
 * Works for:
 * - SMOL stubs (compressed binaries with PRESSED_DATA section)
 * - node-smol binaries (with SMFG config in PRESSED_DATA)
 * - Plain Node.js PE binaries (reads VS_VERSION_INFO resource)
 *
 * @param binary_path Path to binary file
 * @return Version string (e.g., "25.5.0"), or NULL if not found.
 *         Caller must free() the returned string.
 */
char* smol_extract_node_version_fast(const char *binary_path) {
    if (!binary_path) {
        return NULL;
    }

#ifdef _WIN32
    int fd = _open(binary_path, _O_RDONLY | _O_BINARY);
#else
    int fd = open(binary_path, O_RDONLY);
#endif
    if (fd < 0) {
        return NULL;
    }

    smol_metadata_t metadata;
    uint8_t *smfg_config = NULL;
    char *version = NULL;

    /* Try platform-optimized marker finding first. */
#if defined(__APPLE__)
    int64_t section_offset;
    if (smol_find_pressed_data_offset_macho(fd, &section_offset) == 0 && section_offset > 0) {
        /* Seek past the magic marker. */
        if (lseek(fd, section_offset + MAGIC_MARKER_LEN, SEEK_SET) != -1) {
            if (smol_read_metadata_with_config(fd, &metadata, &smfg_config) == 0 && smfg_config) {
                version = extract_version_from_smfg(smfg_config);
                free(smfg_config);
            }
        }
    }
#elif defined(__linux__)
    int64_t section_offset;
    if (smol_find_pressed_data_offset_elf(fd, &section_offset) == 0 && section_offset > 0) {
        /* Seek past the magic marker. */
        if (lseek(fd, section_offset + MAGIC_MARKER_LEN, SEEK_SET) != -1) {
            if (smol_read_metadata_with_config(fd, &metadata, &smfg_config) == 0 && smfg_config) {
                version = extract_version_from_smfg(smfg_config);
                free(smfg_config);
            }
        }
    }
#elif defined(_WIN32)
    int64_t section_offset;
    if (smol_find_pressed_data_offset_pe(fd, &section_offset) == 0 && section_offset > 0) {
        /* Seek past the magic marker. */
        if (lseek(fd, section_offset + MAGIC_MARKER_LEN, SEEK_SET) != -1) {
            if (smol_read_metadata_with_config(fd, &metadata, &smfg_config) == 0 && smfg_config) {
                version = extract_version_from_smfg(smfg_config);
                free(smfg_config);
            }
        }
    }
#endif

    /* Fallback to slow marker search if platform-specific search failed. */
    if (!version) {
        int64_t marker_offset = find_marker(fd, MAGIC_MARKER_PART1, MAGIC_MARKER_PART2,
                                            MAGIC_MARKER_PART3, MAGIC_MARKER_LEN);
        if (marker_offset > 0) {
            if (lseek(fd, marker_offset, SEEK_SET) != -1) {
                if (smol_read_metadata_with_config(fd, &metadata, &smfg_config) == 0 && smfg_config) {
                    version = extract_version_from_smfg(smfg_config);
                    free(smfg_config);
                }
            }
        }
    }

    /* Fallback to raw __smol_node_ver section (for uncompressed Node.js binaries).
     * This is needed for binpress which reads the version from the input binary
     * before compression - the PRESSED_DATA section doesn't exist yet. */
    if (!version) {
        version = extract_node_ver_from_section(fd);
    }

    /* For PE files, try VS_VERSION_INFO as final fallback.
     * This handles plain Node.js binaries from nodejs.org. */
    if (!version) {
        version = extract_pe_version_info(fd);
    }

    close(fd);
    return version;
}

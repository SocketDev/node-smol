/**
 * @file smol_segment_reader_metadata.c
 * @brief Format-agnostic SMOL metadata reading, validation, and marker search.
 *
 * Part of the smol_segment_reader.c translation unit (see that file's header
 * for the split layout). This part owns the on-disk SMOL metadata record:
 * reading it from a positioned descriptor, finding it via the magic marker,
 * validating its fields, and locating the marker inside an in-memory buffer.
 * No executable-format parsing lives here.
 */

#include "socketsecurity/bin-infra/smol_segment_reader_internal.h"

/**
 * Read SMOL metadata after marker position.
 *
 * Assumes file descriptor is positioned immediately after the magic marker.
 * This is extracted as a shared helper to avoid code duplication between
 * platforms with different marker-finding strategies.
 */
int smol_read_metadata_after_marker(int fd, smol_metadata_t *metadata) {
    if (fd < 0 || !metadata) {
        fprintf(stderr, "Error: Invalid arguments to smol_read_metadata_after_marker\n");
        return -1;
    }

    /* Initialize metadata structure. */
    memset(metadata, 0, sizeof(smol_metadata_t));

    /* Read compressed size (8 bytes). Use read_eintr for signal safety. */
    if (read_eintr(fd, &metadata->compressed_size, sizeof(metadata->compressed_size))
        != sizeof(metadata->compressed_size)) {
        fprintf(stderr, "Error: Failed to read compressed size\n");
        return -1;
    }

    /* Read uncompressed size (8 bytes). */
    if (read_eintr(fd, &metadata->uncompressed_size, sizeof(metadata->uncompressed_size))
        != sizeof(metadata->uncompressed_size)) {
        fprintf(stderr, "Error: Failed to read uncompressed size\n");
        return -1;
    }

    /* Read cache key (16 bytes, not null-terminated in binary). */
    char cache_key_raw[CACHE_KEY_LEN];
    if (read_eintr(fd, cache_key_raw, CACHE_KEY_LEN) != CACHE_KEY_LEN) {
        fprintf(stderr, "Error: Failed to read cache key\n");
        return -1;
    }
    /* Copy to output and null-terminate. */
    memcpy(metadata->cache_key, cache_key_raw, CACHE_KEY_LEN);
    metadata->cache_key[CACHE_KEY_LEN] = '\0';

    /* Read platform metadata (PLATFORM_METADATA_LEN bytes: platform, arch, libc). */
    if (read_eintr(fd, metadata->platform_metadata, PLATFORM_METADATA_LEN) != PLATFORM_METADATA_LEN) {
        fprintf(stderr, "Error: Failed to read platform metadata\n");
        return -1;
    }

    /* Read integrity hash (INTEGRITY_HASH_LEN bytes: SHA-256 of compressed data). */
    if (read_eintr(fd, metadata->integrity_hash, INTEGRITY_HASH_LEN) != INTEGRITY_HASH_LEN) {
        fprintf(stderr, "Error: Failed to read integrity hash\n");
        return -1;
    }

    /* Read has_smol_config flag (SMOL_CONFIG_FLAG_LEN byte). */
    uint8_t has_smol_config;
    if (read_eintr(fd, &has_smol_config, SMOL_CONFIG_FLAG_LEN) != SMOL_CONFIG_FLAG_LEN) {
        fprintf(stderr, "Error: Failed to read has_smol_config flag\n");
        return -1;
    }

    /* Skip smol config binary if present (SMOL_CONFIG_BINARY_LEN bytes). */
    if (has_smol_config != 0) {
        /* Smol config binary is present, skip it. */
        if (lseek(fd, SMOL_CONFIG_BINARY_LEN, SEEK_CUR) == -1) {
            fprintf(stderr, "Error: Failed to skip smol config binary: %s\n", strerror(errno));
            return -1;
        }
    }

    /* Record offset to compressed data (current position). */
    metadata->data_offset = lseek(fd, 0, SEEK_CUR);
    if (metadata->data_offset == -1) {
        fprintf(stderr, "Error: Failed to get data offset: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * Read SMOL metadata from file descriptor.
 */
int smol_read_metadata(int fd, smol_metadata_t *metadata) {
    if (fd < 0 || !metadata) {
        fprintf(stderr, "Error: Invalid arguments to smol_read_metadata\n");
        return -1;
    }

    /* Find compressed data marker. */
    int64_t data_offset = find_marker(fd, MAGIC_MARKER_PART1, MAGIC_MARKER_PART2,
                                      MAGIC_MARKER_PART3, MAGIC_MARKER_LEN);
    if (data_offset == -1) {
        fprintf(stderr, "Error: Could not find compressed data marker\n");
        return -1;
    }

    /* Seek to metadata (find_marker returns offset AFTER marker). */
    if (lseek(fd, data_offset, SEEK_SET) == -1) {
        fprintf(stderr, "Error: Failed to seek to metadata: %s\n", strerror(errno));
        return -1;
    }

    /* Use shared helper to read metadata. */
    return smol_read_metadata_after_marker(fd, metadata);
}

/**
 * Validate SMOL metadata.
 */
int smol_validate_metadata(const smol_metadata_t *metadata, size_t max_size) {
    if (!metadata) {
        fprintf(stderr, "Error: NULL metadata pointer\n");
        return -1;
    }

    /* Validate sizes are non-zero. */
    if (metadata->compressed_size == 0 || metadata->uncompressed_size == 0) {
        fprintf(stderr, "Error: Invalid sizes (compressed=%llu, uncompressed=%llu)\n",
                (unsigned long long)metadata->compressed_size,
                (unsigned long long)metadata->uncompressed_size);
        return -1;
    }

    /* Validate sizes against max_size limit (if specified). */
    if (max_size > 0) {
        if (metadata->compressed_size > max_size) {
            fprintf(stderr, "Error: Compressed size %llu exceeds limit %zu\n",
                    (unsigned long long)metadata->compressed_size, max_size);
            return -1;
        }
        if (metadata->uncompressed_size > max_size) {
            fprintf(stderr, "Error: Uncompressed size %llu exceeds limit %zu\n",
                    (unsigned long long)metadata->uncompressed_size, max_size);
            return -1;
        }
    }

    /* Validate cache key is exactly 16 hex characters. */
    if (strlen(metadata->cache_key) != CACHE_KEY_LEN) {
        fprintf(stderr, "Error: Cache key must be exactly %d characters (got %zu)\n",
                CACHE_KEY_LEN, strlen(metadata->cache_key));
        return -1;
    }

    for (int i = 0; i < CACHE_KEY_LEN; i++) {
        char c = metadata->cache_key[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            fprintf(stderr, "Error: Invalid cache key format (must be hex)\n");
            return -1;
        }
    }

    /* Validate platform metadata values are in valid ranges. */
    uint8_t platform = metadata->platform_metadata[0];
    uint8_t arch = metadata->platform_metadata[1];
    uint8_t libc = metadata->platform_metadata[2];

    if (platform > PLATFORM_WIN32) {
        fprintf(stderr, "Error: Invalid platform value: %u (expected 0-2)\n", platform);
        return -1;
    }
    if (arch > ARCH_ARM) {
        fprintf(stderr, "Error: Invalid architecture value: %u (expected 0-3)\n", arch);
        return -1;
    }
    if (libc != LIBC_GLIBC && libc != LIBC_MUSL && libc != LIBC_NA) {
        fprintf(stderr, "Error: Invalid libc value: %u (expected 0, 1, or 255)\n", libc);
        return -1;
    }

    return 0;
}

/**
 * Search for SMOL marker in memory buffer.
 */
int smol_find_marker_in_buffer(const uint8_t *buffer, size_t size, size_t *offset_out) {
    if (!buffer || !offset_out || size == 0) {
        return -1;
    }

    /* Build marker at runtime to avoid false positives. */
    char marker[MAGIC_MARKER_LEN + 1];
    int marker_len = snprintf(marker, sizeof(marker), "%s%s%s",
                             MAGIC_MARKER_PART1, MAGIC_MARKER_PART2, MAGIC_MARKER_PART3);

    if (marker_len != (int)MAGIC_MARKER_LEN) {
        return -1;
    }

    /* Search for marker in buffer. */
    if (size < MAGIC_MARKER_LEN) {
        return -1;
    }

    /* Two-level search: check first 4 bytes before full memcmp.
     * This is faster because most positions fail early without full comparison. */
    uint32_t marker_head = 0;
    memcpy(&marker_head, marker, 4);

    const size_t search_limit = size - MAGIC_MARKER_LEN;
    for (size_t i = 0; i <= search_limit; i++) {
        /* FAST: Check first 4 bytes (single comparison). */
        uint32_t buf_head;
        memcpy(&buf_head, buffer + i, 4);
        if (buf_head != marker_head) {
            continue;  /* Fast rejection. */
        }
        /* SLOW: Full comparison only if prefix matches. */
        if (memcmp(buffer + i, marker, MAGIC_MARKER_LEN) == 0) {
            *offset_out = i;
            return 0;
        }
    }

    return -1;
}

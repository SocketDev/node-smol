/**
 * @file smol_segment_reader_elf.c
 * @brief ELF section parsing for SMOL PRESSED_DATA and SMOL_NODE_VER.
 *
 * Part of the smol_segment_reader.c translation unit (see that file's header
 * for the split layout). This part owns every ELF header walk: the PT_NOTE
 * marker search behind smol_read_metadata_elf() and the SMOL_NODE_VER section
 * lookup the version reader falls back to.
 */

#include "socketsecurity/bin-infra/smol_segment_reader_internal.h"

#if defined(__linux__)
#include "socketsecurity/bin-infra/ptnote_finder.h"

/**
 * Find SMOL data offset directly from ELF PT_NOTE headers.
 * This is MUCH faster than scanning the entire file for the magic marker.
 *
 * Instead of reading potentially 20MB+ of file data to find the marker,
 * this reads only ELF headers (~4-8KB) to find the PT_NOTE with our marker.
 *
 * @param fd File descriptor (must be open for reading, seekable)
 * @param section_offset_out Output: file offset to PRESSED_DATA content (at marker start)
 * @return 0 on success, -1 on error
 */
static int smol_find_pressed_data_offset_elf(int fd, int64_t *section_offset_out) {
    if (fd < 0 || !section_offset_out) {
        return -1;
    }

    /* Use PT_NOTE finder which returns offset AT marker start. */
    long marker_pos = find_marker_in_ptnote(fd, MAGIC_MARKER_PART1, MAGIC_MARKER_PART2,
                                            MAGIC_MARKER_PART3, 0);
    if (marker_pos < 0) {
        return -1;
    }

    *section_offset_out = marker_pos;
    return 0;
}

/**
 * Read SMOL metadata using optimized ELF PT_NOTE search.
 * This is much faster than scanning the entire file for the magic marker.
 *
 * @param fd File descriptor (must be open for reading, seekable)
 * @param metadata Output structure (caller-allocated)
 * @return 0 on success, -1 on error
 */
int smol_read_metadata_elf(int fd, smol_metadata_t *metadata) {
    if (fd < 0 || !metadata) {
        fprintf(stderr, "Error: Invalid arguments to smol_read_metadata_elf\n");
        return -1;
    }

    /* Find marker in PT_NOTE segments (returns offset AT marker start). */
    long marker_pos = find_marker_in_ptnote(fd, MAGIC_MARKER_PART1, MAGIC_MARKER_PART2,
                                            MAGIC_MARKER_PART3, 0);
    if (marker_pos < 0) {
        /* Fallback to slow marker search. */
        return smol_read_metadata(fd, metadata);
    }

    /* Seek past the marker to the metadata. */
    if (lseek(fd, marker_pos + MAGIC_MARKER_LEN, SEEK_SET) == -1) {
        fprintf(stderr, "Error: Failed to seek to metadata after PT_NOTE marker: %s\n",
                strerror(errno));
        return -1;
    }

    /* Use shared helper to read metadata. */
    return smol_read_metadata_after_marker(fd, metadata);
}
#endif /* __linux__ */

#if defined(__linux__) || (!defined(__APPLE__) && !defined(_WIN32))
/**
 * Find SMOL_NODE_VER section in ELF binary.
 *
 * @param fd File descriptor (must be open for reading, seekable)
 * @param section_offset_out Output: file offset to section data
 * @param section_size_out Output: section size
 * @return 0 on success, -1 on error
 */
static int smol_find_node_ver_section_elf(int fd, int64_t *section_offset_out, size_t *section_size_out) {
    if (fd < 0 || !section_offset_out || !section_size_out) {
        return -1;
    }

    /* Seek to beginning. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        return -1;
    }

    /* Read ELF header. */
    unsigned char e_ident[16];
    if (read_eintr(fd, e_ident, 16) != 16) {
        return -1;
    }

    /* Check ELF magic. */
    if (e_ident[0] != 0x7F || e_ident[1] != 'E' || e_ident[2] != 'L' || e_ident[3] != 'F') {
        return -1;  /* Not an ELF file. */
    }

    int is_64bit = (e_ident[4] == 2);  /* EI_CLASS: 1=32-bit, 2=64-bit */

    /* Read section header offset and count. */
    uint64_t e_shoff;
    uint16_t e_shentsize, e_shnum, e_shstrndx;

    if (is_64bit) {
        /* 64-bit: e_shoff at offset 40 (8 bytes). */
        if (lseek(fd, 40, SEEK_SET) == -1) {
            return -1;
        }
        if (read_eintr(fd, &e_shoff, 8) != 8) {
            return -1;
        }
        /* e_shentsize at offset 58, e_shnum at 60, e_shstrndx at 62. */
        if (lseek(fd, 58, SEEK_SET) == -1) {
            return -1;
        }
    } else {
        /* 32-bit: e_shoff at offset 32 (4 bytes). */
        uint32_t shoff32;
        if (lseek(fd, 32, SEEK_SET) == -1) {
            return -1;
        }
        if (read_eintr(fd, &shoff32, 4) != 4) {
            return -1;
        }
        e_shoff = shoff32;
        /* e_shentsize at offset 46, e_shnum at 48, e_shstrndx at 50. */
        if (lseek(fd, 46, SEEK_SET) == -1) {
            return -1;
        }
    }

    if (read_eintr(fd, &e_shentsize, 2) != 2 ||
        read_eintr(fd, &e_shnum, 2) != 2 ||
        read_eintr(fd, &e_shstrndx, 2) != 2) {
        return -1;
    }

    /* Validate section header count. */
    if (e_shnum == 0 || e_shnum > 10000 || e_shstrndx >= e_shnum) {
        return -1;
    }

    /* Read string table section header to get string table offset. */
    off_t strtab_hdr_offset = e_shoff + (e_shstrndx * e_shentsize);
    if (lseek(fd, strtab_hdr_offset, SEEK_SET) == -1) {
        return -1;
    }

    uint64_t strtab_offset, strtab_size;
    if (is_64bit) {
        /* 64-bit section header: sh_offset at offset 24, sh_size at 32. */
        if (lseek(fd, strtab_hdr_offset + 24, SEEK_SET) == -1) {
            return -1;
        }
        if (read_eintr(fd, &strtab_offset, 8) != 8) {
            return -1;
        }
        if (read_eintr(fd, &strtab_size, 8) != 8) {
            return -1;
        }
    } else {
        /* 32-bit section header: sh_offset at offset 16, sh_size at 20. */
        uint32_t off32, size32;
        if (lseek(fd, strtab_hdr_offset + 16, SEEK_SET) == -1) {
            return -1;
        }
        if (read_eintr(fd, &off32, 4) != 4) {
            return -1;
        }
        if (read_eintr(fd, &size32, 4) != 4) {
            return -1;
        }
        strtab_offset = off32;
        strtab_size = size32;
    }

    /* Read string table (limit to 1MB for safety). */
    if (strtab_size > 1024 * 1024) {
        return -1;
    }

    char *strtab = (char *)malloc(strtab_size);
    if (!strtab) {
        return -1;
    }

    if (lseek(fd, strtab_offset, SEEK_SET) == -1) {
        free(strtab);
        return -1;
    }
    if (read_eintr(fd, strtab, strtab_size) != (ssize_t)strtab_size) {
        free(strtab);
        return -1;
    }

    /* Iterate through section headers looking for SMOL_NODE_VER. */
    for (uint16_t i = 0; i < e_shnum; i++) {
        off_t shdr_offset = e_shoff + (i * e_shentsize);
        if (lseek(fd, shdr_offset, SEEK_SET) == -1) {
            free(strtab);
            return -1;
        }

        uint32_t sh_name;
        uint64_t sh_offset_val, sh_size_val;

        if (read_eintr(fd, &sh_name, 4) != 4) {
            free(strtab);
            return -1;
        }

        /* Get section name from string table. */
        if (sh_name >= strtab_size) {
            continue;
        }
        const char *name = strtab + sh_name;

        /* Check if this is SMOL_NODE_VER. */
        if (strcmp(name, "SMOL_NODE_VER") == 0) {
            if (is_64bit) {
                /* Read sh_offset (at offset 24) and sh_size (at offset 32). */
                if (lseek(fd, shdr_offset + 24, SEEK_SET) == -1) {
                    free(strtab);
                    return -1;
                }
                if (read_eintr(fd, &sh_offset_val, 8) != 8) {
                    free(strtab);
                    return -1;
                }
                if (read_eintr(fd, &sh_size_val, 8) != 8) {
                    free(strtab);
                    return -1;
                }
            } else {
                /* Read sh_offset (at offset 16) and sh_size (at offset 20). */
                uint32_t off32, size32;
                if (lseek(fd, shdr_offset + 16, SEEK_SET) == -1) {
                    free(strtab);
                    return -1;
                }
                if (read_eintr(fd, &off32, 4) != 4) {
                    free(strtab);
                    return -1;
                }
                if (read_eintr(fd, &size32, 4) != 4) {
                    free(strtab);
                    return -1;
                }
                sh_offset_val = off32;
                sh_size_val = size32;
            }

            free(strtab);
            *section_offset_out = sh_offset_val;
            *section_size_out = (size_t)sh_size_val;
            return 0;
        }
    }

    free(strtab);
    return -1;  /* Section not found. */
}
#endif /* __linux__ */

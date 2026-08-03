/**
 * @file smol_segment_reader_macho.c
 * @brief Mach-O section parsing for SMOL PRESSED_DATA and __smol_node_ver.
 *
 * Part of the smol_segment_reader.c translation unit (see that file's header
 * for the split layout). This part owns every Mach-O header walk: the SMOL/
 * __PRESSED_DATA lookup behind smol_read_metadata_macho(), the LIEF-free
 * PRESSED_DATA presence check, and the __DATA/__smol_node_ver lookup the
 * version reader falls back to.
 */

#include "socketsecurity/bin-infra/smol_segment_reader_internal.h"

/* Mach-O magic numbers */
#define MH_MAGIC_64 0xfeedfacf
#define MH_CIGAM_64 0xcffaedfe
#define MH_MAGIC    0xfeedface
#define MH_CIGAM    0xcefaedfe
#define FAT_MAGIC   0xcafebabe
#define FAT_CIGAM   0xbebafeca
#define FAT_MAGIC_64 0xcafebabf
#define FAT_CIGAM_64 0xbfbafeca

/* Load command types */
#define LC_SEGMENT    0x1
#define LC_SEGMENT_64 0x19

/* Mach-O header offsets */
#define MACHO_HEADER_NCMDS_OFFSET 16  /* Offset of ncmds field in both mach_header and mach_header_64 */

#if defined(__APPLE__)
/**
 * Find SMOL segment offset directly from Mach-O headers.
 * This is MUCH faster than scanning the entire file for the magic marker.
 *
 * Instead of reading potentially 20MB+ of file data to find the marker,
 * this reads only ~4-8KB of Mach-O headers to find the segment offset directly.
 *
 * @param fd File descriptor (must be open for reading, seekable)
 * @param section_fileoff_out Output: file offset to __PRESSED_DATA section data
 * @return 0 on success, -1 on error
 */
static int smol_find_pressed_data_offset_macho(int fd, int64_t *section_fileoff_out) {
    if (fd < 0 || !section_fileoff_out) {
        return -1;
    }

    /* Seek to beginning. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        return -1;
    }

    /* Read magic number. */
    uint32_t magic;
    if (read_eintr(fd, &magic, sizeof(magic)) != sizeof(magic)) {
        return -1;
    }

    /* Check if it's a Mach-O file. */
    int is_64bit;
    if (magic == MH_MAGIC_64 || magic == MH_CIGAM_64) {
        is_64bit = 1;
    } else if (magic == MH_MAGIC || magic == MH_CIGAM) {
        is_64bit = 0;
    } else {
        return -1;  /* Not a Mach-O file. */
    }

    /* Read ncmds. */
    if (lseek(fd, MACHO_HEADER_NCMDS_OFFSET, SEEK_SET) == -1) {
        return -1;
    }

    uint32_t ncmds;
    if (read_eintr(fd, &ncmds, sizeof(ncmds)) != sizeof(ncmds)) {
        return -1;
    }

    /* Validate ncmds. */
    #define MAX_REASONABLE_NCMDS_FD 10000
    if (ncmds > MAX_REASONABLE_NCMDS_FD) {
        return -1;
    }

    /* Position after header. */
    off_t load_cmd_offset = is_64bit ? 32 : 28;
    if (lseek(fd, load_cmd_offset, SEEK_SET) == -1) {
        return -1;
    }

    /* Iterate through load commands looking for SMOL segment. */
    for (uint32_t i = 0; i < ncmds; i++) {
        off_t cmd_start = lseek(fd, 0, SEEK_CUR);
        if (cmd_start == -1) {
            return -1;
        }

        uint32_t cmd, cmdsize;
        if (read_eintr(fd, &cmd, sizeof(cmd)) != sizeof(cmd) ||
            read_eintr(fd, &cmdsize, sizeof(cmdsize)) != sizeof(cmdsize)) {
            return -1;
        }

        if (cmd == (uint32_t)(is_64bit ? LC_SEGMENT_64 : LC_SEGMENT)) {
            char segname[16];
            if (read_eintr(fd, segname, 16) != 16) {
                return -1;
            }

            /* Check if this is the SMOL segment. */
            if (strncmp(segname, "SMOL", 4) == 0) {
                /* Read segment fields to find sections.
                 * 64-bit: vmaddr(8), vmsize(8), fileoff(8), filesize(8), maxprot(4), initprot(4), nsects(4)
                 * 32-bit: vmaddr(4), vmsize(4), fileoff(4), filesize(4), maxprot(4), initprot(4), nsects(4)
                 */
                uint64_t seg_fileoff;
                uint32_t nsects;

                if (is_64bit) {
                    /* Skip vmaddr, vmsize. */
                    if (lseek(fd, 16, SEEK_CUR) == -1) {
                        return -1;
                    }
                    /* Read fileoff. */
                    if (read_eintr(fd, &seg_fileoff, 8) != 8) {
                        return -1;
                    }
                    /* Skip filesize, maxprot, initprot. */
                    if (lseek(fd, 8 + 4 + 4, SEEK_CUR) == -1) {
                        return -1;
                    }
                } else {
                    uint32_t fileoff32;
                    /* Skip vmaddr, vmsize. */
                    if (lseek(fd, 8, SEEK_CUR) == -1) {
                        return -1;
                    }
                    /* Read fileoff. */
                    if (read_eintr(fd, &fileoff32, 4) != 4) {
                        return -1;
                    }
                    seg_fileoff = fileoff32;
                    /* Skip filesize, maxprot, initprot. */
                    if (lseek(fd, 4 + 4 + 4, SEEK_CUR) == -1) {
                        return -1;
                    }
                }

                /* Read nsects. */
                if (read_eintr(fd, &nsects, sizeof(nsects)) != sizeof(nsects)) {
                    return -1;
                }

                #define MAX_REASONABLE_NSECTS_FD 1000
                if (nsects > MAX_REASONABLE_NSECTS_FD) {
                    return -1;
                }

                /* Skip flags (4 bytes). */
                if (lseek(fd, 4, SEEK_CUR) == -1) {
                    return -1;
                }

                /* Iterate through sections looking for __PRESSED_DATA. */
                for (uint32_t j = 0; j < nsects; j++) {
                    char sectname[16];
                    if (read_eintr(fd, sectname, 16) != 16) {
                        return -1;
                    }

                    /* Check if this is __PRESSED_DATA. */
                    if (sectname[0] == '_' && sectname[1] == '_' &&
                        strncmp(sectname, "__PRESSED_DATA", 14) == 0) {
                        /* Skip segname (16 bytes). */
                        if (lseek(fd, 16, SEEK_CUR) == -1) {
                            return -1;
                        }

                        /* Read section offset.
                         * 64-bit section: addr(8), size(8), offset(4)
                         * 32-bit section: addr(4), size(4), offset(4)
                         */
                        uint32_t section_offset;
                        if (is_64bit) {
                            /* Skip addr, size. */
                            if (lseek(fd, 16, SEEK_CUR) == -1) {
                                return -1;
                            }
                        } else {
                            /* Skip addr, size. */
                            if (lseek(fd, 8, SEEK_CUR) == -1) {
                                return -1;
                            }
                        }
                        if (read_eintr(fd, &section_offset, sizeof(section_offset)) != sizeof(section_offset)) {
                            return -1;
                        }

                        *section_fileoff_out = section_offset;
                        return 0;
                    }

                    /* Skip rest of section structure.
                     * 64-bit: sectname(16) + segname(16) + addr(8) + size(8) + offset(4) + align(4) + reloff(4) + nreloc(4) + flags(4) + reserved1(4) + reserved2(4) + reserved3(4) = 80 bytes
                     * 32-bit: sectname(16) + segname(16) + addr(4) + size(4) + offset(4) + align(4) + reloff(4) + nreloc(4) + flags(4) + reserved1(4) + reserved2(4) = 68 bytes
                     * We already read sectname (16 bytes), so skip remaining.
                     */
                    if (lseek(fd, is_64bit ? 64 : 52, SEEK_CUR) == -1) {
                        return -1;
                    }
                }

                /* SMOL segment found but no __PRESSED_DATA section. */
                return -1;
            }
        }

        /* Move to next load command. */
        if (cmdsize == 0 || cmdsize > INT32_MAX) {
            return -1;
        }
        if (lseek(fd, cmd_start + cmdsize, SEEK_SET) == -1) {
            return -1;
        }
    }

    return -1;  /* SMOL segment not found. */
}

/**
 * Read SMOL metadata using optimized Mach-O header parsing.
 * This is much faster than scanning the entire file for the magic marker.
 *
 * @param fd File descriptor (must be open for reading, seekable)
 * @param metadata Output structure (caller-allocated)
 * @return 0 on success, -1 on error
 */
int smol_read_metadata_macho(int fd, smol_metadata_t *metadata) {
    if (fd < 0 || !metadata) {
        fprintf(stderr, "Error: Invalid arguments to smol_read_metadata_macho\n");
        return -1;
    }

    /* Find __PRESSED_DATA section offset via Mach-O headers. */
    int64_t section_offset;
    if (smol_find_pressed_data_offset_macho(fd, &section_offset) != 0) {
        /* Fallback to slow marker search. */
        return smol_read_metadata(fd, metadata);
    }

    /* Seek to section data (which starts with magic marker). */
    if (lseek(fd, section_offset + MAGIC_MARKER_LEN, SEEK_SET) == -1) {
        fprintf(stderr, "Error: Failed to seek to section data: %s\n", strerror(errno));
        return -1;
    }

    /* Use shared helper to read metadata. */
    return smol_read_metadata_after_marker(fd, metadata);
}
#endif /* __APPLE__ */

#if defined(__APPLE__) || !defined(_WIN32)
/**
 * Find __smol_node_ver section in Mach-O __DATA segment.
 *
 * @param fd File descriptor (must be open for reading, seekable)
 * @param section_offset_out Output: file offset to section data
 * @param section_size_out Output: section size
 * @return 0 on success, -1 on error
 */
static int smol_find_node_ver_section_macho(int fd, int64_t *section_offset_out, size_t *section_size_out) {
    /* Reasonable limits for validation. */
    #define MAX_REASONABLE_NCMDS_NODE_VER 256
    #define MAX_REASONABLE_NSECTS_NODE_VER 1000

    if (fd < 0 || !section_offset_out || !section_size_out) {
        return -1;
    }

    /* Seek to beginning. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        return -1;
    }

    /* Read magic number. */
    uint32_t magic;
    if (read_eintr(fd, &magic, sizeof(magic)) != sizeof(magic)) {
        return -1;
    }

    /* Check if it's a Mach-O file. */
    int is_64bit;
    if (magic == MH_MAGIC_64 || magic == MH_CIGAM_64) {
        is_64bit = 1;
    } else if (magic == MH_MAGIC || magic == MH_CIGAM) {
        is_64bit = 0;
    } else {
        return -1;  /* Not a Mach-O file. */
    }

    /* Read ncmds. */
    if (lseek(fd, MACHO_HEADER_NCMDS_OFFSET, SEEK_SET) == -1) {
        return -1;
    }

    uint32_t ncmds;
    if (read_eintr(fd, &ncmds, sizeof(ncmds)) != sizeof(ncmds)) {
        return -1;
    }

    /* Validate ncmds. */
    if (ncmds > MAX_REASONABLE_NCMDS_NODE_VER) {
        return -1;
    }

    /* Position after header. */
    off_t load_cmd_offset = is_64bit ? 32 : 28;
    if (lseek(fd, load_cmd_offset, SEEK_SET) == -1) {
        return -1;
    }

    /* Iterate through load commands looking for __DATA segment. */
    for (uint32_t i = 0; i < ncmds; i++) {
        off_t cmd_start = lseek(fd, 0, SEEK_CUR);
        if (cmd_start == -1) {
            return -1;
        }

        uint32_t cmd, cmdsize;
        if (read_eintr(fd, &cmd, sizeof(cmd)) != sizeof(cmd) ||
            read_eintr(fd, &cmdsize, sizeof(cmdsize)) != sizeof(cmdsize)) {
            return -1;
        }

        if (cmd == (uint32_t)(is_64bit ? LC_SEGMENT_64 : LC_SEGMENT)) {
            char segname[16];
            if (read_eintr(fd, segname, 16) != 16) {
                return -1;
            }

            /* Check if this is the __DATA segment. */
            if (strncmp(segname, "__DATA", 6) == 0) {
                /* Read segment fields to find sections. */
                uint32_t nsects;

                if (is_64bit) {
                    /* Skip vmaddr(8), vmsize(8), fileoff(8), filesize(8), maxprot(4), initprot(4). */
                    if (lseek(fd, 8 + 8 + 8 + 8 + 4 + 4, SEEK_CUR) == -1) {
                        return -1;
                    }
                } else {
                    /* Skip vmaddr(4), vmsize(4), fileoff(4), filesize(4), maxprot(4), initprot(4). */
                    if (lseek(fd, 4 + 4 + 4 + 4 + 4 + 4, SEEK_CUR) == -1) {
                        return -1;
                    }
                }

                /* Read nsects. */
                if (read_eintr(fd, &nsects, sizeof(nsects)) != sizeof(nsects)) {
                    return -1;
                }

                if (nsects > MAX_REASONABLE_NSECTS_NODE_VER) {
                    return -1;
                }

                /* Skip flags (4 bytes). */
                if (lseek(fd, 4, SEEK_CUR) == -1) {
                    return -1;
                }

                /* Iterate through sections looking for __smol_node_ver. */
                for (uint32_t j = 0; j < nsects; j++) {
                    char sectname[16];
                    if (read_eintr(fd, sectname, 16) != 16) {
                        return -1;
                    }

                    /* Check if this is __smol_node_ver. */
                    if (strncmp(sectname, "__smol_node_ver", 15) == 0) {
                        /* Skip segname (16 bytes). */
                        if (lseek(fd, 16, SEEK_CUR) == -1) {
                            return -1;
                        }

                        /* Read section addr, size, offset.
                         * 64-bit: addr(8), size(8), offset(4)
                         * 32-bit: addr(4), size(4), offset(4)
                         */
                        uint64_t section_size;
                        uint32_t section_offset;

                        if (is_64bit) {
                            /* Skip addr. */
                            if (lseek(fd, 8, SEEK_CUR) == -1) {
                                return -1;
                            }
                            /* Read size. */
                            if (read_eintr(fd, &section_size, 8) != 8) {
                                return -1;
                            }
                        } else {
                            uint32_t size32;
                            /* Skip addr. */
                            if (lseek(fd, 4, SEEK_CUR) == -1) {
                                return -1;
                            }
                            /* Read size. */
                            if (read_eintr(fd, &size32, 4) != 4) {
                                return -1;
                            }
                            section_size = size32;
                        }

                        /* Read offset. */
                        if (read_eintr(fd, &section_offset, sizeof(section_offset)) != sizeof(section_offset)) {
                            return -1;
                        }

                        *section_offset_out = section_offset;
                        *section_size_out = (size_t)section_size;
                        return 0;
                    }

                    /* Skip rest of section structure.
                     * 64-bit: 80 bytes total, already read sectname (16), so skip 64.
                     * 32-bit: 68 bytes total, already read sectname (16), so skip 52.
                     */
                    if (lseek(fd, is_64bit ? 64 : 52, SEEK_CUR) == -1) {
                        return -1;
                    }
                }
            }
        }

        /* Move to next load command. */
        if (cmdsize == 0 || cmdsize > INT32_MAX) {
            return -1;
        }
        if (lseek(fd, cmd_start + cmdsize, SEEK_SET) == -1) {
            return -1;
        }
    }

    return -1;  /* Section not found. */
}
#endif /* __APPLE__ || !_WIN32 */

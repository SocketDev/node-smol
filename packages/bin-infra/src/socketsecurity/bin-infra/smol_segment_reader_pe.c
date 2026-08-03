/**
 * @file smol_segment_reader_pe.c
 * @brief PE section and resource parsing for SMOL and Node.js version data.
 *
 * Part of the smol_segment_reader.c translation unit (see that file's header
 * for the split layout). This part owns every PE header walk: the
 * .PRESSED_DATA section lookup behind smol_read_metadata_pe(), the
 * SMOL_NODE_VER section lookup, and the VS_VERSION_INFO resource reader used
 * as the last-resort version fallback for plain nodejs.org PE binaries.
 *
 * Note on guards: only the SMOL section finders are Windows-only. The
 * VS_VERSION_INFO reader is compiled on every platform because binject and
 * binpress inspect Windows binaries while running on macOS/Linux.
 */

#include "socketsecurity/bin-infra/smol_segment_reader_internal.h"

#if defined(_WIN32)
/**
 * Find SMOL section offset directly from PE headers.
 * This is MUCH faster than scanning the entire file for the magic marker.
 *
 * Instead of reading potentially 20MB+ of file data to find the marker,
 * this reads only the PE headers (~1-2KB) to find the section offset directly.
 *
 * @param fd File descriptor (must be open for reading, seekable)
 * @param section_offset_out Output: file offset to .PRESSED_DATA section data
 * @return 0 on success, -1 on error
 */
static int smol_find_pressed_data_offset_pe(int fd, int64_t *section_offset_out) {
    if (fd < 0 || !section_offset_out) {
        return -1;
    }

    /* Seek to beginning. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        return -1;
    }

    /* Read DOS header (first 64 bytes). */
    unsigned char dos_header[64];
    if (read_eintr(fd, dos_header, 64) != 64) {
        return -1;
    }

    /* Check DOS magic ("MZ"). */
    if (dos_header[0] != 'M' || dos_header[1] != 'Z') {
        return -1;  /* Not a PE file. */
    }

    /* Get PE header offset from e_lfanew (offset 0x3C, 4 bytes). */
    uint32_t pe_offset;
    memcpy(&pe_offset, dos_header + 0x3C, 4);

    /* Seek to PE header. */
    if (lseek(fd, pe_offset, SEEK_SET) == -1) {
        return -1;
    }

    /* Read PE signature + COFF header (24 bytes). */
    unsigned char pe_header[24];
    if (read_eintr(fd, pe_header, 24) != 24) {
        return -1;
    }

    /* Check PE signature ("PE\0\0"). */
    if (pe_header[0] != 'P' || pe_header[1] != 'E' ||
        pe_header[2] != 0 || pe_header[3] != 0) {
        return -1;  /* Not a PE file. */
    }

    /* Parse COFF header (starts at offset 4 in pe_header).
     * NumberOfSections: offset 2 (2 bytes)
     * SizeOfOptionalHeader: offset 16 (2 bytes)
     */
    uint16_t number_of_sections;
    uint16_t size_of_optional_header;
    memcpy(&number_of_sections, pe_header + 4 + 2, 2);
    memcpy(&size_of_optional_header, pe_header + 4 + 16, 2);

    /* Validate number of sections. */
    #define MAX_REASONABLE_SECTIONS 200
    if (number_of_sections > MAX_REASONABLE_SECTIONS) {
        return -1;
    }

    /* Calculate section table offset.
     * Section table starts immediately after optional header.
     * Section table offset = pe_offset + 24 (signature + COFF header) + SizeOfOptionalHeader
     */
    long section_table_offset = pe_offset + 24 + size_of_optional_header;

    /* Seek to section table. */
    if (lseek(fd, section_table_offset, SEEK_SET) == -1) {
        return -1;
    }

    /* Each section header is 40 bytes.
     * Name: offset 0 (8 bytes, null-padded)
     * PointerToRawData: offset 20 (4 bytes) - file offset
     */
    for (uint16_t i = 0; i < number_of_sections; i++) {
        unsigned char section_header[40];
        if (read_eintr(fd, section_header, 40) != 40) {
            return -1;
        }

        /* Check if this is .PRESSED_DATA section. */
        /* Section name is 8 bytes, null-padded. */
        if (section_header[0] == '.' &&
            section_header[1] == 'P' &&
            section_header[2] == 'R' &&
            section_header[3] == 'E' &&
            section_header[4] == 'S' &&
            section_header[5] == 'S' &&
            section_header[6] == 'E' &&
            section_header[7] == 'D') {
            /* Found it! Get PointerToRawData. */
            uint32_t pointer_to_raw_data;
            memcpy(&pointer_to_raw_data, section_header + 20, 4);
            *section_offset_out = pointer_to_raw_data;
            return 0;
        }
    }

    return -1;  /* Section not found. */
}

/**
 * Read SMOL metadata using optimized PE header parsing.
 * This is much faster than scanning the entire file for the magic marker.
 *
 * @param fd File descriptor (must be open for reading, seekable)
 * @param metadata Output structure (caller-allocated)
 * @return 0 on success, -1 on error
 */
int smol_read_metadata_pe(int fd, smol_metadata_t *metadata) {
    if (fd < 0 || !metadata) {
        fprintf(stderr, "Error: Invalid arguments to smol_read_metadata_pe\n");
        return -1;
    }

    /* Find .PRESSED_DATA section offset via PE headers. */
    int64_t section_offset;
    if (smol_find_pressed_data_offset_pe(fd, &section_offset) != 0) {
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
#endif /* _WIN32 */

#if defined(_WIN32)
/**
 * Find SMOL_NODE_VER section in PE binary.
 *
 * @param fd File descriptor (must be open for reading, seekable)
 * @param section_offset_out Output: file offset to section data
 * @param section_size_out Output: section size
 * @return 0 on success, -1 on error
 */
static int smol_find_node_ver_section_pe(int fd, int64_t *section_offset_out, size_t *section_size_out) {
    if (fd < 0 || !section_offset_out || !section_size_out) {
        return -1;
    }

    /* Seek to beginning. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        return -1;
    }

    /* Read DOS header. */
    unsigned char dos_header[64];
    if (read_eintr(fd, dos_header, 64) != 64) {
        return -1;
    }

    /* Check DOS magic ("MZ"). */
    if (dos_header[0] != 'M' || dos_header[1] != 'Z') {
        return -1;  /* Not a PE file. */
    }

    /* Get PE header offset from e_lfanew (offset 0x3C). */
    uint32_t pe_offset;
    memcpy(&pe_offset, dos_header + 0x3C, 4);

    /* Seek to PE header. */
    if (lseek(fd, pe_offset, SEEK_SET) == -1) {
        return -1;
    }

    /* Read PE signature + COFF header (24 bytes). */
    unsigned char pe_header[24];
    if (read_eintr(fd, pe_header, 24) != 24) {
        return -1;
    }

    /* Check PE signature. */
    if (pe_header[0] != 'P' || pe_header[1] != 'E' ||
        pe_header[2] != 0 || pe_header[3] != 0) {
        return -1;
    }

    /* Parse COFF header. */
    uint16_t number_of_sections;
    uint16_t size_of_optional_header;
    memcpy(&number_of_sections, pe_header + 4 + 2, 2);
    memcpy(&size_of_optional_header, pe_header + 4 + 16, 2);

    if (number_of_sections > MAX_REASONABLE_SECTIONS) {
        return -1;
    }

    /* Calculate section table offset. */
    long section_table_offset = pe_offset + 24 + size_of_optional_header;

    /* Iterate through sections looking for SMOL_NOD (8-char truncated name). */
    for (uint16_t i = 0; i < number_of_sections; i++) {
        if (lseek(fd, section_table_offset + (i * 40), SEEK_SET) == -1) {
            return -1;
        }

        unsigned char section_header[40];
        if (read_eintr(fd, section_header, 40) != 40) {
            return -1;
        }

        /* Section name is 8 bytes, may be truncated. */
        /* SMOL_NODE_VER truncates to "SMOL_NOD" */
        if (memcmp(section_header, "SMOL_NOD", 8) == 0) {
            uint32_t virtual_size, pointer_to_raw_data;
            memcpy(&virtual_size, section_header + 8, 4);
            memcpy(&pointer_to_raw_data, section_header + 20, 4);

            *section_offset_out = pointer_to_raw_data;
            *section_size_out = virtual_size;
            return 0;
        }
    }

    return -1;  /* Section not found. */
}
#endif /* _WIN32 */

/**
 * Extract version from PE VS_VERSION_INFO resource using native parsing.
 *
 * This reads the PE resource directory directly without LIEF, making it
 * 30-100x faster than full binary parsing.
 *
 * @param fd File descriptor (must be open for reading, seekable)
 * @return Version string (e.g., "22.5.0"), or NULL if not found.
 *         Caller must free() the returned string.
 */
static char* extract_pe_version_info(int fd) {
    if (fd < 0) {
        return NULL;
    }

    /* Seek to beginning. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        return NULL;
    }

    /* Read DOS header. */
    unsigned char dos_header[64];
    if (read_eintr(fd, dos_header, 64) != 64) {
        return NULL;
    }

    /* Check DOS magic. */
    if (dos_header[0] != 'M' || dos_header[1] != 'Z') {
        return NULL;
    }

    /* Get PE header offset. */
    uint32_t pe_offset;
    memcpy(&pe_offset, dos_header + 0x3C, 4);

    /* Seek to PE header. */
    if (lseek(fd, pe_offset, SEEK_SET) == -1) {
        return NULL;
    }

    /* Read PE signature + COFF header (24 bytes). */
    unsigned char pe_coff[24];
    if (read_eintr(fd, pe_coff, 24) != 24) {
        return NULL;
    }

    /* Check PE signature. */
    if (pe_coff[0] != 'P' || pe_coff[1] != 'E' || pe_coff[2] != 0 || pe_coff[3] != 0) {
        return NULL;
    }

    /* Get optional header size and number of sections. */
    uint16_t number_of_sections;
    uint16_t size_of_optional_header;
    memcpy(&number_of_sections, pe_coff + 4 + 2, 2);
    memcpy(&size_of_optional_header, pe_coff + 4 + 16, 2);

    /* Read optional header to get resource directory info. */
    if (size_of_optional_header < 96) {
        return NULL;  /* Too small for data directories. */
    }

    unsigned char *opt_header = (unsigned char *)malloc(size_of_optional_header);
    if (!opt_header) {
        return NULL;
    }

    if (read_eintr(fd, opt_header, size_of_optional_header) != size_of_optional_header) {
        free(opt_header);
        return NULL;
    }

    /* Determine PE32 vs PE64. */
    uint16_t magic;
    memcpy(&magic, opt_header, 2);
    int is_pe64 = (magic == 0x20b);  /* PE32+ */

    /* Data directories start at different offsets for PE32 vs PE64.
     * PE32: offset 96, PE64: offset 112.
     * Resource directory is index 2 (each entry is 8 bytes: RVA + Size).
     */
    size_t data_dir_offset = is_pe64 ? 112 : 96;
    size_t resource_dir_entry = data_dir_offset + (2 * 8);  /* Index 2. */

    if (resource_dir_entry + 8 > size_of_optional_header) {
        free(opt_header);
        return NULL;
    }

    uint32_t resource_rva, resource_size;
    memcpy(&resource_rva, opt_header + resource_dir_entry, 4);
    memcpy(&resource_size, opt_header + resource_dir_entry + 4, 4);
    free(opt_header);

    if (resource_rva == 0 || resource_size == 0) {
        return NULL;  /* No resource directory. */
    }

    /* Find .rsrc section to convert RVA to file offset. */
    long section_table_offset = pe_offset + 24 + size_of_optional_header;
    if (lseek(fd, section_table_offset, SEEK_SET) == -1) {
        return NULL;
    }

    uint32_t rsrc_raw_offset = 0;
    uint32_t rsrc_virtual_addr = 0;

    for (uint16_t i = 0; i < number_of_sections && i < 100; i++) {
        unsigned char section[40];
        if (read_eintr(fd, section, 40) != 40) {
            return NULL;
        }

        /* Check for .rsrc section. */
        if (memcmp(section, ".rsrc\0\0\0", 8) == 0) {
            memcpy(&rsrc_virtual_addr, section + 12, 4);  /* VirtualAddress. */
            memcpy(&rsrc_raw_offset, section + 20, 4);    /* PointerToRawData. */
            break;
        }
    }

    if (rsrc_raw_offset == 0) {
        return NULL;  /* .rsrc section not found. */
    }

    /* Convert resource directory RVA to file offset. */
    uint32_t resource_file_offset = rsrc_raw_offset + (resource_rva - rsrc_virtual_addr);

    /* Read resource directory header. */
    if (lseek(fd, resource_file_offset, SEEK_SET) == -1) {
        return NULL;
    }

    /* Resource directory table: 16 bytes header + entries.
     * We need to find RT_VERSION (type 16).
     */
    unsigned char rsrc_dir[16];
    if (read_eintr(fd, rsrc_dir, 16) != 16) {
        return NULL;
    }

    uint16_t num_name_entries, num_id_entries;
    memcpy(&num_name_entries, rsrc_dir + 12, 2);
    memcpy(&num_id_entries, rsrc_dir + 14, 2);

    uint16_t total_entries = num_name_entries + num_id_entries;
    if (total_entries > 100) {
        return NULL;  /* Sanity check. */
    }

    /* Each entry is 8 bytes: Name/ID (4) + Offset (4). */
    /* Scan for RT_VERSION (ID = 16). */
    uint32_t version_subdir_offset = 0;
    for (uint16_t i = 0; i < total_entries; i++) {
        unsigned char entry[8];
        if (read_eintr(fd, entry, 8) != 8) {
            return NULL;
        }

        uint32_t id, offset;
        memcpy(&id, entry, 4);
        memcpy(&offset, entry + 4, 4);

        /* RT_VERSION = 16. High bit of offset indicates subdirectory. */
        if (id == 16 && (offset & 0x80000000)) {
            version_subdir_offset = offset & 0x7FFFFFFF;
            break;
        }
    }

    if (version_subdir_offset == 0) {
        return NULL;  /* No RT_VERSION resource. */
    }

    /* Navigate to version subdirectory (level 2 - resource ID). */
    if (lseek(fd, resource_file_offset + version_subdir_offset, SEEK_SET) == -1) {
        return NULL;
    }

    if (read_eintr(fd, rsrc_dir, 16) != 16) {
        return NULL;
    }

    memcpy(&num_name_entries, rsrc_dir + 12, 2);
    memcpy(&num_id_entries, rsrc_dir + 14, 2);
    total_entries = num_name_entries + num_id_entries;
    if (total_entries == 0 || total_entries > 100) {
        return NULL;
    }

    /* Get first entry's offset (to language directory). */
    unsigned char entry[8];
    if (read_eintr(fd, entry, 8) != 8) {
        return NULL;
    }

    uint32_t lang_subdir_offset;
    memcpy(&lang_subdir_offset, entry + 4, 4);
    if (!(lang_subdir_offset & 0x80000000)) {
        return NULL;  /* Expected subdirectory. */
    }
    lang_subdir_offset &= 0x7FFFFFFF;

    /* Navigate to language subdirectory (level 3). */
    if (lseek(fd, resource_file_offset + lang_subdir_offset, SEEK_SET) == -1) {
        return NULL;
    }

    if (read_eintr(fd, rsrc_dir, 16) != 16) {
        return NULL;
    }

    memcpy(&num_name_entries, rsrc_dir + 12, 2);
    memcpy(&num_id_entries, rsrc_dir + 14, 2);
    total_entries = num_name_entries + num_id_entries;
    if (total_entries == 0 || total_entries > 100) {
        return NULL;
    }

    /* Get first entry's data offset. */
    if (read_eintr(fd, entry, 8) != 8) {
        return NULL;
    }

    uint32_t data_entry_offset;
    memcpy(&data_entry_offset, entry + 4, 4);
    if (data_entry_offset & 0x80000000) {
        return NULL;  /* Expected data entry, not subdirectory. */
    }

    /* Read resource data entry (16 bytes). */
    if (lseek(fd, resource_file_offset + data_entry_offset, SEEK_SET) == -1) {
        return NULL;
    }

    unsigned char data_entry[16];
    if (read_eintr(fd, data_entry, 16) != 16) {
        return NULL;
    }

    uint32_t data_rva, data_size;
    memcpy(&data_rva, data_entry, 4);
    memcpy(&data_size, data_entry + 4, 4);

    if (data_size < 52 || data_size > 65536) {
        return NULL;  /* Sanity check. */
    }

    /* Convert data RVA to file offset. */
    uint32_t data_file_offset = rsrc_raw_offset + (data_rva - rsrc_virtual_addr);

    /* Read VS_VERSION_INFO structure. */
    if (lseek(fd, data_file_offset, SEEK_SET) == -1) {
        return NULL;
    }

    unsigned char *version_info = (unsigned char *)malloc(data_size);
    if (!version_info) {
        return NULL;
    }

    if (read_eintr(fd, version_info, data_size) != (ssize_t)data_size) {
        free(version_info);
        return NULL;
    }

    /* VS_VERSION_INFO structure:
     * WORD wLength, WORD wValueLength, WORD wType
     * WCHAR szKey[] = "VS_VERSION_INFO" (null-terminated, padded to DWORD)
     * VS_FIXEDFILEINFO Value
     *
     * VS_FIXEDFILEINFO at offset ~40 (after header + key + padding):
     * DWORD dwSignature (0xFEEF04BD)
     * DWORD dwStrucVersion
     * DWORD dwFileVersionMS (major.minor as high.low words)
     * DWORD dwFileVersionLS (build.revision as high.low words)
     */

    /* Find VS_FIXEDFILEINFO signature. */
    char *version = NULL;
    for (size_t i = 0; i + 52 <= data_size; i++) {
        uint32_t sig;
        memcpy(&sig, version_info + i, 4);
        if (sig == 0xFEEF04BD) {
            /* Found VS_FIXEDFILEINFO. */
            uint32_t file_version_ms, file_version_ls;
            memcpy(&file_version_ms, version_info + i + 8, 4);
            memcpy(&file_version_ls, version_info + i + 12, 4);

            uint16_t major = (file_version_ms >> 16) & 0xFFFF;
            uint16_t minor = file_version_ms & 0xFFFF;
            uint16_t build = (file_version_ls >> 16) & 0xFFFF;

            /* Format as "major.minor.build". */
            version = (char *)malloc(32);
            if (version) {
                snprintf(version, 32, "%u.%u.%u", major, minor, build);
            }
            break;
        }
    }

    free(version_info);
    return version;
}

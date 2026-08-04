/**
 * @file smol_segment_reader_detect.c
 * @brief LIEF-free PRESSED_DATA presence checks for all three formats.
 *
 * Part of the smol_segment_reader.c translation unit (see that file's header
 * for the split layout). This part owns the yes/no detection phase declared in
 * smol_detect.h: it resolves the path, then delegates header parsing to
 * build-infra's format-agnostic section finders. The SMOL layer keeps only the
 * section naming.
 */

#include "socketsecurity/bin-infra/smol_segment_reader_internal.h"

/*
 * ============================================================================
 * Native Mach-O / ELF / PE PRESSED_DATA detection (no LIEF dependency)
 * ============================================================================
 *
 * Thin bin-infra wrappers around build-infra's format-agnostic section
 * finders. The SMOL layer owns section naming (".PRESSED_DATA", etc.)
 * and the resolution of relative paths; the finders own header parsing.
 */

/* Open path with relative-path resolution; caller owns the FILE*. */
static FILE *smol_open_resolved(const char *path) {
    if (!path) return NULL;
    char resolved_path[PATH_MAX];
    const char *to_open = resolve_absolute_path(path, resolved_path);
    return fopen(to_open, "rb");
}

int smol_has_pressed_data_macho_impl(const char *path) {
    if (!path) return -1;
    FILE *fp = smol_open_resolved(path);
    if (!fp) return 0;
    int64_t off;
    uint64_t size;
    int ok = (bf_find_macho_section(fp, MACHO_SEGMENT_SMOL,
                                    MACHO_SECTION_PRESSED_DATA,
                                    &off, &size) == 0 && size > 0);
    fclose(fp);
    return ok ? 1 : 0;
}

int smol_has_pressed_data_elf_impl(const char *path) {
    if (!path) return -1;
    FILE *fp = smol_open_resolved(path);
    if (!fp) return 0;
    int64_t off;
    uint64_t size;
    int ok = (bf_find_elf_section(fp, ELF_SECTION_PRESSED_DATA,
                                  &off, &size) == 0 && size > 0);
    fclose(fp);
    return ok ? 1 : 0;
}

int smol_has_pressed_data_pe_impl(const char *path) {
    if (!path) return -1;
    FILE *fp = smol_open_resolved(path);
    if (!fp) return 0;
    /* ".PRESSED_DATA" truncates to ".PRESSED" in PE's 8-byte name slot. */
    static const char kName8[8] = {'.', 'P', 'R', 'E', 'S', 'S', 'E', 'D'};
    int64_t off;
    uint32_t size;
    int ok = (bf_find_pe_section(fp, kName8, &off, &size) == 0 && size > 0);
    fclose(fp);
    return ok ? 1 : 0;
}

// ============================================================================
// binject_commands.c — Orchestration for each binject CLI command
// ============================================================================
//
// WHAT THIS FILE DOES
// Implements the one function per binject command that decides what to do:
// single-resource inject, batch inject (SEA and/or VFS in one pass), list,
// extract, and verify. Each one detects the binary format and delegates to the
// matching platform backend (elf_inject.c, pe_inject.c,
// macho_inject_lief_wrapper.c), preparing VFS archives and repacking stubs
// along the way.
//
// WHY IT EXISTS
// This is the top layer of the "engine" behind binject: main.c parses flags,
// this file turns them into a sequence of format-specific calls. It is the
// third of binject's three phases (core primitives in binject.c → compressed
// stub cache in binject_stub_cache.c → command orchestration here) and was
// split out of binject.c when that file outgrew the 1000-line source cap.
// ============================================================================

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  // For O_CLOEXEC, lstat, fdopen
#define _XOPEN_SOURCE 700        // For additional POSIX features
#ifdef __APPLE__
#define _DARWIN_C_SOURCE         // For O_NOFOLLOW on macOS
#endif
#endif  // !_WIN32

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
/* Windows compat shims (PATH_MAX, S_ISREG/S_ISDIR, 64-bit fseek/ftell). */
#include "socketsecurity/build-infra/file_io_common.h"
#include "socketsecurity/binject/binject.h"
#include "socketsecurity/binject/binject_internal.h"
#include "socketsecurity/binject/stub_repack.h"

/* Shared segment/section naming and stub detection from bin-infra */
#include "socketsecurity/bin-infra/segment_names.h"
#include "socketsecurity/bin-infra/smol_detect.h"

/* Shared file utilities from build-infra */
#include "socketsecurity/build-infra/file_utils.h"

/* TAR/GZIP utilities for VFS preparation */
#include "socketsecurity/build-infra/tar_create.h"
#include "socketsecurity/build-infra/gzip_compress.h"

/* Human-readable format names indexed by binject_format_t enum values */
static const char *BINJECT_FORMAT_NAMES[] = {"unknown", "Mach-O", "ELF", "PE"};

/* Map user-friendly section identifiers to actual section names for each format.
 * Returns the format-specific section name, or the original section_name if no mapping exists. */
static const char *map_section_name(binject_format_t format, const char *section_name) {
    if (format == BINJECT_FORMAT_MACHO) {
        if (strcmp(section_name, "sea") == 0) {
            return MACHO_SECTION_NODE_SEA_BLOB;
        } else if (strcmp(section_name, "vfs") == 0) {
            return MACHO_SECTION_SMOL_VFS_BLOB;
        }
    } else if (format == BINJECT_FORMAT_ELF) {
        if (strcmp(section_name, "sea") == 0) {
            return ELF_NOTE_NODE_SEA_BLOB;
        } else if (strcmp(section_name, "vfs") == 0) {
            return ELF_NOTE_SMOL_VFS_BLOB;
        }
    } else if (format == BINJECT_FORMAT_PE) {
        if (strcmp(section_name, "sea") == 0) {
            return PE_RESOURCE_NODE_SEA_BLOB;
        } else if (strcmp(section_name, "vfs") == 0) {
            return PE_RESOURCE_SMOL_VFS_BLOB;
        }
    }
    return section_name;
}

/* CLI: single command */
int binject_single(const char *executable, const char *output, const char *resource_file,
                   const char *section_name) {
    (void)output; // Unused parameter.
    /* During injection, work with the provided binary directly.
     * Stub extraction should only happen at execution time, not during injection.
     * This prevents false positives when injecting into binaries that contain
     * the magic marker string in their code (like binject itself). */
    printf("Injecting resource into %s...\n", executable);
    printf("  Resource: %s\n", resource_file);
    printf("  Section: %s\n", section_name);

    /* Detect binary format */
    binject_format_t format = binject_detect_format(executable);
    printf("  Format: %s\n", BINJECT_FORMAT_NAMES[format]);

    if (format == BINJECT_FORMAT_UNKNOWN) {
        fprintf(stderr, "Error: Unsupported binary format\n");
        return BINJECT_ERROR_INVALID_FORMAT;
    }

    /* Read resource */
    uint8_t *data = NULL;
    size_t size = 0;
    int rc = binject_read_resource(resource_file, &data, &size);
    if (rc != BINJECT_OK) {
        return rc;
    }

    printf("  Resource size: %zu bytes\n", size);

    /* Calculate checksum */
    uint32_t checksum = binject_checksum(data, size);
    printf("  Checksum: 0x%08x\n", checksum);

    /* Platform-specific injection. */
    if (format == BINJECT_FORMAT_MACHO) {
        /* Map section identifier to Mach-O segment/section names. */
        const char *segment = MACHO_SEGMENT_NODE_SEA;
        const char *macho_section = NULL;

        if (strcmp(section_name, "sea") == 0) {
            macho_section = MACHO_SECTION_NODE_SEA_BLOB;
        } else if (strcmp(section_name, "vfs") == 0) {
            macho_section = MACHO_SECTION_SMOL_VFS_BLOB;
        } else {
            fprintf(stderr, "Error: Unknown section identifier '%s'\n", section_name);
            free(data);
            return BINJECT_ERROR_INVALID_ARGS;
        }

        rc = binject_macho(executable, segment, macho_section, data, size);
    } else if (format == BINJECT_FORMAT_ELF) {
        /* Use LIEF for cross-platform ELF injection. */
        rc = binject_elf_lief(executable, section_name, data, size);
    } else if (format == BINJECT_FORMAT_PE) {
        /* Use LIEF for cross-platform PE injection. */
        rc = binject_pe_lief(executable, section_name, data, size);
    } else {
        fprintf(stderr, "Error: Unsupported binary format\n");
        rc = BINJECT_ERROR_INVALID_FORMAT;
    }

    free(data);
    return rc;
}

/**
 * Detect if binary is a SMOL stub (compressed or uncompressed).
 * Checks for __PRESSED_DATA section (macOS) or PRESSED_DATA section (Linux/Windows).
 *
 * Returns 1 if binary has SMOL section (regardless of compression status).
 * Returns 0 if binary has no SMOL section.
 *
 * Note: Use binject_is_compressed_stub() to check if stub contains compressed data.
 */
static int binject_is_smol_stub(const char *executable_path) {
    binject_format_t format = binject_detect_format(executable_path);

    /* Use pure-C detection for all three formats to skip LIEF's full parse. */
    switch (format) {
        case BINJECT_FORMAT_MACHO:
            return smol_has_pressed_data_macho_impl(executable_path);
        case BINJECT_FORMAT_ELF:
            return smol_has_pressed_data_elf_impl(executable_path);
        case BINJECT_FORMAT_PE:
            return smol_has_pressed_data_pe_impl(executable_path);
        default:
            return 0;
    }
}

/* CLI: batch inject command (SEA and/or VFS in one pass) */
int binject_batch(const char *executable, const char *output,
                         const char *sea_resource, const char *vfs_resource,
                         int vfs_in_memory, int skip_repack, const uint8_t *vfs_config_data) {
    (void)vfs_in_memory; // Reserved for future VFS extraction control at runtime

    /* Initialize all allocated resources to NULL for cleanup */
    int rc = BINJECT_OK;
    char *temp_extracted = NULL;
    uint8_t *sea_data = NULL;
    uint8_t *vfs_data = NULL;
    char *temp_injected = NULL;

    /* Detect SMOL stub and extract if needed */
    int is_smol_stub = 0;
    const char *original_stub = NULL;
    const char *injection_target = executable;

    /* Detect stub type: check SMOL section first, then compression status */
    if (!skip_repack) {
        is_smol_stub = binject_is_smol_stub(executable);
    }

    int is_compressed = binject_is_compressed_stub(injection_target);

    /* Handle SMOL stubs */
    if (is_smol_stub && !skip_repack) {
        if (is_compressed) {
            /* Compressed SMOL stub - extract and inject into extracted binary */
            original_stub = executable;
            printf("\nDetected SMOL compressed stub\n");
        } else {
            /* Uncompressed SMOL stub (minimal decompressor without embedded data) */
            fprintf(stderr, "Error: Cannot inject into uncompressed stub binary\n");
            fprintf(stderr, "  This is a minimal stub launcher without an embedded node binary\n");
            fprintf(stderr, "  Options:\n");
            fprintf(stderr, "  1. Inject into the original node binary before compression\n");
            fprintf(stderr, "  2. Inject into a compressed node-smol with embedded node binary\n");
            fprintf(stderr, "  3. For compressed stubs, binject will automatically extract and inject\n");
            rc = BINJECT_ERROR_INVALID_FORMAT;
            goto cleanup;
        }
    }
    char extracted_path[PATH_MAX];
    const char *target_binary = injection_target;

    if (is_compressed) {
        if (!is_smol_stub) {
            printf("Detected compressed self-extracting stub: %s\n", injection_target);
        }

        /* Get path to extracted binary in cache */
        rc = binject_get_extracted_path(injection_target, extracted_path, sizeof(extracted_path));
        if (rc != BINJECT_OK) {
            goto cleanup;
        }

        printf("Looking up extracted binary in cache...\n");

        /* Check if extracted binary exists in cache */
        BINJECT_STAT_STRUCT st;
        if (binject_stat(extracted_path, &st) != 0) {
            printf("Extracted binary not found in cache, extracting with LIEF...\n");

            /* Use LIEF-based extraction which reads section data directly.
             * The buffer-based binject_extract_stub_to_cache fails on repacked stubs
             * because it searches file headers instead of Mach-O/ELF/PE sections.
             * This enables auto-overwrite (cache key changes after repack) and
             * cross-platform builds (e.g., extracting Linux stubs on macOS). */
            int extract_result = smol_extract_binary_lief(injection_target, extracted_path);
            if (extract_result != 0) {
                fprintf(stderr, "Error: Failed to extract compressed stub\n");
                rc = BINJECT_ERROR;
                goto cleanup;
            }
        }

        printf("Found extracted binary: %s\n", extracted_path);

        /* Inject into the extracted binary */
        printf("Injecting resource into %s...\n", extracted_path);
        target_binary = extracted_path;
    } else {
        printf("Batch injection into %s...\n", injection_target);
    }

    /* Detect binary format */
    binject_format_t format = binject_detect_format(target_binary);
    printf("  Format: %s\n", BINJECT_FORMAT_NAMES[format]);

    if (format == BINJECT_FORMAT_UNKNOWN) {
        fprintf(stderr, "Error: Unknown binary format\n");
        rc = BINJECT_ERROR_INVALID_FORMAT;
        goto cleanup;
    }

    /* Read SEA resource if provided */
    size_t sea_size = 0;
    if (sea_resource) {
        rc = binject_read_resource(sea_resource, &sea_data, &sea_size);
        if (rc != BINJECT_OK) {
            goto cleanup;
        }
        printf("  SEA resource: %s (%zu bytes)\n", sea_resource, sea_size);
    }

    /* Read VFS resource if provided */
    size_t vfs_size = 0;
    int vfs_compat_mode = 0;
    if (vfs_resource && strlen(vfs_resource) > 0) {
        /* Handle different VFS input formats */
        if (is_directory(vfs_resource)) {
            /* Directory: create tar.gz */
            printf("  VFS source: directory %s\n", vfs_resource);
            int tar_rc = tar_gz_create_from_directory(vfs_resource, &vfs_data, &vfs_size, 6);
            if (tar_rc != TAR_OK) {
                fprintf(stderr, "Error: Failed to create tar.gz from directory\n");
                rc = BINJECT_ERROR;
                goto cleanup;
            }
            printf("  VFS resource: %zu bytes (tar.gz from directory)\n", vfs_size);
        } else if (is_tar_gz_file(vfs_resource)) {
            /* Already tar.gz: read as-is */
            printf("  VFS source: tar.gz file %s\n", vfs_resource);
            rc = binject_read_resource(vfs_resource, &vfs_data, &vfs_size);
            if (rc != BINJECT_OK) {
                goto cleanup;
            }
            /* Verify it's actually gzip data */
            if (!is_gzip_data(vfs_data, vfs_size)) {
                fprintf(stderr, "Error: File has .tar.gz extension but doesn't contain gzip data\n");
                rc = BINJECT_ERROR;
                goto cleanup;
            }
            printf("  VFS resource: %s (%zu bytes)\n", vfs_resource, vfs_size);
        } else if (is_tar_file(vfs_resource)) {
            /* Uncompressed tar: read and compress */
            printf("  VFS source: tar file %s (will compress)\n", vfs_resource);
            uint8_t *tar_data = NULL;
            size_t tar_size = 0;
            rc = binject_read_resource(vfs_resource, &tar_data, &tar_size);
            if (rc != BINJECT_OK) {
                goto cleanup;
            }
            /* Compress with gzip */
            int gz_rc = gzip_compress(tar_data, tar_size, &vfs_data, &vfs_size, 6);
            free(tar_data);
            if (gz_rc != GZIP_OK) {
                fprintf(stderr, "Error: gzip compression failed\n");
                rc = BINJECT_ERROR;
                goto cleanup;
            }
            printf("  VFS resource: %zu bytes (compressed from %zu bytes)\n", vfs_size, tar_size);
        } else {
            /* Unknown format - try to read and auto-detect */
            printf("  VFS source: %s (auto-detecting format)\n", vfs_resource);
            uint8_t *file_data = NULL;
            size_t file_size = 0;
            rc = binject_read_resource(vfs_resource, &file_data, &file_size);
            if (rc != BINJECT_OK) {
                goto cleanup;
            }
            /* Check if it's gzip data */
            if (is_gzip_data(file_data, file_size)) {
                printf("  Detected gzip format\n");
                vfs_data = file_data;
                vfs_size = file_size;
            } else {
                /* Assume it's uncompressed tar, compress it */
                printf("  Assuming uncompressed data, compressing with gzip...\n");
                int gz_rc = gzip_compress(file_data, file_size, &vfs_data, &vfs_size, 6);
                free(file_data);
                if (gz_rc != GZIP_OK) {
                    fprintf(stderr, "Error: gzip compression failed\n");
                    rc = BINJECT_ERROR;
                    goto cleanup;
                }
            }
            printf("  VFS resource: %zu bytes\n", vfs_size);
        }
    } else if (vfs_resource && strlen(vfs_resource) == 0) {
        /* VFS compatibility mode - create empty 0-byte VFS section */
        printf("  VFS resource: compatibility mode (0-byte flag)\n");
        vfs_size = 0;
        vfs_data = NULL;
        vfs_compat_mode = 1;
    }

    /* Perform injection based on format */
    /* For compressed stubs, inject into extracted binary and write output there temporarily.
     * We'll repack the stub after injection succeeds. */
    const char *injection_output = is_compressed ? target_binary : output;

    /* Debug: Print VFS data sizes before injection */
#ifdef DEBUG
    printf("DEBUG: About to inject VFS - vfs_data=%p, vfs_size=%zu, vfs_config_data=%p\n",
           (void*)vfs_data, vfs_size, (void*)vfs_config_data);
#endif

    if (format == BINJECT_FORMAT_MACHO) {
        rc = binject_macho_lief_batch(target_binary, injection_output, sea_data, sea_size, vfs_data, vfs_size, vfs_compat_mode, vfs_config_data);
    } else if (format == BINJECT_FORMAT_ELF) {
        rc = binject_batch_elf(target_binary, injection_output, sea_data, sea_size, vfs_data, vfs_size, vfs_compat_mode, vfs_config_data);
    } else if (format == BINJECT_FORMAT_PE) {
        rc = binject_batch_pe(target_binary, injection_output, sea_data, sea_size, vfs_data, vfs_size, vfs_compat_mode, vfs_config_data);
    } else {
        fprintf(stderr, "Error: Unsupported binary format for injection\n");
        rc = BINJECT_ERROR_INVALID_FORMAT;
    }

    if (rc != BINJECT_OK) {
        goto cleanup;
    }

    /* If this was a compressed stub (but NOT a SMOL stub), repack it with the modified binary */
    /* SMOL stubs are handled by the separate repack block below (lines 852-890) */
    if (is_compressed && !is_smol_stub) {
        if (skip_repack) {
            printf("\n");
            printf("⚠ Skipping stub repacking (--skip-repack flag)\n");
            printf("✓ Modified extracted binary: %s\n", target_binary);
            printf("  You can test this binary directly before repacking.\n");
        } else {
            printf("\n");
            printf("Repacking compressed stub...\n");
            rc = binject_repack_workflow(executable, target_binary, output, vfs_config_data);
            if (rc != BINJECT_OK) {
                fprintf(stderr, "Error: Failed to repack compressed stub\n");
                goto cleanup;
            }
            printf("✓ Stub repacking complete\n");
        }
    }

    /* Repack SMOL stub if we detected it */
    if (is_smol_stub && original_stub) {
        printf("\nRepacking SMOL stub...\n");

        /* Move injected binary to temp location */
        temp_injected = (char*)malloc(PATH_MAX);
        if (!temp_injected) {
            rc = BINJECT_ERROR_WRITE_FAILED;
            goto cleanup;
        }

        snprintf(temp_injected, PATH_MAX, "%s.injected", target_binary);

        /* For cache-based injection, the injected binary is at the cache path (target_binary) */
        const char *injected_binary = target_binary;

        if (rename(injected_binary, temp_injected) != 0) {
            fprintf(stderr, "Error: Failed to rename injected binary: %s\n", strerror(errno));
            rc = BINJECT_ERROR_WRITE_FAILED;
            goto cleanup;
        }

        /* Run repack workflow */
        rc = binject_repack_workflow(
            original_stub,
            temp_injected,
            output,
            vfs_config_data
        );

        /* Cleanup temp_injected */
        remove(temp_injected);
        free(temp_injected);
        temp_injected = NULL;

        if (rc != 0) {
            goto cleanup;
        }

        printf("✓ SMOL stub repacked successfully\n");
    }

cleanup:
    /* Free all allocated resources */
    if (sea_data) {
        free(sea_data);
    }
    if (vfs_data) {
        free(vfs_data);
    }
    if (temp_extracted) {
        remove(temp_extracted);
        free(temp_extracted);
    }
    if (temp_injected) {
        remove(temp_injected);
        free(temp_injected);
    }

    return rc;
}

/* CLI: list command */
int binject_list(const char *executable) {
    printf("Listing resources in %s...\n\n", executable);

    binject_format_t format = binject_detect_format(executable);
    if (format == BINJECT_FORMAT_UNKNOWN) {
        fprintf(stderr, "Error: Unsupported binary format\n");
        return BINJECT_ERROR_INVALID_FORMAT;
    }

    if (format == BINJECT_FORMAT_MACHO) {
        return binject_macho_list(executable);
    } else if (format == BINJECT_FORMAT_ELF) {
        return binject_elf_list(executable);
    } else if (format == BINJECT_FORMAT_PE) {
        return binject_pe_list(executable);
    } else {
        fprintf(stderr, "Error: Unsupported binary format\n");
        return BINJECT_ERROR_INVALID_FORMAT;
    }
}

/* CLI: extract command */
int binject_extract(const char *executable, const char *section_name,
                    const char *output_file) {
    printf("Extracting section '%s' from %s...\n", section_name, executable);
    printf("  Output: %s\n", output_file);

    binject_format_t format = binject_detect_format(executable);
    if (format == BINJECT_FORMAT_UNKNOWN) {
        fprintf(stderr, "Error: Unsupported binary format\n");
        return BINJECT_ERROR_INVALID_FORMAT;
    }

    const char *actual_section_name = map_section_name(format, section_name);

    if (format == BINJECT_FORMAT_MACHO) {
        return binject_macho_extract(executable, actual_section_name, output_file);
    } else if (format == BINJECT_FORMAT_ELF) {
        return binject_elf_extract(executable, actual_section_name, output_file);
    } else if (format == BINJECT_FORMAT_PE) {
        return binject_pe_extract(executable, actual_section_name, output_file);
    } else {
        fprintf(stderr, "Error: Unsupported binary format\n");
        return BINJECT_ERROR_INVALID_FORMAT;
    }
}

/* CLI: verify command */
int binject_verify(const char *executable, const char *section_name) {
    printf("Verifying section '%s' in %s...\n", section_name, executable);

    binject_format_t format = binject_detect_format(executable);
    if (format == BINJECT_FORMAT_UNKNOWN) {
        fprintf(stderr, "Error: Unsupported binary format\n");
        return BINJECT_ERROR_INVALID_FORMAT;
    }

    const char *actual_section_name = map_section_name(format, section_name);

    if (format == BINJECT_FORMAT_MACHO) {
        return binject_macho_verify(executable, actual_section_name);
    } else if (format == BINJECT_FORMAT_ELF) {
        return binject_elf_verify(executable, actual_section_name);
    } else if (format == BINJECT_FORMAT_PE) {
        return binject_pe_verify(executable, actual_section_name);
    } else {
        fprintf(stderr, "Error: Unsupported binary format\n");
        return BINJECT_ERROR_INVALID_FORMAT;
    }
}

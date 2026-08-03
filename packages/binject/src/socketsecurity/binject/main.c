// ============================================================================
// main.c — CLI entry point for binject
// ============================================================================
//
// WHAT THIS FILE DOES
// Parses command-line arguments and dispatches to the right binject operation:
// single-resource injection, batch injection (SEA + VFS together), listing
// sections, extracting data, or verifying that a section exists.
//
// WHY IT EXISTS
// This is the "front door" of the binject tool. Build scripts call
// `binject --sea config.json -o output` and this file turns those flags
// into calls to the core injection functions defined in binject.c.
//
// The two jobs that used to sit in this file — finding a usable host Node.js
// (node_resolve.c) and generating a SEA blob from a sea-config.json
// (sea_blob.c) — moved to sibling units when this file outgrew the 1000-line
// source cap. What is left is argument parsing and dispatch.
// ============================================================================

/**
 * binject - Pure C alternative to postject
 * Main CLI entry point
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  // For strdup
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#include "socketsecurity/build-infra/posix_compat.h"
#else
#include <unistd.h>
#endif
#include "socketsecurity/binject/binject.h"
#include "socketsecurity/binject/json_parser.h"
#include "socketsecurity/binject/node_resolve.h"
#include "socketsecurity/binject/sea_blob.h"
#include "socketsecurity/binject/smol_config.h"
#include "socketsecurity/binject/vfs_config.h"
#include "socketsecurity/binject/vfs_utils.h"
#include "socketsecurity/bin-infra/smol_segment_reader.h"
#include "socketsecurity/build-infra/debug_common.h"

/**
 * Check if a file has a .json extension
 */
static int is_json_file(const char *path) {
    if (!path) return 0;
    const char *ext = strrchr(path, '.');
    return ext && strcmp(ext, ".json") == 0;
}

static void print_usage(const char *program) {
    printf("binject - Pure C alternative to postject\n\n");
    printf("Usage:\n");
    printf("  %s inject -e <executable> -o <output> [--sea <path>] [--vfs <path>|--vfs-on-disk <path>|--vfs-in-memory <path>|--vfs-compat] [--skip-repack]\n", program);
    printf("  %s blob <sea-config.json>\n", program);
    printf("  %s list <executable>\n", program);
    printf("  %s extract -e <executable> [--vfs|--sea] -o <output>\n", program);
    printf("  %s verify -e <executable> [--vfs|--sea]\n", program);
    printf("  %s --help\n", program);
    printf("  %s --version\n\n", program);
    printf("Commands:\n");
    printf("  inject            Inject a resource into an executable\n");
    printf("  blob              Generate SEA blob from sea-config.json (does not inject)\n");
    printf("  list              List all embedded resources\n");
    printf("  extract           Extract a resource from an executable\n");
    printf("  verify            Verify the integrity of a resource\n\n");
    printf("Options:\n");
    printf("  -o, --output <path>           Output file path\n");
    printf("  -e, --executable <path>       Input executable path\n");
    printf("  --vfs <path>                  Inject VFS to NODE_SEA/__SMOL_VFS_BLOB (extracts to disk at runtime)\n");
    printf("                                Accepts: directory, .tar.gz, .tgz, or .tar (auto-compressed)\n");
    printf("                                Note: VFS can also be configured in sea-config.json (smol.vfs section)\n");
    printf("  --vfs-on-disk <path>          Alias for --vfs\n");
    printf("  --vfs-in-memory <path>        Inject VFS and keep in memory at runtime (no extraction)\n");
    printf("  --vfs-compat                  Enable VFS support without bundling files (compatibility mode)\n");
    printf("  --sea <path>                  Inject SEA blob to NODE_SEA/__NODE_SEA_BLOB\n");
    printf("                                If path ends in .json, automatically embeds smol config + VFS from 'smol' section\n");
    printf("  --skip-repack                 Skip SMOL stub auto-detection and repacking\n");
    printf("                                (SMOL stubs with __PRESSED_DATA are auto-detected unless this flag is used)\n");
    printf("  -h, --help                    Show this help message\n");
    printf("  -v, --version                 Show version information\n\n");
    printf("Notes:\n");
    printf("  VFS Configuration Priority:\n");
    printf("    1. CLI flags (--vfs, --vfs-in-memory, --vfs-on-disk, --vfs-compat)\n");
    printf("    2. sea-config.json smol.vfs section (if CLI flags not provided)\n");
    printf("  CLI flags always take precedence over sea-config.json settings.\n");
}

int main(int argc, char *argv[]) {
    DEBUG_INIT("binject");

    if (argc < 2) {
        print_usage(argv[0]);
        return BINJECT_ERROR_INVALID_ARGS;
    }

    const char *command = argv[1];

    if (strcmp(command, "--version") == 0 || strcmp(command, "-v") == 0) {
        printf("binject %s\n", VERSION);
        return BINJECT_OK;
    }

    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        print_usage(argv[0]);
        return BINJECT_OK;
    }

    if (strcmp(command, "inject") == 0) {
        const char *executable = NULL;
        const char *output = NULL;
        const char *sea_resource = NULL;
        const char *vfs_resource = NULL;
        int vfs_in_memory = 0;  // Default: extract VFS to disk at runtime
        int skip_repack = 0; // Default: repack compressed stubs

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--executable") == 0) {
                if (i + 1 < argc) executable = argv[++i];
            } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
                if (i + 1 < argc) output = argv[++i];
            } else if (strcmp(argv[i], "--vfs") == 0 || strcmp(argv[i], "--vfs-on-disk") == 0) {
                if (i + 1 < argc) vfs_resource = argv[++i];
            } else if (strcmp(argv[i], "--vfs-in-memory") == 0) {
                if (i + 1 < argc) vfs_resource = argv[++i];
                vfs_in_memory = 1;
            } else if (strcmp(argv[i], "--vfs-compat") == 0) {
                vfs_resource = "";  // Empty string marker for VFS compatibility mode
            } else if (strcmp(argv[i], "--sea") == 0) {
                if (i + 1 < argc) sea_resource = argv[++i];
            } else if (strcmp(argv[i], "--skip-repack") == 0) {
                skip_repack = 1;
            }
        }

        if (!executable || !output || (!sea_resource && !vfs_resource)) {
            fprintf(stderr, "Error: inject requires --executable, --output, and at least one of --sea <path> or --vfs <path>\n");
            return BINJECT_ERROR_INVALID_ARGS;
        }

        if (vfs_resource && !sea_resource) {
            fprintf(stderr, "Error: --vfs requires --sea to be specified\n");
            fprintf(stderr, "VFS (Virtual File System) must be injected alongside a SEA (Single Executable Application) blob\n");
            return BINJECT_ERROR_INVALID_ARGS;
        }

        // Check if SEA resource is a JSON config file
        // If so, generate the blob using node --experimental-sea-config and parse smol + VFS config
        char *generated_blob = NULL;
        uint8_t *smol_config_binary = NULL;
        uint8_t *vfs_config_binary = NULL;  // VFS config for embedding (108 bytes, SVFG format)
        char *temp_vfs_archive = NULL;  // Track temporary VFS archive (must be deleted)
        int cli_vfs_specified = (vfs_resource != NULL);  // Track if CLI specified VFS
        const char *vfs_mode_for_config = "on-disk";  // Track VFS mode for config generation

        if (sea_resource && is_json_file(sea_resource)) {
            // Parse sea-config.json to extract smol config and VFS config
            sea_config_t *config = parse_sea_config(sea_resource);
            if (config) {
                // Parse smol.update configuration from JSON to struct.
                smol_update_config_t smol_update_config;
                if (parse_smol_update_config(config->smol, &smol_update_config) == 0) {
                    // Preserve nodeVersion from original stub if not specified in config.
                    // This ensures the repacked stub has correct version for code cache matching.
                    if (!smol_update_config.node_version || strlen(smol_update_config.node_version) == 0) {
                        char *extracted_node_version = smol_extract_node_version_fast(executable);
                        if (extracted_node_version) {
                            printf("✓ Preserving nodeVersion from stub: %s\n", extracted_node_version);
                            free((void*)smol_update_config.node_version);
                            smol_update_config.node_version = extracted_node_version;
                        }
                    }
                    // Serialize smol config to binary (1192 bytes, SMFG v2).
                    smol_config_binary = serialize_smol_config(&smol_update_config);
                }
                smol_config_free(&smol_update_config);

                // Process VFS config (priority 2: only if CLI flags not provided)
                if (!cli_vfs_specified && config->vfs) {
                    printf("VFS: Using configuration from sea-config.json\n");

                    // Handle compat mode
                    if (strcmp(config->vfs->mode, "compat") == 0) {
                        printf("VFS: compat mode (API compatibility, no files embedded)\n");
                        vfs_resource = "";  // Empty string marker for compat mode
                        vfs_mode_for_config = "compat";
                    } else {
                        // Resolve source path (relative to sea-config.json directory)
                        char *resolved_source = resolve_relative_path(sea_resource, config->vfs->source);
                        if (!resolved_source) {
                            fprintf(stderr, "Error: Failed to resolve VFS source path\n");
                            free_sea_config(config);
                            if (smol_config_binary) free(smol_config_binary);
                            return BINJECT_ERROR;
                        }

                        // Detect source type
                        vfs_source_type_t source_type = detect_vfs_source_type(resolved_source);
                        if (source_type == VFS_SOURCE_NOT_FOUND) {
                            // Source doesn't exist - skip VFS gracefully.
                            printf("VFS: Source not found '%s', skipping VFS\n", resolved_source);
                            free(resolved_source);
                            resolved_source = NULL;
                            // Continue without VFS.
                        } else if (source_type == VFS_SOURCE_ERROR) {
                            fprintf(stderr, "Error: Invalid VFS source: %s\n", resolved_source);
                            free(resolved_source);
                            free_sea_config(config);
                            if (smol_config_binary) free(smol_config_binary);
                            return BINJECT_ERROR;
                        }

                        // Only process VFS if source was found and valid.
                        if (resolved_source != NULL) {
                            if (source_type == VFS_SOURCE_DIR) {
                                // Directory - create TAR.GZ with gzip level 9
                                printf("VFS: Creating archive from directory '%s' (gzip level 9)\n", resolved_source);
                                temp_vfs_archive = create_vfs_archive_from_dir(resolved_source);
                                if (!temp_vfs_archive) {
                                    fprintf(stderr, "Error: Failed to create VFS archive\n");
                                    free(resolved_source);
                                    free_sea_config(config);
                                    if (smol_config_binary) free(smol_config_binary);
                                    return BINJECT_ERROR;
                                }
                                vfs_resource = temp_vfs_archive;
                            } else if (source_type == VFS_SOURCE_TAR) {
                                // .tar file - compress with gzip level 9
                                printf("VFS: Compressing tar archive '%s' (gzip level 9)\n", resolved_source);
                                temp_vfs_archive = compress_tar_archive(resolved_source);
                                if (!temp_vfs_archive) {
                                    fprintf(stderr, "Error: Failed to compress VFS archive\n");
                                    free(resolved_source);
                                    free_sea_config(config);
                                    if (smol_config_binary) free(smol_config_binary);
                                    return BINJECT_ERROR;
                                }
                                vfs_resource = temp_vfs_archive;
                            } else {
                                // .tar.gz file - use as-is
                                printf("VFS: Using compressed archive '%s'\n", resolved_source);
                                // Note: Don't free resolved_source here - vfs_resource takes ownership
                                vfs_resource = resolved_source;
                                resolved_source = NULL;  // Prevent double-free
                            }

                            // Set mode flag
                            if (strcmp(config->vfs->mode, "in-memory") == 0) {
                                vfs_in_memory = 1;
                                vfs_mode_for_config = "in-memory";
                                printf("VFS: mode=in-memory (keep in RAM)\n");
                            } else {
                                // "on-disk" mode (default if not in-memory)
                                vfs_mode_for_config = "on-disk";
                                printf("VFS: mode=on-disk (extract to temp directory)\n");
                            }

                            // Only free resolved_source if it wasn't transferred to vfs_resource
                            if (resolved_source) {
                                free(resolved_source);
                            }
                        }
                    }
                } else if (cli_vfs_specified) {
                    printf("Note: CLI VFS flags override sea-config.json vfs section\n");
                }

                // Generate VFS config binary (108 bytes, SVFG format) if VFS is enabled
                if (vfs_resource) {
                    vfs_config_t runtime_vfs_config;
                    runtime_vfs_config.mode = vfs_mode_for_config;
                    runtime_vfs_config.prefix = "/snapshot";  // Default prefix

                    vfs_config_binary = serialize_vfs_config(&runtime_vfs_config);
                    if (!vfs_config_binary) {
                        fprintf(stderr, "Error: Failed to serialize VFS config\n");
                        free_sea_config(config);
                        if (smol_config_binary) free(smol_config_binary);
                        if (temp_vfs_archive) {
                            if (unlink(temp_vfs_archive) != 0 && errno != ENOENT) {
                                fprintf(stderr, "Warning: Failed to delete temporary file %s: %s\n",
                                        temp_vfs_archive, strerror(errno));
                            }
                            free(temp_vfs_archive);
                        }
                        return BINJECT_ERROR;
                    }
                }

                free_sea_config(config);
            }

            // Generate SEA blob
            generated_blob = binject_generate_sea_blob_from_config(sea_resource, executable);
            if (!generated_blob) {
                fprintf(stderr, "Error: Failed to generate SEA blob from config\n");
                if (smol_config_binary) free(smol_config_binary);
                if (vfs_config_binary) free(vfs_config_binary);
                if (temp_vfs_archive) {
                    if (unlink(temp_vfs_archive) != 0 && errno != ENOENT) {
                        fprintf(stderr, "Warning: Failed to delete temporary file %s: %s\n",
                                temp_vfs_archive, strerror(errno));
                    }
                    free(temp_vfs_archive);
                }
                return BINJECT_ERROR;
            }
            sea_resource = generated_blob;  // Use generated blob instead
        }

        // Generate VFS config if VFS is being injected but config wasn't generated yet
        // (happens when using --vfs command-line flag without sea-config.json)
        if (vfs_resource && !vfs_config_binary) {
            vfs_config_t runtime_vfs_config;
            runtime_vfs_config.mode = vfs_mode_for_config;
            runtime_vfs_config.prefix = "/snapshot";  // Default prefix

            vfs_config_binary = serialize_vfs_config(&runtime_vfs_config);
            if (!vfs_config_binary) {
                fprintf(stderr, "Error: Failed to serialize VFS config\n");
                if (generated_blob) free(generated_blob);
                if (smol_config_binary) free(smol_config_binary);
                if (temp_vfs_archive) {
                    if (unlink(temp_vfs_archive) != 0 && errno != ENOENT) {
                        fprintf(stderr, "Warning: Failed to delete temporary file %s: %s\n",
                                temp_vfs_archive, strerror(errno));
                    }
                    free(temp_vfs_archive);
                }
                return BINJECT_ERROR;
            }
        }

        int result = binject_batch(executable, output, sea_resource, vfs_resource, vfs_in_memory, skip_repack, vfs_config_binary);

        // Clean up generated resources
        if (generated_blob) {
            free(generated_blob);
        }
        if (smol_config_binary) {
            free(smol_config_binary);
        }
        if (vfs_config_binary) {
            free(vfs_config_binary);
        }
        if (temp_vfs_archive) {
            // Delete temporary archive file
            if (unlink(temp_vfs_archive) != 0 && errno != ENOENT) {
                fprintf(stderr, "Warning: Failed to delete temporary file %s: %s\n",
                        temp_vfs_archive, strerror(errno));
            }
            free(temp_vfs_archive);
        }

        return result;
    }

    if (strcmp(command, "list") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: list requires an executable path\n");
            return BINJECT_ERROR_INVALID_ARGS;
        }
        return binject_list(argv[2]);
    }

    if (strcmp(command, "extract") == 0) {
        const char *executable = NULL;
        const char *section = NULL;
        const char *output = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--executable") == 0) {
                if (i + 1 < argc) executable = argv[++i];
            } else if (strcmp(argv[i], "--vfs") == 0) {
                section = "vfs";
            } else if (strcmp(argv[i], "--sea") == 0) {
                section = "sea";
            } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
                if (i + 1 < argc) output = argv[++i];
            }
        }

        if (!executable || !section || !output) {
            fprintf(stderr, "Error: extract requires --executable, either --vfs or --sea, and --output\n");
            return BINJECT_ERROR_INVALID_ARGS;
        }

        return binject_extract(executable, section, output);
    }

    if (strcmp(command, "verify") == 0) {
        const char *executable = NULL;
        const char *section = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--executable") == 0) {
                if (i + 1 < argc) executable = argv[++i];
            } else if (strcmp(argv[i], "--vfs") == 0) {
                section = "vfs";
            } else if (strcmp(argv[i], "--sea") == 0) {
                section = "sea";
            }
        }

        if (!executable || !section) {
            fprintf(stderr, "Error: verify requires --executable and either --vfs or --sea\n");
            return BINJECT_ERROR_INVALID_ARGS;
        }

        return binject_verify(executable, section);
    }

    if (strcmp(command, "blob") == 0) {
        // Generate SEA blob from sea-config.json (does not inject into binary)
        // Usage: binject blob <sea-config.json>

        if (argc < 3) {
            fprintf(stderr, "Error: blob command requires a sea-config.json path\n");
            fprintf(stderr, "Usage: %s blob <sea-config.json>\n", argv[0]);
            return BINJECT_ERROR_INVALID_ARGS;
        }

        const char *config_path = argv[2];

        // Validate config file exists
        if (!is_json_file(config_path)) {
            fprintf(stderr, "Error: Config file must be a JSON file (*.json): %s\n", config_path);
            return BINJECT_ERROR_INVALID_ARGS;
        }

        // Use a dummy executable path (required by binject_generate_sea_blob_from_config for version extraction)
        // Since we don't have a target binary yet, use the host node
        char *node_binary = binject_find_system_node_binary();
        if (!node_binary) {
            fprintf(stderr, "Error: Node.js not found on system. Blob generation requires Node.js.\n");
            return BINJECT_ERROR;
        }

        // Generate the blob
        char *blob_path = binject_generate_sea_blob_from_config(config_path, node_binary);
        free(node_binary);

        if (!blob_path) {
            fprintf(stderr, "Error: Failed to generate SEA blob\n");
            return BINJECT_ERROR;
        }

        printf("✓ SEA blob generated: %s\n", blob_path);
        printf("  To inject into a binary: binject inject -e <binary> -o <output> --sea %s\n", blob_path);

        free(blob_path);
        return BINJECT_OK;
    }

    fprintf(stderr, "Error: unknown command '%s'\n", command);
    print_usage(argv[0]);
    return BINJECT_ERROR_INVALID_ARGS;
}

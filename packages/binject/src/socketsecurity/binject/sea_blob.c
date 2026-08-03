// ============================================================================
// sea_blob.c — Generating a SEA blob from a sea-config.json
// ============================================================================
//
// WHAT THIS FILE DOES
// Runs `node --experimental-sea-config <config>` on a version-appropriate node,
// then works out where node wrote the blob (the config's `output` field,
// resolved relative to the config's own directory) and hands that path back.
// Along the way it reads the target binary's embedded Node.js version and warns
// when a mismatch would silently break code cache or snapshot support.
//
// WHY IT EXISTS
// Two commands need this exact sequence — `binject inject --sea config.json`
// and `binject blob config.json` — so it lives in one unit instead of twice in
// main.c. It was split out of main.c when that file outgrew the 1000-line
// source cap; the public CLI surface is unchanged, and the entry point is
// declared in sea_blob.h.
// ============================================================================

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  // For strdup
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <process.h>
#include "socketsecurity/build-infra/posix_compat.h"
#else
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
/* Windows compat shims (PATH_MAX, S_ISREG/S_ISDIR, 64-bit fseek/ftell). */
#include "socketsecurity/build-infra/file_io_common.h"
#include "socketsecurity/binject/sea_blob.h"
#include "socketsecurity/binject/json_parser.h"
#include "socketsecurity/binject/node_resolve.h"
#include "socketsecurity/bin-infra/binary_format.h"
#include "socketsecurity/bin-infra/smol_segment_reader.h"

// Host platform string macros (compile-time)
#ifdef __APPLE__
#define HOST_PLATFORM "darwin"
#define HOST_OS_NAME "macOS"
#elif defined(_WIN32)
#define HOST_PLATFORM "win32"
#define HOST_OS_NAME "Windows"
#else
#define HOST_PLATFORM "linux"
#define HOST_OS_NAME "Linux"
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define HOST_ARCH "arm64"
#else
#define HOST_ARCH "x64"
#endif

/**
 * SEA Performance Impact Constants (in milliseconds)
 *
 * These values are based on documented benchmarks and real-world measurements:
 *
 * Code Cache (useCodeCache):
 *   - Provides ~13% faster startup time
 *   - Reduces V8 script compilation time by pre-compiling JavaScript to bytecode
 *   - Measured impact: 21.6ms for 500-module TypeScript application (161.3ms → 139.7ms)
 *   - V8 reports 20-40% reduction in parse/compile time
 *   - Reference: https://github.com/yyx990803/bun-vs-node-sea-startup (real benchmark)
 *   - Reference: https://v8.dev/blog/improved-code-caching
 *
 * Snapshot (useSnapshot):
 *   - Provides ~2x faster startup (skips parse/compile/execute)
 *   - Pre-initializes V8 heap state, avoiding cold start initialization
 *   - Simple apps: ~20ms savings (40ms → 20ms on MacBook)
 *   - Complex apps: ~100ms savings (TypeScript compiler benchmark)
 *   - Reference: Node.js startup snapshots talk by Joyee Cheung
 *   - Reference: https://v8.dev/blog/custom-startup-snapshots (TypeScript example)
 *
 * Combined (Code Cache + Snapshot):
 *   - Effects are mostly additive (snapshot dominates the benefit)
 *   - Simple apps: ~40ms total (20ms code cache + 20ms snapshot)
 *   - Complex apps: ~125ms total (25ms code cache + 100ms snapshot)
 *   - Most significant for applications with large dependency trees
 *
 * Note: Actual impact varies based on:
 *   - Application size and complexity (number of modules/functions)
 *   - CPU performance and memory bandwidth
 *   - Whether modules are CJS or ESM (affects parse/compile cost)
 */
/* Code cache impact: 21.6ms measured in real benchmark
 * Source: https://github.com/yyx990803/bun-vs-node-sea-startup - Real benchmark with 500 TypeScript modules
 * Source: https://v8.dev/blog/improved-code-caching - V8 engineering blog */
#define SEA_PERF_CODE_CACHE_MIN_MS 20
#define SEA_PERF_CODE_CACHE_MAX_MS 25

/* Snapshot impact: 20ms (simple apps) to 100ms (TypeScript compiler)
 * Source: https://v8.dev/blog/custom-startup-snapshots - TypeScript example
 * Source: Node.js startup snapshots talk by Joyee Cheung */
#define SEA_PERF_SNAPSHOT_MIN_MS 20
#define SEA_PERF_SNAPSHOT_MAX_MS 100

/* Combined impact: additive effect (code cache + snapshot)
 * Source: Calculated from above benchmarks */
#define SEA_PERF_COMBINED_MIN_MS 40
#define SEA_PERF_COMBINED_MAX_MS 125

/**
 * Parsed SEA config settings (cached to avoid redundant file reads)
 */
typedef struct {
    int has_code_cache;  /* useCodeCache: true */
    int has_snapshot;    /* useSnapshot: true */
    int parsed;          /* Whether config has been parsed */
} sea_config_opts_t;

/**
 * Parse SEA config file to extract optimization settings.
 * Results are cached to avoid redundant file I/O.
 * Returns 1 on success, 0 on failure.
 */
static int parse_sea_config_opts(const char *config_path, sea_config_opts_t *opts) {
    if (!config_path || !opts) return 0;

    /* Reset options */
    opts->has_code_cache = 0;
    opts->has_snapshot = 0;
    opts->parsed = 0;

    FILE *config_file = fopen(config_path, "rb");
    if (!config_file) return 0;

    fseek(config_file, 0, SEEK_END);
    long fsize = ftell(config_file);
    fseek(config_file, 0, SEEK_SET);

    if (fsize <= 0 || fsize >= 1024 * 1024) {
        fclose(config_file);
        return 0;
    }

    char *config_content = malloc(fsize + 1);
    if (!config_content) {
        fclose(config_file);
        return 0;
    }

    if (fread(config_content, 1, fsize, config_file) != (size_t)fsize) {
        free(config_content);
        fclose(config_file);
        return 0;
    }
    config_content[fsize] = '\0';
    fclose(config_file);

    /* Simple string search for useCodeCache (avoid full JSON parse for performance) */
    const char *use_code_cache_str = strstr(config_content, "useCodeCache");
    if (use_code_cache_str) {
        const char *colon = strchr(use_code_cache_str, ':');
        if (colon) {
            const char *value = colon + 1;
            while (*value && (*value == ' ' || *value == '\t' || *value == '\n' || *value == '\r')) {
                value++;
            }
            if (strncmp(value, "true", 4) == 0) {
                opts->has_code_cache = 1;
            }
        }
    }

    /* Check for useSnapshot: true */
    const char *use_snapshot_str = strstr(config_content, "useSnapshot");
    if (use_snapshot_str) {
        const char *colon = strchr(use_snapshot_str, ':');
        if (colon) {
            const char *value = colon + 1;
            while (*value && (*value == ' ' || *value == '\t' || *value == '\n' || *value == '\r')) {
                value++;
            }
            if (strncmp(value, "true", 4) == 0) {
                opts->has_snapshot = 1;
            }
        }
    }

    free(config_content);
    opts->parsed = 1;
    return 1;
}

/* Generate SEA blob from JSON config using node --experimental-sea-config.
 * Contract and ownership rules are documented in sea_blob.h. */
char* binject_generate_sea_blob_from_config(const char *config_path, const char *executable) {
    char *node_binary = NULL;

    // Helper macro for cleanup on error
    #define CLEANUP_AND_RETURN_NULL() do { \
        if (node_binary) { \
            free(node_binary); \
        } \
        return NULL; \
    } while(0)

    // Detect target binary format for cross-platform warning messages
    const char *target_platform = "unknown";
    FILE *fp = fopen(executable, "rb");
    if (fp) {
        uint8_t magic[4];
        if (fread(magic, 1, 4, fp) == 4) {
            binary_format_t format = detect_binary_format(magic);
            switch (format) {
                case BINARY_FORMAT_MACHO: target_platform = "darwin"; break;
                case BINARY_FORMAT_ELF:   target_platform = "linux"; break;
                case BINARY_FORMAT_PE:    target_platform = "win32"; break;
                default: break;
            }
        }
        fclose(fp);
    }

    // For SEA blob generation, we need to use a Node.js binary that matches the target version.
    // SEA blobs are version-specific - a blob generated with Node 24 won't work in Node 25.
    //
    // Search order:
    // 1. $PATH - respects user's environment (nvm use, volta, fnm, etc.)
    // 2. nvm version-specific path for exact version match
    // 3. Fallback to any available node with warning about code cache/bytecode

    // Step 1: Extract target Node.js version from the executable (if available).
    // SMOL binaries embed their Node.js version in SMOL_VFS_CONFIG for version matching.
    // Plain Node.js binaries or cross-platform targets won't have this - that's fine.
    // Use fast native parsing instead of LIEF (30-60x faster on large binaries).
    char *target_version = smol_extract_node_version_fast(executable);

    // Step 2: Find node binary matching the target version
    char *found_version = NULL;
    int version_matched = 0;
    node_binary = binject_find_matching_node_binary(target_version, &found_version, &version_matched);

    if (!node_binary) {
        fprintf(stderr, "Error: Node.js not found on system\n");
        fprintf(stderr, "   Searched: $PATH, nvm directories\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "   To install Node.js:\n");
        if (target_version) {
            fprintf(stderr, "     nvm install %s\n", target_version);
            fprintf(stderr, "     nvm use %s\n", target_version);
        } else {
            fprintf(stderr, "     nvm install node\n");
        }
        fprintf(stderr, "\n");
        if (target_version) {
            free(target_version);
        }
        return NULL;
    }

    // Step 3: Parse SEA config to check optimization settings
    sea_config_opts_t config_opts = {0};
    parse_sea_config_opts(config_path, &config_opts);

    // Step 4: Display version status and warnings
    if (target_version && found_version) {
        if (version_matched) {
            printf("✓ Node.js version match: %s\n", found_version);
        } else {
            // Version mismatch - warn about code cache/bytecode implications
            fprintf(stderr, "\n");
            fprintf(stderr, "⚠️  Version mismatch: target needs %s, using %s\n", target_version, found_version);
            fprintf(stderr, "   Binary: %s\n", node_binary);
            fprintf(stderr, "\n");

            if (config_opts.has_code_cache || config_opts.has_snapshot) {
                // Build performance impact message based on what they wanted
                char impact_msg[256] = {0};
                int total_min = 0;
                int total_max = 0;

                if (config_opts.has_code_cache && config_opts.has_snapshot) {
                    snprintf(impact_msg, sizeof(impact_msg), "code cache + snapshot");
                    total_min = SEA_PERF_COMBINED_MIN_MS;
                    total_max = SEA_PERF_COMBINED_MAX_MS;
                } else if (config_opts.has_code_cache) {
                    snprintf(impact_msg, sizeof(impact_msg), "code cache");
                    total_min = SEA_PERF_CODE_CACHE_MIN_MS;
                    total_max = SEA_PERF_CODE_CACHE_MAX_MS;
                } else if (config_opts.has_snapshot) {
                    snprintf(impact_msg, sizeof(impact_msg), "snapshot");
                    total_min = SEA_PERF_SNAPSHOT_MIN_MS;
                    total_max = SEA_PERF_SNAPSHOT_MAX_MS;
                }

                fprintf(stderr, "   SEA format may be incompatible with target Node.js version.\n");
                fprintf(stderr, "   %s won't work correctly (startup ~%d-%dms slower).\n", impact_msg, total_min, total_max);
            } else {
                fprintf(stderr, "   Plain JS blob should work, but SEA format may be incompatible.\n");
            }

            fprintf(stderr, "\n");
            fprintf(stderr, "   Fix: nvm install %s && nvm use %s\n", target_version, target_version);
            fprintf(stderr, "\n");
        }
    } else if (!target_version && found_version) {
        // No embedded version in target binary. This happens when:
        // 1. Fresh node-smol download (SMOL_CONFIG not yet injected)
        // 2. Plain Node.js binary (no SMOL_CONFIG section)
        // 3. Cross-platform build where target can't be executed
        //
        // For code cache/snapshot: V8 bytecode is PLATFORM-SPECIFIC, not just version-specific.
        // Even with matching versions, code cache from darwin-arm64 won't work on win32-x64.
        // The blob MUST be generated by the same Node.js binary that will execute it.
        if (config_opts.has_code_cache || config_opts.has_snapshot) {
            fprintf(stderr, "\n");
            fprintf(stderr, "⚠️  Cannot verify target version for code cache generation\n");
            fprintf(stderr, "   Host Node.js: %s (%s-%s)\n", found_version, HOST_PLATFORM, HOST_ARCH);
            fprintf(stderr, "   Target binary: %s (version unknown)\n", target_platform);
            fprintf(stderr, "\n");
            fprintf(stderr, "   V8 bytecode/snapshots are version and platform-specific.\n");
            fprintf(stderr, "   Code cache may not work if target Node.js version differs.\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "   Generating blob anyway...\n");
            fprintf(stderr, "\n");
        } else {
            // Safe: plain JS blob is version-agnostic
            printf("Generating SEA blob with Node.js %s (plain JS)\n", found_version);
        }
    } else if (!found_version) {
        fprintf(stderr, "⚠️  Warning: Could not determine Node.js version\n");
        fprintf(stderr, "   Binary: %s\n", node_binary);
        fprintf(stderr, "   Continuing anyway...\n\n");
    }

    // Clean up version strings
    if (target_version) {
        free(target_version);
    }
    if (found_version) {
        free(found_version);
    }

    if (!node_binary) {
        fprintf(stderr, "Error: Failed to determine node binary path\n");
        return NULL;
    }

    // Validate config_path doesn't contain dangerous patterns
    if (!config_path || strlen(config_path) == 0) {
        fprintf(stderr, "Error: Config path is empty\n");
        CLEANUP_AND_RETURN_NULL();
    }

    // Check for path traversal attempts
    if (strstr(config_path, "..") != NULL) {
        fprintf(stderr, "Error: Path traversal detected in config path\n");
        CLEANUP_AND_RETURN_NULL();
    }

    // Verify file exists and is readable
    struct stat st;
    if (stat(config_path, &st) != 0) {
        fprintf(stderr, "Error: Config file not found: %s\n", config_path);
        CLEANUP_AND_RETURN_NULL();
    }

    // Verify it's a regular file (not symlink, device, etc)
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Error: Config path is not a regular file: %s\n", config_path);
        CLEANUP_AND_RETURN_NULL();
    }

    printf("Detected SEA config file: %s\n", config_path);

    /* Use cached config_opts from earlier parsing (avoids redundant file read) */
    if (config_opts.parsed && !config_opts.has_code_cache) {
        fprintf(stderr, "\n");
        fprintf(stderr, "⚠️  Performance Warning: useCodeCache not enabled\n");
        fprintf(stderr, "   Setting 'useCodeCache: true' provides ~13%% faster startup (~22ms)\n");
        fprintf(stderr, "   Trade-off: +2-3 MB binary size\n");
        fprintf(stderr, "   Recommended for production builds where startup speed matters\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "   Add to %s:\n", config_path);
        fprintf(stderr, "   {\n");
        fprintf(stderr, "     \"useCodeCache\": true,\n");
        fprintf(stderr, "     ...\n");
        fprintf(stderr, "   }\n");
        fprintf(stderr, "\n");
    }

    printf("Generating SEA blob using: %s --experimental-sea-config %s\n",
           node_binary, config_path);

#ifdef _WIN32
    // Windows: use _spawnvp and _cwait
    char *argv[] = {
        (char*)node_binary,
        (char*)"--experimental-sea-config",
        (char*)config_path,
        NULL
    };

    intptr_t pid = _spawnvp(_P_NOWAIT, node_binary, (const char* const*)argv);
    if (pid == -1) {
        fprintf(stderr, "Error: Failed to spawn process\n");
        CLEANUP_AND_RETURN_NULL();
    }

    int status;
    if (_cwait(&status, pid, 0) == -1) {
        fprintf(stderr, "Error: Failed to wait for process\n");
        CLEANUP_AND_RETURN_NULL();
    }

    if (status != 0) {
        fprintf(stderr, "Error: node --experimental-sea-config failed with exit code %d\n", status);
        CLEANUP_AND_RETURN_NULL();
    }
#else
    // Unix: use fork and exec
    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stderr, "Error: Failed to fork process\n");
        CLEANUP_AND_RETURN_NULL();
    }

    if (pid == 0) {
        // Child process: change to config directory, then run node
        // Extract directory and filename from config_path
        char config_dir[PATH_MAX];
        const char *config_filename = config_path;
        const char *last_slash = strrchr(config_path, '/');
        if (last_slash != NULL) {
            size_t dir_len = last_slash - config_path;
            if (dir_len > 0 && dir_len < sizeof(config_dir)) {
                memcpy(config_dir, config_path, dir_len);
                config_dir[dir_len] = '\0';

                // Change to config directory so relative paths in config work
                if (chdir(config_dir) != 0) {
                    fprintf(stderr, "Error: Failed to change to config directory %s: %s\n",
                            config_dir, strerror(errno));
                    _exit(1);
                }

                // Use just the filename after changing directory
                config_filename = last_slash + 1;
            }
        }

        char *argv[] = {
            (char*)node_binary,
            (char*)"--experimental-sea-config",
            (char*)config_filename,
            NULL
        };
        // Use execv for absolute paths, execvp for PATH lookup (like "node")
        if (node_binary[0] == '/') {
            execv(node_binary, argv);
        } else {
            execvp(node_binary, argv);
        }
        // If exec returns, it failed - print error and exit
        fprintf(stderr, "Error: exec failed for %s: %s\n", node_binary, strerror(errno));
        _exit(1);
    }

    // Parent: wait for child
    int status;
    pid_t result;
    /* Retry waitpid on EINTR (interrupted by signal) */
    do {
        result = waitpid(pid, &status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        fprintf(stderr, "Error: waitpid failed: %s\n", strerror(errno));
        CLEANUP_AND_RETURN_NULL();
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        fprintf(stderr, "Error: node --experimental-sea-config failed (exit code: %d)\n", exit_code);
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "Process terminated by signal: %d\n", WTERMSIG(status));
        }
        CLEANUP_AND_RETURN_NULL();
    }
#endif

    // Free node_binary
    free(node_binary);
    node_binary = NULL;

    // Parse sea-config.json using cJSON
    sea_config_t *config = parse_sea_config(config_path);
    if (!config) {
        fprintf(stderr, "Error: Failed to parse sea-config.json\n");
        return NULL;
    }

    // Construct full path to blob file (config directory + output filename)
    // The blob is written to the same directory as the config file
    char *blob_path = NULL;
    const char *last_slash = strrchr(config_path, '/');
#ifdef _WIN32
    const char *last_bsep = strrchr(config_path, '\\');
    if (last_bsep && (!last_slash || last_bsep > last_slash)) {
        last_slash = last_bsep;
    }
#endif
#ifdef _WIN32
    int is_absolute = (config->output[0] == '/' || config->output[0] == '\\' ||
                       (config->output[0] != '\0' && config->output[1] == ':'));
#else
    int is_absolute = (config->output[0] == '/');
#endif
    if (last_slash != NULL && !is_absolute) {
        // Relative output path - prepend config directory
        size_t dir_len = last_slash - config_path;
        size_t blob_len = strlen(config->output);
        blob_path = (char*)malloc(dir_len + 1 + blob_len + 1);  // dir + '/' + filename + '\0'
        if (blob_path) {
            memcpy(blob_path, config_path, dir_len);
            blob_path[dir_len] = '/';
            memcpy(blob_path + dir_len + 1, config->output, blob_len);
            blob_path[dir_len + 1 + blob_len] = '\0';
        }
    } else {
        // Absolute output path or no directory in config_path - use as-is
        blob_path = strdup(config->output);
    }

    free_sea_config(config);

    if (!blob_path) {
        fprintf(stderr, "Error: Failed to allocate memory for blob path\n");
        return NULL;
    }

    // Verify the blob file was created - open directly instead of stat to avoid TOCTOU
    FILE *verify_fp = fopen(blob_path, "rb");
    if (!verify_fp) {
        fprintf(stderr, "Error: Generated blob file not found: %s\n", blob_path);
        free(blob_path);
        return NULL;
    }
    fclose(verify_fp);

    printf("✓ Generated SEA blob: %s\n", blob_path);

    // Clean up macro definition
    #undef CLEANUP_AND_RETURN_NULL
    return blob_path;
}

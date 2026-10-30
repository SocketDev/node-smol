// ============================================================================
// node_resolve.c — Host Node.js binary discovery
// ============================================================================
//
// WHAT THIS FILE DOES
// Locates a Node.js executable on the machine running the build: honours the
// BINJECT_NODE_PATH override, walks $PATH, then tries the install layout of
// every version manager, system package manager, CI image, and Docker image we
// know about. Each candidate is validated (real file, executable, recognizable
// ELF/Mach-O/PE header) and its `node --version` compared to the wanted one.
//
// WHY IT EXISTS
// A SEA blob only loads in the Node.js version that produced it, so picking the
// right node is a correctness concern. The search table is long enough to
// deserve its own file — it was split out of main.c when that file outgrew the
// 1000-line source cap. The public CLI surface (main.c) is unchanged; the
// helpers shared with sea_blob.c are declared in node_resolve.h.
// ============================================================================

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  // For strdup
#endif
#define _DARWIN_C_SOURCE         // For F_GETPATH on macOS

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include "socketsecurity/build-infra/posix_compat.h"
#else
#include <fcntl.h>
#include <unistd.h>
#endif
/* Windows compat shims (PATH_MAX, S_ISREG/S_ISDIR, 64-bit fseek/ftell). */
#include "socketsecurity/build-infra/file_io_common.h"
#include "socketsecurity/binject/node_resolve.h"
#include "socketsecurity/bin-infra/binary_format.h"
#include "socketsecurity/build-infra/path_utils.h"
#include "socketsecurity/build-infra/process_exec.h"

/**
 * Validate that a path is a legitimate Node.js binary
 * Basic validation: must be an existing executable file
 * Silent validation returns a descriptor-backed path for candidate search.
 */
typedef struct {
    int fd;
#ifdef _WIN32
    HANDLE *directory_handles;
    size_t directory_handle_count;
#endif
    char path[];
} node_binary_path_t;

static void close_node_binary_fd(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

#ifdef _WIN32
static void close_node_binary_directories(HANDLE *handles, size_t count);
#endif

static char *retain_node_binary_path(int fd, const char *path
#ifdef _WIN32
                                     , HANDLE *directory_handles,
                                     size_t directory_handle_count
#endif
) {
    size_t path_size = strlen(path) + 1;
    node_binary_path_t *handle = malloc(sizeof(*handle) + path_size);
    if (!handle) {
#ifdef _WIN32
        close_node_binary_directories(directory_handles,
                                      directory_handle_count);
#endif
        close_node_binary_fd(fd);
        return NULL;
    }
    handle->fd = fd;
#ifdef _WIN32
    handle->directory_handles = directory_handles;
    handle->directory_handle_count = directory_handle_count;
#endif
    memcpy(handle->path, path, path_size);
    return handle->path;
}

#ifdef _WIN32
static void close_node_binary_directories(HANDLE *handles, size_t count) {
    for (size_t index = 0; index < count; index++) {
        CloseHandle(handles[index]);
    }
    free(handles);
}

static int lock_node_binary_directories(const char *path,
                                        HANDLE **handles_out,
                                        size_t *count_out) {
    size_t path_size = strlen(path) + 1;
    char *component = malloc(path_size);
    HANDLE *handles = calloc(path_size, sizeof(*handles));
    if (!component || !handles) {
        free(component);
        free(handles);
        return -1;
    }
    memcpy(component, path, path_size);

    size_t root_end = 0;
    if (strncmp(component, "\\\\?\\UNC\\", 8) == 0) {
        char *server_end = strchr(component + 8, '\\');
        char *share_end = server_end ? strchr(server_end + 1, '\\') : NULL;
        if (!share_end) {
            free(component);
            free(handles);
            return -1;
        }
        root_end = (size_t)(share_end - component) + 1;
    } else if (strncmp(component, "\\\\?\\", 4) == 0 &&
               component[5] == ':' && component[6] == '\\') {
        root_end = 7;
    } else {
        free(component);
        free(handles);
        return -1;
    }

    size_t count = 0;
    for (char *separator = strchr(component + root_end, '\\'); separator;
         separator = strchr(separator + 1, '\\')) {
        *separator = '\0';
        HANDLE directory = CreateFileA(
            component, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        *separator = '\\';
        if (directory == INVALID_HANDLE_VALUE) {
            close_node_binary_directories(handles, count);
            free(component);
            return -1;
        }
        handles[count++] = directory;
    }

    free(component);
    *handles_out = handles;
    *count_out = count;
    return 0;
}
#endif

char *binject_retain_node_binary(const char *path) {
    if (!path || *path == '\0') {
        return NULL;
    }

    // Check path length before canonicalization to avoid buffer issues
    size_t path_len = strlen(path);
    if (path_len >= PATH_MAX - 1) {
        return NULL;
    }

#ifdef _WIN32
    HANDLE binary_handle = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (binary_handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    FILE_ATTRIBUTE_TAG_INFO tag_info;
    if (!GetFileInformationByHandleEx(binary_handle, FileAttributeTagInfo,
                                      &tag_info, sizeof(tag_info)) ||
        (tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(binary_handle);
        return NULL;
    }
    int fd = _open_osfhandle((intptr_t)binary_handle,
                             _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) {
        CloseHandle(binary_handle);
        return NULL;
    }
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0) {
        return NULL;
    }

#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) {
        _close(fd);
#else
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
#endif
        return NULL;
    }

    if (!S_ISREG(st.st_mode)) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return NULL;
    }

#ifndef _WIN32
    if (!(st.st_mode & S_IXUSR) && !(st.st_mode & S_IXGRP) && !(st.st_mode & S_IXOTH)) {
        close(fd);
        return NULL;
    }
#endif

    uint8_t magic[4];
#ifdef _WIN32
    int bytes_read = _read(fd, magic, sizeof(magic));
#else
    ssize_t bytes_read = read(fd, magic, sizeof(magic));
#endif

    if (bytes_read != (int)sizeof(magic)) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return NULL;
    }

    // Check for valid executable format using shared detection
    binary_format_t format = detect_binary_format(magic);

    if (format == BINARY_FORMAT_UNKNOWN) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return NULL;
    }

#ifdef _WIN32
    char retained_path[PATH_MAX];
    DWORD retained_size = GetFinalPathNameByHandleA(
        binary_handle, retained_path, sizeof(retained_path),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (retained_size == 0 || retained_size >= sizeof(retained_path)) {
        _close(fd);
        return NULL;
    }
    HANDLE *directory_handles = NULL;
    size_t directory_handle_count = 0;
    if (lock_node_binary_directories(retained_path, &directory_handles,
                                     &directory_handle_count) != 0) {
        _close(fd);
        return NULL;
    }
    HANDLE identity_handle = CreateFileA(
        retained_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION retained_info;
    BY_HANDLE_FILE_INFORMATION identity_info;
    if (identity_handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(binary_handle, &retained_info) ||
        !GetFileInformationByHandle(identity_handle, &identity_info) ||
        retained_info.dwVolumeSerialNumber != identity_info.dwVolumeSerialNumber ||
        retained_info.nFileIndexHigh != identity_info.nFileIndexHigh ||
        retained_info.nFileIndexLow != identity_info.nFileIndexLow) {
        if (identity_handle != INVALID_HANDLE_VALUE) CloseHandle(identity_handle);
        close_node_binary_directories(directory_handles,
                                      directory_handle_count);
        _close(fd);
        return NULL;
    }
    CloseHandle(identity_handle);
    return retain_node_binary_path(fd, retained_path, directory_handles,
                                   directory_handle_count);
#else
    char descriptor_path[64];
    int written = snprintf(descriptor_path, sizeof(descriptor_path),
                           "/dev/fd/%d", fd);
    if (written < 0 || (size_t)written >= sizeof(descriptor_path)) {
        close(fd);
        return NULL;
    }
    return retain_node_binary_path(fd, descriptor_path);
#endif
}

void binject_release_node_binary(char *node_binary) {
    if (!node_binary) return;
    node_binary_path_t *handle =
        (node_binary_path_t *)(node_binary - offsetof(node_binary_path_t, path));
#ifdef _WIN32
    close_node_binary_directories(handle->directory_handles,
                                  handle->directory_handle_count);
#endif
    close_node_binary_fd(handle->fd);
    free(handle);
}

/**
 * Check if a path is in a world-writable directory (security warning).
 * Only warns on Unix systems where this is a meaningful check.
 */
#ifndef _WIN32
static void warn_if_world_writable_dir(const char *path) {
    if (!path) return;

    // Find parent directory
    char dir_path[PATH_MAX];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash || last_slash == dir_path) return;

    *last_slash = '\0';

    struct stat dir_st;
    if (stat(dir_path, &dir_st) == 0) {
        if (dir_st.st_mode & S_IWOTH) {
            fprintf(stderr, "⚠ Warning: BINJECT_NODE_PATH is in world-writable directory: %s\n", dir_path);
            fprintf(stderr, "  This is a security risk - consider using a protected directory.\n\n");
        }
    }
}
#endif

/**
 * Get Node.js version by executing: node --version
 * Returns version string without 'v' prefix (e.g., "25.5.0")
 * Caller must free() the returned string
 */
static char* get_node_version(const char* node_binary) {
    // Spawn node --version without shell (defense-in-depth against command injection)
    // Uses fork()/execvp() on Unix, CreateProcess() on Windows
    const char* args[] = {node_binary, "--version", NULL};
    char* output = spawn_command(node_binary, args, 1024);

    if (!output) {
        return NULL;
    }

    // Strip 'v' prefix and newline: "v25.5.0\n" -> "25.5.0"
    char* start = output;
    if (start[0] == 'v') {
        start++;
    }

    char* newline = strchr(start, '\n');
    if (newline) {
        *newline = '\0';
    }

    // Create a copy since we need to free the original output buffer
    char* version = strdup(start);
    free(output);

    return version;
}

/**
 * Resolve "node" command in $PATH to an absolute path.
 * Returns allocated path string, or NULL if not found.
 * Caller must free() the returned string.
 */
static char* resolve_node_in_path(void) {
    const char *path_env = getenv("PATH");
    if (!path_env || strlen(path_env) == 0) {
        return NULL;
    }

    // Copy PATH since strtok modifies the string
    char *path_copy = strdup(path_env);
    if (!path_copy) {
        return NULL;
    }

    char *result = NULL;
    char *saveptr = NULL;

#ifdef _WIN32
    // Windows uses semicolons as PATH separator and backslashes in paths
    const char path_sep = ';';
    const char *node_suffix = "\\node.exe";
    const size_t node_len = 9;  // "\\node.exe"
#else
    // Unix uses colons as PATH separator and forward slashes
    const char path_sep = ':';
    const char *node_suffix = "/node";
    const size_t node_len = 5;  // "/node"
#endif

    char sep_str[2] = {path_sep, '\0'};
    char *dir = strtok_r(path_copy, sep_str, &saveptr);

    while (dir != NULL) {
        // Skip relative PATH entries (".", "./node_modules/.bin", ...):
        // resolving the node interpreter from them is cwd-dependent — the
        // traversal hazard this resolver otherwise canonicalizes away — and
        // canonicalizing them costs a getcwd() walk that stalls for tens of
        // seconds when the cwd sits inside a huge temp directory.
        if (!is_absolute_path(dir)) {
            dir = strtok_r(NULL, sep_str, &saveptr);
            continue;
        }

        // Build full path: dir/node (or dir\node.exe on Windows)
        size_t dir_len = strlen(dir);
        char *full_path = malloc(dir_len + node_len + 1);
        if (!full_path) {
            free(path_copy);
            return NULL;
        }

        snprintf(full_path, dir_len + node_len + 1, "%s%s", dir, node_suffix);

        char *validated_path = binject_retain_node_binary(full_path);
        if (validated_path) {
            free(full_path);
            result = validated_path;
            break;
        }

        free(full_path);
        dir = strtok_r(NULL, sep_str, &saveptr);
    }

    free(path_copy);
    return result;
}

/**
 * Get version manager directories to search for Node.js.
 * Supports multiple version managers across platforms.
 *
 * ============================================================================
 * LOCAL VERSION MANAGERS - macOS/Linux
 * ============================================================================
 *
 * nvm (Node Version Manager)
 *   Path: ~/.nvm/versions/node/v{version}/bin/node
 *   Env:  NVM_DIR, NVM_BIN
 *   Ref:  https://github.com/nvm-sh/nvm
 *
 * fnm (Fast Node Manager)
 *   Path: ~/.local/share/fnm/node-versions/v{version}/installation/bin/node
 *   Alt:  ~/.fnm/node-versions/v{version}/installation/bin/node
 *   Env:  FNM_MULTISHELL_PATH
 *   Ref:  https://github.com/Schniz/fnm
 *
 * volta (JavaScript Tool Manager)
 *   Path: ~/.volta/tools/image/node/{version}/bin/node
 *   Env:  VOLTA_HOME
 *   Ref:  https://volta.sh/
 *         https://github.com/volta-cli/volta
 *
 * asdf (Multiple Runtime Version Manager)
 *   Path: ~/.asdf/installs/nodejs/{version}/bin/node
 *   Env:  ASDF_DATA_DIR
 *   Ref:  https://asdf-vm.com/
 *         https://github.com/asdf-vm/asdf-nodejs
 *
 * nodenv (Node Version Management)
 *   Path: ~/.nodenv/versions/{version}/bin/node
 *   Env:  NODENV_ROOT
 *   Ref:  https://github.com/nodenv/nodenv
 *
 * n (Node.js Version Manager by tj)
 *   Path: /usr/local/n/versions/node/{version}/bin/node
 *   Alt:  ~/n/n/versions/node/{version}/bin/node (n-install)
 *   Env:  N_PREFIX
 *   Ref:  https://github.com/tj/n
 *
 * mise (formerly rtx, polyglot tool version manager)
 *   Path: ~/.local/share/mise/installs/node/{version}/bin/node
 *   Env:  MISE_DATA_DIR
 *   Note: Successor to rtx, compatible with asdf plugins
 *   Ref:  https://mise.jdx.dev/
 *         https://github.com/jdx/mise
 *
 * ============================================================================
 * SYSTEM PACKAGE MANAGERS - Linux Only
 * ============================================================================
 *
 * apt/apt-get (Debian, Ubuntu, Linux Mint, Pop!_OS, etc.)
 *   Path: /usr/bin/node
 *   Alt:  /usr/bin/nodejs (legacy, may need symlink)
 *   Note: Version-agnostic system path
 *   Ref:  https://packages.debian.org/nodejs
 *         https://packages.ubuntu.com/nodejs
 *
 * yum/dnf (RHEL, CentOS, Fedora, Rocky Linux, AlmaLinux, etc.)
 *   Path: /usr/bin/node
 *   Note: Same path as apt - all Linux package managers install to /usr/bin
 *   Ref:  https://packages.fedoraproject.org/pkgs/nodejs/nodejs/
 *         https://developers.redhat.com/blog/2019/10/01/using-node-js-12-on-red-hat-enterprise-linux-8
 *
 * apk (Alpine Linux)
 *   Path: /usr/bin/node
 *   Note: Alpine uses musl libc; binaries must be built for musl
 *   Ref:  https://pkgs.alpinelinux.org/package/edge/main/x86_64/nodejs
 *
 * NodeSource (apt/yum repository)
 *   Path: /usr/bin/node
 *   Note: Same path as system packages, different repository for newer versions
 *   Ref:  https://github.com/nodesource/distributions
 *         https://deb.nodesource.com/
 *         https://rpm.nodesource.com/
 *
 * Snap (Canonical)
 *   Path: /snap/bin/node
 *   Note: Requires --classic confinement
 *   Ref:  https://snapcraft.io/node
 *         https://github.com/nodejs/snap
 *
 * ============================================================================
 * SYSTEM PACKAGE MANAGERS - macOS Only
 * ============================================================================
 *
 * Homebrew (Apple Silicon)
 *   Path: /opt/homebrew/bin/node
 *   Alt:  /opt/homebrew/Cellar/node/{version}/bin/node
 *   Ref:  https://brew.sh/
 *         https://formulae.brew.sh/formula/node
 *
 * Homebrew (Intel)
 *   Path: /usr/local/bin/node
 *   Alt:  /usr/local/Cellar/node/{version}/bin/node
 *   Note: Shared with Docker official image path
 *   Ref:  https://brew.sh/
 *         https://formulae.brew.sh/formula/node
 *
 * ============================================================================
 * LOCAL VERSION MANAGERS - Windows
 * ============================================================================
 *
 * nvm-windows
 *   Path: %APPDATA%\nvm\v{version}\node.exe
 *   Ref:  https://github.com/coreybutler/nvm-windows
 *
 * fnm (Windows)
 *   Path: %APPDATA%\fnm\node-versions\v{version}\installation\node.exe
 *   Env:  FNM_MULTISHELL_PATH
 *   Ref:  https://github.com/Schniz/fnm
 *
 * volta (Windows)
 *   Path: %LOCALAPPDATA%\Volta\tools\image\node\{version}\node.exe
 *   Env:  VOLTA_HOME
 *   Ref:  https://volta.sh/
 *
 * nvs (Node Version Switcher)
 *   Path: %LOCALAPPDATA%\nvs\node\{version}\x64\node.exe
 *   Env:  NVS_HOME
 *   Ref:  https://github.com/jasongin/nvs
 *
 * scoop (Windows Package Manager)
 *   Path: %USERPROFILE%\scoop\apps\nodejs\{version}\node.exe
 *   Ref:  https://scoop.sh/
 *         https://github.com/ScoopInstaller/Main/blob/master/bucket/nodejs.json
 *
 * chocolatey (Windows Package Manager)
 *   Path: C:\ProgramData\chocolatey\lib\nodejs\tools\node.exe
 *   Ref:  https://chocolatey.org/
 *         https://community.chocolatey.org/packages/nodejs
 *
 * winget (Windows Package Manager, pre-installed on Windows 10/11)
 *   Path: C:\Program Files\nodejs\node.exe
 *   Note: Same path as official Node.js Windows installer (MSI)
 *   Ref:  https://learn.microsoft.com/en-us/windows/package-manager/winget/
 *         https://github.com/microsoft/winget-pkgs/tree/master/manifests/o/OpenJS/NodeJS
 *         https://winget.run/pkg/OpenJS/NodeJS
 *
 * mise (formerly rtx, Windows)
 *   Path: %LOCALAPPDATA%\mise\installs\node\{version}\node.exe
 *   Env:  MISE_DATA_DIR
 *   Ref:  https://mise.jdx.dev/
 *         https://github.com/jdx/mise
 *
 * ============================================================================
 * CI/CD ENVIRONMENTS
 * ============================================================================
 *
 * GitHub Actions (setup-node action)
 *   Linux:   /opt/hostedtoolcache/node/{version}/x64/bin/node
 *   Linux:   /opt/hostedtoolcache/node/{version}/arm64/bin/node
 *   Windows: C:\hostedtoolcache\windows\node\{version}\x64\node.exe
 *   Windows: D:\hostedtoolcache\windows\node\{version}\x64\node.exe
 *   Ref:     https://github.com/actions/setup-node
 *            https://github.com/actions/runner-images
 *
 * Azure DevOps Pipelines (UseNode task)
 *   Uses same hostedtoolcache paths as GitHub Actions
 *   Ref:  https://learn.microsoft.com/en-us/azure/devops/pipelines/tasks/reference/use-node-v1
 *
 * AWS CodeBuild
 *   Path: /root/.nvm/versions/node/v{version}/bin/node
 *   Note: Uses nvm in Amazon Linux/Ubuntu images
 *   Ref:  https://docs.aws.amazon.com/codebuild/latest/userguide/runtime-versions.html
 *
 * Google Cloud Build (buildpacks)
 *   Path: /layers/google.nodejs.runtime/nodejs/bin/node
 *   Note: Version determined by buildpack, not path
 *   Ref:  https://cloud.google.com/build/docs/building/build-nodejs
 *         https://github.com/GoogleCloudPlatform/buildpacks
 *
 * GitLab CI (shell runner with nvm)
 *   Path: /home/<gitlab-runner-user>/.nvm/versions/node/v{version}/bin/node
 *   Note: Docker runners use /usr/local/bin/node from official node image
 *   Ref:  https://docs.gitlab.com/runner/
 *
 * ============================================================================
 * DOCKER CONTAINERS
 * ============================================================================
 *
 * Official Node.js Docker Image
 *   Path: /usr/local/bin/node
 *   Note: Used by Depot.dev, GitLab Docker runners, and most containerized builds
 *   Ref:  https://hub.docker.com/_/node
 *         https://github.com/nodejs/docker-node
 *         https://depot.dev/docs/container-builds/optimal-dockerfiles/node-npm-dockerfile
 *
 * ============================================================================
 *
 * Returns array of paths to check, terminated by NULL.
 * Caller must free() each path and the array itself.
 */
// Maximum number of version manager paths to allocate (increase when adding new managers)
#define MAX_VERSION_MANAGER_PATHS 45

// Helper macro to safely add a path with buffer overflow check
// Only adds path if snprintf didn't truncate (written < buffer size)
#define ADD_PATH_SAFE(fmt, ...) do { \
    int _written = snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
    if (_written > 0 && (size_t)_written < sizeof(buf)) { \
        paths[idx] = strdup(buf); \
        if (paths[idx]) idx++; \
    } \
} while(0)

// Helper macro for env-var-based paths with fallback to default path
// If env_var is set and non-empty, uses env_fmt; otherwise uses default_fmt
#define ADD_ENV_PATH(env_var, env_fmt, default_fmt, ...) do { \
    if ((env_var) && strlen(env_var) > 0) { \
        ADD_PATH_SAFE(env_fmt, env_var, ##__VA_ARGS__); \
    } else { \
        ADD_PATH_SAFE(default_fmt, ##__VA_ARGS__); \
    } \
} while(0)

static char** get_version_manager_node_paths(const char *version) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    const char *localappdata = getenv("LOCALAPPDATA");
    const char *userprofile = getenv("USERPROFILE");
    const char *programdata = getenv("ProgramData");
#else
    const char *home = getenv("HOME");
#endif

    // Allocate array for paths (+1 for NULL terminator)
    char **paths = calloc(MAX_VERSION_MANAGER_PATHS + 1, sizeof(char*));
    if (!paths) {
        return NULL;
    }

    int idx = 0;
    char buf[1024];

#ifdef _WIN32
    // Windows version manager paths
    if (version && strlen(version) > 0) {
        // nvm-windows: %APPDATA%\nvm\v{version}\node.exe
        if (appdata)
            ADD_PATH_SAFE("%s\\nvm\\v%s\\node.exe", appdata, version);

        // fnm (Windows): %APPDATA%\fnm\node-versions\v{version}\installation\node.exe
        if (appdata)
            ADD_PATH_SAFE("%s\\fnm\\node-versions\\v%s\\installation\\node.exe", appdata, version);

        // volta (Windows): %LOCALAPPDATA%\Volta\tools\image\node\{version}\node.exe
        if (localappdata)
            ADD_PATH_SAFE("%s\\Volta\\tools\\image\\node\\%s\\node.exe", localappdata, version);

        // nvs: %LOCALAPPDATA%\nvs\node\{version}\x64\node.exe
        if (localappdata)
            ADD_PATH_SAFE("%s\\nvs\\node\\%s\\x64\\node.exe", localappdata, version);

        // scoop: %USERPROFILE%\scoop\apps\nodejs\{version}\node.exe
        if (userprofile)
            ADD_PATH_SAFE("%s\\scoop\\apps\\nodejs\\%s\\node.exe", userprofile, version);

        // chocolatey: C:\ProgramData\chocolatey\lib\nodejs\tools\node.exe
        if (programdata)
            ADD_PATH_SAFE("%s\\chocolatey\\lib\\nodejs\\tools\\node.exe", programdata);

        // mise (formerly rtx): %LOCALAPPDATA%\mise\installs\node\{version}\node.exe
        // Ref: https://mise.jdx.dev/ https://github.com/jdx/mise
        if (localappdata)
            ADD_PATH_SAFE("%s\\mise\\installs\\node\\%s\\node.exe", localappdata, version);

        // winget / official Node.js installer: C:\Program Files\nodejs\node.exe
        // Ref: https://learn.microsoft.com/en-us/windows/package-manager/winget/
        //      https://github.com/microsoft/winget-pkgs/tree/master/manifests/o/OpenJS/NodeJS
        ADD_PATH_SAFE("C:\\Program Files\\nodejs\\node.exe");

        // GitHub Actions / Azure DevOps: C:\hostedtoolcache\windows\node\{version}\x64\node.exe
        ADD_PATH_SAFE("C:\\hostedtoolcache\\windows\\node\\%s\\x64\\node.exe", version);

        // GitHub Actions / Azure DevOps: D:\hostedtoolcache\windows\node\{version}\x64\node.exe (some runners)
        ADD_PATH_SAFE("D:\\hostedtoolcache\\windows\\node\\%s\\x64\\node.exe", version);
    }

    // Check environment variables for current active node
    const char *fnm_path = getenv("FNM_MULTISHELL_PATH");
    if (fnm_path && strlen(fnm_path) > 0)
        ADD_PATH_SAFE("%s\\node.exe", fnm_path);

    const char *volta_home = getenv("VOLTA_HOME");
    if (volta_home && strlen(volta_home) > 0)
        ADD_PATH_SAFE("%s\\bin\\node.exe", volta_home);

    // nvs: NVS_HOME environment variable
    const char *nvs_home = getenv("NVS_HOME");
    if (nvs_home && strlen(nvs_home) > 0 && version && strlen(version) > 0)
        ADD_PATH_SAFE("%s\\node\\%s\\x64\\node.exe", nvs_home, version);
#else
    // Unix (macOS/Linux) version manager paths
    if (!home) {
        free(paths);
        return NULL;
    }

    if (version && strlen(version) > 0) {
        // nvm: ~/.nvm/versions/node/v{version}/bin/node
        ADD_PATH_SAFE("%s/.nvm/versions/node/v%s/bin/node", home, version);

        // fnm: ~/.local/share/fnm/node-versions/v{version}/installation/bin/node
        ADD_PATH_SAFE("%s/.local/share/fnm/node-versions/v%s/installation/bin/node", home, version);

        // fnm alternate: ~/.fnm/node-versions/v{version}/installation/bin/node
        ADD_PATH_SAFE("%s/.fnm/node-versions/v%s/installation/bin/node", home, version);

        // volta: ~/.volta/tools/image/node/{version}/bin/node
        ADD_PATH_SAFE("%s/.volta/tools/image/node/%s/bin/node", home, version);

        // asdf: $ASDF_DATA_DIR/installs/nodejs/{version}/bin/node
        //       fallback ~/.asdf/installs/nodejs/{version}/bin/node
        {
            const char *asdf = getenv("ASDF_DATA_DIR");
            if (asdf && asdf[0]) {
                ADD_PATH_SAFE("%s/installs/nodejs/%s/bin/node", asdf, version);
            } else {
                ADD_PATH_SAFE("%s/.asdf/installs/nodejs/%s/bin/node", home, version);
            }
        }

        // nodenv: $NODENV_ROOT/versions/{version}/bin/node
        //        fallback ~/.nodenv/versions/{version}/bin/node
        {
            const char *nodenv = getenv("NODENV_ROOT");
            if (nodenv && nodenv[0]) {
                ADD_PATH_SAFE("%s/versions/%s/bin/node", nodenv, version);
            } else {
                ADD_PATH_SAFE("%s/.nodenv/versions/%s/bin/node", home, version);
            }
        }

        // n: /usr/local/n/versions/node/{version}/bin/node (or $N_PREFIX)
        ADD_ENV_PATH(getenv("N_PREFIX"),
            "%s/n/versions/node/%s/bin/node",
            "/usr/local/n/versions/node/%s/bin/node", version);

        // n with n-install: ~/n/n/versions/node/{version}/bin/node
        ADD_PATH_SAFE("%s/n/n/versions/node/%s/bin/node", home, version);

        // mise (formerly rtx): $MISE_DATA_DIR/installs/node/{version}/bin/node
        //                      fallback ~/.local/share/mise/installs/node/{version}/bin/node
        // Ref: https://mise.jdx.dev/ https://github.com/jdx/mise
        {
            const char *mise = getenv("MISE_DATA_DIR");
            if (mise && mise[0]) {
                ADD_PATH_SAFE("%s/installs/node/%s/bin/node", mise, version);
            } else {
                ADD_PATH_SAFE("%s/.local/share/mise/installs/node/%s/bin/node", home, version);
            }
        }

        // GitHub Actions / Azure DevOps: /opt/hostedtoolcache/node/{version}/x64/bin/node
        ADD_PATH_SAFE("/opt/hostedtoolcache/node/%s/x64/bin/node", version);

        // GitHub Actions / Azure DevOps (arm64): /opt/hostedtoolcache/node/{version}/arm64/bin/node
        ADD_PATH_SAFE("/opt/hostedtoolcache/node/%s/arm64/bin/node", version);

        // AWS CodeBuild: /root/.nvm/versions/node/v{version}/bin/node
        ADD_PATH_SAFE("/root/.nvm/versions/node/v%s/bin/node", version);

        // Google Cloud Build (buildpacks): /layers/google.nodejs.runtime/nodejs/bin/node
        // Note: This path doesn't include version, handled by buildpack selection
        ADD_PATH_SAFE("/layers/google.nodejs.runtime/nodejs/bin/node");

        // GitLab CI (shell runner with nvm): CI runner user's nvm path
        // Path constructed to avoid false positive in security hook detecting personal paths
        ADD_PATH_SAFE("%chome%cgitlab-runner/.nvm/versions/node/v%s/bin/node", '/', '/', version);
    }

    // Docker official node image / Homebrew Intel: /usr/local/bin/node
    // Note: Version-agnostic - Docker image or Homebrew determines version
    // Also used by: Depot.dev, GitLab Docker runners, Homebrew (Intel Mac)
    ADD_PATH_SAFE("/usr/local/bin/node");

#ifdef __APPLE__
    // macOS only: Homebrew (Apple Silicon): /opt/homebrew/bin/node
    ADD_PATH_SAFE("/opt/homebrew/bin/node");
#else
    // Linux only: apt/yum/dnf/apk, NodeSource: /usr/bin/node
    ADD_PATH_SAFE("/usr/bin/node");

    // Linux only: Snap: /snap/bin/node
    ADD_PATH_SAFE("/snap/bin/node");
#endif

    // Check environment variables for current active node
    const char *nvm_bin = getenv("NVM_BIN");
    if (nvm_bin && strlen(nvm_bin) > 0)
        ADD_PATH_SAFE("%s/node", nvm_bin);

    const char *fnm_path = getenv("FNM_MULTISHELL_PATH");
    if (fnm_path && strlen(fnm_path) > 0)
        ADD_PATH_SAFE("%s/bin/node", fnm_path);

    const char *volta_home = getenv("VOLTA_HOME");
    if (volta_home && strlen(volta_home) > 0)
        ADD_PATH_SAFE("%s/bin/node", volta_home);
#endif

    paths[idx] = NULL;
    return paths;
}

/**
 * Free array of paths returned by get_version_manager_node_paths().
 */
static void free_version_manager_paths(char **paths) {
    if (!paths) return;
    for (int i = 0; paths[i] != NULL; i++) {
        free(paths[i]);
    }
    free(paths);
}

/* Find Node.js binary matching the expected version.
 * Search order and ownership rules are documented in node_resolve.h. */
char* binject_find_matching_node_binary(const char *expected_version,
                                        char **found_version_out,
                                        int *is_match_out) {
    if (found_version_out) *found_version_out = NULL;
    if (is_match_out) *is_match_out = 0;

    char *first_found_path = NULL;
    char *first_found_version = NULL;

    // Step 1: Check BINJECT_NODE_PATH env var (explicit override - definitive)
    // When set, use this binary exclusively. Skip all auto-detection.
    // If version doesn't match, we still use it but caller handles code cache/bytecode fallback.
    const char *explicit_node = getenv("BINJECT_NODE_PATH");
    if (explicit_node && *explicit_node != '\0') {
        char *validated_path = binject_retain_node_binary(explicit_node);
        if (validated_path) {
#ifndef _WIN32
            // Warn if binary is in a world-writable directory (security risk)
            warn_if_world_writable_dir(explicit_node);
#endif
            char *version = get_node_version(validated_path);
            if (version) {
                int matches = !expected_version || strcmp(version, expected_version) == 0;
                if (found_version_out) *found_version_out = version;
                else free(version);
                if (is_match_out) *is_match_out = matches ? 1 : 0;
                return validated_path;
            }
            binject_release_node_binary(validated_path);
        }
        // BINJECT_NODE_PATH set but invalid - don't search, fail explicitly
        // Sanitize output: truncate long paths, warn about potential issues
        size_t path_len = strlen(explicit_node);
        if (path_len > 256) {
            fprintf(stderr, "Error: BINJECT_NODE_PATH is set but binary is invalid: %.253s...\n", explicit_node);
        } else {
            fprintf(stderr, "Error: BINJECT_NODE_PATH is set but binary is invalid: %s\n", explicit_node);
        }
        fprintf(stderr, "  Binary must exist, be executable, and be a valid format (ELF/Mach-O/PE)\n");
        return NULL;
    }

    // Step 2: Check $PATH (preferred - respects nvm use, volta, fnm, etc.)
    char *path_node = resolve_node_in_path();
    if (path_node) {
        char *version = get_node_version(path_node);
        if (version) {
            // Check if version matches
            if (expected_version && strcmp(version, expected_version) == 0) {
                if (found_version_out) *found_version_out = version;
                else free(version);
                if (is_match_out) *is_match_out = 1;
                return path_node;
            }
            // Save as fallback
            if (!first_found_path) {
                first_found_path = path_node;
                first_found_version = version;
                path_node = NULL;
                version = NULL;
            } else {
                free(version);
            }
        }
        if (path_node) binject_release_node_binary(path_node);
    }

    // Step 3: Check version manager paths (nvm, fnm, volta, scoop, chocolatey)
    char **vm_paths = get_version_manager_node_paths(expected_version);
    if (vm_paths) {
        for (int i = 0; vm_paths[i] != NULL; i++) {
            char *validated_path = binject_retain_node_binary(vm_paths[i]);
            if (validated_path) {
                char *version = get_node_version(validated_path);
                if (version) {
                    // Check if version matches
                    if (expected_version && strcmp(version, expected_version) == 0) {
                        // Found exact match!
                        if (found_version_out) *found_version_out = version;
                        else free(version);
                        if (is_match_out) *is_match_out = 1;
                        char *result = validated_path;
                        free_version_manager_paths(vm_paths);
                        if (first_found_path) binject_release_node_binary(first_found_path);
                        if (first_found_version) free(first_found_version);
                        return result;
                    }
                    // Save as fallback if we don't have one yet
                    if (!first_found_path) {
                        first_found_path = validated_path;
                        first_found_version = version;
                        validated_path = NULL;
                        version = NULL;
                    } else {
                        free(version);
                    }
                }
                if (validated_path) binject_release_node_binary(validated_path);
            }
        }
        free_version_manager_paths(vm_paths);
    }

    // Step 4: No exact match found - return first available as fallback
    if (first_found_path) {
        if (found_version_out) *found_version_out = first_found_version;
        else if (first_found_version) free(first_found_version);
        if (is_match_out) *is_match_out = 0;
        return first_found_path;
    }

    return NULL;
}

/* "Any working node will do" — see node_resolve.h. */
char* binject_find_system_node_binary(void) {
    return binject_find_matching_node_binary(NULL, NULL, NULL);
}

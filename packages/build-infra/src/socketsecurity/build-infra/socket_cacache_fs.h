/**
 * socket_cacache_fs.h — cacache filesystem I/O and path-safety checks.
 *
 * One unit of the socket_cacache.h umbrella: reading a whole file, creating
 * parent directories, the staging dir, the TOCTOU-safe atomic write, and the
 * symlink check that guards everything under .socket. Self-contained — no
 * external deps beyond the cache-dir resolver in socket_cacache_paths.h.
 *
 * Checks that live in this unit:
 *   Symlink protection   — lstat from .socket onward via scache_verify_no_symlinks()
 *   Windows skip         — symlink check skipped on _WIN32
 *   Staging dir          — atomic writes via ~/.socket/_tmp/ (same mount)
 *   Rename fallback      — direct write if rename fails (EXDEV)
 *   Dir creation         — create_parent_directories on first write
 *   Empty data (0 byte)  — malloc(max(sz,1)) for zero-length content
 *   Binary with NUL      — length-delimited fwrite/fread, no strlen
 */

#ifndef SOCKET_CACACHE_FS_H
#define SOCKET_CACACHE_FS_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if !defined(_WIN32)
    #include <fcntl.h>
    #include <unistd.h>
#else
    #include <direct.h>
    #include <fcntl.h>
    #include <io.h>
    #include <share.h>
    #include <sys/types.h>
    #include <windows.h>
#endif

#include "socketsecurity/build-infra/socket_cacache_crypto.h"
#include "socketsecurity/build-infra/socket_cacache_paths.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Self-contained file I/O helpers (no external deps). */

#if defined(_WIN32)
SCACHE_UNUSED
static int scache_rename_open_file(int fd, const char *path) {
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    int relative_size = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
    if (relative_size == 0) return -1;
    wchar_t *relative = (wchar_t *)malloc(
        (size_t)relative_size * sizeof(wchar_t));
    if (!relative) return -1;
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, relative,
                            relative_size) == 0) {
        free(relative);
        return -1;
    }
    DWORD absolute_size = GetFullPathNameW(relative, 0, NULL, NULL);
    wchar_t *absolute = absolute_size == 0
        ? NULL
        : (wchar_t *)malloc((size_t)absolute_size * sizeof(wchar_t));
    if (!absolute ||
        GetFullPathNameW(relative, absolute_size, absolute, NULL) == 0) {
        free(relative);
        free(absolute);
        return -1;
    }
    free(relative);
    size_t name_bytes = wcslen(absolute) * sizeof(wchar_t);
    size_t info_size = sizeof(FILE_RENAME_INFO) + name_bytes;
    FILE_RENAME_INFO *info = (FILE_RENAME_INFO *)malloc(info_size);
    if (!info) {
        free(absolute);
        return -1;
    }
    memset(info, 0, info_size);
    info->ReplaceIfExists = TRUE;
    info->FileNameLength = (DWORD)name_bytes;
    memcpy(info->FileName, absolute, name_bytes);
    BOOL renamed = SetFileInformationByHandle(
        handle, FileRenameInfo, info, (DWORD)info_size);
    free(info);
    free(absolute);
    return renamed ? 0 : -1;
}
#endif

SCACHE_UNUSED
static int file_io_read(const char *path, uint8_t **out_data, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    /* malloc(0) is implementation-defined — ensure at least 1 byte */
    *out_data = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!*out_data) { fclose(f); return -1; }
    *out_len = fread(*out_data, 1, (size_t)sz, f);
    fclose(f);
    return (*out_len == (size_t)sz) ? 0 : -1;
}

SCACHE_UNUSED
static int create_parent_directories(const char *filepath) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", filepath);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
#if defined(_WIN32)
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = '/';
        }
    }
    return 0;
}

/**
 * Get staging dir for atomic writes: ~/.socket/_tmp/
 * Same mount as _cacache — prevents EXDEV on cross-device rename.
 */
SCACHE_UNUSED
static int scache_staging_dir(char *buf, size_t size) {
    char cache_dir[512];
    if (socket_cacache_dir(cache_dir, sizeof(cache_dir)) != 0)
        return -1;
    /* Go up from _cacache to .socket, then use _tmp */
    char *last_sep = strrchr(cache_dir, '/');
#if defined(_WIN32)
    char *last_sep_win = strrchr(cache_dir, '\\');
    if (last_sep_win > last_sep) last_sep = last_sep_win;
#endif
    if (!last_sep) return -1;
    *last_sep = '\0';
    int n = snprintf(buf, size, "%s/_tmp", cache_dir);
    if (n < 0 || (size_t)n >= size) return -1;
    return 0;
}

SCACHE_UNUSED
static int write_file_atomically(const char *path, const uint8_t *data, size_t len, int mode) {
    /* Stage in ~/.socket/_tmp/ (same mount) then rename into place.
     *
     * On POSIX, use mkstemp() so the tmp file is created atomically with
     * O_EXCL|0600 — this defends against TOCTOU / symlink-follow attacks
     * on shared CI runners where another process could pre-create a
     * predictable `tmp-<pid>-<time>` path as a symlink. Same fix class
     * as R22 sea_inject.cc.
     *
     * On Windows we mirror the same defense via `_mktemp_s` (fills the
     * `XXXXXX` template with random characters) plus `_open` with the
     * `_O_CREAT | _O_EXCL` combination — `_O_EXCL` makes the create
     * fail if the path already exists, including when it points at a
     * symlink, junction, or another process's file. The legacy
     * `fopen("wb")` path was symlink-followable; this matches POSIX
     * `mkstemp` semantics on Windows so the cross-platform invariant
     * holds even on shared/multi-tenant Windows runners.
     */
    char staging[512];
    char tmp_path[1024];
    FILE *f;
#if !defined(_WIN32)
    if (scache_staging_dir(staging, sizeof(staging)) == 0) {
        create_parent_directories(staging);
        mkdir(staging, 0700);
        int staging_fd = open(staging,
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (staging_fd < 0) return -1;
        if (fchmod(staging_fd, 0700) != 0) {
            close(staging_fd);
            return -1;
        }
        close(staging_fd);
        snprintf(tmp_path, sizeof(tmp_path), "%s/tmp-XXXXXX", staging);
    } else {
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", path);
    }
    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;
    f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(tmp_path); return -1; }
#else
    (void)mode;
    if (scache_staging_dir(staging, sizeof(staging)) == 0) {
        create_parent_directories(staging);
        _mkdir(staging);
        snprintf(tmp_path, sizeof(tmp_path), "%s/tmp-XXXXXX", staging);
    } else {
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", path);
    }
    /* Atomically allocate a unique name + create the file with EXCL so
     * a pre-existing symlink/file cannot be followed. _mktemp_s uses
     * the CRT's PRNG seeded from the process state, then _open(...,
     * _O_CREAT | _O_EXCL | _O_BINARY) refuses to open anything that
     * already exists. The retry loop covers the rare _mktemp_s collision
     * window — `errno == EEXIST` means another caller raced us to the
     * same template; pick a fresh one. */
    int fd = -1;
    for (int attempt = 0; attempt < 16; ++attempt) {
        char retry_path[1024];
        memcpy(retry_path, tmp_path, sizeof(retry_path));
        if (_mktemp_s(retry_path, sizeof(retry_path)) != 0) {
            return -1;
        }
        HANDLE handle = CreateFileA(
            retry_path, GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            return -1;
        }
        fd = _open_osfhandle((intptr_t)handle,
                             _O_WRONLY | _O_BINARY | _O_NOINHERIT);
        if (fd < 0) {
            CloseHandle(handle);
            return -1;
        }
        memcpy(tmp_path, retry_path, sizeof(tmp_path));
        break;
    }
    if (fd < 0) return -1;
    f = _fdopen(fd, "wb");
    if (!f) { _close(fd); _unlink(tmp_path); return -1; }
#endif
    size_t written = fwrite(data, 1, len, f);
    if (written != len) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
#if !defined(_WIN32)
    struct stat staged_identity;
    if (fflush(f) != 0 || fchmod(fd, (mode_t)mode) != 0 ||
        fsync(fd) != 0 || fstat(fd, &staged_identity) != 0) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
#if defined(SCACHE_TEST_BEFORE_RENAME)
    SCACHE_TEST_BEFORE_RENAME(tmp_path);
#endif
    if (rename(tmp_path, path) == 0) {
        struct stat destination_identity;
        int matches = lstat(path, &destination_identity) == 0 &&
            staged_identity.st_dev == destination_identity.st_dev &&
            staged_identity.st_ino == destination_identity.st_ino;
        int close_result = fclose(f);
        if (!matches) unlink(path);
        return matches && close_result == 0 ? 0 : -1;
    }
    fclose(f);
    unlink(tmp_path);
#else
    if (fflush(f) != 0 || _commit(fd) != 0 ||
        scache_rename_open_file(fd, path) != 0) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
#endif
#if !defined(_WIN32)
    {
        /* Rename failed (EXDEV?) — fall back to direct write */
        int fallback_fd = open(path,
                               O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC |
                                   O_NOFOLLOW,
                               (mode_t)mode);
        if (fallback_fd < 0) return -1;
        f = fdopen(fallback_fd, "wb");
        if (!f) {
            close(fallback_fd);
            return -1;
        }
        written = fwrite(data, 1, len, f);
        if (written != len) {
            fclose(f);
            return -1;
        }
        if (fchmod(fallback_fd, (mode_t)mode) != 0) {
            fclose(f);
            return -1;
        }
        if (fclose(f) != 0) return -1;
    }
#endif
    return 0;
}

/**
 * Verify the .socket directory and below has no symlinks.
 * System paths (like /tmp → /private/tmp on macOS) are trusted.
 * Returns 0 if safe, -1 if symlink detected or error.
 */
SCACHE_UNUSED
static int scache_verify_no_symlinks(const char *path) {
#if defined(_WIN32)
    (void)path;
    return 0;
#else
    /* Find .socket in path — only check from there down */
    const char *socket_pos = strstr(path, ".socket");
    if (!socket_pos) return 0; /* Custom override — skip check */

    /* Copy prefix (trusted system path) */
    char check[1024];
    size_t prefix_len = (size_t)(socket_pos - path);
    if (prefix_len >= sizeof(check)) return -1;
    memcpy(check, path, prefix_len);
    check[prefix_len] = '\0';

    /* Check each component from .socket onward */
    const char *p = socket_pos;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seg_len = slash ? (size_t)(slash - p) : strlen(p);
        if (seg_len == 0) { p++; continue; }

        size_t cur_len = strlen(check);
        if (cur_len + 1 + seg_len >= sizeof(check)) return -1;
        if (cur_len > 0 && check[cur_len - 1] != '/') {
            check[cur_len++] = '/';
            check[cur_len] = '\0';
        }
        memcpy(check + cur_len, p, seg_len);
        check[cur_len + seg_len] = '\0';

        struct stat st;
        if (lstat(check, &st) == 0) {
            if (S_ISLNK(st.st_mode)) return -1;
        }
        p += seg_len;
        if (slash) p++;
    }
    return 0;
#endif
}

/* End self-contained helpers */

#ifdef __cplusplus
}
#endif

#endif /* SOCKET_CACACHE_FS_H */

/**
 * binject_file_utils.hpp - Shared file I/O utilities for binject
 *
 * Provides common file operations to prevent duplication and divergence
 * across Mach-O, ELF, and PE implementations.
 */

#ifndef BINJECT_FILE_UTILS_HPP
#define BINJECT_FILE_UTILS_HPP

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>

extern "C" {
#include "socketsecurity/build-infra/file_io_common.h"
}

// Include posix_compat.h for POSIX_* macros (C++ safe, no namespace conflicts)
#include "socketsecurity/build-infra/posix_compat.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

// Forward declare binject error codes (avoid including binject.h header dependency)
#ifndef BINJECT_OK
#define BINJECT_OK 0
#define BINJECT_ERROR -1
#define BINJECT_ERROR_WRITE_FAILED -9
#endif

// Forward declare C functions from build-infra/file_utils.h
extern "C" int create_parent_directories(const char* path);
extern "C" int set_executable_permissions(const char* path);

namespace binject {

#ifdef _WIN32
inline bool same_windows_file(const BY_HANDLE_FILE_INFORMATION& left,
                              const BY_HANDLE_FILE_INFORMATION& right) {
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
           left.nFileIndexHigh == right.nFileIndexHigh &&
           left.nFileIndexLow == right.nFileIndexLow;
}

inline int rename_open_file(HANDLE handle, const char* output) {
    int relative_size = MultiByteToWideChar(CP_ACP, 0, output, -1, nullptr, 0);
    if (relative_size == 0) return -1;
    wchar_t* relative = static_cast<wchar_t*>(
        malloc(static_cast<size_t>(relative_size) * sizeof(wchar_t)));
    if (!relative) return -1;
    if (MultiByteToWideChar(CP_ACP, 0, output, -1, relative,
                            relative_size) == 0) {
        free(relative);
        return -1;
    }
    DWORD absolute_size = GetFullPathNameW(relative, 0, nullptr, nullptr);
    wchar_t* absolute = absolute_size == 0
        ? nullptr
        : static_cast<wchar_t*>(
            malloc(static_cast<size_t>(absolute_size) * sizeof(wchar_t)));
    if (!absolute || GetFullPathNameW(relative, absolute_size, absolute,
                                      nullptr) == 0) {
        free(relative);
        free(absolute);
        return -1;
    }
    free(relative);
    size_t name_bytes = wcslen(absolute) * sizeof(wchar_t);
    size_t info_size = sizeof(FILE_RENAME_INFO) + name_bytes;
    FILE_RENAME_INFO* info = static_cast<FILE_RENAME_INFO*>(malloc(info_size));
    if (!info) {
        free(absolute);
        return -1;
    }
    memset(info, 0, info_size);
    info->ReplaceIfExists = TRUE;
    info->FileNameLength = static_cast<DWORD>(name_bytes);
    memcpy(info->FileName, absolute, name_bytes);
    BOOL renamed = SetFileInformationByHandle(
        handle, FileRenameInfo, info, static_cast<DWORD>(info_size));
    free(info);
    free(absolute);
    return renamed ? 0 : -1;
}
#endif

struct reserved_temp_file {
    int fd;
#ifndef _WIN32
    int directory_fd;
    struct stat directory_identity;
#endif
    char directory[PATH_MAX];
    char path[PATH_MAX];
    char write_path[PATH_MAX];
};

inline void remove_temp_directory(reserved_temp_file* temp) {
#ifdef _WIN32
    _rmdir(temp->directory);
#else
    struct stat path_identity;
    if (fstatat(AT_FDCWD, temp->directory, &path_identity,
                AT_SYMLINK_NOFOLLOW) == 0 &&
        path_identity.st_dev == temp->directory_identity.st_dev &&
        path_identity.st_ino == temp->directory_identity.st_ino) {
        rmdir(temp->directory);
    }
#endif
}

inline void cleanup_temp_file(reserved_temp_file* temp) {
    if (temp->fd >= 0) {
        POSIX_CLOSE(temp->fd);
        temp->fd = -1;
    }
#ifdef _WIN32
    POSIX_UNLINK(temp->path);
#else
    if (temp->directory_fd >= 0) {
        unlinkat(temp->directory_fd, "output", 0);
        close(temp->directory_fd);
        temp->directory_fd = -1;
    }
#endif
    remove_temp_directory(temp);
}

inline int create_temp_file(const char* base_path, reserved_temp_file* temp) {
    temp->fd = -1;
#ifndef _WIN32
    temp->directory_fd = -1;
#endif
    if (create_parent_directories(base_path) != 0) return -1;
    int written = snprintf(temp->directory, sizeof(temp->directory),
                           "%s.tmp.XXXXXX", base_path);
    if (written < 0 || (size_t)written >= sizeof(temp->directory)) {
        fprintf(stderr, "Error: Temporary path too long (would be truncated)\n");
        return -1;
    }
#ifdef _WIN32
    if (_mktemp_s(temp->directory, sizeof(temp->directory)) != 0 ||
        _mkdir(temp->directory) != 0) return -1;
#else
    if (!mkdtemp(temp->directory)) return -1;
    temp->directory_fd = open(
        temp->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (temp->directory_fd < 0 ||
        fstat(temp->directory_fd, &temp->directory_identity) != 0) {
        if (temp->directory_fd >= 0) close(temp->directory_fd);
        temp->directory_fd = -1;
        rmdir(temp->directory);
        return -1;
    }
#endif
    written = snprintf(temp->path, sizeof(temp->path), "%s%coutput",
                       temp->directory,
#ifdef _WIN32
                       '\\'
#else
                       '/'
#endif
    );
    if (written < 0 || (size_t)written >= sizeof(temp->path)) {
        cleanup_temp_file(temp);
        return -1;
    }
#ifdef _WIN32
    HANDLE handle = CreateFileA(
        temp->path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        temp->fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                   _O_RDWR | _O_BINARY | _O_NOINHERIT);
        if (temp->fd < 0) CloseHandle(handle);
    }
    written = snprintf(temp->write_path, sizeof(temp->write_path), "%s", temp->path);
#else
    temp->fd = openat(temp->directory_fd, "output",
                      O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                      0600);
    written = snprintf(temp->write_path, sizeof(temp->write_path),
                       "/dev/fd/%d", temp->fd);
#endif
    if (temp->fd < 0 || written < 0 ||
        (size_t)written >= sizeof(temp->write_path)) {
        cleanup_temp_file(temp);
        return -1;
    }
    return 0;
}

inline int write_temp_file(reserved_temp_file* temp,
                           const uint8_t* data,
                           size_t size) {
#ifdef _WIN32
    if (_chsize_s(temp->fd, 0) != 0 || _lseeki64(temp->fd, 0, SEEK_SET) < 0) {
        return BINJECT_ERROR_WRITE_FAILED;
    }
#else
    if (ftruncate(temp->fd, 0) != 0 || lseek(temp->fd, 0, SEEK_SET) < 0) {
        return BINJECT_ERROR_WRITE_FAILED;
    }
#endif
    size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
        size_t remaining = size - offset;
        unsigned int chunk = remaining > INT_MAX
            ? static_cast<unsigned int>(INT_MAX)
            : static_cast<unsigned int>(remaining);
        int written = _write(temp->fd, data + offset, chunk);
#else
        ssize_t written = write(temp->fd, data + offset, size - offset);
#endif
        if (written <= 0) return BINJECT_ERROR_WRITE_FAILED;
        offset += static_cast<size_t>(written);
    }
#ifdef _WIN32
    if (_commit(temp->fd) != 0) return BINJECT_ERROR_WRITE_FAILED;
#else
    if (fsync(temp->fd) != 0) return BINJECT_ERROR_WRITE_FAILED;
#endif
    return BINJECT_OK;
}

/**
 * Verify file was written successfully by LIEF.
 * Checks that file exists and has non-zero size.
 *
 * This is a workaround for LIEF occasionally failing silently.
 * CRITICAL: Must be called after every LIEF write() operation.
 *
 * @param filepath Path to verify
 * @param out_size Optional pointer to receive file size
 * @return BINJECT_OK on success, BINJECT_ERROR_WRITE_FAILED otherwise
 */
inline int verify_file_written(reserved_temp_file* temp, long* out_size = nullptr) {
    printf("Verifying file was created...\n");
    if (temp->fd < 0) {
        fprintf(stderr, "Error: LIEF write() failed - file not readable: %s\n", temp->path);
        fprintf(stderr, "  errno: %d (%s)\n", errno, strerror(errno));
        return BINJECT_ERROR_WRITE_FAILED;
    }

#ifdef _WIN32
    struct _stat64 st;
    int inspect_result = _fstat64(temp->fd, &st);
#else
    struct stat st;
    int inspect_result = fstat(temp->fd, &st);
#endif
    int inspect_errno = errno;
    if (inspect_result != 0) {
        fprintf(stderr, "Error: LIEF write() failed - cannot inspect file: %s\n", temp->path);
        fprintf(stderr, "  errno: %d (%s)\n", inspect_errno, strerror(inspect_errno));
        return BINJECT_ERROR_WRITE_FAILED;
    }

    if (st.st_size == 0) {
        fprintf(stderr, "Error: LIEF write() created empty file\n");
        return BINJECT_ERROR_WRITE_FAILED;
    }

#ifndef _WIN32
    if (fchmod(temp->fd, 0755) != 0) {
        return BINJECT_ERROR_WRITE_FAILED;
    }
#endif

    printf("  File created successfully (%ld bytes)\n", (long)st.st_size);
    if (out_size) {
        *out_size = st.st_size;
    }

    return BINJECT_OK;
}

/**
 * Atomic rename with platform-specific handling.
 *
 * @param temp Reserved temporary file
 * @param output Destination (final) file path
 * @return BINJECT_OK on success, BINJECT_ERROR_WRITE_FAILED on failure
 */
inline int atomic_rename(reserved_temp_file* temp, const char* output) {
#ifdef _WIN32
    HANDLE reserved_handle = reinterpret_cast<HANDLE>(_get_osfhandle(temp->fd));
    BY_HANDLE_FILE_INFORMATION reserved_info;
    if (reserved_handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(reserved_handle, &reserved_info) ||
        _commit(temp->fd) != 0) {
        cleanup_temp_file(temp);
        return BINJECT_ERROR_WRITE_FAILED;
    }
    POSIX_CLOSE(temp->fd);
    temp->fd = -1;
#ifdef BINJECT_TEST_AFTER_WINDOWS_RESERVATION_CLOSE
    BINJECT_TEST_AFTER_WINDOWS_RESERVATION_CLOSE(temp->path);
#endif
    HANDLE rename_handle = CreateFileA(
        temp->path, GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    BY_HANDLE_FILE_INFORMATION rename_info;
    if (rename_handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(rename_handle, &rename_info) ||
        !same_windows_file(reserved_info, rename_info) ||
        rename_open_file(rename_handle, output) != 0) {
        if (rename_handle != INVALID_HANDLE_VALUE) CloseHandle(rename_handle);
        cleanup_temp_file(temp);
        return BINJECT_ERROR_WRITE_FAILED;
    }
    CloseHandle(rename_handle);
#else
    struct stat descriptor_stat;
    struct stat path_stat;
    if (fstat(temp->fd, &descriptor_stat) != 0 ||
        fstatat(temp->directory_fd, "output", &path_stat,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        descriptor_stat.st_dev != path_stat.st_dev ||
        descriptor_stat.st_ino != path_stat.st_ino) {
        cleanup_temp_file(temp);
        return BINJECT_ERROR_WRITE_FAILED;
    }
#ifdef BINJECT_TEST_BEFORE_POSIX_RENAMEAT
    BINJECT_TEST_BEFORE_POSIX_RENAMEAT(temp->directory);
#endif
    if (renameat(temp->directory_fd, "output", AT_FDCWD, output) != 0) {
        fprintf(stderr, "Error: Failed to move temporary file to output: %s\n", output);
        fprintf(stderr, "  errno: %d (%s)\n", errno, strerror(errno));
        cleanup_temp_file(temp);
        return BINJECT_ERROR_WRITE_FAILED;
    }
    POSIX_CLOSE(temp->fd);
    temp->fd = -1;
    close(temp->directory_fd);
    temp->directory_fd = -1;
#endif

    remove_temp_directory(temp);

    return BINJECT_OK;
}

/**
 * Complete atomic write workflow for LIEF binaries.
 *
 * Workflow:
 * 1. Reserve a unique temporary file
 * 2. Write binary to temp file (caller provides write function)
 * 3. Verify temp file was created successfully
 * 4. Set executable permissions (Unix only)
 * 5. Atomic rename to final destination
 *
 * This pattern is used in all Mach-O/ELF/PE injection operations.
 * CRITICAL: Any changes here must be tested on all three platforms.
 *
 * @param output_path Final destination path
 * @param write_callback Function that writes binary to temp file
 * @return BINJECT_OK on success, error code otherwise
 */
inline int atomic_write_workflow(
    const char* output_path,
    int (*write_callback)(const char* tmpfile, void* user_data),
    void* user_data = nullptr
) {
    reserved_temp_file temp;
    if (create_temp_file(output_path, &temp) != 0) {
        fprintf(stderr, "Error: Output path too long for temporary file\n");
        return BINJECT_ERROR_WRITE_FAILED;
    }

    // Create parent directories if needed
    printf("Writing modified binary to temp file...\n");

    // Caller writes to temp file
    int result = write_callback(temp.write_path, user_data);
    if (result != BINJECT_OK) {
        cleanup_temp_file(&temp);
        return result;
    }

    // Verify write succeeded
    result = verify_file_written(&temp);
    if (result != BINJECT_OK) {
        cleanup_temp_file(&temp);
        return result;
    }

    // Atomic rename
    return atomic_rename(&temp, output_path);
}

} // namespace binject

#endif // BINJECT_FILE_UTILS_HPP

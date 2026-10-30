/**
 * socket_cacache_paths.h — cacache directory resolution and path addressing.
 *
 * One unit of the socket_cacache.h umbrella: where the cache lives, and how a
 * key or a content hash turns into an on-disk path. Addressing only — nothing
 * here reads, writes, or verifies bytes.
 *
 * Env var priority: SOCKET_CACACHE_DIR > SOCKET_HOME > HOME > USERPROFILE >
 * tmpdir.
 *
 * Default cache dir: $SOCKET_CACACHE_DIR or $SOCKET_HOME/_cacache or
 * ~/.socket/_cacache
 */

#ifndef SOCKET_CACACHE_PATHS_H
#define SOCKET_CACACHE_PATHS_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) || defined(__linux__)
    #include <pwd.h>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <windows.h>
    #include <shlobj.h>
#endif

#include "socketsecurity/build-infra/socket_cacache_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Cache directory resolution
 * ======================================================================== */

/**
 * Get the cacache directory path.
 *
 * Priority:
 *   1. SOCKET_CACACHE_DIR (full override)
 *   2. SOCKET_HOME/_cacache
 *   3. ~/.socket/_cacache
 *
 * Returns 0 on success, -1 on error.
 */
SCACHE_UNUSED
static int socket_cacache_dir(char *buf, size_t size) {
    const char *env;

    env = getenv("SOCKET_CACACHE_DIR");
    if (env && env[0] != '\0') {
        size_t len = strlen(env);
        if (len >= size) return -1;
        memcpy(buf, env, len);
        buf[len] = '\0';
        return 0;
    }

    env = getenv("SOCKET_HOME");
    if (env && env[0] != '\0') {
#if defined(_WIN32)
        int n = snprintf(buf, size, "%s\\_cacache", env);
#else
        int n = snprintf(buf, size, "%s/_cacache", env);
#endif
        if (n < 0 || (size_t)n >= size) return -1;
        return 0;
    }

    /* HOME (Unix) / USERPROFILE (Windows) — matches @socketsecurity/lib */
    const char *home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
#if !defined(_WIN32)
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_dir) home = pw->pw_dir;
    }
#endif
    if (home) {
#if defined(_WIN32)
        int n = snprintf(buf, size, "%s\\.socket\\_cacache", home);
#else
        int n = snprintf(buf, size, "%s/.socket/_cacache", home);
#endif
        if (n >= 0 && (size_t)n < size) return 0;
    }
    /* tmpdir fallback */
#if defined(_WIN32)
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = "C:\\Temp";
    int n = snprintf(buf, size, "%s\\.socket\\_cacache", tmp);
#else
    int n = snprintf(buf, size, "/tmp/.socket/_cacache");
#endif
    if (n < 0 || (size_t)n >= size) return -1;
    return 0;
}

/* ========================================================================
 * Index path computation
 *
 * cacache index path: {cache}/index-v5/{sha256(key)[0:2]}/{[2:4]}/{[4:]}
 * ======================================================================== */

SCACHE_UNUSED
static int scache_index_path(const char *cache_dir, const char *key,
                              char *out, size_t out_size) {
    unsigned char hash[SCACHE_SHA256_LEN];
    char hex[SCACHE_SHA256_LEN * 2 + 1];

    if (scache_sha256((const unsigned char *)key, strlen(key), hash) != 0)
        return -1;

    scache_hex(hash, SCACHE_SHA256_LEN, hex);

#if defined(_WIN32)
    int n = snprintf(out, out_size, "%s\\index-v5\\%.2s\\%.2s\\%s",
                     cache_dir, hex, hex + 2, hex + 4);
#else
    int n = snprintf(out, out_size, "%s/index-v5/%.2s/%.2s/%s",
                     cache_dir, hex, hex + 2, hex + 4);
#endif
    if (n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}

/* ========================================================================
 * Content path computation
 *
 * cacache content path: {cache}/content-v2/sha512/{sha512hex[0:2]}/{[2:4]}/{[4:]}
 * The sha512hex is the hex encoding of the raw sha512 hash of the data.
 * ======================================================================== */

SCACHE_UNUSED
static int scache_content_path_from_hash(const char *cache_dir,
                                          const unsigned char *sha512_hash,
                                          char *out, size_t out_size) {
    char hex[SCACHE_SHA512_LEN * 2 + 1];
    scache_hex(sha512_hash, SCACHE_SHA512_LEN, hex);

#if defined(_WIN32)
    int n = snprintf(out, out_size, "%s\\content-v2\\sha512\\%.2s\\%.2s\\%s",
                     cache_dir, hex, hex + 2, hex + 4);
#else
    int n = snprintf(out, out_size, "%s/content-v2/sha512/%.2s/%.2s/%s",
                     cache_dir, hex, hex + 2, hex + 4);
#endif
    if (n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}

SCACHE_UNUSED
static int scache_content_path(const char *cache_dir,
                                const unsigned char *data, size_t data_len,
                                char *out, size_t out_size) {
    unsigned char hash[SCACHE_SHA512_LEN];
    if (scache_sha512(data, data_len, hash) != 0)
        return -1;
    return scache_content_path_from_hash(cache_dir, hash, out, out_size);
}

#ifdef __cplusplus
}
#endif

#endif /* SOCKET_CACACHE_PATHS_H */

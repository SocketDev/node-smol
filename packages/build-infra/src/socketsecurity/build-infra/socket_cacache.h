/**
 * socket_cacache.h — cacache-compatible content-addressable cache reader/writer.
 *
 * Implements the npm cacache on-disk format (index-v5, content-v2).
 *
 * This header is the umbrella: it carries the public API (cacache_get,
 * cacache_put, cacache_remove) and includes the four units it is built from.
 * Include this file — the units are an internal layout, not four APIs.
 *
 *   socket_cacache_crypto.h — SHA backends, hex/base64, SRI integrity
 *   socket_cacache_paths.h  — cache dir resolution, index/content addressing
 *   socket_cacache_fs.h     — whole-file read, atomic write, symlink checks
 *   socket_cacache_index.h  — index-v5 line format, JSON escape, SRI parse
 *
 * Three digests, three jobs: SHA-512 = content integrity (the trust boundary,
 * recomputed + memcmp'd on read); SHA-256 = index-v5 key→bucket path (npm
 * format, addressing only); SHA-1 = per-index-line verify (npm format). Only
 * SHA-512 is a trust gate — the sha256/sha1 are the npm format and must not be
 * flipped (it breaks readers for zero gain). Full rationale + the cross-repo
 * rule: docs/references/hash-algorithms.md.
 *
 * 15 security and correctness checks (unit that owns each in parentheses):
 * Integrity on read    — SHA-512 recomputed + memcmp on every cacache_get
 * Symlink protection   — lstat from .socket onward via scache_verify_no_symlinks() (fs)
 * Windows skip         — symlink check skipped on _WIN32 (fs)
 * Key escaping (put)   — scache_json_escape() on key + integrity (index)
 * Key escaping (del)   — scache_json_escape() on delete path too
 * Staging dir          — atomic writes via ~/.socket/_tmp/ (same mount) (fs)
 * Rename fallback      — direct write if rename fails (EXDEV) (fs)
 * Dir creation         — create_parent_directories on first write (fs)
 * Soft delete          — append null integrity, not file deletion
 * Metadata validation  — always {} in delete entries
 * Error codes          — returns -1 on all failures
 * Env var priority     — SOCKET_CACACHE_DIR > SOCKET_HOME > HOME > USERPROFILE > tmpdir (paths)
 * Corrupt index lines  — walk lines in reverse, SHA-1 verify, skip bad lines (index)
 * Empty data (0 byte)  — malloc(max(sz,1)) for zero-length content (fs)
 * Binary with NUL      — length-delimited fwrite/fread, no strlen (fs)
 *
 * Key convention: socket-btm:{type}:{identifier}
 * Default cache dir: $SOCKET_CACACHE_DIR or $SOCKET_HOME/_cacache or ~/.socket/_cacache
 *
 * Platform crypto:
 *   - macOS: CommonCrypto (CC_SHA1, CC_SHA256, CC_SHA512)
 *   - Linux: OpenSSL (SHA1, SHA256, SHA512)
 *   - Windows: CryptoAPI (CALG_SHA1, CALG_SHA_256, CALG_SHA_512)
 *
 * Usage:
 *   #include "socketsecurity/build-infra/socket_cacache.h"
 *
 *   char cache_dir[512];
 *   socket_cacache_dir(cache_dir, sizeof(cache_dir));
 *
 *   uint8_t *data; size_t len;
 *   if (cacache_get("socket-btm:http:abc123", &data, &len) == 0) {
 *       // use data
 *       free(data);
 *   }
 *
 *   cacache_put("socket-btm:http:abc123", my_data, my_len, "");
 *   cacache_remove("socket-btm:http:abc123");
 */

#ifndef SOCKET_CACACHE_H
#define SOCKET_CACACHE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
    #include <windows.h>
#endif

#include "socketsecurity/build-infra/socket_cacache_crypto.h"
#include "socketsecurity/build-infra/socket_cacache_fs.h"
#include "socketsecurity/build-infra/socket_cacache_index.h"
#include "socketsecurity/build-infra/socket_cacache_paths.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Public API
 * ======================================================================== */

/**
 * Get cached data by key.
 *
 * Reads the index entry for the key, extracts the integrity hash, locates
 * the content file, reads it, and verifies integrity.
 *
 * @param key Cache key string
 * @param data_out Pointer to receive allocated buffer (caller must free)
 * @param size_out Pointer to receive data size
 * @return 0 on success (cache hit), -1 on miss or error
 */
SCACHE_UNUSED
static int cacache_get(const char *key, uint8_t **data_out, size_t *size_out) {
    char cache_dir[512];
    if (socket_cacache_dir(cache_dir, sizeof(cache_dir)) != 0)
        return -1;
    if (scache_verify_no_symlinks(cache_dir) != 0)
        return -1;

    char index_path[1024];
    if (scache_index_path(cache_dir, key, index_path, sizeof(index_path)) != 0)
        return -1;

    uint8_t *index_data = NULL;
    size_t index_len = 0;
    if (file_io_read(index_path, &index_data, &index_len) != 0)
        return -1;

    char integrity[128];
    if (scache_parse_last_integrity((const char *)index_data, index_len,
                                     integrity, sizeof(integrity)) != 0) {
        free(index_data);
        return -1;
    }
    free(index_data);

    unsigned char content_hash[SCACHE_SHA512_LEN];
    if (scache_parse_integrity_hash(integrity, content_hash) != 0)
        return -1;

    char content_path[1024];
    if (scache_content_path_from_hash(cache_dir, content_hash,
                                       content_path, sizeof(content_path)) != 0)
        return -1;

    uint8_t *content_data = NULL;
    size_t content_len = 0;
    if (file_io_read(content_path, &content_data, &content_len) != 0)
        return -1;

    /* Verify integrity. */
    unsigned char verify_hash[SCACHE_SHA512_LEN];
    if (scache_sha512(content_data, content_len, verify_hash) != 0) {
        free(content_data);
        return -1;
    }
    if (memcmp(content_hash, verify_hash, SCACHE_SHA512_LEN) != 0) {
        free(content_data);
        return -1;
    }

    *data_out = content_data;
    *size_out = content_len;
    return 0;
}

/**
 * Store data in the cache.
 *
 * Writes the content file and appends an index entry.
 *
 * @param key Cache key string
 * @param data Data to cache
 * @param data_len Size of data in bytes
 * @param metadata JSON metadata string (pass "" or NULL for empty)
 * @return 0 on success, -1 on error
 */
SCACHE_UNUSED
static int cacache_put(const char *key, const uint8_t *data, size_t data_len,
                        const char *metadata) {
    char cache_dir[512];
    if (socket_cacache_dir(cache_dir, sizeof(cache_dir)) != 0)
        return -1;
    if (scache_verify_no_symlinks(cache_dir) != 0)
        return -1;

    /* Compute integrity. */
    char integrity[128];
    if (scache_integrity(data, data_len, integrity) != 0)
        return -1;

    /* Write content file. */
    char content_path[1024];
    if (scache_content_path(cache_dir, data, data_len,
                             content_path, sizeof(content_path)) != 0)
        return -1;

    if (create_parent_directories(content_path) != 0)
        return -1;

    if (write_file_atomically(content_path, data, data_len, 0644) != 0)
        return -1;

    /* Build index entry. */
    char entry[4096];
    int entry_len = scache_build_index_entry(key, integrity, data_len,
                                              metadata, entry, sizeof(entry));
    if (entry_len < 0) {
#if defined(_WIN32)
        DeleteFileA(content_path);
#else
        unlink(content_path);
#endif
        return -1;
    }

    /* Write index file (append). */
    char index_path[1024];
    if (scache_index_path(cache_dir, key, index_path, sizeof(index_path)) != 0)
        return -1;

    if (create_parent_directories(index_path) != 0)
        return -1;

    FILE *fp = fopen(index_path, "ab");
    if (!fp) {
        /* Index dir may not exist yet; try creating and retry. */
        if (create_parent_directories(index_path) == 0) {
            fp = fopen(index_path, "ab");
        }
        if (!fp) return -1;
    }

    size_t written = fwrite(entry, 1, (size_t)entry_len, fp);
    fclose(fp);

    if (written != (size_t)entry_len)
        return -1;

    return 0;
}

/**
 * Remove a cache entry by key (soft delete).
 *
 * Appends an entry with integrity=null to the index file, which shadows
 * all previous entries for this key. This matches cacache's soft-delete
 * behavior — content files are left for garbage collection.
 *
 * @param key Cache key string
 * @return 0 on success, -1 on error
 */
SCACHE_UNUSED
static int cacache_remove(const char *key) {
    char cache_dir[512];
    if (socket_cacache_dir(cache_dir, sizeof(cache_dir)) != 0)
        return -1;
    if (scache_verify_no_symlinks(cache_dir) != 0)
        return -1;

    char index_path[1024];
    if (scache_index_path(cache_dir, key, index_path, sizeof(index_path)) != 0)
        return -1;

    /* Build JSON entry with integrity:null (soft delete) */
    long long now_ms = (long long)time(NULL) * 1000;
    char escaped_key[2048];
    if (scache_json_escape(key, escaped_key, sizeof(escaped_key)) < 0)
        return -1;
    char json_entry[4096];
    snprintf(json_entry, sizeof(json_entry),
        "{\"key\":\"%s\",\"integrity\":null,\"time\":%lld,\"size\":0,\"metadata\":{}}", escaped_key, now_ms);

    /* SHA-1 of the JSON entry for the line hash */
    uint8_t sha1_hash[20];
    scache_sha1((const uint8_t *)json_entry, strlen(json_entry), sha1_hash);
    char sha1_hex[41];
    scache_hex(sha1_hash, 20, sha1_hex);

    /* Append to index file: {sha1}\t{json}\n */
    if (create_parent_directories(index_path) != 0)
        return -1;

    FILE *f = fopen(index_path, "ab");
    if (!f) return -1;
    fprintf(f, "%s\t%s\n", sha1_hex, json_entry);
    fclose(f);

    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SOCKET_CACACHE_H */

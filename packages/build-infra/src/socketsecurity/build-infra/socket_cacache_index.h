/**
 * socket_cacache_index.h — cacache index-v5 line format: build and parse.
 *
 * One unit of the socket_cacache.h umbrella: the on-disk index entry format,
 * plus the JSON escaping and SRI parsing that format needs. Format only — this
 * unit never touches the filesystem.
 *
 * Index file format (one entry per line):
 *   {sha1(json_line)}\t{json_line}\n
 *
 * json_line fields: key, integrity, time, size, metadata
 *
 * Checks that live in this unit:
 *   Key escaping         — scache_json_escape() on key + integrity
 *   Metadata validation  — always {} when no metadata is supplied
 *   Corrupt index lines  — walk lines in reverse, SHA-1 verify, skip bad lines
 */

#ifndef SOCKET_CACACHE_INDEX_H
#define SOCKET_CACACHE_INDEX_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "socketsecurity/build-infra/socket_cacache_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Escape a string for safe JSON embedding.
 * Escapes: \ → \\, " → \", control chars → \uXXXX.
 * Returns bytes written (excluding null terminator), or -1 if buffer too small.
 */
SCACHE_UNUSED
static int scache_json_escape(const char *src, char *dst, size_t dst_size) {
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0'; si++) {
        char c = src[si];
        if (c == '"' || c == '\\') {
            if (di + 2 >= dst_size) return -1;
            dst[di++] = '\\';
            dst[di++] = c;
        } else if ((unsigned char)c < 0x20) {
            if (di + 6 >= dst_size) return -1;
            di += snprintf(dst + di, dst_size - di, "\\u%04x", (unsigned char)c);
        } else {
            if (di + 1 >= dst_size) return -1;
            dst[di++] = c;
        }
    }
    if (di >= dst_size) return -1;
    dst[di] = '\0';
    return (int)di;
}

/**
 * Build a cacache index entry line.
 * Format: {sha1hex}\t{json}\n
 *
 * Returns total length written (excluding null terminator), or -1 on error.
 */
SCACHE_UNUSED
static int scache_build_index_entry(const char *key, const char *integrity,
                                     size_t data_size, const char *metadata,
                                     char *out, size_t out_size) {
    long long now_ms = (long long)time(NULL) * 1000;

    /* Escape key and integrity for safe JSON embedding */
    char esc_key[2048];
    if (scache_json_escape(key, esc_key, sizeof(esc_key)) < 0) return -1;
    char esc_integrity[256];
    if (scache_json_escape(integrity, esc_integrity, sizeof(esc_integrity)) < 0) return -1;

    char json[4096];
    int jlen;
    if (metadata && metadata[0] != '\0') {
        jlen = snprintf(json, sizeof(json),
            "{\"key\":\"%s\",\"integrity\":\"%s\",\"time\":%lld,\"size\":%lu,\"metadata\":%s}",
            esc_key, esc_integrity, now_ms, (unsigned long)data_size, metadata);
    } else {
        jlen = snprintf(json, sizeof(json),
            "{\"key\":\"%s\",\"integrity\":\"%s\",\"time\":%lld,\"size\":%lu,\"metadata\":{}}",
            esc_key, esc_integrity, now_ms, (unsigned long)data_size);
    }
    if (jlen < 0 || (size_t)jlen >= sizeof(json))
        return -1;

    unsigned char sha1_hash[SCACHE_SHA1_LEN];
    if (scache_sha1((const unsigned char *)json, (size_t)jlen, sha1_hash) != 0)
        return -1;

    char sha1_hex[SCACHE_SHA1_LEN * 2 + 1];
    scache_hex(sha1_hash, SCACHE_SHA1_LEN, sha1_hex);

    int total = snprintf(out, out_size, "%s\t%s\n", sha1_hex, json);
    if (total < 0 || (size_t)total >= out_size)
        return -1;

    return total;
}

/**
 * Parse the last valid index entry from an index file to extract the integrity string.
 * cacache uses the last entry as the authoritative one.
 * Returns 0 on success and fills integrity_out (must be >= 128 bytes).
 */
/**
 * Parse the last valid integrity from an index file.
 * Walks lines in REVERSE, verifies SHA-1 line hash, skips corrupt lines.
 * This matches Rust's read_index_entry behavior — corrupt trailing lines
 * don't block valid earlier entries.
 */
SCACHE_UNUSED
static int scache_parse_last_integrity(const char *index_data, size_t index_len,
                                        char *integrity_out, size_t integrity_size) {
    /* Walk from end to find line boundaries */
    const char *end = index_data + index_len;
    const char *line_end = end;

    while (line_end > index_data) {
        /* Find start of current line */
        const char *line_start = line_end - 1;
        while (line_start > index_data && *(line_start - 1) != '\n') {
            line_start--;
        }

        size_t line_len = (size_t)(line_end - line_start);
        /* Skip empty lines and trailing newlines */
        while (line_len > 0 && (line_start[line_len - 1] == '\n' || line_start[line_len - 1] == '\r')) {
            line_len--;
        }
        if (line_len == 0) {
            line_end = line_start;
            continue;
        }

        /* Find tab separator: {sha1hex}\t{json} */
        const char *tab = (const char *)memchr(line_start, '\t', line_len);
        if (!tab || (size_t)(tab - line_start) != 40) {
            /* No tab or wrong hash length — corrupt line, skip */
            line_end = line_start;
            continue;
        }

        /* Verify SHA-1 of JSON portion */
        const char *json_start = tab + 1;
        size_t json_len = line_len - (size_t)(json_start - line_start);

        unsigned char sha1_hash[SCACHE_SHA1_LEN];
        scache_sha1((const unsigned char *)json_start, json_len, sha1_hash);
        char sha1_hex[41];
        scache_hex(sha1_hash, SCACHE_SHA1_LEN, sha1_hex);

        /* Compare computed hash with line prefix */
        if (memcmp(sha1_hex, line_start, 40) != 0) {
            /* SHA-1 mismatch — corrupt line, skip */
            line_end = line_start;
            continue;
        }

        /* Valid line! Extract integrity value */
        /* Check for null integrity (soft delete) */
        if (strstr(json_start, "\"integrity\":null") != NULL) {
            return -1; /* Key was deleted */
        }

        const char *needle = "\"integrity\":\"";
        const char *found = strstr(json_start, needle);
        if (!found) {
            line_end = line_start;
            continue;
        }

        const char *val = found + strlen(needle);
        const char *val_end = strchr(val, '"');
        if (!val_end) {
            line_end = line_start;
            continue;
        }

        size_t vlen = (size_t)(val_end - val);
        if (vlen >= integrity_size) {
            line_end = line_start;
            continue;
        }

        memcpy(integrity_out, val, vlen);
        integrity_out[vlen] = '\0';
        return 0;
    }

    return -1; /* No valid entry found */
}

/**
 * Parse an SRI integrity string to extract the raw sha512 hash.
 * Input: "sha512-{base64}" -> 64-byte raw hash.
 * Returns 0 on success.
 */
SCACHE_UNUSED
static int scache_parse_integrity_hash(const char *integrity, unsigned char *hash_out) {
    if (strncmp(integrity, "sha512-", 7) != 0)
        return -1;

    const char *b64 = integrity + 7;
    size_t b64_len = strlen(b64);

    /* Decode base64 inline. */
    static const unsigned char b64_decode[256] = {
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255, 62,255,255,255, 63,
         52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,  0,255,255,
        255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
         15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255,
        255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
         41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    };

    size_t out_idx = 0;
    size_t i = 0;
    while (i + 3 < b64_len) {
        unsigned char a = b64_decode[(unsigned char)b64[i]];
        unsigned char b = b64_decode[(unsigned char)b64[i+1]];
        unsigned char c = b64_decode[(unsigned char)b64[i+2]];
        unsigned char d = b64_decode[(unsigned char)b64[i+3]];
        if (a == 255 || b == 255) break;

        if (out_idx < SCACHE_SHA512_LEN)
            hash_out[out_idx++] = (a << 2) | (b >> 4);
        if (c != 255 && b64[i+2] != '=' && out_idx < SCACHE_SHA512_LEN)
            hash_out[out_idx++] = (b << 4) | (c >> 2);
        if (d != 255 && b64[i+3] != '=' && out_idx < SCACHE_SHA512_LEN)
            hash_out[out_idx++] = (c << 6) | d;

        i += 4;
    }

    if (out_idx < SCACHE_SHA512_LEN)
        return -1;

    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SOCKET_CACACHE_INDEX_H */

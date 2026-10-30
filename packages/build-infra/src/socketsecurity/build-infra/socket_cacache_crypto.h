/**
 * socket_cacache_crypto.h — cacache digest and encoding primitives.
 *
 * One unit of the socket_cacache.h umbrella: the platform SHA backends plus
 * the hex, base64, and SRI-integrity encoders the other cacache units build
 * on. Include the umbrella header, not this file, unless you only need the
 * primitives.
 *
 * Three digests, three jobs: SHA-512 = content integrity (the trust boundary,
 * recomputed + memcmp'd on read); SHA-256 = index-v5 key→bucket path (npm
 * format, addressing only); SHA-1 = per-index-line verify (npm format). Only
 * SHA-512 is a trust gate — the sha256/sha1 are the npm format and must not be
 * flipped (it breaks readers for zero gain). Full rationale + the cross-repo
 * rule: docs/references/hash-algorithms.md.
 *
 * Platform crypto:
 *   - macOS: CommonCrypto (CC_SHA1, CC_SHA256, CC_SHA512)
 *   - Linux: OpenSSL (SHA1, SHA256, SHA512)
 *   - Windows: CryptoAPI (CALG_SHA1, CALG_SHA_256, CALG_SHA_512)
 */

#ifndef SOCKET_CACACHE_CRYPTO_H
#define SOCKET_CACACHE_CRYPTO_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
    #include <CommonCrypto/CommonDigest.h>
#elif defined(__linux__)
    #include <openssl/sha.h>
#elif defined(_WIN32)
    #include <windows.h>
    #include <wincrypt.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || __has_attribute(unused)
#  define SCACHE_UNUSED __attribute__((unused))
#else
#  define SCACHE_UNUSED
#endif

#define SCACHE_SHA1_LEN   20
#define SCACHE_SHA256_LEN 32
#define SCACHE_SHA512_LEN 64

/* ========================================================================
 * Platform crypto primitives
 * ======================================================================== */

SCACHE_UNUSED
static int scache_sha1(const unsigned char *data, size_t len, unsigned char *out) {
#if defined(__APPLE__)
    CC_SHA1(data, (CC_LONG)len, out);
    return 0;
#elif defined(__linux__)
    SHA1(data, len, out);
    return 0;
#elif defined(_WIN32)
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return -1;
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return -1;
    }
    if (!CryptHashData(hHash, data, (DWORD)len, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return -1;
    }
    DWORD hash_len = SCACHE_SHA1_LEN;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, out, &hash_len, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return -1;
    }
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return 0;
#else
    (void)data; (void)len; (void)out;
    return -1;
#endif
}

SCACHE_UNUSED
static int scache_sha256(const unsigned char *data, size_t len, unsigned char *out) {
#if defined(__APPLE__)
    CC_SHA256(data, (CC_LONG)len, out);
    return 0;
#elif defined(__linux__)
    SHA256(data, len, out);
    return 0;
#elif defined(_WIN32)
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return -1;
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return -1;
    }
    if (!CryptHashData(hHash, data, (DWORD)len, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return -1;
    }
    DWORD hash_len = SCACHE_SHA256_LEN;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, out, &hash_len, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return -1;
    }
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return 0;
#else
    (void)data; (void)len; (void)out;
    return -1;
#endif
}

SCACHE_UNUSED
static int scache_sha512(const unsigned char *data, size_t len, unsigned char *out) {
#if defined(__APPLE__)
    CC_SHA512(data, (CC_LONG)len, out);
    return 0;
#elif defined(__linux__)
    SHA512(data, len, out);
    return 0;
#elif defined(_WIN32)
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return -1;
    if (!CryptCreateHash(hProv, CALG_SHA_512, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return -1;
    }
    if (!CryptHashData(hHash, data, (DWORD)len, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return -1;
    }
    DWORD hash_len = SCACHE_SHA512_LEN;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, out, &hash_len, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return -1;
    }
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return 0;
#else
    (void)data; (void)len; (void)out;
    return -1;
#endif
}

/* ========================================================================
 * Hex / Base64 encoding
 * ======================================================================== */

SCACHE_UNUSED
static void scache_hex(const unsigned char *data, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) {
        snprintf(out + (i * 2), 3, "%02x", data[i]);
    }
    out[len * 2] = '\0';
}

static const char scache_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * Base64-encode data into output buffer.
 * Output must be at least ((len + 2) / 3 * 4 + 1) bytes.
 */
SCACHE_UNUSED
static void scache_base64(const unsigned char *data, size_t len, char *out) {
    size_t i, j;
    for (i = 0, j = 0; i + 2 < len; i += 3) {
        out[j++] = scache_b64_table[(data[i] >> 2) & 0x3F];
        out[j++] = scache_b64_table[((data[i] & 0x03) << 4) | ((data[i+1] >> 4) & 0x0F)];
        out[j++] = scache_b64_table[((data[i+1] & 0x0F) << 2) | ((data[i+2] >> 6) & 0x03)];
        out[j++] = scache_b64_table[data[i+2] & 0x3F];
    }
    if (i < len) {
        out[j++] = scache_b64_table[(data[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            out[j++] = scache_b64_table[((data[i] & 0x03) << 4) | ((data[i+1] >> 4) & 0x0F)];
            out[j++] = scache_b64_table[(data[i+1] & 0x0F) << 2];
        } else {
            out[j++] = scache_b64_table[(data[i] & 0x03) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
}

/**
 * Compute SRI integrity string: "sha512-{base64(sha512(data))}".
 * Output must be at least 96 bytes (7 prefix + 88 base64 + 1 null).
 */
SCACHE_UNUSED
static int scache_integrity(const unsigned char *data, size_t len, char *out) {
    unsigned char hash[SCACHE_SHA512_LEN];
    if (scache_sha512(data, len, hash) != 0)
        return -1;
    memcpy(out, "sha512-", 7);
    scache_base64(hash, SCACHE_SHA512_LEN, out + 7);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SOCKET_CACACHE_CRYPTO_H */

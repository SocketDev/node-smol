#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "socketsecurity/bin-infra/compression_constants.h"
#include "socketsecurity/bin-infra/decompressor_limits.h"
#include "socketsecurity/bin-infra/smol_segment_reader.h"
#include "socketsecurity/bin-stub-builder/update_config.h"

typedef struct {
    long long last_check;
    long long last_notification;
    const char *latest_known;
} dlx_update_check_t;

#define BIN_INFRA_STUB_DECOMPRESS_H
#define BIN_INFRA_STUB_EXE_PATH_H
#define DLX_CACHE_COMMON_H
#define UPDATE_INTEGRATION_H
#define SOCKETSECURITY_BUILD_INFRA_STDIN_REDIRECT_H

static const size_t fixture_uncompressed_size = 64 * 1024 * 1024;
static int integrity_valid = 1;
static int cache_result;
static int cache_lookups;
static int decompressed_allocation_attempts;
static int exec_calls;

static void *test_malloc(size_t size);
static int test_execve(const char *path, char *const argv[], char *const envp[]);
static int stub_zstd_decompress(const unsigned char *compressed_data,
                                size_t compressed_size,
                                unsigned char *decompressed_data,
                                size_t uncompressed_size);
static int stub_executable_path(char *buf, size_t size);
static int dlx_calculate_integrity(const unsigned char *data, size_t size,
                                   char *integrity);
static int dlx_write_to_cache(const char *cache_key, const unsigned char *data,
                              size_t size, const char *exe_path,
                              const char *integrity,
                              const dlx_update_check_t *update_check);
int smol_verify_integrity(const uint8_t *data, size_t size,
                          const uint8_t *expected_hash);
int dlx_get_cached_binary_path(const char *cache_key, uint64_t expected_size,
                               char *output_path, size_t output_path_size);
int dlx_get_cache_base_dir(char *base_dir, size_t base_dir_size);
void stub_check_for_updates(const update_config_t *config, const char *base_dir,
                            const char *cache_key, const char *pattern,
                            const char *exe_path);
void stub_filter_update_args(int *argc, char *argv[]);
void redirect_stdin_if_piped(void);

#define main macho_stub_main
#define malloc test_malloc
#define execve test_execve
#include "../src/socketsecurity/bin-stub-builder/macho_stub.c"
#undef execve
#undef malloc
#undef main

static void *test_malloc(size_t size) {
    if (size == fixture_uncompressed_size) {
        decompressed_allocation_attempts++;
        return NULL;
    }
    return malloc(size);
}

static int test_execve(const char *path, char *const argv[], char *const envp[]) {
    (void)path;
    (void)argv;
    (void)envp;
    exec_calls++;
    errno = ENOENT;
    return -1;
}

int smol_read_metadata_macho(int fd, smol_metadata_t *metadata) {
    (void)fd;
    memset(metadata, 0, sizeof(*metadata));
    metadata->compressed_size = 4;
    metadata->uncompressed_size = fixture_uncompressed_size;
    memcpy(metadata->cache_key, "0123456789abcdef", 17);
    metadata->data_offset = SMOL_CONFIG_BINARY_LEN + 1;
    return 0;
}

int smol_validate_metadata(const smol_metadata_t *metadata, size_t max_size) {
    return metadata->uncompressed_size <= max_size ? 0 : -1;
}

int smol_verify_integrity(const uint8_t *data, size_t size, const uint8_t *expected_hash) {
    (void)data;
    (void)size;
    (void)expected_hash;
    return integrity_valid ? 0 : -1;
}

int dlx_get_cached_binary_path(const char *cache_key, uint64_t expected_size,
                               char *output_path, size_t output_path_size) {
    (void)cache_key;
    (void)expected_size;
    cache_lookups++;
    if (cache_result == 0) {
        snprintf(output_path, output_path_size, "/cached/node");
    }
    return cache_result;
}

int dlx_get_cache_base_dir(char *base_dir, size_t base_dir_size) {
    snprintf(base_dir, base_dir_size, "/cache");
    return 0;
}

void stub_check_for_updates(const update_config_t *config, const char *base_dir,
                            const char *cache_key, const char *pattern,
                            const char *exe_path) {
    (void)config;
    (void)base_dir;
    (void)cache_key;
    (void)pattern;
    (void)exe_path;
}

void stub_filter_update_args(int *argc, char *argv[]) {
    (void)argc;
    (void)argv;
}

void redirect_stdin_if_piped(void) {}

static int stub_zstd_decompress(const unsigned char *compressed_data,
                                size_t compressed_size,
                                unsigned char *decompressed_data,
                                size_t uncompressed_size) {
    (void)compressed_data;
    (void)compressed_size;
    (void)decompressed_data;
    (void)uncompressed_size;
    return -1;
}

static int stub_executable_path(char *buf, size_t size) {
    (void)buf;
    (void)size;
    return -1;
}

static int dlx_calculate_integrity(const unsigned char *data, size_t size,
                                   char *integrity) {
    (void)data;
    (void)size;
    (void)integrity;
    return -1;
}

static int dlx_write_to_cache(const char *cache_key, const unsigned char *data,
                              size_t size, const char *exe_path,
                              const char *integrity,
                              const dlx_update_check_t *update_check) {
    (void)cache_key;
    (void)data;
    (void)size;
    (void)exe_path;
    (void)integrity;
    (void)update_check;
    return -1;
}

static int run_fixture(void) {
    FILE *file = tmpfile();
    assert(file != NULL);
    uint8_t fixture[SMOL_CONFIG_BINARY_LEN + 1 + 4] = {0};
    memcpy(fixture + SMOL_CONFIG_BINARY_LEN + 1, "data", 4);
    assert(fwrite(fixture, sizeof(fixture), 1, file) == 1);
    assert(fflush(file) == 0);

    char arg0[] = "stub";
    char *argv[] = {arg0, NULL};
    update_config_t config;
    update_config_init(&config);
    int fd = dup(fileno(file));
    assert(fd >= 0);
    int result = extract_and_execute(fd, "/stub", 1, argv, &config);
    if (fcntl(fd, F_GETFD) != -1) {
        close(fd);
    }
    fclose(file);
    return result;
}

int main(void) {
    cache_result = 0;
    assert(run_fixture() == 1);
    assert(cache_lookups == 1);
    assert(decompressed_allocation_attempts == 0);
    assert(exec_calls == 1);

    cache_result = -1;
    assert(run_fixture() == 1);
    assert(cache_lookups == 2);
    assert(decompressed_allocation_attempts == 1);
    assert(exec_calls == 1);

    integrity_valid = 0;
    assert(run_fixture() == 1);
    assert(cache_lookups == 2);
    assert(decompressed_allocation_attempts == 1);
    assert(exec_calls == 1);

    puts("Passed: 3 cache allocation runtime cases");
    return 0;
}

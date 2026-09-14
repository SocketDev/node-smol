import { afterAll, beforeAll, describe, expect, it } from 'vitest'

import { existsSync, promises as fs } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'

import { safeDelete } from '@socketsecurity/lib-stable/fs/safe'
import { spawnSync } from '@socketsecurity/lib-stable/process/spawn/child'

let fixtureDir: string
let fixturePath: string

beforeAll(async () => {
  fixtureDir = await fs.mkdtemp(path.join(os.tmpdir(), 'reserved-temp-file-'))
  fixturePath = path.join(fixtureDir, 'reserved-temp-file')
  const sourcePath = path.join(fixtureDir, 'reserved-temp-file.cpp')
  await fs.writeFile(
    sourcePath,
    String.raw`
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
static bool swap_on_close = false;
static std::string swap_held_path;
static void swap_reserved_path(const char* path);
#define BINJECT_TEST_AFTER_WINDOWS_RESERVATION_CLOSE(path) swap_reserved_path(path)
#else
static bool replace_directory_on_rename = false;
static std::string replaced_directory_path;
static void replace_reserved_directory(const char* path);
#define BINJECT_TEST_BEFORE_POSIX_RENAMEAT(path) replace_reserved_directory(path)
#endif
#include "socketsecurity/bin-infra/binject_file_utils.hpp"
#include "socketsecurity/bin-infra/elf_note_utils_raw_write.hpp"

#ifdef _WIN32
static void swap_reserved_path(const char* path) {
    if (!swap_on_close) return;
    if (!MoveFileExA(path, swap_held_path.c_str(), 0)) return;
    HANDLE replacement = CreateFileA(
        path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (replacement != INVALID_HANDLE_VALUE) CloseHandle(replacement);
}
#else
static void replace_reserved_directory(const char* path) {
    if (!replace_directory_on_rename) return;
    if (rename(path, replaced_directory_path.c_str()) != 0 ||
        mkdir(path, 0700) != 0) return;
    std::string replacement = std::string(path) + "/output";
    FILE* writer = fopen(replacement.c_str(), "wb");
    if (!writer) return;
    fwrite("evil", 1, 4, writer);
    fclose(writer);
}
#endif

extern "C" int create_parent_directories(const char*) { return 0; }
extern "C" int set_executable_permissions(const char*) { return 0; }
extern "C" int write_file_atomically(const char*, const unsigned char*, size_t, int) { return -1; }

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::string root = argv[1];
    std::string output = root + "/output";
    binject::reserved_temp_file first;
    if (binject::create_temp_file(output.c_str(), &first) != 0) return 3;
#ifdef _WIN32
    std::string locked_move = root + "/locked-move";
    if (MoveFileExA(first.path, locked_move.c_str(), 0)) return 15;
#endif
    FILE* writer = fopen(first.write_path, "wb");
    if (!writer || fwrite("safe", 1, 4, writer) != 4 || fclose(writer) != 0) return 4;
    if (binject::verify_file_written(&first) != BINJECT_OK) return 5;
    if (binject::atomic_rename(&first, output.c_str()) != BINJECT_OK) return 6;
    FILE* reader = fopen(output.c_str(), "rb");
    char bytes[6] = {};
    if (!reader || fread(bytes, 1, 4, reader) != 4 || fclose(reader) != 0 ||
        strcmp(bytes, "safe") != 0) return 7;

    std::string victim = root + "/victim";
    writer = fopen(victim.c_str(), "wb");
    if (!writer || fwrite("keep", 1, 4, writer) != 4 || fclose(writer) != 0) return 8;
    binject::reserved_temp_file swapped;
    if (binject::create_temp_file((root + "/swapped").c_str(), &swapped) != 0) return 9;
    if (binject::write_temp_file(&swapped,
                                 reinterpret_cast<const uint8_t*>("held"), 4) != BINJECT_OK) return 10;
    std::string held = std::string(swapped.directory) + "/held";
#ifdef _WIN32
    swap_held_path = held;
    swap_on_close = true;
#else
    if (rename(swapped.path, held.c_str()) != 0 ||
        symlink(victim.c_str(), swapped.path) != 0) return 11;
#endif
    if (binject::atomic_rename(&swapped, (root + "/bad-output").c_str()) == BINJECT_OK) return 12;
    reader = fopen(victim.c_str(), "rb");
    memset(bytes, 0, sizeof(bytes));
    if (!reader || fread(bytes, 1, 4, reader) != 4 || fclose(reader) != 0 ||
        strcmp(bytes, "keep") != 0) return 13;
#ifdef _WIN32
    DeleteFileA(held.c_str());
    _rmdir(swapped.directory);
#else
    unlink(held.c_str());
    rmdir(swapped.directory);

    binject::reserved_temp_file ancestor_swapped;
    if (binject::create_temp_file((root + "/ancestor").c_str(),
                                  &ancestor_swapped) != 0) return 16;
    if (binject::write_temp_file(
            &ancestor_swapped,
            reinterpret_cast<const uint8_t*>("bound"), 5) != BINJECT_OK) return 17;
    replaced_directory_path = std::string(ancestor_swapped.directory) + ".held";
    replace_directory_on_rename = true;
    std::string ancestor_output = root + "/ancestor-output";
    if (binject::atomic_rename(&ancestor_swapped,
                               ancestor_output.c_str()) != BINJECT_OK) return 18;
    reader = fopen(ancestor_output.c_str(), "rb");
    memset(bytes, 0, sizeof(bytes));
    if (!reader || fread(bytes, 1, 5, reader) != 5 || fclose(reader) != 0 ||
        strcmp(bytes, "bound") != 0) return 19;
    std::string replacement = std::string(ancestor_swapped.directory) + "/output";
    reader = fopen(replacement.c_str(), "rb");
    memset(bytes, 0, sizeof(bytes));
    if (!reader || fread(bytes, 1, 4, reader) != 4 || fclose(reader) != 0 ||
        strcmp(bytes, "evil") != 0) return 20;
    unlink(replacement.c_str());
    rmdir(ancestor_swapped.directory);
    rmdir(replaced_directory_path.c_str());
#endif

    std::string malformed = root + "/malformed-elf";
    std::vector<uint8_t> elf(64, 0);
    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    uint64_t phoff = 64;
    uint16_t phentsize = UINT16_MAX;
    uint16_t phnum = UINT16_MAX;
    memcpy(elf.data() + 32, &phoff, sizeof(phoff));
    memcpy(elf.data() + 54, &phentsize, sizeof(phentsize));
    memcpy(elf.data() + 56, &phnum, sizeof(phnum));
    std::ofstream malformed_file(malformed, std::ios::binary);
    malformed_file.write(reinterpret_cast<const char*>(elf.data()), elf.size());
    malformed_file.close();
    const uint8_t note_data[] = {1};
    std::vector<elf_note_utils::NoteEntry> notes = {
        elf_note_utils::NoteEntry("example", note_data, sizeof(note_data))
    };
    if (elf_note_utils::smol_reuse_multi_ptnote(
            malformed, root + "/malformed-output", notes) == 0) return 14;
    return 0;
}
`,
  )
  const compiler = process.env.CXX || 'c++'
  const compiled = spawnSync(
    compiler,
    [
      '-std=c++17',
      '-Ipackages/bin-infra/src',
      '-Ipackages/build-infra/src',
      sourcePath,
      '-o',
      fixturePath,
    ],
    { encoding: 'utf8', stdio: 'pipe' },
  )
  if (compiled.status !== 0) {
    throw new Error(compiled.stderr)
  }
})

afterAll(async () => {
  if (fixtureDir) {
    await safeDelete(fixtureDir)
  }
})

describe('reserved temporary files', () => {
  it('keeps writes on the reserved descriptor and rejects a swapped path', async () => {
    const runDir = await fs.mkdtemp(path.join(fixtureDir, 'run-'))
    const result = spawnSync(fixturePath, [runDir], {
      encoding: 'utf8',
      stdio: 'pipe',
    })
    expect(result.status, result.stderr).toBe(0)
    expect(existsSync(path.join(runDir, 'bad-output'))).toBe(false)
  })
})

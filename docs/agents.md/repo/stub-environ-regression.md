# Stub environ regression

The binary stubs must hand the child process the **live** `environ`, never the
`envp` captured at `main()`. The C fix is in place and is pinned by two tests
under `packages/bin-stub-builder/test/`: `stub_environ_test.c` (behavioural,
run through `c-unit-tests.test.mts`) and `stub-environ-invariants.test.mts`
(source scan). This page records their design. Tracked by node-smol#2.

## The bug this guards against

A packed binary `setenv()`s `SMOL_STUB_PATH` and `SMOL_CACHE_KEY`, then
`execve()`s the extracted inner node. `setenv()` makes glibc and musl
reallocate `environ`, so a pointer captured as `main`'s `envp` goes stale. A
child exec'd with that stale pointer receives the original environment without
the SMOL variables.

The consequence is severe and quiet. Without `SMOL_STUB_PATH`, the inner node's
bootstrap falls back to its own path, which carries no VFS payload, because the
payload lives in the outer stub. `hasVFS()` returns false, argv is never
rewritten, and node parses application flags as its own, emitting
`bad option: --service`. The binary degrades to bare node while still exiting
like a working build on any path that takes no flags.

## Current state of the fix

All three stubs under
`packages/bin-stub-builder/src/socketsecurity/bin-stub-builder/` are correct:

| Stub           | How it is correct                                                                                                                     |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `elf_stub.c`   | declares `extern char **environ;`, execs with `environ` at both sites, `main` no longer takes `envp`                                  |
| `macho_stub.c` | same shape, both exec sites                                                                                                           |
| `pe_stub.c`    | `CreateProcessA(..., NULL, ...)` passes NULL for `lpEnvironment`, which inherits the current environment; `envp` is explicitly unused |

Before the tests below existed, a search for `SMOL_STUB_PATH` across the repo
returned only those three sources, a node patch, and `bootstrap.js` — no test
referred to it, so a refactor could reintroduce the stale pointer with every
suite still green. The two tests in `packages/bin-stub-builder/test/` close
that hole.

## Test design

Extend the existing C harness rather than adding a new framework:

- `packages/bin-stub-builder/test/Makefile` compiles each entry of `TESTS` into
  `build/$(BUILD_MODE)/$(PLATFORM_ARCH)/out/`.
- `packages/bin-stub-builder/test/update_config_test.c` is the model, using the
  minunit-style framework from `bin-infra/test.h`.
- `packages/bin-stub-builder/test/c-unit-tests.test.mts` builds and runs the
  binaries through vitest and greps stdout for `Passed: N` and `Failed: N`, so
  any new test must emit that same summary format.

### Behavioural half

`stub_environ_test.c` is a self-re-exec test, which exercises a real `execve`
without needing Docker:

1. Invoked with a child flag, the program prints `getenv("SMOL_STUB_PATH")` or
   a sentinel, then exits 0.
2. As parent, it captures `environ`, sets a marker via `setenv()` along with
   enough padding variables to force a reallocation, forks, and execs itself
   with `environ`, reading the child's stdout through a pipe.
3. It asserts the marker arrived.

That single assertion holds on every platform and goes red on Linux the moment
a stale pointer comes back.

**Do not assert that the stale-`envp` path fails.** Whether the startup block
still aliases the live one is the libc's choice, and it is not stable even
within one platform. Measured on a darwin arm64 machine while building this
test, the startup pointer was a stack address and the live one was heap, so the
child exec'd with the startup pointer saw `SMOL_STUB_PATH` unset, the same
result glibc gives. An assertion either way is a coin flip. Run the stale path
and print its outcome as informational output; never assert on it.

### Source-invariant half

The behavioural half depends on libc behaviour, so pair it with a portable
assertion that reads the three stub sources and requires:

- no `execve(` call passes `envp`
- `elf_stub.c` and `macho_stub.c` each declare `extern char **environ;`

Blank out comments **and string-literal contents** before scanning, keeping
every other character in place so offsets stay usable.
`DEBUG_LOG("Calling execve()...\n")` in `elf_stub.c` matches a naive `execve(`
search and will make the scan red for the wrong reason otherwise. Give the
scanner a positive control that asserts it still flags
`execve(output_path, argv, envp)`, so it can never quietly degrade into a
no-op.

### Portability note

Declare `_XOPEN_SOURCE 700` alongside `_POSIX_C_SOURCE 200809L`. glibc guards
`realpath()` behind the X/Open extension, so a POSIX-only declaration compiles
on Apple's libc and fails every Linux runner with an implicit-declaration
warning.

## Acceptance

A regression test nobody has watched fail is not yet a regression test, so
both halves were deliberately broken and watched go red before this page
stopped calling the test missing. Reverting one `elf_stub.c` exec site to
`envp` turned two scan cases red by name; pointing the behavioural test's
asserted exec at the startup pointer made the child report
`SMOL_STUB_PATH=<unset>` on darwin arm64. Both edits were then restored.

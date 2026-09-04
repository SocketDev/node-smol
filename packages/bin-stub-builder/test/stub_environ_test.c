/**
 * Stub environ regression tests
 *
 * The stubs setenv() SMOL_STUB_PATH and SMOL_CACHE_KEY and then exec the
 * extracted node. setenv() grows the environment block, and glibc and musl
 * reallocate it as it grows, so the envp pointer handed to main() can end up
 * addressing the block libc walked away from. A child exec'd with that pointer
 * receives an environment without the SMOL variables: the inner node then finds
 * no VFS payload, never rewrites argv, and the packed binary quietly degrades
 * to bare node.
 *
 * These tests exec a fresh copy of this program and read back what the child
 * actually sees, pinning the rule the stubs follow -- exec with the live
 * `environ`.
 *
 * The exec that uses the startup pointer is reported, never asserted. Apple's
 * libc keeps the old block aliased to the new one, so that exec succeeds on
 * macOS and fails on Linux, and an assertion either way would be red on one
 * platform for reasons that have nothing to do with a regression.
 *
 * Build: cd test && make
 * Run: ./build/<mode>/<platform-arch>/out/stub_environ_test
 */

/* Ask for the POSIX 2008 surface (fork, execve, pipe, setenv) before any
 * header is pulled in, because -std=c11 hides it. glibc guards realpath()
 * behind the X/Open extension instead, so ask for that too. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include the test framework. */
#include "../../bin-infra/src/socketsecurity/bin-infra/test.h"

#ifdef _WIN32

int main(void) {
    TEST_SUITE("Stub environ regression");
    printf("  Skipped: these tests need POSIX fork/execve. The Windows stub\n");
    printf("  passes NULL for lpEnvironment, which CreateProcessA reads as\n");
    printf("  \"inherit the current environment\"; the source check for that\n");
    printf("  lives in stub-environ-invariants.test.mts.\n");
    return TEST_REPORT();
}

#else

#include <errno.h>
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

/* The live environment block. This is the pointer the stubs exec with, and it
 * is the one libc keeps current across setenv(). */
extern char **environ;

/* The two variables the stubs hand to the extracted node. */
#define STUB_PATH_NAME "SMOL_STUB_PATH"
#define CACHE_KEY_NAME "SMOL_CACHE_KEY"
#define STUB_PATH_VALUE "/tmp/node-smol-live-environ-stub"
#define CACHE_KEY_VALUE "0123456789abcdef"

/* What the child prints for a variable it cannot see, which is exactly what a
 * child exec'd with a stale environment pointer reports. */
#define UNSET_REPORT "<unset>"

/* Switches this program into child mode. */
#define CHILD_FLAG "--report-smol-env"

/* setenv() adds one slot at a time, and libc reallocates the block as it
 * grows. Padding makes that growth big enough to move the block. */
#define PADDING_VARS 64

/* Room for the child's two-line report. */
#define REPORT_MAX 4096

/* Absolute path to this executable, used to exec a fresh copy of it. */
static char self_path[PATH_MAX];

/* The environment block as main() received it, before any setenv() call. */
static char **startup_environ = NULL;

/**
 * Child mode: print what this process can see of the two SMOL variables.
 */
static int report_child_environment(void) {
    const char *stub_path = getenv(STUB_PATH_NAME);
    const char *cache_key = getenv(CACHE_KEY_NAME);
    printf(STUB_PATH_NAME "=%s\n", stub_path ? stub_path : UNSET_REPORT);
    printf(CACHE_KEY_NAME "=%s\n", cache_key ? cache_key : UNSET_REPORT);
    fflush(stdout);
    return 0;
}

/**
 * Exec a fresh copy of this program in child mode with `child_environ` as its
 * environment, and copy everything it prints into `report`.
 *
 * Returns true when the child ran and exited 0.
 */
static bool run_child_with_environment(char *const child_environ[], char *report,
                                       size_t report_size) {
    int pipe_fds[2];
    report[0] = '\0';
    if (pipe(pipe_fds) != 0) {
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    if (pid == 0) {
        /* Point stdout at the pipe, then hand the process over to a fresh copy
         * of this program carrying the environment under test. */
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipe_fds[1]);
        char *child_argv[] = {self_path, (char *)CHILD_FLAG, NULL};
        execve(self_path, child_argv, child_environ);
        _exit(127);
    }
    close(pipe_fds[1]);
    size_t used = 0;
    while (used + 1 < report_size) {
        ssize_t got = read(pipe_fds[0], report + used, report_size - used - 1);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (got == 0) {
            break;
        }
        used += (size_t)got;
    }
    report[used] = '\0';
    close(pipe_fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * Copy the value the child reported for `name` into `value`.
 *
 * Returns false when the child printed no line for that variable.
 */
static bool read_reported_value(const char *report, const char *name, char *value,
                                size_t value_size) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", name);
    size_t needle_len = strlen(needle);
    const char *line = report;
    while (line != NULL && *line != '\0') {
        if (strncmp(line, needle, needle_len) == 0) {
            const char *start = line + needle_len;
            const char *end = strchr(start, '\n');
            size_t length = (end != NULL) ? (size_t)(end - start) : strlen(start);
            if (length >= value_size) {
                length = value_size - 1;
            }
            memcpy(value, start, length);
            value[length] = '\0';
            return true;
        }
        line = strchr(line, '\n');
        if (line != NULL) {
            line++;
        }
    }
    return false;
}

/**
 * Set the two SMOL markers plus enough padding variables to make libc grow
 * (and on glibc and musl, move) the environment block.
 */
static bool set_marker_environment(void) {
    for (int index = 0; index < PADDING_VARS; index++) {
        char name[64];
        snprintf(name, sizeof(name), "SMOL_ENVIRON_PADDING_%02d", index);
        if (setenv(name, "padding", 1) != 0) {
            return false;
        }
    }
    if (setenv(STUB_PATH_NAME, STUB_PATH_VALUE, 1) != 0) {
        return false;
    }
    return setenv(CACHE_KEY_NAME, CACHE_KEY_VALUE, 1) == 0;
}

/**
 * Control: a child exec'd with an empty environment reports both markers
 * missing. Without this the tests below could pass because the harness reads
 * the parent's environment rather than the child's.
 */
TEST(child_reports_unset_for_an_empty_environment) {
    char *empty_environ[] = {NULL};
    char report[REPORT_MAX];
    char value[PATH_MAX];
    ASSERT(run_child_with_environment(empty_environ, report, sizeof(report)),
           "child exec with an empty environment failed");
    ASSERT(read_reported_value(report, STUB_PATH_NAME, value, sizeof(value)),
           "child printed no " STUB_PATH_NAME " line");
    ASSERT_STR_EQ(UNSET_REPORT, value);
    ASSERT(read_reported_value(report, CACHE_KEY_NAME, value, sizeof(value)),
           "child printed no " CACHE_KEY_NAME " line");
    ASSERT_STR_EQ(UNSET_REPORT, value);
    return TEST_PASS;
}

TEST(live_environ_carries_stub_path) {
    char report[REPORT_MAX];
    char value[PATH_MAX];
    ASSERT(run_child_with_environment(environ, report, sizeof(report)),
           "child exec with the live environ failed");
    ASSERT(read_reported_value(report, STUB_PATH_NAME, value, sizeof(value)),
           "child printed no " STUB_PATH_NAME " line");
    ASSERT_STR_EQ(STUB_PATH_VALUE, value);
    return TEST_PASS;
}

TEST(live_environ_carries_cache_key) {
    char report[REPORT_MAX];
    char value[PATH_MAX];
    ASSERT(run_child_with_environment(environ, report, sizeof(report)),
           "child exec with the live environ failed");
    ASSERT(read_reported_value(report, CACHE_KEY_NAME, value, sizeof(value)),
           "child printed no " CACHE_KEY_NAME " line");
    ASSERT_STR_EQ(CACHE_KEY_VALUE, value);
    return TEST_PASS;
}

/**
 * Exec the child with the pointer main() started with and print what arrived.
 *
 * glibc and musl move the block, so that child usually sees neither marker;
 * Apple's libc keeps the old block aliased, so it usually sees both. Both
 * outcomes are normal for the platform, which is why this is reported and
 * never asserted.
 */
static void report_startup_environ_outcome(void) {
    char report[REPORT_MAX];
    char value[PATH_MAX];
    printf(COLOR_BOLD "\n=== Informational: exec with the startup environ pointer ===\n" COLOR_RESET);
    printf("  live environ:    %p\n", (void *)environ);
    printf("  startup environ: %p\n", (void *)startup_environ);
    if (startup_environ == NULL) {
        printf("  no startup pointer to try\n");
        return;
    }
    if (!run_child_with_environment(startup_environ, report, sizeof(report))) {
        printf("  child exec failed\n");
        return;
    }
    if (!read_reported_value(report, STUB_PATH_NAME, value, sizeof(value))) {
        printf("  child printed no " STUB_PATH_NAME " line\n");
        return;
    }
    printf("  child saw " STUB_PATH_NAME "=%s\n", value);
    printf("  Not asserted: libc decides whether the startup block still\n");
    printf("  aliases the live one.\n");
}

int main(int argc, char *argv[], char *envp[]) {
    startup_environ = envp;

    if (argc > 1 && strcmp(argv[1], CHILD_FLAG) == 0) {
        return report_child_environment();
    }

    if (argv[0] == NULL || realpath(argv[0], self_path) == NULL) {
        fprintf(stderr, "stub_environ_test: cannot resolve this executable from argv[0]\n");
        return 1;
    }

    TEST_SUITE("Stub environ regression");

    if (!set_marker_environment()) {
        fprintf(stderr, "stub_environ_test: setenv failed while preparing the environment\n");
        return 1;
    }

    RUN_TEST(child_reports_unset_for_an_empty_environment);
    RUN_TEST(live_environ_carries_stub_path);
    RUN_TEST(live_environ_carries_cache_key);

    report_startup_environ_outcome();

    return TEST_REPORT();
}

#endif /* _WIN32 */

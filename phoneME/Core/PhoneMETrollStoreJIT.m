#import <TargetConditionals.h>

#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

extern int csops(pid_t pid,
                 unsigned int operations,
                 void *user_address,
                 size_t user_size);
extern int ptrace(int request, pid_t pid, caddr_t address, int data);
extern char **environ;

#define PHONEME_CS_OPS_STATUS 0
#define PHONEME_CS_KILL 0x00000200
#define PHONEME_CS_DEBUGGED 0x10000000
#define PHONEME_PT_TRACE_ME 0

static const char *const kPhoneMEJITChildArgument =
    "--phoneme-trollstore-jit-child";
static atomic_bool gPhoneMETrollStoreBootstrapStarted = false;

static bool phoneme_process_allows_unsigned_executable_pages(void) {
    int flags = 0;
    if (csops(getpid(), PHONEME_CS_OPS_STATUS, &flags, sizeof(flags)) != 0) {
        return false;
    }

    // Match the original TrollStore auto-JIT build behavior: attached processes
    // are CS_DEBUGGED, while TrollStore/AppSync-style launch environments can
    // instead run without CS_KILL.
    return (flags & PHONEME_CS_DEBUGGED) != 0 ||
           (flags & PHONEME_CS_KILL) == 0;
}

// Queried through dlsym by Core. This function must never test JIT by jumping
// into unsigned code: on A12+ the kernel can terminate the process at the first
// instruction even when mmap/mprotect appeared to succeed.
__attribute__((used, visibility("default")))
int32_t phoneme_platform_jit_status(void) {
#if defined(PHONEME_INTERPRETER_ONLY) && PHONEME_INTERPRETER_ONLY
    return 0;
#else
    if (phoneme_process_allows_unsigned_executable_pages()) {
        return 1;
    }
#if defined(PHONEME_TROLLSTORE_BUILD) && PHONEME_TROLLSTORE_BUILD
    // This is the launch-time bootstrap used by the earlier TrollStore builds.
    // A successful spawn means the dedicated PT_TRACE_ME helper was launched;
    // keep the same readiness contract so the JIT artifact does not require a
    // manual Settings action after every launch.
    if (atomic_load_explicit(&gPhoneMETrollStoreBootstrapStarted,
                             memory_order_acquire)) {
        return 1;
    }
#endif
    return 0;
#endif
}

#if defined(PHONEME_TROLLSTORE_BUILD) && PHONEME_TROLLSTORE_BUILD && \
    !(defined(PHONEME_INTERPRETER_ONLY) && PHONEME_INTERPRETER_ONLY)

// Restore the original self-bootstrap from e400b24. This executes before the
// SwiftUI lifecycle, so the TrollStore JIT build starts ready without requiring
// the user to visit Settings or invoke the external enable-jit URL manually.
__attribute__((constructor, used, visibility("default")))
void phoneme_trollstore_jit_bootstrap_constructor(void) {
    int argc = *_NSGetArgc();
    char **argv = *_NSGetArgv();

    if (argc > 1 && argv != NULL && argv[1] != NULL &&
        strcmp(argv[1], kPhoneMEJITChildArgument) == 0) {
        const int result = ptrace(PHONEME_PT_TRACE_ME, 0, NULL, 0);
        _exit(result == 0 ? 0 : 1);
    }

    setenv("PHONEME_TROLLSTORE_JIT_PACKAGE", "1", 0);

    if (argv == NULL || argv[0] == NULL || argv[0][0] == '\0') {
        return;
    }

    char *child_argv[] = {
        argv[0],
        (char *)kPhoneMEJITChildArgument,
        NULL,
    };
    pid_t child_pid = 0;
    const int spawn_result = posix_spawn(
        &child_pid,
        argv[0],
        NULL,
        NULL,
        child_argv,
        environ);
    if (spawn_result == 0) {
        atomic_store_explicit(&gPhoneMETrollStoreBootstrapStarted,
                              true,
                              memory_order_release);
    }
}

#endif
#endif

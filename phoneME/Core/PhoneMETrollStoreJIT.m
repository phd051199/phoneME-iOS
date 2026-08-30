#import <TargetConditionals.h>

#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

extern int csops(pid_t pid,
                 unsigned int operations,
                 void *user_address,
                 size_t user_size);

#define PHONEME_CS_OPS_STATUS 0
#define PHONEME_CS_DEBUGGED 0x10000000

static bool phoneme_process_allows_unsigned_executable_pages(void) {
    int flags = 0;
    if (csops(getpid(), PHONEME_CS_OPS_STATUS, &flags, sizeof(flags)) != 0) {
        return false;
    }

    // On stock A12+ iOS, merely being a TrollStore platform/no-sandbox app is
    // not proof that unsigned executable pages are usable. In particular,
    // CS_KILL may be absent while execution of freshly generated ARM64 still
    // triggers AMFI termination. TrollStore's supported JIT flow attaches to
    // the *target app* with PT_ATTACHEXC and leaves that process CS_DEBUGGED.
    // Requiring CS_DEBUGGED avoids false-positive JIT readiness and lets the
    // VM safely stay in interpreter mode until TrollStore has really enabled
    // JIT for this process.
    return (flags & PHONEME_CS_DEBUGGED) != 0;
}

// Queried through dlsym by Core. This function must never test JIT by jumping
// into unsigned code: on A12+ the kernel can terminate the process at the first
// instruction even when mmap/mprotect appeared to succeed.
__attribute__((used, visibility("default")))
int32_t phoneme_platform_jit_status(void) {
    if (phoneme_process_allows_unsigned_executable_pages()) {
        return 1;
    }
    // Never infer JIT permission from a successful child spawn. posix_spawn()
    // only proves that the helper process was created; PT_TRACE_ME can still
    // fail. Core deliberately trusts this status without executing a probe on
    // A12+, so a false positive here can turn into an immediate AMFI kill when
    // generated ARM64 code is entered. csops is the source of truth.
    return 0;
}

#if defined(PHONEME_TROLLSTORE_BUILD) && PHONEME_TROLLSTORE_BUILD

// Package marker only. JIT itself must be enabled by TrollStore's supported
// external attach flow (the UI opens apple-magnifier://enable-jit). Do not try
// to self-bootstrap with a PT_TRACE_ME child: tracing the child does not mark
// this parent process CS_DEBUGGED and can create a dangerous false positive on
// A12+ devices such as iPhone XR/XS.
__attribute__((constructor, used, visibility("default")))
void phoneme_trollstore_jit_bootstrap_constructor(void) {
    setenv("PHONEME_TROLLSTORE_JIT_PACKAGE", "1", 0);
}

#endif
#endif

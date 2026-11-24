#ifndef DM_STALL_TIMING_H
#define DM_STALL_TIMING_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

// Opt-in for a bounded diagnostic run. No clock reads when disabled.
static int dm_stall_timing;
__attribute__((constructor)) static void dm_stall_configure(void) {
    const char *value = getenv("DM_STALL_TIMING");
    dm_stall_timing = value && value[0] == '1' && !value[1];
}
typedef struct {
    const char *stage;
    uint64_t start, detail;
} dm_stall_span;
static inline void dm_stall_end(dm_stall_span *span) {
    if (!span->start) return;
    int saved_errno = errno;
    uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (end - span->start < 50000000ull) { errno = saved_errno; return; }
    uint64_t thread = 0;
    pthread_threadid_np(NULL, &thread);
    LOG("[stall] uptime %.3f thread %llu %s %.1f ms detail %llu",
        end / 1e9, (unsigned long long)thread, span->stage,
        (end - span->start) / 1e6, (unsigned long long)span->detail);
    errno = saved_errno;
}
#define DM_STALL_SCOPE(label, value) \
    __attribute__((cleanup(dm_stall_end))) dm_stall_span dm_span = { \
        (label), dm_stall_timing ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0, (value) }

#endif

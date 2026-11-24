// CPU-only tests of use-aware finalized texture retention. No Metal device or
// command queue is created. NSObject stand-ins exercise owner/pin/parent lifetime.
// Upstream behavior: seedds/nxnative 034e91a8a64a78fb09173f9d23750c900af52b83.
#import <Foundation/Foundation.h>
#include "../nvn/nvn_owner.h"
#include <stdatomic.h>
#include <stdio.h>

static int checks, failures;
#define CHECK(c, label) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, label); } } while (0)
static _Atomic uint64_t frame = 1, submission = 1, notes;
static NSMutableArray *deferred;
void own_hook_drop(id o, int resident) { (void)resident; [deferred addObject:o]; }
void own_hook_unheld(const void *o) { (void)o; }
void own_hook_changed(uint64_t serial) { (void)serial; atomic_fetch_add(&notes, 1); }
uint64_t own_hook_submission(void) { return atomic_load(&submission); }
uint64_t own_hook_frame(void) { return atomic_load(&frame); }

@interface OwnerTestView : NSObject
@property(strong) id parentTexture;
@end
@implementation OwnerTestView
@end

static uint32_t ghost_count(void) { pthread_mutex_lock(&g_smu); uint32_t n = own_stats_locked().ghost; pthread_mutex_unlock(&g_smu); return n; }
static uint64_t puts_count(void) { pthread_mutex_lock(&g_smu); uint64_t n = own_stats_locked().ghost_puts; pthread_mutex_unlock(&g_smu); return n; }
static int is_ghost(uint64_t key) {
    __block int found = 0; pthread_mutex_lock(&g_smu);
    own_each_locked(^(uint64_t k, id o, int ghost) { (void)o; if (k == key && ghost) found = 1; });
    pthread_mutex_unlock(&g_smu); return found;
}
static int held(id o) { pthread_mutex_lock(&g_smu); int h = entry_holds((__bridge void *)o); pthread_mutex_unlock(&g_smu); return h; }
static void valid(void) { char why[256] = ""; pthread_mutex_lock(&g_smu); int ok = own_check_locked(why, sizeof why); pthread_mutex_unlock(&g_smu); CHECK(ok, why); }
static void idle(void) { atomic_fetch_add(&frame, GHOST_IDLE); }
static void drain_idle(void) { for (int i = 0; i < 4096 && ghost_count() > NGHOST; i++) own_ghost_trim(); CHECK(ghost_count() == NGHOST, "idle entries drain to NGHOST"); }
static void fill(uint64_t base, int count) { for (int i = 0; i < count; i++) tex_put(base + i, [NSObject new]); }
typedef struct { uint64_t base; int count; } Reader;
static _Atomic unsigned read_misses;
static void *resolve_worker(void *context) { @autoreleasepool {
    Reader *r = context;
    for (int pass = 0; pass < 4; pass++) for (int i = 0; i < r->count; i++)
        if (!own_resolve_ref(r->base + i)) atomic_fetch_add(&read_misses, 1);
    return NULL;
} }

int main(int argc, const char **argv) { @autoreleasepool {
    int baseline = argc > 1 && !strcmp(argv[1], "--baseline");
    setenv("DM_NVN_GHOST_LRU", baseline ? "0" : "1", 1);
    deferred = [NSMutableArray array];
    if (baseline) {
        tex_put(1000, [NSObject new]);
        for (int i = 1; i < NGHOST; i++) tex_put(1000 + i, [NSObject new]);
        CHECK(tex_get(1000) != nil, "oldest ring entry exists at capacity");
        tex_put(1000 + NGHOST, [NSObject new]);
        CHECK(!tex_get(1000) && ghost_count() == NGHOST, "baseline ignores lookup recency and keeps original ring bound");
        uint64_t before = notes; idle(); own_ghost_trim();
        CHECK(notes == before && ghost_count() == NGHOST, "LRU commit hook is a no-op under baseline switch");
        valid();
    } else {
        enum { BASE = 10000, N = 4 * NGHOST };
        fill(BASE, N);
        CHECK(ghost_count() == N, "active working set exceeds old 512-entry ring");
        uint64_t initial_puts = puts_count(); int misses = 0;
        for (int f = 0; f < 2 * GHOST_IDLE; f++) {
            frame++; submission += 25; own_ghost_trim();
            for (int i = 0; i < N; i++) misses += tex_get(BASE + i) == nil;
        }
        CHECK(!misses && puts_count() == initial_puts, "2048 repeatedly used serials incur no eviction or republish");
        pthread_t threads[4]; Reader readers[4];
        for (int i = 0; i < 4; i++) { readers[i] = (Reader){BASE + i * (N / 4), N / 4}; CHECK(!pthread_create(&threads[i], NULL, resolve_worker, &readers[i]), "start resolution reader"); }
        for (int i = 0; i < 128; i++) { tex_put(BASE + i, [NSObject new]); own_ghost_trim(); }
        for (int i = 0; i < 4; i++) CHECK(!pthread_join(threads[i], NULL), "join resolution reader");
        CHECK(!read_misses && ghost_count() == N, "concurrent lookups/replacements/trim retain every active serial");
        // Establish a deterministic use order after the concurrent exercise.
        for (int i = 0; i < N; i++) tex_get(BASE + i);
        valid();
        frame += GHOST_IDLE - 1; own_ghost_trim();
        CHECK(ghost_count() == N, "frame-idle threshold is not early");
        frame++; uint64_t n0 = notes; own_ghost_trim();
        CHECK(ghost_count() == N - GHOST_TRIM && notes - n0 == GHOST_TRIM, "commit trims at most 32 entries with one invalidation each");
        CHECK(!is_ghost(BASE) && is_ghost(BASE + N - 1), "oldest idle entries leave first");
        drain_idle(); valid();

        fill(20000, NGHOST + 64);
        CHECK(ghost_count() == NGHOST + 64, "new active set retained while old idle entries drain on puts");
        submission += GHOST_IDLE_SUBS - 1; own_ghost_trim();
        CHECK(ghost_count() == NGHOST + 64, "submission-idle threshold is not early when no presents occur");
        submission++; own_ghost_trim(); own_ghost_trim();
        CHECK(ghost_count() == NGHOST, "1024 submissions without use permits bounded trimming with frame stationary");
        valid();

        fill(30000, NGHOST + 1); uint32_t before = ghost_count();
        uint64_t original_frame = frame, original_sub = submission;
        frame = 0; submission = 0; own_ghost_trim();
        CHECK(ghost_count() == before, "counter regression cannot underflow into an idle eviction");
        frame = original_frame; submission = original_sub;
        idle(); uint64_t touched = 30000; CHECK(tex_get(touched) != nil, "refresh oldest new entry");
        drain_idle(); CHECK(is_ghost(touched), "lookup makes a ghost most recently used"); valid();
    }

    // Pins outlive cache eviction, and follow a replacement while old storage goes
    // through the replay layer's deferred-drop hook, not a direct release.
    uint64_t pin_key = 40000; id pinned = [NSObject new], replacement = [NSObject new];
    tex_put(pin_key, pinned); mtl_tex_pin(pin_key); tex_put(pin_key, replacement);
    CHECK(tex_get(pin_key) == replacement && !held(pinned) && held(replacement), "replacement updates both entry and pin");
    CHECK([deferred containsObject:pinned], "replaced object remains in deferred release");
    idle(); fill(41000, NGHOST + 1);
    if (!baseline) { idle(); drain_idle(); }
    CHECK(!is_ghost(pin_key) && tex_get(pin_key) == replacement && held(replacement), "pin still resolves after its ghost entry is evicted");
    mtl_tex_unpin(pin_key);
    CHECK(!tex_get(pin_key) && !held(replacement), "last pin releases its resolution only through owner hooks");
    valid();

    // A stale view's use-list/ring record must disappear with it, and a later
    // publication of the same serial must have exactly one new recency record.
    id parent = [OwnerTestView new]; OwnerTestView *view = [OwnerTestView new]; view.parentTexture = parent;
    uint64_t view_key = 50000; tex_put(view_key, view);
    CHECK(held(view) && held(parent), "view counts its parent");
    CHECK(own_view_drop_stale(view_key, view) && !is_ghost(view_key) && !held(parent), "stale view drops object and recency record together");
    id newer = [NSObject new]; tex_put(view_key, newer);
    CHECK(tex_get(view_key) == newer, "same serial can be published after stale-view removal");
    CHECK(!own_view_drop_stale(view_key, newer) && tex_get(view_key) == newer, "ordinary replacement is not invalidated as a stale view");
    valid();

    printf("ghost retention (%s): %d checks, %d failed; no Metal device created\n", baseline ? "baseline ring" : "use-aware", checks, failures);
    return failures != 0;
} }

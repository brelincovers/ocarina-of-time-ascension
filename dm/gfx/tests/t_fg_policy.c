// CPU-only frame-generation policy regression tests. No Metal device or game.
// clang -std=c11 -Wall -Wextra -Werror -pthread t_fg_policy.c -o /tmp/t_fg_policy
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include "../nvn/nvn_fg_policy.h"

static int checks, failures;
#define CHECK(c, why) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, why); } } while (0)

static dm_fg_admission good(void) {
    return (dm_fg_admission){
        .now_ns = 1000000000, .target_period_ns = 33333333,
        .display_interval_ns = 16666667, .cadence_tolerance_ns = 1000,
        .sample_end_ns = 980000000, .max_sample_age_ns = 100000000,
        .sample_total_ns = 23000000, .additional_gpu_ns = 8000000,
        .predicted_ready_ns = 1007000000, .deadline_ns = 1008000000,
        .max_queued_real_frames = 1, .capable = 1, .cadence_valid = 1, .deadline_valid = 1
    };
}

static void admission(void) {
    dm_fg_admission a = good();
    CHECK(dm_fg_admit(&a) == DM_FG_ADMIT, "fixed 60 Hz with measured headroom and a reserved slot");
    a.display_interval_ns = 8333333;
    CHECK(dm_fg_admit(&a) == DM_FG_ADMIT, "measured 120 Hz opportunity is not rejected because capability is above 60");
    a = good(); a.cadence_valid = 0;
    CHECK(dm_fg_admit(&a) == DM_FG_NO_CADENCE, "a capable GPU and nominal rate do not establish cadence");
    a = good(); a.display_interval_ns = 20000000;
    CHECK(dm_fg_admit(&a) == DM_FG_NO_CADENCE, "50 Hz cannot provide two uniform display slots per 30 Hz frame");
    a = good(); a.capable = 0;
    CHECK(dm_fg_admit(&a) == DM_FG_NO_CAPABILITY, "cadence cannot override missing interpolation support");
    a = good(); a.sample_total_ns = 34670000;
    CHECK(dm_fg_admit(&a) == DM_FG_GPU_BUDGET, "observed overloaded baseline is rejected before expensive work");
    a = good(); a.sample_total_ns = 31000000; a.sample_has_generation = 1;
    CHECK(dm_fg_admit(&a) == DM_FG_NO_BASE_TIMING, "combined GPU cost is not mistaken for baseline");
    a.sample_generation_measured = 1; a.sample_generation_ns = 8000000;
    CHECK(dm_fg_admit(&a) == DM_FG_ADMIT, "measured generated cost is removed exactly once before reserve");
    a.sample_generation_ns = 31000000;
    CHECK(dm_fg_admit(&a) == DM_FG_NO_BASE_TIMING, "invalid subtraction cannot create free GPU budget");
    a = good(); a.sample_has_generation = 1; a.sample_is_same_generated_path = 1;
    a.sample_total_ns = 31000000; a.additional_gpu_ns = 0;
    CHECK(dm_fg_admit(&a) == DM_FG_ADMIT, "same generated path compares measured full cost without a second reserve");
    a.sample_total_ns = 35000000;
    CHECK(dm_fg_admit(&a) == DM_FG_GPU_BUDGET, "measured overloaded generated path is still rejected");
    a.sample_total_ns = 31000000; a.additional_gpu_ns = 8000000;
    CHECK(dm_fg_admit(&a) == DM_FG_NO_BASE_TIMING, "same generated path must not double-reserve the estimate");
    a.additional_gpu_ns = 0; a.sample_has_generation = 0;
    CHECK(dm_fg_admit(&a) == DM_FG_NO_BASE_TIMING, "path annotation cannot silently bypass baseline-only reserve");
    a = good(); a.sample_generation_ns = 10000000;
    CHECK(dm_fg_admit(&a) == DM_FG_ADMIT, "stray generated timing never subtracts from a baseline-only sample");
    a = good(); a.sample_total_ns = 0;
    CHECK(dm_fg_admit(&a) == DM_FG_NO_BASE_TIMING, "missing GPU timing is not zero-cost work");
    a = good(); a.sample_end_ns = a.now_ns + 1;
    CHECK(dm_fg_admit(&a) == DM_FG_STALE_TIMING, "mixed or reversed clocks are rejected");
    a = good(); a.sample_end_ns = 1;
    CHECK(dm_fg_admit(&a) == DM_FG_STALE_TIMING, "stale light scene cannot admit a new heavy scene");
    a = good(); a.queued_real_frames = 2;
    CHECK(dm_fg_admit(&a) == DM_FG_QUEUE_BUSY, "generation does not add work ahead of queued genuine frames");
    a = good(); a.deadline_valid = 0;
    CHECK(dm_fg_admit(&a) == DM_FG_NO_DEADLINE, "recent FPS alone does not establish a presentation opportunity");
    a = good(); a.predicted_ready_ns = a.deadline_ns + 1;
    CHECK(dm_fg_admit(&a) == DM_FG_MISSED_DEADLINE, "work predicted after the slot is rejected");
    a = good(); a.deadline_ns = a.now_ns;
    CHECK(dm_fg_admit(&a) == DM_FG_MISSED_DEADLINE, "expired presentation cannot be recovered by enqueueing");
    a = good(); a.additional_gpu_ns = UINT64_MAX;
    CHECK(dm_fg_admit(&a) == DM_FG_GPU_BUDGET, "cost overflow cannot admit impossible work");
    a = good(); a.display_interval_ns = UINT64_MAX;
    CHECK(dm_fg_admit(&a) == DM_FG_NO_CADENCE, "interval overflow cannot admit impossible cadence");
}

static void ownership(void) {
    dm_fg_slot s = {0};
    CHECK(dm_fg_slot_reusable(&s), "unused slot is available");
    s.composition_submitted = 10; s.queued = 1;
    CHECK(!dm_fg_slot_reusable(&s), "composing a queued candidate owns its allocator");
    s.queued = 0; // A failed genuine present or queue discard must not imply GPU completion.
    CHECK(!dm_fg_slot_reusable(&s), "real-frame failure cannot retire unfinished composition");
    s.composition_done = 10; s.queued = 1;
    CHECK(!dm_fg_slot_reusable(&s), "completed composition remains owned by its queued candidate");
    s.queued = 0; s.reader_submitted = 12;
    CHECK(!dm_fg_slot_reusable(&s), "generated display reader still owns the slot");
    s.reader_done = 11;
    CHECK(!dm_fg_slot_reusable(&s), "an older completion cannot retire this reader");
    s.reader_done = 12;
    CHECK(dm_fg_slot_reusable(&s), "both GPU uses completed and queue ownership released");
    s.composition_submitted = 13;
    CHECK(!dm_fg_slot_reusable(&s), "next composition must wait for its own completion");
}

static _Atomic uint64_t outcome_state;
static _Atomic unsigned winners;
static _Atomic int start;
static void *finish_thread(void *arg) {
    dm_fg_outcome result = *(dm_fg_outcome *)arg;
    while (!atomic_load(&start)) sched_yield();
    if (dm_fg_finish_once(&outcome_state, 42, result)) atomic_fetch_add(&winners, 1);
    return NULL;
}

static void outcomes(void) {
    CHECK(!dm_fg_finish_once(&outcome_state, 0, DM_FG_REJECTED_LATE), "ordinary late genuine frame has no candidate outcome");
    CHECK(dm_fg_attempt_begin(&outcome_state, 41), "candidate token initialized");
    CHECK(dm_fg_finish_once(&outcome_state, 41, DM_FG_DROPPED), "dropped presentation has one terminal outcome");
    CHECK(!dm_fg_finish_once(&outcome_state, 41, DM_FG_PRESENTED), "duplicate callback cannot change a terminal result");
    CHECK(dm_fg_attempt_begin(&outcome_state, 42), "slot reused with a fresh identity");
    CHECK(!dm_fg_attempt_begin(&outcome_state, 43), "unresolved candidate cannot be overwritten");
    CHECK(!dm_fg_finish_once(&outcome_state, 41, DM_FG_PRESENTED), "old callback cannot finish new candidate");
    CHECK(!dm_fg_finish_once(&outcome_state, 42, DM_FG_PENDING), "pending is not a terminal outcome");
    pthread_t threads[8]; dm_fg_outcome terminal[8]; int made = 0;
    for (int i = 0; i < 8; i++) {
        terminal[i] = i & 1 ? DM_FG_PRESENTED : DM_FG_FAILED;
        if (pthread_create(&threads[i], NULL, finish_thread, &terminal[i])) break;
        made++;
    }
    atomic_store(&start, 1);
    for (int i = 0; i < made; i++) pthread_join(threads[i], NULL);
    CHECK(made == 8 && atomic_load(&winners) == 1, "concurrent completion paths count exactly one terminal outcome");
    CHECK(!dm_fg_attempt_begin(&outcome_state, 42), "old identity cannot be reused after completion");
    CHECK(!dm_fg_attempt_begin(&outcome_state, DM_FG_ATTEMPT_MAX + 1), "identity overflow is rejected");
}

static void latency(void) {
    uint64_t ns = 123;
    CHECK(!dm_fg_latency_ns(0, 100, &ns) && ns == 123, "missing enqueue timestamp remains unmeasured");
    CHECK(!dm_fg_latency_ns(100, 0, &ns) && ns == 123, "unpresented/dropped drawable has no latency sample");
    CHECK(!dm_fg_latency_ns(100, 99, &ns) && ns == 123, "out-of-order timestamps are not negative latency");
    CHECK(dm_fg_latency_ns(100, 150, &ns) && ns == 50, "enqueue-to-screen duration is measured without claiming input latency");
}

static void pair_timing(void) {
    const uint64_t now=1000000000, frame=33333333, tol=1000;
    dm_fg_cadence c={7,1008000000,16666667}; dm_fg_pair p={0};
    CHECK(dm_fg_pair_plan(&c,now,now+frame,0,frame,tol,&p),"60 Hz pair can be reserved on observed ticks");
    CHECK(p.generated_ns==1024666667 && p.real_ns==1041333334,"60 Hz pair uses consecutive ticks in temporal order");
    CHECK(dm_fg_pair_valid(&p,&c,now,tol),"unchanged epoch and phase validate the reservation");
    dm_fg_cadence later=c; later.next_tick_ns+=c.interval_ns;
    CHECK(dm_fg_pair_valid(&p,&later,1010000000,tol),"a newer observation of the same phase does not retarget the pair");
    CHECK(!dm_fg_pair_valid(&p,&later,p.generated_ns,tol),"expired generated target is rejected rather than moved later");
    CHECK(dm_fg_pair_real_target(&p,0,now)==0,"rejected generation keeps ordinary real-frame fallback");
    CHECK(dm_fg_pair_real_target(&p,1,now)==p.real_ns,"accepted pair retains its absolute genuine target");
    CHECK(dm_fg_pair_real_target(&p,1,p.real_ns)==0,"late genuine frame falls back immediately instead of being dropped");
    later=c; later.epoch++;
    CHECK(!dm_fg_pair_valid(&p,&later,now,tol),"same-Hz display change invalidates the old epoch");
    later=c; later.next_tick_ns+=c.interval_ns/2;
    CHECK(!dm_fg_pair_valid(&p,&later,now,tol),"phase shift invalidates a pair even if refresh is unchanged");
    later=c; later.interval_ns=8333333;
    CHECK(!dm_fg_pair_valid(&p,&later,now,tol),"refresh transition invalidates old targets");
    c=(dm_fg_cadence){8,1004000000,8333333};
    CHECK(dm_fg_pair_plan(&c,now,now+frame,0,frame,tol,&p),"120 Hz pair can be reserved on observed ticks");
    CHECK(p.real_ns-p.generated_ns==2*c.interval_ns,"30-to-60 pair on120 Hz separates by two ticks, not one");
    uint64_t previous=p.real_ns;
    c.next_tick_ns+=4*c.interval_ns;
    CHECK(dm_fg_pair_plan(&c,now+frame,now+2*frame,previous,frame,tol,&p)&&p.generated_ns>previous,"next pair follows the preceding real frame chronologically");
    CHECK(!dm_fg_pair_plan(&c,now+frame,now+2*frame,p.real_ns,frame,tol,&p),"overlapping pair cannot overtake the previous real frame");
    c=(dm_fg_cadence){9,1006000000,11111111};
    CHECK(!dm_fg_pair_plan(&c,now,now+frame,0,frame,tol,&p),"90 Hz has no uniform16.67 ms pair spacing");
    c=(dm_fg_cadence){9,1006000000,20000000};
    CHECK(!dm_fg_pair_plan(&c,now,now+frame,0,frame,tol,&p),"50 Hz has no uniform30-to-60 opportunity");
    c=(dm_fg_cadence){9,UINT64_MAX-10,10};
    CHECK(!dm_fg_pair_plan(&c,UINT64_MAX-20,UINT64_MAX-1,0,40,1,&p),"pair-target overflow is rejected");
}

static void history_and_cost(void) {
    CHECK(dm_fg_history_reset(2,1,0,1),"captured previous colour without previous encode requires reset");
    CHECK(!dm_fg_history_reset(3,2,2,1),"consecutive capture and interpolation preserve SDK history");
    CHECK(dm_fg_history_reset(4,2,2,1),"skipped source frame invalidates temporal correspondence");
    CHECK(dm_fg_history_reset(5,4,3,1),"budget skip cannot claim the last capture was the last interpolation");
    CHECK(dm_fg_history_reset(5,4,4,0),"invalid depth/camera history requires reset");
    CHECK(dm_fg_history_reset(0,UINT64_MAX,UINT64_MAX,1),"frame-number wrap does not create valid history");
    dm_fg_cost c={0};
    CHECK(!dm_fg_cost_fresh(&c,1000,100),"absent GPU phase timing remains unknown");
    CHECK(!dm_fg_cost_record(&c,1,0,20),"missing GPU start is not a cost measurement");
    CHECK(!dm_fg_cost_record(&c,1,30,20),"reversed GPU timestamps are rejected");
    CHECK(dm_fg_cost_record(&c,1,900,950)&&c.duration_ns==50,"actual completed scope records its own duration");
    CHECK(dm_fg_cost_fresh(&c,1000,100),"recent completed scope is measurable");
    CHECK(!dm_fg_cost_fresh(&c,1100,100),"old phase timing is explicitly stale");
    CHECK(!dm_fg_cost_fresh(&c,940,100),"future completion cannot be fresh");
    CHECK(!dm_fg_cost_record(&c,2,800,900)&&c.attempt==1,"out-of-order feedback does not replace a newer sample");
    uint64_t ready=123;
    CHECK(!dm_fg_remaining_ready(1000,1100,50,0,&ready)&&ready==123,"an unmeasured remainder cannot masquerade as predicted readiness");
    CHECK(dm_fg_remaining_ready(1000,1100,50,1,&ready)&&ready==1150,"backlogged queue frontier precedes measured remaining work");
    CHECK(dm_fg_remaining_ready(1000,900,50,1,&ready)&&ready==1050,"completed upstream GPU work is not charged again");
    CHECK(!dm_fg_remaining_ready(1000,UINT64_MAX-10,50,1,&ready),"readiness overflow remains unknown");
}

static void camera_shape(void) {
    CHECK(dm_fg_camera_shape_verified(1920,1080),"original 16:9 scene keeps frame-generation eligibility");
    CHECK(dm_fg_camera_shape_verified(2560,1440),"original 16:9 reconstructed canvas keeps eligibility");
    CHECK(dm_fg_camera_shape_verified(683,384),"DRS integer rounding does not disable the original aspect");
    CHECK(dm_fg_camera_shape_verified(953,536),"captured rounded DRS scene remains eligible");
    CHECK(!dm_fg_camera_shape_verified(1920,1200),"16:10 requires verified camera metadata before frame generation");
    CHECK(!dm_fg_camera_shape_verified(1920,1344),"rounded taller canvas is not treated as the original camera");
    CHECK(!dm_fg_camera_shape_verified(1920,1440),"4:3 requires verified camera metadata");
    CHECK(!dm_fg_camera_shape_verified(1920,1104),"nearest supported taller output is outside pixel-rounding tolerance");
    CHECK(!dm_fg_camera_shape_verified(0,1080) && !dm_fg_camera_shape_verified(1920,0),"missing scene dimensions fail closed");
    CHECK(dm_fg_camera_shape_verified(UINT64_MAX,UINT64_MAX/16*9),"shape comparison does not overflow integer dimensions");
}

int main(void) {
    admission(); ownership(); outcomes(); latency(); pair_timing(); history_and_cost(); camera_shape();
    printf("frame-generation policy: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}

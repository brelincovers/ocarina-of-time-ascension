// Profiling closes at the selected frame's present, not when round-robin NVN
// queue replay temporarily visits a different frame. No Metal device is used.
// Fake counter / event objects exercise production heap ownership without a GPU.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

@interface ProfileHeap : NSObject
@property int invalidations, resolutions;
@end
@implementation ProfileHeap
- (void)invalidateCounterRange:(NSRange)range { (void)range; _invalidations++; }
- (NSData *)resolveCounterRange:(NSRange)range {
    _resolutions++; uint64_t values[GP_MAX];
    for (NSUInteger i = 0; i < range.length; i++) values[i] = 1000 + 100 * i;
    return [NSData dataWithBytes:values length:range.length * sizeof *values];
}
@end
@interface ProfileDevice : NSObject
@end
@implementation ProfileDevice
- (uint64_t)queryTimestampFrequency { return 1000000000; }
@end
@interface ProfileEvent : NSObject
@property(copy) MTLSharedEventNotificationBlock notification;
@property uint64_t value;
@property int registrations;
@end
@implementation ProfileEvent
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value block:(MTLSharedEventNotificationBlock)block {
    (void)listener; CHECK(!_notification, "a pending report keeps exclusive ownership of the heap");
    _notification = block; _value = value; _registrations++;
}
- (void)complete {
    MTLSharedEventNotificationBlock block = _notification; _notification = nil;
    CHECK(block != nil, "the sampled frame registered a completion report");
    if (block) block((id)self, _value);
}
@end
// A draw pass answers only the render encoder's timestamp selector, an upload encoder only the compute one: a stamp sent to the wrong kind throws, as Metal 4's encoders do.
@interface ProfileRender : NSObject
@property int stamps; @property NSUInteger last;
@end
@implementation ProfileRender
- (void)writeTimestampWithGranularity:(MTL4TimestampGranularity)g afterStage:(MTLRenderStages)s intoHeap:(id)h atIndex:(NSUInteger)i { (void)g; (void)s; (void)h; _stamps++; _last = i; }
@end
@interface ProfileCompute : NSObject
@property int stamps;
@end
@implementation ProfileCompute
- (void)writeTimestampWithGranularity:(MTL4TimestampGranularity)g intoHeap:(id)h atIndex:(NSUInteger)i { (void)g; (void)h; (void)i; _stamps++; }
@end

int main(void) { @autoreleasepool {
    ProfileHeap *heap = [ProfileHeap new]; ProfileEvent *event = [ProfileEvent new];
    g_gp_heap = (id)heap; g_dev = (id)[ProfileDevice new]; g_ev = (id)event;
    setenv("DM_NVN_GPUPROF", "every:1", 1);
    atomic_store(&g_cur_frame, 0); gp_begin(nil, 'C', nil); CHECK(!g_gp_state, "frame zero is not profiled");
    for (uint64_t frame = 1; frame <= 3; frame++) {
        atomic_store(&g_cur_frame, frame); gp_begin(nil, 'C', nil); gp_end(nil);
        CHECK(g_gp_state == 1 && g_gp_frame == frame && g_gp_n == 1, "a fresh sample belongs only to its selected frame");
        gp_begin(nil, 'C', nil); gp_end(nil);
        CHECK(g_gp_n == 2, "all encoders in the frame share its sample");
        ProfileCompute *foreign = [ProfileCompute new];
        atomic_store(&g_cur_frame, frame + 1); gp_begin(foreign, 'C', nil); gp_end(foreign);
        CHECK(g_gp_state == 1 && !atomic_load(&g_gp_busy) && g_gp_n == 2 && !foreign.stamps,
              "interleaved foreign-frame commands neither close nor contaminate the sample");
        gp_present_finish(frame + 1);
        CHECK(g_gp_state == 1 && !event.notification, "another frame's present cannot close the selected frame");
        atomic_store(&g_cur_frame, frame); gp_begin(nil, 'C', nil); gp_end(nil);
        CHECK(g_gp_n == 3, "selected-frame work after an interleaved queue is still sampled");
        g_fence = frame * 10; gp_present_finish(frame);
        CHECK(g_gp_state == 2 && atomic_load(&g_gp_busy), "the selected present closes the complete sample");
        CHECK(event.value == g_fence, "report waits for every committed segment of the sampled frame");
        int registrations = event.registrations;
        gp_present_finish(frame);
        CHECK(event.registrations == registrations, "a repeated present cannot schedule a duplicate report");
        atomic_store(&g_cur_frame, frame + 1); gp_begin(foreign, 'C', nil); gp_end(foreign);
        CHECK(g_gp_n == 3 && heap.invalidations == frame && !foreign.stamps, "no following frame overwrites pending timestamps");
        [event complete];
        CHECK(!atomic_load(&g_gp_busy) && heap.resolutions == frame, "completion resolves the sampled frame once and releases the heap");
    }
    // Sparse selection also closes on present, even without later encoder work.
    g_gp_every = 5; g_gp_state = 0;
    atomic_store(&g_cur_frame, 5); gp_begin(nil, 'C', nil); gp_end(nil); g_fence = 50;
    gp_present_finish(5); [event complete];
    CHECK(event.registrations == 4 && heap.resolutions == 4, "sparse and consecutive selection both report per frame");
    // An upload flush ending its own encoder inside a profiled draw pass (#531) leaves the pass's label alone; the pass's own end still stamps it.
    ProfileRender *pass = [ProfileRender new]; ProfileCompute *upload = [ProfileCompute new];
    atomic_store(&g_cur_frame, 7); gp_begin(nil, 'C', nil); CHECK(g_gp_state == 0, "the frame after a report reopens the heap");
    atomic_store(&g_cur_frame, 10); gp_begin(pass, 'R', nil); int k = g_gp_cur;
    @try { gp_end(upload); } @catch (NSException *x) { CHECK(0, "another queue's encoder got the open pass's timestamp"); }
    CHECK(upload.stamps == 0 && g_gp_cur == k, "another queue's encoder does not close the open pass");
    gp_present_finish(10);
    CHECK(g_gp_state == 1 && !event.notification, "a present cannot report an encoder whose end timestamp is still pending");
    gp_end(pass); CHECK(pass.stamps == 2 && pass.last == 2 * (NSUInteger)k + 1 && g_gp_cur < 0, "the pass's own end writes its end stamp");
    g_fence = 100; gp_present_finish(10); [event complete];
    CHECK(event.value == 100, "the closed render pass reports at its committed fence");
    // A fixed frame selection needs the same ownership rules as periodic mode.
    g_gp_every = 0; g_gp_f0 = g_gp_f1 = 14; g_gp_state = 0;
    atomic_store(&g_cur_frame, 13); gp_begin(nil, 'C', nil); CHECK(g_gp_state == 0, "fixed selection ignores earlier frames");
    atomic_store(&g_cur_frame, 14); gp_begin(nil, 'C', nil); gp_end(nil);
    atomic_store(&g_cur_frame, 15); gp_begin(upload, 'C', nil); gp_end(upload); gp_present_finish(15);
    CHECK(g_gp_state == 1 && g_gp_n == 1 && !event.notification, "fixed selection survives a foreign frame and its present");
    atomic_store(&g_cur_frame, 14); gp_begin(nil, 'C', nil); gp_end(nil); g_fence = 140;
    gp_present_finish(14); [event complete];
    CHECK(event.value == 140 && heap.resolutions == 6, "fixed selection reports all matching work on its own present");
    printf("t_gpuprof: %d checks, %d failed\n", checks, failed); return failed != 0;
} }

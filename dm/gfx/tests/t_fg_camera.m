// The frame-generation camera must match the queued GPU-visible uniform bytes.
// Like t_ubp_running, this uses the actual uniform/shadow machinery. It creates
// a Metal device and records work but never submits a frame or launches a game.
// Build with t_ubp_running's MTL_T_SRC/frameworks, substituting this source.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while(0)
enum { LEN=0x20000, CAMERA_BYTES=96*sizeof(float), BINDING_BYTES=13568 };
static uint8_t *mem;
static NvShader *shader;
static uint64_t running;

static void camera_values(float c[96],float translation) {
    memset(c,0,CAMERA_BYTES);
    for (int k=0;k<4;k++) { c[12+5*k]=1; c[28+5*k]=1; }
    c[15]=translation; c[56]=1; c[57]=25000; c[62]=16.0f/9; c[70]=1.1f;
    c[84]=c[89]=c[94]=1;
}
static void clear_open_updates(void) {
    ubd_bm_clear(); g_nubd=0; g_ubd_ver++;
    memset(g_ubd_pf,0,sizeof g_ubd_pf); g_ubd_wide=0;
    g_ubd_env_lo=UINT64_MAX; g_ubd_env_hi=0; pend_reset(); memset(g_ubpc,0,sizeof g_ubpc);
}
static void begin_case(long frame,uint64_t offset) {
    clear_open_updates(); atomic_store(&g_cur_frame,(uint64_t)frame);
    S.ubo[0][0]=(p_bind_buf){.stage=0,.index=0,.addr=(uint64_t)mem+offset,.size=BINDING_BYTES};
    float c[96]; camera_values(c,0); memcpy(mem+offset,c,sizeof c);
    FG.cam_frame=-1;
}
static uint64_t capture(void) {
    nvn_rbind b={.kind=NVN_RB_CBUF,.reg=0,.fixw=96}; id<MTLBuffer> use=nil;
    return cbuf_slot(shader,0,&b,&use);
}
static void inline_float(unsigned index,float value) {
    mtl_sub_update(&value,S.ubo[0][0].addr+index*sizeof(float),sizeof value);
}
static void full_camera(float translation) {
    float c[96]; camera_values(c,translation); mtl_sub_update(c,S.ubo[0][0].addr,sizeof c);
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_TBDR","0",1); setenv("DM_NVN_UB_DEFER","1",1); setenv("DM_NVN_UB_LAZY","160",1);
    mtl_init();
    mem=mmap(NULL,LEN,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    CHECK(mem!=MAP_FAILED,"test memory allocated"); if (mem==MAP_FAILED) return 1;
    mtl_pool_add(mem,LEN); state_reset(); memset(&R,0,sizeof R); mtl_sub_begin();
    running=g_done+1000; g_fence=running; // synthetic uncompleted submissions, never sent to GPU
    shader=[NvShader new]; shader->cb_ext[0]=CAMERA_BYTES;
    strlcpy(shader->key,"framegen-camera-fixture",sizeof shader->key);
    atomic_store(&g_fg_mode,1); // test the contract independently of hardware/build capability gates

    begin_case(100,0);
    CHECK(capture()!=0 && FG.cam_frame==100 && FG.cam[15]==0,"clean bound camera is captured");
    CHECK(g_ubp_ready==0,"384-byte camera snapshot is synchronous");

    inline_float(15,7);
    CHECK(g_nubd>0 && !ubd_covers(S.ubo[0][0].addr,S.ubo[0][0].addr+CAMERA_BYTES),"partial camera inline update stays deferred");
    CHECK(((float *)mem)[15]==0,"guest camera memory is still stale before queued update executes");
    CHECK(capture()!=0 && FG.cam_frame==100 && FG.cam[15]==7,"camera sees queued partial inline update, not stale guest memory");
    CHECK(g_ubp_ready==0,"cached384-byte shadow requires no worker wait");
    inline_float(15,8); capture();
    CHECK(FG.cam[15]==8,"later same-frame update replaces the earlier captured camera");

    begin_case(101,0x4000); full_camera(11);
    CHECK(ubd_covers(S.ubo[0][0].addr,S.ubo[0][0].addr+CAMERA_BYTES),"full deferred camera update covers the observed block");
    capture(); CHECK(FG.cam_frame==101 && FG.cam[15]==11,"fully deferred camera is read from ordered update bytes");
    CHECK(((float *)(mem+0x4000))[15]==0,"full deferred update does not alter original guest bytes early");

    begin_case(102,0x8000); inline_float(15,19);
    gw_commit(running); clear_open_updates(); // an earlier submission still owns the queued inline bytes
    inline_float(16,23); capture();
    CHECK(FG.cam_frame==102 && FG.cam[15]==19 && FG.cam[16]==23,"camera combines running and current inline updates in order");
    CHECK(g_ubp_ready==0,"running-update camera overlay is also synchronous");

    begin_case(103,0xc000); capture();
    CHECK(FG.cam_frame==103,"a valid camera exists before unknown write");
    pend_add(S.ubo[0][0].addr+15*sizeof(float),sizeof(float),3);
    capture();
    CHECK(FG.cam_frame!=103,"unknown queued GPU write invalidates earlier same-frame camera");

    begin_case(104,0x10000); capture();
    float c[96]; camera_values(c,2); c[15]=NAN; fg_cam_note(c);
    CHECK(FG.cam_frame!=104,"malformed scene matrix invalidates earlier same-frame camera");
    camera_values(c,3); fg_cam_note(c); CHECK(FG.cam_frame==104,"valid camera can recover after invalidation");
    c[57]=10000; fg_cam_note(c);
    CHECK(FG.cam_frame==104 && FG.cam[15]==3,"readable environment-probe camera preserves the valid scene camera");
    fg_cam_note(NULL); CHECK(FG.cam_frame!=104,"explicit unreadable-camera notification invalidates current sample");

    begin_case(105,0x14000); capture();
    CHECK(FG.cam_frame==105,"valid camera before invalid binding");
    S.ubo[0][0].addr=0; capture();
    CHECK(FG.cam_frame!=105,"invalid-address camera binding invalidates earlier same-frame camera");

    // The expanded guest projection is not yet a verified MetalFX camera
    // contract. Test the real guard before any resource setup or encoding.
    uint64_t made=atomic_load(&FG.made);
    FG.prev_ok=1; FG.warm=3; FG.last_frame=FG.last_encoded_frame=105;
    NVNcopyRegion tall={0,0,0,2560,1600,1};
    fg_frame(nil,nil,nil,&tall,1920,1200);
    CHECK(!FG.prev_ok && !FG.warm && !FG.gen_ready,"tall scene invalidates interpolation history before encoding");
    CHECK(FG.last_frame==-2 && FG.last_encoded_frame==-2,"returning to 16:9 cannot inherit tall scene history");
    CHECK(atomic_load(&FG.made)==made && !FG.fi,"unverified tall scene performs no interpolation or resource setup");
    CHECK(atomic_load(&g_fg_fallback)==101,"actual tall scene reports its camera-contract limitation");
    atomic_store(&g_fg_mode,-1); setenv("DM_FRAMEGEN","1",1); mtl_present_screen(1920,1200);
    CHECK(!fg_on(),"tall screen blocks requested frame generation at startup");
    CHECK(strstr(mtl_framegen_status(),"expanded aspect camera metadata")!=NULL,"Diagnostics names the actual inactive reason");
    mtl_present_screen(0,0);

    clear_open_updates();
    printf("t_fg_camera: %d checks, %d failed\n",checks,failed); fflush(stdout);
    _exit(failed!=0);
}}

// Admission tests for the exact terminal-quad proof. No GPU or game launch.
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { NVN_POLYGON_MODE_FILL=2, NVN_DEPTH_FUNC_ALWAYS=8,
       NVN_FRONT_FACE_CCW=1, NVN_FACE_FRONT=1, NVN_FACE_BACK=2, NVN_FACE_FRONT_AND_BACK=3 };
typedef struct { const char *key; } Shader;
typedef struct { Shader *sh[6]; } NvProg;
typedef struct { int count,prim,instances; uintptr_t ind; int mdi_cnt; } p_draw;
typedef p_draw p_draw_idx;
static struct {
    int rast_discard;
    struct { int mode,front,cull; } poly;
    struct { int a2c; } ms;
    struct { unsigned mask; } chmask;
    struct { int dt,df,st; } ds;
    unsigned char vp_sw[1][4];
} S;
static struct { struct { void *depth; } rt; } R;
static _Atomic int g_origin_ll,g_depth_zero;
static int g_queue_origin_ll, pending_vertices, pending_uniform, alpha_test;
static float vertices[64][4], uniforms[4][4];
static int atest_on(void) { return alpha_test; }
static int attr_read(int a,const p_draw *dw,const p_draw_idx *di,float out[64][4]) {
    (void)a; (void)dw; (void)di; memcpy(out,vertices,sizeof vertices); return pending_vertices;
}
static int ubo_float(int stage,uint32_t ref,float *out) {
    if (pending_uniform || stage || (ref>>2)<2 || (ref>>2)>5) return 0;
    *out=uniforms[(ref>>2)-2][ref&3]; return 1;
}
#define MLOG(...) do { fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); } while(0)
#include "../nvn/nvn_fg_geometry.inc"

static Shader vertex={"a5b1bd74e47458d7_b"};
static NvProg program={.sh={&vertex}};
static p_draw draw={.count=6,.prim=4,.instances=1};
static int checks,failures;
#define CHECK(label,expression) do { checks++; if (!(expression)) { failures++; printf("FAIL: %s\n",label); } else printf("PASS: %s\n",label); } while(0)
static void reset(void) {
    memset(&S,0,sizeof S); memset(&R,0,sizeof R); memset(uniforms,0,sizeof uniforms);
    S.poly.mode=NVN_POLYGON_MODE_FILL; S.poly.front=NVN_FRONT_FACE_CCW; S.chmask.mask=15;
    uniforms[0][0]=uniforms[1][1]=uniforms[2][2]=uniforms[3][3]=1;
    const float quad[6][4]={{-1,-1,0,1},{1,-1,0,1},{1,1,0,1},{-1,-1,0,1},{1,1,0,1},{-1,1,0,1}};
    memcpy(vertices,quad,sizeof quad); atomic_store(&g_origin_ll,0); atomic_store(&g_depth_zero,0);
    g_queue_origin_ll=pending_vertices=pending_uniform=alpha_test=0;
    draw=(p_draw){.count=6,.prim=4,.instances=1}; vertex.key="a5b1bd74e47458d7_b";
}
int main(void) {
    int flip=-1;
    reset(); CHECK("identity two-triangle quad",fg_terminal_geometry(&program,&draw,NULL,&flip)&&flip==0);
    S.poly.cull=NVN_FACE_BACK; CHECK("counterclockwise front survives back culling",fg_terminal_geometry(&program,&draw,NULL,&flip));
    S.poly.cull=NVN_FACE_FRONT; CHECK("culled terminal triangle rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); atomic_store(&g_origin_ll,1); CHECK("lower-left viewport records vertical flip",fg_terminal_geometry(&program,&draw,NULL,&flip)&&flip==1);
    S.vp_sw[0][1]=1; CHECK("two vertical flips cancel",fg_terminal_geometry(&program,&draw,NULL,&flip)&&flip==0);
    reset(); uniforms[1][1]=-1; CHECK("vertex vertical reflection records flip",fg_terminal_geometry(&program,&draw,NULL,&flip)&&flip==1);
    reset(); uniforms[0][0]=-1; CHECK("horizontal reflection rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); uniforms[0][0]=0.8f; CHECK("inset quad rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); uniforms[3][0]=0.1f; CHECK("perspective mapping rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); vertices[4][0]=-1; vertices[4][1]=1; vertices[5][0]=1; vertices[5][1]=-1;
    CHECK("overlapping halves sharing a side rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); memcpy(vertices[5],vertices[4],sizeof vertices[5]); CHECK("missing corner rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); pending_vertices=1; CHECK("pending vertex writes rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); pending_uniform=1; CHECK("unresolved uniform writes rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); vertices[0][0]=NAN; CHECK("non-finite input rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); uniforms[2][3]=2; CHECK("depth-clipped quad rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); S.chmask.mask=7; CHECK("partial output mask rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); alpha_test=1; CHECK("alpha-test coverage rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); vertex.key="unknown"; CHECK("unknown vertex shader rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); draw.ind=1; CHECK("indirect draw rejected",!fg_terminal_geometry(&program,&draw,NULL,&flip));
    reset(); CHECK("indexed draw uses the same proof",fg_terminal_geometry(&program,NULL,&draw,&flip)&&flip==0);
    printf("%d checks, %d failures\n",checks,failures); return failures?1:0;
}

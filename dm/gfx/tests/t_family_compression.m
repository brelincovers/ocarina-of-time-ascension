// Lossless HDR/depth fidelity for fully rewritten resolution-family targets.
// This deliberately does NOT bless loading bytes written through a different-size
// alias: t_image_alias's cross_size_read covers why that remains unsafe by default.
// Run baseline and opt-in in different processes, with Metal validation.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failures;
#define CHECK(c, label) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static tex_t family_image(pool_t *pool, unsigned w, unsigned h, int depth) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=nvn_new_serial(), .pool=pool, .off=depth ? 8u<<20 : 0};
    t.mem = pool->mem + t.off;
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=depth ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA16F,
        .flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE|NVN_TEXTURE_FLAGS_IMAGE, .width=w, .height=h, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t); return t;
}

int main(void) { @autoreleasepool {
    mtl_init(); NSError *error = nil;
    int compressed = getenv("DM_NVN_FAMILY_NOCOMPRESS") ? !strcmp(getenv("DM_NVN_FAMILY_NOCOMPRESS"), "0") : DM_GAME_FAMILY_COMPRESS;
    printf("family compression expected %s\n", compressed ? "on" : "off");
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0.5,1); }\n"
        "struct O { float4 c [[color(0)]]; float d [[depth(any)]]; };\n"
        "fragment O f(float4 p [[position]]) { uint x=uint(p.x),y=uint(p.y); O o; o.c=float4(0.25f+float(x%16)/16.0f, 1.0f+float(y%8)/8.0f, 4.0f+float((x+y)%8)/4.0f, 0.75f); o.d=0.25f+float((x+y)%4)/8.0f; return o; }\n"
        options:nil error:&error];
    if (!lib) { fprintf(stderr, "fixture library: %s\n", error.localizedDescription.UTF8String); return 2; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:@"v"]; pd.fragmentFunction=[lib newFunctionWithName:@"f"];
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA16Float; pd.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> color=[g_dev newRenderPipelineStateWithDescriptor:pd error:&error];
    pd.colorAttachments[0].writeMask=MTLColorWriteMaskNone;
    id<MTLRenderPipelineState> depth=[g_dev newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!color || !depth) { fprintf(stderr,"fixture pipelines: %s\n",error.localizedDescription.UTF8String); return 2; }
    MTLDepthStencilDescriptor *ds=[MTLDepthStencilDescriptor new]; ds.depthCompareFunction=MTLCompareFunctionAlways; ds.depthWriteEnabled=YES;
    id<MTLDepthStencilState> write=[g_dev newDepthStencilStateWithDescriptor:ds]; ds.depthCompareFunction=MTLCompareFunctionEqual; ds.depthWriteEnabled=NO;
    id<MTLDepthStencilState> equal=[g_dev newDepthStencilStateWithDescriptor:ds];
    void *mem=NULL; if(posix_memalign(&mem,16384,16u<<20)) return 2; memset(mem,0,16u<<20);
    pool_t pool={.magic=MAGIC('P','O','O','L'),.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE,.mem=mem,.size=16u<<20};
    tex_t colors[2]={family_image(&pool,320,192,0),family_image(&pool,256,144,0)};
    tex_t depths[2]={family_image(&pool,320,192,1),family_image(&pool,256,144,1)};
    id<MTLTexture> ct[2],dt[2]; mtl_sub_begin();
    for(int i=0;i<2;i++){ct[i]=tex_mtl(&colors[i]);dt[i]=tex_mtl(&depths[i]);}
    mtl_sub_commit(); mtl_drain();
    CHECK(ct[0] && ct[1] && ct[0].heap && ct[0].heap==ct[1].heap,"color resolutions share production family storage");
    CHECK(dt[0] && dt[1] && dt[0].heap && dt[0].heap==dt[1].heap,"depth resolutions share production family storage");
    CHECK(ct[0].allowGPUOptimizedContents==compressed && dt[0].allowGPUOptimizedContents==compressed,"production descriptors use the selected compression policy");
    printf("reported Metal compression type: HDR %ld, depth %ld\n", (long)ct[0].compressionType, (long)dt[0].compressionType);
    const int sizes[]={0,1,0,1,0};
    for(unsigned iteration=0;iteration<sizeof sizes/sizeof sizes[0];iteration++){
        int k=sizes[iteration]; NSUInteger w=ct[k].width,h=ct[k].height,pixels=w*h;
        id<MTLBuffer> out=[g_dev newBufferWithLength:pixels*12 options:MTLResourceStorageModeShared]; res_add(out); res_commit();
        id<MTLCommandBuffer> cb=q_cb();
        // Production uses explicit Metal 4 barriers. Placement-heap textures are
        // untracked, so this classic-Metal fixture must express the same dependencies.
        id<MTLFence> written=[g_dev newFence];
        for(int pass=0;pass<2;pass++){
            MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture=ct[k]; rp.colorAttachments[0].loadAction=pass?MTLLoadActionLoad:MTLLoadActionClear;
            rp.colorAttachments[0].storeAction=MTLStoreActionStore; rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);
            rp.depthAttachment.texture=dt[k]; rp.depthAttachment.loadAction=pass?MTLLoadActionLoad:MTLLoadActionClear;
            rp.depthAttachment.storeAction=MTLStoreActionStore; rp.depthAttachment.clearDepth=1;
            id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:rp];
            if(pass) [e waitForFence:written beforeStages:MTLRenderStageVertex];
            [e setRenderPipelineState:pass?color:depth]; [e setDepthStencilState:pass?equal:write];
            [e setViewport:(MTLViewport){0,0,w,h,0,1}]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e updateFence:written afterStages:MTLRenderStageFragment]; [e endEncoding];
        }
        id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
        [blit waitForFence:written];
        [blit copyFromTexture:ct[k] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1) toBuffer:out destinationOffset:0 destinationBytesPerRow:w*8 destinationBytesPerImage:pixels*8];
        [blit copyFromTexture:dt[k] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1) toBuffer:out destinationOffset:pixels*8 destinationBytesPerRow:w*4 destinationBytesPerImage:pixels*4];
        [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status==MTLCommandBufferStatusCompleted,"compressed target GPU work completes without errors");
        int rgba_ok=1,depth_ok=1; NSUInteger bad_color=0,bad_depth=0; const _Float16 *rgba=out.contents; const float *z=(const float *)((const uint8_t *)out.contents+pixels*8);
        for(NSUInteger y=0;y<h;y++)for(NSUInteger x=0;x<w;x++){
            NSUInteger p=y*w+x; _Float16 expected[4]={0.25f+(float)(x%16)/16,1.0f+(float)(y%8)/8,4.0f+(float)((x+y)%8)/4,0.75f};
            if(memcmp(rgba+p*4,expected,sizeof expected)) {
                if(bad_color++<3) fprintf(stderr,"HDR mismatch pass%u pixel%lu,%lu actual=%g,%g,%g,%g expected=%g,%g,%g,%g\n", iteration,(unsigned long)x,(unsigned long)y,
                    (double)rgba[p*4],(double)rgba[p*4+1],(double)rgba[p*4+2],(double)rgba[p*4+3],(double)expected[0],(double)expected[1],(double)expected[2],(double)expected[3]);
                rgba_ok=0;
            }
            if(z[p]!=0.25f+(float)((x+y)%4)/8) {bad_depth++;depth_ok=0;}
        }
        CHECK(rgba_ok,"HDR values above white survive a size switch and depth-EQUAL color pass bit exactly");
        CHECK(depth_ok,"stored scene depth remains bit exact after the EQUAL pass");
        printf("%lux%lu pass %u HDR=%d depth=%d mismatches=%lu/%lu\n",(unsigned long)w,(unsigned long)h,iteration,rgba_ok,depth_ok,(unsigned long)bad_color,(unsigned long)bad_depth); res_remove(out); res_commit();
    }
    for(int i=0;i<2;i++){mtl_tex_release(&colors[i]);mtl_tex_release(&depths[i]);ct[i]=nil;dt[i]=nil;}
    mtl_alias_drop_pool(&pool); mtl_drain(); free(mem);
    printf("t_family_compression: %d checks, %d failed\n",checks,failures); return failures!=0;
} }

// Offscreen colour-contract fixture. No game launch, swapchain or presentation.
// Build: xcrun clang -fobjc-arc -O1 t_framegen_color.m -framework Foundation
//          -framework Metal -o /an/isolated/output/t_framegen_color
// Optional argument: a4200b53be47212c_b.metallib extracted read-only from the
// preserved shader image. It compares against the actual compiled game shader,
// in addition to the independent double-precision ST2084 reference below.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nvn/nvn_framegen_color.h"

enum { W = 16, H = 1 };
typedef struct { float v[4]; } Vec4;
static int checks, failures;
#define CHECK(label, value) do { checks++; if (!(value)) { failures++; fprintf(stderr, "FAIL: %s\n", label); } else printf("PASS: %s\n", label); } while (0)

static NSString *const source = @"#include <metal_stdlib>\nusing namespace metal;\n"
    FG_HDR_COLOR_MSL
    "struct V { float4 p [[position]]; float4 uv [[user(locn0)]]; };\n"
    "vertex V v(uint i [[vertex_id]]) { float2 uv=float2((i<<1)&2,i&2); V o; o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1); o.uv=float4(uv,0,1); return o; }\n"
    "fragment float4 encode(V in [[stage_in]], texture2d<float> scene [[texture(0)]], constant float4 *rows [[buffer(0)]]) { float4 c=scene.read(uint2(in.p.xy)); return float4(fghdr_encode(c.rgb,rows),c.a); }\n"
    // This fixture's conservative composition keeps genuine encoded values for
    // changed canvas pixels. It tests colour/HUD preservation, not whether every
    // game pass after reconstruction is HUD or whether interpolation is correct.
    "fragment float4 composite(V in [[stage_in]], texture2d<float> real [[texture(0)]], texture2d<float> gen [[texture(1)]], texture2d<float> before [[texture(2)]], texture2d<float> after [[texture(3)]], constant float4 *rows [[buffer(0)]]) { uint2 q=uint2(in.p.xy); float4 w=real.read(q); if(any(before.read(q).rgb != after.read(q).rgb)) return w; return float4(fghdr_encode(gen.read(q).rgb,rows),w.a); }\n";

static double pq(double nits) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    double p = pow(fabs(nits) / 10000.0, m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
static unsigned code(double v) { return (unsigned)llround(fmin(1.0, fmax(0.0, v)) * 1023.0); }
static int distance(uint32_t a, uint32_t b) {
    int worst = 0;
    for (int c = 0; c < 3; c++) { int d = abs((int)((a >> (10*c)) & 1023) - (int)((b >> (10*c)) & 1023)); if (d > worst) worst=d; }
    return worst;
}
static id<MTLTexture> texture(id<MTLDevice> dev, MTLPixelFormat format, MTLTextureUsage usage, const void *data, size_t stride) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:W height:H mipmapped:NO];
    d.storageMode=MTLStorageModeShared; d.usage=usage;
    id<MTLTexture> t=[dev newTextureWithDescriptor:d];
    if (data) [t replaceRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0 withBytes:data bytesPerRow:stride];
    return t;
}
static id<MTLRenderPipelineState> pipeline(id<MTLDevice> dev, id<MTLFunction> vertex, id<MTLFunction> fragment) {
    MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new]; d.vertexFunction=vertex; d.fragmentFunction=fragment;
    d.colorAttachments[0].pixelFormat=MTLPixelFormatRGB10A2Unorm;
    NSError *error=nil; id<MTLRenderPipelineState> p=[dev newRenderPipelineStateWithDescriptor:d error:&error];
    if (!p) fprintf(stderr,"pipeline: %s\n",error.localizedDescription.UTF8String);
    return p;
}
static int render(id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pso, NSArray<id<MTLTexture>> *inputs, id<MTLBuffer> params,
                  id<MTLTexture> target, id<MTLBuffer> blend, NSArray<id<MTLResource>> *resources) {
    id<MTLCommandBuffer> cb=[queue commandBuffer]; MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor new];
    rp.colorAttachments[0].texture=target; rp.colorAttachments[0].loadAction=MTLLoadActionDontCare; rp.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:pso]; [e setViewport:(MTLViewport){0,0,W,H,0,1}];
    for (NSUInteger i=0;i<inputs.count;i++) [e setFragmentTexture:inputs[i] atIndex:i];
    [e setFragmentBuffer:params offset:0 atIndex:0];
    if (blend) [e setFragmentBuffer:blend offset:0 atIndex:28];
    for (id<MTLResource> r in resources) [e useResource:r usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) fprintf(stderr,"GPU: %s\n",cb.error.localizedDescription.UTF8String);
    return cb.status==MTLCommandBufferStatusCompleted;
}
static void read10(id<MTLTexture> t, uint32_t *data) { [t getBytes:data bytesPerRow:W*4 fromRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0]; }

int main(int argc,const char **argv) { @autoreleasepool {
    id<MTLDevice> dev=MTLCreateSystemDefaultDevice(); if (!dev) return 2;
    printf("GPU: %s; shader-chain input remains 8d9afacc48e2\n",dev.name.UTF8String);
    MTLCompileOptions *options=[MTLCompileOptions new]; options.mathMode=MTLMathModeSafe;
    NSError *error=nil; id<MTLLibrary> lib=[dev newLibraryWithSource:source options:options error:&error];
    if (!lib) { fprintf(stderr,"MSL: %s\n",error.localizedDescription.UTF8String); return 2; }
    id<MTLFunction> vs=[lib newFunctionWithName:@"v"];
    id<MTLRenderPipelineState> encode=pipeline(dev,vs,[lib newFunctionWithName:@"encode"]), compose=pipeline(dev,vs,[lib newFunctionWithName:@"composite"]);
    if (!encode || !compose) return 2;
    id<MTLCommandQueue> queue=[dev newCommandQueue];
    const float luminance[W]={0,0.005,0.1,1,20,100,203,400,600,1000,2000,4000,10000,50,150,750};
    Vec4 linear[W];
    for (int i=0;i<W;i++) { linear[i]=(Vec4){{luminance[i]/203,luminance[i]/203,luminance[i]/203,1}}; }
    linear[13]=(Vec4){{2,0.1,0.3,1}}; linear[14]=(Vec4){{0.1,2,0.3,1}}; linear[15]=(Vec4){{0.3,0.1,2,1}};
    Vec4 rows[3]={{{203,0,0,0}},{{0,203,0,0}},{{0,0,203,0}}};
    id<MTLBuffer> matrix=[dev newBufferWithBytes:rows length:sizeof rows options:MTLResourceStorageModeShared];
    id<MTLTexture> scene=texture(dev,MTLPixelFormatRGBA32Float,MTLTextureUsageShaderRead,linear,sizeof linear);
    id<MTLTexture> output=texture(dev,MTLPixelFormatRGB10A2Unorm,MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead,NULL,0);
    CHECK("PQ helper completes",render(queue,encode,@[scene],matrix,output,nil,@[]));
    uint32_t actual[W],expected[W]; read10(output,actual); int max_error=0;
    for (int i=0;i<W;i++) { expected[i]=3u<<30; for (int c=0;c<3;c++) expected[i]|=code(pq((double)linear[i].v[c]*203))<<(c*10); int d=distance(actual[i],expected[i]); if(d>max_error)max_error=d; }
    printf("Independent ST2084 reference maximum error: %d / 1023\n",max_error);
    CHECK("0 through 10000 nits and saturated colours agree within one code",max_error<=1);
    CHECK("400, 1000 and 4000 nit highlights remain above 203 nit white",(actual[7]&1023)>(actual[6]&1023) && (actual[9]&1023)>(actual[7]&1023) && (actual[11]&1023)>(actual[9]&1023));

    id<MTLTexture> roundtrip=texture(dev,MTLPixelFormatRGB10A2Unorm,MTLTextureUsageRenderTarget,NULL,0);
    CHECK("stationary composition completes",render(queue,compose,@[output,scene,scene,scene],matrix,roundtrip,nil,@[]));
    uint32_t composed[W]; read10(roundtrip,composed);
    CHECK("stationary genuine/generated colour is bit-identical",memcmp(actual,composed,sizeof actual)==0);
    Vec4 changed[W],replacement[W]; memcpy(changed,linear,sizeof changed);
    for (int i=0;i<W;i++) replacement[i]=(Vec4){{0,0,0,1}};
    // Including a bright 1000-nit HUD pixel: it must retain the genuine encoded
    // value even when the generated scene underneath is entirely black.
    const int hud[]={6,9,13}; for (unsigned i=0;i<sizeof hud/sizeof *hud;i++) changed[hud[i]].v[0]+=0.125f;
    id<MTLTexture> after=texture(dev,MTLPixelFormatRGBA32Float,MTLTextureUsageShaderRead,changed,sizeof changed);
    id<MTLTexture> generated=texture(dev,MTLPixelFormatRGBA32Float,MTLTextureUsageShaderRead,replacement,sizeof replacement);
    CHECK("HUD composition completes",render(queue,compose,@[output,generated,scene,after],matrix,roundtrip,nil,@[])); read10(roundtrip,composed);
    CHECK("genuine 203-nit and 1000-nit HUD and coloured HUD stay exact",composed[6]==actual[6] && composed[9]==actual[9] && composed[13]==actual[13]);
    CHECK("unmasked generated scene is used",(composed[7]&0x3fffffff)==0);

    if(argc>1) {
        id<MTLLibrary> guest=[dev newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
        MTLFunctionConstantValues *constants=[MTLFunctionConstantValues new]; BOOL borderOff=YES;
        [constants setConstantValue:&borderOff type:MTLDataTypeBool atIndex:216];
        id<MTLFunction> fs=[guest newFunctionWithName:@"main0" constantValues:constants error:&error];
        id<MTLRenderPipelineState> native=fs?pipeline(dev,vs,fs):nil;
        CHECK("preserved guest terminal shader loads",native!=nil);
        if(native) {
            id<MTLArgumentEncoder> ae=[fs newArgumentEncoderWithBufferIndex:0];
            id<MTLBuffer> arguments=[dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared]; [ae setArgumentBuffer:arguments offset:0];
            id<MTLBuffer> uniforms=[dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; memset(uniforms.contents,0,65536); memcpy((uint8_t *)uniforms.contents+18*16,rows,sizeof rows);
            id<MTLBuffer> border=[dev newBufferWithLength:16+32*16 options:MTLResourceStorageModeShared]; memset(border.contents,0,border.length);
            id<MTLBuffer> blend=[dev newBufferWithLength:4 options:MTLResourceStorageModeShared]; memset(blend.contents,0,4);
            MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new]; sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterNearest; sd.sAddressMode=sd.tAddressMode=MTLSamplerAddressModeClampToEdge; sd.supportArgumentBuffers=YES;
            id<MTLSamplerState> sampler=[dev newSamplerStateWithDescriptor:sd];
            [ae setBuffer:uniforms offset:0 atIndex:0]; [ae setTexture:scene atIndex:1]; [ae setSamplerState:sampler atIndex:2]; [ae setSamplerState:sampler atIndex:3]; [ae setBuffer:border offset:0 atIndex:4];
            CHECK("preserved guest shader executes",render(queue,native,@[],arguments,roundtrip,blend,@[uniforms,border,scene])); read10(roundtrip,composed);
            max_error=0; for(int i=0;i<W;i++){int d=distance(actual[i],composed[i]);if(d>max_error)max_error=d;}
            printf("Preserved guest PQ shader maximum error: %d / 1023\n",max_error);
            CHECK("helper matches actual compiled guest terminal shader",max_error<=1);
            // A non-diagonal gamut matrix catches transposition/row-order bugs.
            Vec4 gamut[3]={{{127.365852f,66.849985f,8.784163f,0}},{{14.028859f,186.669971f,2.30117f,0}},{{3.326647f,17.777937f,181.895416f,0}}};
            memcpy(matrix.contents,gamut,sizeof gamut); memcpy((uint8_t *)uniforms.contents+18*16,gamut,sizeof gamut);
            CHECK("gamut helper executes",render(queue,encode,@[scene],matrix,output,nil,@[])); read10(output,actual);
            CHECK("gamut guest executes",render(queue,native,@[],arguments,roundtrip,blend,@[uniforms,border,scene])); read10(roundtrip,composed);
            max_error=0; for(int i=0;i<W;i++){int d=distance(actual[i],composed[i]);if(d>max_error)max_error=d;}
            printf("Preserved guest gamut+PQ maximum error: %d / 1023\n",max_error);
            CHECK("captured-row ordering and FMA match guest transform",max_error<=1);
        }
    } else printf("NOT TESTED: actual compiled guest shader (supply its isolated extracted metallib).\n");
    printf("%d checks, %d failures. Fixture validates colour math; runtime binding, HUD coverage and actual generated presentation remain separate.\n",checks,failures);
    return failures?1:0;
} }

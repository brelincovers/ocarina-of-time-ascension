// Descriptor/cache isolation plus the existing exact-color/depth first-use fixture.
// Run in separate processes with DM_NVN_MESH_TGHINT=0 and =1: the policy is latched.
#define main mesh_first_use_main
#include "t_mesh_first_use.m"
#undef main

static void hint_descriptors(void) {
    mtl_init(); NSError *error = nil;
    id<MTLLibrary> lib = lib_tag([g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; }; struct P { uint x; };\n"
        "using M = metal::mesh<V, void, 3, 1, metal::topology::triangle>;\n"
        "[[object]] void obj(object_data P& p [[payload]], mesh_grid_properties grid, uint tid [[thread_index_in_threadgroup]]) { if (!tid) { p.x=0; grid.set_threadgroups_per_grid(uint3(1)); } }\n"
        "[[mesh]] void msh(M m, const object_data P& p [[payload]], uint tid [[thread_index_in_threadgroup]]) { if (!tid) m.set_primitive_count(0); }\n"
        "fragment float4 pix() { return float4(1); }\n" options:nil error:&error], @"ascension_mesh_hint_fixture");
    require(lib != nil, "mesh hint fixture library compiles");
    NvShader *ts = [NvShader new], *ms = [NvShader new];
    ts->fn = fn4_note([lib newFunctionWithName:@"obj"], lib, @"obj", nil); ts->mesh_threads = 32; ts->mesh_payload = 4;
    ms->fn = fn4_note([lib newFunctionWithName:@"msh"], lib, @"msh", nil); ms->mesh_threads = 32; ms->mesh_obj = ts->fn;
    id<MTLFunction> fs = fn4_note([lib newFunctionWithName:@"pix"], lib, @"pix", nil);
    MTLRenderPipelineDescriptor *base = [MTLRenderPipelineDescriptor new]; base.rasterSampleCount = 1; base.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    int enabled = mesh_tghint_enabled();
    CHECK(!mesh_tghint_threads(0) && !mesh_tghint_threads(1) && !mesh_tghint_threads(31) && !mesh_tghint_threads(33), "partial groups receive no hint");
    CHECK(mesh_tghint_threads(32) && mesh_tghint_threads(64), "complete SIMD groups are eligible");
    MTL4MeshRenderPipelineDescriptor *d = mtk_desc4(base, ts, ms, fs, 0);
    require(d != nil, "production Metal 4 mesh descriptor exists");
    CHECK(d.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth == enabled && d.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth == enabled, "actual task and mesh descriptor follows launch policy");
    CHECK(d.maxTotalThreadsPerObjectThreadgroup == 32 && d.maxTotalThreadsPerMeshThreadgroup == 32, "hints do not alter dispatch sizes");
    d.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = NO; d.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth = NO;
    uint64_t baseline = arch_key(d);
    d.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = YES; uint64_t task_key = arch_key(d);
    d.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth = YES; uint64_t both_key = arch_key(d);
    d.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = NO; uint64_t mesh_key = arch_key(d);
    CHECK(baseline && task_key && both_key && mesh_key && baseline != task_key && baseline != mesh_key && task_key != both_key && mesh_key != both_key,
          "persistent archive keys distinguish each hint combination");
    MTL4MeshRenderPipelineDescriptor *notask = mtk_desc4(base, nil, ms, fs, 0);
    CHECK(notask && notask.maxTotalThreadsPerObjectThreadgroup == 1 && !notask.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth,
          "mesh-only one-thread object shim never promises a SIMD-sized group");
    CHECK(notask.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth == enabled, "mesh-only mesh stage follows policy");
    ts->mesh_threads = 1; ms->mesh_threads = 31;
    MTL4MeshRenderPipelineDescriptor *partial = mtk_desc4(base, ts, ms, fs, 0);
    CHECK(!partial.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth && !partial.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth,
          "actual nonmultiple descriptors retain the original compiler contract");
    setenv("DM_NVN_MESH_TGHINT", enabled ? "0" : "1", 1);
    CHECK(mesh_tghint_enabled() == enabled, "policy cannot mix incompatible in-process cache entries after startup");
}
int main(void) { @autoreleasepool { hint_descriptors(); return mesh_first_use_main(); } }

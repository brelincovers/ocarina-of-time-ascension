// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; };
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }

static float3 srgb_to_linear(float3 color) {
    return select(pow(max((color + 0.055f) / 1.055f, 0.0f), 2.4f), color / 12.92f, color <= 0.04045f);
}

static float3 linear_to_srgb(float3 color) {
    return select(1.055f * pow(max(color, 0.0f), 1.0f / 2.4f) - 0.055f, color * 12.92f, color <= 0.0031308f);
}

constant float detail_strength [[function_constant(0)]];

// Beta 5's bounded detail filter, applied to the SDR presentation in linear light.
// Every tap clamps to the active crop so padding cannot create an outline.
fragment float4 fs_detail(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float2 uv = clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw);
    float2 texel = 1.0f / float2(t.get_width(), t.get_height());
    float2 dx = float2(texel.x, 0.0f), dy = float2(0.0f, texel.y);
    float4 pixel = t.sample(s, uv);
    float3 center = srgb_to_linear(pixel.rgb);
    float3 average = (srgb_to_linear(t.sample(s, clamp(uv + dx, c.edge.xy, c.edge.zw)).rgb) +
                      srgb_to_linear(t.sample(s, clamp(uv - dx, c.edge.xy, c.edge.zw)).rgb) +
                      srgb_to_linear(t.sample(s, clamp(uv + dy, c.edge.xy, c.edge.zw)).rgb) +
                      srgb_to_linear(t.sample(s, clamp(uv - dy, c.edge.xy, c.edge.zw)).rgb)) * 0.25f;
    float3 detail = center - average;
    float peak = max(max(abs(detail.r), abs(detail.g)), abs(detail.b));
    float3 correction = clamp(detail * detail_strength, -0.05f, 0.05f) * smoothstep(0.007f, 0.027f, peak);
    return float4(linear_to_srgb(clamp(center + correction, 0.0f, 1.0f)), pixel.a);
}

// SDR splash screens on an HDR drawable: decode sRGB, convert Rec.709 to BT.2020, and encode at 203-nit reference white.
fragment float4 fs_sdr_to_pq(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float4 pixel = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));
    float3 linear = srgb_to_linear(pixel.rgb);
    const float3x3 rec709_to_2020 = float3x3(float3(0.627404f, 0.069097f, 0.016391f),
                                          float3(0.329283f, 0.919540f, 0.088013f),
                                          float3(0.043313f, 0.011362f, 0.895595f));
    float3 p = pow(max(rec709_to_2020 * linear, 0.0f) * 0.0203f, 0.1593017578125f);
    float3 pq = pow((0.8359375f + 18.8515625f * p) / (1.0f + 18.6875f * p), 78.84375f);
    return float4(pq, pixel.a);
}

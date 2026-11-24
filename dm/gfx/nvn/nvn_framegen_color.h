#ifndef NVN_FRAMEGEN_COLOR_H
#define NVN_FRAMEGEN_COLOR_H

// Host-only equivalent of the colour operation in the game's terminal HDR
// fragment shader a4200b53be47212c (chain 8d9afacc48e2). `rows` is an immutable
// copy of fragment uniform slot 0, float4 rows 18..20, for this very draw.
// Keep the guest's FMA order and absolute-value behaviour. These coefficients
// already contain the game's gamut/luminance treatment: do not add another
// Rec.709 conversion, paper-white multiplier, exposure or tone map here.
// The caller must prove the final draw samples the reconstructed canvas at the
// expected coordinates and preserve the genuine frame on an unknown contract.
// The shader is not part of the translated-game shader chain.
#define FG_HDR_COLOR_MSL \
    "static float3 fghdr_encode(float3 c, constant float4 *rows) {\n" \
    "  float3 y = float3(c.g * rows[0].y, c.g * rows[1].y, c.g * rows[2].y);\n" \
    "  y = fma(float3(c.r), float3(rows[0].x, rows[1].x, rows[2].x), y);\n" \
    "  y = fma(float3(c.b), float3(rows[0].z, rows[1].z, rows[2].z), y);\n" \
    "  float3 p = exp2(log2(abs(y * 9.9999997473787516355514526367188e-05f)) * 0.1593017578125f);\n" \
    "  float3 q = fma(p, float3(18.8515625f), float3(0.8359375f)) * (float3(1.0f) / fma(p, float3(18.6875f), float3(1.0f)));\n" \
    "  return exp2(log2(abs(q)) * 78.84375f);\n" \
    "}\n"

#endif

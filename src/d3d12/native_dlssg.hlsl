Texture2DArray<float4> SourceColor : register(t0);
Texture2DArray<float2> SourceMotion : register(t1);
Texture2DArray<float> SourceDepth : register(t2);
Texture2D<float4> Generated : register(t3);
ByteAddressBuffer DisableInterpolation : register(t4);
Texture2DArray<float4> CurrentFallback : register(t5);
RWTexture2D<float4> Color : register(u0);
RWTexture2D<float2> Motion : register(u1);
RWTexture2D<float> Depth : register(u2);

cbuffer Params : register(b0) {
    uint2 Extent;
    uint ColorSlice;
    uint GuideSlice;
    float4 OutputRect;
    float4 MotionRect;
    float4 DepthRect;
    float2 MotionScale;
    float2 JitterDelta;
    float4 Rotation;
    float4 SourceTangents;
    float4 TargetTangents;
    uint MotionSlice;
    uint ReversedDepth;
    uint PackPrevious;
    uint EncodeSrgb;
    float4 DepthConvention; // near, far, infinite, unused
    // Where this eye sits in the feature's private textures, and the cell of
    // them this dispatch writes: the eye plus its share of any seam beside it.
    uint EyeX;
    uint CellX;
    uint CellWidth;
    uint CellHeight;
    float2 MotionNormal; // one over the feature's size
};

float3 rotate(float3 ray, float4 q) {
    return ray + 2 * cross(q.xyz, cross(q.xyz, ray) + q.w * ray);
}
float2 source_uv(float3 ray, float4 tangents) {
    return (ray.xy / max(-ray.z, 0.00001) - tangents.xz) / (tangents.yw - tangents.xz);
}
// DLSS-G takes display-ready colour. An sRGB view decodes on read, so encode
// before NGX sees it and decode again for the sRGB render target.
float3 encode_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float3 decode_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float4 display(float4 c) {
    return EncodeSrgb != 0 ? float4(encode_srgb(c.rgb), c.a) : c;
}
// The hardware sRGB decode is not an exact inverse of the formula above. An
// unresampled 8-bit texel goes back on its own code; only the resampled
// previous frame keeps the precision between codes.
float4 display_texel(float4 c) {
    return EncodeSrgb != 0 ? float4(round(encode_srgb(c.rgb) * 255.0) / 255.0, c.a) : c;
}
float4 sample_color(float2 coordinate) {
    float2 c = clamp(coordinate, OutputRect.xy, OutputRect.xy + float2(Extent) - 1);
    int2 lo = int2(c), hi = min(lo + 1, int2(OutputRect.xy + float2(Extent) - 1));
    float2 f = frac(c);
    return lerp(lerp(SourceColor.Load(int4(lo, ColorSlice, 0)),
                     SourceColor.Load(int4(hi.x, lo.y, ColorSlice, 0)), f.x),
                lerp(SourceColor.Load(int4(lo.x, hi.y, ColorSlice, 0)),
                     SourceColor.Load(int4(hi, ColorSlice, 0)), f.x),
                f.y);
}

[numthreads(8, 8, 1)] void PackNativeDlssG(uint3 id : SV_DispatchThreadID) {
    if (id.x >= CellWidth || id.y >= CellHeight) {
        return;
    }
    // Both eyes can share one feature, side by side. Pixels of the cell
    // outside the eye - the seam between eyes, or rows below a shorter eye -
    // repeat the eye's nearest edge, so NGX sees each eye as if alone.
    uint2 cell = uint2(CellX + id.x, id.y);
    uint2 p = uint2(clamp(int2(cell) - int2(EyeX, 0), int2(0, 0), int2(Extent) - 1));
    float2 uv = (float2(p) + 0.5) / float2(Extent);
    float3 ray = float3(lerp(TargetTangents.x, TargetTangents.y, uv.x),
                        lerp(TargetTangents.z, TargetTangents.w, uv.y), -1);
    float3 previous_ray = rotate(ray, Rotation);
    float2 previous_uv = source_uv(previous_ray, SourceTangents);
    if (PackPrevious != 0) {
        if (previous_ray.z >= -0.00001 || any(previous_uv < 0) || any(previous_uv > 1)) {
            Color[cell] = display_texel(CurrentFallback.Load(int4(int2(OutputRect.xy) + int2(p), ColorSlice, 0)));
            Motion[cell] = 0;
            Depth[cell] = ReversedDepth != 0 ? 0 : 1;
            return;
        }
        uv = previous_uv;
    }
    // A rotated edge can land exactly at UV 1. Fractional guide rectangles
    // also occur when the color viewport is smaller than the guide output.
    int2 depth_lo = int2(DepthRect.xy);
    int2 depth_hi = max(depth_lo, int2(ceil(DepthRect.xy + DepthRect.zw)) - 1);
    int2 motion_lo = int2(MotionRect.xy);
    int2 motion_hi = max(motion_lo, int2(ceil(MotionRect.xy + MotionRect.zw)) - 1);
    int2 dp = clamp(int2(DepthRect.xy + uv * DepthRect.zw), depth_lo, depth_hi);
    int2 mp = clamp(int2(MotionRect.xy + uv * MotionRect.zw), motion_lo, motion_hi);
    float z = SourceDepth.Load(int4(dp, GuideSlice, 0));
    float2 mv = SourceMotion.Load(int4(mp, MotionSlice, 0));
    // Foreground dilation at depth edges avoids interpolating unrelated motion
    // vectors. Jitter and MV_Scale follow the bridge's existing guide contract.
    // The aligned reset seed uses only depth; dilation has no role there.
    [branch] if (PackPrevious == 0) {
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                int2 q = clamp(dp + int2(x, y), depth_lo, depth_hi);
                float d = SourceDepth.Load(int4(q, GuideSlice, 0));
                if ((ReversedDepth != 0 && d > z) || (ReversedDepth == 0 && d < z)) {
                    z = d;
                    float2 quv = (float2(q) + 0.5 - DepthRect.xy) / DepthRect.zw;
                    int2 qm = clamp(int2(MotionRect.xy + quv * MotionRect.zw), motion_lo, motion_hi);
                    mv = SourceMotion.Load(int4(qm, MotionSlice, 0));
                }
            }
        }
    }
    if (PackPrevious != 0) {
        Color[cell] = display(sample_color(OutputRect.xy + uv * float2(Extent) - 0.5));
        Motion[cell] = 0; // the reset seed has no predecessor
        float normal_z = ReversedDepth != 0 ? 1 - z : z;
        float n = DepthConvention.x, far_plane = DepthConvention.y;
        float linear_z =
            DepthConvention.z != 0
                ? n / max(1 - normal_z, 0.000001)
                : n * far_plane / max(far_plane - normal_z * (far_plane - n), 0.000001);
        float target_z = linear_z / max(-previous_ray.z, 0.00001);
        float target_depth = DepthConvention.z != 0
                                 ? 1 - n / target_z
                                 : far_plane * (target_z - n) / (target_z * (far_plane - n));
        Depth[cell] = saturate(ReversedDepth != 0 ? 1 - target_depth : target_depth);
    } else {
        Color[cell] = display_texel(SourceColor.Load(int4(int2(OutputRect.xy) + int2(p), ColorSlice, 0)));
        float2 backward = mv * MotionScale + JitterDelta;
        float2 a_uv = (float2(p) + 0.5 + backward) / float2(Extent);
        float3 a_ray = float3(lerp(SourceTangents.x, SourceTangents.y, a_uv.x),
                              lerp(SourceTangents.z, SourceTangents.w, a_uv.y), -1);
        float4 inverse_rotation = float4(-Rotation.xyz, Rotation.w);
        float3 b_ray = rotate(a_ray, inverse_rotation);
        float2 b_uv = source_uv(b_ray, TargetTangents);
        // As a fraction of the whole feature, which may hold both eyes.
        Motion[cell] = (b_uv - (float2(p) + 0.5) / float2(Extent)) * float2(Extent) * MotionNormal;
        // Dilation chooses motion, while depth remains at the original pixel.
        Depth[cell] = SourceDepth.Load(int4(dp, GuideSlice, 0));
    }
}

struct Vertex {
    float4 position : SV_Position;
};
Vertex NativeDlssGVS(uint id : SV_VertexID) {
    Vertex v;
    v.position = float4(id == 0 ? float2(-1, -1) : id == 1 ? float2(-1, 3) : float2(3, -1), 0, 1);
    return v;
}
float4 NativeDlssGPS(Vertex input) : SV_Target {
    int2 p = int2(input.position.xy);
    float4 current = SourceColor.Load(int4(p, ColorSlice, 0));
    if ((DisableInterpolation.Load(0) & 255u) != 0) {
        return current;
    }
    // The private colour may hold two alpha bits; the real frame's alpha is exact.
    float3 generated = Generated.Load(int3(p - int2(OutputRect.xy) + int2(EyeX, 0), 0)).rgb;
    // An sRGB target encodes only approximately. Decoding the nearest 8-bit
    // code, rather than a 10-bit value between two, keeps unmoved pixels exact.
    return float4(EncodeSrgb != 0 ? decode_srgb(round(generated * 255.0) / 255.0) : generated,
                  current.a);
}

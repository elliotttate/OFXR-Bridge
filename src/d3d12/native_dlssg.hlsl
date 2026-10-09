Texture2DArray<float4> SourceColor : register(t0);
Texture2DArray<float2> SourceMotion : register(t1);
Texture2DArray<float> SourceDepth : register(t2);
Texture2D<float4> Generated : register(t3);
ByteAddressBuffer DisableInterpolation : register(t4);
Texture2DArray<float4> CurrentFallback : register(t5);
Texture2D<float4> Packed : register(t6);
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
    uint EncodeSrgb;
    // The eye's size in the feature: Extent, or smaller when the feature runs
    // at a reduced resolution.
    uint2 FeatureExtent;
    // Where this eye sits in the feature's private textures, and the cell of
    // them this dispatch writes: the eye plus its share of any seam beside it.
    uint EyeX;
    uint CellX;
    uint CellWidth;
    uint CellHeight;
    float2 MotionNormal; // one over the feature's size
    float2 GuideScale; // guide texels per feature pixel
    // A reduced-resolution generated frame takes the real frame's detail where
    // the two differ by less than one over this, in display units, fading out
    // towards it. Zero restores none.
    float DetailFalloff;
    // How far the composed generated frame lies from B towards A: one half
    // for 2X, two thirds and one third for 3X.
    float TowardsA;
    uint DepthInverted; // nearer surfaces have larger depth values
    uint Padding2;
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
float4 bilinear(Texture2DArray<float4> t, float2 coordinate) {
    float2 c = clamp(coordinate, OutputRect.xy, OutputRect.xy + float2(Extent) - 1);
    int2 lo = int2(c), hi = min(lo + 1, int2(OutputRect.xy + float2(Extent) - 1));
    float2 f = frac(c);
    return lerp(lerp(t.Load(int4(lo, ColorSlice, 0)), t.Load(int4(hi.x, lo.y, ColorSlice, 0)), f.x),
                lerp(t.Load(int4(lo.x, hi.y, ColorSlice, 0)), t.Load(int4(hi, ColorSlice, 0)), f.x),
                f.y);
}
float4 sample_color(float2 coordinate) {
    return bilinear(SourceColor, coordinate);
}
// One eye's cell of a feature texture, sampled at a feature coordinate.
float4 bilinear_cell(Texture2D<float4> t, float2 coordinate) {
    float2 c = clamp(coordinate, float2(EyeX, 0), float2(EyeX, 0) + float2(FeatureExtent) - 1);
    int2 lo = int2(c), hi = min(lo + 1, int2(EyeX, 0) + int2(FeatureExtent) - 1);
    float2 f = frac(c);
    return lerp(lerp(t.Load(int3(lo, 0)), t.Load(int3(hi.x, lo.y, 0)), f.x),
                lerp(t.Load(int3(lo.x, hi.y, 0)), t.Load(int3(hi, 0)), f.x), f.y);
}
// Catmull-Rom weights for the four texels about a sample, from its fraction.
float4 catmull_rom(float f) {
    float f2 = f * f, f3 = f2 * f;
    return float4(-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1,
                  -1.5 * f3 + 2 * f2 + 0.5 * f, 0.5 * f3 - 0.5 * f2);
}
// One eye's cell of a feature texture, sampled at a feature coordinate with a
// Catmull-Rom filter.
float4 cubic_cell(Texture2D<float4> t, float2 coordinate) {
    int2 lo = int2(EyeX, 0), hi = int2(EyeX, 0) + int2(FeatureExtent) - 1;
    float2 c = clamp(coordinate, float2(lo), float2(hi));
    int2 base = int2(floor(c));
    float2 f = c - float2(base);
    float4 wx = catmull_rom(f.x), wy = catmull_rom(f.y);
    float4 sum = 0;
    [unroll] for (int j = 0; j < 4; ++j) {
        float4 row = 0;
        [unroll] for (int i = 0; i < 4; ++i) {
            row += wx[i] * t.Load(int3(clamp(base + int2(i - 1, j - 1), lo, hi), 0));
        }
        sum += wy[j] * row;
    }
    return sum;
}
bool reduced() {
    return any(FeatureExtent != Extent);
}
// The output-pixel coordinate of a feature pixel's centre, within the eye.
float2 eye_position(uint2 p) {
    return (float2(p) + 0.5) * float2(Extent) / float2(FeatureExtent);
}

// Where the content at a point of B's view was in A, in B's camera, as an
// offset in B's UV. NGX dilates the vectors at depth edges itself, as it does
// for a game's own frame generation; doing it here measured no different and
// cost more. Jitter and MV_Scale follow the bridge's existing guide contract.
float2 towards_a(float2 uv) {
    int2 motion_lo = int2(MotionRect.xy);
    int2 motion_hi = max(motion_lo, int2(ceil(MotionRect.xy + MotionRect.zw)) - 1);
    int2 mp = clamp(int2(MotionRect.xy + uv * MotionRect.zw), motion_lo, motion_hi);
    float2 mv = SourceMotion.Load(int4(mp, MotionSlice, 0));
    float2 backward = mv * MotionScale + JitterDelta;
    float2 a_uv = uv + backward / float2(Extent);
    float3 a_ray = float3(lerp(SourceTangents.x, SourceTangents.y, a_uv.x),
                          lerp(SourceTangents.z, SourceTangents.w, a_uv.y), -1);
    float4 inverse_rotation = float4(-Rotation.xyz, Rotation.w);
    float3 b_ray = rotate(a_ray, inverse_rotation);
    return source_uv(b_ray, TargetTangents) - uv;
}

// The point of the nearest surface among a depth texel and its four
// neighbours, as NGX dilates motion at depth edges: an edge pixel moves with
// the surface in front. The full 3x3 measured the same and cost more.
float2 nearest_surface(float2 uv) {
    int2 lo = int2(DepthRect.xy);
    int2 hi = max(lo, int2(ceil(DepthRect.xy + DepthRect.zw)) - 1);
    int2 centre = clamp(int2(DepthRect.xy + uv * DepthRect.zw), lo, hi);
    int2 pick = centre;
    float best = SourceDepth.Load(int4(centre, GuideSlice, 0));
    const int2 neighbours[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    [unroll] for (int i = 0; i < 4; ++i) {
        int2 p = clamp(centre + neighbours[i], lo, hi);
        float d = SourceDepth.Load(int4(p, GuideSlice, 0));
        if (DepthInverted != 0 ? d > best : d < best) {
            best = d;
            pick = p;
        }
    }
    return (float2(pick) + 0.5 - DepthRect.xy) / DepthRect.zw;
}

// Both eyes can share one feature, side by side. Pixels of the cell outside
// the eye - the seam between eyes, or rows below a shorter eye - repeat the
// eye's nearest edge, so NGX sees each eye as if alone.
bool pack_cell(uint3 id, out uint2 cell, out uint2 p) {
    cell = uint2(CellX + id.x, id.y);
    p = uint2(clamp(int2(cell) - int2(EyeX, 0), int2(0, 0), int2(FeatureExtent) - 1));
    return id.x < CellWidth && id.y < CellHeight;
}
// The real frame at a feature pixel: the texel itself, or at a reduced
// resolution the bilinear average about its centre - for half size, exactly
// the four texels it covers.
float4 packed_color(Texture2DArray<float4> t, uint2 p) {
    float4 c;
    if (reduced()) {
        c = display(bilinear(t, OutputRect.xy + eye_position(p) - 0.5));
    } else {
        c = display_texel(t.Load(int4(int2(OutputRect.xy) + int2(p), ColorSlice, 0)));
    }
    return c;
}

// Reseeds the feature with A aligned into B's camera. A reset evaluation reads
// only colour - NGX's output does not change whatever motion and depth it is
// given - so the seed writes neither. Its rotated bilinear reads measure
// faster in wider groups than B's pack does.
[numthreads(16, 8, 1)] void SeedNativeDlssG(uint3 id : SV_DispatchThreadID) {
    uint2 cell, p;
    if (!pack_cell(id, cell, p)) {
        return;
    }
    float2 uv = (float2(p) + 0.5) / float2(FeatureExtent);
    float3 ray = float3(lerp(TargetTangents.x, TargetTangents.y, uv.x),
                        lerp(TargetTangents.z, TargetTangents.w, uv.y), -1);
    float3 previous_ray = rotate(ray, Rotation);
    float2 previous_uv = source_uv(previous_ray, SourceTangents);
    if (previous_ray.z >= -0.00001 || any(previous_uv < 0) || any(previous_uv > 1)) {
        Color[cell] = packed_color(CurrentFallback, p);
    } else {
        Color[cell] = display(sample_color(OutputRect.xy + previous_uv * float2(Extent) - 0.5));
    }
}

[numthreads(8, 8, 1)] void PackNativeDlssG(uint3 id : SV_DispatchThreadID) {
    uint2 cell, p;
    if (!pack_cell(id, cell, p)) {
        return;
    }
    Color[cell] = packed_color(SourceColor, p);
    // Motion and depth may sit on a coarser grid. The cell holding a guide
    // texel's centre writes it, sampling the game's guides at that centre.
    uint2 g = uint2((float2(cell) + 0.5) * GuideScale);
    if (any(uint2((float2(g) + 0.5) / GuideScale) != cell)) {
        return;
    }
    float2 q = clamp((float2(g) + 0.5) / GuideScale - float2(EyeX, 0), 0.5,
                     float2(FeatureExtent) - 0.5);
    float2 uv = q / float2(FeatureExtent);
    // A rotated edge can land exactly at UV 1. Fractional guide rectangles
    // also occur when the color viewport is smaller than the guide output.
    int2 depth_lo = int2(DepthRect.xy);
    int2 depth_hi = max(depth_lo, int2(ceil(DepthRect.xy + DepthRect.zw)) - 1);
    int2 dp = clamp(int2(DepthRect.xy + uv * DepthRect.zw), depth_lo, depth_hi);
    // As a fraction of the whole feature, which may hold both eyes.
    Motion[g] = towards_a(uv) * float2(FeatureExtent) * MotionNormal;
    // Dilation chooses motion, while depth remains at the original pixel.
    Depth[g] = SourceDepth.Load(int4(dp, GuideSlice, 0));
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
    if (!reduced()) {
        float3 generated = Generated.Load(int3(p - int2(OutputRect.xy) + int2(EyeX, 0), 0)).rgb;
        // An sRGB target encodes only approximately. Decoding the nearest 8-bit
        // code, rather than a 10-bit value between two, keeps unmoved pixels exact.
        return float4(EncodeSrgb != 0 ? decode_srgb(round(generated * 255.0) / 255.0) : generated,
                      current.a);
    }
    // A reduced-resolution feature's frame is upsampled, losing the real
    // frames' detail above that resolution. The engine motion says where in B
    // each generated pixel's content is: B's texel there, less B as packed
    // there, is that detail, added back where the generated frame agrees with
    // the packed B. Where they disagree - an occlusion, or content the vectors
    // do not describe - the pixel stays as generated.
    float2 x = input.position.xy - OutputRect.xy;
    // Catmull-Rom keeps the generated frame's edges sharper than bilinear,
    // which measured 7% less error for no measurable cost. The packed B it is
    // compared with stays bilinear: filtering both alike measured worse.
    float3 generated = cubic_cell(Generated, x * float2(FeatureExtent) / float2(Extent) - 0.5 +
                                                 float2(EyeX, 0)).rgb;
    float2 uv = x / float2(Extent);
    // The content at x came from y = x - TowardsA * motion(y); two steps of
    // that fixed point follow the motion field across most of an edge. The
    // second step takes the nearest surface's motion, which cut the error at
    // moving edges by a tenth; in the first step it made no difference.
    float2 y = uv - TowardsA * towards_a(uv);
    y = uv - TowardsA * towards_a(nearest_surface(saturate(y)));
    int2 texel = clamp(int2(y * float2(Extent)), int2(0, 0), int2(Extent) - 1);
    float2 centre = (float2(texel) + 0.5) * float2(FeatureExtent) / float2(Extent) - 0.5 +
                    float2(EyeX, 0);
    float3 low = bilinear_cell(Packed, centre).rgb;
    float3 high = display(SourceColor.Load(int4(int2(OutputRect.xy) + texel, ColorSlice, 0))).rgb;
    float difference = max(max(abs(generated.r - low.r), abs(generated.g - low.g)),
                           abs(generated.b - low.b));
    float weight = DetailFalloff > 0 ? saturate(1 - difference * DetailFalloff) : 0;
    float3 restored = generated + (high - low) * weight;
    return float4(EncodeSrgb != 0 ? decode_srgb(restored) : saturate(restored), current.a);
}

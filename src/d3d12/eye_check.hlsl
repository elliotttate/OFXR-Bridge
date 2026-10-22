// Which DLSS stream made which eye's image (D3D12EyeCheck). Each eye's
// captured image is correlated with the tile each evaluation copied of its
// output, over the tile, on the logarithm of the luminance: the tile is scene
// colour before the tone map and the image the picture after it, and a
// monotonic map is near enough affine on a log scale for a correlation to see
// through. The same eye correlates closely; the other eye's tile is off by the
// parallax of whatever is near, so the wrong pairing correlates worse wherever
// near things are in the tile - which is where the wrong guides would hurt.
// A tile of far things alone correlates either way and decides nothing.
Texture2DArray<float4> Image : register(t0);
Texture2D<float4> TileA : register(t1);
Texture2D<float4> TileB : register(t2);
RWByteAddressBuffer Result : register(u0);

cbuffer Params : register(b0) {
    // Per view: its rectangle in the image, x, y, width, height.
    uint4 ViewRect[2];
    // Per view: its array slice.
    uint2 ViewSlice;
    // The tiles' size, the same for both streams.
    uint2 TileSize;
    // Per stream: the tile's origin in the output rectangle - A.x, A.y, B.x,
    // B.y - and the output rectangle's size, A.w, A.h, B.w, B.h, which the
    // view's rectangle maps onto.
    uint4 TileOrigin;
    uint4 OutputSize;
};

static const uint kColumns = 64;
static const uint kRows = 32;
static const uint kSamples = kColumns * kRows;
static const uint kThreads = 256;
// Per pairing (view 0 or 1 with stream A or B): the sums of the image's
// values, the tile's, their squares and their product.
static const uint kPairings = 4;
static const uint kSums = 5;

groupshared float g_partial[kPairings * kSums][kThreads];

float log_luma(float4 color) {
    const float3 weights = float3(0.2126, 0.7152, 0.0722);
    return log(max(dot(color.rgb, weights), 0.0) + 0.001);
}

float tile_value(uint stream, int2 texel) {
    return stream == 0u ? log_luma(TileA.Load(int3(texel, 0)))
                        : log_luma(TileB.Load(int3(texel, 0)));
}

float image_value(uint view, uint stream, uint2 texel) {
    // The tile's texel in the output rectangle, taken to the view's.
    float2 output = float2(TileOrigin[2u * stream], TileOrigin[2u * stream + 1u]) +
        float2(texel) + 0.5;
    float2 size = float2(OutputSize[2u * stream], OutputSize[2u * stream + 1u]);
    float2 scale = float2(ViewRect[view].zw) / max(size, float2(1.0, 1.0));
    float2 position = float2(ViewRect[view].xy) + output * scale;
    int2 image_texel = clamp(int2(position), int2(ViewRect[view].xy),
        int2(ViewRect[view].xy + ViewRect[view].zw) - int2(1, 1));
    return log_luma(Image.Load(int4(image_texel, int(ViewSlice[view]), 0)));
}

[numthreads(kThreads, 1, 1)]
void Compare(uint3 thread : SV_GroupThreadID) {
    float sums[kPairings * kSums];
    [unroll] for (uint i = 0; i < kPairings * kSums; ++i) sums[i] = 0.0;

    for (uint sample = thread.x; sample < kSamples; sample += kThreads) {
        uint column = sample % kColumns;
        uint row = sample / kColumns;
        uint2 texel = uint2((column * TileSize.x + TileSize.x / 2u) / kColumns,
                            (row * TileSize.y + TileSize.y / 2u) / kRows);
        texel = min(texel, TileSize - uint2(1u, 1u));
        [unroll] for (uint stream = 0; stream < 2; ++stream) {
            float tile = tile_value(stream, int2(texel));
            [unroll] for (uint view = 0; view < 2; ++view) {
                float image = image_value(view, stream, texel);
                uint base = (view * 2u + stream) * kSums;
                sums[base + 0] += image;
                sums[base + 1] += tile;
                sums[base + 2] += image * image;
                sums[base + 3] += tile * tile;
                sums[base + 4] += image * tile;
            }
        }
    }
    [unroll] for (uint j = 0; j < kPairings * kSums; ++j) g_partial[j][thread.x] = sums[j];
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = kThreads / 2u; stride > 0u; stride >>= 1u) {
        if (thread.x < stride) {
            [unroll] for (uint k = 0; k < kPairings * kSums; ++k) {
                g_partial[k][thread.x] += g_partial[k][thread.x + stride];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (thread.x == 0u) {
        const float n = float(kSamples);
        // Per pairing: the correlation, then the image's variance; after
        // those, each tile's variance. A flat image or tile decides nothing.
        [unroll] for (uint p = 0; p < kPairings; ++p) {
            float sx = g_partial[p * kSums + 0][0];
            float sy = g_partial[p * kSums + 1][0];
            float sxx = g_partial[p * kSums + 2][0];
            float syy = g_partial[p * kSums + 3][0];
            float sxy = g_partial[p * kSums + 4][0];
            float var_x = n * sxx - sx * sx;
            float var_y = n * syy - sy * sy;
            float covariance = n * sxy - sx * sy;
            float correlation = covariance / sqrt(max(var_x, 1.0e-6) * max(var_y, 1.0e-6));
            Result.Store(p * 4u, asuint(correlation));
            Result.Store((kPairings + p) * 4u, asuint(var_x / (n * n)));
        }
        [unroll] for (uint s = 0; s < 2; ++s) {
            // The tile's variance is the same for both views; take view 0's.
            float sy = g_partial[s * kSums + 1][0];
            float syy = g_partial[s * kSums + 3][0];
            Result.Store((2u * kPairings + s) * 4u, asuint((n * syy - sy * sy) / (n * n)));
        }
    }
}

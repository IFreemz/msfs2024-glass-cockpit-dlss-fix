// Post-DLSS cockpit display overlay. Runs on the DLSS command list right after the upscale:
//   zero      - one-time reset of the fit accumulators and per-display parameters
//   fit       - per display, least-squares fit of rendered colour = a * display_texture + b (render resolution),
//               plus per pixel the remainder (scene - a * texture): dirt, sun glare, reflections, shadow edges
//   solve     - turns the sums into a, b per display and smooths them over time
//   composite - redraws the display texture at output resolution over the DLSS output (no history -> no ghosting).
//               The glass layer is taken from the DLSS output itself (sharp dirt/shadows), clamped to this frame's
//               remainder range around the pixel, which removes ghost trails of the old screen content (TAA-style clamp).
//   clear     - empties the UV map for the next frame

cbuffer Constants : register(b0)
{
    uint2 map_size;
    uint2 output_size;
    float2 map_per_output;   // render pixels per output pixel
    float2 jitter;           // offset added to map coordinates to undo this frame's camera jitter
    float smoothing;         // weight of the new fit per frame
    uint flags;              // bit 0: reset smoothing, bit 1: debug green tint, bit 2: DLSS output readable (typed UAV load),
                             // bit 3: smooth lines
    float line_width;        // smooth lines: footprint scale, percent / 50 (1 = DLSS-like, the 50 % maximum)
    uint padding;
};

Texture2D<float4> map : register(t0);       // xy = display uv, z = depth, w = slot + 1 (0 = not a display)
Texture2D<float4> scene : register(t1);     // DLSS input colour (render resolution, pre-exposed linear)
Texture2D<float4> displays[32] : register(t2);
RWTexture2D<float4> output : register(u0);  // DLSS output, R11G11B10_FLOAT
RWTexture2D<uint> tiles : register(u1);     // per 8x8 map tile: display slot + 1 this frame (0 = none)
RWStructuredBuffer<float4> params : register(u2); // per slot: [2s] = a.rgb, valid; [2s+1] = b.rgb, samples
RWTexture2D<float4> map_rw : register(u3);
RWByteAddressBuffer sums : register(u4);    // per slot 13 x uint64 fixed point
RWTexture2D<float4> residual : register(u5); // per render pixel: scene - a * display, w = slot + 1 (0 = none)
SamplerState display_sampler : register(s0);

static const uint slot_count = 32;
static const uint terms = 13;
static const float fixed_scale = 16777216.0;

float3 srgb_to_linear(float3 c)
{
    // Matches the conversion in the game's display shader.
    return select(c <= 0.04045, c / 12.92, pow(c * 0.947867 + 0.0521327, 2.4));
}

// The game averages four half-footprint taps (SampleBias -1) before linearising; do the same at output scale.
float3 sample_display(uint slot, float2 uv, float2 dx, float2 dy)
{
    static const float2 offsets[4] = { float2(0.125, 0.375), float2(0.375, -0.125), float2(-0.125, -0.375), float2(-0.375, 0.125) };
    float3 sum = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
        sum += displays[NonUniformResourceIndex(slot)].SampleGrad(display_sampler, uv + offsets[i].x * dx + offsets[i].y * dy, dx * 0.5, dy * 0.5).rgb;
    return srgb_to_linear(saturate(sum * 0.25));
}

// Smooth lines: what DLSS converges to on a still screen, without its history. DLSS averages each pixel over the area of
// one jittered render pixel across frames; this averages a 4x4 grid over that area (at least 1.25 output pixels) in one
// frame, so stair steps on thin slanted lines in the screen texture are smoothed instead of shown texel-sharp.
float3 sample_display_smooth(uint slot, float2 uv, float2 dx, float2 dy)
{
    float2 width = max(1.0 / map_per_output, 1.25) * line_width;
    float2 fx = dx * width.x, fy = dy * width.y;
    float3 sum = 0;
    [unroll] for (uint i = 0; i < 16; ++i)
    {
        // Rotated 4x4 grid: no two samples share a row or column, so near-horizontal and near-vertical lines get
        // 16 distinct coverage levels.
        float2 o = (float2(i % 4, i / 4) + float2(((i / 4) * 0.25 + 0.125), ((i % 4) * 0.25 + 0.125))) / 4.0 - 0.5;
        sum += displays[NonUniformResourceIndex(slot)].SampleGrad(display_sampler, uv + o.x * fx + o.y * fy, fx * 0.25, fy * 0.25).rgb;
    }
    return srgb_to_linear(saturate(sum / 16.0));
}

uint map_slot(float4 m)
{
    return m.w > 0.5 && m.w < slot_count + 0.5 ? uint(m.w + 0.5) - 1 : slot_count;
}

[numthreads(64, 1, 1)]
void zero(uint3 id : SV_DispatchThreadID)
{
    if (id.x < slot_count * terms) sums.Store<uint64_t>(id.x * 8, 0);
    if (id.x < slot_count * 2) params[id.x] = 0;
}

groupshared uint tile_slot;

void accumulate(uint slot, float3 x, float3 y)
{
    uint base = slot * terms * 8;
    uint count = WaveActiveCountBits(true);
    float3 sx = WaveActiveSum(x), sy = WaveActiveSum(y), sxx = WaveActiveSum(x * x), sxy = WaveActiveSum(x * y);
    if (WaveIsFirstLane())
    {
        sums.InterlockedAdd64(base, uint64_t(count));
        float values[12] = { sx.r, sx.g, sx.b, sy.r, sy.g, sy.b, sxx.r, sxx.g, sxx.b, sxy.r, sxy.g, sxy.b };
        [unroll] for (uint i = 0; i < 12; ++i)
            sums.InterlockedAdd64(base + (i + 1) * 8, uint64_t(max(values[i], 0.0) * fixed_scale));
    }
}

float4 load_map(int2 p)
{
    return all(p >= 0) && all(p < int2(map_size)) ? map.Load(int3(p, 0)) : 0;
}

[numthreads(8, 8, 1)]
void fit(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    if (index == 0) tile_slot = 0xffffffff;
    GroupMemoryBarrierWithGroupSync();
    int2 p = int2(id.xy);
    float4 m = load_map(p);
    uint slot = map_slot(m);
    if (slot < slot_count) InterlockedMin(tile_slot, slot);

    bool active = slot < slot_count;
    float3 x = 0, y = 0;
    if (active)
    {
        // UV footprint of this render pixel from neighbours on the same display.
        float4 r = load_map(p + int2(1, 0)), l = load_map(p - int2(1, 0));
        float4 d = load_map(p + int2(0, 1)), u = load_map(p - int2(0, 1));
        float2 dx = map_slot(r) == slot ? r.xy - m.xy : map_slot(l) == slot ? m.xy - l.xy : 0;
        float2 dy = map_slot(d) == slot ? d.xy - m.xy : map_slot(u) == slot ? m.xy - u.xy : 0;
        x = sample_display(slot, m.xy, dx, dy);
        y = scene.Load(int3(p, 0)).rgb;
        active = all(isfinite(y));
        y = clamp(y, 0, 1024);
    }
    GroupMemoryBarrierWithGroupSync();

    // Per-pixel remainder against last frame's gain (glass layer: dirt, glare, reflections, shadows).
    float4 gain = active ? params[slot * 2] : 0;
    if (all(p < int2(map_size))) residual[p] = active && gain.w > 0 ? float4(y - gain.rgb * x, slot + 1) : 0;
    if (index == 0 && all(group.xy < (map_size + 7) / 8)) tiles[group.xy] = tile_slot < slot_count ? tile_slot + 1 : 0;

    // Waterfall: each pass handles the lanes of one display slot.
    bool done = !active;
    while (WaveActiveAnyTrue(!done))
    {
        if (!done)
        {
            uint first = WaveReadLaneFirst(slot);
            if (slot == first)
            {
                accumulate(slot, x, y);
                done = true;
            }
        }
    }
}

[numthreads(slot_count, 1, 1)]
void solve(uint3 id : SV_DispatchThreadID)
{
    uint slot = id.x, base = slot * terms * 8;
    double v[terms];
    [unroll] for (uint i = 0; i < terms; ++i)
    {
        uint64_t raw = sums.Load<uint64_t>(base + i * 8);
        v[i] = i == 0 ? double(raw) : double(raw) / fixed_scale;
        sums.Store<uint64_t>(base + i * 8, 0);
    }
    float4 previous_a = params[slot * 2], previous_b = params[slot * 2 + 1];
    if (flags & 1) previous_a.w = 0;
    double n = v[0];
    if (n < 64) { params[slot * 2] = previous_a; return; }
    float3 a, b;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        double mx = v[1 + c] / n, my = v[4 + c] / n;
        double variance = v[7 + c] / n - mx * mx, covariance = v[10 + c] / n - mx * my;
        double gain = variance > 1e-6 ? max(covariance / variance, 0.0) : (mx > 1e-4 ? my / mx : 0.0);
        a[c] = float(gain);
        b[c] = float(my - gain * mx);
    }
    float w = previous_a.w > 0 ? smoothing : 1;
    params[slot * 2] = float4(lerp(previous_a.rgb, a, w), 1);
    params[slot * 2 + 1] = float4(lerp(previous_b.rgb, b, w), float(n));
}

bool tap(int2 p, out float4 value)
{
    value = load_map(p);
    return map_slot(value) < slot_count;
}

[numthreads(8, 8, 1)]
void composite(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= output_size)) return;
    float2 location = (float2(id.xy) + 0.5) * map_per_output - 0.5 + jitter;
    int2 cell = int2(floor(location));
    if (any(cell < 0) || any(cell + 1 >= int2(map_size))) return;
    uint2 tile_size = (map_size + 7) / 8;
    if (!tiles[uint2(cell) / 8] && !tiles[min(uint2(cell + 1) / 8, tile_size - 1)]) return;
    float4 a, b, c, d;
    if (!tap(cell, a) || !tap(cell + int2(1, 0), b) || !tap(cell + int2(0, 1), c) || !tap(cell + int2(1, 1), d)) return;
    // Never blend across a display edge.
    if (a.w != b.w || a.w != c.w || a.w != d.w) return;
    uint slot = map_slot(a);
    float4 gain = params[slot * 2], offset = params[slot * 2 + 1];
    if (gain.w <= 0) return;
    float2 f = location - cell;
    float2 uv = lerp(lerp(a.xy, b.xy, f.x), lerp(c.xy, d.xy, f.x), f.y);
    float2 dx = lerp(b.xy - a.xy, d.xy - c.xy, f.y) * map_per_output.x;
    float2 dy = lerp(c.xy - a.xy, d.xy - b.xy, f.x) * map_per_output.y;
    float3 content = ((flags & 8) ? sample_display_smooth(slot, uv, dx, dy) : sample_display(slot, uv, dx, dy)) * gain.rgb;

    // Glass layer (dirt, glare, reflections, shadows) = this frame's remainder: bilinear value at this pixel and its
    // spread over the 3x3 render pixels around it (same display only).
    float3 smooth = 0, m1 = 0, m2 = 0;
    float smooth_weight = 0, count = 0;
    int2 centre = int2(round(location));
    [unroll] for (int j = 0; j < 9; ++j)
    {
        int2 q = centre + int2(j % 3 - 1, j / 3 - 1);
        if (any(q < 0) || any(q >= int2(map_size))) continue;
        float4 rv = residual[q];
        if (uint(rv.w + 0.5) != slot + 1) continue;
        m1 += rv.rgb; m2 += rv.rgb * rv.rgb; count += 1;
    }
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        int2 q = cell + int2(k & 1, k >> 1);
        float4 rv = residual[q];
        if (uint(rv.w + 0.5) != slot + 1) continue;
        float w = ((k & 1) ? f.x : 1 - f.x) * ((k >> 1) ? f.y : 1 - f.y);
        smooth += w * rv.rgb;
        smooth_weight += w;
    }
    float3 glass = offset.rgb;
    if (smooth_weight > 1e-3)
    {
        glass = smooth / smooth_weight;
        if ((flags & 4) && count > 0)
        {
            // DLSS output minus the clean content = sharp glass detail + ghost of old content. Keep it only within the
            // local spread of this frame's glass layer (variance clipping, as in TAA); ghost trails fall outside.
            float3 mean = m1 / count, spread = sqrt(max(m2 / count - mean * mean, 0)) + 0.002;
            glass = clamp(output[id.xy].rgb - content, glass - spread, glass + spread);
        }
    }
    float3 colour = content + glass;
    if (flags & 2) colour = colour * float3(0.4, 1.0, 0.4) + float3(0, 0.02, 0);
    if (all(isfinite(colour))) output[id.xy] = float4(max(colour, 0), 1);
}

[numthreads(8, 8, 1)]
void clear(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    if (all(id.xy < map_size) && all(group.xy < (map_size + 7) / 8) && tiles[group.xy]) map_rw[id.xy] = 0;
}

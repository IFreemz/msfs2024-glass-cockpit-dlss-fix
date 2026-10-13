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
                             // bit 3: smooth lines, bits 4-6: unused
    float line_width;        // smooth lines: footprint scale, percent / 50 (1 = DLSS-like, the 50 % maximum)
    uint layer;              // map layer of this upscaler call (VR: one layer per view, in call order)
    float glass_strength;    // share of the glass layer (dirt, glare, reflections, shadows) kept, 0..1 (Advanced setting)
    float edge_fade;         // experimental: fade into the upscaler's picture over this many render pixels at display edges
    float edge_stretch;      // experimental: extend displays outwards by up to this many render pixels
    float detail_clamp;      // experimental: 0..1, keep the redrawn screen within the sim's own local brightness range this frame
    float angle_soften;      // experimental: 0..1, sample screens seen at a steep angle like the sim does (softer)
};

Texture2DArray<float4> map : register(t0);       // xy = display uv, z = depth, w = slot + 1 (0 = not a display)
Texture2D<float4> scene : register(t1);     // DLSS input colour (render resolution, pre-exposed linear)
Texture2D<float4> displays[32] : register(t2);
RWTexture2D<float4> output : register(u0);  // DLSS output, R11G11B10_FLOAT
RWTexture2D<uint> tiles : register(u1);     // per 8x8 map tile: display slot + 1 this frame (0 = none)
RWStructuredBuffer<float4> params : register(u2); // per slot: [3s] = a.rgb, valid; [3s+1] = b.rgb, samples; [3s+2] = mean screen.rgb, fit quality
RWTexture2DArray<float4> map_rw : register(u3);
RWByteAddressBuffer sums : register(u4);    // per slot 20 x uint64 fixed point
RWTexture2D<float4> residual : register(u5); // per render pixel: scene - a * display, w = slot + 1 (0 = none)
SamplerState display_sampler : register(s0);

static const uint slot_count = 32;
static const uint terms = 20;
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
    for (uint i = id.x; i < slot_count * terms; i += 64) sums.Store<uint64_t>(i * 8, 0);
    for (uint j = id.x; j < slot_count * 3; j += 64) params[j] = 0;
}

groupshared uint tile_slot;

// detail: pooled products of neighbour differences (screen x, scene y): xx, yy, xy (fine detail only, see solve).
void accumulate(uint slot, float3 x, float3 y, float3 detail)
{
    uint base = slot * terms * 8;
    uint count = WaveActiveCountBits(true);
    float3 sx = WaveActiveSum(x), sy = WaveActiveSum(y), sxx = WaveActiveSum(x * x), sxy = WaveActiveSum(x * y), syy = WaveActiveSum(y * y);
    float3 sd = WaveActiveSum(float3(detail.x, detail.y, max(detail.z, 0))), sd_negative = WaveActiveSum(max(-detail.z, 0));
    if (WaveIsFirstLane())
    {
        sums.InterlockedAdd64(base, uint64_t(count));
        float values[19] = { sx.r, sx.g, sx.b, sy.r, sy.g, sy.b, sxx.r, sxx.g, sxx.b, sxy.r, sxy.g, sxy.b, syy.r / 1024, syy.g / 1024, syy.b / 1024, // y*y scaled: fixed point headroom
            sd.x, sd.y / 1024, sd.z, sd_negative.x };
        [unroll] for (uint i = 0; i < 19; ++i)
            sums.InterlockedAdd64(base + (i + 1) * 8, uint64_t(max(values[i], 0.0) * fixed_scale));
    }
}

float4 load_map(int2 p)
{
    return all(p >= 0) && all(p < int2(map_size)) ? map.Load(int4(p, layer, 0)) : 0;
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
    float3 x = 0, y = 0, detail = 0;
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
        // Fine detail: differences to the right and lower neighbour on the same display. Sun glare and other smooth
        // light on the glass hardly changes these, so dark screens with thin symbols still score well in the sun.
        [unroll] for (uint k = 0; k < 2; ++k)
        {
            float4 n = k ? d : r;
            int2 np = p + int2(1 - k, k);
            if (map_slot(n) != slot) continue;
            float3 hy = clamp(scene.Load(int3(np, 0)).rgb, 0, 1024) - y;
            float3 hx = sample_display(slot, n.xy, dx, dy) - x;
            if (all(isfinite(hy))) detail += float3(dot(hx, hx), dot(hy, hy), dot(hx, hy));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Per-pixel remainder against last frame's gain (glass layer: dirt, glare, reflections, shadows).
    float4 gain = active ? params[slot * 3] : 0;
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
                accumulate(slot, x, y, detail);
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
    float4 previous_a = params[slot * 3], previous_b = params[slot * 3 + 1], previous_q = params[slot * 3 + 2];
    if (flags & 1) previous_a.w = 0;
    double n = v[0];
    if (n < 64) { params[slot * 3] = previous_a; return; }
    // Per channel: scene = gain * screen + offset (least squares).
    double mx[3], my[3], variance[3], covariance[3], pooled_variance = 0, pooled_covariance = 0, pooled_scene_variance = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        mx[c] = v[1 + c] / n; my[c] = v[4 + c] / n;
        variance[c] = v[7 + c] / n - mx[c] * mx[c]; covariance[c] = v[10 + c] / n - mx[c] * my[c];
        pooled_variance += variance[c]; pooled_covariance += covariance[c]; pooled_scene_variance += max(v[13 + c] * 1024 / n - my[c] * my[c], 0.0);
    }
    float3 a, b;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        a[c] = variance[c] > 1e-6 ? float(max(covariance[c] / variance[c], 0.0)) : (mx[c] > 1e-4 ? float(my[c] / mx[c]) : 0);
        b[c] = float(my[c] - a[c] * mx[c]);
    }
    // Fit quality: how much of the scene's variation on the screen the screen texture explains (r squared, all
    // channels pooled). Healthy screens score about 0.3 to 1 (glare on the glass lowers it), a picture the texture
    // doesn't hold below 0.1: if the sim shows something the texture doesn't (e.g. a tablet
    // whose picture is drawn differently), redrawing the texture would show wrong colours, so that screen is left to
    // the upscaler. A screen texture with no contrast (blank, or a tablet whose picture isn't in it) has no moving
    // content to keep sharp, so it is left to the upscaler too.
    double quality = pooled_variance > 3e-6 && pooled_scene_variance > 1e-9 && pooled_covariance > 0
        ? pooled_covariance * pooled_covariance / (pooled_variance * pooled_scene_variance) : 0.0;
    // The same on fine detail only. Sun glare across a mostly dark screen dwarfs its thin symbols and drags the score
    // above down, but barely touches the detail; a picture the texture doesn't hold still scores low on both.
    double detail_xx = v[16], detail_yy = v[17] * 1024, detail_xy = v[18] - v[19];
    if (pooled_variance > 3e-6 && detail_xx > 0 && detail_yy > 0 && detail_xy > 0)
        quality = max(quality, detail_xy * detail_xy / (detail_xx * detail_yy));
    float w = previous_a.w > 0 ? smoothing : 1;
    float q = previous_a.w != 0 ? lerp(previous_q.w, float(quality), smoothing) : float(quality);
    bool keep = previous_a.w < 0 ? q > 0.25 : q > 0.15; // hysteresis: no flicker near the limit
    params[slot * 3] = float4(lerp(previous_a.rgb, a, w), keep ? 1 : -1); // w: valid (1), rejected (-1)
    params[slot * 3 + 1] = float4(lerp(previous_b.rgb, b, w), float(n));
    params[slot * 3 + 2] = float4(float3(mx[0], mx[1], mx[2]), q);
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
    bool covered = tap(cell, a) & tap(cell + int2(1, 0), b) & tap(cell + int2(0, 1), c) & tap(cell + int2(1, 1), d);
    float2 f = location - cell;
    float2 uv, dx, dy;
    bool filled = false;
    if (covered && a.w == b.w && a.w == c.w && a.w == d.w)
    {
        uv = lerp(lerp(a.xy, b.xy, f.x), lerp(c.xy, d.xy, f.x), f.y);
        dx = lerp(b.xy - a.xy, d.xy - c.xy, f.y) * map_per_output.x;
        dy = lerp(c.xy - a.xy, d.xy - b.xy, f.x) * map_per_output.y;
    }
    else
    {
        // Edge stretcher (experimental): pixels at a display's edge are normally left to the upscaler. Fill them when a
        // display lies within edge_stretch render pixels (pushes edges outwards), from the nearest display render
        // pixel, its texture coordinates continued along its gradients.
        float reach = edge_stretch;
        if (reach <= 0) return;
        int radius = min(int(ceil(reach + 0.5)), 4);
        float nearest = 1e9;
        int2 nearest_q = 0;
        [loop] for (int y = -radius; y <= radius + 1; ++y)
            [loop] for (int x = -radius; x <= radius + 1; ++x)
            {
                int2 q = cell + int2(x, y);
                float4 v = load_map(q);
                if (map_slot(v) >= slot_count) continue;
                float dist = length(float2(q) - location);
                if (dist < nearest) { nearest = dist; nearest_q = q; }
            }
        if (nearest > reach + 0.5) return;
        float4 m = load_map(nearest_q);
        if (nearest > edge_stretch) return;
        float4 r = load_map(nearest_q + int2(1, 0)), l = load_map(nearest_q - int2(1, 0));
        float4 down = load_map(nearest_q + int2(0, 1)), up = load_map(nearest_q - int2(0, 1));
        float2 gx = r.w == m.w ? r.xy - m.xy : l.w == m.w ? m.xy - l.xy : 0;
        float2 gy = down.w == m.w ? down.xy - m.xy : up.w == m.w ? m.xy - up.xy : 0;
        float2 o = location - float2(nearest_q);
        uv = m.xy + o.x * gx + o.y * gy;
        dx = gx * map_per_output.x;
        dy = gy * map_per_output.y;
        a = m;
        filled = true;
    }
    uint slot = map_slot(a);

    // Edge fade: the display's edge in the map moves with the camera jitter every frame, and a hard cut there crawls
    // (and shows thin lines some aircraft have at the edge of their screen picture sharper than the sim does). Fade
    // from the upscaler's (temporally smoothed) picture to the redrawn screen over the outer edge_fade render pixels.
    // Distance to the edge from the share of a tent (radius 3 render pixels) around this pixel that lies on this
    // display: about 2/3 at the outermost redrawn pixel, 1 from two render pixels in. Needs the upscaler output readable.
    float edge = 1;
    if (!filled && edge_fade > 0 && (flags & 4))
    {
        float inside = 0;
        [unroll] for (int t = 0; t < 36; ++t)
        {
            int2 q = cell + int2(t % 6 - 2, t / 6 - 2);
            float2 dist = abs(float2(q) - location);
            if (load_map(q).w == a.w) inside += max(3 - dist.x, 0) * max(3 - dist.y, 0);
        }
        float inside_pixels = saturate((inside / 81 - 2.0 / 3) * 3) * 2;
        // Wider fades: the tent only measures up to 2 render pixels; further in, look for the edge along 8 directions.
        if (inside_pixels >= 2 && edge_fade > 2)
        {
            static const float2 directions[8] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
                float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071), float2(-0.7071, -0.7071) };
            float nearest = 13;
            int reach = min(int(ceil(edge_fade)) + 1, 13);
            [loop] for (int r = 3; r <= reach && nearest > 12; ++r)
                [unroll] for (int k = 0; k < 8; ++k)
                    if (load_map(int2(round(location + directions[k] * r))).w != a.w) nearest = min(nearest, r - 0.5);
            inside_pixels = nearest;
        }
        edge = saturate(inside_pixels / edge_fade);
    }
    float blend = edge;
    if (blend <= 0) return;
    float4 gain = params[slot * 3], offset = params[slot * 3 + 1];
    if (gain.w <= 0) return;
    // Angle softening: on a screen seen at a steep angle the texture footprint of a pixel is long and thin. Anisotropic
    // filtering at output resolution keeps one-texel lines there crisp (they break into dashes); the sim samples at
    // render resolution and its picture is soft there. Move towards an isotropic footprint the size of a render pixel,
    // more the steeper the angle (anisotropy 1.5 -> 4). Screens seen straight on are unchanged.
    if (angle_soften > 0)
    {
        float lx = length(dx), ly = length(dy);
        float major = max(lx, ly), minor = max(min(lx, ly), 1e-9);
        float t = angle_soften * saturate((major / minor - 1.5) / 2.5);
        if (t > 0)
        {
            float2 render_scale = 1 / max(map_per_output, 1e-3);
            float2 sim_dx = (lx > 0 ? dx / lx : float2(1, 0)) * major * render_scale.x;
            float2 sim_dy = (ly > 0 ? dy / ly : float2(0, 1)) * major * render_scale.y;
            dx = lerp(dx, sim_dx, t);
            dy = lerp(dy, sim_dy, t);
        }
    }
    float3 content = ((flags & 8) ? sample_display_smooth(slot, uv, dx, dy) : sample_display(slot, uv, dx, dy)) * gain.rgb;

    // Glass layer (dirt, glare, reflections, shadows) = this frame's remainder: bilinear value at this pixel and its
    // spread over the 3x3 render pixels around it (same display only).
    float3 smooth = 0, m1 = 0, m2 = 0, scene_low = 1e9, scene_high = -1e9;
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
    // Detail clamp range: what the sim drew in the 5x5 render pixels around this one (same display). A wider window
    // than 3x3 changes less from frame to frame as the jitter moves, so the clamp doesn't flicker.
    if (detail_clamp > 0)
        [unroll] for (int s = 0; s < 25; ++s)
        {
            int2 q = centre + int2(s % 5 - 2, s / 5 - 2);
            if (any(q < 0) || any(q >= int2(map_size)) || uint(residual[q].w + 0.5) != slot + 1) continue;
            float3 y = scene.Load(int3(q, 0)).rgb;
            if (all(isfinite(y))) { scene_low = min(scene_low, y); scene_high = max(scene_high, y); }
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
    // Detail clamp: the redrawn screen may not be brighter or darker than what the sim itself drew in the 5x5 render
    // pixels around this one this frame. Content the sim shows survives; texture detail too thin for the sim to show
    // (e.g. a one-texel line where a screen's picture meets its bezel band) is held to the sim's level. Done with the
    // full glass layer, which the sim's picture contains, before the glass layer setting takes its share away.
    if (detail_clamp > 0 && count > 0 && all(scene_high >= scene_low))
    {
        float3 margin = (scene_high - scene_low) * 0.1 + 0.002;
        colour = lerp(colour, clamp(colour, scene_low - margin, scene_high + margin), detail_clamp);
    }
    if (glass_strength < 1) colour -= (1 - glass_strength) * (glass - offset.rgb);
    if (flags & 2) colour = colour * float3(0.4, 1.0, 0.4) + float3(0, 0.02, 0);
    if (blend < 1) colour = lerp(output[id.xy].rgb, colour, blend * blend * (3 - 2 * blend));
    if (all(isfinite(colour))) output[id.xy] = float4(max(colour, 0), 1);
}

[numthreads(8, 8, 1)]
void clear(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    if (all(id.xy < map_size) && all(group.xy < (map_size + 7) / 8) && tiles[group.xy]) map_rw[uint3(id.xy, layer)] = 0;
}

// Post-DLSS cockpit display overlay. Runs on the DLSS command list right after the upscale:
//   zero      - one-time reset of the fit accumulators and per-display parameters
//   fit       - per display, least-squares fit of rendered colour = a * display_texture + b (render resolution),
//               plus per pixel the remainder (scene - a * texture): dirt, sun glare, reflections, shadow edges
//   solve     - turns the sums into a, b per display and smooths them over time
//   composite - redraws the display texture at output resolution over the DLSS output (no history -> no ghosting;
//               the optional temporal anti-aliasing keeps a history that is dropped wherever the screen changes).
//               The glass layer is taken from the DLSS output itself (sharp dirt/shadows), clamped to this frame's
//               remainder range around the pixel, which removes ghost trails of the old screen content (TAA-style clamp).
//   clear     - empties the UV map for the next frame
// Screen edges: where each screen ends is learnt in its texture (edge_bounds), where the camera jitter doesn't move
// it, so edge pixels get a steady partial coverage instead of flipping between the fix and the upscaler every frame.

cbuffer Constants : register(b0)
{
    uint2 map_size;
    uint2 output_size;
    float2 map_per_output;   // render pixels per output pixel
    float2 jitter;           // offset added to map coordinates to undo this frame's camera jitter
    float smoothing;         // weight of the new fit per frame
    uint flags;              // bit 0: reset smoothing, bit 1: debug green tint, bit 2: DLSS output readable (typed UAV load),
                             // bit 3: smooth lines, bit 4: dark screen fix, bit 5: UV seam fix, bit 6: temporal anti-aliasing,
                             // bit 7: soft-edged line smoothing, bit 8: stable screen edges, bit 9: full-size sampling,
                             // bit 14: occlusion check (depth input bound), bit 15: reversed depth (closer = larger)
    float line_width;        // smooth lines: footprint scale, percent / 50 (1 = DLSS-like, the 50 % maximum)
    uint layer;              // map layer of this upscaler call (VR: one layer per view, in call order)
    float glass_strength;    // share of the glass layer (dirt, glare, reflections, shadows) kept, 0..1 (Advanced setting)
    float edge_fade;         // experimental: fade into the upscaler's picture over this many render pixels at display edges
    float edge_stretch;      // experimental: extend displays outwards by up to this many render pixels
    float detail_clamp;      // experimental: 0..1, keep the redrawn screen within the sim's own local brightness range this frame
    float angle_soften;      // experimental: 0..1, sample screens seen at a steep angle like the sim does (softer)
    uint history_info;       // temporal anti-aliasing: bit 0 = history slice written this frame (the other is last frame's),
                             // bit 1 = last frame's history belongs to this view and can be used
    uint frame_index;        // temporal anti-aliasing: picks this frame's sample position inside the pixel
    uint bounds_reset;       // stable screen edges: slots whose learnt edges are forgotten (another texture took the slot)
    uint diag_index;         // diagnostics: this upscaler call's number (screen pixels in the map are counted per call)
    float edge_trim;         // experimental: leave the outer edge_trim render pixels of each screen to the upscaler
};

Texture2DArray<float4> map : register(t0);       // xy = display uv, z = depth, w = slot + 1 (0 = not a display)
Texture2D<float4> scene : register(t1);     // DLSS input colour (render resolution, pre-exposed linear)
Texture2D<float4> displays[32] : register(t2);
Texture2D<float> scene_depth : register(t34); // the upscaler's depth input (render resolution; occlusion check)
RWTexture2D<float4> output : register(u0);  // DLSS output, R11G11B10_FLOAT
RWTexture2D<uint> tiles : register(u1);     // per 8x8 map tile: display slot + 1 this frame (0 = none)
RWStructuredBuffer<float4> params : register(u2); // per slot: [3s] = a.rgb, valid; [3s+1] = b.rgb, samples; [3s+2] = mean screen.rgb, fit quality
RWTexture2DArray<float4> map_rw : register(u3);
RWByteAddressBuffer sums : register(u4);    // per slot 20 x uint64 fixed point
RWTexture2D<float4> residual : register(u5); // per render pixel: scene - a * display, w = slot + 1 (0 = none), + 64 where
                                             // the screen's picture has an edge (a symbol's outline: not trusted, see fit)
// Temporal anti-aliasing history per output pixel, two slices per view (this frame's and last frame's):
RWTexture2DArray<float4> history : register(u6);    // rgb = screen colour (before the brightness match), w = brightness of the
                                                    // screen texture at the pixel's own coordinates (sees content changes)
RWTexture2DArray<float2> history_uv : register(u7); // where the pixel was on its screen texture (0 = not a screen pixel)
// Stable screen edges: per slot a 64x64 grid over the texture; per cell the smallest and largest u and v any render
// pixel of the screen sampled there (24-bit fixed point; min > max = nothing seen). Kept until the slot changes texture.
RWByteAddressBuffer edge_bounds : register(u8);
// Glass layer averaged over the last few frames, per render pixel and view (w = 1: valid). The sim draws its sun glare
// and shadow edges a little differently every frame; DLSS averages that out, a single frame's glass layer flickers.
RWTexture2DArray<float4> glass_history : register(u9);
RWTexture2DArray<float2> glass_history_uv : register(u10); // where on its screen texture that render pixel was then
SamplerState display_sampler : register(s0);

static const uint slot_count = 32;
static const uint terms = 20;
static const float fixed_scale = 16777216.0;
static const uint bounds_cells = 64;
// Learnt edges in two halves that take turns (a new one every 128 upscaler calls, about a second): each call learns into
// the current half, which was emptied when its turn began, and uses what both halves have seen. So an edge the camera no
// longer sees (a screen partly hidden behind its bezel from this angle) is forgotten within about two seconds.
static const uint bounds_bytes = slot_count * bounds_cells * bounds_cells * 16;
static const uint diag_base = 2 * bounds_bytes; // after the edge bounds: 64 call counters
uint bounds_half(uint call) { return (call >> 7) & 1; }
static const float3 luma = float3(0.2126, 0.7152, 0.0722);
static const float bounds_scale = 16777215.0;

float3 srgb_to_linear(float3 c)
{
    // Matches the conversion in the game's display shader.
    return select(c <= 0.04045, c / 12.92, pow(c * 0.947867 + 0.0521327, 2.4));
}

// Full-size sampling (flags bit 9): the redrawn screens are read at full size only (their smaller mip levels flicker
// when the view moves). Instead, a footprint holding many texels gets more taps, n x n, at most ~1.5 texels apart.
// The fit pass reads them exactly like the sim does (its mip levels, its four taps): the glass layer is the sim's
// picture minus that, and any difference in sampling would show up in it as flicker around thin lines.
static bool sim_sampling = false;

float3 texture_tap(uint slot, float2 uv, float2 gx, float2 gy)
{
    if ((flags & 512) && !sim_sampling) return displays[NonUniformResourceIndex(slot)].SampleLevel(display_sampler, uv, 0).rgb;
    return displays[NonUniformResourceIndex(slot)].SampleGrad(display_sampler, uv, gx, gy).rgb;
}

uint taps_for(uint slot, float2 fx, float2 fy)
{
    float w, h;
    displays[NonUniformResourceIndex(slot)].GetDimensions(w, h);
    float texels = max(length(fx * float2(w, h)), length(fy * float2(w, h)));
    return clamp(uint(ceil(texels / 1.5)), 2, 8);
}

// n x n taps over the footprint fx, fy around uv (rotated grid: no two taps share a row or column); soft: Gaussian
// weights (sigma 0.4 of the footprint) over a footprint twice as wide, given as such by the caller.
float3 sample_area(uint slot, float2 uv, float2 fx, float2 fy, uint n, bool soft)
{
    float3 sum = 0;
    float total = 0, step = 1.0 / n;
    [loop] for (uint j = 0; j < n; ++j)
        [loop] for (uint i = 0; i < n; ++i)
        {
            float2 o = (float2(i, j) + (float2(j, i) + 0.5) * step) * step - 0.5;
            float2 r = o * 2;
            float w = soft ? exp(-dot(r, r) / (2 * 0.16)) : 1;
            sum += w * texture_tap(slot, uv + o.x * fx + o.y * fy, fx * step, fy * step);
            total += w;
        }
    return sum / total;
}

// The game averages four half-footprint taps (SampleBias -1) before linearising; do the same at output scale.
float3 sample_display(uint slot, float2 uv, float2 dx, float2 dy)
{
    uint n = sim_sampling || !(flags & 512) ? 2 : taps_for(slot, dx, dy);
    if (n > 2) return srgb_to_linear(saturate(sample_area(slot, uv, dx, dy, n, false)));
    static const float2 offsets[4] = { float2(0.125, 0.375), float2(0.375, -0.125), float2(-0.125, -0.375), float2(-0.375, 0.125) };
    float3 sum = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
        sum += texture_tap(slot, uv + offsets[i].x * dx + offsets[i].y * dy, dx * 0.5, dy * 0.5);
    return srgb_to_linear(saturate(sum * 0.25));
}

// Smooth lines: what DLSS converges to on a still screen, without its history. DLSS averages each pixel over the area of
// one jittered render pixel across frames; this averages a 4x4 grid over that area (at least 1.25 output pixels) in one
// frame, so stair steps on thin slanted lines in the screen texture are smoothed instead of shown texel-sharp.
float3 sample_display_smooth(uint slot, float2 uv, float2 dx, float2 dy)
{
    float2 width = max(1.0 / map_per_output, 1.25) * line_width;
    // Soft-edged (flags bit 7): Gaussian weights (sigma 0.4 of the width) over twice the area instead of a box. A box's
    // hard edges let through fine detail that flickers as the view moves; a Gaussian passes almost none of it.
    // At least a rotated 4x4 grid (16 distinct coverage levels for near-horizontal and near-vertical lines), more taps
    // where the area holds many texels.
    bool soft = (flags & 128) != 0;
    float spread = soft ? 2.0 : 1.0;
    float2 fx = dx * width.x * spread, fy = dy * width.y * spread;
    return srgb_to_linear(saturate(sample_area(slot, uv, fx, fy, (flags & 512) ? max(taps_for(slot, fx, fy), 4) : 4, soft)));
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
    for (uint c = id.x; c < 2 * slot_count * bounds_cells * bounds_cells; c += 64) edge_bounds.Store4(c * 16, uint4(0xffffffff, 0xffffffff, 0, 0));
}

uint bounds_address(uint slot, int2 cell) { return (slot * bounds_cells * bounds_cells + cell.y * bounds_cells + cell.x) * 16; }

// Learnt extent (umin, vmin, umax, vmax) of the screen in one texture cell; false if nothing was seen there.
bool cell_bounds(uint slot, int2 cell, out float4 b)
{
    b = 0;
    if (any(cell < 0) || any(cell >= int(bounds_cells))) return false;
    uint4 r = edge_bounds.Load4(bounds_address(slot, cell)), s = edge_bounds.Load4(bounds_bytes + bounds_address(slot, cell));
    r = uint4(min(r.xy, s.xy), max(r.zw, s.zw)); // both halves
    b = float4(r) / bounds_scale;
    return r.x <= r.z && r.y <= r.w;
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

// Occlusion check: something drawn after a screen in front of it (a sidestick, a hand, the yoke) is closer to the camera
// in the sim's final depth buffer than the screen surface the map recorded there. Compared as 1 / distance (reversed depth
// is about that already; for standard depth 1 - depth is), with 1.5 % slack: the map keeps depth at half precision, and
// a screen's cover glass or a decal on it sits only millimetres in front.
bool occluded(int2 p, float screen_depth)
{
    float depth = scene_depth.Load(int3(p, 0));
    bool reversed = (flags & 32768) != 0;
    float scene_near = reversed ? depth : 1 - depth, screen_near = reversed ? screen_depth : 1 - screen_depth;
    return scene_near > screen_near * 1.015 + 1e-6;
}

// The map at render pixel p. With the occlusion check, a screen pixel with something in front of it reads as no screen,
// marked by z = -1.
float4 load_map(int2 p)
{
    if (any(p < 0) || any(p >= int2(map_size))) return 0;
    float4 m = map.Load(int4(p, layer, 0));
    if ((flags & 16384) && m.w > 0.5 && occluded(p, m.z)) return float4(0, 0, -1, 0);
    return m;
}

// Distance in output pixels from texture coordinates uv (per-pixel gradients dx, dy) to the nearest learnt edge of the
// screen's picture in its texture (a cell side with nothing beyond it), positive inside, and the direction on the
// monitor towards that edge. false: no learnt edges around, or no gradients (a band pointing at one texel).
bool edge_distance(uint slot, float2 uv, float2 dx, float2 dy, out float d, out float2 toward)
{
    d = 1e9; toward = 0;
    if (length(dx) + length(dy) < 1e-7) return false;
    int2 cell = int2(floor(saturate(uv) * bounds_cells - 1e-4));
    float4 b;
    bool found = cell_bounds(slot, cell, b);
    static const int2 sides[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    // Past the edge: measure from the neighbouring cell with screen in it.
    [unroll] for (uint k = 0; k < 4; ++k)
        if (!found && cell_bounds(slot, cell + sides[k], b)) { found = true; cell += sides[k]; }
    if (!found) return false;
    float2 gu = float2(dx.x, dy.x), gv = float2(dx.y, dy.y); // change of u and v per output pixel along x and y
    float su = max(length(gu), 1e-9), sv = max(length(gv), 1e-9);
    // A small, distant screen: its cells are only a pixel or two on the monitor, too few samples land in each for an
    // edge to be learnt reliably. Leave its edges to the usual handling.
    if (min(1.0 / bounds_cells / su, 1.0 / bounds_cells / sv) < 3) return false;
    float4 unused;
    float e;
    if (!cell_bounds(slot, cell + sides[0], unused)) { e = (uv.x - b.x) / su; if (e < d) { d = e; toward = -gu / su; } }
    if (!cell_bounds(slot, cell + sides[1], unused)) { e = (b.z - uv.x) / su; if (e < d) { d = e; toward = gu / su; } }
    if (!cell_bounds(slot, cell + sides[2], unused)) { e = (uv.y - b.y) / sv; if (e < d) { d = e; toward = -gv / sv; } }
    if (!cell_bounds(slot, cell + sides[3], unused)) { e = (b.w - uv.y) / sv; if (e < d) { d = e; toward = gv / sv; } }
    return true;
}

// Share of the output pixel at map position `location` (texture coordinates uv) that lies on the screen, from the
// learnt edges. Only where the screen really ends on the monitor: the picture can also end at a UV seam inside one
// screen mesh (e.g. the FSS E-Jets' bezel band), where the screen goes on beyond it (the seam handling blends those).
// -1: unknown (no learnt edges around).
float edge_coverage(uint slot, float2 uv, float2 dx, float2 dy, float2 location, bool mixed)
{
    float d;
    float2 toward;
    if (!edge_distance(slot, uv, dx, dy, d, toward)) return -1;
    // A pixel partly off the screen on the monitor lies within about a render pixel of where the screen ends. No learnt
    // edge that close: the screen's end isn't one in its texture (several screens share one texture, e.g. the A220, A340
    // and A380 displays, and the texture beyond this one's edge belongs to another): leave the pixel to the usual handling.
    if (mixed && d > 1 / min(map_per_output.x, map_per_output.y) + 0.5) return -1;
    if (d > 1.5) return 1;
    // One render pixel past the edge on the monitor: still this screen there means a seam, not its end; another
    // screen there means two textures meet within one display (e.g. the E-Jet PFD's halves): no gap to blend into.
    float2 probe = location + toward * (max(d, 0) * map_per_output + 1.0);
    if (map_slot(load_map(int2(round(probe)))) < slot_count) return 1;
    return saturate(0.5 + d);
}


[numthreads(8, 8, 1)]
void fit(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    sim_sampling = true;
    if (index == 0) tile_slot = 0xffffffff;
    GroupMemoryBarrierWithGroupSync();
    int2 p = int2(id.xy);
    float4 m = load_map(p);
    uint slot = map_slot(m);
    if (slot < slot_count) InterlockedMin(tile_slot, slot);

    bool active = slot < slot_count;
    bool outline = false;
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
            outline = outline || abs(dot(hx, luma)) > 0.02;
        }
        // Next to a symbol's outline the remainder below is mostly how differently the sim and this pass sample the
        // texture there, and that changes with the jitter every frame. Glass effects (dirt, glare, reflections) change
        // slowly across a screen: the composite takes them from the pixels around, not from outlines.
        [unroll] for (uint k2 = 0; k2 < 2; ++k2)
        {
            float4 n = k2 ? u : l;
            if (map_slot(n) == slot) outline = outline || abs(dot(sample_display(slot, n.xy, dx, dy) - x, luma)) > 0.02;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Per-pixel remainder against last frame's gain (glass layer: dirt, glare, reflections, shadows).
    float4 gain = active ? params[slot * 3] : 0;
    if (all(p < int2(map_size))) residual[p] = active && gain.w > 0 ? float4(y - gain.rgb * x, slot + 1 + (outline ? 64 : 0)) : 0;
    if (index == 0 && all(group.xy < (map_size + 7) / 8)) tiles[group.xy] = tile_slot < slot_count ? tile_slot + 1 : 0;
    // Diagnostics: screen pixels in the map at this call (the GPU's view: were this frame's screens drawn before it?).
    uint screen_pixels = WaveActiveCountBits(slot < slot_count && all(p < int2(map_size)));
    if (WaveIsFirstLane() && screen_pixels) { uint unused; edge_bounds.InterlockedAdd(diag_base + (diag_index & 63) * 4, screen_pixels, unused); }

    // Stable screen edges: widen the bounds of the texture cell this render pixel sampled (one set of atomics per cell
    // and wave). With the camera jitter the samples move every frame, so the bounds close in on the screen's real edge.
    if (flags & 256)
    {
        bool pending = slot < slot_count;
        uint key = pending ? slot * bounds_cells * bounds_cells + uint(clamp(m.y, 0, 1) * bounds_cells - 1e-4) * bounds_cells + uint(clamp(m.x, 0, 1) * bounds_cells - 1e-4) : 0;
        uint2 q = uint2(saturate(m.xy) * bounds_scale + 0.5);
        while (WaveActiveAnyTrue(pending))
        {
            if (pending)
            {
                uint first = WaveReadLaneFirst(key);
                if (key == first)
                {
                    uint2 low = WaveActiveMin(q), high = WaveActiveMax(q);
                    if (WaveIsFirstLane())
                    {
                        uint address = key * 16 + bounds_half(diag_index) * bounds_bytes, unused;
                        edge_bounds.InterlockedMin(address, low.x, unused);
                        edge_bounds.InterlockedMin(address + 4, low.y, unused);
                        edge_bounds.InterlockedMax(address + 8, high.x, unused);
                        edge_bounds.InterlockedMax(address + 12, high.y, unused);
                    }
                    pending = false;
                }
            }
        }
    }

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

// Glass layer around a render pixel: Gaussian average (sigma 1.7 render pixels) of the remainder over the 8x8 render
// pixels around `location`, from any screen's pixels (the glass is one sheet, also where two screen textures meet) and
// not at symbol outlines; there only if hardly anything else is around (text in a small box). spread: its local
// standard deviation. False: no screen pixels around.
bool glass_around(float2 location, out float3 glass, out float3 spread, out float outline_share)
{
    float3 smooth = 0, m2 = 0, outline_sum = 0, outline_m2 = 0;
    float smooth_weight = 0, outline_weight = 0;
    int2 centre = int2(floor(location));
    [loop] for (int j = 0; j < 64; ++j)
    {
        int2 q = centre + int2(j % 8 - 3, j / 8 - 3);
        if (any(q < 0) || any(q >= int2(map_size))) continue;
        float4 rv = residual[q];
        uint tag = uint(rv.w + 0.5);
        if ((tag & 63) == 0) continue;
        float2 t = float2(q) - location;
        float w = exp(-dot(t, t) / (2 * 1.7 * 1.7));
        if (tag & 64) { outline_weight += w; outline_sum += w * rv.rgb; outline_m2 += w * rv.rgb * rv.rgb; continue; }
        smooth += w * rv.rgb; m2 += w * rv.rgb * rv.rgb; smooth_weight += w;
    }
    outline_share = outline_weight / max(outline_weight + smooth_weight, 1e-6);
    if (smooth_weight < 0.5 && outline_weight > 0)
    {
        float extra = outline_weight * saturate(1 - 2 * smooth_weight);
        smooth += outline_sum * (extra / outline_weight); m2 += outline_m2 * (extra / outline_weight); smooth_weight += extra;
    }
    glass = smooth / max(smooth_weight, 1e-6);
    spread = sqrt(max(m2 / max(smooth_weight, 1e-6) - glass * glass, 0)) + 0.002;
    return smooth_weight > 1e-3;
}

// Steady glass: this frame's glass layer around each render pixel, blended into its average over the last few frames
// (a fifth per frame). The average may differ from this frame's by the local spread plus a
// quarter of the glass itself (the frame-to-frame wobble of the sim's glare); beyond that, the light really changed (or
// the view moved) and the average follows at once, so there are no trails.
static const float glass_blend = 0.2;
[numthreads(8, 8, 1)]
void glass_filter(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= map_size)) return;
    uint3 h = uint3(id.xy, layer);
    float3 now, spread;
    float outline_share;
    if ((uint(residual[id.xy].w + 0.5) & 63) == 0 || !glass_around(float2(id.xy), now, spread, outline_share)) { glass_history[h] = 0; return; }
    // The view moved (the pixel shows another spot of its screen than last frame, by more than a quarter of a render
    // pixel): start over, the average belongs elsewhere now.
    float4 m = load_map(int2(id.xy)), r = load_map(int2(id.xy) + int2(1, 0)), d = load_map(int2(id.xy) + int2(0, 1));
    float2 gx = r.w == m.w ? r.xy - m.xy : 0, gy = d.w == m.w ? d.xy - m.xy : 0;
    float step = max(length(gx), length(gy));
    // The map is drawn with this frame's camera jitter: the spot at the unjittered pixel position.
    float2 here = saturate(m.xy + jitter.x * gx + jitter.y * gy);
    float2 then = glass_history_uv[h];
    bool same_spot = step > 0 && length(here - then) < 0.25 * step;
    float4 old = glass_history[h];
    float3 range = spread + 0.25 * abs(now) + 0.002;
    float3 value = old.w > 0 && same_spot && !(flags & 8192) ? lerp(clamp(old.rgb, now - range, now + range), now, glass_blend) : now;
    glass_history[h] = float4(value, 1);
    glass_history_uv[h] = here;
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
    // The slot took another screen texture (the sim recycles them): its brightness match belongs to the old one. Start
    // over, and leave the screen to the upscaler until the new one has enough samples (a wrong match would flash).
    if (bounds_reset & (1u << slot)) previous_a.w = 0;
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
    // Experimental (dark screen fix): it can also let through a screen whose picture the sim draws differently
    // (reported for the iFly 737 EFB), so it is off by default.
    double detail_xx = v[16], detail_yy = v[17] * 1024, detail_xy = v[18] - v[19];
    if ((flags & 16) && pooled_variance > 3e-6 && detail_xx > 0 && detail_yy > 0 && detail_xy > 0)
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

// Texture coordinates at `location` continued from map pixel q (value m) along the steps to its neighbours on the
// same side of any UV seam (the smaller step of each pair; a step longer than `limit` crosses a seam and is no step at
// all, e.g. from a bezel band pointing at one texel into the picture next to it), and the per-output-pixel gradients.
void continue_from(int2 q, float4 m, float2 location, float limit, out float2 uv, out float2 dx, out float2 dy)
{
    float4 r = load_map(q + int2(1, 0)), l = load_map(q - int2(1, 0)), down = load_map(q + int2(0, 1)), up = load_map(q - int2(0, 1));
    float2 to_r = r.w == m.w ? r.xy - m.xy : 1e3, to_l = l.w == m.w ? m.xy - l.xy : 1e3;
    float2 to_d = down.w == m.w ? down.xy - m.xy : 1e3, to_u = up.w == m.w ? m.xy - up.xy : 1e3;
    float2 gx = length(to_r) <= length(to_l) ? to_r : to_l, gy = length(to_d) <= length(to_u) ? to_d : to_u;
    if (length(gx) > limit) gx = 0;
    if (length(gy) > limit) gy = 0;
    float2 o = location - float2(q);
    uv = m.xy + o.x * gx + o.y * gy;
    dx = gx * map_per_output.x;
    dy = gy * map_per_output.y;
}

// ---------------------------------------------------------------------------------------------------------------
// Temporal anti-aliasing (experimental). The game's own TAA leaves no trails on the screens: it keeps old frames only
// as long as they agree with the current frame around each pixel. The same here, with two advantages: the current
// frame is the exact screen texture (no render-resolution noise to hide), and a change of the screen's content is seen
// in the texture itself: each history pixel remembers the texture's brightness at its own coordinates, and if the
// texture there reads differently now, the history is dropped the frame a digit changes (not clamped away slowly).
// Each frame samples the texture at another position inside the pixel (8-frame Halton pattern); blended over frames
// that is the screen filtered over the pixel's area, like a still picture with TAA, and stable while the view moves.
static const float temporal_blend = 0.1; // share of the new frame

// Brightness of the screen texture at uv over one pixel's footprint: the change reference stored with the history.
float texture_brightness(uint slot, float2 uv, float2 dx, float2 dy)
{
    return dot(srgb_to_linear(texture_tap(slot, uv, dx, dy)), luma);
}

uint history_slice(bool current) { return layer * 2 + ((history_info & 1) ^ (current ? 0 : 1)); }

float2 halton_offset(uint index)
{
    static const float2 pattern[8] = { float2(0.5, 0.333333), float2(0.25, 0.666667), float2(0.75, 0.111111), float2(0.125, 0.444444),
        float2(0.625, 0.777778), float2(0.375, 0.222222), float2(0.875, 0.555556), float2(0.0625, 0.888889) };
    return pattern[index % 8] - 0.5;
}

// Last frame's texture coordinates at output position q (bilinear between pixel centres); false off a screen.
bool history_uv_at(float2 q, out float2 uv)
{
    uv = 0;
    float2 base = q - 0.5;
    int2 i = int2(floor(base));
    float2 f = base - i;
    if (any(i < 0) || any(i + 1 >= int2(output_size))) return false;
    uint s = history_slice(false);
    float2 a = history_uv[uint3(i, s)], b = history_uv[uint3(i + int2(1, 0), s)], c = history_uv[uint3(i + int2(0, 1), s)], d = history_uv[uint3(i + 1, s)];
    if (!all(float4(a.x, b.x, c.x, d.x) > 0)) return false;
    uv = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
    return true;
}

float4 history_load(int2 p, out bool valid)
{
    valid = all(p >= 0) && all(p < int2(output_size)) && history_uv[uint3(p, history_slice(false))].x > 0;
    return valid ? history[uint3(p, history_slice(false))] : 0;
}

// Last frame's colour at output position q: Catmull-Rom, which keeps the history sharp although it is resampled every
// frame while the view moves; bilinear where the 4x4 taps run off the screen.
bool history_colour_at(float2 q, uint slot, float2 dx, float2 dy, out float4 value, out bool changed)
{
    changed = false;
    float2 base = q - 0.5;
    int2 i = int2(floor(base));
    float2 f = base - i;
    float2 w[4] = { f * (-0.5 + f * (1 - 0.5 * f)), 1 + f * f * (-2.5 + 1.5 * f), f * (0.5 + f * (2 - 1.5 * f)), f * f * (-0.5 + 0.5 * f) };
    float4 sum = 0, inner = 0;
    float inner_weight = 0;
    bool all_valid = true;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
        {
            bool valid;
            float4 v = history_load(i + int2(x - 1, y - 1), valid);
            all_valid = all_valid && valid;
            sum += v * (w[x].x * w[y].y);
            if (x >= 1 && x <= 2 && y >= 1 && y <= 2 && valid)
            {
                float b = (x == 1 ? 1 - f.x : f.x) * (y == 1 ? 1 - f.y : f.y);
                inner += v * b; inner_weight += b;
                // Content change: the texture now, at the coordinates this history pixel had, against what it read then.
                float2 seen = history_uv[uint3(i + int2(x - 1, y - 1), history_slice(false))];
                float now = texture_brightness(slot, seen, dx, dy);
                changed = changed || abs(now - v.w) > 0.01 + 0.05 * max(now, v.w);
            }
        }
    if (all_valid) { value = float4(max(sum.rgb, 0), sum.w); return true; }
    value = inner;
    return inner_weight > 0.999;
}

float3 sample_lines(uint slot, float2 uv, float2 dx, float2 dy)
{
    return (flags & 8) ? sample_display_smooth(slot, uv, dx, dy) : sample_display(slot, uv, dx, dy);
}

// Screen colour (before the brightness match) at output pixel p, accumulated over frames. Writes this frame's history.
float3 temporal_content(uint2 p, uint slot, float2 uv, float2 dx, float2 dy)
{
    float2 j = halton_offset(frame_index);
    float3 current = sample_lines(slot, uv + j.x * dx + j.y * dy, dx, dy);
    float3 still = sample_lines(slot, uv, dx, dy); // without history: as the fix draws it otherwise
    // What the screen shows around this pixel now: history outside that range shows something no longer there.
    float3 low = current, high = current;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        float2 o = float2(k & 1 ? 0.6 : -0.6, k & 2 ? 0.6 : -0.6);
        float3 n = sample_display(slot, uv + o.x * dx + o.y * dy, dx, dy);
        low = min(low, n); high = max(high, n);
    }
    float reference = texture_brightness(slot, uv, dx, dy);

    float3 result = still;
    if (history_info & 2)
    {
        // Where this point of the screen was last frame: follow last frame's texture coordinates from this pixel (Newton
        // steps on this frame's gradients). No motion vectors needed, and it can't take another screen or another part
        // of the same texture by mistake: the coordinates must match at the end.
        float det = dx.x * dy.y - dx.y * dy.x;
        float2 q = float2(p) + 0.5;
        bool found = abs(det) > 1e-14;
        float2 step = 1e3;
        [unroll] for (uint n = 0; n < 4; ++n)
        {
            float2 seen;
            if (!found || !history_uv_at(q, seen)) { found = false; break; }
            float2 e = uv - seen;
            step = float2(e.x * dy.y - e.y * dy.x, dx.x * e.y - dx.y * e.x) / det;
            if (length(step) > 64) { found = false; break; }
            q += step;
            if (length(step) < 0.01) break;
        }
        float4 previous;
        bool changed;
        if (found && length(step) < 0.05 && history_colour_at(q, slot, dx, dy, previous, changed))
        {
            if (!changed)
            {
                // Resampling the history at a fraction of a pixel softens it a little every frame: take more of the new
                // frame the further from a whole pixel the history moved (up to 0.25 at half a pixel).
                float2 moved = abs(q - 0.5 - round(q - 0.5) - (float2(p) - round(float2(p))));
                float blend = temporal_blend + 0.3 * max(moved.x, moved.y);
                float3 margin = (high - low) * 0.05 + 0.001;
                result = lerp(clamp(previous.rgb, low - margin, high + margin), current, blend);
            }
        }
    }
    history[uint3(p, history_slice(true))] = float4(result, reference);
    history_uv[uint3(p, history_slice(true))] = max(uv, 1.0 / 65535);
    return result;
}

[numthreads(8, 8, 1)]
void composite(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= output_size)) return;
    if (flags & 64) history_uv[uint3(id.xy, history_slice(true))] = 0; // not a screen pixel unless written below
    float2 location = (float2(id.xy) + 0.5) * map_per_output - 0.5 + jitter;
    int2 cell = int2(floor(location));
    if (any(cell < 0) || any(cell + 1 >= int2(map_size))) return;
    uint2 tile_size = (map_size + 7) / 8;
    if (!tiles[uint2(cell) / 8] && !tiles[min(uint2(cell + 1) / 8, tile_size - 1)]) return;
    float4 a, b, c, d;
    bool covered = tap(cell, a) & tap(cell + int2(1, 0), b) & tap(cell + int2(0, 1), c) & tap(cell + int2(1, 1), d);
    // Next to something in front of the screen, the upscaler's picture: the learnt edges and the edge stretcher would
    // continue the screen over it.
    if (!covered && min(min(a.z, b.z), min(c.z, d.z)) < 0) return;
    float2 f = location - cell;
    float2 uv, dx, dy;
    float2 seam_uv = 0, seam_dx = 0, seam_dy = 0; // UV seam: the other side, and its share of this pixel
    float seam_share = 0, seam_upscaler = 0;
    float band_share = 0; // share of this pixel on a bezel band (all of it one texture spot): left to the upscaler
    float2 join_uv = 0, join_dx = 0, join_dy = 0; // two screen textures meeting in this pixel: the other one, and its share
    uint join_slot = slot_count;
    float join_share = 0;
    bool filled = false;
    float coverage = 1; // stable screen edges: share of this pixel on the screen
    bool inside = covered && a.w == b.w && a.w == c.w && a.w == d.w;
    bool edge_pixel = false;
    if (!inside && (flags & 256))
    {
        // A pixel at a screen's edge: continue the texture coordinates of the nearest render pixel on the screen and
        // let the learnt edge decide how much of the pixel is screen.
        float4 taps[4] = { a, b, c, d };
        float weights[4] = { (1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y };
        uint k = 4;
        [unroll] for (uint i = 0; i < 4; ++i)
            if (map_slot(taps[i]) < slot_count && (k == 4 || weights[i] > weights[k])) k = i;
        if (k < 4)
        {
            continue_from(cell + int2(k & 1, k >> 1), taps[k], location, 0.02, uv, dx, dy);
            uint other = 4;
            [unroll] for (uint i2 = 0; i2 < 4; ++i2)
                if (map_slot(taps[i2]) < slot_count && map_slot(taps[i2]) != map_slot(taps[k]) && (other == 4 || weights[i2] > weights[other])) other = i2;
            if (other < 4)
            {
                // Two screen textures meet in this pixel within one display (e.g. the FSS E-Jet PFD's halves). Drawn
                // from the nearest render pixel's texture alone, the pixel would flip between the two with the jitter:
                // both instead, each by its share of the pixel from the learnt edges (which the jitter doesn't move).
                continue_from(cell + int2(other & 1, other >> 1), taps[other], location, 0.02, join_uv, join_dx, join_dy);
                float d_near, d_other;
                float2 unused;
                bool near_known = edge_distance(map_slot(taps[k]), uv, dx, dy, d_near, unused);
                bool other_known = edge_distance(map_slot(taps[other]), join_uv, join_dx, join_dy, d_other, unused);
                float near_share = near_known && other_known ? 0.5 * (saturate(0.5 + d_near) + saturate(0.5 - d_other))
                                 : near_known ? saturate(0.5 + d_near) : other_known ? saturate(0.5 - d_other) : 0.5;
                a = taps[k]; coverage = 1; edge_pixel = true;
                join_slot = map_slot(taps[other]); join_share = 1 - near_share;
            }
            else
            {
                float share = edge_coverage(map_slot(taps[k]), uv, dx, dy, location, true);
                if (share > 0.5) { a = taps[k]; coverage = share; edge_pixel = true; }
            }
        }
    }
    if (edge_pixel)
    {
        if (length(dx) + length(dy) < 1e-7) return; // the edge of a bezel band (one texture spot): the upscaler's
    }
    else if (inside)
    {
        uv = lerp(lerp(a.xy, b.xy, f.x), lerp(c.xy, d.xy, f.x), f.y);
        dx = lerp(b.xy - a.xy, d.xy - c.xy, f.y) * map_per_output.x;
        dy = lerp(c.xy - a.xy, d.xy - b.xy, f.x) * map_per_output.y;
        // All four render pixels around point at one texture spot: a bezel band, not screen content (see below).
        if ((flags & 32) && length(dx) + length(dy) < 1e-7) return;
        // UV seam: one screen mesh whose texture coordinates jump between neighbouring pixels (e.g. the FSS E-Jets'
        // bezel band points at the texture's corner, right next to the picture). Blending across the jump samples
        // whatever lies between in the texture: a thin line along the seam that the sim never draws. Continue the
        // nearest render pixel's own coordinates instead, along the steps to its neighbours on the same side.
        if (flags & 32)
        {
            float lab = length(b.xy - a.xy), lcd = length(d.xy - c.xy), lac = length(c.xy - a.xy), lbd = length(d.xy - b.xy);
            float jump = max(max(lab, lcd), max(lac, lbd)), step = min(min(lab, lcd), min(lac, lbd));
            if (jump > 0.02 && jump > 4 * step)
            {
                // Two sides: the taps near the nearest one in the texture, and the rest. Each side continues its own
                // nearest tap; the two samples are blended by how much of this pixel each side covers (bilinear
                // weights), so the seam is as smooth as the upscaler draws it instead of following the render grid.
                float4 taps[4] = { a, b, c, d };
                float weights[4] = { (1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y };
                uint k = 0;
                [unroll] for (uint i = 1; i < 4; ++i) if (weights[i] > weights[k]) k = i;
                uint other = 4;
                float side = 0;
                [unroll] for (uint j = 0; j < 4; ++j)
                {
                    if (length(taps[j].xy - taps[k].xy) < jump * 0.5) side += weights[j];
                    else if (other == 4 || weights[j] > weights[other]) other = j;
                }
                continue_from(cell + int2(k & 1, k >> 1), taps[k], location, jump * 0.5, uv, dx, dy);
                if (other < 4)
                {
                    continue_from(cell + int2(other & 1, other >> 1), taps[other], location, jump * 0.5, seam_uv, seam_dx, seam_dy);
                    seam_share = 1 - side;
                    // Right on the seam the render grid still shows as small steps; the upscaler's picture there is
                    // smooth (it has seen the seam from many jittered frames) and, as nothing moves across a seam,
                    // free of trails. Hand those pixels to it: fully at the middle of the seam, not at all a render
                    // pixel away.
                    seam_upscaler = 1 - abs(2 * side - 1);
                    // Stable screen edges: the split from where the picture ends in its texture (learnt, so the camera
                    // jitter doesn't move it) instead of from which render pixels happen to be on either side. The
                    // picture is the side whose coordinates change across the pixel (a bezel band points at one texel).
                    if (flags & 256)
                    {
                        bool picture_near = length(dx) + length(dy) >= length(seam_dx) + length(seam_dy);
                        float d;
                        float2 toward;
                        if (picture_near ? edge_distance(map_slot(taps[k]), uv, dx, dy, d, toward)
                                         : edge_distance(map_slot(taps[other]), seam_uv, seam_dx, seam_dy, d, toward))
                        {
                            float picture = saturate(0.5 + d);
                            seam_share = picture_near ? 1 - picture : picture;
                            seam_upscaler = 0;
                            // The other side is a bezel band pointing at one texture spot (e.g. the E-Jet ND's top
                            // edge): not screen content, and whatever sits on that spot (a parked mouse cursor) would be
                            // stretched across the band. Its share goes to the upscaler, the picture is drawn alone.
                            float2 band_dx = picture_near ? seam_dx : dx, band_dy = picture_near ? seam_dy : dy;
                            if (length(band_dx) + length(band_dy) < 1e-7)
                            {
                                band_share = picture_near ? seam_share : 1 - seam_share;
                                seam_share = picture_near ? 0 : 1;
                            }
                        }
                    }
                }
            }
        }
        if ((flags & 256) && seam_share == 0)
        {
            float share = edge_coverage(map_slot(a), uv, dx, dy, location, false);
            if (share >= 0) coverage = share;
        }
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
    if (!filled && !edge_pixel && edge_fade > 0 && (flags & 4))
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
    float blend = (flags & 4) ? min(edge, 1 - seam_upscaler) : edge;
    // The upscaler's pixel already holds its share of the screen: a pixel half on the screen stays the upscaler's, from
    // there the redrawn screen takes over, fully on a whole pixel. Steady, as the learnt edge doesn't move with jitter.
    coverage = (flags & 4) ? saturate(2 * coverage - 1) : (coverage >= 0.75 ? 1 : 0);
    if (blend <= 0 || coverage <= 0) return;
    // Edge trim (experimental, the reverse of the edge stretcher): leave the outer edge_trim render pixels of each screen
    // to the upscaler, for screens whose redrawn edge shows a sliver of something that isn't there (another screen's
    // picture next to it in a shared texture, a frame drawn over the screen's edge). Measured from where the screen meets
    // something that isn't a screen (two screens meeting within one display is no edge): from the learnt edge where it is
    // that same edge (it doesn't move with the camera jitter), else from the render pixels around.
    float trim_keep = 1;
    if (edge_trim > 0 && !filled)
    {
        int radius = min(int(ceil(edge_trim + 1)), 5);
        float nearest = 1e9;
        [loop] for (int ty = -radius; ty <= radius + 1; ++ty)
            [loop] for (int tx = -radius; tx <= radius + 1; ++tx)
            {
                int2 q = cell + int2(tx, ty);
                if (any(q < 0) || any(q >= int2(map_size)) || map_slot(load_map(q)) < slot_count) continue;
                nearest = min(nearest, length(float2(q) - location));
            }
        float dist = nearest - 0.5; // render pixels from the screen's end (half way to the first pixel off it)
        float learnt;
        float2 toward;
        if (dist < edge_trim + 1.5 && (flags & 256) && edge_distance(slot, uv, dx, dy, learnt, toward))
        {
            learnt *= map_per_output.x;
            if (abs(learnt - dist) < 1.5) dist = learnt;
        }
        trim_keep = saturate(dist - edge_trim + 0.5);
        if (trim_keep <= 0) return;
    }
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
    float3 content = ((flags & 64) && !filled && !edge_pixel && seam_share == 0 ? temporal_content(id.xy, slot, uv, dx, dy) : sample_lines(slot, uv, dx, dy)) * gain.rgb;
    if (seam_share > 0)
    {
        float3 other_side = (flags & 8) ? sample_display_smooth(slot, seam_uv, seam_dx, seam_dy) : sample_display(slot, seam_uv, seam_dx, seam_dy);
        content = lerp(content, other_side * gain.rgb, seam_share);
    }
    if (join_share > 0)
    {
        float4 join_gain = params[join_slot * 3];
        if (join_gain.w > 0) content = lerp(content, sample_lines(join_slot, join_uv, join_dx, join_dy) * join_gain.rgb, join_share);
    }

    // Glass layer (dirt, glare, reflections, shadows) = this frame's remainder around this pixel: its soft average and
    // spread over the 5x5 render pixels around it (Gaussian weights, same display only, not at symbol outlines). A soft
    // average of the jittered render pixels hardly changes from frame to frame; the nearest few would, wherever the
    // remainder has fine detail. The sharp glass detail itself comes from the upscaler's (temporally stable) picture.
    float3 smooth = 0, m1 = 0, m2 = 0, scene_low = 1e9, scene_high = -1e9;
    float smooth_weight = 0, count = 0, outline_weight = 0;
    float3 outline_sum = 0, outline_m2 = 0;
    int2 centre = int2(floor(location));
    [loop] for (int j = 0; j < 64; ++j)
    {
        int2 q = centre + int2(j % 8 - 3, j / 8 - 3);
        if (any(q < 0) || any(q >= int2(map_size))) continue;
        float4 rv = residual[q];
        uint tag = uint(rv.w + 0.5);
        // Any screen's pixels: the glass is one sheet, also where two screen textures meet within one display (e.g.
        // the E-Jet PFD's halves); taken from this slot's alone, a small box at such a join has almost none.
        if ((tag & 63) == 0) continue;
        float2 t = float2(q) - location;
        float w = exp(-dot(t, t) / (2 * 1.7 * 1.7));
        if (tag & 64) { outline_weight += w; outline_sum += w * rv.rgb; outline_m2 += w * rv.rgb * rv.rgb; continue; }
        smooth += w * rv.rgb; m2 += w * rv.rgb * rv.rgb; smooth_weight += w; count += 1;
    }
    // Detail clamp range: what the sim drew in the 5x5 render pixels around this one (same display). A wider window
    // than 3x3 changes less from frame to frame as the jitter moves, so the clamp doesn't flicker.
    if (detail_clamp > 0)
        [unroll] for (int s = 0; s < 25; ++s)
        {
            int2 q = centre + int2(s % 5 - 2, s / 5 - 2);
            if (any(q < 0) || any(q >= int2(map_size)) || (uint(residual[q].w + 0.5) & 63) != slot + 1) continue;
            float3 y = scene.Load(int3(q, 0)).rgb;
            if (all(isfinite(y))) { scene_low = min(scene_low, y); scene_high = max(scene_high, y); }
        }
    float3 glass = offset.rgb;
    // Steady glass (debug bit 13 off): the centre from the averaged glass layer of the 4 render pixels around.
    float3 steady = 0;
    float steady_weight = 0;
    if (!(flags & 8192))
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            int2 q = cell + int2(k & 1, k >> 1);
            float w = ((k & 1) ? f.x : 1 - f.x) * ((k >> 1) ? f.y : 1 - f.y);
            float4 v = glass_history[uint3(q, layer)];
            if (v.w > 0 && w > 0) { steady += w * v.rgb; steady_weight += w; }
        }
    // Hardly any pixels away from outlines around (text in a small box): use the outline pixels too, rather than
    // a value from one or two pixels that changes every frame.
    if (smooth_weight < 0.5 && outline_weight > 0)
    {
        float extra = outline_weight * saturate(1 - 2 * smooth_weight);
        smooth += outline_sum * (extra / outline_weight); m2 += outline_m2 * (extra / outline_weight); smooth_weight += extra;
    }
    if (smooth_weight > 1e-3)
    {
        glass = smooth / smooth_weight;
        float3 spread_now = sqrt(max(m2 / smooth_weight - glass * glass, 0)) + 0.002;
        if (steady_weight > 0.05) glass = steady / steady_weight;
        if (flags & 4)
        {
            // DLSS output minus the clean content = sharp glass detail + ghost of old content. Keep it only within the
            // local spread of this frame's glass layer (variance clipping, as in TAA); ghost trails fall outside.
            float3 spread = spread_now;
            // Bit 12, calm glass at symbols: not next to symbol outlines (their share around this pixel, Gaussian weighted,
            // hardly changes with the jitter): DLSS's lines sit slightly elsewhere than the redrawn ones, differently every
            // frame. Debug bit 11: the smooth part only, everywhere.
            float detail = (flags & 2048) ? 0 : (flags & 4096) ? saturate(1 - 3 * outline_weight / (outline_weight + smooth_weight)) : 1;
            glass = lerp(glass, clamp(output[id.xy].rgb - content, glass - spread, glass + spread), detail);
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
    float keep = (blend < 1 ? blend * blend * (3 - 2 * blend) : 1) * coverage * trim_keep;
    if (band_share > 0 && (flags & 4)) keep *= 1 - band_share;
    if (keep < 1) colour = lerp(output[id.xy].rgb, colour, keep);
    if (all(isfinite(colour))) output[id.xy] = float4(max(colour, 0), 1);
}

[numthreads(8, 8, 1)]
void clear(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    uint index = id.y * map_size.x + id.x, cells = bounds_cells * bounds_cells;
    if (id.x < map_size.x && index < slot_count * cells)
    {
        bool reset = bounds_reset & (1u << (index / cells));
        uint next = bounds_half(diag_index + 1);
        // The next call starts a new turn: empty that half. A slot that took another texture: empty both.
        if (reset || next != bounds_half(diag_index)) edge_bounds.Store4(next * bounds_bytes + index * 16, uint4(0xffffffff, 0xffffffff, 0, 0));
        if (reset) edge_bounds.Store4((1 - next) * bounds_bytes + index * 16, uint4(0xffffffff, 0xffffffff, 0, 0));
    }
    if (all(id.xy < map_size) && all(group.xy < (map_size + 7) / 8) && tiles[group.xy]) map_rw[uint3(id.xy, layer)] = 0;
    if (all(id.xy == 0)) edge_bounds.Store(diag_base + ((diag_index + 1) & 63) * 4, 0);
}

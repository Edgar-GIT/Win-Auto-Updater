#pragma once

// HLSL for the GPU test ("Void Burner"). One full-screen pixel shader ray-marches
// the whole scene — a violet graveyard at night with a rotating void cube over
// a rune altar — so the load is pure pixel shading and scales with resolution
// and with the quality value (march / fog / shadow step counts).
//
// The same source is used two ways: it is pre-compiled into
// gpu_shader_bytecode.inc (see tools/), and it is embedded here so the test can
// still compile it at run time with d3dcompiler_47.dll if the blob is missing.

namespace nt {

inline const char kGpuShaderSource[] = R"HLSL(
cbuffer FrameCB : register(b0)
{
    float4 uRes;     // xy = scene viewport in pixels, zw = 1 / xy
    float4 uParams;  // x = time (s), y = quality 0..1, z = aspect (w / h), w = unused
    float4 uBlit;    // xy = uv scale of the valid scene region, zw = last valid uv
};

Texture2D uTex : register(t0);
SamplerState uSamp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Full-screen triangle from the vertex id: no vertex buffer needed.
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    o.uv = p;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    return o;
}

// Upscale of the (possibly lower resolution) scene to the back buffer.
float4 PSBlit(VSOut i) : SV_Target
{
    float2 uv = min(i.uv * uBlit.xy, uBlit.zw);
    return float4(uTex.Sample(uSamp, uv).rgb, 1.0);
}

// HUD overlay: premultiplied-alpha bitmap drawn by the bench window.
float4 PSHud(VSOut i) : SV_Target
{
    return uTex.Sample(uSamp, i.uv);
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

static const float TAU = 6.2831853;
static const float3 CUBE_C = float3(0.0, 3.25, 0.0);
static const float CUBE_R = 1.15;
static const float MAXD = 60.0;

float hash11(float p)
{
    p = frac(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return frac(p);
}

float hash21(float2 p)
{
    float3 p3 = frac(float3(p.x, p.y, p.x) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float2 hash22(float2 p)
{
    float3 p3 = frac(float3(p.x, p.y, p.x) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xx + p3.yz) * p3.zy);
}

float hash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float noise2(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float noise3(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(i);
    float n100 = hash31(i + float3(1.0, 0.0, 0.0));
    float n010 = hash31(i + float3(0.0, 1.0, 0.0));
    float n110 = hash31(i + float3(1.0, 1.0, 0.0));
    float n001 = hash31(i + float3(0.0, 0.0, 1.0));
    float n101 = hash31(i + float3(1.0, 0.0, 1.0));
    float n011 = hash31(i + float3(0.0, 1.0, 1.0));
    float n111 = hash31(i + float3(1.0, 1.0, 1.0));
    float x00 = lerp(n000, n100, f.x);
    float x10 = lerp(n010, n110, f.x);
    float x01 = lerp(n001, n101, f.x);
    float x11 = lerp(n011, n111, f.x);
    return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
}

float fbm2(float2 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++)
    {
        sum += amp * noise2(p);
        p = p * 2.03 + float2(17.1, 9.2);
        amp *= 0.5;
    }
    return sum;
}

float2 rot(float2 p, float a)
{
    float c = cos(a);
    float s = sin(a);
    return float2(c * p.x - s * p.y, s * p.x + c * p.y);
}

float sdBox(float3 p, float3 b)
{
    float3 q = abs(p) - b;
    return length(max(q, float3(0.0, 0.0, 0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}

// Wireframe box: only the 12 edges, with thickness e.
float sdBoxFrame(float3 p, float3 b, float e)
{
    p = abs(p) - b;
    float3 q = abs(p + e) - e;
    float d1 = length(max(float3(p.x, q.y, q.z), float3(0.0, 0.0, 0.0))) + min(max(p.x, max(q.y, q.z)), 0.0);
    float d2 = length(max(float3(q.x, p.y, q.z), float3(0.0, 0.0, 0.0))) + min(max(q.x, max(p.y, q.z)), 0.0);
    float d3 = length(max(float3(q.x, q.y, p.z), float3(0.0, 0.0, 0.0))) + min(max(q.x, max(q.y, p.z)), 0.0);
    return min(min(d1, d2), d3);
}

float sdCapsule(float3 p, float3 a, float3 b, float r)
{
    float3 pa = p - a;
    float3 ba = b - a;
    float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h) - r;
}

// Position of the void cube's centre (it bobs a little).
float3 cubeCenter(float t)
{
    return CUBE_C + float3(0.0, 0.22 * sin(t * 0.8), 0.0);
}

float3 cubeTurn(float3 q, float t)
{
    q.xz = rot(q.xz, t * 0.37);
    q.xy = rot(q.xy, t * 0.29);
    q.yz = rot(q.yz, t * 0.21);
    return q;
}

float groundHeight(float2 xz)
{
    return 0.10 * noise2(xz * 0.8) + 0.04 * noise2(xz * 2.9);
}

// Two-step circular stone altar under the cube.
float sdAltar(float3 p)
{
    float r = length(p.xz);
    float d1 = max(r - 2.7, abs(p.y - 0.10) - 0.16);
    float d2 = max(r - 2.0, abs(p.y - 0.34) - 0.14);
    return min(d1, d2);
}

// Gravestones and crosses: one random plot per grid cell around the altar.
float sdGraves(float3 p)
{
    const float G = 3.4;
    float2 cell = floor(p.xz / G + 0.5);
    float2 lp = p.xz - cell * G;
    // Never step further than a little past the cell wall: the next cell has
    // its own stone, which this evaluation knows nothing about.
    float bound = G * 0.5 - max(abs(lp.x), abs(lp.y)) + 0.55;
    float h1 = hash21(cell + 3.7);
    float h2 = hash21(cell * 1.31 + 11.9);
    float rc = length(cell);
    float result = bound;
    if (h1 >= 0.30 && rc >= 1.2 && rc <= 7.5)
    {
        float2 jitter = (hash22(cell + 5.1) - 0.5) * 0.9;
        float3 q = float3(lp.x - jitter.x, p.y, lp.y - jitter.y);
        q.xz = rot(q.xz, (h2 - 0.5) * 1.1);
        q.xy = rot(q.xy, (h1 - 0.65) * 0.24);   // old stones lean
        float hh = 0.55 + 0.40 * h2;
        float d = 0.0;
        if (h1 > 0.72)
        {
            float upright = sdBox(q - float3(0.0, hh, 0.0), float3(0.075, hh, 0.075));
            float arms = sdBox(q - float3(0.0, hh * 1.45, 0.0), float3(0.34, 0.075, 0.075));
            d = min(upright, arms);
        }
        else
        {
            float body = max(abs(q.x) - 0.40, abs(q.y - hh) - hh);
            float cap = length(float2(q.x, q.y - 2.0 * hh)) - 0.40;
            d = max(min(body, cap), abs(q.z) - 0.09);
        }
        d = min(d, sdBox(q - float3(0.0, 0.06, 0.0), float3(0.50, 0.08, 0.22)));
        result = min(d, bound);
    }
    return result;
}

// Dead tree: a trunk and five levels of mirrored, twisting branches.
float sdTree(float3 p, float seed)
{
    p.xz = rot(p.xz, seed * 6.2831);
    float d = sdCapsule(p, float3(0.0, 0.0, 0.0), float3(0.0, 1.7, 0.0), 0.13 - 0.03 * saturate(p.y / 1.7));
    float3 q = p - float3(0.0, 1.6, 0.0);
    float sc = 1.0;
    [loop] for (int i = 0; i < 5; i++)
    {
        q.x = abs(q.x);
        q.xy = rot(q.xy, 0.58 + 0.10 * seed);
        q.xz = rot(q.xz, 1.9);
        d = min(d, sdCapsule(q, float3(0.0, 0.0, 0.0), float3(0.0, 0.95, 0.0), 0.058) * sc);
        q.y -= 0.95;
        q *= 1.30;
        sc /= 1.30;
    }
    return d;
}

// x, z, seed, scale
static const float4 TREES[5] =
{
    float4(5.4, 1.0, 0.31, 1.00),
    float4(-4.6, -3.4, 0.71, 0.92),
    float4(-1.9, 5.6, 0.45, 1.05),
    float4(15.5, -9.0, 0.13, 1.55),
    float4(-13.0, 11.5, 0.88, 1.45)
};

float sdTrees(float3 p)
{
    float d = 1000.0;
    [loop] for (int i = 0; i < 5; i++)
    {
        float4 tr = TREES[i];
        float3 q = p - float3(tr.x, 0.0, tr.y);
        float reach = 3.3 * tr.w;
        float bound = max(length(q.xz) - reach, q.y - 5.4 * tr.w);
        if (bound > 0.35)
        {
            d = min(d, bound);
        }
        else
        {
            d = min(d, sdTree(q / tr.w, tr.z) * tr.w);
        }
    }
    return d;
}

// Wrought-iron fence around the graveyard (a ring of spiked bars).
float sdFence(float3 p)
{
    const float R = 20.0;
    float dr = length(p.xz) - R;
    float result = abs(dr) - 0.5;
    if (abs(dr) <= 0.9)
    {
        float a = atan2(p.z, p.x);
        const float seg = TAU / 180.0;
        float al = (a - seg * floor(a / seg + 0.5)) * R;
        float bar = max(length(float2(dr, al)) - 0.035, p.y - 1.95);
        float tip = length(float3(dr, p.y - 2.0, al)) - 0.075;
        float rails = min(length(float2(dr, p.y - 0.55)), length(float2(dr, p.y - 1.60))) - 0.03;
        result = min(min(bar, tip), rails);
    }
    return result;
}

// Crystal shards orbiting the cube.
float sdShards(float3 p, float t)
{
    float3 s = p - CUBE_C;
    float a = atan2(s.z, s.x) + t * 0.45;
    const float seg = TAU / 7.0;
    float ia = floor(a / seg + 0.5);
    a -= ia * seg;
    float id = ia - 7.0 * floor(ia / 7.0);   // stable identity 0..6
    float k = hash11(id * 3.17 + 4.0);
    float r = length(s.xz);
    float3 q = float3(cos(a) * r - (2.75 + 0.5 * k), s.y - 0.9 * sin(t * (0.9 + k) + id * 1.7), sin(a) * r);
    q.xy = rot(q.xy, t * 1.3 + id);
    q.yz = rot(q.yz, t * 0.9 + k * 6.0);
    float size = 0.10 * (0.7 + 0.9 * k);
    return sdBox(q, float3(size, size, size)) - 0.012;
}

// x = distance, y = material id, z = distance to the neon parts (for the glow)
float3 map(float3 p, float t)
{
    float d = p.y - groundHeight(p.xz);
    float m = 1.0;

    float da = sdAltar(p);
    if (da < d) { d = da; m = 2.0; }

    float dg = sdGraves(p);
    if (dg < d) { d = dg; m = 3.0; }

    // The cube and its shards only matter near the centre.
    float sphere = length(p - CUBE_C) - 4.3;
    float neon = sphere + 1.2;
    if (sphere < 0.5)
    {
        float3 q = cubeTurn(p - cubeCenter(t), t);
        float df = sdBoxFrame(q, float3(CUBE_R, CUBE_R, CUBE_R), 0.045);
        float dv = sdBox(q, float3(CUBE_R - 0.06, CUBE_R - 0.06, CUBE_R - 0.06));
        float ds = sdShards(p, t);
        if (df < d) { d = df; m = 4.0; }
        if (dv < d) { d = dv; m = 5.0; }
        if (ds < d) { d = ds; m = 6.0; }
        neon = min(df, ds + 0.05);
    }
    else if (sphere < d)
    {
        d = sphere;
        m = 0.0;
    }

    float dt = sdTrees(p);
    if (dt < d) { d = dt; m = 7.0; }

    float dfc = sdFence(p);
    if (dfc < d) { d = dfc; m = 8.0; }

    return float3(d, m, neon);
}

// Tetrahedron offsets for the normal estimate.
static const float3 NORMAL_K[4] =
{
    float3(1.0, -1.0, -1.0),
    float3(-1.0, -1.0, 1.0),
    float3(-1.0, 1.0, -1.0),
    float3(1.0, 1.0, 1.0)
};

float3 calcNormal(float3 p, float t)
{
    float3 n = float3(0.0, 0.0, 0.0);
    [loop] for (int i = 0; i < 4; i++)
    {
        float3 e = NORMAL_K[i];
        n += e * map(p + e * 0.0015, t).x;
    }
    return normalize(n);
}

float softShadow(float3 ro, float3 rd, float maxT, float t, int steps)
{
    float res = 1.0;
    float tt = 0.06;
    [loop] for (int i = 0; i < 40; i++)
    {
        if (i >= steps || tt > maxT)
        {
            break;
        }
        float h = map(ro + rd * tt, t).x;
        res = min(res, 9.0 * h / tt);
        if (res < 0.01)
        {
            break;
        }
        tt += clamp(h, 0.04, 0.45);
    }
    return saturate(res);
}

float calcAO(float3 p, float3 n, float t)
{
    float occ = 0.0;
    float sca = 1.0;
    [loop] for (int i = 0; i < 4; i++)
    {
        float h = 0.03 + 0.14 * float(i);
        float d = map(p + n * h, t).x;
        occ += (h - d) * sca;
        sca *= 0.75;
    }
    return saturate(1.0 - 2.2 * occ);
}

// Lightning: a few flickers at the end of some 8-second periods.
float lightning(float t)
{
    const float period = 8.3;
    float k = frac(t / period);
    float id = floor(t / period);
    float armed = step(0.90, k) * step(0.35, hash11(id * 7.31 + 2.0));
    float f = floor(t * 22.0);
    return armed * step(0.45, hash11(f * 1.37 + id)) * (0.4 + 0.6 * hash11(f * 2.71));
}

float3 sky(float3 rd, float t, float flash)
{
    float h = saturate(rd.y);
    float3 col = lerp(float3(0.046, 0.014, 0.082), float3(0.002, 0.002, 0.010), pow(h, 0.5));
    col += float3(0.34, 0.06, 0.48) * pow(saturate(1.0 - abs(rd.y)), 14.0) * 0.38;

    if (rd.y > 0.0)
    {
        float2 suv = rd.xz / (rd.y + 0.35) * 22.0;
        float2 c = floor(suv);
        float hs = hash21(c);
        float2 f = frac(suv) - 0.5 - (hash22(c + 9.0) - 0.5) * 0.7;
        float star = smoothstep(0.07, 0.0, length(f)) * step(0.80, hs);
        col += float3(0.90, 0.85, 1.00) * star * (0.55 + 0.45 * sin(t * 2.5 + hs * 60.0)) * h;
    }

    float3 md = normalize(float3(-0.55, 0.42, -0.72));
    float m = saturate(dot(rd, md));
    col += float3(0.50, 0.38, 0.85) * (pow(m, 24.0) * 0.20 + pow(m, 220.0) * 0.55);
    float disc = smoothstep(0.9968, 0.9973, m);
    float craters = noise2(rd.xy * 55.0) * 0.5 + noise2(rd.xy * 120.0) * 0.25;
    col = lerp(col, float3(0.92, 0.86, 1.00) * (0.75 + 0.4 * craters), disc);

    if (rd.y > -0.02)
    {
        float2 cuv = rd.xz / (rd.y + 0.28) * 1.35 + float2(t * 0.012, t * 0.006);
        float c = fbm2(cuv);
        float cover = smoothstep(0.42, 0.80, c) * smoothstep(-0.02, 0.22, rd.y);
        float moonlit = smoothstep(0.55, 0.95, c) * (0.3 + 0.7 * m * m * m);
        float3 cc = lerp(float3(0.010, 0.006, 0.022), float3(0.26, 0.17, 0.46), moonlit);
        cc += float3(0.75, 0.70, 1.00) * flash * (0.35 + 0.9 * c);
        col = lerp(col, cc, cover * 0.85 * (1.0 - disc * 0.6));
    }
    col += float3(0.25, 0.22, 0.45) * flash * 0.25;
    return col;
}

// What is seen through a face of the cube: stars, a nebula, and a smaller
// wireframe cube spinning around a bright singularity.
float3 voidInterior(float3 pos, float3 rd, float t)
{
    float3 qo = cubeTurn(pos - cubeCenter(t), t);
    float3 qd = cubeTurn(rd, t);
    float3 col = float3(0.0, 0.0, 0.0);

    float3 sd = qd;
    sd.xy = rot(sd.xy, t * 0.05);
    float2 suv = float2(atan2(sd.z, sd.x), asin(clamp(sd.y, -1.0, 1.0))) * 14.0;
    float2 sc = floor(suv);
    float sh = hash21(sc);
    float2 sf = frac(suv) - 0.5 - (hash22(sc + 2.0) - 0.5) * 0.7;
    float star = smoothstep(0.10, 0.0, length(sf)) * step(0.72, sh);
    col += float3(0.85, 0.75, 1.00) * star * (0.6 + 0.4 * sin(t * 3.0 + sh * 50.0));
    float nebula = fbm2(suv * 0.22 + float2(t * 0.03, 0.0));
    col += float3(0.30, 0.06, 0.55) * smoothstep(0.45, 0.90, nebula) * 0.55;

    float tt = 0.02;
    float g1 = 0.0;
    float g2 = 0.0;
    [loop] for (int i = 0; i < 26; i++)
    {
        float3 ip = qo + qd * tt;
        if (max(abs(ip.x), max(abs(ip.y), abs(ip.z))) > CUBE_R + 0.02)
        {
            break;
        }
        float3 r = ip;
        r.xy = rot(r.xy, t * 0.9);
        r.yz = rot(r.yz, -t * 0.7);
        r.xz = rot(r.xz, t * 0.5);
        float d1 = sdBoxFrame(r, float3(0.52, 0.52, 0.52), 0.022);
        float d2 = length(ip) - 0.17;
        g1 += 1.0 / (1.0 + d1 * d1 * 900.0);
        g2 += 1.0 / (1.0 + d2 * d2 * 260.0);
        tt += clamp(min(d1, abs(d2)), 0.035, 0.25);
    }
    col += float3(0.75, 0.35, 1.00) * g1 * 0.16 + float3(1.00, 0.55, 0.95) * g2 * 0.05;
    return col;
}

float4 PSScene(VSOut i) : SV_Target
{
    float time = uParams.x;
    float quality = saturate(uParams.y);
    int steps = int(lerp(56.0, 120.0, quality));
    int fogSteps = int(lerp(10.0, 30.0, quality));
    int shadowSteps = int(lerp(14.0, 32.0, quality));

    float2 p = i.uv * 2.0 - 1.0;
    p.y = -p.y;
    p.x *= uParams.z;

    float flash = lightning(time);
    float pulse = 0.85 + 0.15 * sin(time * 2.3) + 0.6 * flash;
    float3 violet = float3(0.66, 0.26, 1.00);
    float3 cubePos = cubeCenter(time);

    // Slow orbit around the cube, with a little hand-held sway.
    float ang = time * 0.10 + 0.9;
    float rad = 11.2 + 1.3 * sin(time * 0.071);
    float3 ro = float3(cos(ang) * rad, 3.3 + 0.75 * sin(time * 0.13), sin(ang) * rad);
    float3 ta = float3(0.0, 2.75 + 0.2 * sin(time * 0.31), 0.0);
    float3 sway = float3(noise2(float2(time * 1.7, 1.0)), noise2(float2(time * 1.9, 7.0)),
                         noise2(float2(time * 1.5, 3.0))) - 0.5;
    ta += sway * (0.06 + 0.25 * flash);
    float3 ww = normalize(ta - ro);
    float3 uu = normalize(cross(float3(0.0, 1.0, 0.0), ww));
    float3 vv = cross(ww, uu);
    float3 rd = normalize(p.x * uu + p.y * vv + 1.5 * ww);

    // Primary ray.
    float tHit = 0.05;
    float mat = 0.0;
    float glow = 0.0;
    bool hit = false;
    [loop] for (int s = 0; s < 128; s++)
    {
        if (s >= steps)
        {
            break;
        }
        float3 h = map(ro + rd * tHit, time);
        glow += 1.0 / (1.0 + h.z * h.z * 55.0);
        if (h.x < 0.0012 * tHit + 0.0008)
        {
            hit = true;
            mat = h.y;
            break;
        }
        tHit += h.x * 0.9;
        if (tHit > MAXD)
        {
            break;
        }
    }
    glow *= 96.0 / float(steps);

    // Rays that ran out of steps near the horizon still land on the ground.
    bool farGround = false;
    if (!hit && rd.y < -0.01)
    {
        float tPlane = (0.07 - ro.y) / rd.y;
        if (tPlane > 0.0 && tPlane < 400.0)
        {
            tHit = max(tHit, tPlane);
            hit = true;
            mat = 1.0;
            farGround = true;
        }
    }

    float3 col = float3(0.0, 0.0, 0.0);
    if (hit)
    {
        float3 pos = ro + rd * tHit;
        float3 n = float3(0.0, 1.0, 0.0);
        if (!farGround)
        {
            n = calcNormal(pos, time);
        }

        float3 L = cubePos - pos;
        float ld = length(L);
        L /= ld;
        float ndl = saturate(dot(n, L));
        float att = 1.0 / (1.0 + 0.045 * ld * ld);
        float shadow = 1.0;
        bool emissive = (mat > 3.5 && mat < 5.5);
        if (ndl > 0.0 && !emissive && !farGround)
        {
            shadow = softShadow(pos + n * 0.03, L, ld - 2.1, time, shadowSteps);
        }
        float ao = 1.0;
        if (!emissive && !farGround)
        {
            ao = calcAO(pos, n, time);
        }

        float3 moonDir = normalize(float3(-0.55, 0.42, -0.72));
        float3 lightC = violet * 7.0 * pulse * att * ndl * shadow;
        float3 moonC = float3(0.30, 0.36, 0.60) * 0.22 * saturate(dot(n, moonDir));
        float3 ambC = float3(0.030, 0.018, 0.055) * (0.45 + 0.55 * n.y) * ao;
        float3 boltC = float3(0.70, 0.65, 1.00) * flash * 1.6 * saturate(n.y * 0.7 + 0.3);

        float3 albedo = float3(0.0, 0.0, 0.0);
        float3 emis = float3(0.0, 0.0, 0.0);
        float spec = 0.0;
        if (mat < 1.5)
        {
            float g = noise2(pos.xz * 1.7);
            float g2 = noise2(pos.xz * 9.0);
            albedo = lerp(float3(0.045, 0.040, 0.055), float3(0.050, 0.075, 0.050),
                          smoothstep(0.35, 0.75, g)) * (0.7 + 0.5 * g2);
        }
        else if (mat < 2.5)
        {
            albedo = float3(0.16, 0.15, 0.19) * (0.7 + 0.4 * noise3(pos * 5.0));
            float r = length(pos.xz);
            float a = atan2(pos.z, pos.x);
            float rings = smoothstep(0.035, 0.0, abs(r - 1.55)) + smoothstep(0.030, 0.0, abs(r - 2.35)) +
                          smoothstep(0.020, 0.0, abs(r - 0.90));
            float runes = smoothstep(0.75, 0.95, sin(a * 12.0 + time * 0.3)) *
                          smoothstep(0.12, 0.0, abs(r - 1.95));
            float spokeAngle = abs(frac(a / TAU * 8.0 + 0.5) - 0.5) * TAU / 8.0;
            float spokes = smoothstep(0.035, 0.0, spokeAngle * r) * step(r, 1.55) * step(0.30, r);
            emis = violet * (rings + runes + spokes * 0.7) * (1.4 + 0.8 * sin(time * 1.7 + r * 3.0)) *
                   step(0.5, n.y);
            spec = 0.4;
        }
        else if (mat < 3.5)
        {
            float nz = noise3(pos * 4.5);
            albedo = float3(0.30, 0.28, 0.34) * (0.55 + 0.6 * nz);
            float moss = smoothstep(0.55, 0.0, pos.y) * 0.7 * noise2(pos.xz * 6.0 + 3.0);
            albedo = lerp(albedo, float3(0.06, 0.10, 0.06), moss);
            spec = 0.25;
        }
        else if (mat < 4.5)
        {
            emis = float3(0.80, 0.42, 1.00) * 5.0 * pulse + float3(1.2, 1.2, 1.2);
        }
        else if (mat < 5.5)
        {
            float fres = pow(1.0 - saturate(dot(n, -rd)), 3.0);
            emis = voidInterior(pos, rd, time) + violet * fres * 0.9;
        }
        else if (mat < 6.5)
        {
            albedo = float3(0.05, 0.03, 0.09);
            float rim = 1.0 - saturate(dot(n, -rd));
            emis = violet * rim * rim * 2.7;
            spec = 1.0;
        }
        else
        {
            albedo = float3(0.035, 0.030, 0.040);
            spec = 0.15;
        }

        float3 halfVec = normalize(L - rd);
        float sp = pow(saturate(dot(n, halfVec)), 32.0) * spec;
        col = albedo * (lightC + moonC + ambC + boltC) + violet * sp * 2.5 * att * shadow * pulse + emis;

        // Distance haze into the horizon colour.
        float haze = 1.0 - exp(-0.00030 * tHit * tHit);
        col = lerp(col, float3(0.030, 0.010, 0.056) + float3(0.25, 0.22, 0.45) * flash * 0.2, haze);
    }
    else
    {
        col = sky(rd, time, flash);
        tHit = MAXD;
    }

    // Volumetric fog lit by the cube: thin haze everywhere, thick mist hugging the ground.
    float fogEnd = min(tHit, 46.0);
    float stepLen = fogEnd / float(fogSteps);
    float tf = stepLen * hash21(i.pos.xy + frac(time) * 61.0);
    float3 scatter = float3(0.0, 0.0, 0.0);
    float trans = 1.0;
    [loop] for (int f = 0; f < 32; f++)
    {
        if (f >= fogSteps)
        {
            break;
        }
        float3 fp = ro + rd * tf;
        float nz = noise3(fp * 0.42 + float3(time * 0.09, -time * 0.03, time * 0.06));
        float height = max(fp.y, 0.0);
        float dens = (0.020 * exp(-height * 0.45) + 0.110 * exp(-height * 2.6) * nz) * (0.45 + 1.1 * nz);
        float3 lp = cubePos - fp;
        float l2 = dot(lp, lp);
        float3 li = violet * (5.2 * pulse / (1.0 + 0.55 * l2)) + float3(0.010, 0.012, 0.026) +
                    float3(0.60, 0.55, 1.00) * flash * 0.8;
        scatter += trans * dens * li * stepLen;
        trans *= exp(-dens * stepLen * 1.25);
        tf += stepLen;
    }
    col = col * trans + scatter;

    // Energy beam through the cube's vertical axis, integrated analytically along
    // the ray (a Gaussian around the axis), so it stays smooth at any step count.
    {
        float a2 = dot(rd.xz, rd.xz);
        float b2 = dot(ro.xz, rd.xz);
        float c2 = dot(ro.xz, ro.xz);
        if (a2 > 0.00001)
        {
            float tc = -b2 / a2;
            if (tc > 0.0 && tc < tHit + 0.6)
            {
                float d2 = max(c2 - b2 * b2 / a2, 0.0);
                float by = ro.y + rd.y * tc;
                const float k = 5.5;
                float core = exp(-k * d2) * sqrt(3.14159 / (k * a2));
                float flow = 0.55 + 0.45 * noise2(float2(by * 1.3 - time * 2.6, 2.0)) ;
                float fade = smoothstep(0.25, 1.1, by) * exp(-max(by - 5.0, 0.0) * 0.10);
                col += float3(0.74, 0.38, 1.00) * core * flow * fade * 0.42 * pulse;
                col += float3(1.00, 0.85, 1.00) * exp(-k * 9.0 * d2) * sqrt(3.14159 / (k * 9.0 * a2)) *
                       fade * 0.30 * pulse;
            }
        }
    }

    // Spirit wisps drifting between the graves (cyan / magenta points of light).
    {
        float3 wisps = float3(0.0, 0.0, 0.0);
        for (int w = 0; w < 7; w++)
        {
            float fw = float(w);
            float dir = (frac(fw * 0.5) > 0.25) ? 1.0 : -1.0;
            float wa = fw * 0.898 + time * (0.11 + 0.03 * fw) * dir;
            float wr = 4.4 + 0.75 * fw + 1.4 * sin(fw * 2.3 + time * 0.17);
            float3 wp = float3(cos(wa) * wr, 0.85 + 0.12 * fw + 0.45 * sin(time * 0.9 + fw * 1.9), sin(wa) * wr);
            float3 dw = wp - ro;
            float tw = dot(dw, rd);
            if (tw > 0.0 && tw < tHit)
            {
                float3 cw = dw - rd * tw;
                float w2 = dot(cw, cw);
                float flick = 0.65 + 0.35 * sin(time * 5.0 + fw * 7.0);
                float amount = (1.0 / (1.0 + w2 * 260.0) + 0.05 / (1.0 + w2 * 7.0)) * flick;
                float3 tint = lerp(float3(0.35, 0.90, 1.00), float3(1.00, 0.40, 0.95), frac(fw * 0.37));
                wisps += tint * amount;
            }
        }
        col += wisps * 0.9;
    }

    col += glow * 0.035 * float3(0.70, 0.32, 1.00) * pulse;

    // Tone map, grade, vignette, grain.
    col = 1.0 - exp(-col * 1.45);
    col = pow(saturate(col), float3(0.4545, 0.4545, 0.4545));
    col = lerp(col, col * col * (3.0 - 2.0 * col), 0.65);   // S-curve: deep blacks, hot neon
    col = col * float3(1.03, 0.95, 1.10) + float3(0.008, 0.002, 0.016);
    float2 vq = i.uv - 0.5;
    col *= 1.0 - dot(vq, vq) * 1.1;
    col += (hash21(i.pos.xy + time) - 0.5) * 0.018;
    return float4(saturate(col), 1.0);
}
)HLSL";

// Minimal fallback scene, used only if the main shader cannot be created on a
// machine: still a full-screen procedural load, but with the simplest HLSL.
inline const char kGpuFallbackSource[] = R"HLSL(
cbuffer FrameCB : register(b0)
{
    float4 uRes;
    float4 uParams;
    float4 uBlit;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSScene(VSOut i) : SV_Target
{
    float t = uParams.x;
    float2 p = i.uv * 2.0 - 1.0;
    p.x *= uParams.z;
    float v = 0.0;
    float2 q = p;
    for (int k = 0; k < 48; k++)
    {
        q = float2(q.x * q.x - q.y * q.y, 2.0 * q.x * q.y) * 0.5 + float2(sin(t * 0.3 + float(k) * 0.13), cos(t * 0.23)) * 0.6;
        v += exp(-dot(q, q) * 1.5);
    }
    v /= 48.0;
    float3 col = float3(0.55, 0.20, 0.95) * v * 2.4 + float3(0.02, 0.01, 0.05);
    return float4(saturate(col), 1.0);
}
)HLSL";

}  // namespace nt

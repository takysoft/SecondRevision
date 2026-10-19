// SecondRevision - the shadow factor (docs/terv-latvany-roadmap.md 3.2; FFinalPass::DrawShadowFactor). One source, two
// builds:
//   - without RDX_RT, the shadow maps' factor: the renderer compiles it at run time with D3DCompile, from the text the
//     build puts into the DLL (ShadowFactorText.h, scripts/embed-text.ps1);
//   - with RDX_RT=1, the RT shadows' factor (docs/terv-rt.md): the build compiles it with dxc into DXIL
//     (ShadowFactorRtVS.h, ShadowFactorRtPS.h). The same lights, the same shares, the same factor out - only whether a
//     light is blocked comes from rays traced against the scene (FRtPass::BuildScene) instead of the cube maps.
// Plain ASCII: the build refuses anything else (embed-text.ps1).
//
// Shadow maps (FShadowMaps). For every pixel of the factor target the world's surface point is rebuilt from the depth,
// and each chosen light's two cube maps are asked: does an actor stand between the surface and the light (the actors'
// map), and no wall (the level's map)? Where both hold, the light's share of the pixel's light is taken away - its
// estimated contribution against all the light the pixel receives (RdxMath::RelativeShadowShare). The level's own shadows
// are in its light maps already: the level's map keeps an actor's shadow off a spot a wall has darkened there. Soft
// edges: comparison samples around the direction, each a 2x2 of the linear comparison sampler - a quick five first, all
// lit or all dark being the answer, then Taps more on a spiral. A pixel drawn onto after the engine's ClearZ (the weapon,
// NoOnTop 0), or with something translucent over it, is left alone.

// The light arrays' length: handed over as a macro by the run-time compile (FinalPass.cpp), read from the C++ side's own
// header by dxc - one number either way.
#ifndef RDX_SHADOW_MAX_LIGHTS
#include "RenderLimits.h"
#endif

Texture2D<float> WorldDepth : register(t0);
Texture2D<float> OnTopDepth : register(t1);
Texture2D<uint2> WorldStencil : register(t2);        // .g: 0 the level's BSP, 1 an actor, 2 translucent on top
Texture2D<float> SceneLight : register(t3);          // the light each surface receives (FSolidPass, PSMainLit)
TextureCubeArray<float> WorldCubes : register(t4);   // FShadowMaps: the level's geometry, a cube a light
TextureCubeArray<float> ActorCubes : register(t5);   // FShadowMaps: the actors
SamplerComparisonState Cmp : register(s0);           // reversed depth: lit where the reference >= the stored depth
cbuffer Shadow : register(b0)
{
    float TanX; float TanY; uint Scale; uint Count;
    uint NoOnTop; uint Flags; float Calib; float MaxShare;   // Flags: 1 the relative share, 2 the share map, 4 walls cast,
                                                             // 16 (RT) the rays a pixel traced instead of the factor,
                                                             // 32 (RT) the actors' non-opaque triangles never block,
                                                             // 64 (RT) only the level blocks, 128 only the actors -
                                                             // measurements
    float3 AxisX; float Near;                                // the camera's axes in world space
    float3 AxisY; float ActorTexel;                          // a map face's texel at unit distance: 2 / its size
    float3 AxisZ; float WorldTexel;
    uint Taps; uint Marks; float Spread; float LightSize;    // Spread: the soft edge's radius, in direction units
                                                             // LightSize: the RT shadows' light, a disc this wide (radius)
    float4 ViewRect;                                         // the world view's part of the target: x y width height, 0..1
    float4 SkipRect[4];                                      // views drawn later onto other parts of the screen: no shadows
    uint SkipCount; float3 Eye;                              // Eye: the camera in world space, for the RT shadows' rays
// The light arrays come LAST, and their length is pasted in from RdxMath::ShadowMaxLights - see the layout enum there.
// Anything declared after them would shift when the count changes, and the C++ side fills this buffer by index.
    float4 LightCam[RDX_SHADOW_MAX_LIGHTS];   // xyz camera space, w radius
    float4 LightInfo[RDX_SHADOW_MAX_LIGHTS];  // x the light's cube, y brightness 0..1, z its weight 0..1
    float4 LightSpot[RDX_SHADOW_MAX_LIGHTS];  // xyz axis in camera space, w cos of the half angle (< -1: a point)
};

float4 VSMain(uint Id : SV_VertexID) : SV_Position
{
    float2 P = float2((Id << 1) & 2, Id & 2);
    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);
}

// A pixel's place within the world view's own part of the target (ViewRect), -1..1.
float2 ViewNdc(int2 P, float2 Size)
{
    return ((float2(P) + 0.5f) / Size - ViewRect.xy) / ViewRect.zw * 2.0f - 1.0f;
}

float3 ViewAt(int2 P, float2 Size)
{
    float D = WorldDepth.Load(int3(P, 0));
    float W = (D > 0.0f) ? 1.0f / D : 1.0e6f;
    float2 Ndc = ViewNdc(P, Size);
    return float3(Ndc.x * TanX * W, Ndc.y * TanY * W, W);
}

// One comparison: the surface Dist from the light along the unit direction D. Its reference depth is taken along D itself
// - near a cube's edge a sample may fall onto the next face, whose depth runs along another axis.
float TapLit(TextureCubeArray<float> Cube, float3 D, float Dist, float Bias, float Slot)
{
    float3 V = D * Dist;
    float Ma = max(abs(V.x), max(abs(V.y), abs(V.z)));
    return Cube.SampleCmpLevelZero(Cmp, float4(D, Slot), Near / max(Ma - Bias, Near));
}

void Basis(float3 D, out float3 T, out float3 B)
{
    float3 Up = (abs(D.z) < 0.9f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    T = normalize(cross(Up, D));
    B = cross(D, T);
}

// The quick look: the middle and four around it, as a sum (0..5).
float Pcf5(TextureCubeArray<float> Cube, float3 D, float3 T, float3 B, float Dist, float Bias, float Slot, float Radius)
{
    float R = 0.7f * Radius;
    float Sum = TapLit(Cube, D, Dist, Bias, Slot);
    Sum += TapLit(Cube, normalize(D + (T + B) * R), Dist, Bias, Slot);
    Sum += TapLit(Cube, normalize(D + (T - B) * R), Dist, Bias, Slot);
    Sum += TapLit(Cube, normalize(D - (T - B) * R), Dist, Bias, Slot);
    Sum += TapLit(Cube, normalize(D - (T + B) * R), Dist, Bias, Slot);
    return Sum;
}

float Pcf(TextureCubeArray<float> Cube, float3 D, float Dist, float Bias, float Slot, float Radius, uint N, float Rot)
{
    float3 T, B;
    Basis(D, T, B);
    float Sum = Pcf5(Cube, D, T, B, Dist, Bias, Slot, Radius);
    if (Sum >= 4.995f || Sum <= 0.005f)
        return Sum / 5.0f;
    [loop] for (uint k = 0; k < N; k++)
    {
        float A = float(k) * 2.39996323f + Rot;
        float Rk = sqrt((float(k) + 0.5f) / float(N)) * Radius;
        Sum += TapLit(Cube, normalize(D + (T * cos(A) + B * sin(A)) * Rk), Dist, Bias, Slot);
    }
    return Sum / float(5u + N);
}

// One light's share of the pixel's own light, before anything is looked up in the maps - arithmetic only, so it can be
// summed over every light first (ShadowFactorSurface.hlsli). MaxShare is NOT in it: that is the artistic strength, and it is
// applied after the shares have been normalised.
float LightShare(uint i, float3 S, float3 N, float Total)
{
    float R = LightCam[i].w;
    float3 ToL = LightCam[i].xyz - S;
    float Dist = length(ToL);
    if (Dist >= R || Dist < 1.0f)
        return 0.0f;
    float NdotL = dot(N, ToL / Dist);
    if (NdotL < 0.02f)
        return 0.0f;
    float Cone = 1.0f;
    if (LightSpot[i].w > -1.5f)
        Cone = saturate((dot(-ToL / Dist, LightSpot[i].xyz) - LightSpot[i].w) / max(1.0f - LightSpot[i].w, 0.001f));
    float Att = 1.0f - Dist / R;
    // The graduated pick's weight (RdxMath::ShadowLightWeight, followed over time) AFTER the saturate: a near, strong light
    // saturates, and a weight inside it would do nothing to exactly the lights that matter most. Being in LightShare, it
    // counts in SumShare as well - a faded light takes less of the pixel's share from the lights near it.
    return (((Flags & 1u) != 0u)
        ? saturate(Calib * LightInfo[i].y * Att * Att * NdotL * Cone / max(Total, 0.02f))
: saturate(Calib * LightInfo[i].y * Att * Att) * Cone) * LightInfo[i].z;
}


#ifndef RDX_RT

float PSMain(float4 Pos : SV_Position) : SV_Target
{
#include "ShadowFactorSurface.hlsli"
    float Result = 1.0f;
    [loop] for (uint i = 0; i < Count; i++)
    {
        float R = LightCam[i].w;
        float3 ToL = LightCam[i].xyz - S;
        float Dist = length(ToL);
        if (Dist >= R || Dist < 1.0f)
            continue;
        float NdotL = dot(N, ToL / Dist);
        if (NdotL < 0.02f)
            continue;
        // The share: of the pixel's own light - a dim light on a dark street takes most of what there is - or, with the
        // marker SecondRevision-shadow-absolute.txt, absolute. The cone's fade and the distance falloff are LightShare's
        // own, and they live only there: a second copy here would be a formula to keep in step with the first.
        float Share = LightShare(i, S, N, Total) * ShareNorm;
        if (Share < 0.004f)
            continue;
        // The share map (the marker SecondRevision-shadow-ratio.txt): as if every light were blocked.
        if ((Flags & 2u) != 0u)
        {
            Result *= 1.0f - Share;
            continue;
        }
        // From the light to the surface, turned into the world's orientation - the maps'. Moved off the surface along its
        // normal by a texel or two of the map at that distance, so that the surface does not shadow itself.
        float Slot = LightInfo[i].x;
        float3 Vc = -ToL;
        float3 Vw = AxisX * Vc.x + AxisY * Vc.y + AxisZ * Vc.z;
        float Ma = max(abs(Vw.x), max(abs(Vw.y), abs(Vw.z)));
        // An actor's own surface is in the actors' map too: it needs more room, or it shadows itself in a fine
        // pattern across the flat of it.
        float TexA = Ma * ActorTexel;
        // And more still where the light grazes the surface: the depth runs steeply across a map texel there, and a mesh's
        // flat facets shadow themselves in patches without it. The slope counts at most 8.
        float Slope = min(sqrt(saturate(1.0f - NdotL * NdotL)) / NdotL, 8.0f);
        // The level's own surfaces are in no actors' map (only the movers' are), so their push stays a texel and a half: with
        // the slope's share it would take a wall's lookup, where a light grazes it, off the wall and into a nearby
        // box's shadow - a false faint patch, and one that grows with the map's texel. The depth's slack still grows
        // with the slope.
        float Push = (Kind == 1u) ? (3.0f + 1.5f * Slope) : 1.5f;
        float Slack = (Kind == 1u) ? ((2.0f + 1.5f * Slope) * TexA + 2.0f) : ((1.0f + 0.75f * Slope) * TexA + 0.5f);
        float3 Va = Vw + Nw * (Push * TexA);
        float Da = length(Va);
        float Lit = Pcf(ActorCubes, Va / Da, Da, Slack, Slot, Spread, Taps, Rot);
        // With the level's own geometry casting too (Flags 4), a pixel with no actor between it and the light can still
        // be in shadow - so the world lookup must not be skipped here. Without the bit the old way stands, for the A/B.
        if (Lit >= 0.999f && (Flags & 4u) == 0u)
            continue;
        float TexW = Ma * WorldTexel;
        // The same slope-scaled push the actors' lookup gets. Without it, where a light grazes a surface the depth runs
        // steeply across a map texel and the surface shadows ITSELF in the world map - which took the shadow off walls
        // altogether: the shadow fell on the grass but not on the wall right behind it.
        float3 Vb = Vw + Nw * ((6.0f + 4.0f * Slope) * TexW);
        float Db = length(Vb);
        float3 Tw, Bw;
        Basis(Vb / Db, Tw, Bw);
        // The receiving surface is in the world map ITSELF, so every world surface has to pass a comparison against its
        // own stored depth - and an absolute slack runs out with distance, because a map texel covers more and more world
        // as it goes. With an absolute slack the whole picture goes nearly black (mean 44 -> 13, 87%
        // of the pixels darker) - every wall shadowed itself. Hence the term that grows with the distance.
        float WorldBias = (3.0f + 2.0f * Slope) * TexW + 0.02f * Ma + 2.0f;
        float Open = Pcf5(WorldCubes, Vb / Db, Tw, Bw, Db, WorldBias, Slot, 2.0f * WorldTexel) / 5.0f;
        // Flags 4: the level's geometry blocks the light like an actor does - a wall, a built-in desk, a bench. Its
        // shadow is in the level's light map as well, so the two add up; that is accepted, and
        // the strength is turned down globally at the end instead. Without the bit the world only gates an actor's
        // shadow, exactly as before.
        float Blocked = ((Flags & 4u) != 0u) ? (1.0f - Lit * Open) : ((1.0f - Lit) * Open);
        Result *= 1.0f - Share * Blocked;
    }
    return Result;
}

#else   // RDX_RT

// The scene the rays are traced against (FRtPass::BuildScene): the level's instance masked 1, the actors' 2, their
// triangles in world units.
RaytracingAccelerationStructure Scene : register(t6);

cbuffer RtShadow : register(b1)
{
    float4 AoParams;    // the RT occlusion's (AoPS): x its radius, world units; y its strength - the SSAO's own two
    uint4 RtKnobs;      // the RT factor's: x the most lights a pixel traces (1..RT_TRACED_MAX), y the edges' samples; z the
                        // instances the occlusion's rays meet, 0 for the level and the actors (another: a measurement's);
                        // w the actors' geometry that is the player's own body, which those rays pass (none: 0xFFFFFFFF)
    uint4 RtRows;       // RtGeo's rows of bits: x where they start, in uints; y how many uints a light's row is
};
// The scene's geometries, as many as the frame holds (FRtPass::FillShadowExclusion): a uint each - its first vertex in
// the shadow maps' buffers, bit 31 set for the arena's. Then for each light, in the order of the arrays above, a row of
// bits, one a geometry: the actors' geometries its rays pass through - its own mesh, a lamp's shade around it
// (FShadowLightFrame::Own, turned into the scene's geometry order by FRtPass).
ByteAddressBuffer RtGeo : register(t10);
// The casters' texture places - u, v and the mask's slot a vertex, three floats (FShadowMaps::CasterUvw) - the frame's
// and the arena's, and the masks the maps are drawn with, sampled as they sample them.
ByteAddressBuffer FrameUvw : register(t7);
ByteAddressBuffer ArenaUvw : register(t8);
Texture2DArray<float> Masks : register(t9);
SamplerState MaskSampler : register(s1);

// Where a ray meets a masked caster's triangle (a leaf, a fence): is its texture opaque there? Slot 0 is solid.
bool OpaqueAt(uint G, uint Prim, float2 Bary)
{
    uint Info = RtGeo.Load(G * 4u);
    uint At = ((Info & 0x7FFFFFFFu) + Prim * 3u) * 12u;
    float3 A, B, C;
    if ((Info >> 31) != 0u)
    {
        A = asfloat(ArenaUvw.Load3(At));
        B = asfloat(ArenaUvw.Load3(At + 12u));
        C = asfloat(ArenaUvw.Load3(At + 24u));
    }
    else
    {
        A = asfloat(FrameUvw.Load3(At));
        B = asfloat(FrameUvw.Load3(At + 12u));
        C = asfloat(FrameUvw.Load3(At + 24u));
    }
    if (A.z < 0.5f)
        return true;
    float2 Uv = A.xy * (1.0f - Bary.x - Bary.y) + B.xy * Bary.x + C.xy * Bary.y;
    return Masks.SampleLevel(MaskSampler, float3(Uv, A.z), 0.0f) >= 0.5f;
}

// A pixel's surface, as the RT factor works with it (FactorSurface).
struct FSurface
{
    uint   Kind;       // 0 the level's BSP, 1 an actor
    float3 S;          // the surface point, camera space
    float3 N;          // its normal (towards the viewer's side), camera space
    float3 Nw;         // the same normal in world space
    float  Total;      // all the light the pixel receives
    float  Rot;        // a turn of the soft edge's spiral, per pixel
    float  ShareNorm;  // what every light's share is multiplied by (see the sum below)
};

// Everything before the lights, as the maps' PSMain has it inline (ShadowFactorSurface.hlsli). Negative when the pixel is
// a surface to shadow, with Sf filled; otherwise the factor the pixel gets as it is - 1 (nothing of the world's to shadow
// there), or 0 (a light's mark in the diagnostic view).
float FactorSurface(float4 Pos, out FSurface Sf)
{
    Sf = (FSurface)0;
#include "ShadowFactorSurface.hlsli"
    Sf.Kind      = Kind;
    Sf.S         = S;
    Sf.N         = N;
    Sf.Nw        = Nw;
    Sf.Total     = Total;
    Sf.Rot       = Rot;
    Sf.ShareNorm = ShareNorm;
    return -1.0f;
}

// The samples a pixel adds where a shadow's edge crosses its 2x2 quad (see PSMain) - RtKnobs.y, RdxMath::RtShadowEdgeSamples
// - and the share a light must have for them: below it the edge's soft part is too faint to show.
#define RT_EDGE_SHARE   0.03f
// The most lights a pixel traces rays to - RtKnobs.x, RdxMath::RtShadowTracedLights, at most RT_TRACED_MAX: those with the
// largest shares of its light. 0, the default, is every light of the frame with a share, as the maps take them: the
// preset's graduated pick then decides alone, where a cut per pixel lets a shadow come and go as two lights' shares cross.
#define RT_TRACED_MAX 8u
// The lights a pixel keeps track of, a bit each. The presets pick 20 at the most; a light past these is left unshadowed.
#define RT_MASK_LIGHTS 32u

// Whether light Light's rays pass the actors' geometry G - its own mesh, its shade: its bit in the light's row of
// RtGeo.
bool LightPasses(uint Light, uint G)
{
    return ((RtGeo.Load((RtRows.x + Light * RtRows.y + (G >> 5u)) * 4u) >> (G & 31u)) & 1u) != 0u;
}

// What is between the point O and the point T (world space), of the instances in Mask: 0 nothing, 1 the level, 2 an actor
// - the first thing the ray finds, not the nearest. The ray ends Stop short of T, and is light Light's. Only the
// actors' geometries with a mask, or that some light must not see, are non-opaque in the scene (FRtPass::BuildScene),
// so only those come back here as candidates; every other triangle ends the ray on its own.
uint HitWhat(float3 O, float3 T, float Stop, uint Light, uint Mask)
{
    float3 D = T - O;
    float Len = length(D);
    if (Len - Stop <= 0.01f)
        return 0u;
    RayDesc Ray;
    Ray.Origin    = O;
    Ray.Direction = D / Len;
    Ray.TMin      = 0.0f;
    Ray.TMax      = Len - Stop;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> Q;
    Q.TraceRayInline(Scene, RAY_FLAG_NONE, Mask, Ray);
    while (Q.Proceed())
    {
        uint G = Q.CandidateGeometryIndex();
        if (Q.CandidateInstanceID() != 1u)
            Q.CommitNonOpaqueTriangleHit();
        else if ((Flags & 32u) == 0u && !LightPasses(Light, G) &&
                 OpaqueAt(G, Q.CandidatePrimitiveIndex(), Q.CandidateTriangleBarycentrics()))
            Q.CommitNonOpaqueTriangleHit();
    }
    if (Q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return 0u;
    return (Q.CommittedInstanceID() == 1u) ? 2u : 1u;
}

// Is light blocked on its way from T to the surface at O? With the level's geometry casting (Flags 4 - on unless the marker
// SecondRevision-shadow-bsp-off.txt turns it off, FBackend::RequestShadowWorldCast), exactly as the maps take it: anything in
// the way blocks, on every surface - a wall, a desk built into the level, a body. On the level's own surfaces its shadow
// is in the light map as well, so there the two add up; the maps accept that the same way, and the compensation takes the
// strength down for the picture as a whole. One ray.
// Without the bit, by what the surface is (Kind): an actor has no light map, so everything in the way blocks. A surface of
// the level keeps only what its light map lacks, the actors' shadows: an actor blocks, and only where no wall stands
// between as well - a light behind a wall gives this surface nothing, and a body in front of that wall must not take it
// away. One ray against everything first - a wall ends it early - and against the level alone only when it found an actor.
// Flags 64 and 128 test the level alone or the actors alone, whatever the surface.
bool BlockedFor(float3 O, float3 T, float Stop, uint Light, uint Kind)
{
    // The first ray's instances: everything - or for the measurements the level alone (Flags 64), the actors alone (128).
    // One place that traces, in a loop of at most two turns, so that the shader stays small.
    uint Mask = ((Flags & 64u) != 0u) ? 0x01u : (((Flags & 128u) != 0u) ? 0x02u : 0x03u);
    // Anything blocks: with the level casting, on an actor's surface, and in the measurements.
    bool Any = (Flags & 196u) != 0u || Kind == 1u;
    bool Blocked = false;
    [loop] for (uint Pass = 0u; Pass < 2u; Pass++)
    {
        uint What = HitWhat(O, T, Stop, Light, Mask);
        if (Pass == 1u)
        {
            Blocked = (What == 0u);              // an actor on the level's surface: blocked unless a wall stands between
            break;
        }
        if (Any || What != 2u)
        {
            // Anything blocks, or - on the level's surface, without the level casting - nothing, or a wall, whose shade its
            // light map holds.
            Blocked = Any ? (What != 0u) : false;
            break;
        }
        Mask = 0x01u;
    }
    return Blocked;
}

// The rays of light i from the pixel's surface point Sw: where they start - off the surface along its normal, so that the
// surface does not block its own rays - where the light is, and the disc across the direction to it. The rebuilt point is
// only as good as the depth, and an actor's surface on the screen is its mesh at the level of detail its distance gave it,
// while the scene holds the whole mesh - so more room for actors, and more with the distance and where the light grazes
// the surface.
void LightRays(uint i, FSurface Sf, float3 Sw, out float3 O, out float3 Lw, out float3 Tl, out float3 Bl)
{
    float3 Lc = LightCam[i].xyz;
    Lw = Eye + AxisX * Lc.x + AxisY * Lc.y + AxisZ * Lc.z;
    float3 ToL = Lw - Sw;
    float3 L = ToL / max(length(ToL), 1.0e-3f);
    float NdotL = max(dot(Sf.Nw, L), 0.02f);
    float Slope = min(sqrt(saturate(1.0f - NdotL * NdotL)) / NdotL, 8.0f);
    float Push = ((Sf.Kind == 1u) ? (1.0f + 0.004f * Sf.S.z) : (0.25f + 0.001f * Sf.S.z)) * (1.0f + 0.25f * Slope);
    O = Sw + Sf.Nw * Push;
    Basis(L, Tl, Bl);
}

// The lights are discs LightSize in radius, facing the pixel, so a shadow is sharp where the blocker touches the surface
// and softer the further it stands off. What blocks depends on the surface (BlockedFor): on an actor anything, on the
// level an actor with no wall between. A ray stops short of the disc by the maps' near plane and the disc's own radius: a
// light set close to a ceiling has part of its disc beyond it, and that must not count as a blocker.
//
// Each pixel of a 2x2 quad traces its first ray to a light at its own quarter of the disc; where the four agree, that is
// the answer for all four - most of the picture, fully lit or fully dark. Where they do not, a shadow's edge crosses the
// quad, and each of its pixels adds RtKnobs.y more on a spiral over the whole disc. Three rays make a grainy edge alone,
// so the texels of a four-by-four tile take their rays at turns of their own, and the pass after this averages the tile
// (FFinalPass::DrawRtFactorBlur): an edge from the tile's forty-eight.
//
// What a ray costs is the WAVE's time, not the pixel's: the pixels of a wave trace together, and a turn is done when its
// slowest ray is. Taken light by light, a light that one pixel needs cost the whole wave a turn, and a wave's pixels need
// different lights - measured on SHADOW10, neither fewer lights a pixel nor fewer rays a quad made it any faster. So each
// pixel works through its own list instead, a ray a turn, and a wave takes as many turns as its busiest pixel needs: first
// the first rays, then - after the quad's vote, which needs every lane at it - the edges' samples.
static uint GRays = 0u;   // the rays this pixel traced, for the diagnostic view (Flags 16)

float PSMain(float4 Pos : SV_Position) : SV_Target
{
    FSurface Sf;
    float Early = FactorSurface(Pos, Sf);
    bool On = Early < 0.0f;
    float3 Sw = Eye + AxisX * Sf.S.x + AxisY * Sf.S.y + AxisZ * Sf.S.z;
    float Stop = Near + LightSize;
    // The first rays' quarter, and a turn of their pattern for the quad: the four quads of a four-by-four tile a sixteenth
    // of a turn apart, so that the tile's sixteen first rays go evenly round the disc. The smoothing after this pass
    // averages the tile (FFinalPass::DrawRtFactorBlur), so what the tile's texels trace between them is what an edge gets.
    uint2 Q = uint2(Pos.xy);
    float Corner = float((Q.x & 1u) + 2u * (Q.y & 1u));
    float QuadTurn = float(((Q.x >> 1) & 1u) + 2u * ((Q.y >> 1) & 1u));
    float A0 = QuadTurn * 0.3926991f + Corner * 1.5707963f + 0.7853982f;
    // The edges' spiral the same way: a turn of its own and a step outwards of its own a texel of the tile - the turns in
    // the bits' reverse order, the steps in another, so the two do not go round together.
    uint Tile = (Q.x & 3u) + ((Q.y & 3u) << 2);
    float TileTurn = (float(reversebits(Tile) >> 28) + 0.5f) / 16.0f * 6.2831853f;
    float TileStep = (float((Tile * 7u) & 15u) + 0.5f) / 16.0f;
    uint Lights = min(Count, RT_MASK_LIGHTS);
    // The share a light needs for a ray: with RtKnobs.x at 0 the floor alone, otherwise the RtKnobs.x-th largest of the
    // pixel's, found by passing each share down a short list kept in order - each place keeps the larger, and hands the
    // smaller on - and then picked out of it with no index that varies (a varying one would put the list into memory).
    uint Traced = min(RtKnobs.x, RT_TRACED_MAX);
    uint Edges = min(RtKnobs.y, 16u);
    float Floor = 0.004f;
    if (On && Traced > 0u)
    {
        float Top[RT_TRACED_MAX];
        [unroll] for (uint t = 0; t < RT_TRACED_MAX; t++)
            Top[t] = 0.0f;
        [loop] for (uint j = 0; j < Lights; j++)
        {
            float Sj = LightShare(j, Sf.S, Sf.N, Sf.Total);
            [unroll] for (uint t = 0; t < RT_TRACED_MAX; t++)
            {
                float Hi = max(Top[t], Sj);
                Sj = min(Top[t], Sj);
                Top[t] = Hi;
            }
        }
        float Last = 0.0f;
        [unroll] for (uint t = 0; t < RT_TRACED_MAX; t++)
            Last = (t + 1u == Traced) ? Top[t] : Last;
        Floor = max(Floor, Last * Sf.ShareNorm);
    }
    // The pixel's lists: the lights it traces (Need), and of those the ones whose edge would show (Strong). The share map
    // (the marker SecondRevision-shadow-ratio.txt) traces nothing: every light as if blocked.
    float Result = 1.0f;
    uint Need = 0u, Strong = 0u;
    [loop] for (uint i = 0; i < Lights; i++)
    {
        float Share = On ? LightShare(i, Sf.S, Sf.N, Sf.Total) * Sf.ShareNorm : 0.0f;
        if ((Flags & 2u) != 0u)
        {
            if (Share >= 0.004f)
                Result *= 1.0f - Share;
        }
        else if (Share >= Floor)
        {
            Need |= 1u << i;
            if (Share >= RT_EDGE_SHARE)
                Strong |= 1u << i;
        }
    }
    // The first rays, one light of the list a turn.
    uint Todo = Need, Hit = 0u;
    [loop] while (WaveActiveAnyTrue(Todo != 0u))
    {
        if (Todo != 0u)
        {
            uint i = firstbitlow(Todo);
            Todo &= Todo - 1u;
            float3 O, Lw, Tl, Bl;
            LightRays(i, Sf, Sw, O, Lw, Tl, Bl);
            if (BlockedFor(O, Lw + (Tl * cos(A0) + Bl * sin(A0)) * (0.7f * LightSize), Stop, i, Sf.Kind))
                Hit |= 1u << i;
            GRays++;
        }
    }
    // The quad's vote: a light that one of the four found open and another blocked has its shadow's edge across the quad.
    uint Open = Need & ~Hit;
    uint AnyOpen = Open | QuadReadAcrossX(Open) | QuadReadAcrossY(Open) | QuadReadAcrossDiagonal(Open);
    uint AnyHit  = Hit | QuadReadAcrossX(Hit) | QuadReadAcrossY(Hit) | QuadReadAcrossDiagonal(Hit);
    uint Edge = (Edges > 0u) ? (Need & Strong & AnyOpen & AnyHit) : 0u;
    // Where the first ray is the answer: the light blocked or not.
    uint Plain = Need & ~Edge;
    [loop] while (Plain != 0u)
    {
        uint i = firstbitlow(Plain);
        Plain &= Plain - 1u;
        if ((Hit >> i) & 1u)
            Result *= 1.0f - LightShare(i, Sf.S, Sf.N, Sf.Total) * Sf.ShareNorm;
    }
    // The edges: Edges more for each, a ray a turn; a light done, its share of the pixel taken by what its samples found
    // blocked.
    uint K = 0u;
    float Sum = 0.0f;
    [loop] while (WaveActiveAnyTrue(Edge != 0u))
    {
        if (Edge != 0u)
        {
            uint i = firstbitlow(Edge);
            float3 O, Lw, Tl, Bl;
            LightRays(i, Sf, Sw, O, Lw, Tl, Bl);
            float A = float(K) * 2.39996323f + TileTurn;
            float R = sqrt((float(K) + TileStep) / float(Edges)) * LightSize;
            if (BlockedFor(O, Lw + (Tl * cos(A) + Bl * sin(A)) * R, Stop, i, Sf.Kind))
                Sum += 1.0f;
            GRays++;
            K++;
            if (K == Edges)
            {
                float Blocked = (Sum + float((Hit >> i) & 1u)) / float(Edges + 1u);
                Result *= 1.0f - LightShare(i, Sf.S, Sf.N, Sf.Total) * Sf.ShareNorm * Blocked;
                Edge &= Edge - 1u;
                K = 0u;
                Sum = 0.0f;
            }
        }
    }
    // The diagnostic view (Flags 16): white no ray, black 48 or more.
    if ((Flags & 16u) != 0u)
        return On ? saturate(1.0f - float(GRays) / 48.0f) : 1.0f;
    return On ? Result : Early;
}

// The RT ambient occlusion (docs/terv-rt.md; FFinalPass::DrawAoRt): the raw occlusion the SSAO's raw pass would write
// (GFinalAoShader) - one value a factor texel into its R8 target, 1 open - from rays instead of the depth's neighbours.
// The SSAO's own passes stay around it: the small depth before, and after it the blur over whole 4x4 tiles (two each
// way), multiplied into the shadow factor. So the samples follow the SSAO's tile: each texel of a 4x4 tile takes its own
// turn of one spiral of 16 x RT_AO_RAYS directions, and the blur averages the whole spiral.
//
// A ray goes into the hemisphere over the surface, cosine-weighted (a point of the unit disc, raised onto the sphere), as
// far as the SSAO's radius; the nearest thing it meets there counts by the SSAO's own curve, 1 - (t / R)^2 - so a crease
// darkens and a wall a radius away does not. The masked triangles let it through where they are clear, as the shadows'
// rays do. A lamp's own shade occludes the ceiling around it like anything else; only the player's own body is passed
// (RtKnobs.w) - in first person it is not drawn, and a darkening under feet nobody sees is only a stain. Its shadow from
// the lights is the shadows' business and stays.
#define RT_AO_RAYS 4u

// The nearest hit within TMax along D from O, or TMax where there is none.
float NearestHit(float3 O, float3 D, float TMax)
{
    RayDesc Ray;
    Ray.Origin    = O;
    Ray.Direction = D;
    Ray.TMin      = 0.0f;
    Ray.TMax      = TMax;
    RayQuery<RAY_FLAG_NONE> Q;
    Q.TraceRayInline(Scene, RAY_FLAG_NONE, (RtKnobs.z != 0u) ? RtKnobs.z : 0x03u, Ray);
    while (Q.Proceed())
    {
        if (Q.CandidateInstanceID() != 1u)
            Q.CommitNonOpaqueTriangleHit();
        else if (Q.CandidateGeometryIndex() != RtKnobs.w &&
                 OpaqueAt(Q.CandidateGeometryIndex(), Q.CandidatePrimitiveIndex(), Q.CandidateTriangleBarycentrics()))
            Q.CommitNonOpaqueTriangleHit();
    }
    return (Q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? Q.CommittedRayT() : TMax;
}

float AoPS(float4 Pos : SV_Position) : SV_Target
{
    FSurface Sf;
    // Left out - outside the world view, under what is drawn on top, the sky - the SSAO's own rule: open.
    if (FactorSurface(Pos, Sf) >= 0.0f)
        return 1.0f;
    float Radius = AoParams.x;
    float3 Sw = Eye + AxisX * Sf.S.x + AxisY * Sf.S.y + AxisZ * Sf.S.z;
    // Off the surface as the shadows' rays start, so that it does not occlude itself.
    float Push = (Sf.Kind == 1u) ? (1.0f + 0.004f * Sf.S.z) : (0.25f + 0.001f * Sf.S.z);
    float3 O = Sw + Sf.Nw * Push;
    float3 T, B;
    Basis(Sf.Nw, T, B);
    uint2 Q = uint2(Pos.xy);
    uint Tile = (Q.x & 3u) + ((Q.y & 3u) << 2);
    float Sum = 0.0f;
    [unroll] for (uint k = 0; k < RT_AO_RAYS; k++)
    {
        // The tile's spiral: sample n of its 16 x RT_AO_RAYS, evenly over the disc by area, a golden angle apart.
        float n = float(k * 16u + Tile);
        float U = (n + 0.5f) / float(16u * RT_AO_RAYS);
        float Phi = n * 2.39996323f;
        float R = sqrt(U);
        float3 D = T * (R * cos(Phi)) + B * (R * sin(Phi)) + Sf.Nw * sqrt(max(1.0f - U, 0.0f));
        float Hit = NearestHit(O, normalize(D), Radius) / Radius;
        Sum += 1.0f - Hit * Hit;
    }
    // RdxMath::AoFromSum's shape: 1 open, 1 - Strength shut in on every side.
    return saturate(1.0f - AoParams.y * Sum / float(RT_AO_RAYS));
}

#endif  // RDX_RT

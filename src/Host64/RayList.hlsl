// SecondRevisionHost64 - the rays' list (docs/terv-rt-host.md, Host64Protocol.h: RayPass_List).
//
// What the renderer's pixel shaders would trace, each written down as a request in a buffer (RenderLimits.h says how:
// RDX_RAY_*), and answered here in the request's own slot of the answers' buffer with what the ray met: which instance
// of the scene, which geometry of it, which triangle, where on the triangle, how far - or only whether it met anything,
// where that is all a request asks. No colour is made here - the textures what was met is drawn with live in the
// renderer's process, which shades the answer itself.
//
// What the renderer's own shader traces on to from what a ray met cannot be written down by a pixel - those rays
// start where this pass's answer says. They are traced here, where a request asks for them: the level's lamps' light
// at the place, each lamp's only where nothing stands between the two - the renderer's own text (RtLampLight.hlsli) on
// the renderer's own table of the lamps -, and the level's see-through surfaces the ray passed on its way, with the
// lamps' light on each. The renderer's shader reads these where its other build traces (SolidPass.hlsl:
// WaterReflectRtSolid, RtThinOver), and what is decided here by a surface's record is decided as it is there.
//
// The scene and the tables are FRtPass's, as the renderer's own passes read them (ShadowFactor.hlsl): the instances'
// masks, the geometries' table, the masked bodies' texture places and the masks. A body a ray asks about is met where
// its mask is solid, as the shadows' rays meet it.
//
// Built by dxc at build time (Host64.vcxproj): inline ray tracing is shader model 6.5.
#include "../RevisionDX12/RenderLimits.h"
#include "../RevisionDX12/RayListSlots.hlsli"

// This pass traces: it is neither of the renderer's two builds that leave the tracing to it.
#define RDX_RT_NO_QUERY 0
#define RDX_RT_HOST     0

RaytracingAccelerationStructure Scene : register(t6);

// The requests, a slot each; slot 0 holds how many slots the frame took.
ByteAddressBuffer   Requests : register(t0);
RWByteAddressBuffer Answers  : register(u0);
// The see-through surfaces the rays passed, a slot each as this pass takes them; slot 0 counts the slots taken.
RWByteAddressBuffer Passed   : register(u1);

// The scene's geometries (the bodies, in their list's order): a uint each - its first vertex among the texture places,
// bit 31 set for the arena's.
ByteAddressBuffer RtGeo : register(t10);
ByteAddressBuffer FrameUvw : register(t7);
ByteAddressBuffer ArenaUvw : register(t8);
Texture2DArray<float> Masks : register(t9);
SamplerState MaskSampler : register(s1);

// The renderer's own tables for what is traced on, copied over as they are: the mirror rays' table (FRtPass::
// PrepareWater - the level's numbers, a row a body, the level's lamps) and the level's surfaces (FRtPass::SetLevel -
// a uint a triangle, then a record a surface). Views with their lengths: what is read past an end reads as nought.
ByteAddressBuffer RtWater       : register(t11);
ByteAddressBuffer LevelMaterial : register(t12);
#include "../RevisionDX12/RtTables.hlsli"

cbuffer List : register(b0)
{
    uint4 ListKnobs;    // x: the slots the pass may read; y: the bodies' geometry that is the player's own body
                        // (none: 0xFFFFFFFF), which a request may ask to pass; z: how many bytes of the mirror
                        // rays' table the frame brought - nought: none, and nothing is traced on; w: the slots of
                        // the passed surfaces' buffer
};

// Whether the table's lamps lie within what the frame brought of the table: their loops run by the table's own
// counts, and a count that says more than came - a copy cut short - would run them over whatever lies there.
bool LampsHold()
{
    const uint  Bytes = ListKnobs.z;
    const uint4 Head  = RtWater.Load4(32u);     // the lamps' first byte, their count, the buckets', the lists'
    const uint4 More  = RtWater.Load4(48u);     // the large lamps' first byte, their count
    return Bytes >= RDX_RT_TABLE_HEAD && Head.x <= Bytes && Head.y <= (Bytes - Head.x) / RDX_RT_LAMP_BYTES &&
           Head.z <= Bytes && RDX_RT_LAMP_BUCKETS * 8u <= Bytes - Head.z && Head.w <= Bytes && More.x <= Bytes &&
           More.y <= (Bytes - More.x) / 4u;
}

// Where a ray meets a masked body's triangle (a leaf, a fence): is its texture solid there? Slot 0 is solid.
bool OpaqueAt(uint G, uint Prim, float2 Bary)
{
    const uint Info = RtGeo.Load(G * 4u);
    const uint At   = ((Info & 0x7FFFFFFFu) + Prim * 3u) * 12u;
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
    const float2 Uv = A.xy * (1.0f - Bary.x - Bary.y) + B.xy * Bary.x + C.xy * Bary.y;
    return Masks.SampleLevel(MaskSampler, float3(Uv, A.z), 0.0f) >= 0.5f;
}

// Whether a lamp's ray takes a triangle it has to ask about, as the renderer's own takes it: by the mask alone - the
// player's own body stands in a lamp's light like any other.
bool RtTakes(uint G, uint Prim, float2 Bary)
{
    return OpaqueAt(G, Prim, Bary);
}
// A lamp too faint to show in the pixel that asked is left out, by the request's step.
#define RDX_RT_LAMPS_LEAST 1
#include "../RevisionDX12/RtLampLight.hlsli"

// An answer's first four uints:
//   x  the distance along the ray, a float's bits; 0xFFFFFFFF: the ray met nothing
//   y  bits 30-31 the instance (0 the level, 1 the bodies, 2 the sky's walls, 3 the see-through surfaces), bit 29 set
//      where the triangle's front faces the ray, the low 24 bits the geometry
//   z  the triangle within the geometry
//   w  where on the triangle: the second and the third corner's weights, sixteen bits each (the second's the high ones)
uint4 Packed(float T, uint Instance, bool bFront, uint Geometry, uint Prim, float2 Bary)
{
    const float2 W = saturate(Bary);
    return uint4(asuint(T), (Instance << 30) | (bFront ? 0x20000000u : 0u) | (Geometry & 0x00FFFFFFu), Prim,
                 ((uint)(W.x * 65535.0f + 0.5f) << 16) | (uint)(W.y * 65535.0f + 0.5f));
}

// The lamps' light at what a mirror ray met, where the renderer's shader asks the lamps for it there (SolidPass.hlsl,
// WaterReflectRtSolid - the two have to decide alike): a surface of the level that does not carry its own light, with
// the bodies alone in the lamps' way where its light map is known - the map holds the level's own shadows - and the
// level and the bodies where it is not; a body that carries no light of its own and that the engine has not lit, with
// both in the way. By: what stood in the way was looked for among - nought where the place takes no lamp's light so.
float3 MetLight(RayDesc R, uint Instance, uint Geometry, uint Prim, float2 Bary, float T, out uint By)
{
    By = 0u;
    const float3 Ray  = R.Direction;
    const float3 Spot = R.Origin + Ray * T;
    if (Instance == 0u)
    {
        const uint  Surf = LevelMaterial.Load(Prim * 4u);
        const uint  At   = RtLevel.x + Surf * RDX_RT_RECORD_BYTES;
        const uint4 Rec  = (Surf < RtLevel.y) ? LevelMaterial.Load4(At) : uint4(0x808080u, 0xFFFFFFFFu, 0u, 0u);
        if ((Rec.x & 0x1000000u) != 0u)
            return 0.0f;
        float3 N = asfloat(LevelMaterial.Load3(At + RDX_RT_RECORD_NORMAL));
        N  = (dot(N, N) < 0.25f) ? -Ray : normalize(N);
        N  = (dot(N, Ray) > 0.0f) ? -N : N;
        By = (Rec.w != 0u) ? 0x02u : 0x03u;
        return RtLampLight(Spot, N, (Rec.x & 0x2000000u) != 0u, By);
    }
    if (Instance == 1u)
    {
        const uint Row  = RtGeoRow(Geometry);
        const uint Info = RtWater.Load(Row);
        const uint C    = RtWater.Load(Row + 4u);
        const uint At   = ((Info & 0x7FFFFFFFu) + Prim * 3u) * 12u;
        float3 VA, VB, VC;
        if ((Info >> 31) != 0u)
        {
            VA = asfloat(ArenaUvw.Load3(At));
            VB = asfloat(ArenaUvw.Load3(At + 12u));
            VC = asfloat(ArenaUvw.Load3(At + 24u));
        }
        else
        {
            VA = asfloat(FrameUvw.Load3(At));
            VB = asfloat(FrameUvw.Load3(At + 12u));
            VC = asfloat(FrameUvw.Load3(At + 24u));
        }
        // The face's own light's bit rides in the first corner's slot, beside the face's key.
        const uint Face = (uint)(frac(VA.z) * 64.0f + 0.5f);
        if ((C & 0x1000000u) != 0u || (Face & 1u) != 0u || (C & 0x4000000u) != 0u)
            return 0.0f;
        const float W0 = 1.0f - Bary.x - Bary.y;
        float3 N = normalize(RtCasterNormal(VA) * W0 + RtCasterNormal(VB) * Bary.x + RtCasterNormal(VC) * Bary.y);
        N  = (dot(N, Ray) > 0.3f) ? -N : N;
        By = 0x03u;
        return RtLampLight(Spot, N, (C & 0x2000000u) != 0u, By);
    }
    return 0.0f;
}

// The level's see-through surfaces a mirror ray passes before what it met (Reach units off), the nearest first, as
// the renderer's shader looks for them (SolidPass.hlsl, RtThinOver): each a ray of its own from just past the one
// before, RDX_RT_THIN_LAYERS of them at most. With the lamps' light on each that the level draws lit - not on one
// that only scales what is behind it, that carries its own light, or whose texture the renderer has not got yet.
// Whether a surface has a hole where the ray passes is the renderer's to say, by its texture: it reads on to the next.
// They go into slots of the passed surfaces' buffer, taken together; First: the first of them. What comes back is
// how many - none where the buffer has no room left for them, and its count then tells the renderer so.
uint PassedOn(RayDesc R, float Reach, out uint First)
{
    uint4 SlotA[RDX_RT_THIN_LAYERS];
    uint4 SlotB[RDX_RT_THIN_LAYERS];
    uint  Count = 0u;
    float From  = R.TMin;
    First = 0u;
    [loop] for (uint Layer = 0u; Layer < RDX_RT_THIN_LAYERS && From < Reach; Layer++)
    {
        RayDesc S;
        S.Origin    = R.Origin;
        S.Direction = R.Direction;
        S.TMin      = From;
        S.TMax      = Reach;
        RayQuery<RAY_FLAG_NONE> Q;
        Q.TraceRayInline(Scene, RAY_FLAG_NONE, 0x08u, S);
        while (Q.Proceed())
        {
        }
        if (Q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            break;
        const float T = Q.CommittedRayT();
        From = T + 0.5f;
        float3 Lit = 0.0f;
        uint   By  = 0u;
        const uint Surf = LevelMaterial.Load((RtLevel.w + Q.CommittedPrimitiveIndex()) * 4u);
        if (Surf < RtLevel.y)
        {
            const uint  At      = RtLevel.x + Surf * RDX_RT_RECORD_BYTES;
            const uint4 Rec     = LevelMaterial.Load4(At);
            const uint  Kind    = LevelMaterial.Load(At + RDX_RT_RECORD_KIND);
            const bool  bScales = (Kind & RDX_RT_KIND_MODULATED) != 0u && (Kind & RDX_RT_KIND_TRANSLUCENT) == 0u;
            if (Rec.z != 0u && !bScales && (Rec.x & 0x1000000u) == 0u && (Kind & RDX_RT_KIND_UNLIT) == 0u)
            {
                float3 N = asfloat(LevelMaterial.Load3(At + RDX_RT_RECORD_NORMAL));
                N   = (dot(N, N) < 0.25f) ? -R.Direction : normalize(N);
                N   = (dot(N, R.Direction) > 0.0f) ? -N : N;
                By  = 0x03u;
                Lit = RtLampLight(R.Origin + R.Direction * T, N, (Rec.x & 0x2000000u) != 0u, By);
            }
        }
        SlotA[Count] = uint4(asuint(T), Q.CommittedPrimitiveIndex(), asuint(Lit.x), asuint(Lit.y));
        SlotB[Count] = uint4(asuint(Lit.z), By, 0u, 0u);
        Count++;
    }
    if (Count == 0u)
        return 0u;
    Passed.InterlockedAdd(0u, Count, First);
    if (First + Count > ListKnobs.w)
        return 0u;
    [loop] for (uint i = 0u; i < Count; i++)
    {
        Passed.Store4((First + i) * RDX_RAY_SLOT_BYTES, SlotA[i]);
        Passed.Store4((First + i) * RDX_RAY_SLOT_BYTES + 16u, SlotB[i]);
    }
    return Count;
}

// What a ray meets first of the level, the bodies and the sky's walls with the player's own body out of its way, as
// the renderer's shader looks where it traces (SolidPass.hlsl, RtReach): how far off - To where it meets nothing,
// and a sky wall is nothing.
float ReachPast(float3 Origin, float3 Way, float From, float To)
{
    RayDesc E;
    E.Origin    = Origin;
    E.Direction = Way;
    E.TMin      = From;
    E.TMax      = To;
    RayQuery<RAY_FLAG_NONE> Q;
    Q.TraceRayInline(Scene, RAY_FLAG_NONE, 0x07u, E);
    while (Q.Proceed())
    {
        if (Q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            const uint G = Q.CandidateGeometryIndex();
            if (G != ListKnobs.y && OpaqueAt(G, Q.CandidatePrimitiveIndex(), Q.CandidateTriangleBarycentrics()))
                Q.CommitNonOpaqueTriangleHit();
        }
    }
    return (Q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && Q.CommittedInstanceID() != 2u) ? Q.CommittedRayT() : To;
}

// Whether the eye sees a place (RDX_RAY_FLAG_SEEN): nothing between the two, from two units off the eye to two
// short of the place - the look the renderer's shader takes before it traces on from a pane (PSPaneKeep).
bool EyeSees(float3 Place)
{
    const float3 To  = Place - RtEye.xyz;
    const float  Far = length(To);
    if (!(Far > 4.0f))
        return true;
    return !(ReachPast(RtEye.xyz, To / Far, 2.0f, Far - 2.0f) < Far - 2.0f);
}

// Whether the lamp at the eye is in sight of a ray's start along the ray (RDX_RAY_FLAG_EYE), as the renderer's
// shader finds it where it traces (SolidPass.hlsl, GlassMirror): a ray the same way from the same place, as far as
// where it passes the eye, the player's own body out of its way - in sight where that ray meets nothing up to half
// a unit short of there. That ray meets what the request's own ray met (bMet, T, and what) - unless that was the
// player's own body, which is in the request's way and not in that ray's: then it is traced.
bool EyeInSight(RayDesc R, bool bMet, float T, uint Instance, uint Geometry)
{
    const float3 Sp   = R.Origin - RtEye.xyz;
    const float  Near = -dot(Sp, R.Direction);
    if (!(Near > 1.0f))
        return false;
    if (!bMet || T > Near)
        return true;
    if (Instance != 1u || Geometry != ListKnobs.y)
        return Instance == 2u || T >= Near - 0.5f;
    return ReachPast(R.Origin, R.Direction, R.TMin, Near) >= Near - 0.5f;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 Id : SV_DispatchThreadID)
{
    // Slot 0 is the count itself; what lies past the slots the frame took is another frame's.
    const uint Slot  = Id.x;
    const uint Taken = min(Requests.Load(0u), ListKnobs.x);
    if (Slot == 0u || Slot >= Taken)
        return;
    const uint  At   = Slot * RDX_RAY_SLOT_BYTES;
    const uint4 A    = Requests.Load4(At);
    const uint4 B    = Requests.Load4(At + 16u);
    const uint  Kind = RayWordKind(B.w);
    // A slot taken and never written - its pixel was thrown away after it took it: nobody reads its answer.
    if (Kind == RDX_RAY_KIND_NONE)
        return;
    RayDesc R;
    R.Origin    = asfloat(A.xyz);
    R.Direction = RayUnpackWay(B.x);
    R.TMin      = RayWordFrom(B.w);
    R.TMax      = asfloat(A.w);
    const uint Asked  = RayWordMask(B.w);
    const uint Mask   = (Asked != 0u) ? Asked : 0x03u;
    const uint Passes = ((B.w & RDX_RAY_FLAG_NO_BODY) != 0u) ? ListKnobs.y : 0xFFFFFFFFu;
    GRtLampLeast = RayLeast(RayWordLeast(B.w));
    uint4 Out  = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
    uint4 More = uint4(0u, 0u, 0u, 0u);
    // A ray with nowhere to go meets nothing; one whose numbers are no numbers is not traced at all. Nor is one
    // asked for only where the eye sees its start, where the eye does not - the answer says so; and where the
    // frame brought no table to know the eye's place by, it is taken for not seen.
    const bool bSound = R.TMax > R.TMin && all(abs(R.Origin) < 1e9f) && dot(R.Direction, R.Direction) > 0.25f;
    bool bSeen = true;
    if ((B.w & RDX_RAY_FLAG_SEEN) != 0u)
    {
        bSeen = false;
        if (bSound && ListKnobs.z != 0u)
            bSeen = EyeSees(R.Origin);
    }
    if (!bSeen)
        Out = uint4(0xFFFFFFFFu, 1u, 0u, 0u);
    else if (bSound)
    {
        if (Kind == RDX_RAY_KIND_ANY)
        {
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> Q;
            Q.TraceRayInline(Scene, RAY_FLAG_NONE, Mask, R);
            while (Q.Proceed())
            {
                if (Q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
                {
                    const uint G = Q.CandidateGeometryIndex();
                    if (G != Passes && OpaqueAt(G, Q.CandidatePrimitiveIndex(), Q.CandidateTriangleBarycentrics()))
                        Q.CommitNonOpaqueTriangleHit();
                }
            }
            if (Q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
                Out = Packed(Q.CommittedRayT(), Q.CommittedInstanceID(), Q.CommittedTriangleFrontFace(),
                             Q.CommittedGeometryIndex(), Q.CommittedPrimitiveIndex(),
                             Q.CommittedTriangleBarycentrics());
        }
        else
        {
            RayQuery<RAY_FLAG_NONE> Q;
            Q.TraceRayInline(Scene, RAY_FLAG_NONE, Mask, R);
            while (Q.Proceed())
            {
                if (Q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
                {
                    const uint G = Q.CandidateGeometryIndex();
                    if (G != Passes && OpaqueAt(G, Q.CandidatePrimitiveIndex(), Q.CandidateTriangleBarycentrics()))
                        Q.CommitNonOpaqueTriangleHit();
                }
            }
            const bool bMet = Q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
            if (bMet)
                Out = Packed(Q.CommittedRayT(), Q.CommittedInstanceID(), Q.CommittedTriangleFrontFace(),
                             Q.CommittedGeometryIndex(), Q.CommittedPrimitiveIndex(),
                             Q.CommittedTriangleBarycentrics());
            // Traced on from there, where the request asks for it and the frame brought the tables it goes by.
            if (ListKnobs.z != 0u)
            {
                float3 Lit   = 0.0f;
                uint   By    = 0u;
                uint   Count = 0u;
                uint   First = 0u;
                bool   bEye  = false;
                if (LampsHold())
                {
                    if (bMet && (B.w & RDX_RAY_FLAG_LAMPS) != 0u)
                        Lit = MetLight(R, Q.CommittedInstanceID(), Q.CommittedGeometryIndex(),
                                       Q.CommittedPrimitiveIndex(), Q.CommittedTriangleBarycentrics(),
                                       Q.CommittedRayT(), By);
                    if ((B.w & RDX_RAY_FLAG_THIN) != 0u && ListKnobs.w > 1u)
                        Count = PassedOn(R, bMet ? Q.CommittedRayT() : R.TMax, First);
                }
                if ((B.w & RDX_RAY_FLAG_EYE) != 0u)
                    bEye = EyeInSight(R, bMet, Q.CommittedRayT(), Q.CommittedInstanceID(),
                                      Q.CommittedGeometryIndex());
                More = uint4(asuint(Lit), RayPackMore(By, Count, First, bEye));
            }
        }
    }
    Answers.Store4(At, Out);
    // What was traced on to is read only where it was asked for.
    if ((B.w & (RDX_RAY_FLAG_LAMPS | RDX_RAY_FLAG_THIN | RDX_RAY_FLAG_EYE)) != 0u)
        Answers.Store4(At + 16u, More);
}

// SecondRevision - the level's lamps' light at a place a mirror ray met, each lamp's only where nothing stands between
// the two. Included by the renderer's own shader (SolidPass.hlsl) and by the 64-bit host's pass that traces on from
// what a ray met (src/Host64/RayList.hlsl): one text, so the light is the same whoever traces.
// Whoever includes this has declared the table (RtWater, with RtTables.hlsli), and - where rays are traced here - the
// scene and RtTakes, which says whether a ray takes a triangle it has to ask about.

// A glint's ray stops this short of its lamp: a lamp's own shade around it must not hide it.
#define RT_GLINT_STOP 24.0f

// A caster's vertex's normal, out of its texture place's fractions (RdxMath::UnpackOctNormal).
float3 RtCasterNormal(float3 V)
{
    const float2 Q = floor(frac(V.xy * 4096.0f) * RDX_RT_NORMAL_STEPS + 0.5f) / (RDX_RT_NORMAL_STEPS - 1.0f) * 2.0f -
                     1.0f;
    float3 N = float3(Q, 1.0f - abs(Q.x) - abs(Q.y));
    if (N.z < 0.0f)
        N.xy = (1.0f - abs(N.yx)) * float2((Q.x >= 0.0f) ? 1.0f : -1.0f, (Q.y >= 0.0f) ? 1.0f : -1.0f);
    return normalize(N);
}

#if RDX_RT_LAMPS_LEAST
// Below this much of full light at the spot a lamp is left out, where whoever includes this goes by a request's
// step - how faint a light still shows in the pixel that asked (RayListSlots.hlsli, RayLeast).
static float GRtLampLeast = 0.004f;
#endif

// The light a spot gets from the level's lamps (FRtPass::SetLamps), worked out at the spot: every lamp that reaches it,
// by how far off the lamp is and how squarely the spot's face looks at it - and a spot's cone - and only where
// nothing stands between the two: a ray to the lamp, against the level and the actors alike, so that whatever casts
// a shadow in the picture casts it here. It stops short of the lamp, whose own shade must not hide it. N: the face's
// normal, towards the ray. Mask: what may stand in the way - the level and the actors (3), the actors alone (2), or
// nothing at all, and no ray is traced (0). In the engine's own measure; the caller brings it into the picture's.
float3 RtLampLight(float3 Spot, float3 N, bool bSpecial, uint Mask)
{
#if RDX_RT_HOST
    // The host's to trace: it worked the light out where its ray met the place (GRtLit), against what GRtLitBy
    // names. Asked against anything else - the host had not the level's surfaces, or read one differently - the
    // lamps' light below, with nothing in its way.
    if (Mask != 0u && GRtLitBy == Mask)
        return GRtLit;
#endif
    const uint4 Head = RtWater.Load4(32u);     // the lamps' first byte, their count, the buckets', the lists'
    const uint4 More = RtWater.Load4(48u);     // the large lamps' first byte, their count, the cell, the gain
    if (Head.y == 0u)
        return 0.0f;
    const int3  Cell   = (int3)floor(Spot / asfloat(More.z));
    const uint  Bucket = ((uint)Cell.x * 73856093u ^ (uint)Cell.y * 19349663u ^ (uint)Cell.z * 83492791u) &
                         (RDX_RT_LAMP_BUCKETS - 1u);
    const uint2 List   = RtWater.Load2(Head.z + Bucket * 8u);
    // Never past the table's own lamps, whatever a list's length says: a list holds a lamp once.
    const uint  Count  = min(List.y + More.y, Head.y * 2u);
    float3 Sum = 0.0f;
    [loop] for (uint i = 0u; i < Count; i++)
    {
        const uint   Id = (i < List.y) ? RtWater.Load(Head.w + (List.x + i) * 4u)
                                       : RtWater.Load(More.x + (i - List.y) * 4u);
        const uint   At = Head.x + Id * RDX_RT_LAMP_BYTES;
        const float4 Place = asfloat(RtWater.Load4(At));           // where it is, and its reach
        const float3 D     = Place.xyz - Spot;
        const float  DistSq = dot(D, D);
        if (DistSq >= Place.w * Place.w)
            continue;
        const float4 Colour = asfloat(RtWater.Load4(At + 16u));    // its colour, and a spot's cone's cosine
        const uint4  Aim    = RtWater.Load4(At + 32u);             // where a spot points, and the lamp's flags
        if (((Aim.w & 1u) != 0u) != bSpecial)
            continue;
        const float  Dist = sqrt(DistSq);
        const float3 To   = D / max(Dist, 1e-3f);
        const float  Fall = 1.0f - DistSq / (Place.w * Place.w);
        float K = saturate(dot(N, To)) * Fall * Fall;
        if (Colour.w >= -1.0f)
            K *= saturate((-dot(To, asfloat(Aim.xyz)) - Colour.w) / max(1.0f - Colour.w, 0.001f));
#if RDX_RT_LAMPS_LEAST
        if (K * max(Colour.r, max(Colour.g, Colour.b)) < GRtLampLeast)
            continue;
#else
        if (K * max(Colour.r, max(Colour.g, Colour.b)) < 0.004f)
            continue;
#endif
#if !RDX_RT_NO_QUERY
        if (Mask != 0u && Dist > RT_GLINT_STOP + 4.0f)
        {
            RayDesc R;
            R.Origin    = Spot + N;
            R.Direction = To;
            R.TMin      = 1.0f;
            R.TMax      = Dist - RT_GLINT_STOP;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> Q;
            Q.TraceRayInline(Scene, RAY_FLAG_NONE, Mask, R);
            while (Q.Proceed())
            {
                if (Q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
                    RtTakes(Q.CandidateGeometryIndex(), Q.CandidatePrimitiveIndex(), Q.CandidateTriangleBarycentrics()))
                    Q.CommitNonOpaqueTriangleHit();
            }
            if (Q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
                continue;
        }
#endif
        Sum += Colour.rgb * K;
    }
    return Sum;
}

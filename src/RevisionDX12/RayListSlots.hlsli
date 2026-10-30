// SecondRevision - the rays' list: what both ends of it read a request's slot by (RenderLimits.h says what a slot
// holds). Included by the renderer's builds that write the requests down or read their answers (SolidPass.hlsl) and by
// the 64-bit host's pass that answers them (src/Host64/RayList.hlsl): one text, so the two cannot differ.

// A ray's way in one uint: the direction folded onto a square, sixteen bits each way. What comes back is the
// direction to about a hundred-thousandth of its length - a hair's breadth at any distance a level has.
uint RayPackWay(float3 Way)
{
    const float3 D = Way / max(abs(Way.x) + abs(Way.y) + abs(Way.z), 1e-20f);
    const float2 S = float2((D.x >= 0.0f) ? 1.0f : -1.0f, (D.y >= 0.0f) ? 1.0f : -1.0f);
    const float2 E = (D.z >= 0.0f) ? D.xy : (1.0f - abs(D.yx)) * S;
    const uint2  Q = (uint2)(saturate(E * 0.5f + 0.5f) * 65535.0f + 0.5f);
    return (Q.x << 16) | Q.y;
}

float3 RayUnpackWay(uint Packed)
{
    const float2 E = float2((float)(Packed >> 16), (float)(Packed & 0xFFFFu)) / 65535.0f * 2.0f - 1.0f;
    float3 D = float3(E, 1.0f - abs(E.x) - abs(E.y));
    if (D.z < 0.0f)
        D.xy = (1.0f - abs(D.yx)) * float2((E.x >= 0.0f) ? 1.0f : -1.0f, (E.y >= 0.0f) ? 1.0f : -1.0f);
    return normalize(D);
}

// How alike a pixel's first ray must be to another's for the other's answer to serve it, where a few pixels' rays are
// traced for their neighbours (Reflections=RTHalf, SolidPass.hlsl's RtAskLed; RTLow, RaySparse.hlsl): the cosine
// between their ways at least, and their starts no further apart than this share of their distance from the eye and a
// unit.
#define RAY_ALIKE_COS 0.99f
#define RAY_ALIKE_FAR 0.03f
// ... and the sort of a pixel's block that tells which blocks' answers may serve each other there (RTLow: SolidPass.hlsl's
// RtAskFlushLow works it out, RaySparse.hlsl compares by it): its length, then its words in their order as the list
// holds them, a step each.
uint RaySortStep(uint Sig, uint Word) { return Sig * 0x9E3779B1u + Word; }
// ... and where in the own list a block the pass kept says where its requests went in the host's list - a byte
// address: beside its second request, or, where it has one request, in its first one's fourth uint (RaySparse.hlsl).
uint RaySparseAnswerAt(uint At, uint Count)
{
    return (Count > 1u) ? (At + 1u) * RDX_RAY_SLOT_BYTES + 24u : At * RDX_RAY_SLOT_BYTES + 12u;
}

// The lamps' light on what a mirror ray meets is worked out lamp by lamp, each with a ray of its own to see whether
// anything stands in its way - and a lamp whose light there would not show in the pixel is left out: one that
// gives less than RT_LAMP_LEAST of full light where the pixel shows all of the mirror image, and as many times
// more as the pixel shows less of the image (a pane of glass seen face on gives back a twelfth of it). A request
// carries that as a step - RayLeastFor of the image's share in the pixel - and RayLeast is the light a step stands
// for.
#define RT_LAMP_LEAST 0.004f
uint  RayLeastFor(float Share) { return (uint)clamp(1.0f / max(Share, 0.0625f) - 1.0f, 0.0f, 15.0f); }
float RayLeast(uint Step)      { return RT_LAMP_LEAST * (float)(1u + min(Step, 15u)); }

// A request's packed word: its kind, the instances its ray meets, its flags (RDX_RAY_FLAG_*), how far from its
// start the ray begins to look, and the lamps' step (RayLeastFor). A block's first request has the block's length
// put in beside them.
uint RayPackWord(uint Kind, uint Mask, uint Flags, float From, uint Least)
{
    return Kind | ((Mask & 0xFFu) << RDX_RAY_MASK_SHIFT) | Flags |
           (min((uint)(max(From, 0.0f) * 16.0f + 0.5f), RDX_RAY_FROM_MASK) << RDX_RAY_FROM_SHIFT) |
           (min(Least, 15u) << RDX_RAY_LEAST_SHIFT);
}
uint  RayWordKind(uint Word)  { return Word & 3u; }
uint  RayWordMask(uint Word)  { return (Word >> RDX_RAY_MASK_SHIFT) & 0xFFu; }
uint  RayWordCount(uint Word) { return (Word >> RDX_RAY_COUNT_SHIFT) & RDX_RAY_COUNT_MASK; }
float RayWordFrom(uint Word)  { return (float)((Word >> RDX_RAY_FROM_SHIFT) & RDX_RAY_FROM_MASK) / 16.0f; }
uint  RayWordLeast(uint Word) { return Word >> RDX_RAY_LEAST_SHIFT; }

// An answer's last uint, where the host traced on from what the ray met: what stood in the way of the lamps' light
// there was looked for among (Lit - 0: no light was worked out), how many see-through surfaces the ray passed, the
// first of them in the passed surfaces' buffer, and whether the lamp at the eye is in sight of the ray's start.
uint RayPackMore(uint Lit, uint Passed, uint First, bool bEye)
{
    return (Lit & 3u) | (min(Passed, 15u) << RDX_RAY_MORE_PASSED_SHIFT) |
           ((First & RDX_RAY_MORE_FIRST_MASK) << RDX_RAY_MORE_FIRST_SHIFT) | (bEye ? RDX_RAY_MORE_EYE : 0u);
}
uint RayMoreLit(uint More)    { return More & 3u; }
uint RayMorePassed(uint More) { return (More >> RDX_RAY_MORE_PASSED_SHIFT) & 15u; }
uint RayMoreFirst(uint More)  { return (More >> RDX_RAY_MORE_FIRST_SHIFT) & RDX_RAY_MORE_FIRST_MASK; }
bool RayMoreEye(uint More)    { return (More & RDX_RAY_MORE_EYE) != 0u; }

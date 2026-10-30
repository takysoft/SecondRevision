// SecondRevision - Reflections=RTLow: of the rays a round's pixels wrote down, the ones the 64-bit host traces - about
// one of each three by three pixels (FRaySparse; docs/terv-nedves-feluletek.md 7.2). Run after the pixels wrote all
// they ask into the own list (SolidPass.hlsl, RtAskFlush at RtEye.w 2), before the host takes its list.
//
// The pixels ask as at RTHalf - of each two by two the first that writes asks for the alike others (RtAskLed) - and
// lie in tiles of three by three, on a grid that stays where it is. A tile's lender is the first pixel that asks of
// its middle's two by two - or, where none does, the first of the tile's pixels that does, row by row. Its blocks are
// kept; a block of another pixel of the tile whose requests are the same sort as one of the lender's - the same words,
// in the same order - and whose first ray starts at about the same place and goes about the same way borrows that
// one, and the rest are kept too. A kept block's requests go one after another into the host's list, which the host
// traces as it traces any, and the block says where they went in the own list (RaySparseAnswerAt; 0: the host's
// list had no room); a block that borrows names in its first uint the lender's block it borrows, and its first
// word's kind is none. The build that reads the answers goes by these (SolidPass.hlsl, RtFind).
//
// A thread a pixel: a pixel's blocks are its own thread's to mark, and of another pixel's it reads only the lender's
// first request - its place, its sort, its way, its word - which nobody writes.
//
// Compiled by dxc at build time (RevisionDX12.vcxproj), and by scripts/check_shaders.py and tests/RayThinCheck too.
// Plain ASCII.

#include "RenderLimits.h"
#include "RayListSlots.hlsli"

// The pixels' blocks, the last written first - a pixel's cell holds the first slot of one (0: none), and a block's
// first request the first slot of the one written at that pixel before it (SolidPass.hlsl, RtHeads) - in the own
// list (RenderLimits.h, RDX_RAY_*); and the host's list, emptied, which the kept requests go into.
RWTexture2D<uint>   Heads  : register(u0);
RWByteAddressBuffer Asked  : register(u1);
RWByteAddressBuffer Traced : register(u2);

cbuffer Sparse : register(b0)
{
    uint2  Size;        // the heads' size: the pixels'
    uint2  Unused;
    float3 Eye;         // the camera, in the level's space (SolidPass.hlsl's RtEye)
    float  Unused2;
};

// Room for Count requests in the host's list - a wave's lanes that take room at once with one atomic: the first slot
// of this lane's.
uint TakeTraced(uint Count)
{
#if __SHADER_TARGET_MAJOR >= 6
    const uint Before = WavePrefixSum(Count);
    const uint All    = WaveActiveSum(Count);
    uint Base = 0u;
    if (WaveIsFirstLane())
        Traced.InterlockedAdd(0u, All, Base);
    return WaveReadLaneFirst(Base) + Before;
#else
    uint First;
    Traced.InterlockedAdd(0u, Count, First);
    return First;
#endif
}

// A block's sort as its pixel worked it out (RaySortStep): in its first request's fourth uint (A.w) - or of its one
// word, where it has one request (SolidPass.hlsl, RtAskFlushLow).
uint SortOf(uint4 A, uint Count, uint Word)
{
    return (Count > 1u) ? A.w : RaySortStep(1u, Word);
}

[numthreads(8, 8, 1)]
void SparseCS(uint3 Id : SV_DispatchThreadID)
{
    const uint2 P = Id.xy;
    // A round that asked more than the own list holds is given up whole (RtFind): nothing of it is read.
    const uint2 Head = Asked.Load2(0u);
    if (P.x >= Size.x || P.y >= Size.y || Head.x > Head.y)
        return;
    uint At = Heads[P];
    if (At == 0u)
        return;
    // The tile's lender: the first pixel that asks of its middle's two by two - which lies within the tile, and whose
    // first asking pixel asked for the alike others of the four (SolidPass.hlsl, RtAskLed) -, or else the first of the
    // tile's pixels that asks, row by row.
    const uint2 Base   = P / 3u * 3u;
    const uint2 Four   = (Base + 1u) & ~1u;
    uint2       Lender = P;
    bool        bFound = false;
    [unroll] for (uint q = 0u; q < 4u; q++)
    {
        const uint2 Q = Four + uint2(q & 1u, q >> 1u);
        if (!bFound && Q.x < Size.x && Q.y < Size.y && Heads[Q] != 0u)
        {
            Lender = Q;
            bFound = true;
        }
    }
    [loop] for (uint k = 0u; k < 9u && !bFound; k++)
    {
        const uint2 Q = Base + uint2(k % 3u, k / 3u);
        if (Q.x < Size.x && Q.y < Size.y && Heads[Q] != 0u)
        {
            Lender = Q;
            bFound = true;
        }
    }
    const bool bLender = all(Lender == P);
    const uint LenderAt = Heads[Lender];
    const uint Room = Traced.Load(4u);
    [loop] for (uint n = 0u; n < RDX_RAY_CHAIN_MOST && At != 0u && At < Head.x; n++)
    {
        const uint4 A     = Asked.Load4(At * RDX_RAY_SLOT_BYTES);
        const uint4 B     = Asked.Load4(At * RDX_RAY_SLOT_BYTES + 16u);
        const uint  Count = RayWordCount(B.w);
        if (Count != 0u && Count <= Head.x - At)
        {
            uint Lent = 0u;
            if (!bLender)
            {
                const uint   Sig   = SortOf(A, Count, B.w);
                const float3 From  = asfloat(A.xyz);
                const float3 Way   = RayUnpackWay(B.x);
                const float  Reach = RAY_ALIKE_FAR * length(From - Eye) + 1.0f;
                uint L = LenderAt;
                [loop] for (uint m = 0u; m < RDX_RAY_CHAIN_MOST && L != 0u && L < Head.x && Lent == 0u; m++)
                {
                    const uint4 LA     = Asked.Load4(L * RDX_RAY_SLOT_BYTES);
                    const uint4 LB     = Asked.Load4(L * RDX_RAY_SLOT_BYTES + 16u);
                    const uint  LCount = RayWordCount(LB.w);
                    if (LCount != 0u && LCount <= Head.x - L && SortOf(LA, LCount, LB.w) == Sig)
                    {
                        const float3 Gap = From - asfloat(LA.xyz);
                        if (dot(Gap, Gap) < Reach * Reach && dot(Way, RayUnpackWay(LB.x)) > RAY_ALIKE_COS)
                            Lent = L;
                    }
                    L = LB.z;
                }
            }
            if (Lent != 0u)
            {
                Asked.Store(At * RDX_RAY_SLOT_BYTES + 28u, B.w & ~3u);
                Asked.Store(At * RDX_RAY_SLOT_BYTES, Lent);
            }
            else
            {
                // Kept: its requests into the host's list - none where that has no room left -, the first one's
                // reach back in its place where the sort stood.
                const uint First = TakeTraced(Count);
                const bool bRoom = First + Count <= Room;
                if (bRoom)
                {
                    [loop] for (uint i = 0u; i < Count; i++)
                    {
                        uint4       RA = Asked.Load4((At + i) * RDX_RAY_SLOT_BYTES);
                        const uint4 RB = Asked.Load4((At + i) * RDX_RAY_SLOT_BYTES + 16u);
                        if (i == 0u && Count > 1u)
                            RA.w = Asked.Load((At + 1u) * RDX_RAY_SLOT_BYTES + 20u);
                        Traced.Store4((First + i) * RDX_RAY_SLOT_BYTES, RA);
                        Traced.Store4((First + i) * RDX_RAY_SLOT_BYTES + 16u, RB);
                    }
                }
                Asked.Store(RaySparseAnswerAt(At, Count), bRoom ? First : 0u);
            }
        }
        At = B.z;
    }
}

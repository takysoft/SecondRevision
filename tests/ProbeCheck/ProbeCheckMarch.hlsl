// ProbeCheck's own entry point beside MirrorProbe.hlsl's: the march of the probe's shared block - the one the mirror
// layers run (SolidPass.hlsl holds the same block) - on rays the check hands over, against the pyramid the passes made.
// A ray, 48 bytes: where it starts from the probe's centre and how far it may go; its way; and what the march says -
// how far along it crossed, and 1 where it did.
// Plain ASCII, as the renderer's own shader files.

#include "../../src/RevisionDX12/MirrorProbe.hlsl"

RWByteAddressBuffer Rays : register(u5);

[numthreads(64, 1, 1)]
void MarchCS(uint3 Id : SV_DispatchThreadID)
{
    if (Id.x >= Level)          // the check's ray count rides in the passes' level
        return;
    const uint   B   = Id.x * 48u;
    const float4 At  = asfloat(Rays.Load4(B));
    const float4 Way = asfloat(Rays.Load4(B + 16u));
    float      T;
    const bool bCrossed = ProbeMarch(At.xyz, Way.xyz, At.w, T);
    Rays.Store4(B + 32u, uint4(asuint(T), bCrossed ? 1u : 0u, 0u, 0u));
}

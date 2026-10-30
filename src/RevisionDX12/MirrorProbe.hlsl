// SecondRevision - the probe gathered from the picture (FMirrorProbe; docs/tukrozodes-szonda-terv.md): a cube of the
// level round a point, filled each frame from what the world's view shows of it, for the mirror layers to read where
// the picture cannot give what a mirror ray meets - behind the eye, off the picture's edge. Compiled at run time
// (cs_5_0) from the text the build puts into the DLL (MirrorProbeText.h, scripts/embed-text.ps1).
// Plain ASCII: the build refuses anything else (embed-text.ps1).
//
// Each frame, after the water's copies of the picture and the depth (FBackend::CopyForWater), in this order:
//   (StatsClearCS - a counting frame's numbers emptied first.)
//   (ClearSpareCS, TransferKeyCS, TransferGatherCS, TransferBackCS - where the probe moved: what it holds, taken over
//              round its new centre.)
//   CarveCS  - a thread a cell: a point the eye now sees past is gone (RdxMath::ProbeCarved); the frame's keys emptied.
//   KeyCS    - a thread a pixel, one of each two by two (Pick, another each frame): the nearest point of the level
//              each cell is shown this frame, as its distance's bits.
//   GatherCS - the same pixels again: the one whose point is that nearest takes the cell by RdxMath::ProbeGather's rule.
//   BaseCS   - the pyramid's finest level from the cells; UpCS - each coarser one from the four under it
//              (RdxMath::ProbeMipOf); DownCS - from RDX_PROBE_FILL_UP levels up down to the finest, an entry that knows
//              nothing takes the one above it, so a hole takes the nearest level above it that knows something.
// The mirror layers read the finest level (SolidPass.hlsl): the march below, a read a step.
//
// The cells (Cells, 32 bytes each, by face, row and column): the point from the probe's centre (three floats) and its
// distance (0: nothing known); its colour (three halves); the frame it was last seen in. The keys (Keys): a uint a cell.
// The pyramid (Levels, 16 bytes an entry, the finest level first, each by face, row and column): colour and trust
// (four halves), the distance, a spare.

Texture2D        Picture : register(t0);      // the water's copy of the picture
Texture2D<float> Depth   : register(t1);      // and of the depth: the view depth's reciprocal, 0 where nothing was drawn
Texture2D<uint2> Kinds   : register(t2);      // the depth's stencil: what kind of surface each pixel is (FSolidPass::SetKind)
// The shadows' added target (FBackend::ApplyShadows): what the see-through draws laid over each pixel's surface so far -
// smoke, a lamp's glare, a pane - and the flashlight's light on it. None of that belongs to the surface: it is seen from
// here alone, and would be mirrored where it is not.
Texture2D        Added   : register(t3);
RWByteAddressBuffer Cells  : register(u0);
RWByteAddressBuffer Keys   : register(u1);
RWByteAddressBuffer Levels : register(u2);
RWByteAddressBuffer Spare  : register(u3);      // cells as they stand round the probe's new centre, as it moves
// What a counting frame's passes did (Count): the cells carved out, the cells the picture's pixels took, the cells
// known after the frame, the cells taken over as the probe moved - four uints, emptied by StatsClearCS.
RWByteAddressBuffer Stats  : register(u4);

cbuffer ProbeConsts : register(b0)
{
    float3 AxisX;  float TanX;      // the world view's axes in the world, and its half-width's and half-height's tangents
    float3 AxisY;  float TanY;
    float3 AxisZ;  float Reach;     // how far from the probe's centre a point is kept
    float3 Centre; float Spare0;    // the probe's centre from the camera, along the world's axes
    float4 Rect;                    // the world view's place in the picture, 0..1: x, y, width, height
    float4 Rules;                   // x, y: RdxMath::ProbeCarveSlack and ProbeCarveShare; z: ProbeSameShare
    uint   Frame;                   // counts the frames, for each cell's last sight
    uint   Pick;                    // which pixel of each two by two gathers: 0..3, x the low bit
    uint   Level;                   // the pyramid's level UpCS and DownCS work on
    uint   Reset;                   // 1: CarveCS empties every cell
    float3 Shift;  uint Count;      // as the probe moves: its old centre from its new one; 1: a counting frame
};

// One more of a counting frame's numbers (Stats: 0 carved, 4 taken, 8 known, 12 taken over).
void Counted(uint At)
{
    uint Was;
    if (Count != 0u)
        Stats.InterlockedAdd(At, 1u, Was);
}

#define ProbeLevelLoad(Byte) Levels.Load4(Byte)

// ---- PROBE-SHARED-BEGIN: MirrorProbe.hlsl and SolidPass.hlsl hold this block alike, character for character
// (scripts/check_shaders.py), each with ProbeLevelLoad defined before it.
// The probe's numbers (RenderLimits.h's RDX_PROBE_*_VALUE): texels a face of the finest level, the pyramid's levels,
// and the mirror rays' march - its even steps and the halvings after the step that crossed.
#define RDX_PROBE_SIDE     128u
#define RDX_PROBE_LEVELS   8u
#define RDX_PROBE_STEPS    8
#define RDX_PROBE_HALVINGS 8
// A direction's face of the cube and place on it, -1..1 each way, as Direct3D lays a cube's faces (RdxMath::ProbeFaceOf).
uint ProbeFaceOf(float3 D, out float2 Uv)
{
    const float3 A = abs(D);
    if (A.x >= A.y && A.x >= A.z)
    {
        Uv = float2((D.x > 0.0f) ? -D.z : D.z, -D.y) / A.x;
        return (D.x > 0.0f) ? 0u : 1u;
    }
    if (A.y >= A.z)
    {
        Uv = float2(D.x, (D.y > 0.0f) ? D.z : -D.z) / A.y;
        return (D.y > 0.0f) ? 2u : 3u;
    }
    Uv = float2((D.z > 0.0f) ? D.x : -D.x, -D.y) / A.z;
    return (D.z > 0.0f) ? 4u : 5u;
}
// A level's texels a face, and where it starts among the pyramid's entries.
uint ProbeLevelSide(uint Level)
{
    return RDX_PROBE_SIDE >> Level;
}
uint ProbeLevelStart(uint Level)
{
    // Six faces of the levels before it: 6 S^2 (1 + 1/4 + ... ) up to the level, which sums to this.
    const uint S = RDX_PROBE_SIDE, L = RDX_PROBE_SIDE >> Level;
    return 8u * (S * S - L * L);
}
// The entry of a level that holds a direction (not of length nought).
uint ProbeEntryOf(uint Level, float3 D)
{
    float2     Uv;
    const uint Face = ProbeFaceOf(D, Uv);
    const uint S    = ProbeLevelSide(Level);
    const uint2 T   = min(uint2((Uv * 0.5f + 0.5f) * float(S)), uint2(S - 1u, S - 1u));
    return ProbeLevelStart(Level) + (Face * S + T.y) * S + T.x;
}
struct FProbeEntry
{
    float3 Colour;
    float  Trust;       // 0: nothing known
    float  Dist;        // the nearest point the entry holds, from the probe's centre
};
FProbeEntry ProbeRead(uint Entry)
{
    const uint4 V = ProbeLevelLoad(Entry * 16u);
    FProbeEntry E;
    E.Colour = float3(f16tof32(V.x), f16tof32(V.x >> 16), f16tof32(V.y));
    E.Trust  = f16tof32(V.y >> 16);
    E.Dist   = asfloat(V.z);
    return E;
}
// Whether a place along a mirror ray, from the probe's centre, lies as far as the point the finest level holds in its
// direction or further: the ray has passed that point's surface there. An entry that knows nothing stops no ray.
bool ProbePassed(float3 Q)
{
    const float L = length(Q);
    if (!(L > 0.0f))
        return false;
    const FProbeEntry E = ProbeRead(ProbeEntryOf(0u, Q));
    return E.Trust > 0.0f && L >= E.Dist;
}
// RdxMath::ProbeMarch on the finest level: a ray from At (from the probe's centre) along Way (of length one), in even
// steps out to Reach, the step that crossed then halved. T: how far along the ray; false where it crossed nothing.
bool ProbeMarch(float3 At, float3 Way, float Reach, out float T)
{
    T = 0.0f;
    float Prev = 0.0f;
    [loop] for (int i = 1; i <= RDX_PROBE_STEPS; i++)
    {
        const float S = Reach * float(i) / float(RDX_PROBE_STEPS);
        if (ProbePassed(At + Way * S))
        {
            float Lo = Prev, Hi = S;
            [loop] for (int h = 0; h < RDX_PROBE_HALVINGS; h++)
            {
                const float M = 0.5f * (Lo + Hi);
                if (ProbePassed(At + Way * M))
                    Hi = M;
                else
                    Lo = M;
            }
            T = Hi;
            return true;
        }
        Prev = S;
    }
    return false;
}
// ---- PROBE-SHARED-END

// How many levels up a hole may take its colour from (RenderLimits.h's RDX_PROBE_FILL_UP_VALUE): a block of about
// eleven degrees at the finest level's 128 texels a face (docs/tukrozodes-szonda-terv.md 6.1).
#define RDX_PROBE_FILL_UP 4u
// The least of what a see-through draw lays over a pixel that keeps it out of the probe: a step of the picture's
// ten-bit colours, below which nothing was laid on.
#define RDX_PROBE_OVER_LEAST (1.0f / 1024.0f)

void ProbeWrite(uint Entry, float3 Colour, float Trust, float Dist)
{
    Levels.Store4(Entry * 16u, uint4(f32tof16(Colour.r) | (f32tof16(Colour.g) << 16),
                                     f32tof16(Colour.b) | (f32tof16(Trust) << 16), asuint(Dist), 0u));
}

[numthreads(8, 8, 1)]
void CarveCS(uint3 Id : SV_DispatchThreadID)
{
    if (Id.x >= RDX_PROBE_SIDE || Id.y >= RDX_PROBE_SIDE)
        return;
    const uint I = (Id.z * RDX_PROBE_SIDE + Id.y) * RDX_PROBE_SIDE + Id.x;
    Keys.Store(I * 4u, 0xFFFFFFFFu);
    if (Reset != 0u)
    {
        Cells.Store4(I * 32u, uint4(0u, 0u, 0u, 0u));
        Cells.Store4(I * 32u + 16u, uint4(0u, 0u, 0u, 0u));
        return;
    }
    const float4 P = asfloat(Cells.Load4(I * 32u));
    if (!(P.w > 0.0f))
        return;
    // The point as the eye sees it now. Off the picture, or behind the eye, nothing is known of it this frame.
    const float3 W = Centre + P.xyz;
    const float3 V = float3(dot(W, AxisX), dot(W, AxisY), dot(W, AxisZ));
    if (V.z < 1.0f)
        return;
    const float2 Ndc = float2(V.x / (V.z * TanX), V.y / (V.z * TanY));
    if (any(abs(Ndc) >= 1.0f))
        return;
    uint Wd, Ht;
    Depth.GetDimensions(Wd, Ht);
    // The four pixels round it: only where the eye sees past it in every one of them is it gone - at a silhouette's
    // edge one of them still shows what the point lies on.
    const float2 At = (Rect.xy + (Ndc * 0.5f + 0.5f) * Rect.zw) * float2(Wd, Ht) - 0.5f;
    const int2   B  = int2(floor(At));
    float Nearest = 1e30f;
    [unroll] for (int k = 0; k < 4; k++)
    {
        const int2  Q = clamp(B + int2(k & 1, k >> 1), int2(0, 0), int2(int(Wd) - 1, int(Ht) - 1));
        const float Z = Depth.Load(int3(Q, 0));
        Nearest = min(Nearest, (Z > 0.0f) ? 1.0f / Z : 1e30f);
    }
    if (Nearest > V.z + Rules.x + V.z * Rules.y)
    {
        Cells.Store(I * 32u + 12u, 0u);
        Counted(0u);
    }
}

[numthreads(1, 1, 1)]
void StatsClearCS(uint3 Id : SV_DispatchThreadID)
{
    Stats.Store4(0u, uint4(0u, 0u, 0u, 0u));
}

// The pixel a thread takes, its point from the probe's centre, and the cell that point falls in; false where it gives
// the probe nothing - nothing drawn there, not the level's own surface (an actor, the grass: they move, or stand where
// they please), something laid over it, outside the world's view, or out of the probe's reach.
bool ProbePixel(uint2 Id, out float3 From, out uint Cell, out uint2 Px)
{
    Px   = Id * 2u + uint2(Pick & 1u, Pick >> 1);
    From = float3(0.0f, 0.0f, 0.0f);
    Cell = 0u;
    uint Wd, Ht;
    Depth.GetDimensions(Wd, Ht);
    if (Px.x >= Wd || Px.y >= Ht)
        return false;
    const float Z = Depth.Load(int3(Px, 0));
    if (!(Z > 0.0f) || Kinds.Load(int3(Px, 0)).g != 0u)
        return false;
    const float3 Over = Added.Load(int3(Px, 0)).rgb;
    if (max(Over.r, max(Over.g, Over.b)) > RDX_PROBE_OVER_LEAST)
        return false;
    const float2 Ndc = ((float2(Px) + 0.5f) / float2(Wd, Ht) - Rect.xy) / Rect.zw * 2.0f - 1.0f;
    if (any(abs(Ndc) > 1.0f))
        return false;
    const float Zv = 1.0f / Z;
    From = (Ndc.x * TanX * Zv) * AxisX + (Ndc.y * TanY * Zv) * AxisY + Zv * AxisZ - Centre;
    const float L = length(From);
    if (L > Reach || L < 1.0f)
        return false;
    Cell = ProbeEntryOf(0u, From);      // the finest level's entries are numbered as the cells are
    return true;
}

[numthreads(8, 8, 1)]
void KeyCS(uint3 Id : SV_DispatchThreadID)
{
    float3 From;
    uint   Cell, Was;
    uint2  Px;
    if (ProbePixel(Id.xy, From, Cell, Px))
        Keys.InterlockedMin(Cell * 4u, asuint(length(From)), Was);
}

[numthreads(8, 8, 1)]
void GatherCS(uint3 Id : SV_DispatchThreadID)
{
    float3 From;
    uint   Cell;
    uint2  Px;
    if (!ProbePixel(Id.xy, From, Cell, Px))
        return;
    // The nearest of the frame's points in the cell - within a hair: this entry point works the distance out again,
    // and need not round its last bit as KeyCS did. Pixels as near as that are the same point, and the first of them
    // to claim the key writes the cell alone - two writing it would leave one's point with the other's colour.
    const float L   = length(From);
    const uint  Key = Keys.Load(Cell * 4u);
    if (Key == 0xFFFFFFFEu || L > asfloat(Key) * 1.00001f)
        return;
    uint Was;
    Keys.InterlockedCompareExchange(Cell * 4u, Key, 0xFFFFFFFEu, Was);
    if (Was != Key)
        return;
    const uint   B    = Cell * 32u;
    const float  Held = asfloat(Cells.Load(B + 12u));
    const float3 C    = Picture.Load(int3(Px, 0)).rgb;
    const uint4  Seen = uint4(f32tof16(C.r) | (f32tof16(C.g) << 16), f32tof16(C.b), Frame, 0u);
    if (!(Held > 0.0f) || L < Held * (1.0f - Rules.z))
    {
        Cells.Store4(B, uint4(asuint(From), asuint(L)));
        Cells.Store4(B + 16u, Seen);
        Counted(4u);
    }
    else if (L <= Held * (1.0f + Rules.z))
    {
        Cells.Store4(B + 16u, Seen);
        Counted(4u);
    }
}

// As the probe moves (FMirrorProbe::Update, before the frame's own passes): what it holds, taken over round its new
// centre - each cell's point, from the new centre, into the cell it falls in now, the nearest where several fall in one
// (as KeyCS and GatherCS take the picture's) - into the spare cells, then copied back. What it has seen goes with it.
[numthreads(8, 8, 1)]
void ClearSpareCS(uint3 Id : SV_DispatchThreadID)
{
    if (Id.x >= RDX_PROBE_SIDE || Id.y >= RDX_PROBE_SIDE)
        return;
    const uint I = (Id.z * RDX_PROBE_SIDE + Id.y) * RDX_PROBE_SIDE + Id.x;
    Keys.Store(I * 4u, 0xFFFFFFFFu);
    Spare.Store4(I * 32u, uint4(0u, 0u, 0u, 0u));
    Spare.Store4(I * 32u + 16u, uint4(0u, 0u, 0u, 0u));
}

// A cell's point from the new centre, and the cell it falls in there; false where the cell holds none, or the point lies
// out of the probe's reach from there.
bool MovedPoint(uint3 Id, out uint I, out float3 From, out uint Cell)
{
    I    = (Id.z * RDX_PROBE_SIDE + Id.y) * RDX_PROBE_SIDE + Id.x;
    From = float3(0.0f, 0.0f, 0.0f);
    Cell = 0u;
    if (Id.x >= RDX_PROBE_SIDE || Id.y >= RDX_PROBE_SIDE)
        return false;
    const float4 P = asfloat(Cells.Load4(I * 32u));
    if (!(P.w > 0.0f))
        return false;
    From = P.xyz + Shift;
    const float L = length(From);
    if (L > Reach || L < 1.0f)
        return false;
    Cell = ProbeEntryOf(0u, From);
    return true;
}

[numthreads(8, 8, 1)]
void TransferKeyCS(uint3 Id : SV_DispatchThreadID)
{
    float3 From;
    uint   I, Cell, Was;
    if (MovedPoint(Id, I, From, Cell))
        Keys.InterlockedMin(Cell * 4u, asuint(length(From)), Was);
}

[numthreads(8, 8, 1)]
void TransferGatherCS(uint3 Id : SV_DispatchThreadID)
{
    float3 From;
    uint   I, Cell;
    if (!MovedPoint(Id, I, From, Cell))
        return;
    const float L   = length(From);
    const uint  Key = Keys.Load(Cell * 4u);
    if (Key == 0xFFFFFFFEu || L > asfloat(Key) * 1.00001f)
        return;
    uint Was;
    Keys.InterlockedCompareExchange(Cell * 4u, Key, 0xFFFFFFFEu, Was);
    if (Was != Key)
        return;
    Spare.Store4(Cell * 32u, uint4(asuint(From), asuint(L)));
    Spare.Store4(Cell * 32u + 16u, Cells.Load4(I * 32u + 16u));
    Counted(12u);
}

[numthreads(8, 8, 1)]
void TransferBackCS(uint3 Id : SV_DispatchThreadID)
{
    if (Id.x >= RDX_PROBE_SIDE || Id.y >= RDX_PROBE_SIDE)
        return;
    const uint I = (Id.z * RDX_PROBE_SIDE + Id.y) * RDX_PROBE_SIDE + Id.x;
    Cells.Store4(I * 32u, Spare.Load4(I * 32u));
    Cells.Store4(I * 32u + 16u, Spare.Load4(I * 32u + 16u));
}

[numthreads(8, 8, 1)]
void BaseCS(uint3 Id : SV_DispatchThreadID)
{
    if (Id.x >= RDX_PROBE_SIDE || Id.y >= RDX_PROBE_SIDE)
        return;
    const uint  I = (Id.z * RDX_PROBE_SIDE + Id.y) * RDX_PROBE_SIDE + Id.x;
    const float D = asfloat(Cells.Load(I * 32u + 12u));
    const uint2 C = Cells.Load2(I * 32u + 16u);
    if (D > 0.0f)
    {
        Levels.Store4(I * 16u, uint4(C.x, (C.y & 0xFFFFu) | (f32tof16(1.0f) << 16), asuint(D), 0u));
        Counted(8u);
    }
    else
        Levels.Store4(I * 16u, uint4(0u, 0u, 0u, 0u));
}

[numthreads(8, 8, 1)]
void UpCS(uint3 Id : SV_DispatchThreadID)
{
    const uint S = ProbeLevelSide(Level);
    if (Id.x >= S || Id.y >= S)
        return;
    const uint Below = ProbeLevelStart(Level - 1u), SB = S * 2u;
    float3 Colour = float3(0.0f, 0.0f, 0.0f);
    float  Weight = 0.0f, Dist = 1e30f;
    [unroll] for (int k = 0; k < 4; k++)
    {
        const uint2       T = Id.xy * 2u + uint2(k & 1, k >> 1);
        const FProbeEntry E = ProbeRead(Below + (Id.z * SB + T.y) * SB + T.x);
        if (E.Trust > 0.0f)
        {
            Colour += E.Colour * E.Trust;
            Weight += E.Trust;
            Dist    = min(Dist, E.Dist);
        }
    }
    const uint Out = ProbeLevelStart(Level) + (Id.z * S + Id.y) * S + Id.x;
    if (Weight > 0.0f)
        ProbeWrite(Out, Colour / Weight, Weight * 0.25f, Dist);
    else
        Levels.Store4(Out * 16u, uint4(0u, 0u, 0u, 0u));
}

[numthreads(8, 8, 1)]
void DownCS(uint3 Id : SV_DispatchThreadID)
{
    const uint S = ProbeLevelSide(Level);
    if (Id.x >= S || Id.y >= S)
        return;
    const uint Here = ProbeLevelStart(Level) + (Id.z * S + Id.y) * S + Id.x;
    if (ProbeRead(Here).Trust > 0.0f)
        return;
    // The texel above it holds its nearest point and its trust; its colour is the four nearest above it, each by how
    // near it stands and how much it is trusted - so a hole fills smooth, not in the coarser level's squares. On its
    // own face: a hole at a face's edge takes the edge's colour.
    const uint   SP     = S / 2u;
    const uint   Start  = ProbeLevelStart(Level + 1u) + Id.z * SP * SP;
    const uint   Parent = Start + (Id.y / 2u) * SP + Id.x / 2u;
    const float2 At     = (float2(Id.xy) + 0.5f) * 0.5f - 0.5f;
    const int2   Lo     = int2(floor(At));
    const float2 Fr     = At - float2(Lo);
    float3 Colour = float3(0.0f, 0.0f, 0.0f);
    float  Weight = 0.0f;
    [unroll] for (int k = 0; k < 4; k++)
    {
        const int2  T = clamp(Lo + int2(k & 1, k >> 1), int2(0, 0), int2(int(SP) - 1, int(SP) - 1));
        const float W = ((k & 1) ? Fr.x : 1.0f - Fr.x) * ((k >> 1) ? Fr.y : 1.0f - Fr.y);
        const FProbeEntry E = ProbeRead(Start + uint(T.y) * SP + uint(T.x));
        Colour += E.Colour * (W * E.Trust);
        Weight += W * E.Trust;
    }
    const FProbeEntry Own = ProbeRead(Parent);
    if (Own.Trust > 0.0f && Weight > 0.0f)
        ProbeWrite(Here, Colour / Weight, Own.Trust, Own.Dist);
    else
        Levels.Store4(Here * 16u, Levels.Load4(Parent * 16u));
}

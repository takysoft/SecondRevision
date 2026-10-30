// SecondRevision - the wet floors' late reflection pass (FWetReflect; docs/terv-nedves-feluletek.md 8.6). Once a frame,
// right after the copies of the picture and the depth are made (FBackend::CopyForWater) - the opaque world and the actors
// drawn, nothing see-through yet, the mirror probe gathered -, over the pixels the wet floors marked (SolidPass.hlsl's
// WetMark): what the water standing on a floor and the film of it on the wet stone give back of the world, from the
// copy of the picture. The image is laid on, and what lies under the water is dimmed by as much as its surface gives
// back: the blend is Src.rgb + Dst.rgb * Src.a. With the shadows' targets the added one takes the same, so their factor
// leaves the image alone, as it leaves the water's. Plain ASCII: the build refuses anything else (embed-text.ps1).

// The wet block as the frame's wet runs read it (SolidPass.hlsl's WetConsts), then the pass's own: the copies' size in
// pixels and one over it; the film's share of what the water would give back where the stone has a relief
// (RdxMath::WetFilmShare), the damp stone's spread - how far across its image is spread for each unit the ray goes
// (RdxMath::WetDampSpread) -, the lamps' streaks' strength and their roughness on the damp stone
// (RdxMath::WetStreakStrength, WetStreakRough); in StreakLook.x their
// roughness on the water (RdxMath::WetStreakPuddleRough); in Rays.x whether the water's image is the rays' this frame
// (SolidPass.hlsl's WetMirror, Reflections=RTLow and up), in Rays.y how thick the rain falls on the floors (0 to 1,
// RdxFloorRainThick), in Rays.z the level's clock its rings run by, in Rays.w the image the floors give back by the
// level - 0 none (Reflections=Low: the water and the lamps' streaks alone), 1 coarse (Medium), 2 the full trace.
cbuffer WetPass : register(b0)
{
    float4 WetView[4];
    float4 WetScreen;
    float4 WetLook[2];
    float4 WetCover;
    float4 CopySize;
    float4 Film;
    float4 StreakLook;
    float4 Rays;
};
// The lamps whose streaks the floors give back this frame (FBackend::SetWetLamps; the lamps' bodies of
// SecondRevisionLampSurfaces.txt, each of its glass's colour): how many in WetLampCount.x - every lamp due a streak;
// WET_LAMP_MAX (RdxMath::WetLampMax) is only the room a constant buffer has - and two rows a lamp: its place in the
// view's space and its glass's radius, its glass's colour by its brightness.
#define WET_LAMP_MAX 2047
cbuffer WetLamps : register(b1)
{
    float4 WetLampCount;
    float4 WetLamp[2 * WET_LAMP_MAX];
};
// The own lamps - the flashlight, the weapon lamps - for their glints (FBackend::DrawWetReflections): how many (x)
// and their spill's share (y); their cones' rims as the solid pass's own lamps have them (SpotRims: the hot rim, one
// over its width, the spill's rim, one over its width); then each one's three rows - its lens in the view's space and
// its radius, its light as bright as the lens shines it, its axis.
#define WET_SPOT_MAX 9
cbuffer WetSpots : register(b2)
{
    float4 SpotHead;
    float4 SpotRims;
    float4 Spot[3 * WET_SPOT_MAX];
};
Texture2D       SceneCopy : register(t0);
Texture2D       DepthCopy : register(t1);
Texture2D<uint> WetMark   : register(t2);     // a pixel's mark at its even column, its relief's normal at the odd one
Texture2D<float4> WetCoverMap : register(t3); // the rain's cover (FBackend::SetWetCover), as the floor's runs read it
SamplerState    Samp      : register(s0);

// ---- WET-SHARED-BEGIN: SolidPass.hlsl and WetReflect.hlsl hold this alike (scripts/check_shaders.py checks it), so
// the late reflection pass finds the patches the floor was drawn with. The arithmetic is RdxMath's, WetLean and on.
// The hash of a grid point the noise leans by there: FieldLean's, by whole-number arithmetic.
float2 WetLean(int2 Q)
{
    uint H = (uint)Q.x * 0x9E3779B9u + (uint)Q.y * 0xBB67AE85u;
    H ^= H >> 15;
    H *= 0x6A09E667u;
    H ^= H >> 13;
    H *= 0x510E527Fu;
    H ^= H >> 16;
    return float2((float)(H & 0xFFFFu), (float)(H >> 16)) * (2.0f / 65535.0f) - 1.0f;
}
// The gradient noise the patches are made of: zero at every grid point, leaning there, a quintic ease between.
float WetNoise(float2 P)
{
    const float2 I   = floor(P);
    const float2 F   = P - I;
    const float2 U   = F * F * F * (F * (F * 6.0f - 15.0f) + 10.0f);
    const int2   Q   = (int2)I;
    const float  V00 = dot(WetLean(Q), F);
    const float  V10 = dot(WetLean(Q + int2(1, 0)), F - float2(1.0f, 0.0f));
    const float  V01 = dot(WetLean(Q + int2(0, 1)), F - float2(0.0f, 1.0f));
    const float  V11 = dot(WetLean(Q + int2(1, 1)), F - float2(1.0f, 1.0f));
    return V00 + U.x * (V10 - V00) + U.y * (V01 - V00) + U.x * U.y * (V00 - V10 - V01 + V11);
}
// The patch value at a place of the level: three sizes of the noise, each half the one before.
float WetPatch(float2 Level)
{
    float2 P   = Level / WetLook[0].x;
    float  Sum = 0.0f;
    float  Amp = 1.0f;
    [unroll] for (int i = 0; i < 3; i++)
    {
        Sum += Amp * WetNoise(P);
        P    = P * 2.03f + float2(17.7f, 31.3f);
        Amp *= 0.5f;
    }
    return Sum;
}
// How wet the floor is with that patch value, and how high the water stands in its relief.
float WetLevelOf(float Patch)
{
    return saturate(WetLook[0].y + WetLook[0].z * Patch);
}
float WetFloodOf(float Patch)
{
    return WetLook[1].x + (WetLook[1].y - WetLook[1].x) * saturate(0.5f + WetLook[1].z * Patch);
}
// A pixel's place in the view (its depth Pos.z holds one over the view depth) and in the level, by the view's rows.
float3 WetViewPlace(float4 Pos)
{
    const float Z = 1.0f / max(Pos.z, 1e-6f);
    return float3((Pos.xy - WetScreen.xy) * (Z / max(WetScreen.z, 1.0f)), Z);
}
float3 WetLevelPlace(float4 Pos)
{
    const float3 P = WetViewPlace(Pos);
    return WetView[0].xyz + P.x * WetView[1].xyz + P.y * WetView[2].xyz + P.z * WetView[3].xyz;
}
// The rain's cover at a place of the level (FBackend::SetWetCover; docs/terv-nedves-feluletek.md 8.4): its open share
// there, eased by a roof's edge - 1 where the rain falls, 0 under the roof. A floor more than 64 units
// (RdxMath::WetUnderFloor) below the highest floor near it - a pier under its shelter's roof - takes the lower floors'
// share, eased the same way from the edge of the floor above; any other the highest floor's own. No grid (WetCover.z
// nought): open everywhere.
float WetCoverOf(float3 Place)
{
    if (WetCover.z <= 0.0f)
        return 1.0f;
    const float2 Uv = (Place.xy - WetCover.xy) * WetCover.zw;
    if (any(Uv < 0.0f) || any(Uv > 1.0f))
        return 1.0f;
    // Each of the four cells round the place gives its own share - the lower floors' where the place lies more than 64
    // units under that cell's highest floor, else the highest floor's - and the four are mixed by how near the place is
    // to each: a floor under a higher one dries across a cell, not cut along the grid's lines.
    float2 Size;
    WetCoverMap.GetDimensions(Size.x, Size.y);
    const float2 At = Uv * Size - 0.5f;
    const float2 C0 = floor(At);
    const float2 F  = At - C0;
    float        S[4];
    [unroll] for (int k = 0; k < 4; k++)
    {
        const float4 V = WetCoverMap.Load(int3(clamp(C0 + float2(k & 1, k >> 1), 0.0f, Size - 1.0f), 0));
        S[k] = (Place.z < V.w - 64.0f) ? V.z : V.x;
    }
    return lerp(lerp(S[0], S[1], F.x), lerp(S[2], S[3], F.x), F.y);
}
// ---- WET-SHARED-END
// ---- RAIN-SHARED-BEGIN: SolidPass.hlsl and WetReflect.hlsl hold this alike (scripts/check_shaders.py checks it), so
// the water and the wet floors' puddles ring alike under the same rain.
#define RAIN_RING_REACH    6.0f       // how far a ring runs, in the level's units
#define RAIN_RING_WIDTH    2.5f       // its crest's width
#define RAIN_RING_SECONDS  0.7f       // how long it lasts
#define RAIN_RING_STEEP    0.4f       // the crest's slope, before the bell's own 0.86
#define RAIN_RING_FAR      2560.0f
// Two numbers in 0..1 out of a cell's whole-number place and a count: a scramble of integers, the same on every card.
float2 RainDice(int2 Cell, int Count)
{
    uint H = ((uint)Cell.x * 2654435761u) ^ ((uint)Cell.y * 1103515245u) ^ ((uint)Count * 668265263u);
    H ^= H >> 15;
    H *= 2654435761u;
    H ^= H >> 13;
    H *= 1103515245u;
    H ^= H >> 16;
    return float2(H & 0xFFFFu, H >> 16) / 65535.0f;
}
// One net's ring at a point: Foot how much of the level a pixel covers there along the view, Cell the cells' size, Beat
// the seconds between two drops in a cell, Salt the net's own number. The drop lands in the cell's middle part, so its
// ring stays inside the cell. A crest finer than a pixel's share of the level is drawn as wide as that and as much
// lower: the slope it gives the pixel is the same on the whole, where a finer crest would come and go between pixels.
float2 RainRing(float2 World, float Time, float Foot, float Cell, float Beat, int Salt)
{
    const float2 P    = World / Cell;
    const int2   Id   = (int2)floor(P);
    const float  T    = Time / Beat + RainDice(Id, Salt).x;       // every cell keeps its own time
    const float  Age  = frac(T) * Beat / RAIN_RING_SECONDS;       // 0 as the drop goes in, 1 as its ring is gone
    const float2 Mid  = (float2)Id + 0.3f + 0.4f * RainDice(Id, Salt + 1 + (int)floor(T));
    const float2 D    = (P - Mid) * Cell;
    const float  R    = length(D);
    const float  Wide = max(RAIN_RING_WIDTH, Foot);
    const float  X    = (R - Age * RAIN_RING_REACH) / Wide;
    // At its full height while it runs out, and gone over the last part of the way: a ring that faded from the start
    // would be seen only as the dot it begins as.
    const float  Live = saturate((1.0f - Age) * 2.5f);
    return D / max(R, 0.01f) * (-2.0f * X * exp(-X * X) * Live * RAIN_RING_STEEP * (RAIN_RING_WIDTH / Wide));
}
// The rings at a point of the level, as a slope of the surface: Foot how much of the level a pixel covers there along
// the view, Flat the square of its distance from the camera along the level, On how thick the rain falls, 0 to 1. They
// go where a whole ring comes to no more than about a pixel.
float2 RainRingsAt(float2 World, float Time, float Foot, float Flat, float On)
{
    const float Keep = saturate(1.5f - Foot / RAIN_RING_REACH) *
                       saturate((RAIN_RING_FAR * RAIN_RING_FAR - Flat) / (0.4f * RAIN_RING_FAR * RAIN_RING_FAR));
    if (Keep <= 0.0f)
        return 0.0f;
    return (RainRing(World, Time, Foot, 62.0f, 1.5f, 0) + RainRing(World, Time, Foot, 80.0f, 0.9f, 4096)) *
           (Keep * On);
}
// How much of a wet floor's standing water is left near an edge the floor ends at and drops from, as water runs off
// such an edge: none within WET_EDGE_NONE of it, all of it from WET_EDGE_ALL in, a smoothstep between - and a wall the
// floor runs into keeps it to the wall's foot, where water gathers. Found in the picture: round the pixel's place, along
// the floor in eight ways and at three reaches, each point put on the screen and the depth there read. Where the floor
// goes on, what is seen there is the point itself; where it has ended, what is seen lies further off - the ground below;
// a wall rises nearer, as does a thing lying on the floor. View space: P the pixel's place, Up the level's up (of
// length one); a point Q is seen at the pixel (Centre + Q.xy * Focal / Q.z) * Scale + Offset of the depth's copy, whose
// depth is one over the distance along the view.
#define WET_EDGE_NONE 1.05f
#define WET_EDGE_ALL  2.6f
float WetEdgeKeep(float3 P, float3 Up, float2 Centre, float Focal, float2 Scale, float2 Offset)
{
    float2 Size;
    DepthCopy.GetDimensions(Size.x, Size.y);
    float3 A = float3(1.0f, 0.0f, 0.0f) - Up * Up.x;
    if (dot(A, A) < 0.01f)
        A = float3(0.0f, 1.0f, 0.0f) - Up * Up.y;
    A = normalize(A);
    const float3 B       = cross(Up, A);
    const float  Reach[3] = { WET_EDGE_NONE, 0.5f * (WET_EDGE_NONE + WET_EDGE_ALL), WET_EDGE_ALL };
    float        Edge    = WET_EDGE_ALL + 1.0f;
    [unroll] for (int k = 0; k < 8; k++)
    {
        const float  T   = 0.78539816f * (float)k;
        const float3 Way = A * cos(T) + B * sin(T);
        [unroll] for (int r = 0; r < 3; r++)
        {
            const float3 Q = P + Way * Reach[r];
            if (Q.z <= 1.0f)
                break;
            const float2 At = (Centre + Q.xy * (Focal / Q.z)) * Scale + Offset;
            if (any(At < 0.0f) || any(At >= Size))
                break;
            const float Seen = 1.0f / max(DepthCopy.Load(int3(At, 0)).r, 1e-6f);
            if (Seen > Q.z + max(2.0f, 0.01f * Q.z))
            {
                Edge = min(Edge, (r == 0) ? 0.0f : 0.5f * (Reach[r - 1] + Reach[r]));
                break;
            }
        }
    }
    return smoothstep(WET_EDGE_NONE, WET_EDGE_ALL, Edge);
}
// ---- RAIN-SHARED-END
// The whole screen in one triangle.
float4 VSFull(uint Id : SV_VertexID) : SV_Position
{
    const float2 P = float2((float)((Id << 1) & 2u), (float)(Id & 2u));
    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);
}

// Whether the mark is of the floor seen at a pixel of that depth (RdxMath::WetMarkHolds): the depth's top bits the same,
// or a step off.
bool WetMarkHolds(uint Mark, float Depth)
{
    if (Mark == 0u)
        return false;
    const uint A = Mark >> 12;
    const uint B = asuint(Depth) >> 12;
    return ((A > B) ? A - B : B - A) <= 1u;
}

// The relief's normal a wet floor left beside its mark (SolidPass.hlsl's WetTiltOf; RdxMath::WetTiltNormal), in the
// view's space: false where the floor had no relief, or where the word is another floor's than the mark's.
bool WetTiltNormal(uint Tilt, uint Mark, out float3 N)
{
    N = float3(0.0f, 0.0f, 0.0f);
    const uint A = Tilt >> 20;
    const uint B = Mark >> 20;
    if ((Tilt & 0xFFC00u) == 0u || ((A > B) ? A - B : B - A) > 1u)
        return false;
    const float2 Q  = float2((float)((Tilt >> 10) & 1023u), (float)(Tilt & 1023u));
    const float2 XY = (Q - 1.0f) * (2.0f / 1022.0f) - 1.0f;
    const float3 L  = float3(XY, sqrt(saturate(1.0f - dot(XY, XY))));
    N = normalize(float3(dot(L, WetView[1].xyz), dot(L, WetView[2].xyz), dot(L, WetView[3].xyz)));
    return true;
}

// Where a point of the view's space is seen in the copies, 0..1: by the view's centre - the frame's jitter in it, as the
// picture was drawn - and its projection.
float2 WetUvOf(float3 Q)
{
    return (WetScreen.xy + Q.xy * (WetScreen.z / Q.z)) * CopySize.zw;
}
float WetSceneZ(float2 Uv)
{
    return 1.0f / max(DepthCopy.Load(int3(min(Uv * CopySize.xy, CopySize.xy - 1.0f), 0)).r, 1e-6f);
}

// The copy of the picture at Uv, spread over Radius pixels: the place itself and eight samples on two rings about it,
// the same every frame - a fixed pattern, not a turning one (docs/terv-nedves-feluletek.md 9.3).
float3 WetImage(float2 Uv, float Radius)
{
    const float3 Sharp = SceneCopy.SampleLevel(Samp, Uv, 0.0f).rgb;
    if (Radius < 0.75f)
        return Sharp;
    const float2 Ring[8] = { float2(1.0f, 0.0f), float2(0.0f, 1.0f), float2(-1.0f, 0.0f), float2(0.0f, -1.0f),
                             float2(0.35f, 0.35f), float2(-0.35f, 0.35f), float2(-0.35f, -0.35f), float2(0.35f, -0.35f) };
    float3 Sum = Sharp;
    [unroll] for (int k = 0; k < 8; k++)
        Sum += SceneCopy.SampleLevel(Samp, saturate(Uv + Ring[k] * (Radius * CopySize.zw)), 0.0f).rgb;
    return Sum / 9.0f;
}

// The ray from the floor's point P along R, through the copy of the depth - SolidPass.hlsl's WaterReflect's walk, in the
// view's space: from the point in steps that grow with the distance, the place the ray has just gone behind what the
// picture holds found more closely by a few halvings; behind by more than the step itself, or a wall's thickness, is
// something in front, not what the ray met. rgb: the image there, spread by Spread for each unit the ray went; a: how
// much of it to trust - nothing where the ray met nothing, less towards the picture's edges.
float4 WetTrace(float3 P, float3 R, float Spread, int Steps)
{
    const float Reach = 4096.0f;
    float Prev  = 0.0f;
    float PrevZ = P.z;
    [loop] for (int i = 1; i <= Steps; i++)
    {
        const float  T = Reach * (float(i) / Steps) * (float(i) / Steps);
        const float3 Q = P + R * T;
        if (Q.z < 1.0f)
            break;                                     // behind the camera: not in the picture
        const float2 Uv = WetUvOf(Q);
        if (any(Uv < 0.0f) || any(Uv > 1.0f))
            break;
        const float Scene = WetSceneZ(Uv);
        if (Q.z > Scene && Q.z - Scene < max(max(24.0f, Q.z * 0.02f), (Q.z - PrevZ) * 1.5f))
        {
            float  Lo = Prev;
            float  Hi = T;
            float2 At = Uv;
            [unroll] for (int k = 0; k < 5; k++)
            {
                const float  M  = 0.5f * (Lo + Hi);
                const float3 MQ = P + R * M;
                const float  MZ = max(MQ.z, 1.0f);
                const float2 MU = saturate(WetUvOf(float3(MQ.xy, MZ)));
                if (MZ > WetSceneZ(MU))
                {
                    Hi = M;
                    At = MU;
                }
                else
                    Lo = M;
            }
            const float  Z    = max((P + R * Hi).z, 1.0f);
            const float2 Edge = saturate(min(At, 1.0f - At) * 12.0f);
            return float4(WetImage(At, min(Spread * Hi * WetScreen.z / Z, 48.0f)), Edge.x * Edge.y);
        }
        Prev  = T;
        PrevZ = Q.z;
    }
    // Nothing met within reach: what lies along the ray's own direction far away - the sky, or land beyond the reach -
    // wherever the picture holds something further off than the ray has come.
    if (R.z > 0.01f)
    {
        const float2 Far = WetUvOf(R);
        if (all(Far >= 0.0f) && all(Far <= 1.0f) && WetSceneZ(Far) > PrevZ)
        {
            const float2 Edge = saturate(min(Far, 1.0f - Far) * 12.0f);
            return float4(WetImage(Far, min(Spread * 2.0f * WetScreen.z, 48.0f)), Edge.x * Edge.y);
        }
    }
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
}

// A lamp's streak (docs/terv-nedves-feluletek.md 8.7): its glass's light off the floor as a rough mirror gives it back -
// the GGX lobe about the half-way direction, Smith's masking in its height-correlated form, the water's Fresnel. The
// lobe's shape follows the lamp, the eye and the floor: seen at a low angle it stretches along the view, towards the eye
// - that is the streak, not a stretch laid on the screen - and the stone's relief turns each of its slopes into the
// streak or out of it. The glass is a ball of the lamp's radius (Karis, "Real Shading in Unreal Engine 4", 2013): the
// light comes from its point nearest the mirrored ray, and the lobe is widened by the ball's size with its peak lowered
// as much - a lamp near the floor gives back a patch as large as it is, not a burning point. Its light is a ball of glass
// of that brightness's, falling with the square of the distance: a lamp far off still draws its streak. Lamp: its place
// and radius; Colour: its glass's colour by its brightness.
float3 WetStreak(float3 P, float3 E, float3 N, float Alpha, float4 Lamp, float3 Colour)
{
    const float3 ToC  = Lamp.xyz - P;
    const float  Dist = max(length(ToC), Lamp.w + 1.0f);
    const float3 Mir  = reflect(E, N);
    const float3 Off  = dot(ToC, Mir) * Mir - ToC;
    const float3 L    = normalize(ToC + Off * saturate(Lamp.w / max(length(Off), 1e-3f)));
    const float3 V    = -E;
    const float  NL   = dot(N, L);
    const float  NV   = dot(N, V);
    if (NL <= 0.0f || NV <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);
    const float3 H    = normalize(L + V);
    const float  NH   = saturate(dot(N, H));
    const float  A2   = Alpha * Alpha;
    const float  K    = NH * NH * (A2 - 1.0f) + 1.0f;
    const float  Wide = Alpha / saturate(Alpha + Lamp.w / (2.0f * Dist));
    const float  D    = A2 / (3.14159265f * K * K) * (Wide * Wide);
    const float  Vis  = 0.5f / (NL * sqrt(NV * NV * (1.0f - A2) + A2) + NV * sqrt(NL * NL * (1.0f - A2) + A2));
    const float  F    = 0.02f + 0.98f * pow(1.0f - saturate(dot(V, H)), 5.0f);
    const float  Fall = 3.14159265f * Lamp.w * Lamp.w / (Dist * Dist);
    return Colour * (D * Vis * F * NL * Fall * Film.z);
}

// The frame's lamps' streaks, summed: a lamp of no size gives none, and a fire none - the water gives back its flame
// itself (PSWetFlames).
float3 WetStreaks(float3 P, float3 E, float3 N, float Alpha)
{
    float3    Sum   = float3(0.0f, 0.0f, 0.0f);
    const int Count = min((int)WetLampCount.x, WET_LAMP_MAX);
    [loop] for (int i = 0; i < Count; i++)
    {
        const float4 Lamp = WetLamp[i * 2];
        if (Lamp.w > 0.0f && WetLamp[i * 2 + 1].a < 0.5f)
            Sum += WetStreak(P, E, N, Alpha, Lamp, WetLamp[i * 2 + 1].rgb);
    }
    return Sum;
}

// The own lamps' glints: each one's lens a lamp of its own (WetStreak), as much as its cone throws its light towards
// the floor's point - none outside it.
float3 WetSpotStreaks(float3 P, float3 E, float3 N, float Alpha)
{
    float3    Sum   = float3(0.0f, 0.0f, 0.0f);
    const int Count = min((int)SpotHead.x, WET_SPOT_MAX);
    [loop] for (int i = 0; i < Count; i++)
    {
        const float4 Lens  = Spot[i * 3];
        const float  Cos   = dot(normalize(P - Lens.xyz), Spot[i * 3 + 2].xyz);
        const float  Hot   = smoothstep(0.0f, 1.0f, (Cos - SpotRims.x) * SpotRims.y);
        const float  Spill = smoothstep(0.0f, 1.0f, (Cos - SpotRims.z) * SpotRims.w);
        const float  Cone  = Hot + (1.0f - Hot) * SpotHead.y * Spill;
        if (Cone > 0.0f)
            Sum += WetStreak(P, E, N, Alpha, Lens, Spot[i * 3 + 1].rgb * Cone);
    }
    return Sum;
}

// How far the mirrored ray from P along R (of length one) goes before it meets something standing in its way - Reach
// where nothing that near does. The depth's copy walked across the screen WET_MEET_STRIDE pixels a step, the ray's
// depth at each by the projection (one over it goes evenly across the screen), so a thing's edge is found to within a
// step; the steps start Offset of a step on (0 to 1, the pixel's own, the same every frame), so that where the edge
// is found falls apart from pixel to pixel instead of in bands. Behind what the copy holds by less than a wall's
// thickness (as in WetTrace) is a meeting - unless what it holds there is a wet floor: their own relief, drawn as
// geometry, would meet the ray as it leaves the floor.
#define WET_MEET_STRIDE 2.0f
float WetFirstMeet(float3 P, float3 R, float Reach, float Offset)
{
    float T = Reach;
    if (R.z < 0.0f)
        T = min(T, (P.z - 1.0f) / -R.z);             // no nearer than the camera's own plane
    const float3 Q     = P + R * T;
    const float  I0    = 1.0f / P.z;
    const float  I1    = 1.0f / max(Q.z, 1.0f);
    const float2 From  = WetScreen.xy + P.xy * (WetScreen.z * I0);
    const float2 To    = WetScreen.xy + Q.xy * (WetScreen.z * I1);
    const float  Steps = ceil(length(To - From) / WET_MEET_STRIDE);
    float        PrevZ = P.z;
    [loop] for (float i = 1.0f; i <= Steps; i += 1.0f)
    {
        const float  S  = (i - Offset) / Steps;
        const float2 At = lerp(From, To, S);
        if (any(At < 0.0f) || any(At >= CopySize.xy))
            break;
        const float I     = lerp(I0, I1, S);
        const float Z     = 1.0f / I;
        const float D     = DepthCopy.Load(int3(At, 0)).r;
        const float Scene = 1.0f / max(D, 1e-6f);
        if (Z > Scene && Z - Scene < max(max(24.0f, Z * 0.02f), (Z - PrevZ) * 1.5f) &&
            !WetMarkHolds(WetMark.Load(int3((int)At.x * 2, (int)At.y, 0)), D))
            return T * S * I1 / I;                   // the share of T the ray has gone at that depth
        PrevZ = Z;
    }
    return Reach;
}

struct PSOut
{
    float4 Colour : SV_Target0;
#if RDX_LIGHT_TARGET
    float4 Lit    : SV_Target1;
    float4 Added  : SV_Target2;
#endif
};

// A wet floor's pixel: its place, how wet it is there (the same patches its floor was drawn with), how much of it is
// under water (its mark), and the stone's own normal where it has a relief (the word beside the mark). The view mirrored
// by that normal - by the water's where the water stands, level but for the rain's rings -, by the water's Fresnel
// (n = 1.33: 0.02 face on). What the pixel gives back: all of it where the water stands; on the damp stone the film's share of it by how
// wet the stone is, each of its slopes giving back what lies along its own mirrored ray - and nothing where the floor
// has no relief (Geometry off: the water alone gives back). The lamps' streaks as much, by the same normal. The
// measuring views (SecondRevision-wet-view.txt): 3 the image alone, 4 how much of it was trusted, as grey, 5 the streak
// alone.
PSOut PSWet(float4 Pos : SV_Position)
{
    const int2  At   = int2(Pos.xy);
    const uint  Mark = WetMark.Load(int3(At.x * 2, At.y, 0));
    const float D    = DepthCopy.Load(int3(At, 0)).r;
    if (!WetMarkHolds(Mark, D))
        discard;
    const float4 Here   = float4(Pos.xy, D, 1.0f);
    const float3 P      = WetViewPlace(Here);
    const float3 Place  = WetLevelPlace(Here);
    const float  Wet    = WetLevelOf(WetPatch(Place.xy)) * WetCoverOf(Place);
    const float3 Up     = normalize(float3(WetView[1].z, WetView[2].z, WetView[3].z));
    // The standing water thins out before an edge the floor drops from (WetEdgeKeep).
    const float  Puddle = (Mark & 63u) ? (float)(Mark & 63u) / 63.0f *
                                         WetEdgeKeep(P, Up, WetScreen.xy, max(WetScreen.z, 1.0f), float2(1.0f, 1.0f),
                                                     float2(0.0f, 0.0f))
                                       : 0.0f;
    const float3 E      = normalize(P);
    float3       Stone;
    const bool   bRelief = WetTiltNormal(WetMark.Load(int3(At.x * 2 + 1, At.y, 0)), Mark, Stone);
    // The water's surface level, but where the rain falls into it its rings tilt it (RainRingsAt), as the water's: how
    // much of the floor a pixel covers there along the view, its distance from the camera along the level.
    float3 Water = Up;
    if (Puddle > 0.0f && Rays.y > 0.0f)
    {
        const float2 Off  = Place.xy - WetView[0].xy;
        const float  Foot = length(P) / (max(WetScreen.z, 1.0f) * max(-dot(E, Up), 0.01f));
        const float2 Ring = RainRingsAt(Place.xy, Rays.z, Foot, dot(Off, Off), Rays.y);
        const float3 L    = normalize(float3(-Ring, 1.0f));
        Water = normalize(float3(dot(L, WetView[1].xyz), dot(L, WetView[2].xyz), dot(L, WetView[3].xyz)));
    }
    const float3 N      = normalize(lerp(bRelief ? Stone : Up, Water, Puddle));
    const float3 R      = reflect(E, N);
    const float  F      = 0.02f + 0.98f * pow(1.0f - saturate(-dot(E, N)), 5.0f);
    // Where the rays give the water's image back (WetMirror lays it over this pass's), this pass gives back the damp
    // stone's film alone, and dims nothing: the layer dims what lies under its water itself.
    const bool   bRays  = Rays.x > 0.5f;
    const float  Damp   = bRelief ? Film.x * Wet : 0.0f;
    const float  Share  = lerp(Damp, 1.0f, Puddle);
    const float  Gives  = F * (bRays ? Damp * (1.0f - Puddle) : Share);
    // The level's image (Rays.w): none at Low, at Medium half the steps and spread twice as far.
    const bool   bImage = Rays.w > 0.5f;
    const bool   bCoarse = Rays.w < 1.5f;
    float4 Seen = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if ((Gives > 1.0f / 1024.0f && bImage) || WetScreen.w > 2.5f)
        Seen = WetTrace(P, R, (1.0f - Puddle) * Film.y * (bCoarse ? 2.0f : 1.0f), bCoarse ? 12 : 24);
    const float  Alpha  = lerp(Film.w, StreakLook.x, Puddle);
    const float3 Streak = (WetStreaks(P, E, N, Alpha) + WetSpotStreaks(P, E, N, Alpha)) * Share;
    PSOut Out;
    Out.Colour = float4(Seen.rgb * (Seen.a * Gives) + Streak, (bRays || !bImage) ? 1.0f : 1.0f - F * Puddle);
    if (WetScreen.w > 2.5f && WetScreen.w < 3.5f)
        Out.Colour = float4(Seen.rgb * Seen.a, 0.0f);
    if (WetScreen.w > 3.5f && WetScreen.w < 4.5f)
        Out.Colour = float4(Seen.aaa, 0.0f);
    if (WetScreen.w > 4.5f && WetScreen.w < 5.5f)
        Out.Colour = float4(Streak, 0.0f);
#if RDX_LIGHT_TARGET
    Out.Lit   = float4(0.0f, 0.0f, 0.0f, 0.0f);
    Out.Added = Out.Colour;
#endif
    return Out;
}

// The fires' flames in the wet floors' mirror - this pass drawn again after the volumetric walk (FBackend::
// DrawVolumetrics), with the walk's picture of this frame's flames (half the scene's size, laid over the whole of it)
// in the picture's copy's place (t0). Read where the pixel's mirrored ray passes nearest a fire (the lamps marked as
// fires, WetLamp[..].a): the flame's own picture there, the same flame and moving as it does, upside down as the
// mirror turns it; hidden where something stands before the flame in the mirror (WetFirstMeet); by the water's
// Fresnel and as much as the floor gives back (Share).
PSOut PSWetFlames(float4 Pos : SV_Position)
{
    const int2  At   = int2(Pos.xy);
    const uint  Mark = WetMark.Load(int3(At.x * 2, At.y, 0));
    const float D    = DepthCopy.Load(int3(At, 0)).r;
    if (!WetMarkHolds(Mark, D))
        discard;
    const float4 Here   = float4(Pos.xy, D, 1.0f);
    const float3 P      = WetViewPlace(Here);
    const float3 Place  = WetLevelPlace(Here);
    const float  Wet    = WetLevelOf(WetPatch(Place.xy)) * WetCoverOf(Place);
    const float3 Up     = normalize(float3(WetView[1].z, WetView[2].z, WetView[3].z));
    const float  Puddle = (Mark & 63u) ? (float)(Mark & 63u) / 63.0f *
                                         WetEdgeKeep(P, Up, WetScreen.xy, max(WetScreen.z, 1.0f), float2(1.0f, 1.0f),
                                                     float2(0.0f, 0.0f))
                                       : 0.0f;
    const float3 E      = normalize(P);
    float3       Stone;
    const bool   bRelief = WetTiltNormal(WetMark.Load(int3(At.x * 2 + 1, At.y, 0)), Mark, Stone);
    float3 Water = Up;
    if (Puddle > 0.0f && Rays.y > 0.0f)
    {
        const float2 Off  = Place.xy - WetView[0].xy;
        const float  Foot = length(P) / (max(WetScreen.z, 1.0f) * max(-dot(E, Up), 0.01f));
        const float2 Ring = RainRingsAt(Place.xy, Rays.z, Foot, dot(Off, Off), Rays.y);
        const float3 L    = normalize(float3(-Ring, 1.0f));
        Water = normalize(float3(dot(L, WetView[1].xyz), dot(L, WetView[2].xyz), dot(L, WetView[3].xyz)));
    }
    const float3 N     = normalize(lerp(bRelief ? Stone : Up, Water, Puddle));
    const float3 R     = reflect(E, N);
    const float  F     = 0.02f + 0.98f * pow(1.0f - saturate(-dot(E, N)), 5.0f);
    const float  Share = lerp(bRelief ? Film.x * Wet : 0.0f, 1.0f, Puddle);
    // The fire the mirrored ray passes nearest, how far along the ray that is, and its glow's ball.
    float     T = -1.0f, Ball = 0.0f, Miss = 1e30f;
    float3    FireAt = float3(0.0f, 0.0f, 0.0f);
    const int Count = min((int)WetLampCount.x, WET_LAMP_MAX);
    [loop] for (int i = 0; i < Count; i++)
    {
        const float4 Lamp = WetLamp[i * 2];
        const float  t    = dot(Lamp.xyz - P, R);
        if (WetLamp[i * 2 + 1].a < 0.5f || t <= 0.0f)
            continue;
        const float Off = length(P + R * t - Lamp.xyz);
        if (Off < Miss)
        {
            Miss   = Off;
            T      = t;
            Ball   = Lamp.w;
            FireAt = Lamp.xyz;
        }
    }
    // Hidden where something stands before the flame in the mirror (WetFirstMeet): met below the flame's foot - the
    // bottom of its glow's ball, the top of what burns: the barrel round it - or well before it, more than two of the
    // ball's radii short of it: a crate in front. The fuel at the flame's own foot hides nothing of it. Walked only
    // where there is a flame to hide.
    const float Offset = frac(52.9829189f * frac(dot(Pos.xy, float2(0.06711056f, 0.00583715f))));
    const float Reach  = max(T - Ball, 1.0f);
    // A measurement (the wet floors' measuring view 6): red where no fire was found, green the walk's picture read
    // there four times as bright, blue where something was met before the flame.
    if (WetScreen.w > 5.5f && WetScreen.w < 6.5f)
    {
        PSOut M;
        const float2 Mu = WetUvOf(P + R * max(T, 1.0f));
        const float3 Ms = SceneCopy.SampleLevel(Samp, saturate(Mu), 0.0f).rgb;
        const float  Mm = (T > 0.0f) ? WetFirstMeet(P, R, Reach, Offset) : Reach;
        const bool   Mh = Mm < Reach && (dot(P + R * Mm - FireAt, Up) < -Ball || Mm < T - 2.0f * Ball);
        M.Colour = float4((T <= 0.0f) ? 1.0f : 0.0f, 4.0f * max(Ms.r, max(Ms.g, Ms.b)), Mh ? 1.0f : 0.0f, 0.0f);
#if RDX_LIGHT_TARGET
        M.Lit   = float4(0.0f, 0.0f, 0.0f, 0.0f);
        M.Added = M.Colour;
#endif
        return M;
    }
    if (T <= 0.0f)
        discard;
    const float3 Q = P + R * T;
    if (Q.z <= 1.0f)
        discard;
    const float2 Uv = WetUvOf(Q);
    if (any(Uv < 0.0f) || any(Uv > 1.0f))
        discard;
    const float3 Gives = SceneCopy.SampleLevel(Samp, Uv, 0.0f).rgb * (F * Share);
    if (max(Gives.r, max(Gives.g, Gives.b)) < 1.0f / 4096.0f)
        discard;
    const float Met = WetFirstMeet(P, R, Reach, Offset);
    if (Met < Reach && (dot(P + R * Met - FireAt, Up) < -Ball || Met < T - 2.0f * Ball))
        discard;
    PSOut Out;
    Out.Colour = float4(Gives, 1.0f);
#if RDX_LIGHT_TARGET
    Out.Lit   = float4(0.0f, 0.0f, 0.0f, 0.0f);
    Out.Added = Out.Colour;
#endif
    return Out;
}

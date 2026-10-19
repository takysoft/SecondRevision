#include "Pch.h"
#include "FieldVolume.h"
#include "Log.h"
#include "RenderMath.h"
#include "ViewGeneration.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"

// One triangle over the whole target; for each pixel the stretches of its ray within reach of the field's polygons,
// walked, and what the field gives off along them summed.
static const char* GFieldVolumeShader =
"cbuffer View : register(b0)\n"
"{\n"
"    float4 V0;    // the engine's screen centre, its projection, how many polygons the field has\n"
"    float4 V1;    // engine pixels to one of the target's, the level's clock, the field's half thickness\n"
"    float4 V2;    // the eye in the world\n"
"    float4 V3;    // the view's X axis in the world\n"
"    float4 V4;    // its Y\n"
"    float4 V5;    // its Z\n"
"    float4 V6;    // four numbers to spare\n"
"};\n"
"Texture2D<float>  Depth : register(t0);\n"
"Texture3D<float4> Noise : register(t1);\n"
"// The field's polygons, FIELD_ROWS rows each: the plane's normal with how many corners; a ball about the polygon -\n"
"// its middle from the eye, its radius with the half thickness; the hue of the field it is of, with how bright that\n"
"// field is laid on; then the corners, from the eye, in the world's axes, going round so that an edge crossed with\n"
"// the normal points out of the polygon.\n"
"StructuredBuffer<float4> Poly : register(t2);\n"
"SamplerState Wrap : register(s0);\n"
"#define FIELD_CORNERS 12\n"
"#define FIELD_ROWS    (FIELD_CORNERS + 3)\n"
"// One cell of the arcs' pattern, in the level's units; the noise repeats every FIELD_GRAIN places of its grid.\n"
"#define FIELD_CELL    12.8f\n"
"#define FIELD_GRAIN   64.0f\n"
"// How far apart along a ray the pattern is asked for at the nearest, and the most places a stretch of the field is\n"
"// asked at: a longer stretch is asked at places further apart, never cut short - a ray that runs along one of the\n"
"// field's surfaces still comes to the ones behind it. Between two places the pattern is taken as even, and what\n"
"// lies between is worked out, not looked for: an arc thinner than the gap is caught whole, wherever in it it lies.\n"
"#define FIELD_GAP     0.5f\n"
"#define FIELD_PLACES  32\n"
"// A spark: the side of the cells they light in and its radius, both in the level's units.\n"
"#define FIELD_SPARK_CELL 1.5f\n"
"#define FIELD_SPARK      0.14f\n"
"\n"
"float4 FieldVS(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 T = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(T * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
"}\n"
"\n"
"// Four numbers of no pattern at a place, each going smoothly through the level's space: one read of the noise.\n"
"float4 Grain(float3 P)\n"
"{\n"
"    const float3 I = floor(P);\n"
"    const float3 F = P - I;\n"
"    return Noise.SampleLevel(Wrap, (I + F * F * (3.0f - 2.0f * F) + 0.5f) / FIELD_GRAIN, 0.0f);\n"
"}\n"
"// The four numbers of one place of the noise's grid, as they are: the same for every place within the cell.\n"
"float4 Lot(float3 Cell)\n"
"{\n"
"    return Noise.SampleLevel(Wrap, (floor(Cell) + 0.5f) / FIELD_GRAIN, 0.0f);\n"
"}\n"
"// Four clouds at once - the noise's four numbers, each summed over four sizes: about its middle 0.47 on the whole.\n"
"float4 Cloud(float3 P)\n"
"{\n"
"    return Grain(P) * 0.5f + Grain(P * 2.03f + float3(17.7f, 9.3f, 5.1f)) * 0.25f +\n"
"           Grain(P * 4.09f + float3(3.1f, 27.9f, 13.3f)) * 0.125f +\n"
"           Grain(P * 8.23f + float3(41.3f, 7.7f, 19.9f)) * 0.0625f;\n"
"}\n"
"\n"
"// How far a place is from one polygon of the field: off its plane where the place is over the polygon, and to the\n"
"// polygon's nearest edge where it is beyond it - so the field is as thick past its ends as it is to its sides.\n"
"float FarFrom(uint K, float3 P)\n"
"{\n"
"    const uint   Row = K * FIELD_ROWS;\n"
"    const float4 Pl  = Poly[Row];\n"
"    const uint   N   = (uint)Pl.w;\n"
"    const float  S   = dot(Pl.xyz, P - Poly[Row + 3].xyz);\n"
"    const float3 Q   = P - Pl.xyz * S;\n"
"    bool  bOver = true;\n"
"    float Near2 = 1e30f;\n"
"    [loop] for (uint i = 0; i < N; i++)\n"
"    {\n"
"        const float3 A = Poly[Row + 3 + i].xyz;\n"
"        const float3 B = Poly[Row + 3 + ((i + 1 < N) ? i + 1 : 0)].xyz;\n"
"        const float3 E = B - A;\n"
"        const float3 D = Q - A;\n"
"        if (dot(cross(E, Pl.xyz), D) > 0.0f)\n"
"            bOver = false;\n"
"        const float3 R = D - E * saturate(dot(D, E) / max(dot(E, E), 1e-6f));\n"
"        Near2 = min(Near2, dot(R, R));\n"
"    }\n"
"    return bOver ? abs(S) : sqrt(S * S + Near2);\n"
"}\n"
"\n"
"// The arcs' pattern at a place of the level - the flat field's own (SolidPass.hlsl's FieldArcs), with the place's\n"
"// third number in it: a cloud folded by a slower one, all of it drifting. x: the wide sheet's number, y: the fine\n"
"// sheet's; z: the haze's cloud; w: the slow cloud a patch of the field swells by. Where a sheet's number crosses\n"
"// 0.47 there is an arc: in space that is a thin skin, which stands, leans and folds through the field.\n"
"float4 PatternAt(float3 World, float Time)\n"
"{\n"
"    const float3 W    = World / FIELD_CELL;\n"
"    const float4 Fold = Cloud(W * 0.6f + float3(0.0f, Time * 0.21f, Time * 0.13f));\n"
"    const float4 Wide = Cloud(W + (Fold.xyz - 0.47f) * 2.4f + float3(Time * 0.07f, 0.0f, Time * 0.05f));\n"
"    const float  T2   = Time * 1.7f + 3.0f;\n"
"    const float4 Fine = Cloud(W * 2.3f + 9.1f + (Fold.yzx - 0.47f) * 2.4f + float3(T2 * 0.07f, 0.0f, T2 * 0.05f));\n"
"    return float4(Wide.x, Fine.y, Wide.z, Fold.w);\n"
"}\n"
"\n"
"// What a stretch of a ray takes of a sheet's arcs, the sheet's number going from A at one end to B at the other and\n"
"// taken as even between: the arcs' two shapes, each as its mean over the stretch - x the core, y the glow. Worked\n"
"// out, not looked for at places along it: a core thinner than the stretch is long is caught whole.\n"
"float2 Across(float A, float B, float Sharp)\n"
"{\n"
"    const float Kc = 2.2f * Sharp;\n"
"    const float Kg = 0.45f * Sharp;\n"
"    A -= 0.47f;\n"
"    B -= 0.47f;\n"
"    const float Ca = saturate(1.0f - Kc * abs(A)), Cb = saturate(1.0f - Kc * abs(B));\n"
"    const float Ga = saturate(1.0f - Kg * abs(A)), Gb = saturate(1.0f - Kg * abs(B));\n"
"    const float Ca4 = Ca * Ca * Ca * Ca, Cb4 = Cb * Cb * Cb * Cb;\n"
"    const float Ga3 = Ga * Ga * Ga,      Gb3 = Gb * Gb * Gb;\n"
"    const float SumCa = ((A < 0.0f) ? Ca4 : 2.0f - Ca4) / (4.0f * Kc);\n"
"    const float SumCb = ((B < 0.0f) ? Cb4 : 2.0f - Cb4) / (4.0f * Kc);\n"
"    const float SumGa = ((A < 0.0f) ? Ga3 : 2.0f - Ga3) / (3.0f * Kg);\n"
"    const float SumGb = ((B < 0.0f) ? Gb3 : 2.0f - Gb3) / (3.0f * Kg);\n"
"    const float Span  = B - A;\n"
"    const float Mc    = saturate(1.0f - Kc * abs(0.5f * (A + B)));\n"
"    const float Mg    = saturate(1.0f - Kg * abs(0.5f * (A + B)));\n"
"    const bool  bFlat = abs(Span) < 1e-4f;\n"
"    const float Safe  = bFlat ? 1.0f : Span;\n"
"    return float2(bFlat ? Mc * Mc * Mc : (SumCb - SumCa) / Safe,\n"
"                  (bFlat ? Mg * Mg : (SumGb - SumGa) / Safe) * 0.3f);\n"
"}\n"
"\n"
"// One polygon's stretch of a ray: inside its ball and its slab, before the world's surface (Len along the ray).\n"
"// False where the ray does not come within the polygon's reach.\n"
"bool Reach(uint K, float3 Dir, float Half, float Len, out float A, out float B)\n"
"{\n"
"    A = 0.0f;\n"
"    B = 0.0f;\n"
"    const float4 Ball = Poly[K * FIELD_ROWS + 1];\n"
"    const float  Tc   = dot(Ball.xyz, Dir);\n"
"    const float  H2   = dot(Ball.xyz, Ball.xyz) - Tc * Tc;\n"
"    const float  R2   = Ball.w * Ball.w;\n"
"    if (H2 >= R2)\n"
"        return false;\n"
"    const float  Wide = sqrt(R2 - H2);\n"
"    const float3 Nrm  = Poly[K * FIELD_ROWS].xyz;\n"
"    const float  Dn   = dot(Nrm, Dir);\n"
"    const float  Off  = dot(Nrm, Poly[K * FIELD_ROWS + 3].xyz);\n"
"    A = Tc - Wide;\n"
"    B = Tc + Wide;\n"
"    if (abs(Dn) > 1e-5f)\n"
"    {\n"
"        const float Ta = (Off - Half) / Dn, Tb = (Off + Half) / Dn;\n"
"        A = max(A, min(Ta, Tb));\n"
"        B = min(B, max(Ta, Tb));\n"
"    }\n"
"    else if (abs(Off) > Half)\n"
"        return false;\n"
"    A = max(A, 0.0f);\n"
"    B = min(B, Len);\n"
"    return B > A;\n"
"}\n"
"// How far a place is from the nearest of the field's polygons, and which of them that is. Every one of them is\n"
"// asked, however many there are; one whose ball the place is outside of is passed over.\n"
"float Nearest(float3 P, uint Count, out uint Who)\n"
"{\n"
"    float Far = 1e9f;\n"
"    Who = 0;\n"
"    [loop] for (uint j = 0; j < Count; j++)\n"
"    {\n"
"        const float4 Ball = Poly[j * FIELD_ROWS + 1];\n"
"        const float3 D    = P - Ball.xyz;\n"
"        if (dot(D, D) < Ball.w * Ball.w)\n"
"        {\n"
"            const float One = FarFrom(j, P);\n"
"            if (One < Far)\n"
"            {\n"
"                Far = One;\n"
"                Who = j;\n"
"            }\n"
"        }\n"
"    }\n"
"    return Far;\n"
"}\n"
"\n"
"float4 FieldPS(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    const float  d     = Depth.Load(int3(Pos.xy, 0));\n"
"    const float2 Eng   = Pos.xy * V1.xy;\n"
"    const float3 View  = float3((Eng - V0.xy) / max(V0.z, 1e-3f), 1.0f);\n"
"    const float  Scale = length(View);\n"
"    const float3 Dir   = (V3.xyz * View.x + V4.xyz * View.y + V5.xyz) / Scale;\n"
"    // How far along the ray the world's surface is; where nothing is drawn the ray has no end.\n"
"    const float  Len   = (d > 1e-6f) ? Scale / d : 1e9f;\n"
"    const float  Half  = V1.w;\n"
"    const float  Time  = V1.z;\n"
"    const uint   Count = (uint)V0.w;\n"
"    // Where along a gap the places fall is this pixel's own, and another every frame: what little the evenness\n"
"    // between two places leaves out is then a fine grain, never a line that runs across pixels.\n"
"    const float Shift = frac(52.9829189f * frac(0.06711056f * Pos.x + 0.00583715f * Pos.y) + Time * 7.31f);\n"
"    // A pixel's width at a unit of the ray, for the sparks: none is drawn under a pixel wide.\n"
"    const float Foot  = V1.x / max(V0.z, 1e-3f);\n"
"    float  Core = 0.0f, Glow = 0.0f, Haze = 0.0f, Bolt = 0.0f, Spark = 0.0f;\n"
"    // The hue is each polygon's own, and so is how bright its field is laid on. A place has its nearest polygon's,\n"
"    // and what the ray takes of the field there takes them with it: where two fields of different looks meet, the\n"
"    // one goes over into the other.\n"
"    float4 Tone = 0.0f, SparkTone = 0.0f;\n"
"    float  Weigh = 0.0f;\n"
"    // The stretches of the ray within reach of the field's polygons, in order along the ray - every one of them,\n"
"    // however many: the next is found among all the polygons each time, and nothing is kept in a list that could be\n"
"    // full. Stretches that run into one another are gone through as one, so a place of the level is gone through\n"
"    // once, however many of the field's polygons it is near.\n"
"    float  At = 0.0f, End = -1.0f, LastA = -1.0f;\n"
"    uint   LastK = 0;\n"
"    bool   bOpen = false, bAny = false, bMore = true;\n"
"    [loop] for (uint s = 0; s <= Count && bMore; s++)\n"
"    {\n"
"        float NextA = 1e30f, NextB = 0.0f;\n"
"        uint  NextK = 0;\n"
"        bool  bFound = false;\n"
"        [loop] for (uint k = 0; k < Count; k++)\n"
"        {\n"
"            float A, B;\n"
"            if (!Reach(k, Dir, Half, Len, A, B))\n"
"                continue;\n"
"            // Past the last one taken - by where it begins, then by its number - and before the best so far.\n"
"            const bool bPast = !bAny || A > LastA || (A == LastA && k > LastK);\n"
"            if (bPast && (!bFound || A < NextA))\n"
"            {\n"
"                NextA  = A;\n"
"                NextB  = B;\n"
"                NextK  = k;\n"
"                bFound = true;\n"
"            }\n"
"        }\n"
"        if (bFound)\n"
"        {\n"
"            bAny  = true;\n"
"            LastA = NextA;\n"
"            LastK = NextK;\n"
"            if (bOpen && NextA <= End)\n"
"            {\n"
"                End = max(End, NextB);\n"
"                continue;\n"
"            }\n"
"        }\n"
"        bMore = bFound;\n"
"        // The stretch that is open ends here: the next one begins past it, or there is none.\n"
"        const float Stop = bOpen ? End : At;\n"
"        const float Gap  = max(FIELD_GAP, (End - At) / (float)(FIELD_PLACES - 2));\n"
"        float  WasT = At;\n"
"        float4 Was  = 0.0f;\n"
"        float  WasStrong = 0.0f, WasBolt = 0.0f, WasKey = -1.0f;\n"
"        float3 WasCell = -1e9f;\n"
"        float4 WasHue  = 0.0f;\n"
"        [loop] for (int i = 0; i < FIELD_PLACES && Stop > At; i++)\n"
"        {\n"
"            const float  T = (i == 0) ? At : min(At + ((float)i - 1.0f + Shift) * Gap, Stop);\n"
"            const float3 P = Dir * T;\n"
"            uint Who;\n"
"            const float  Far = Nearest(P, Count, Who);\n"
"            // Full on the surface and nothing at the half thickness, along a quarter of a sine wave between.\n"
"            const float  Strong = cos(saturate(Far / Half) * 1.5707963f);\n"
"            const float4 Hue    = Poly[Who * FIELD_ROWS + 2];\n"
"            const float3 World  = V2.xyz + P;\n"
"            const float4 Now    = PatternAt(World, Time);\n"
"            // The discharges: a part of the field strikes a bolt of its own now and then, each part at its own\n"
"            // times - a line of its own, sharper, gone within a ninth of a second.\n"
"            const float3 Part   = World / FIELD_CELL * 0.6f;\n"
"            const float4 Own    = Lot(Part);\n"
"            const float  Beat   = Time * 9.0f + Own.r * 9.0f;\n"
"            const float  Struck = step(0.97f, Lot(Part + floor(Beat) * float3(13.0f, 7.0f, 5.0f)).g);\n"
"            const float  Key    = Struck * (1.0f + floor(Beat) + dot(floor(Part), float3(1.0f, 57.0f, 113.0f)));\n"
"            float NowBolt = 0.0f;\n"
"            [branch] if (Struck > 0.5f)\n"
"                NowBolt = Cloud(World / FIELD_CELL * 1.5f + floor(Beat) * 13.7f).w;\n"
"            if (i > 0 && T > WasT)\n"
"            {\n"
"                const float  Long = T - WasT;\n"
"                const float  Mid  = 0.5f * (Strong + WasStrong);\n"
"                const float  Rise = 0.5f * (Now.w + Was.w);\n"
"                const float  Live = 0.7f + 1.6f * Rise * Rise * Rise;\n"
"                const float2 Sw   = Across(Was.x, Now.x, 14.0f);\n"
"                const float2 Sf   = Across(Was.y, Now.y, 10.0f);\n"
"                Core += (Sw.x + 0.55f * Sf.x) * Live * Mid * Long;\n"
"                Glow += (Sw.y + 0.55f * Sf.y) * Live * Mid * Long;\n"
"                Haze += (0.09f + 0.09f * 0.5f * (Now.z + Was.z)) * Mid * Long;\n"
"                Tone  += 0.5f * (Hue * Strong + WasHue * WasStrong) * Long;\n"
"                Weigh += Mid * Long;\n"
"                [branch] if (Key > 0.5f && Key == WasKey)\n"
"                    Bolt += Across(WasBolt, NowBolt, 24.0f).x * (1.0f - frac(Beat)) * 2.2f * Mid * Long;\n"
"            }\n"
"            // The sparks: the level is cut into small cells, and now and then one lights a spark at a place of its\n"
"            // own within it, which flares and dies - each cell at its own times. A cell the ray goes through is\n"
"            // asked once; how near the ray passes the spark is worked out, not looked for.\n"
"            const float3 Cell = floor(World / FIELD_SPARK_CELL);\n"
"            [branch] if (any(Cell != WasCell))\n"
"            {\n"
"                const float4 Mine = Lot(Cell + 0.5f);\n"
"                const float  Tick = Time * 1.3f + Mine.a * 10.0f;\n"
"                const float4 Lit  = Lot(Cell + 0.5f + floor(Tick) * float3(7.0f, 3.0f, 11.0f));\n"
"                [branch] if (Lit.a > 0.992f)\n"
"                {\n"
"                    const float3 C    = (Cell + 0.25f + 0.5f * Lit.rgb) * FIELD_SPARK_CELL - V2.xyz;\n"
"                    const float  Tc   = dot(C, Dir);\n"
"                    const float  Rad  = max(FIELD_SPARK, Foot * Tc);\n"
"                    const float  D2   = max(dot(C, C) - Tc * Tc, 0.0f) / (Rad * Rad);\n"
"                    uint SparkWho;\n"
"                    const float  SparkFar = Nearest(C, Count, SparkWho);\n"
"                    const float  Age   = frac(Tick);\n"
"                    const float  Flare = exp(-D2) * cos(saturate(SparkFar / Half) * 1.5707963f) *\n"
"                                         smoothstep(0.0f, 0.2f, Age) * (1.0f - smoothstep(0.3f, 1.0f, Age)) *\n"
"                                         ((Tc > 0.0f && Tc < Len) ? 1.0f : 0.0f);\n"
"                    Spark     += Flare;\n"
"                    SparkTone += Poly[SparkWho * FIELD_ROWS + 2] * Flare;\n"
"                }\n"
"            }\n"
"            WasCell   = Cell;\n"
"            Was       = Now;\n"
"            WasStrong = Strong;\n"
"            WasHue    = Hue;\n"
"            WasBolt   = NowBolt;\n"
"            WasKey    = Key;\n"
"            WasT      = T;\n"
"            if (T >= Stop)\n"
"                break;\n"
"        }\n"
"        At    = NextA;\n"
"        End   = NextB;\n"
"        bOpen = bFound;\n"
"    }\n"
"    if (!bAny)\n"
"        discard;\n"
"    // Held against what a ray straight through the field's middle sums the strength to: an arc seen face on is then\n"
"    // as bright as the flat field's. The glow and the haze fill up towards a most, they do not pile up without end\n"
"    // along a ray that runs the field's length.\n"
"    const float Full  = Half * 1.2732395f;\n"
"    const float Arc   = min((Core + Bolt) / Full * 1.5f, 1.6f);\n"
"    const float Soft  = 0.6f * (1.0f - exp(-(Glow / Full) / 0.6f));\n"
"    const float Mist  = 0.45f * (1.0f - exp(-(Haze / Full) / 0.45f));\n"
"    const float4 Tint  = Tone / max(Weigh, 1e-6f);\n"
"    const float4 Flash = SparkTone / max(Spark, 1e-6f);\n"
"    const float3 Sum   = (Tint.rgb * (Mist + Soft + 0.5f * Arc) + 0.75f * Arc) * Tint.a +\n"
"                         (0.6f * Flash.rgb + 0.8f) * min(Spark * 1.5f, 2.0f) * Flash.a;\n"
"    return float4(Sum, 0.0f);\n"
"}\n";

UBOOL FFieldVolume::Init(ID3D12Device* Device)
{
    if (!Device)
        return 0;
    if (IsReady())
        return 1;

    D3D12_DESCRIPTOR_RANGE R;
    appMemzero(&R, sizeof(R));
    R.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    R.NumDescriptors     = 2;
    R.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER P[3];
    appMemzero(P, sizeof(P));
    P[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P[0].Constants.ShaderRegister = 0;
    P[0].Constants.Num32BitValues = 28;
    P[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
    P[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[1].Descriptor.ShaderRegister = 2;             // t2: the polygons, straight from the root signature
    P[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    P[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    P[2].DescriptorTable.NumDescriptorRanges = 1;
    P[2].DescriptorTable.pDescriptorRanges   = &R;
    P[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // The noise is read smoothly and goes round: it repeats through the level.
    D3D12_STATIC_SAMPLER_DESC S;
    appMemzero(&S, sizeof(S));
    S.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    S.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    S.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    S.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    S.MaxLOD           = D3D12_FLOAT32_MAX;
    S.ShaderRegister   = 0;
    S.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 3;
    Desc.pParameters       = P;
    Desc.NumStaticSamplers = 1;
    Desc.pStaticSamplers   = &S;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("field volume: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        Shutdown();
        return 0;
    }

    const size_t Len = strlen(GFieldVolumeShader);
    if (FAILED(RdxCompile(GFieldVolumeShader, Len, "FieldVolume", nullptr, nullptr, "FieldVS", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GFieldVolumeShader, Len, "FieldVolume", nullptr, nullptr, "FieldPS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("field volume: the shaders failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        Shutdown();
        return 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC Hd;
    appMemzero(&Hd, sizeof(Hd));
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    Hd.NumDescriptors = 2;
    Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    UBOOL bMade = SUCCEEDED(Device->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), Heap.VoidSlot()));
    HeapStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The noise: a cube of four numbers a place, each a number of no pattern - which the card reads smoothly between.
    D3D12_HEAP_PROPERTIES Card, Up;
    appMemzero(&Card, sizeof(Card));
    appMemzero(&Up, sizeof(Up));
    Card.Type = D3D12_HEAP_TYPE_DEFAULT;
    Up.Type   = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Td;
    appMemzero(&Td, sizeof(Td));
    Td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    Td.Width            = NOISE_SIDE;
    Td.Height           = NOISE_SIDE;
    Td.DepthOrArraySize = NOISE_SIDE;
    Td.MipLevels        = 1;
    Td.Format           = DXGI_FORMAT_R16G16B16A16_UNORM;
    Td.SampleDesc.Count = 1;
    UINT64 Bytes = 0;
    UINT   RowsOf = 0;
    UINT64 RowBytes = 0;
    Device->GetCopyableFootprints(&Td, 0, 1, 0, &NoiseFoot, &RowsOf, &RowBytes, &Bytes);
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = Bytes;
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RANGE None = { 0, 0 };
    void*       At   = nullptr;
    bMade = bMade && Bytes > 0 && !RdxForcedFailure(TEXT("field noise")) &&
            SUCCEEDED(Device->CreateCommittedResource(&Card, D3D12_HEAP_FLAG_NONE, &Td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr, __uuidof(ID3D12Resource), Noise.VoidSlot())) &&
            SUCCEEDED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, __uuidof(ID3D12Resource), NoiseUpload.VoidSlot())) &&
            SUCCEEDED(NoiseUpload->Map(0, &None, &At)) && At;
    if (bMade)
    {
        for (INT z = 0; z < NOISE_SIDE; z++)
            for (INT y = 0; y < NOISE_SIDE; y++)
            {
                unsigned short* const Row = (unsigned short*)((BYTE*)At + NoiseFoot.Offset +
                                                              ((UINT64)z * RowsOf + y) * NoiseFoot.Footprint.RowPitch);
                for (INT x = 0; x < NOISE_SIDE; x++)
                    for (INT c = 0; c < 4; c++)
                        Row[x * 4 + c] = RdxMath::FieldNoiseAt(x, y, z, c);
            }
        NoiseUpload->Unmap(0, nullptr);
        D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
        appMemzero(&Sd, sizeof(Sd));
        Sd.Format                  = DXGI_FORMAT_R16G16B16A16_UNORM;
        Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE3D;
        Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Sd.Texture3D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE To = Heap->GetCPUDescriptorHandleForHeapStart();
        To.ptr += (UINT64)HeapStep;
        Device->CreateShaderResourceView(Noise.Get(), &Sd, To);
        bNoiseToCopy = 1;
    }
    for (INT i = 0; i < FRAMES && bMade; i++)
        bMade = HoldPolygons(Device, i, FIRST_POLYGONS);
    if (!bMade)
    {
        RdxLog(TEXT("field volume: its heap, its noise or its polygons' lists could not be made"));
        Shutdown();
        return 0;
    }
    return 1;
}

// A frame slot's list made long enough for Wanted polygons, twice its room at least. False where the device refuses;
// the list is then as it was.
UBOOL FFieldVolume::HoldPolygons(ID3D12Device* Device, INT Slot, INT Wanted)
{
    if (Wanted <= PolyRoom[Slot])
        return 1;
    if (!Device)
        return 0;
    const INT Larger = Max(Wanted, PolyRoom[Slot] * 2);
    D3D12_HEAP_PROPERTIES Up;
    appMemzero(&Up, sizeof(Up));
    Up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = (UINT64)Larger * ROWS * 4 * sizeof(FLOAT);
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    TComRef<ID3D12Resource> Made;
    void*                   MadeAt = nullptr;
    D3D12_RANGE             None   = { 0, 0 };
    if (RdxForcedFailure(TEXT("field polygons")) ||
        FAILED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, __uuidof(ID3D12Resource), Made.VoidSlot())) ||
        FAILED(Made->Map(0, &None, &MadeAt)) || !MadeAt)
    {
        RdxLog(TEXT("field volume: the polygons' list could not be made for %i (it holds %i)"), Larger,
               PolyRoom[Slot]);
        return 0;
    }
    // The shorter one is let go: the slot's last frame is done on the card by the time a frame records into it.
    if (PolyUpload[Slot] && PolyMapped[Slot])
        PolyUpload[Slot]->Unmap(0, nullptr);
    PolyUpload[Slot].Release();
    *PolyUpload[Slot].Slot() = Made.Get();
    Made->AddRef();
    PolyMapped[Slot] = MadeAt;
    PolyRoom[Slot]   = Larger;
    return 1;
}

UBOOL FFieldVolume::EnsurePso(ID3D12Device* Device, DXGI_FORMAT SceneFormat)
{
    if (Pso && PsoFormat == SceneFormat)
        return 1;
    Pso.Release();
    PsoFormat = DXGI_FORMAT_UNKNOWN;
    if (!Device || !Root || !Vs || !Ps || SceneFormat == DXGI_FORMAT_UNKNOWN)
        return 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC G;
    appMemzero(&G, sizeof(G));
    G.pRootSignature        = Root.Get();
    G.VS.pShaderBytecode    = Vs->GetBufferPointer();
    G.VS.BytecodeLength     = Vs->GetBufferSize();
    G.PS.pShaderBytecode    = Ps->GetBufferPointer();
    G.PS.BytecodeLength     = Ps->GetBufferSize();
    G.SampleMask            = UINT_MAX;
    G.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    G.NumRenderTargets      = 1;
    G.RTVFormats[0]         = SceneFormat;
    G.SampleDesc.Count      = 1;
    G.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    G.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    G.RasterizerState.DepthClipEnable = FALSE;
    G.DepthStencilState.DepthEnable   = FALSE;
    G.DepthStencilState.StencilEnable = FALSE;
    // As the engine lays a translucent surface: the field's light + target * (1 - light).
    D3D12_RENDER_TARGET_BLEND_DESC& B = G.BlendState.RenderTarget[0];
    B.BlendEnable           = TRUE;
    B.SrcBlend              = D3D12_BLEND_ONE;
    B.DestBlend             = D3D12_BLEND_INV_SRC_COLOR;
    B.BlendOp               = D3D12_BLEND_OP_ADD;
    B.SrcBlendAlpha         = D3D12_BLEND_ZERO;
    B.DestBlendAlpha        = D3D12_BLEND_ONE;
    B.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    B.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                              D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(Device->CreateGraphicsPipelineState(&G, __uuidof(ID3D12PipelineState), Pso.VoidSlot())))
    {
        RdxLog(TEXT("field volume: the pipeline could not be made"));
        return 0;
    }
    PsoFormat = SceneFormat;
    return 1;
}

void FFieldVolume::Shutdown()
{
    for (INT i = 0; i < FRAMES; i++)
    {
        if (PolyUpload[i] && PolyMapped[i])
            PolyUpload[i]->Unmap(0, nullptr);
        PolyMapped[i] = nullptr;
        PolyUpload[i].Release();
        PolyRoom[i] = 0;
    }
    Noise.Release();
    NoiseUpload.Release();
    bNoiseToCopy = 0;
    Pso.Release();
    PsoFormat = DXGI_FORMAT_UNKNOWN;
    Vs.Release();
    Ps.Release();
    Heap.Release();
    Root.Release();
    DepthSeen   = nullptr;
    DepthFormat = DXGI_FORMAT_UNKNOWN;
}

UBOOL FFieldVolume::Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, DXGI_FORMAT SceneFormat,
                           ID3D12Resource* Depth, DXGI_FORMAT DepthRead, const FView& View, const FLOAT* Rows,
                           INT Count, FLOAT Half, FLOAT Time, INT FrameIndex)
{
    if (!Cmd || !Device || !IsReady() || !Depth || !Rows || Count <= 0 || !EnsurePso(Device, SceneFormat))
        return 0;
    if (bNoiseToCopy)
    {
        D3D12_TEXTURE_COPY_LOCATION Dst, Src;
        appMemzero(&Dst, sizeof(Dst));
        appMemzero(&Src, sizeof(Src));
        Dst.pResource       = Noise.Get();
        Dst.Type            = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.pResource       = NoiseUpload.Get();
        Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint = NoiseFoot;
        Cmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        D3D12_RESOURCE_BARRIER B;
        appMemzero(&B, sizeof(B));
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Noise.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &B);
        bNoiseToCopy = 0;
    }
    // A texture whose view the heap holds was made again since: the view anew (ViewGeneration.h).
    if (ViewsAt != RdxViewGeneration() || DepthSeen != Depth || DepthFormat != DepthRead)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
        appMemzero(&Sd, sizeof(Sd));
        Sd.Format                  = DepthRead;
        Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Sd.Texture2D.MipLevels     = 1;
        Device->CreateShaderResourceView(Depth, &Sd, Heap->GetCPUDescriptorHandleForHeapStart());
        ViewsAt     = RdxViewGeneration();
        DepthSeen   = Depth;
        DepthFormat = DepthRead;
    }
    const INT Slot = ((FrameIndex % FRAMES) + FRAMES) % FRAMES;
    if (!HoldPolygons(Device, Slot, Count))
        return 0;
    appMemcpy(PolyMapped[Slot], Rows, (SIZE_T)Count * ROWS * 4 * sizeof(FLOAT));

    FLOAT C[28];
    appMemzero(C, sizeof(C));
    C[0] = View.Screen[0]; C[1] = View.Screen[1]; C[2] = View.Focal; C[3] = (FLOAT)Count;
    C[4] = View.EngPerPixel[0]; C[5] = View.EngPerPixel[1]; C[6] = Time; C[7] = Half;
    for (INT k = 0; k < 3; k++)
    {
        C[8 + k]  = View.Eye[k];
        C[12 + k] = View.AxisX[k];
        C[16 + k] = View.AxisY[k];
        C[20 + k] = View.AxisZ[k];
    }

    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(Root.Get());
    Cmd->SetPipelineState(Pso.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, 28, C, 0);
    Cmd->SetGraphicsRootShaderResourceView(1, PolyUpload[Slot]->GetGPUVirtualAddress());
    Cmd->SetGraphicsRootDescriptorTable(2, Heap->GetGPUDescriptorHandleForHeapStart());
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
    return 1;
}

#include "Pch.h"
#include "Volumetrics.h"
#include "Log.h"
#include "RenderMath.h"
#include "ViewGeneration.h"

#include <stdio.h>                  // sprintf_s: RenderMath's numbers as the shader's macros

#include <d3dcompiler.h>
#include "ShaderCache.h"

// One walk a pixel over the puffs its ray can meet. The puffs and the tiles' lists arrive as plain buffers read
// straight from the root signature, so this pass needs a descriptor heap for two things only: the scene's depth and
// its own result.
static const char* GVolumetricShader =
"// A puff: where it stands (view space), how far it reaches across and up, the colour its picture is painted in, and\n"
"// how thick its middle is by the unit of the level - FBackend::FCloudPuff, twelve floats.\n"
"struct FPuff { float3 Centre; float Radius; float Tall; float3 Colour; float Dense; float3 Spare; };\n"
"StructuredBuffer<FPuff> Puffs : register(t0);\n"
"// Which puffs a pixel's ray can meet, by tile of this target (RDX_CLOUD_TILE pixels a side, TileCols across): two\n"
"// numbers a tile - where its list starts and how many it holds - then the lists, each a puff's place in Puffs.\n"
"StructuredBuffer<uint>  Tiles : register(t6);\n"
"// The waterfalls' columns (FVolumetrics::FFall), in the WORLD: Place is the centre on the level (xy) and how far\n"
"// the water reaches down and up (z, w); Across the cross-section - the direction across (xy) and the half-widths\n"
"// along it and across it (z, w); Colour the water's own light, and in w whether it comes down into water; Foam the\n"
"// foam's light around its foot, and in w how strongly the column glints.\n"
"struct FFall { float4 Place; float4 Across; float4 Colour; float4 Foam; };\n"
"StructuredBuffer<FFall> Falls : register(t4);\n"
"// The fires' flame bodies (FVolumetrics::FFire), in the WORLD: Place is the middle on the level (xy) and the foot and\n"
"// the top of the sheets they were made from (z, w); Axes the direction along the body (xy) and its half-length along\n"
"// it and half-width across it (z, w); Colour the flame's own tint (rgb) and a seed (w), so two fires do not flicker as\n"
"// one - of a ball of glow, which has no seed, the game's own fade of it (w).\n"
"// Shape: x which of the game's flames it is - 0 a fire standing on its foot, 1 a fireball of a burning body, 2 a\n"
"// lick trailing one, or a small flame that is one tongue - each walked in the shape of the game's own picture of\n"
"// it; y how bright (the game's fade of its card); z how many times stronger it glows by the unit (a small\n"
"// tongue's); w how many times coarser a large fire's grain is - of a body of glow, how far its densest part may\n"
"// turn towards white, of 1.\n"
"struct FFire { float4 Place; float4 Axes; float4 Colour; float4 Shape; };\n"
"StructuredBuffer<FFire> Fires : register(t5);\n"
"// Which fires a pixel's ray can meet, by tile of this target as the puffs are (Tiles): two numbers a tile, then the\n"
"// lists, each a body's place in Fires. A body is listed wherever the ball that holds all of its light can show\n"
"// (RdxMath::FlameBodyBall) - a standing fire's sparks with it.\n"
"StructuredBuffer<uint>  FireTiles : register(t7);\n"
"Texture2D<float4>       Depth : register(t1);\n"
"// The lamps the shafts are drawn from, two rows each: (xyz the lamp in VIEW space, w its reach), and\n"
"// (rgb its hue times its brightness, w which cube of the shadow array is its).\n"
"StructuredBuffer<float4> Lamps : register(t2);\n"
"// Which lamps a pixel's ray can meet, by tile of this target as the puffs are (Tiles): two numbers a tile, then the\n"
"// lists, each a lamp's place in Lamps - a lamp wherever the ball of its reach can show.\n"
"StructuredBuffer<uint>   LampTiles : register(t8);\n"
"// The level's own shadow cubes (FShadowMaps). Only the level's: a shaft is cut by walls and doorways, and\n"
"// asking the actors' cubes as well would double the cost for a person-shaped hole nobody looks for.\n"
"TextureCubeArray<float>  Walls : register(t3);\n"
"SamplerComparisonState   Cmp   : register(s0);\n"
"// The weapon lamps (Flashlight=Everybody), whose light the air of their cones is seen in (OwnBeams): their rows as\n"
"// the world's passes read them (SolidPass.hlsl's OwnRow: the head, then each lamp's seven, in the WORLD), and their\n"
"// maps (FShadowMaps::RenderSpots), what each lamp sees of the level - a slice each.\n"
"StructuredBuffer<float4> OwnRows : register(t9);\n"
"Texture2DArray<float>    OwnMaps : register(t10);\n"
"RWTexture2D<float4>     Out   : register(u0);\n"
"// The temporal upscaler's reactive mask, at the scene's full size (ReactiveBits): where a fire burns, the frames\n"
"// before are trusted less - a flame has no motion vectors, and its tongues are somewhere else every frame; the\n"
"// same for the smoke and the waterfalls' columns, by the share asked of them. Read as well as written, to keep\n"
"// the larger of this and what the water and the splashes put there first; that read is a typed load of an 8-bit\n"
"// format, which not every device has (RDX_TYPED_LOAD), and without it the mask is left alone.\n"
"#if RDX_TYPED_LOAD\n"
"RWTexture2D<unorm float> Reactive : register(u1);\n"
"#endif\n"
"// How much the upscaler is told: of a fire, some over the whole flame body where the ray crosses it, more where\n"
"// it glows; of the smoke and of a column, their share of how much of the pixel they hide. Never all of a pixel.\n"
"#define FIRE_REACTIVE_BODY 0.5f\n"
"#define FIRE_REACTIVE_GAIN 2.0f\n"
"#define REACTIVE_MAX       0.9f\n"
"// ReactiveBits: bit 0, the mask is there to write; bits 8-15 the smoke's share of its cover and bits 16-23 the\n"
"// columns', each in 255ths; bit 24, the standing fires throw sparks (SPARKS_ON).\n"
"cbuffer C : register(b0)\n"
"{\n"
"    uint2  Size;  uint PuffCount; uint Steps;\n"
"    float2 Screen; float Focal;   float Time;\n"
"    float2 EngPerPixel; float Grain; uint TileCols;\n"
"    float3 EyeWorld; float Ambient;\n"
"    float3 AxisX; float Rise;\n"
"    float3 AxisY; float Warp;\n"
"    float3 AxisZ; float SelfShade;\n"
"    float3 LampView; float LampReach;\n"
"    float3 LampHue;  float LampLit;\n"
"    float  StepWorld; float ShaftStrength; uint ShaftCount; uint ShaftSteps;\n"
"    float  ShaftNear; float ShaftMaxZ; float ShaftBias; float ShaftG;\n"
"    uint   FallCount; float FallDensity; float FallAmbient; float FallRate;\n"
"    uint   FireCount; float FireGlow; float FireRate; uint ReactiveBits;\n"
"    float4 NearPlane;   // a mirror's view: the mirror's plane, its normal and how far along it; zero in any other\n"
"    uint4  Window;      // the part of the target the view shows in: left, top, past the right, past the bottom\n"
"    uint   OwnCount; float OwnDust; float OwnForward; float OwnSpare;    // the weapon lamps' beams (OwnBeams)\n"
"};\n"
"// Where along a pixel's ray its walks begin, in view depth: at the eye - in a mirror's view at the mirror, whose\n"
"// eye stands behind it, in whatever is there (WalkCS sets it for the pixel).\n"
"static float RayFrom = 0.0f;\n"
"// The pixel's ray in the world, by the unit of view depth, how long that is, and how far along the ray the\n"
"// picture's own surface lies (WalkCS sets them for the pixel).\n"
"static float3 RayWorld = float3(0.0f, 0.0f, 1.0f);\n"
"static float  RayLong  = 1.0f;\n"
"static float  RayStop  = 1e9f;\n"
"// How much of a ball of glow shows on the pixel's ray - its middle at C, reaching Reach out from it the thinnest\n"
"// way. A ball is not cut by what the picture shows inside it - the wall it was thrown against, a relief's ridges:\n"
"// densest at its middle, it would end on them in a bright edge that follows every ridge. It shows whole while that\n"
"// surface lies behind its middle, or before it by less than half the ball's reach - a relief's ridges on the wall\n"
"// it lies on leave no stripes in it -, and thins out from there, to nothing where the surface hides all of it.\n"
"float GlowShows(float3 C, float Reach)\n"
"{\n"
"    return saturate(2.0f + 2.0f * (RayStop - dot(C - EyeWorld, RayWorld) / (RayLong * RayLong)) * RayLong / Reach);\n"
"}\n"
"\n"
"float Hash(float3 P)\n"
"{\n"
"    P = frac(P * 0.3183099f + float3(0.1f, 0.2f, 0.3f));\n"
"    P *= 17.0f;\n"
"    return frac(P.x * P.y * P.z * (P.x + P.y + P.z));\n"
"}\n"
"float Noise(float3 P)\n"
"{\n"
"    float3 I = floor(P), F = P - I;\n"
"    float3 W = F * F * (3.0f - 2.0f * F);\n"
"    float A = lerp(Hash(I + float3(0,0,0)), Hash(I + float3(1,0,0)), W.x);\n"
"    float B = lerp(Hash(I + float3(0,1,0)), Hash(I + float3(1,1,0)), W.x);\n"
"    float C = lerp(Hash(I + float3(0,0,1)), Hash(I + float3(1,0,1)), W.x);\n"
"    float D = lerp(Hash(I + float3(0,1,1)), Hash(I + float3(1,1,1)), W.x);\n"
"    return lerp(lerp(A, B, W.y), lerp(C, D, W.y), W.z);\n"
"}\n"
"// The view's axes in the world. The noise lives in WORLD terms and not in the view's: fixed to the screen, every\n"
"// wisp slides as the head turns, and that slide is what gives a flat card away in the first place.\n"
"float3 ToWorld(float3 V) { return AxisX * V.x + AxisY * V.y + AxisZ * V.z; }\n"
"\n"
"// How thick the smoke is at one point of the ray, in three parts.\n"
"//\n"
"// The SHAPE is the puffs themselves: each one a small ball, flattened or stretched as the card it came from was,\n"
"// and where two overlap their thicknesses ADD. A column of smoke out of a vent is a stack of them, which is how\n"
"// the game builds it - so the column comes out a column without anything here knowing what a column is.\n"
"//\n"
"// The noise is read at the sample's place IN THE WORLD, pushed downwards with time: the pattern then stands still\n"
"// while the smoke climbs through it, and the climb is what the eye reads as rising. A second, much slower noise\n"
"// bends WHERE the first is read, and that bend is the difference between smoke that curls and a pattern that\n"
"// merely slides across.\n"
"//\n"
"// How much puff there is at a point, by the unit of the level, and what colour. NO noise: this one is asked for\n"
"// a second time towards the lamp, where only how MUCH smoke is in the way matters, and the noise would double the\n"
"// cost of that answer. Only the puffs of the pixel's own tile are asked (First, Count): its list holds every puff\n"
"// the pixel's ray can meet, and every one within the self-shadow's step of that ray.\n"
"//\n"
"// Each puff brings its own thickness (FPuff::Dense): a wisp and a cloud are as thick through their middles, so by\n"
"// the unit the wisp is the denser. Size is the size of the smoke at the point - the puffs' own radii, weighed by\n"
"// how much each gives there - for the grain it is carved with and the step its shadow is looked for at.\n"
"float PuffAt(float3 P, uint First, uint Count, out float3 Tint, out float Size)\n"
"{\n"
"    Tint = float3(0.0f, 0.0f, 0.0f);\n"
"    Size = 0.0f;\n"
"    float Sum  = 0.0f;\n"
"    float Most = 0.0f;          // the thickest of the puffs here: what the limit below is held against\n"
"    [loop] for (uint k = 0u; k < Count; k++)\n"
"    {\n"
"        FPuff Q = Puffs[Tiles[First + k]];\n"
"        float3 D = ToWorld(P - Q.Centre);\n"
"        float r = length(D.xy) / max(Q.Radius, 1e-3f);\n"
"        float h = D.z / max(Q.Tall, 1e-3f);\n"
"        float R = r * r + h * h;\n"
"        if (R < 1.0f)\n"
"        {\n"
"            float Here = (1.0f - R) * (1.0f - R) * Q.Dense;\n"
"            Sum  += Here;\n"
"            Tint += Q.Colour * Here;\n"
"            Size += min(Q.Radius, Q.Tall) * Here;\n"
"            Most  = max(Most, Q.Dense);\n"
"        }\n"
"    }\n"
"    if (Sum <= 1e-7f)\n"
"        return 0.0f;\n"
"    Tint /= Sum;\n"
"    Size /= Sum;\n"
"    // Held near twice the thickest puff's own middle, but SMOOTHLY. A min() is a corner, and the place where this\n"
"    // one turns is a surface of equal thickness - which around a puff is an ellipsoid, and an ellipsoid drawn on a\n"
"    // screen is a CIRCLE. Worse, the self-shadow asks this same question a second time, so the corner landed in the\n"
"    // LIGHTING, where the eye finds a line far more readily than in a density. The same saturating limiter the\n"
"    // relief needed: flat where it was flat, bending towards the limit, and never turning back.\n"
"    float Over = Sum / (2.0f * Most);\n"
"    return Sum * rsqrt(1.0f + Over * Over);\n"
"}\n"
"\n"
"// What the noise leaves of it. Cut HARD rather than merely shaded: a gentle cut leaves a smooth ball with a\n"
"// pattern painted on it, and the HOLES are what make smoke read as smoke.\n"
"// Fine: how many times finer than the grain the smoke was shaped with (Thick).\n"
"float Carve(float3 P, float Fine)\n"
"{\n"
"    float3 W = EyeWorld + ToWorld(P);\n"
"    float3 Swirl = float3(Noise(W * 0.012f + float3(Time * 0.07f, 0.0f, 0.0f)),\n"
"                          Noise(W * 0.012f + float3(0.0f, Time * 0.07f, 11.3f)),\n"
"                          Noise(W * 0.012f + float3(23.1f, 0.0f, Time * 0.05f))) - 0.5f;\n"
"    float3 At = (W - float3(0.0f, 0.0f, Time * Rise)) * (Grain * Fine) + Swirl * Warp;\n"
"    float  N  = Noise(At) * 0.6f + Noise(At * 2.7f) * 0.3f + Noise(At * 6.1f) * 0.1f;\n"
"    return saturate(N * 2.6f - 0.85f);\n"
"}\n"
"\n"
"// The smoke at a point: the puffs there, carved by the noise. The grain goes by the smoke's own size: lumps a\n"
"// cloud's size leave a wisp either whole or gone, so smoke smaller than RDX_CLOUD_GRAIN_SIZE is carved finer, in\n"
"// whole halvings of the grain - each a pattern standing in the world - blended by how far the size lies between\n"
"// two. A grain that followed the size itself would slide through the smoke as its puffs grew.\n"
"float Thick(float3 P, uint First, uint Count, out float3 Tint, out float Size)\n"
"{\n"
"    float Sum = PuffAt(P, First, Count, Tint, Size);\n"
"    if (Sum <= 1e-7f)\n"
"        return 0.0f;\n"
"    float Lod  = clamp(log2(RDX_CLOUD_GRAIN_SIZE / max(Size, 0.25f)), 0.0f, RDX_CLOUD_GRAIN_LODS);\n"
"    float Low  = floor(Lod);\n"
"    float Part = Lod - Low;\n"
"    float Cut  = Carve(P, exp2(Low));\n"
"    if (Part > 0.01f)\n"
"        Cut = lerp(Cut, Carve(P, exp2(Low + 1.0f)), Part);\n"
"    return Sum * Cut;\n"
"}\n"
"\n"
"// One question of the level's cubes: is the point Dist from the lamp, along the view-space direction FromLamp,\n"
"// lit by it? The cubes are drawn in WORLD axes, so only the direction is turned - the distance a rotation\n"
"// leaves alone. The reference depth is taken along the direction itself, because near a cube's edge a sample\n"
"// may land on the next face, whose depth runs along another axis (the same as FShadowFactor's TapLit).\n"
"float LampSees(float3 FromLamp, float Dist, float Slot)\n"
"{\n"
"    float3 D  = normalize(ToWorld(FromLamp));\n"
"    float3 V  = D * Dist;\n"
"    float  Ma = max(abs(V.x), max(abs(V.y), abs(V.z)));\n"
"    return Walls.SampleCmpLevelZero(Cmp, float4(D, Slot), ShaftNear / max(Ma - ShaftBias, ShaftNear));\n"
"}\n"
"\n"
"// LIGHT SHAFTS. The same ray, walked a second time - but through the AIR instead of through the puffs, asking\n"
"// at each step which lamps can see that point. Where a lamp reaches a step, a little of its light scatters\n"
"// towards the eye; where a wall stands between, none does. That difference ALONG the ray is the shaft, and it\n"
"// needs no geometry of its own: the beam is the shape of the hole the level already has.\n"
"//\n"
"// A walk of its own, because the two want different stretches. A puff sits in one place and its stretch is\n"
"// short; the air is everywhere, and its stretch is as far as the eye sees.\n"
"float3 Shafts(float3 Dir, float SceneZ, uint2 At)\n"
"{\n"
"    float Far = min(SceneZ, ShaftMaxZ);\n"
"    if (ShaftCount == 0u || Far <= RayFrom + 1.0f) return float3(0.0f, 0.0f, 0.0f);\n"
"    // The lamps this pixel's ray can meet: its tile's list - read only in a frame with lamps, whose lists hold\n"
"    // every tile's two numbers.\n"
"    uint TileAt = (At.x / RDX_CLOUD_TILE) + (At.y / RDX_CLOUD_TILE) * TileCols;\n"
"    uint First  = LampTiles[TileAt * 2u];\n"
"    uint Held   = LampTiles[TileAt * 2u + 1u];\n"
"    if (Held == 0u) return float3(0.0f, 0.0f, 0.0f);\n"
"    uint  N  = max(ShaftSteps, 4u);\n"
"    float Dt = (Far - RayFrom) / (float)N;\n"
"    // The first step is moved by a fraction of one, differently for each pixel. With a fixed start every ray\n"
"    // samples the same few planes in the air and those planes read as bands across the beam. Scattered, the\n"
"    // same error is noise - and noise, at half size and then read back with a filter, is not seen.\n"
"    float  Jit  = Hash(float3((float)At.x, (float)At.y, 1.0f));\n"
"    float3 Unit = normalize(Dir);\n"
"    float3 Sum  = float3(0.0f, 0.0f, 0.0f);\n"
"    [loop] for (uint k = 0u; k < N; k++)\n"
"    {\n"
"        float3 P = Dir * (RayFrom + ((float)k + Jit) * Dt);\n"
"        [loop] for (uint q = 0u; q < Held; q++)\n"
"        {\n"
"            uint   L = LampTiles[First + q];\n"
"            float4 A = Lamps[L * 2u + 0u];\n"
"            float3 FromLamp = P - A.xyz;\n"
"            float  Away = length(FromLamp);\n"
"            if (Away >= A.w) continue;\n"
"            float Sees = LampSees(FromLamp, Away, Lamps[L * 2u + 1u].w);\n"
"            if (Sees <= 0.002f) continue;\n"
"            // Falling off with the square of the distance, as light does, held to the lamp's own reach so\n"
"            // that a lamp never brightens air the engine says it does not light.\n"
"            float Reach = 1.0f - Away / A.w;\n"
"            // Which way it scatters: forwards, the same Henyey-Greenstein as the smoke. A beam coming\n"
"            // towards the eye is bright and the same beam seen from the side is faint - and THAT is what\n"
"            // makes it read as a beam rather than as an even haze over everything.\n"
"            float Cos   = dot(Unit, FromLamp / max(Away, 1e-3f));\n"
"            float Denom = 1.0f + ShaftG * ShaftG - 2.0f * ShaftG * Cos;\n"
"            float Phase = (1.0f - ShaftG * ShaftG)\n"
"                        / (4.0f * 3.14159265f * max(Denom, 1e-3f) * sqrt(max(Denom, 1e-3f)));\n"
"            Sum += Lamps[L * 2u + 1u].rgb * (Sees * Reach * Reach * Phase * 4.0f * 3.14159265f);\n"
"        }\n"
"    }\n"
"    // Times the length of a step, so that what comes out is the light GATHERED ALONG THE RAY and not a count of\n"
"    // samples - the strength then means the same thing whatever the step count is. Measured against ten thousand\n"
"    // units, about the length of a long street: the strength is then how bright fully lit air gets over one of\n"
"    // those, which is a number a person can hold in their head. Per hundred units instead, and the same 0.6 turns\n"
"    // the end of a corridor into a white blob - measured, not guessed.\n"
"    return Sum * (Dt * 1e-4f * ShaftStrength);\n"
"}\n"
"\n"
"// The stretch of the pixel's ray, EyeWorld + RayWorld * z for z from Lo to Hi, in an own lamp's cone and within\n"
"// its reach - From the eye less the lamp's place, the cone round the unit Axis out to the cosine Cos: Lo and Hi\n"
"// narrowed to it, false where none of it is (RdxMath::OwnLampSpan is the same arithmetic, tested).\n"
"bool OwnBeamSpan(float3 From, float3 Axis, float Cos, float Reach, inout float Lo, inout float Hi)\n"
"{\n"
"    const float3 V  = RayWorld;\n"
"    const float  VV = dot(V, V), FV = dot(From, V), FF = dot(From, From);\n"
"    const float  VA = dot(V, Axis), FA = dot(From, Axis);\n"
"    const float  Ball = FV * FV - VV * (FF - Reach * Reach);\n"
"    if (Ball <= 0.0f) return false;\n"
"    const float BallRoot = sqrt(Ball);\n"
"    Lo = max(Lo, (-FV - BallRoot) / VV);\n"
"    Hi = min(Hi, (-FV + BallRoot) / VV);\n"
"    if (VA > 0.0f)      Lo = max(Lo, -FA / VA);\n"
"    else if (VA < 0.0f) Hi = min(Hi, -FA / VA);\n"
"    else if (FA < 0.0f) return false;\n"
"    const float C2   = Cos * Cos;\n"
"    const float A    = VA * VA - C2 * VV;\n"
"    const float B    = 2.0f * (VA * FA - C2 * FV);\n"
"    const float C    = FA * FA - C2 * FF;\n"
"    const float Disc = B * B - 4.0f * A * C;\n"
"    if (A < -1e-6f * VV)\n"
"    {\n"
"        if (Disc <= 0.0f) return false;\n"
"        const float R = sqrt(Disc);\n"
"        Lo = max(Lo, (-B + R) / (2.0f * A));\n"
"        Hi = min(Hi, (-B - R) / (2.0f * A));\n"
"    }\n"
"    else if (A > 1e-6f * VV)\n"
"    {\n"
"        if (Disc > 0.0f)\n"
"        {\n"
"            const float R = sqrt(Disc);\n"
"            if (VA > 0.0f) Lo = max(Lo, (-B + R) / (2.0f * A));\n"
"            else           Hi = min(Hi, (-B - R) / (2.0f * A));\n"
"        }\n"
"    }\n"
"    else if (B > 0.0f) Lo = max(Lo, -C / B);\n"
"    else if (B < 0.0f) Hi = min(Hi, -C / B);\n"
"    else if (C < 0.0f) return false;\n"
"    return Hi > Lo;\n"
"}\n"
"\n"
"// How much of the light of the own lamp whose rows begin at B its map lets through to X, a place in the air Dist\n"
"// from it, 0..1: one look of the comparison, a little nearer the lamp than X, as SolidPass.hlsl's OwnShadow asks.\n"
"float OwnSees(uint B, float3 X, float Dist)\n"
"{\n"
"    const float4 Head = OwnRows[0];\n"
"    const float4 Q    = float4(X, 1.0f);\n"
"    const float4 C    = float4(dot(OwnRows[B + 3u], Q), dot(OwnRows[B + 4u], Q), dot(OwnRows[B + 5u], Q),\n"
"                               dot(OwnRows[B + 6u], Q));\n"
"    if (C.w <= Head.z) return 0.0f;\n"
"    const float2 Uv = float2(0.5f + 0.5f * C.x / C.w, 0.5f - 0.5f * C.y / C.w);\n"
"    if (any(Uv <= 0.0f) || any(Uv >= 1.0f)) return 0.0f;\n"
"    const float Ref = Head.z / max(C.w - 2.0f * Head.y * Dist, Head.z);\n"
"    return OwnMaps.SampleCmpLevelZero(Cmp, float3(Uv, OwnRows[B + 1u].w), Ref);\n"
"}\n"
"\n"
"// THE WEAPON LAMPS' BEAMS (Flashlight=Everybody): the air of each one's cone gives back a little of the light\n"
"// that crosses it, towards the eye - a torch's beam in a dusty room, its shape the lamp's own cone and what the\n"
"// lamp's map lets through: a wall cuts it off, a man standing in it leaves his shadow in the air. Walked along\n"
"// the stretch of the ray inside the cone alone (OwnBeamSpan), so a pixel whose ray passes through no lamp's light\n"
"// walks nothing. The light as the cone puts it on the level (SolidPass.hlsl's OwnLampsAt, with no face),\n"
"// scattered leaning forwards (OwnForward, RdxMath::OwnBeamForward): looked into, the beam is bright about the\n"
"// lamp; from the side it is fainter. The flashlight's own row - the eye's lamp, for the views that are not the\n"
"// eye's - is left out: its cone throws no beam into the air.\n"
"float3 OwnBeams(float SceneZ, uint2 At)\n"
"{\n"
"    if (OwnCount == 0u) return float3(0.0f, 0.0f, 0.0f);\n"
"    const float4 Head  = OwnRows[0];\n"
"    const float4 Rims  = OwnRows[1];\n"
"    const float4 Fall  = OwnRows[2];\n"
"    const float4 Reach = OwnRows[3];\n"
"    const uint   Count = min((uint)Head.x, OwnCount);\n"
"    const float3 Unit  = RayWorld / max(RayLong, 1e-6f);\n"
"    const float  G     = OwnForward;\n"
"    // The steps moved by a fraction of one, differently for each pixel, as the shafts' are: noise, not bands.\n"
"    const float  Jit   = Hash(float3((float)At.x, (float)At.y, 3.0f));\n"
"    float3 Sum = float3(0.0f, 0.0f, 0.0f);\n"
"    [loop] for (uint L = 0u; L < Count; L++)\n"
"    {\n"
"        const uint   B   = RDX_OWN_ROWS_HEAD + L * RDX_OWN_ROWS_LAMP;\n"
"        const float4 Hue = OwnRows[B + 2u];\n"
"        if (Hue.w > 0.5f) continue;\n"
"        const float3 Lamp = OwnRows[B].xyz;\n"
"        const float3 Axis = OwnRows[B + 1u].xyz;\n"
"        float Lo = RayFrom, Hi = SceneZ;\n"
"        if (!OwnBeamSpan(EyeWorld - Lamp, Axis, Rims.z, Reach.x, Lo, Hi)) continue;\n"
"        const uint  N  = clamp((uint)((Hi - Lo) * RayLong / RDX_OWN_DUST_STEP) + 1u, 4u, RDX_OWN_DUST_STEPS);\n"
"        const float Dt = (Hi - Lo) / (float)N;\n"
"        float Lit = 0.0f;\n"
"        [loop] for (uint k = 0u; k < N; k++)\n"
"        {\n"
"            const float3 D    = EyeWorld + RayWorld * (Lo + ((float)k + Jit) * Dt) - Lamp;\n"
"            const float  Dist = length(D);\n"
"            if (Dist < 1.0f) continue;\n"
"            const float3 Dir   = D / Dist;\n"
"            const float  Cos   = dot(Dir, Axis);\n"
"            const float  Hot   = smoothstep(0.0f, 1.0f, (Cos - Rims.x) * Rims.y);\n"
"            const float  Spill = smoothstep(0.0f, 1.0f, (Cos - Rims.z) * Rims.w);\n"
"            const float  Cone  = Hot + (1.0f - Hot) * Fall.w * Spill;\n"
"            const float  Near  = smoothstep(0.0f, 1.0f, (Dist - Fall.y) * Fall.z);\n"
"            const float  Far   = 1.0f - smoothstep(0.5f * Reach.x, Reach.x, Dist);\n"
"            const float  Q     = Dist / max(Fall.x, 1.0f);\n"
"            const float  Light = Cone * Near * Far / (1.0f + Q * Q);\n"
"            if (Light <= 1e-4f) continue;\n"
"            // Henyey-Greenstein, by the angle between the way the light goes (Dir) and the way it leaves\n"
"            // towards the eye (-Unit), times four pi: one where it scatters evenly.\n"
"            const float Denom = 1.0f + G * G + 2.0f * G * dot(Dir, Unit);\n"
"            const float Phase = (1.0f - G * G) / max(Denom * sqrt(Denom), 1e-3f);\n"
"            Lit += Light * Phase * OwnSees(B, Lamp + D, Dist);\n"
"        }\n"
"        // By the length of a step in the level, so that what comes out is the light gathered along the ray.\n"
"        Sum += Hue.rgb * (Lit * Dt * RayLong);\n"
"    }\n"
"    return Sum * OwnDust;\n"
"}\n"
"\n"
"// WATERFALLS (Water=Low and up): falling water as a volume, in place of the flat sheets the level has for it.\n"
"//\n"
"// The water in a column falls as water falls: where a streak is, is read by how long the water has been falling\n"
"// to get there - the root of the drop - so the streaks run down faster the further they have come, and stretch as\n"
"// they do, and nothing piles up. Each streak is an upright strand standing in the world, so the column is a column\n"
"// from whatever side it is seen. Towards its foot it breaks up, and where it comes down into water a low, wide\n"
"// foam churns around it.\n"
"#define FALL_G    950.0f\n"
"#define FOAM_WIDE 1.7f\n"
"#define FOAM_HIGH 14.0f\n"
"float FallAt(float3 W, out float3 Tint, out float3 Normal, out float Shine)\n"
"{\n"
"    Tint   = float3(0.0f, 0.0f, 0.0f);\n"
"    Normal = float3(0.0f, 0.0f, 1.0f);\n"
"    Shine  = 0.0f;\n"
"    float Sum = 0.0f;\n"
"    [loop] for (uint i = 0u; i < FallCount; i++)\n"
"    {\n"
"        FFall  F = Falls[i];\n"
"        float2 D = W.xy - F.Place.xy;\n"
"        float2 U = F.Across.xy;\n"
"        float2 V = float2(-U.y, U.x);\n"
"        float  a = dot(D, U) / max(F.Across.z, 1e-3f);\n"
"        float  b = dot(D, V) / max(F.Across.w, 1e-3f);\n"
"        float  q = a * a + b * b;\n"
"        float  Drop = F.Place.w - W.z;\n"
"        float  Tall = max(F.Place.w - F.Place.z, 1.0f);\n"
"        if (q < 1.0f && Drop >= 0.0f && Drop <= Tall + 2.0f)\n"
"        {\n"
"            float  Fell   = sqrt(2.0f * Drop / FALL_G);\n"
"            float3 At     = float3(W.xy * 0.16f, (Fell - Time) * FallRate);\n"
"            float  N      = Noise(At) * 0.65f + Noise(At * float3(2.3f, 2.3f, 1.7f) + 7.1f) * 0.35f;\n"
"            float  Down   = saturate(Drop / Tall);\n"
"            // Thick strands near the top; fewer and thinner towards the foot, where the water breaks up.\n"
"            float  Strand = saturate((N - lerp(0.30f, 0.55f, Down * Down)) * 3.0f);\n"
"            float  Body   = saturate((1.0f - q) * 2.5f);\n"
"            float  d      = Body * saturate(Drop / 12.0f) * (0.2f + 0.8f * Strand);\n"
"            Sum   += d;\n"
"            Tint  += F.Colour.rgb * d;\n"
"            Shine += F.Foam.w * d * Strand;\n"
"            Normal = normalize(float3(U * (a / max(F.Across.z, 1e-3f)) + V * (b / max(F.Across.w, 1e-3f)), 1e-3f));\n"
"        }\n"
"        float Foot = W.z - F.Place.z;\n"
"        if (F.Colour.w > 0.5f && Foot > -2.0f && Foot < FOAM_HIGH)\n"
"        {\n"
"            float r = q / (FOAM_WIDE * FOAM_WIDE);\n"
"            if (r < 1.0f)\n"
"            {\n"
"                float Churn = Noise(float3(W.xy * 0.11f, Time * 1.7f)) * 0.6f +\n"
"                              Noise(float3(W.xy * 0.29f, Time * 2.9f + 3.0f)) * 0.4f;\n"
"                float f = (1.0f - r) * saturate(1.0f - Foot / FOAM_HIGH) * saturate(Churn * 2.0f - 0.5f) * 0.5f;\n"
"                Sum  += f;\n"
"                Tint += F.Foam.rgb * f;\n"
"            }\n"
"        }\n"
"    }\n"
"    if (Sum > 1e-4f)\n"
"    {\n"
"        Tint  /= Sum;\n"
"        Shine /= Sum;\n"
"    }\n"
"    return Sum * FallDensity;\n"
"}\n"
"\n"
"// The stretch of a ray any column reaches: the ray against each column's upright cylinder - its foam's, where it has\n"
"// one - cut at its top and its foot. O and R are the ray in the world, R per unit of view depth.\n"
"float2 FallSpan(float3 O, float3 R)\n"
"{\n"
"    float Near = 1e9f, Far = 0.0f;\n"
"    [loop] for (uint i = 0u; i < FallCount; i++)\n"
"    {\n"
"        FFall  F   = Falls[i];\n"
"        float  Rad = max(F.Across.z, F.Across.w) * ((F.Colour.w > 0.5f) ? FOAM_WIDE : 1.0f);\n"
"        float2 o   = O.xy - F.Place.xy;\n"
"        float  A   = dot(R.xy, R.xy);\n"
"        float  B   = 2.0f * dot(o, R.xy);\n"
"        float  C   = dot(o, o) - Rad * Rad;\n"
"        float  t0 = 0.0f, t1 = 1e9f;\n"
"        if (A > 1e-8f)\n"
"        {\n"
"            float Disc = B * B - 4.0f * A * C;\n"
"            if (Disc <= 0.0f) continue;\n"
"            float Root = sqrt(Disc);\n"
"            t0 = (-B - Root) / (2.0f * A);\n"
"            t1 = (-B + Root) / (2.0f * A);\n"
"        }\n"
"        else if (C > 0.0f) continue;\n"
"        float Lo = F.Place.z - 2.0f, Hi = F.Place.w;\n"
"        if (abs(R.z) > 1e-6f)\n"
"        {\n"
"            float za = (Lo - O.z) / R.z, zb = (Hi - O.z) / R.z;\n"
"            t0 = max(t0, min(za, zb));\n"
"            t1 = min(t1, max(za, zb));\n"
"        }\n"
"        else if (O.z < Lo || O.z > Hi) continue;\n"
"        if (t1 > t0) { Near = min(Near, t0); Far = max(Far, t1); }\n"
"    }\n"
"    return float2(max(Near, RayFrom), Far);\n"
"}\n"
"\n"
"// The columns walked along their own stretch of the ray - a column is a narrow thing, and the smoke's steps would put\n"
"// two samples in it. Lit by the light around, by the nearest lamp - more on the side that faces it - and with a glint\n"
"// where the column's round turns that lamp towards the eye. Returns where along the ray the stretch lies, for laying\n"
"// it in front of the smoke or behind it.\n"
"float WalkFalls(float3 Dir, float SceneZ, float Off, out float3 Sum, out float Clear)\n"
"{\n"
"    Sum   = float3(0.0f, 0.0f, 0.0f);\n"
"    Clear = 1.0f;\n"
"    float3 O    = EyeWorld;\n"
"    float3 R    = ToWorld(Dir);\n"
"    float2 Span = FallSpan(O, R);\n"
"    float  Far  = min(Span.y, SceneZ);\n"
"    if (Far <= Span.x) return 1e9f;\n"
"    float  Len   = Far - Span.x;\n"
"    float  Dt    = max(Len / 32.0f, 1.5f);\n"
"    uint   N     = (uint)clamp(ceil(Len / Dt), 1.0f, 32.0f);\n"
"    float  Unit  = length(R);\n"
"    float3 LampW = EyeWorld + ToWorld(LampView);\n"
"    [loop] for (uint k = 0u; k < N; k++)\n"
"    {\n"
"        float Walked = (float)k * Dt;\n"
"        float Step   = min(Dt, Len - Walked);\n"
"        if (Step <= 0.0f) break;\n"
"        float3 W = O + R * (Span.x + Walked + Off * Step);\n"
"        float3 Tint, Nrm;\n"
"        float  Shine;\n"
"        float  T = FallAt(W, Tint, Nrm, Shine);\n"
"        if (T <= 0.0005f) continue;\n"
"        float3 ToLamp = LampW - W;\n"
"        float  Away   = length(ToLamp);\n"
"        float3 L      = ToLamp / max(Away, 1e-3f);\n"
"        float  Reach  = saturate(1.0f - Away / max(LampReach, 1e-3f));\n"
"        float3 Lamp   = LampHue * (LampLit * Reach * Reach);\n"
"        float3 E      = normalize(O - W);\n"
"        float  Wrap   = saturate(dot(Nrm, L) * 0.5f + 0.5f);\n"
"        float  Glint  = pow(saturate(dot(Nrm, normalize(L + E))), 24.0f);\n"
"        float3 Lit    = Tint * (FallAmbient + Lamp * Wrap) + Lamp * (Glint * Shine);\n"
"        float  Cover  = 1.0f - exp(-T * Step * Unit);\n"
"        Sum   += Clear * Cover * Lit;\n"
"        Clear *= 1.0f - Cover;\n"
"        if (Clear < 0.01f) break;\n"
"    }\n"
"    return 0.5f * (Span.x + Far);\n"
"}\n"
"\n"
"// A flame's colour from its heat, 0..1 standing for 1000 K to 2200 K: the hue a hot body gives off at those\n"
"// temperatures (Planck's law at 610, 550 and 465 nm, each row brought to its brightest channel), and a brightness\n"
"// that climbs steeply with the heat - so the hottest part of the flame runs past what the screen shows, into yellow and\n"
"// white, and its cooler edges stay a dim red.\n"
"static const float3 GlowHue[7] =\n"
"{\n"
"    float3(1.0f, 0.1280f, 0.0025f), float3(1.0f, 0.1966f, 0.0085f), float3(1.0f, 0.2671f, 0.0203f),\n"
"    float3(1.0f, 0.3361f, 0.0392f), float3(1.0f, 0.4018f, 0.0653f), float3(1.0f, 0.4636f, 0.0982f),\n"
"    float3(1.0f, 0.5211f, 0.1372f)\n"
"};\n"
"float3 Glow(float T)\n"
"{\n"
"    float x = saturate(T) * 6.0f;\n"
"    uint  i = min((uint)x, 5u);\n"
"    return lerp(GlowHue[i], GlowHue[i + 1u], x - (float)i) * (T * T * (0.4f + 2.6f * T));\n"
"}\n"
"\n"
"// The flame's heat at a point, 0..1: inside the body's footprint, which narrows as it rises; a grain that rises\n"
"// through it with time and is turned aside by a slower one, so the flame licks upward and sways; towards its top only\n"
"// the strongest of the grain is left, and it tears into tongues - which may reach above the sheets the body was made\n"
"// from. Hot at the foot and along the middle, cooler towards the edges and the top. Tint: the flames' own,\n"
"// weighted; Gain: how many times stronger they glow by the unit, weighted the same. Only the bodies of the pixel's\n"
"// own tile are asked (First, Count): its list holds every body the pixel's ray can meet.\n"
"// How far a body's flame reaches is RdxMath's (FireWalkReach, FireBallOut, FireBallTall): the tiles a body is\n"
"// listed in are worked out from the same numbers.\n"
"#define FIRE_REACH RDX_FIRE_REACH\n"
"// Cold: how much of that heat is no flame's but a ball of glow's (RdxMath::FlameShape_Glow), of 1.\n"
"// Z: how far along the pixel's ray the point is, in view depth - a flame is nothing behind the picture's own\n"
"// surface; a ball of glow is walked past it (GlowShows).\n"
"// Pale: how far the glow there may turn towards white where it is dense, of 1 - its picture's own brightest\n"
"// colour says (Shape.w): the sparks' ends in white, the plasma bolt's is green to the last.\n"
"float FireAt(float3 W, float Z, uint First, uint Count, out float3 Tint, out float Gain, out float Cold,\n"
"             out float Pale)\n"
"{\n"
"    Tint = float3(0.0f, 0.0f, 0.0f);\n"
"    float Heat = 0.0f, Weight = 0.0f, Strong = 0.0f, Chill = 0.0f, Whiten = 0.0f;\n"
"    // The grain the balls of glow are torn by, at this place: the same for every ball here, so worked out once -\n"
"    // for the first ball the place lies in - and not once a ball: a burst of sparks is a hundred of them, one\n"
"    // over another.\n"
"    float GlowGrain = -1.0f;\n"
"    [loop] for (uint q = 0u; q < Count; q++)\n"
"    {\n"
"        FFire F    = Fires[FireTiles[First + q]];\n"
"        if (F.Shape.x < 2.5f && Z > RayStop) continue;\n"
"        [branch] if (F.Shape.x > 0.5f)\n"
"        {\n"
"            // A burning body's flames, in the shapes of the game's own pictures of them: a FIREBALL (OnFire_J) - a lumpy\n"
"            // ball, white-hot through most of it, its rim orange - and a LICK (OneFlame_J) - a small drop of flame,\n"
"            // white at its foot, narrowing and fading upwards. Their grain is in their own frame, rising through them:\n"
"            // they go where the game moves them, as its cards do, instead of standing in the world.\n"
"            float3 C = float3(F.Place.xy, 0.5f * (F.Place.z + F.Place.w));\n"
"            float3 L = W - C;\n"
"            float  T = 0.0f;\n"
"            if (F.Shape.x > 2.5f)\n"
"            {\n"
"                // A ball of GLOW - a puff of the electric sparks, a drop of plasma: round, densest at its middle,\n"
"                // its rim torn by a grain that stirs where it is and rises nowhere, and gone before the body's own\n"
"                // radius - the stretch a ray walks through it is cut there. A mesh of glow's - a plasma bolt's -\n"
"                // is the same ball drawn out to its own length, width and height.\n"
"                float2 Along = F.Axes.xy;\n"
"                float  r = length(float3(dot(L.xy, Along) / max(F.Axes.z, 1e-3f),\n"
"                                         dot(L.xy, float2(-Along.y, Along.x)) / max(F.Axes.w, 1e-3f),\n"
"                                         L.z / max(0.5f * (F.Place.w - F.Place.z), 1e-3f)));\n"
"                if (r >= 1.0f) continue;\n"
"                float Show = GlowShows(C, min(F.Axes.z, min(F.Axes.w, 0.5f * (F.Place.w - F.Place.z))));\n"
"                if (Show <= 0.0f) continue;\n"
"                if (GlowGrain < 0.0f)\n"
"                {\n"
"                    float3 P  = W * 0.22f + float3(Time, -Time, Time) * (FireRate * 1.1f);\n"
"                    GlowGrain = Noise(P) * 0.6f + Noise(P * 2.3f + 5.1f) * 0.4f;\n"
"                }\n"
"                float  N    = GlowGrain;\n"
"                float  Edge = 0.82f + 0.36f * (N - 0.5f);\n"
"                T = saturate((Edge - r) * 2.5f) * (0.75f + 0.25f * N) * F.Shape.y;\n"
"                Heat    = max(Heat, T);\n"
"                Tint   += F.Colour.rgb * T;\n"
"                Strong += F.Shape.z * Show * T;\n"
"                Weight += T;\n"
"                Chill  += T;\n"
"                Whiten += F.Shape.w * T;\n"
"                continue;\n"
"            }\n"
"            if (F.Shape.x < 1.5f)\n"
"            {\n"
"                float r = length(float3(L.xy, L.z / RDX_FIRE_BALL_TALL)) / max(F.Axes.z, 1e-3f);\n"
"                if (r >= RDX_FIRE_BALL_OUT) continue;\n"
"                float3 P    = L * 0.14f + float3(0.0f, 0.0f, -Time * FireRate * 1.5f);\n"
"                float  N    = Noise(P) * 0.6f + Noise(P * 2.3f + 5.1f) * 0.4f;\n"
"                float  Edge = 1.0f + 0.35f * (N - 0.5f);\n"
"                // Soft all the way out, as the picture's orange rim fades into nothing; held below white, since a\n"
"                // body wears eight of them over one another.\n"
"                T = saturate((Edge - r) * 1.4f) * (0.8f + 0.2f * N) * 0.7f;\n"
"            }\n"
"            else\n"
"            {\n"
"                float Tall = max(F.Place.w - F.Place.z, 1.0f);\n"
"                float h    = (W.z - F.Place.z) / Tall;\n"
"                if (h < -0.1f || h > 1.1f) continue;\n"
"                float Wd   = max(F.Axes.z, 1e-3f) * lerp(1.0f, 0.35f, saturate(h));\n"
"                float q    = dot(L.xy, L.xy) / (Wd * Wd);\n"
"                if (q >= 1.0f) continue;\n"
"                float3 P   = float3(L.xy * 0.2f, (W.z - F.Place.z) * 0.12f - Time * FireRate * 1.5f);\n"
"                float  N   = Noise(P) * 0.6f + Noise(P * 2.3f + 5.1f) * 0.4f;\n"
"                T = (1.0f - q) * saturate(1.1f - 0.9f * h) * saturate(h * 6.0f + 0.6f) * (0.7f + 0.3f * N);\n"
"            }\n"
"            T *= F.Shape.y;\n"
"            Heat    = max(Heat, T);\n"
"            Tint   += F.Colour.rgb * T;\n"
"            Strong += F.Shape.z * T;\n"
"            Weight += T;\n"
"            continue;\n"
"        }\n"
"        float Tall = max(F.Place.w - F.Place.z, 1.0f);\n"
"        float h    = (W.z - F.Place.z) / Tall;\n"
"        if (h < -0.1f || h > FIRE_REACH) continue;\n"
"        float2 D      = W.xy - F.Place.xy;\n"
"        float2 U      = F.Axes.xy;\n"
"        float2 V      = float2(-U.y, U.x);\n"
"        float  Narrow = lerp(1.0f, 0.3f, saturate(h / FIRE_REACH));\n"
"        float  a      = dot(D, U) / max(F.Axes.z * Narrow, 1e-3f);\n"
"        float  b      = dot(D, V) / max(F.Axes.w * Narrow, 1e-3f);\n"
"        float  q      = a * a + b * b;\n"
"        if (q >= 1.0f) continue;\n"
"        // A large fire's grain is as many times coarser as the fire is larger (RdxMath::FireBodyScale) -\n"
"        // measured from the body's own foot, and its pace stepped with the size, not sliding with it. A\n"
"        // body's size and middle shift a little whenever a flame joins it or leaves it; a grain read at\n"
"        // the place in the world and at the clock, both divided by the size, jumped whole at every such shift.\n"
"        float  Sc   = max(F.Shape.w, 1.0f);\n"
"        float  Slow = exp2(floor(log2(Sc) + 0.5f));\n"
"        float3 P    = float3((W.xy - F.Place.xy) * 0.12f, (W.z - F.Place.z) * 0.07f) / Sc +\n"
"                      float3(F.Place.xy * 0.12f, F.Place.z * 0.07f - Time * FireRate / Slow) + F.Colour.w;\n"
"        float  Sway = Noise(P * float3(0.45f, 0.45f, 0.35f) + 11.3f);\n"
"        float  N    = Noise(P + Sway * 1.6f) * 0.6f + Noise(P * 2.3f + Sway + 5.1f) * 0.4f;\n"
"        float  Cut  = lerp(0.12f, 0.72f, saturate(h / 1.6f));\n"
"        float  f    = (1.0f - q) * saturate((N - Cut) * 4.0f) * saturate(h * 8.0f + 0.8f);\n"
"        // The bed: along the foot the fire burns whole, with only a flicker in it.\n"
"        f = max(f, (1.0f - q) * saturate(1.0f - h * 2.5f) * (0.55f + 0.45f * N));\n"
"        float  T    = f * saturate(1.2f - 0.45f * h - 0.3f * q);\n"
"        Heat    = max(Heat, T);\n"
"        Tint   += F.Colour.rgb * T;\n"
"        Strong += F.Shape.z * T;\n"
"        Weight += T;\n"
"    }\n"
"    Tint = (Weight > 1e-4f) ? Tint / Weight : float3(1.0f, 1.0f, 1.0f);\n"
"    Gain = (Weight > 1e-4f) ? Strong / Weight : 1.0f;\n"
"    Cold = (Weight > 1e-4f) ? Chill / Weight : 0.0f;\n"
"    Pale = (Chill > 1e-4f) ? saturate(Whiten / Chill) : 1.0f;\n"
"    return Heat;\n"
"}\n"
"\n"
"// The stretches of a ray the flame bodies reach, each body's own - its upright cylinder, cut at its foot and at\n"
"// the highest its tongues go - and those that touch made one. A ray that meets a candle and then a fireplace behind\n"
"// it is walked as a candle and as a fireplace: walked as one stretch from the first to the last, the same few steps\n"
"// fall further apart than the candle is wide, and it shows as a scatter of dots. O and R are the ray in the world,\n"
"// R per unit of view depth; nothing reaches past Most. Where more stretches lie apart than FIRE_STRETCHES, the\n"
"// nearest two are walked as one. Returns how many. The bodies of the pixel's tile (First, Count).\n"
"#define FIRE_STRETCHES 6\n"
"uint FireStretches(float3 O, float3 R, float Most, uint First, uint Count, out float2 S[FIRE_STRETCHES])\n"
"{\n"
"    uint n = 0u;\n"
"    [unroll] for (uint z = 0u; z < FIRE_STRETCHES; z++) S[z] = float2(0.0f, 0.0f);\n"
"    [loop] for (uint q = 0u; q < Count; q++)\n"
"    {\n"
"        FFire  F   = Fires[FireTiles[First + q]];\n"
"        float  Rad = max(F.Axes.z, F.Axes.w);\n"
"        float2 o   = O.xy - F.Place.xy;\n"
"        float  A   = dot(R.xy, R.xy);\n"
"        float  B   = 2.0f * dot(o, R.xy);\n"
"        float  C   = dot(o, o) - Rad * Rad;\n"
"        float  t0 = 0.0f, t1 = 1e9f;\n"
"        if (A > 1e-8f)\n"
"        {\n"
"            float Disc = B * B - 4.0f * A * C;\n"
"            if (Disc <= 0.0f) continue;\n"
"            float Root = sqrt(Disc);\n"
"            t0 = (-B - Root) / (2.0f * A);\n"
"            t1 = (-B + Root) / (2.0f * A);\n"
"        }\n"
"        else if (C > 0.0f) continue;\n"
"        float Lo = F.Place.z - 2.0f;\n"
"        float Hi = (F.Shape.x > 0.5f) ? F.Place.w + 0.25f * (F.Place.w - F.Place.z) + 2.0f\n"
"                                      : F.Place.z + FIRE_REACH * max(F.Place.w - F.Place.z, 1.0f);\n"
"        if (abs(R.z) > 1e-6f)\n"
"        {\n"
"            float za = (Lo - O.z) / R.z, zb = (Hi - O.z) / R.z;\n"
"            t0 = max(t0, min(za, zb));\n"
"            t1 = min(t1, max(za, zb));\n"
"        }\n"
"        else if (O.z < Lo || O.z > Hi) continue;\n"
"        if (F.Shape.x > 2.5f)\n"
"        {\n"
"            // A body of glow is listed only in the tiles the ball that holds it shows in (RdxMath::FlameBodyBall):\n"
"            // its stretch is cut to that ball, which the cylinder's corners reach past.\n"
"            float3 Oc = O - float3(F.Place.xy, 0.5f * (F.Place.z + F.Place.w));\n"
"            float  Rs = max(Rad, 0.5f * (F.Place.w - F.Place.z));\n"
"            float  Ab = dot(R, R);\n"
"            float  Bb = 2.0f * dot(Oc, R);\n"
"            float  Db = Bb * Bb - 4.0f * Ab * (dot(Oc, Oc) - Rs * Rs);\n"
"            if (Db <= 0.0f) continue;\n"
"            float  Rb = sqrt(Db);\n"
"            t0 = max(t0, (-Bb - Rb) / (2.0f * Ab));\n"
"            t1 = min(t1, (-Bb + Rb) / (2.0f * Ab));\n"
"        }\n"
"        t0 = max(t0, RayFrom);\n"
"        // A ball of glow is walked whole where the picture's own surface crosses it (GlowShows); a flame ends there.\n"
"        if (F.Shape.x > 2.5f)\n"
"        {\n"
"            if (t0 >= Most) continue;\n"
"        }\n"
"        else\n"
"            t1 = min(t1, Most);\n"
"        if (t1 <= t0) continue;\n"
"        // Made one with every stretch it touches - and, where there is no place left for one more, with the one it\n"
"        // lies nearest to; grown, it may touch others in turn.\n"
"        bool bAgain = true;\n"
"        [loop] while (bAgain)\n"
"        {\n"
"            bAgain = false;\n"
"            [loop] for (uint j = 0u; j < n; j++)\n"
"            {\n"
"                if (t0 <= S[j].y && t1 >= S[j].x)\n"
"                {\n"
"                    t0     = min(t0, S[j].x);\n"
"                    t1     = max(t1, S[j].y);\n"
"                    S[j]   = S[n - 1u];\n"
"                    n     -= 1u;\n"
"                    bAgain = true;\n"
"                    break;\n"
"                }\n"
"            }\n"
"            if (!bAgain && n >= FIRE_STRETCHES)\n"
"            {\n"
"                uint  Best = 0u;\n"
"                float Gap  = 1e9f;\n"
"                [loop] for (uint k = 0u; k < n; k++)\n"
"                {\n"
"                    float g = max(S[k].x - t1, t0 - S[k].y);\n"
"                    if (g < Gap) { Gap = g; Best = k; }\n"
"                }\n"
"                t0      = min(t0, S[Best].x);\n"
"                t1      = max(t1, S[Best].y);\n"
"                S[Best] = S[n - 1u];\n"
"                n      -= 1u;\n"
"                bAgain  = true;\n"
"            }\n"
"        }\n"
"        S[n] = float2(t0, t1);\n"
"        n   += 1u;\n"
"    }\n"
"    return n;\n"
"}\n"
"\n"
"// The flames walked along their own stretches of the ray. A flame is light, not a veil: what it gives off is added\n"
"// up and nothing behind it is hidden. Its own tint colours the glow a little; the heat decides most of it. Returns\n"
"// where along the ray the flames lie - halfway from the nearest to the farthest - for the smoke in front of them to\n"
"// dim them.\n"
"float WalkFires(float3 Dir, float SceneZ, float Off, uint First, uint Held, out float3 Sum)\n"
"{\n"
"    Sum = float3(0.0f, 0.0f, 0.0f);\n"
"    float3 O = EyeWorld;\n"
"    float3 R = ToWorld(Dir);\n"
"    float2 S[FIRE_STRETCHES];\n"
"    uint   Count = FireStretches(O, R, SceneZ, First, Held, S);\n"
"    if (Count == 0u) return 1e9f;\n"
"    float  Unit = length(R);\n"
"    float  Near = 1e9f, Far = 0.0f;\n"
"    [loop] for (uint s = 0u; s < Count; s++)\n"
"    {\n"
"        float From = S[s].x;\n"
"        float Len  = S[s].y - From;\n"
"        Near = min(Near, From);\n"
"        Far  = max(Far, S[s].y);\n"
"        // A unit a step, or a thirty-second of the stretch where that is longer. A stretch under eight units -\n"
"        // a candle's flame, the rim of any - is walked in eight all the same: the few steps a unit gives it would\n"
"        // show as grain.\n"
"        float Dt = max(Len / 32.0f, min(1.0f, Len / 8.0f));\n"
"        uint  N  = (uint)clamp(ceil(Len / Dt), 1.0f, 32.0f);\n"
"        [loop] for (uint k = 0u; k < N; k++)\n"
"        {\n"
"            float Walked = (float)k * Dt;\n"
"            float Step   = min(Dt, Len - Walked);\n"
"            if (Step <= 0.0f) break;\n"
"            float  Z = From + Walked + Off * Step;\n"
"            float3 W = O + R * Z;\n"
"            float3 Tint;\n"
"            float  Gain, Cold, Pale;\n"
"            float  T = FireAt(W, Z, First, Held, Tint, Gain, Cold, Pale);\n"
"            if (T <= 0.001f) continue;\n"
"            // A flame glows by its heat, tinted a little; a ball of glow by its own colour, whiter where it is\n"
"            // densest as far as its picture's own colours go that way (Pale) - as bright by its density as a\n"
"            // flame is by its heat.\n"
"            float3 Hot = Glow(T) * lerp(float3(1.0f, 1.0f, 1.0f), Tint, 0.35f);\n"
"            float3 Own = lerp(Tint, float3(1.0f, 1.0f, 1.0f), Pale * saturate(T * 1.5f - 0.6f)) *\n"
"                         (T * T * (0.4f + 2.6f * T));\n"
"            Sum += lerp(Hot, Own, Cold) * (FireGlow * Gain * Step * Unit);\n"
"        }\n"
"    }\n"
"    return 0.5f * (Near + Far);\n"
"}\n"
"\n"
"// The sparks a fire throws. Each of a fire's few places for one holds a spark at a time: it comes off the flame\n"
"// at a chance place in it, at a chance moment of its place's own beat, flies up and out, slows, sinks and cools\n"
"// from yellow-white to a dim red, and is gone before the next - or the beat passes with none. All of it from the\n"
"// clock and the fire's own number, so a spark is where it is whichever pixel asks. Asked along the ray as a short\n"
"// streak - where it is and where it was a moment before - no thinner than a pixel of this walk is across there,\n"
"// and as much dimmer as it is made wider. The standing fires only: a burning body's flames go where the body goes.\n"
"// Of the pixel's tile (First, Count): a standing fire is listed wherever its sparks can fly\n"
"// (RdxMath::FireSparkFly).\n"
"#define SPARKS_ON    0x01000000u\n"
"#define SPARK_PLACES 5u\n"
"#define SPARK_SHARE  0.6f\n"
"#define SPARK_RADIUS 0.45f\n"
"#define SPARK_GLOW   3.0f\n"
"float3 Sparks(float3 O, float3 R, float SceneZ, uint First, uint Count)\n"
"{\n"
"    float3 Sum = float3(0.0f, 0.0f, 0.0f);\n"
"    const float A = dot(R, R);\n"
"    [loop] for (uint q = 0u; q < Count; q++)\n"
"    {\n"
"        FFire F = Fires[FireTiles[First + q]];\n"
"        if (F.Shape.x > 0.5f) continue;\n"
"        const float  Tall  = max(F.Place.w - F.Place.z, 1.0f);\n"
"        const float  Wide  = max(F.Axes.z, F.Axes.w);\n"
"        const float  Size  = clamp(Tall / 32.0f, 0.5f, 3.0f);\n"
"        // Every spark of this fire stays inside a ball round the flame's top: a ray that misses it meets none.\n"
"        const float3 Top   = float3(F.Place.xy, F.Place.w);\n"
"        const float  Reach = Wide + Tall + RDX_SPARK_FLY * Size;\n"
"        const float3 Oc    = O - Top;\n"
"        const float  B     = dot(Oc, R);\n"
"        if (B * B - A * (dot(Oc, Oc) - Reach * Reach) <= 0.0f) continue;\n"
"        [loop] for (uint j = 0u; j < SPARK_PLACES; j++)\n"
"        {\n"
"            // By where the fire stands, not by its place in the list: the list's order changes with the frame.\n"
"            const float  Seed  = dot(floor(F.Place.xy / 16.0f), float2(7.31f, 3.17f)) + (float)j * 12.9898f;\n"
"            const float  Beat  = 0.9f + 1.4f * Hash(float3(Seed, 1.0f, 0.5f));\n"
"            const float  Clock = Time / Beat + Hash(float3(Seed, 2.0f, 0.5f));\n"
"            const float  Round = floor(Clock);\n"
"            const float3 Rnd   = float3(Hash(float3(Seed, Round, 3.0f)), Hash(float3(Seed, Round, 4.0f)),\n"
"                                        Hash(float3(Seed, Round, 5.0f)));\n"
"            if (Rnd.z > SPARK_SHARE) continue;\n"
"            const float  Life  = Beat * (0.55f + 0.4f * Rnd.x);\n"
"            const float  Age   = (Clock - Round) * Beat;\n"
"            if (Age >= Life) continue;\n"
"            const float  Turn  = 6.2831853f * Rnd.y;\n"
"            const float2 Side  = float2(cos(Turn), sin(Turn));\n"
"            const float3 From  = float3(F.Place.xy + Side * (Wide * 0.5f * Rnd.x),\n"
"                                        F.Place.z + Tall * (0.35f + 0.5f * Rnd.z));\n"
"            const float3 Speed = float3(Side * (18.0f + 30.0f * Rnd.x), 55.0f + 60.0f * Rnd.y) * Size;\n"
"            float Near = 1e9f, At = 0.0f;\n"
"            [unroll] for (uint k = 0u; k < 3u; k++)\n"
"            {\n"
"                // The air slows it and it sinks: where it is after t seconds.\n"
"                const float  t  = max(Age - 0.02f * (float)k, 0.0f);\n"
"                const float  Go = (1.0f - exp(-1.6f * t)) / 1.6f;\n"
"                const float3 P  = From + Speed * Go - float3(0.0f, 0.0f, 22.0f * Size * t * t);\n"
"                const float  s  = dot(P - O, R) / A;\n"
"                const float3 D  = O + R * s - P;\n"
"                const float  d2 = dot(D, D);\n"
"                if (d2 < Near) { Near = d2; At = s; }\n"
"            }\n"
"            if (At <= max(1.0f, RayFrom) || At >= SceneZ) continue;\n"
"            const float Foot = At * EngPerPixel.x / max(Focal, 1e-3f);\n"
"            const float Rad  = max(SPARK_RADIUS * Size, 0.7f * Foot);\n"
"            const float Thin = (SPARK_RADIUS * Size) / Rad;\n"
"            const float Left = 1.0f - Age / Life;\n"
"            Sum += Glow(0.45f + 0.5f * Left) * (exp(-Near / (Rad * Rad)) * Thin * Thin * Left * SPARK_GLOW);\n"
"        }\n"
"    }\n"
"    return Sum;\n"
"}\n"
"\n"
"// The square of how near a ray (O + R s; A is R's own square) passes the stretch from P0 to P1, and S: how far\n"
"// along the ray that is.\n"
"float RayToStretch(float3 O, float3 R, float A, float3 P0, float3 P1, out float S)\n"
"{\n"
"    const float3 D   = P1 - P0;\n"
"    const float3 Q   = O - P0;\n"
"    const float  e   = dot(D, D);\n"
"    const float  b   = dot(R, D);\n"
"    const float  c   = dot(R, Q);\n"
"    const float  f   = dot(D, Q);\n"
"    const float  Den = A * e - b * b;\n"
"    const float  s0  = (Den > 1e-9f) ? (b * f - c * e) / Den : 0.0f;\n"
"    const float  t   = (e > 1e-9f) ? saturate((b * s0 + f) / e) : 0.0f;\n"
"    S = (b * t - c) / A;\n"
"    const float3 Gap = Q + R * S - D * t;\n"
"    return dot(Gap, Gap);\n"
"}\n"
"\n"
"// The discharges in the balls of glow. A puff of the electric sparks and a drop of plasma are charged gas, and what\n"
"// one sees of the charge is the arc: a short, thin, kinked thread of light somewhere in the ball, there for a\n"
"// beat and gone, the next one somewhere else. A ball has places for them by how large it is across - a large\n"
"// ball sparks as a crowd of small ones does, by threads no longer than theirs (ARC_LONG) -, each place with a beat\n"
"// of its own; a beat strikes an arc or - nearly always - passes with none.\n"
"// All of it from the clock and where the ball is, so an arc\n"
"// is where it is whichever pixel asks, and in a mirror's view as in the world's. A thread in the ball's own space -\n"
"// a ball drawn out to a bolt has its arcs drawn out with it -, never out of the ball, whose tiles are all it is\n"
"// listed in; asked along the ray as the stretches between its kinks, no thinner than a pixel of this walk is across\n"
"// there and as much dimmer as it is made wider. As bright as the game's fade of the ball leaves it (Colour.w), and\n"
"// showing as much as the ball does where the picture's own surface crosses it (GlowShows).\n"
"#define ARC_PLACES 16u\n"
"#define ARC_AREA   64.0f\n"
"#define ARC_LONG   2.5f\n"
"#define ARC_KINKS  5u\n"
"#define ARC_RATE   12.0f\n"
"#define ARC_SHARE  0.03f\n"
"#define ARC_THICK  0.004f\n"
"#define ARC_GLOW   8.0f\n"
"float3 Arcs(float3 O, float3 R, uint First, uint Count)\n"
"{\n"
"    float3 Sum = float3(0.0f, 0.0f, 0.0f);\n"
"    const float A = dot(R, R);\n"
"    [loop] for (uint q = 0u; q < Count; q++)\n"
"    {\n"
"        FFire F = Fires[FireTiles[First + q]];\n"
"        if (F.Shape.x < 2.5f) continue;\n"
"        const float3 Half = float3(F.Axes.z, F.Axes.w, 0.5f * (F.Place.w - F.Place.z));\n"
"        const float3 C    = float3(F.Place.xy, 0.5f * (F.Place.z + F.Place.w));\n"
"        const float  Big  = max(Half.x, max(Half.y, Half.z));\n"
"        // Every arc of this body stays inside its ball: a ray that misses the ball meets none.\n"
"        const float3 Oc   = O - C;\n"
"        const float  B    = dot(Oc, R);\n"
"        if (B * B - A * (dot(Oc, Oc) - Big * Big) <= 0.0f) continue;\n"
"        const float  Small = min(Half.x, min(Half.y, Half.z));\n"
"        const float  Show  = GlowShows(C, Small);\n"
"        if (Show <= 0.0f) continue;\n"
"        const float2 Along = F.Axes.xy;\n"
"        const float2 Side  = float2(-Along.y, Along.x);\n"
"        // By where the ball is, in steps of a quarter of its reach - not by its place in the list, which changes\n"
"        // with the frame: a ball that drifts keeps its arc for that far, and two balls that are not one ball\n"
"        // (FFlameGather::Add) do not strike alike.\n"
"        const float  Seed  = dot(floor(C / max(0.25f * Big, 1.0f)), float3(7.31f, 3.17f, 5.43f));\n"
"        // A place for every ARC_AREA square units of the ball's own reach, squared: one at the least, ARC_PLACES\n"
"        // at the most.\n"
"        const uint   Places = (uint)clamp(Big * Big / ARC_AREA, 1.0f, (float)ARC_PLACES);\n"
"        [loop] for (uint j = 0u; j < Places; j++)\n"
"        {\n"
"            const float  Sj    = Seed + (float)j * 12.9898f;\n"
"            const float  Clock = Time * ARC_RATE * (0.7f + 0.6f * Hash(float3(Sj, 1.0f, 0.5f))) +\n"
"                                 Hash(float3(Sj, 2.0f, 0.5f));\n"
"            const float  Round = floor(Clock);\n"
"            // Most beats pass with no arc: asked first, before anything of the arc is worked out.\n"
"            const float  Chance = Hash(float3(Sj, Round, 5.0f));\n"
"            if (Chance > ARC_SHARE) continue;\n"
"            const float3 Rnd   = float3(Hash(float3(Sj, Round, 3.0f)), Hash(float3(Sj, Round, 4.0f)), Chance);\n"
"            // A short thread from a chance place in the ball - in the ball's own space, the ball's reach one - a\n"
"            // chance way in the world: an eighth to a quarter of the ball's thinnest reach long, and no longer\n"
"            // than ARC_LONG units in the largest ball, whichever way it lies in a ball that is drawn out.\n"
"            const float3 Out3 = float3(Rnd.x, Rnd.y, Hash(float3(Sj, Round, 6.0f))) * 2.0f - 1.0f;\n"
"            const float3 Way  = Out3 / max(length(Out3), 1e-3f);\n"
"            const float3 U0   = (float3(Hash(float3(Sj, Round, 7.0f)), Hash(float3(Sj, Round, 8.0f)),\n"
"                                        Hash(float3(Sj, Round, 9.0f))) - 0.5f) * 0.7f;\n"
"            const float  Long = clamp(Small * (0.12f + 0.14f * Rnd.z / ARC_SHARE), 0.6f, ARC_LONG);\n"
"            const float3 From = C + float3(Along * (U0.x * Half.x) + Side * (U0.y * Half.y), U0.z * Half.z);\n"
"            float  Near = 1e9f, At = 0.0f;\n"
"            float3 Was  = From;\n"
"            [unroll] for (uint k = 1u; k <= ARC_KINKS; k++)\n"
"            {\n"
"                // A kink where it turns: every place between its ends pushed a chance way aside.\n"
"                const float n = (float)k;\n"
"                float3 Now = From + Way * (Long * n / (float)ARC_KINKS);\n"
"                if (k < ARC_KINKS)\n"
"                    Now += (float3(Hash(float3(Sj, Round, 10.0f + n)), Hash(float3(Sj, Round, 20.0f + n)),\n"
"                                   Hash(float3(Sj, Round, 30.0f + n))) * 2.0f - 1.0f) * (0.2f * Long);\n"
"                float s;\n"
"                const float d2 = RayToStretch(O, R, A, Was, Now, s);\n"
"                if (d2 < Near) { Near = d2; At = s; }\n"
"                Was = Now;\n"
"            }\n"
"            if (At <= max(1.0f, RayFrom)) continue;\n"
"            const float Foot  = At * EngPerPixel.x / max(Focal, 1e-3f);\n"
"            const float Thick = clamp(ARC_THICK * Small, 0.02f, 0.05f);\n"
"            const float Rad   = max(Thick, 0.4f * Foot);\n"
"            const float Left  = 1.0f - (Clock - Round);\n"
"            // The thread itself, all but white, and its light round it in the ball's own colour.\n"
"            const float Core  = exp(-Near / (Rad * Rad));\n"
"            const float Halo  = 0.06f * exp(-Near / (16.0f * Rad * Rad));\n"
"            Sum += (lerp(F.Colour.rgb, float3(1.0f, 1.0f, 1.0f), 0.75f * saturate(F.Shape.w)) * Core +\n"
"                    F.Colour.rgb * Halo) *\n"
"                   ((Thick / Rad) * Left * Left * F.Colour.w * Show * ARC_GLOW);\n"
"        }\n"
"    }\n"
"    return Sum;\n"
"}\n"
"\n"
"[numthreads(8, 8, 1)]\n"
"void WalkCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= Size.x || t.y >= Size.y) return;\n"
"    Out[t.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
"    if (t.x < Window.x || t.y < Window.y || t.x >= Window.z || t.y >= Window.w) return;\n"
"    if (PuffCount == 0u && ShaftCount == 0u && FallCount == 0u && FireCount == 0u && OwnCount == 0u) return;\n"
"    // The ray, with its outward part held at one - which makes the distance along it the view depth itself, and\n"
"    // that is what lets the scene's own depth stop the walk with no arithmetic at all.\n"
"    float2 Eng = (float2(t.xy) + 0.5f) * EngPerPixel;\n"
"    float3 Dir = float3((Eng - Screen) / max(Focal, 1e-3f), 1.0f);\n"
"    // A mirror's view: the walks begin where this ray comes through the mirror's plane, to the side the view\n"
"    // shows - and nowhere, where it never does.\n"
"    RayFrom = 0.0f;\n"
"    if (dot(NearPlane.xyz, NearPlane.xyz) > 0.25f)\n"
"    {\n"
"        float Along = dot(NearPlane.xyz, Dir) * ((NearPlane.w < 0.0f) ? -1.0f : 1.0f);\n"
"        RayFrom = (Along > 1e-6f) ? abs(NearPlane.w) / Along : 1e9f;\n"
"    }\n"
"    // The scene behind, at full size - the NEAREST of the four it covers, so the smoke stops at the closest thing\n"
"    // in the quad rather than leaking past an edge.\n"
"    int2 Full = int2(t.xy) * 2;\n"
"    float d = Depth.Load(int3(Full, 0)).r;\n"
"    d = max(d, Depth.Load(int3(Full + int2(1, 0), 0)).r);\n"
"    d = max(d, Depth.Load(int3(Full + int2(0, 1), 0)).r);\n"
"    d = max(d, Depth.Load(int3(Full + int2(1, 1), 0)).r);\n"
"    float SceneZ = 1.0f / max(d, 1e-6f);\n"
"    RayWorld = ToWorld(Dir);\n"
"    RayLong  = length(RayWorld);\n"
"    RayStop  = SceneZ;\n"
"    // A measurement only (SecondRevision-volume-depth-view.txt, the steps' bit 30): the depth this walk stops at,\n"
"    // laid over the picture instead of what it walks - red the view depth in bands of 128 units, green up to 1024.\n"
"    if ((Steps & 0x40000000u) != 0u)\n"
"    {\n"
"        Out[t.xy] = float4(frac(SceneZ / 128.0f), saturate(SceneZ / 1024.0f), 0.0f, 1.0f);\n"
"        return;\n"
"    }\n"
"\n"
"    float3 Sum      = float3(0.0f, 0.0f, 0.0f);\n"
"    float  Clear    = 1.0f;\n"
"    float  SmokeMid = 1e9f;\n"
"    // The puffs this pixel's ray can meet: its tile's list.\n"
"    uint ListFirst = 0u, ListCount = 0u;\n"
"    // A branch, said so: flattened, the tile's two numbers are read for every pixel, puffs or none - from\n"
"    // lists that hold nothing then, and past their end where the target is large.\n"
"    [branch] if (PuffCount > 0u)\n"
"    {\n"
"        uint TileAt = (t.x / RDX_CLOUD_TILE) + (t.y / RDX_CLOUD_TILE) * TileCols;\n"
"        ListFirst = Tiles[TileAt * 2u];\n"
"        ListCount = Tiles[TileAt * 2u + 1u];\n"
"    }\n"
"    [branch] if (ListCount > 0u)\n"
"    {\n"
"    // The one stretch of this ray that any puff reaches at all. Two clouds at opposite ends of a level do not make\n"
"    // every pixel walk the corridor between them: what is walked is what something is actually in.\n"
"    float Near = 1e9f, Far = 0.0f;\n"
"    [loop] for (uint i = 0u; i < ListCount; i++)\n"
"    {\n"
"        FPuff Q = Puffs[Tiles[ListFirst + i]];\n"
"        float Bound = max(Q.Radius, Q.Tall);\n"
"        float A = dot(Dir, Dir);\n"
"        float B = -2.0f * dot(Dir, Q.Centre);\n"
"        float C = dot(Q.Centre, Q.Centre) - Bound * Bound;\n"
"        float Disc = B * B - 4.0f * A * C;\n"
"        if (Disc > 0.0f)\n"
"        {\n"
"            float s = sqrt(Disc);\n"
"            Near = min(Near, (-B - s) / (2.0f * A));\n"
"            Far  = max(Far,  (-B + s) / (2.0f * A));\n"
"        }\n"
"    }\n"
"    Near = max(Near, RayFrom);\n"
"    Far  = min(Far, SceneZ);\n"
"    SmokeMid = 0.5f * (Near + Far);\n"
"    [branch] if (Far > Near)\n"
"    {\n"
"\n"
"    // How many steps: enough for the LENGTH actually walked, not a fixed number for every ray. A puff on its own is\n"
"    // a short stretch and wants few; two clouds at opposite ends of a corridor are a long one and want more, or\n"
"    // each of them gets three samples and turns to porridge. Held at both ends - the near case is then cheaper than\n"
"    // a fixed count, and the far one does not run away with the frame.\n"
"    // THE STEP HAS A FIXED LENGTH IN THE WORLD, and the count follows from it - not the other way round. With the\n"
"    // count leading, a span that grows by a hair pushes it up by one and EVERY sample of that ray moves at once:\n"
"    // across the screen that drew circles, and along TIME it is a cloud that shivers as the player walks into it.\n"
"    // The same fault seen from two sides. Here a longer span only ADDS a step at the far end, where what is left\n"
"    // of the light has already been eaten.\n"
"    //\n"
"    // Shorter than the fixed length when the span is short, so a cloud right in front still gets eight samples;\n"
"    // longer when even the ceiling cannot cover the span. Both of those vary SMOOTHLY with the span, which is\n"
"    // exactly what a whole number never did.\n"
"    float Span = Far - Near;\n"
"    float MaxN = (float)max(Steps, 8u);\n"
"    float Dt   = min(max(StepWorld, 1.0f), Span / 8.0f);\n"
"    Dt         = max(Dt, Span / MaxN);\n"
"    uint  N    = (uint)clamp(ceil(Span / Dt), 1.0f, MaxN);\n"
"    // WHERE THE FIRST STEP STARTS, differently for each pixel. The count above is a WHOLE NUMBER, so somewhere\n"
"    // across the screen it goes from twelve to thirteen - and there every sample of that ray moves at once and\n"
"    // reads the noise somewhere else. With one fixed offset for every pixel that jump lines up along the curve\n"
"    // where the count changes, and that curve - a constant chord through a ball - is a CIRCLE. Between eight and\n"
"    // twenty-four there are seventeen such curves, which is why there were circles of every size at once.\n"
"    //\n"
"    // Scattered, the same jump is noise instead, and noise at half size read back through a filter is not seen.\n"
"    // Not a hash: the R2 sequence, whose offsets spread EVENLY over neighbouring pixels rather than landing where\n"
"    // they like - white noise here would trade the rings for a grain that a cloud shows all too well.\n"
"    float Off = frac(dot(float2(t.xy), float2(0.7548776662f, 0.5698402909f)));\n"
"    [loop] for (uint k = 0u; k < N; k++)\n"
"    {\n"
"        // The LAST step is whatever is left of the span rather than a whole one: a fixed length cannot divide an\n"
"        // arbitrary span, and a step that ran past Far would put smoke behind the wall that stopped the walk.\n"
"        float Walked = (float)k * Dt;\n"
"        float Len    = min(Dt, Span - Walked);\n"
"        if (Len <= 0.0f) break;\n"
"        float3 P = Dir * (Near + Walked + Off * Len);\n"
"        float3 Tint;\n"
"        float  Size;\n"
"        float  T = Thick(P, ListFirst, ListCount, Tint, Size);\n"
"        if (T > 0.0005f)\n"
"        {\n"
"            // The nearest lamp, falling off with distance. Not a shadow: a point in the air has no surface for the\n"
"            // shadow maps to darken, and asking them here would be a map lookup on every step of every pixel.\n"
"            float3 ToLamp = LampView - P;\n"
"            float  Away   = length(ToLamp);\n"
"            float  Reach  = saturate(1.0f - Away / max(LampReach, 1e-3f));\n"
"            // ITS OWN SHADOW. One step towards the lamp, and how much smoke stands in the way there. Without this\n"
"            // a volume is an even grey whatever else is done to it, because nothing inside it is darker than its\n"
"            // edge - and an even grey is exactly what a flat card already was. The step goes by the smoke's own\n"
"            // size, as its thickness does: a cloud's step from a wisp lands in clear air, or far up a column of\n"
"            // wisps, and takes the whole column for what stands in the way.\n"
"            float3 Toward = ToLamp / max(Away, 1e-3f);\n"
"            float3 Ignore;\n"
"            float  IgnoreSize;\n"
"            float  Shade  = min(SelfShade, 2.0f * Size);\n"
"            float  InWay  = PuffAt(P + Toward * Shade, ListFirst, ListCount, Ignore, IgnoreSize);\n"
"            float  Open   = exp(-InWay * Shade);\n"
"            // WHICH WAY the light is scattered. Smoke and steam throw most of the light on FORWARDS, so a cloud\n"
"            // with a lamp behind it glows and the same cloud lit from the front is flat - and that difference is\n"
"            // most of what tells steam from a grey shape. Henyey and Greenstein's, the one these are always\n"
"            // written with; g says how much of it goes forwards.\n"
"            float  Cos    = dot(normalize(Dir), Toward);\n"
"            float  G      = 0.45f;\n"
"            float  Denom  = 1.0f + G * G - 2.0f * G * Cos;\n"
"            float  Phase  = (1.0f - G * G) / (4.0f * 3.14159265f * max(Denom, 1e-3f) * sqrt(max(Denom, 1e-3f)));\n"
"            // Held at one for a cloud seen from the side, so the phase only ever brightens a backlit one rather\n"
"            // than darkening every other: these levels are dark enough already.\n"
"            //\n"
"            // SMOOTHLY held, not with a max. A max breaks where its two sides meet, and the place where they meet\n"
"            // here is a fixed ANGLE from the lamp - which is a cone, and a cone drawn on a screen is a CIRCLE. That\n"
"            // circle was visible across the gas: a clean curved line with nothing in the cloud to explain it.\n"
"            // This is the same soft meeting as a hyperbola: far above one it IS the phase, far below it is one, and\n"
"            // in between it bends instead of turning a corner.\n"
"            float  Lobe   = Phase * 4.0f * 3.14159265f;\n"
"            float  Bend   = Lobe - 1.0f;\n"
"            float  Glow   = 0.5f * (Lobe + 1.0f + sqrt(Bend * Bend + 0.35f));\n"
"            float3 Lit    = Tint * (Ambient + LampHue * LampLit * Reach * Reach * Open * Glow);\n"
"            float  Cover  = 1.0f - exp(-T * Len);\n"
"            Sum   += Clear * Cover * Lit;\n"
"            Clear *= 1.0f - Cover;\n"
"            if (Clear < 0.01f) break;\n"
"        }\n"
"    }\n"
"    }\n"
"    }\n"
"\n"
"    // What the upscaler is told of this pixel (Reactive): the smoke's share of what it hides, so far.\n"
"    float React = (1.0f - Clear) * (float)((ReactiveBits >> 8) & 255u) * (1.0f / 255.0f);\n"
"\n"
"    // The waterfalls, laid in front of the smoke or behind it by where their stretches lie along the ray.\n"
"    [branch] if (FallCount > 0u)\n"
"    {\n"
"        float3 FSum;\n"
"        float  FClear;\n"
"        float  FallMid = WalkFalls(Dir, SceneZ, frac(dot(float2(t.xy), float2(0.7548776662f, 0.5698402909f))),\n"
"                                   FSum, FClear);\n"
"        Sum    = (FallMid < SmokeMid) ? FSum + FClear * Sum : Sum + Clear * FSum;\n"
"        Clear *= FClear;\n"
"        React  = max(React, (1.0f - FClear) * (float)((ReactiveBits >> 16) & 255u) * (1.0f / 255.0f));\n"
"    }\n"
"\n"
"    // The fires, added: dimmed by the smoke in front of them, hiding nothing behind them.\n"
"    float FireReact = 0.0f;\n"
"    // The fires this pixel's ray can meet: its tile's list, as the puffs' above - read only in a frame with fires,\n"
"    // whose lists hold every tile's two numbers.\n"
"    uint FireFirst = 0u, FireHeld = 0u;\n"
"    [branch] if (FireCount > 0u)\n"
"    {\n"
"        uint TileAt = (t.x / RDX_CLOUD_TILE) + (t.y / RDX_CLOUD_TILE) * TileCols;\n"
"        FireFirst = FireTiles[TileAt * 2u];\n"
"        FireHeld  = FireTiles[TileAt * 2u + 1u];\n"
"    }\n"
"    [branch] if (FireHeld > 0u)\n"
"    {\n"
"        float3 Flame;\n"
"        float  FireMid = WalkFires(Dir, SceneZ, frac(dot(float2(t.xy), float2(0.7548776662f, 0.5698402909f))),\n"
"                                   FireFirst, FireHeld, Flame);\n"
"        // The sparks with the flames: lit points that hide nothing, dimmed by the smoke in front as they are.\n"
"        if ((ReactiveBits & SPARKS_ON) != 0u)\n"
"            Flame += Sparks(EyeWorld, ToWorld(Dir), SceneZ, FireFirst, FireHeld);\n"
"        // And the discharges in the balls of glow: light as the flames' is, dimmed by the same smoke.\n"
"        Flame += Arcs(EyeWorld, RayWorld, FireFirst, FireHeld);\n"
"        Sum += (FireMid < SmokeMid) ? Flame : Flame * Clear;\n"
"        // The whole body wherever the ray crosses it, not only where a tongue is now: the pixels a tongue has just\n"
"        // left are the ones that trail behind it, and they are inside the body too.\n"
"        if (FireMid < 1e8f)\n"
"            FireReact = max(FIRE_REACTIVE_BODY, saturate(max(Flame.r, max(Flame.g, Flame.b)) * FIRE_REACTIVE_GAIN));\n"
"    }\n"
"\n"
"    // The shafts and the weapon lamps' beams are ADDED. Strictly the smoke in front of a beam should dim it, and\n"
"    // here it does not: what that costs is a beam a little too bright through thick smoke, and what it saves is\n"
"    // walking the two together at every step of both.\n"
"    Out[t.xy] = float4(Sum + Shafts(Dir, SceneZ, t.xy) + OwnBeams(SceneZ, t.xy), 1.0f - Clear);\n"
"\n"
"#if RDX_TYPED_LOAD\n"
"    // The mask is at full size and this walk at half: each pixel here answers for the four it covers.\n"
"    React = saturate(max(React, FireReact));\n"
"    if ((ReactiveBits & 1u) != 0u && React > (0.5f / 255.0f))\n"
"    {\n"
"        uint2 Dim;\n"
"        Reactive.GetDimensions(Dim.x, Dim.y);\n"
"        const float R = React * REACTIVE_MAX;\n"
"        [unroll] for (uint k = 0u; k < 4u; k++)\n"
"        {\n"
"            const uint2 P = t.xy * 2u + uint2(k & 1u, k >> 1u);\n"
"            if (P.x < Dim.x && P.y < Dim.y)\n"
"                Reactive[P] = max(Reactive[P], R);\n"
"        }\n"
"    }\n"
"#endif\n"
"}\n";

UBOOL FVolumetrics::Init(ID3D12Device* Device)
{
    if (!Device)
        return 0;
    if (Walk)
        return 1;

    D3D12_DESCRIPTOR_RANGE Ranges[5];
    appMemzero(Ranges, sizeof(Ranges));
    Ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors     = 1;
    Ranges[0].BaseShaderRegister = 1;                 // t1: the scene's depth
    Ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Ranges[1].NumDescriptors     = 1;
    Ranges[1].BaseShaderRegister = 0;                 // u0: the result
    Ranges[1].OffsetInDescriptorsFromTableStart = 1;
    // Slot 2 of the heap is the result AS A TEXTURE TO READ, which the laying pass uses and this one does not -
    // so the cubes go at 3 rather than filling the first free number.
    Ranges[2].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[2].NumDescriptors     = 1;
    Ranges[2].BaseShaderRegister = 3;                 // t3: the level's shadow cubes
    Ranges[2].OffsetInDescriptorsFromTableStart = 3;
    Ranges[3].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Ranges[3].NumDescriptors     = 1;
    Ranges[3].BaseShaderRegister = 1;                 // u1: the upscaler's reactive mask, where the fires burn
    Ranges[3].OffsetInDescriptorsFromTableStart = 4;
    // Slot 5 is the heat haze's copy of the picture, which only the haze reads: the weapon lamps' maps go at 6.
    Ranges[4].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[4].NumDescriptors     = 1;
    Ranges[4].BaseShaderRegister = 10;                // t10: the weapon lamps' maps
    Ranges[4].OffsetInDescriptorsFromTableStart = 6;

    // The walk's numbers come in a buffer of their own (ConstUpload) and not as constants of the root signature: a
    // root signature holds sixty-four words, each such constant is one of them and each buffer below two, and the
    // walk's numbers beside its buffers do not fit in that.
    D3D12_ROOT_PARAMETER P[10];
    appMemzero(P, sizeof(P));
    P[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    P[0].Descriptor.ShaderRegister = 0;               // b0: the walk's numbers
    P[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[1].Descriptor.ShaderRegister = 0;               // t0: the puffs, straight from the root signature
    P[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    P[2].DescriptorTable.NumDescriptorRanges = 5;
    P[2].DescriptorTable.pDescriptorRanges   = Ranges;
    P[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[3].Descriptor.ShaderRegister = 2;               // t2: the lamps the shafts come from
    P[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[4].Descriptor.ShaderRegister = 4;               // t4: the waterfalls' columns
    P[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[5].Descriptor.ShaderRegister = 5;               // t5: the fires' flame bodies
    P[6].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[6].Descriptor.ShaderRegister = 6;               // t6: the puffs' lists by tile
    P[7].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[7].Descriptor.ShaderRegister = 7;               // t7: the fires' lists by tile
    P[8].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[8].Descriptor.ShaderRegister = 8;               // t8: the shafts' lamps' lists by tile
    P[9].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[9].Descriptor.ShaderRegister = 9;               // t9: the weapon lamps' rows

    // Reversed depth, as the shadow maps are drawn: lit where the reference is at or past what is stored.
    D3D12_STATIC_SAMPLER_DESC Cmp;
    appMemzero(&Cmp, sizeof(Cmp));
    Cmp.Filter         = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    Cmp.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Cmp.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Cmp.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Cmp.ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    Cmp.MaxLOD         = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 10;
    Desc.pParameters       = P;
    Desc.NumStaticSamplers = 1;
    Desc.pStaticSamplers   = &Cmp;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("volumetrics: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    if (FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("volumetrics: the root signature could not be made"));
        return 0;
    }

    // The fires keep the larger of their own value and the reactive mask's (Record): a typed load of an 8-bit format,
    // which a device has only as an option. Without it the shader is made without the read, and the mask left alone -
    // a shader that asks for it would not even become a pipeline there.
    D3D12_FEATURE_DATA_D3D12_OPTIONS Opt;
    appMemzero(&Opt, sizeof(Opt));
    D3D12_FEATURE_DATA_FORMAT_SUPPORT R8;
    appMemzero(&R8, sizeof(R8));
    R8.Format = DXGI_FORMAT_R8_UNORM;
    bTypedLoad = SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &Opt, sizeof(Opt))) &&
                 Opt.TypedUAVLoadAdditionalFormats &&
                 SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &R8, sizeof(R8))) &&
                 (R8.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
    // How far a flame reaches from its body is RenderMath's: the tiles a body is listed in go by the same numbers
    // (RdxMath::FlameBodyBall), and the walk shows nothing of a body outside them.
    char Tile[32], GrainSize[32], GrainLods[32], FireReach[32], BallOut[32], BallTall[32], SparkFly[32];
    char OwnHead[32], OwnRows[32], OwnStep[32], OwnSteps[32];
    sprintf_s(OwnHead, "%iu", (INT)RDX_OWN_HEAD_ROWS);
    sprintf_s(OwnRows, "%iu", (INT)RDX_OWN_LAMP_ROWS);
    sprintf_s(OwnStep, "%.3ff", RdxMath::OwnBeamStepWorld);
    sprintf_s(OwnSteps, "%iu", (INT)RdxMath::OwnBeamStepsMost);
    sprintf_s(Tile, "%iu", (INT)RdxMath::CloudTile);
    sprintf_s(GrainSize, "%.3ff", RdxMath::CloudGrainSize);
    sprintf_s(GrainLods, "%i.0f", (INT)RdxMath::CloudGrainLods);
    sprintf_s(FireReach, "%.3ff", RdxMath::FireWalkReach);
    sprintf_s(BallOut, "%.3ff", RdxMath::FireBallOut);
    sprintf_s(BallTall, "%.3ff", RdxMath::FireBallTall);
    sprintf_s(SparkFly, "%.3ff", RdxMath::FireSparkFly);
    const D3D_SHADER_MACRO Macros[] = { { "RDX_TYPED_LOAD", bTypedLoad ? "1" : "0" },
                                        { "RDX_CLOUD_TILE", Tile },
                                        { "RDX_CLOUD_GRAIN_SIZE", GrainSize },
                                        { "RDX_CLOUD_GRAIN_LODS", GrainLods },
                                        { "RDX_FIRE_REACH", FireReach },
                                        { "RDX_FIRE_BALL_OUT", BallOut },
                                        { "RDX_FIRE_BALL_TALL", BallTall },
                                        { "RDX_SPARK_FLY", SparkFly },
                                        { "RDX_OWN_ROWS_HEAD", OwnHead },
                                        { "RDX_OWN_ROWS_LAMP", OwnRows },
                                        { "RDX_OWN_DUST_STEP", OwnStep },
                                        { "RDX_OWN_DUST_STEPS", OwnSteps },
                                        { nullptr, nullptr } };
    if (!bTypedLoad)
        RdxLog(TEXT("volumetrics: no typed loads of 8-bit formats - the fires leave the upscaler's reactive mask alone"));

    TComRef<ID3DBlob> Cs;
    const UINT Flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (FAILED(RdxCompile(GVolumetricShader, strlen(GVolumetricShader), "Volumetrics", Macros, nullptr,
                          "WalkCS", "cs_5_0", Flags, 0, Cs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("volumetrics: the walk shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature   = Root.Get();
    Pso.CS.pShaderBytecode = Cs->GetBufferPointer();
    Pso.CS.BytecodeLength  = Cs->GetBufferSize();
    if (FAILED(Device->CreateComputePipelineState(&Pso, __uuidof(ID3D12PipelineState), Walk.VoidSlot())))
    {
        RdxLog(TEXT("volumetrics: the walk pipeline could not be made"));
        return 0;
    }
    return 1;
}

void FVolumetrics::Shutdown()
{
    ReleaseTargets();
    Walk.Release();
    Root.Release();
}

void FVolumetrics::NameResources()
{
    auto Name = [](ID3D12Object* Object, const WCHAR* Text) { if (Object) Object->SetName(Text); };
    Name(Result.Get(), L"WalkResult");
    Name(Heap.Get(),   L"WalkViews");
    for (INT i = 0; i < FRAMES; i++)
    {
        for (INT v = 0; v < SlotCount[i]; v++)
        {
            const FViewSlot& S = Slots[i][v];
            Name(S.Upload.Get(),         L"WalkPuffs");
            Name(S.TileUpload.Get(),     L"WalkTiles");
            Name(S.LampUpload.Get(),     L"WalkLamps");
            Name(S.LampTileUpload.Get(), L"WalkLampTiles");
            Name(S.FallUpload.Get(),     L"WalkFalls");
            Name(S.FireUpload.Get(),     L"WalkFires");
            Name(S.FireTileUpload.Get(), L"WalkFireTiles");
            Name(S.ConstUpload.Get(),    L"WalkNumbers");
            Name(S.OwnUpload.Get(),      L"WalkOwnLamps");
        }
    }
}

void FVolumetrics::FreeSlot(FViewSlot& S)
{
    TComRef<ID3D12Resource>* const Buffers[9] = { &S.Upload, &S.TileUpload, &S.LampUpload, &S.LampTileUpload,
                                                  &S.FallUpload, &S.FireUpload, &S.FireTileUpload, &S.ConstUpload,
                                                  &S.OwnUpload };
    void** const Maps[9] = { &S.Mapped, &S.TileMapped, &S.LampMapped, &S.LampTileMapped,
                             &S.FallMapped, &S.FireMapped, &S.FireTileMapped, &S.ConstMapped, &S.OwnMapped };
    for (INT b = 0; b < 9; b++)
    {
        if (*Buffers[b] && *Maps[b])
            (*Buffers[b])->Unmap(0, nullptr);
        *Maps[b] = nullptr;
        Buffers[b]->Release();
    }
    S.Room = S.TileRoom = S.LampTileRoom = S.FireRoom = S.FireTileRoom = 0;
}

UBOOL FVolumetrics::MakeSlot(ID3D12Device* Device, FViewSlot& S)
{
    D3D12_HEAP_PROPERTIES Up;
    appMemzero(&Up, sizeof(Up));
    Up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.Format           = DXGI_FORMAT_UNKNOWN;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // The tile lists' first room: every tile's own two numbers at the least, whatever the frame's puffs, fires or
    // lamps. They are bound by address alone, with no size for the GPU to hold a read to, and a read past a buffer's
    // end is a page fault there - the device lost. No frame's walk may depend on the lists having grown for an
    // earlier frame's smoke: every tile's two numbers are there to read from the first frame.
    const INT FirstTiles = RdxMath::CloudTileFirstRoom(HalfW, HalfH, FIRST_TILE_UINTS);
    struct FWant
    {
        TComRef<ID3D12Resource>* Buffer;
        void**                   At;
        UINT64                   Bytes;
        const TCHAR*             What;
    };
    // A constant buffer is read in pieces of 256 bytes: the walk's numbers' is as long as the next of those.
    const FWant Wants[9] = {
        { &S.Upload, &S.Mapped, (UINT64)(FIRST_PUFFS * PUFF_FLOATS * sizeof(FLOAT)), TEXT("the puffs' list") },
        { &S.TileUpload, &S.TileMapped, (UINT64)FirstTiles * sizeof(UINT), TEXT("the puffs' tile lists") },
        { &S.LampUpload, &S.LampMapped, (UINT64)(MAX_LAMPS * LAMP_FLOATS * sizeof(FLOAT)), TEXT("the lamps' list") },
        { &S.LampTileUpload, &S.LampTileMapped, (UINT64)FirstTiles * sizeof(UINT), TEXT("the lamps' tile lists") },
        { &S.FallUpload, &S.FallMapped, (UINT64)(MAX_FALLS * FALL_FLOATS * sizeof(FLOAT)),
          TEXT("the waterfalls' list") },
        { &S.FireUpload, &S.FireMapped, (UINT64)(FIRST_FIRES * FIRE_FLOATS * sizeof(FLOAT)), TEXT("the fires' list") },
        { &S.FireTileUpload, &S.FireTileMapped, (UINT64)FirstTiles * sizeof(UINT), TEXT("the fires' tile lists") },
        { &S.ConstUpload, &S.ConstMapped, (UINT64)((WALK_CONSTANTS * sizeof(UINT) + 255) / 256) * 256,
          TEXT("the walk's numbers") },
        { &S.OwnUpload, &S.OwnMapped, (UINT64)(OWN_FLOATS * sizeof(FLOAT)), TEXT("the weapon lamps' rows") } };
    const D3D12_RANGE Nothing = { 0, 0 };
    for (INT b = 0; b < 9; b++)
    {
        Bd.Width = Wants[b].Bytes;
        if (FAILED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, __uuidof(ID3D12Resource), Wants[b].Buffer->VoidSlot())) ||
            FAILED((*Wants[b].Buffer)->Map(0, &Nothing, Wants[b].At)) || !*Wants[b].At)
        {
            RdxLog(TEXT("volumetrics: %s could not be made for a view's walk"), Wants[b].What);
            FreeSlot(S);
            return 0;
        }
    }
    S.Room         = FIRST_PUFFS;
    S.TileRoom     = FirstTiles;
    S.LampTileRoom = FirstTiles;
    S.FireRoom     = FIRST_FIRES;
    S.FireTileRoom = FirstTiles;
    return 1;
}

FVolumetrics::FViewSlot* FVolumetrics::ViewSlot(ID3D12Device* Device, INT Frame, INT View)
{
    if (Frame < 0 || Frame >= FRAMES || View < 0 || !Result)
        return nullptr;
    if (View < SlotCount[Frame])
        return &Slots[Frame][View];
    if (!Device)
        return nullptr;
    // As many as the view's number asks: the list is moved where it has to grow, and what it grows by starts empty.
    const INT        Wanted = View + 1;
    FViewSlot* const More   = (FViewSlot*)appRealloc(Slots[Frame], (DWORD)(Wanted * sizeof(FViewSlot)),
                                                     TEXT("RdxWalkSlots"));
    if (!More)
        return nullptr;
    Slots[Frame] = More;
    appMemzero((void*)(More + SlotCount[Frame]), (size_t)(Wanted - SlotCount[Frame]) * sizeof(FViewSlot));
    for (INT v = SlotCount[Frame]; v < Wanted; v++)
    {
        if (!MakeSlot(Device, More[v]))
            return nullptr;             // the count stays where it got to: the view's next walk tries again
        SlotCount[Frame] = v + 1;
    }
    if (View > 0)
    {
        static INT Said = 0;
        if (Said++ < 12)
            RdxLog(TEXT("volumetrics: frame slot %i now holds the lists of %i views"), Frame, SlotCount[Frame]);
    }
    return &More[View];
}

void FVolumetrics::ReleaseTargets()
{
    for (INT i = 0; i < FRAMES; i++)
    {
        for (INT v = 0; v < SlotCount[i]; v++)
            FreeSlot(Slots[i][v]);
        if (Slots[i])
            appFree(Slots[i]);
        Slots[i]     = nullptr;
        SlotCount[i] = 0;
    }
    Result.Release();
    Heap.Release();
    // The heat haze's copy goes with the heap its view sat in: made again, its view is written into the new one.
    HazeCopy.Release();
    HazeW = 0;
    HazeH = 0;
    HalfW = HalfH = 0;
    SceneW = SceneH = 0;
    DepthSeen        = nullptr;
    FurtherDepthSeen = nullptr;
    CubeSeen         = nullptr;
    bCubeStale       = 0;
    for (INT Table = 0; Table < 2; Table++)
    {
        ReactiveSeen[Table]   = nullptr;
        bReactiveStale[Table] = 0;
    }
}

UBOOL FVolumetrics::EnsureTargets(ID3D12Device* Device, INT InSceneW, INT InSceneH)
{
    if (!Device || InSceneW <= 1 || InSceneH <= 1)
        return 0;
    if (Result && SceneW == InSceneW && SceneH == InSceneH)
        return 1;
    ReleaseTargets();
    SceneW = InSceneW;
    SceneH = InSceneH;
    HalfW  = TargetSide(InSceneW);
    HalfH  = TargetSide(InSceneH);

    D3D12_HEAP_PROPERTIES Hp;
    appMemzero(&Hp, sizeof(Hp));
    Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Td;
    appMemzero(&Td, sizeof(Td));
    Td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Td.Width            = (UINT64)HalfW;
    Td.Height           = (UINT)HalfH;
    Td.DepthOrArraySize = 1;
    Td.MipLevels        = 1;
    Td.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Td.SampleDesc.Count = 1;
    Td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // A branch that cannot be tried is a branch nobody knows the state of: the marker SecondRevision-fail-alloc.txt
    // with "volumetrics" in it makes this refuse, and the game's own flat sprites have to come back.
    if (RdxForcedFailure(TEXT("volumetrics")) ||
        FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Td,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               __uuidof(ID3D12Resource), Result.VoidSlot())))
    {
        RdxLog(TEXT("volumetrics: the %ix%i result could not be made - the pass is skipped"), HalfW, HalfH);
        ReleaseTargets();
        return 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC Hd;
    appMemzero(&Hd, sizeof(Hd));
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // Depth, the result to write and to read, the cubes, the mask, the haze's copy, the weapon lamps' maps - and all
    // but the haze's copy over again for the further views' walks.
    Hd.NumDescriptors = HEAP_VIEWS;
    Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(Device->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), Heap.VoidSlot())))
    {
        RdxLog(TEXT("volumetrics: its view heap could not be made - the pass is skipped"));
        ReleaseTargets();
        return 0;
    }
    const UINT Inc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    HeapStep = Inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
    appMemzero(&Ud, sizeof(Ud));
    Ud.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    // And the same texture to READ, for laying it over the picture.
    D3D12_SHADER_RESOURCE_VIEW_DESC Rd;
    appMemzero(&Rd, sizeof(Rd));
    Rd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Rd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Rd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Rd.Texture2D.MipLevels     = 1;
    // In the world's table and in the further views': every walk of a frame writes the one result, in its turn.
    for (INT Table = 0; Table < 2; Table++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE Uav = Heap->GetCPUDescriptorHandleForHeapStart();
        Uav.ptr += (UINT64)(Table * FURTHER + 1) * Inc;
        Device->CreateUnorderedAccessView(Result.Get(), nullptr, &Ud, Uav);
        D3D12_CPU_DESCRIPTOR_HANDLE Srv = Heap->GetCPUDescriptorHandleForHeapStart();
        Srv.ptr += (UINT64)(Table * FURTHER + 2) * Inc;
        Device->CreateShaderResourceView(Result.Get(), &Rd, Srv);
        // The reactive mask's place, a view of nothing until there is a mask to write (Record).
        WriteReactiveView(Device, nullptr, Table);
    }
    // And the cubes': the walk's table holds it whether or not a frame has shafts, and a place left as the heap was
    // made is a view of whatever lay in that memory before - of textures let go since, on which the device hangs.
    WriteCubeView(Device, nullptr, nullptr);
    WriteOwnMapsView(Device, nullptr, nullptr, -1);

    // The world's view's lists and numbers, a set for each frame in flight, kept mapped: written once a frame. A
    // further view's are made at its first walk (ViewSlot).
    for (INT i = 0; i < FRAMES; i++)
    {
        if (!ViewSlot(Device, i, 0))
        {
            RdxLog(TEXT("volumetrics: a frame's lists could not be made - the pass is skipped"));
            ReleaseTargets();
            return 0;
        }
    }
    RdxLog(TEXT("volumetrics: the walk runs at %ix%i, half the scene's %ix%i"), HalfW, HalfH, SceneW, SceneH);
    return 1;
}

UBOOL FVolumetrics::GrowList(ID3D12Device* Device, TComRef<ID3D12Resource>& Buffer, void*& At, INT& Room, INT Wanted,
                             INT Stride, const TCHAR* What)
{
    if (Wanted <= Room)
        return 1;
    if (!Device || !Buffer)
        return 0;
    // Twice the room at least, so a cloud that grows by a puff a frame does not make a buffer a frame.
    const INT Larger = Max(Wanted, Room * 2);
    D3D12_HEAP_PROPERTIES Up;
    appMemzero(&Up, sizeof(Up));
    Up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = (UINT64)Larger * (UINT64)Stride;
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.Format           = DXGI_FORMAT_UNKNOWN;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* Made   = nullptr;
    void*           MadeAt = nullptr;
    D3D12_RANGE     None   = { 0, 0 };
    if (RdxForcedFailure(TEXT("volumetrics lists")) ||
        FAILED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, __uuidof(ID3D12Resource), (void**)&Made)) ||
        FAILED(Made->Map(0, &None, &MadeAt)) || !MadeAt)
    {
        if (Made)
            Made->Release();
        RdxLog(TEXT("volumetrics: %s could not be made for %i (it holds %i)"), What, Larger, Room);
        return 0;
    }
    Buffer->Unmap(0, nullptr);
    *Buffer.Slot() = Made;                  // the smaller one is let go, the larger one is the slot's from here
    At = MadeAt;
    static INT Said = 0;
    if (Said++ < 24)
        RdxLog(TEXT("volumetrics: %s now holds %i (it held %i)"), What, Larger, Room);
    Room = Larger;
    return 1;
}

void FVolumetrics::WriteReactiveView(ID3D12Device* Device, ID3D12Resource* Mask, INT Table)
{
    D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
    At.ptr += (UINT64)(Table * FURTHER + 4) * HeapStep;
    D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
    appMemzero(&Ud, sizeof(Ud));
    Ud.Format        = DXGI_FORMAT_R8_UNORM;
    Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Device->CreateUnorderedAccessView(Mask, nullptr, &Ud, At);
    ReactiveSeen[Table]   = Mask;
    bReactiveStale[Table] = 0;
}

void FVolumetrics::WriteCubeView(ID3D12Device* Device, ID3D12Resource* Cubes,
                                 const D3D12_SHADER_RESOURCE_VIEW_DESC* View)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC None;
    appMemzero(&None, sizeof(None));
    None.Format                     = DXGI_FORMAT_R32_FLOAT;
    None.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    None.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    None.TextureCubeArray.MipLevels = 1;
    None.TextureCubeArray.NumCubes  = 1;
    const UBOOL bCubes = (Cubes && View) ? 1 : 0;
    // In the world's table and in the further views': the same cubes for every walk of a frame.
    for (INT Table = 0; Table < 2; Table++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
        At.ptr += (UINT64)(Table * FURTHER + 3) * HeapStep;
        Device->CreateShaderResourceView(bCubes ? Cubes : nullptr, bCubes ? View : &None, At);
    }
    CubeSeen   = bCubes ? Cubes : nullptr;
    bCubeStale = 0;
}

void FVolumetrics::WriteOwnMapsView(ID3D12Device* Device, ID3D12Resource* Maps,
                                    const D3D12_SHADER_RESOURCE_VIEW_DESC* View, INT Made)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC None;
    appMemzero(&None, sizeof(None));
    None.Format                   = DXGI_FORMAT_R32_FLOAT;
    None.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    None.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    None.Texture2DArray.MipLevels = 1;
    None.Texture2DArray.ArraySize = 1;
    const UBOOL bMaps = (Maps && View) ? 1 : 0;
    // In the world's table and in the further views': the same maps for every walk of a frame.
    for (INT Table = 0; Table < 2; Table++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
        At.ptr += (UINT64)(Table * FURTHER + 6) * HeapStep;
        Device->CreateShaderResourceView(bMaps ? Maps : nullptr, bMaps ? View : &None, At);
    }
    OwnMapsSeen     = bMaps ? Maps : nullptr;
    OwnMapsSeenMade = bMaps ? Made : -1;
    bOwnMapsStale   = 0;
}

UBOOL FVolumetrics::Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Depth,
                           DXGI_FORMAT DepthFormat, const void* Puffs, INT PuffCount, const UINT* Tiles, INT TileUints,
                           INT TileCols, const FWalk& W, INT FrameSlot, ID3D12Resource* Cubes,
                           const D3D12_SHADER_RESOURCE_VIEW_DESC* CubeView, ID3D12Resource* Reactive, INT View)
{
    bWrote     = 0;
    bFiresHeld = 1;
    bLampsHeld = 1;
    if (!Cmd || !Device || !Walk || !Result || !Heap || !Depth)
        return 1;
    if (PuffCount < 0 || !Puffs)
        PuffCount = 0;
    // The view's own lists and numbers in this frame slot. Where a further view's cannot be made, nothing of that
    // view is walked - and its smoke and its fires are said not to be.
    FViewSlot* const Held = ViewSlot(Device, (FrameSlot >= 0 && FrameSlot < FRAMES) ? FrameSlot : 0, Max(View, 0));
    if (!Held)
    {
        bFiresHeld = (W.Fires && W.FireCount > 0) ? 0 : 1;
        bLampsHeld = (W.Lamps && W.LampCount > 0) ? 0 : 1;
        return (PuffCount > 0) ? 0 : 1;
    }
    FViewSlot& S     = *Held;
    const INT  Table = (View > 0) ? 1 : 0;
    // The frame's puffs and their lists, all of them: the slot's buffers are made as large as the frame needs. Where
    // that fails the smoke is not walked at all - a part of it would be a cloud with holes.
    UBOOL bSmoke = 1;
    if (PuffCount > 0 &&
        (!Tiles || TileUints <= 0 || TileCols <= 0 || !S.Upload || !S.TileUpload ||
         !GrowList(Device, S.Upload, S.Mapped, S.Room, PuffCount, PUFF_FLOATS * sizeof(FLOAT),
                   TEXT("the puffs' list")) ||
         !GrowList(Device, S.TileUpload, S.TileMapped, S.TileRoom, TileUints, sizeof(UINT),
                   TEXT("the puffs' tile lists"))))
    {
        PuffCount = 0;
        bSmoke    = 0;
    }
    // The shafts need the cubes and a view of them; without either there are no shafts, and with no puffs
    // either there is nothing for this pass to do.
    INT Lamps = (W.Lamps && Cubes && CubeView) ? Clamp(W.LampCount, 0, (INT)MAX_LAMPS) : 0;
    // Their lists by tile, as long as the frame needs. Where that fails there are no shafts - from some of the lamps
    // they would be shafts that come and go with the frame.
    if (Lamps > 0 &&
        (!W.LampTiles || W.LampTileUints <= 0 || !S.LampTileUpload ||
         !GrowList(Device, S.LampTileUpload, S.LampTileMapped, S.LampTileRoom, W.LampTileUints, sizeof(UINT),
                   TEXT("the lamps' tile lists"))))
    {
        Lamps      = 0;
        bLampsHeld = 0;
    }
    const INT FallCount = W.Falls ? Clamp(W.FallCount, 0, (INT)MAX_FALLS) : 0;
    // The frame's fires, all of them, and their lists by tile: there is no most fires a frame may have, and the
    // slot's lists are made as long as the frame needs. Where that fails none of them is walked - some of them would
    // be a fire with flames missing, and which ones would change with the frame.
    INT FireCount = (W.Fires && W.FireCount > 0) ? W.FireCount : 0;
    if (FireCount > 0 &&
        (!W.FireTiles || W.FireTileUints <= 0 || !S.FireUpload || !S.FireTileUpload ||
         !GrowList(Device, S.FireUpload, S.FireMapped, S.FireRoom, FireCount, FIRE_FLOATS * sizeof(FLOAT),
                   TEXT("the fires' list")) ||
         !GrowList(Device, S.FireTileUpload, S.FireTileMapped, S.FireTileRoom, W.FireTileUints, sizeof(UINT),
                   TEXT("the fires' tile lists"))))
    {
        FireCount  = 0;
        bFiresHeld = 0;
    }
    // The weapon lamps' beams: their rows, with their maps and a view of them.
    const INT OwnCount = (W.Own && W.OwnMaps && W.OwnMapsView) ? Clamp(W.OwnCount, 0, (INT)RDX_OWN_LAMPS) : 0;
    if (PuffCount <= 0 && Lamps <= 0 && FallCount <= 0 && FireCount <= 0 && OwnCount <= 0)
        return bSmoke;
    if (!S.Mapped || !S.TileMapped || !S.LampMapped || !S.LampTileMapped || !S.FallMapped || !S.FireMapped ||
        !S.FireTileMapped || !S.ConstMapped || !S.OwnMapped)
        return bSmoke;
    if (PuffCount > 0)
    {
        appMemcpy(S.Mapped, Puffs, (size_t)PuffCount * PUFF_FLOATS * sizeof(FLOAT));
        appMemcpy(S.TileMapped, Tiles, (size_t)TileUints * sizeof(UINT));
    }
    if (Lamps > 0)
    {
        appMemcpy(S.LampMapped, W.Lamps, (size_t)Lamps * LAMP_FLOATS * sizeof(FLOAT));
        appMemcpy(S.LampTileMapped, W.LampTiles, (size_t)W.LampTileUints * sizeof(UINT));
    }
    if (FallCount > 0)
        appMemcpy(S.FallMapped, W.Falls, (size_t)FallCount * FALL_FLOATS * sizeof(FLOAT));
    if (FireCount > 0)
    {
        appMemcpy(S.FireMapped, W.Fires, (size_t)FireCount * FIRE_FLOATS * sizeof(FLOAT));
        appMemcpy(S.FireTileMapped, W.FireTiles, (size_t)W.FireTileUints * sizeof(UINT));
    }
    if (OwnCount > 0)
        appMemcpy(S.OwnMapped, W.Own, (size_t)OWN_FLOATS * sizeof(FLOAT));

    // A texture whose view the heap holds was made again since: every view anew (ViewGeneration.h).
    if (ViewsAt != RdxViewGeneration())
    {
        ViewsAt           = RdxViewGeneration();
        DepthSeen         = nullptr;
        FurtherDepthSeen  = nullptr;
        bCubeStale        = 1;
        bOwnMapsStale     = 1;
        bReactiveStale[0] = bReactiveStale[1] = 1;
    }
    // The cubes' view: made when the shafts' cubes change, and when they were made anew or let go since - a view of
    // nothing then, until a frame has shafts again. Cubes let go must not stay in the table (the shadows switched off).
    if ((Lamps > 0 && CubeSeen != Cubes) || bCubeStale)
        WriteCubeView(Device, (Lamps > 0) ? Cubes : nullptr, CubeView);
    // The weapon lamps' maps' the same way, and again when they were made again (FShadowMaps::SpotsMade) - perhaps at
    // the very address of the ones before.
    if ((OwnCount > 0 && (OwnMapsSeen != W.OwnMaps || OwnMapsSeenMade != W.OwnMapsMade)) || bOwnMapsStale)
        WriteOwnMapsView(Device, (OwnCount > 0) ? W.OwnMaps : nullptr, W.OwnMapsView, W.OwnMapsMade);

    // The depth's view, in the view's own table. Made when the resource changes and not every frame: writing a
    // descriptor a heap the GPU may still be reading is exactly the kind of thing that works until it does not.
    ID3D12Resource*& DepthHeld = Table ? FurtherDepthSeen : DepthSeen;
    if (DepthHeld != Depth)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
        appMemzero(&Sd, sizeof(Sd));
        Sd.Format                  = DepthFormat;
        Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Sd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE Place = Heap->GetCPUDescriptorHandleForHeapStart();
        Place.ptr += (UINT64)(Table * FURTHER) * HeapStep;
        Device->CreateShaderResourceView(Depth, &Sd, Place);
        DepthHeld = Depth;
    }

    // The reactive mask's view, the same way: made again when the mask changes or was made anew (ForgetReactive). The
    // world's view's is the mask only in a frame with something to tell - fires, or smoke or columns with a share
    // asked of them - and a view of nothing otherwise. The further views' is whatever mask they are handed, telling
    // or not: one table serves all of a frame's further views, and a view written for one of them is what another's
    // walk reads when the frame runs. Whether a walk writes the mask is its own numbers' to say.
    const UINT SmokeShare = (PuffCount > 0) ? (UINT)Clamp((INT)(W.SmokeReactive * 255.0f + 0.5f), 0, 255) : 0u;
    const UINT FallShare  = (FallCount > 0) ? (UINT)Clamp((INT)(W.FallReactive * 255.0f + 0.5f), 0, 255) : 0u;
    const UBOOL bTells    = (bTypedLoad && (FireCount > 0 || SmokeShare || FallShare)) ? 1 : 0;
    if (!bTypedLoad || (Table == 0 && !bTells))
        Reactive = nullptr;
    if (ReactiveSeen[Table] != Reactive || bReactiveStale[Table])
        WriteReactiveView(Device, Reactive, Table);

    UINT C[WALK_CONSTANTS];
    appMemzero(C, sizeof(C));
    FLOAT* F = (FLOAT*)C;
    C[0] = (UINT)HalfW; C[1] = (UINT)HalfH; C[2] = (UINT)PuffCount; C[3] = (UINT)((W.Steps > 4) ? W.Steps : 4);
    F[4] = W.Screen[0]; F[5] = W.Screen[1]; F[6] = W.Focal; F[7] = W.Time;
    F[8] = W.EngPerPixel[0]; F[9] = W.EngPerPixel[1]; F[10] = W.Grain; C[11] = (UINT)Max(TileCols, 1);
    F[12] = W.EyeWorld[0]; F[13] = W.EyeWorld[1]; F[14] = W.EyeWorld[2]; F[15] = W.Ambient;
    F[16] = W.AxisX[0]; F[17] = W.AxisX[1]; F[18] = W.AxisX[2]; F[19] = W.Rise;
    F[20] = W.AxisY[0]; F[21] = W.AxisY[1]; F[22] = W.AxisY[2]; F[23] = W.Warp;
    F[24] = W.AxisZ[0]; F[25] = W.AxisZ[1]; F[26] = W.AxisZ[2]; F[27] = W.SelfShade;
    F[28] = W.LampView[0]; F[29] = W.LampView[1]; F[30] = W.LampView[2]; F[31] = W.LampReach;
    F[32] = W.LampHue[0]; F[33] = W.LampHue[1]; F[34] = W.LampHue[2]; F[35] = W.LampLit;
    F[36] = W.StepWorld; F[37] = W.ShaftStrength;
    C[38] = (UINT)Lamps; C[39] = (UINT)((W.ShaftSteps > 4) ? W.ShaftSteps : 4);
    F[40] = W.ShaftNear; F[41] = W.ShaftMaxZ; F[42] = W.ShaftBias; F[43] = W.ShaftG;
    C[44] = (UINT)FallCount; F[45] = W.FallDensity; F[46] = W.FallAmbient; F[47] = W.FallRate;
    C[48] = (UINT)FireCount; F[49] = W.FireGlow; F[50] = W.FireRate;
    C[51] = ((Reactive && bTells) ? (1u | (SmokeShare << 8) | (FallShare << 16)) : 0u) | (W.bSparks ? 0x01000000u : 0u);
    F[52] = W.NearPlane[0]; F[53] = W.NearPlane[1]; F[54] = W.NearPlane[2]; F[55] = W.NearPlane[3];
    // The whole target where the view names no part of it.
    const UBOOL bWindow = (W.Window[2] > W.Window[0] && W.Window[3] > W.Window[1]) ? 1 : 0;
    C[56] = (UINT)(bWindow ? Clamp(W.Window[0], 0, HalfW) : 0);
    C[57] = (UINT)(bWindow ? Clamp(W.Window[1], 0, HalfH) : 0);
    C[58] = (UINT)(bWindow ? Clamp(W.Window[2], 0, HalfW) : HalfW);
    C[59] = (UINT)(bWindow ? Clamp(W.Window[3], 0, HalfH) : HalfH);
    C[60] = (UINT)OwnCount; F[61] = W.OwnDust; F[62] = W.OwnForward;

    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetComputeRootSignature(Root.Get());
    Cmd->SetPipelineState(Walk.Get());
    appMemcpy(S.ConstMapped, C, sizeof(C));
    D3D12_GPU_DESCRIPTOR_HANDLE Views = Heap->GetGPUDescriptorHandleForHeapStart();
    Views.ptr += (UINT64)(Table * FURTHER) * HeapStep;
    Cmd->SetComputeRootConstantBufferView(0, S.ConstUpload->GetGPUVirtualAddress());
    Cmd->SetComputeRootShaderResourceView(1, S.Upload->GetGPUVirtualAddress());
    Cmd->SetComputeRootDescriptorTable(2, Views);
    Cmd->SetComputeRootShaderResourceView(3, S.LampUpload->GetGPUVirtualAddress());
    Cmd->SetComputeRootShaderResourceView(4, S.FallUpload->GetGPUVirtualAddress());
    Cmd->SetComputeRootShaderResourceView(5, S.FireUpload->GetGPUVirtualAddress());
    Cmd->SetComputeRootShaderResourceView(6, S.TileUpload->GetGPUVirtualAddress());
    Cmd->SetComputeRootShaderResourceView(7, S.FireTileUpload->GetGPUVirtualAddress());
    Cmd->SetComputeRootShaderResourceView(8, S.LampTileUpload->GetGPUVirtualAddress());
    Cmd->SetComputeRootShaderResourceView(9, S.OwnUpload->GetGPUVirtualAddress());
    Cmd->Dispatch((UINT)((HalfW + 7) / 8), (UINT)((HalfH + 7) / 8), 1);
    bWrote = 1;

    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    B.UAV.pResource = Result.Get();
    Cmd->ResourceBarrier(1, &B);
    return bSmoke;
}

// Laying the walk's result over the finished picture. One triangle over the whole target, read with a linear filter
// because what is laid over is half the size - and smoke has no edge to lose by it.
static const char* GLayShader =
"Texture2D    Cloud : register(t0);\n"
"SamplerState Samp  : register(s0);\n"
"struct VOut { float4 Pos : SV_Position; float2 Uv : TEXCOORD0; };\n"
"VOut LayVS(uint id : SV_VertexID)\n"
"{\n"
"    VOut o;\n"
"    float2 uv = float2((id << 1) & 2, id & 2);\n"
"    o.Uv  = uv;\n"
"    o.Pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);\n"
"    return o;\n"
"}\n"
"// Already multiplied by its own cover, which is what the blend expects: src + dst * (1 - cover).\n"
"float4 LayPS(VOut i) : SV_Target { return Cloud.SampleLevel(Samp, i.Uv, 0.0f); }\n";

UBOOL FVolumetrics::InitComposite(ID3D12Device* Device, DXGI_FORMAT SceneFormat)
{
    if (!Device)
        return 0;
    // For the scene's format, and again when that has changed - the HDR mode switched in the running game: a pipeline
    // that writes another format than its target's is none D3D12 lets draw.
    if (Lay && LayFormat == SceneFormat)
        return 1;
    Lay.Release();
    LayRoot.Release();
    LayFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_DESCRIPTOR_RANGE R;
    appMemzero(&R, sizeof(R));
    R.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    R.NumDescriptors     = 1;
    R.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER P;
    appMemzero(&P, sizeof(P));
    P.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    P.DescriptorTable.NumDescriptorRanges = 1;
    P.DescriptorTable.pDescriptorRanges   = &R;
    P.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC S;
    appMemzero(&S, sizeof(S));
    S.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    S.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    S.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    S.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    S.MaxLOD           = D3D12_FLOAT32_MAX;
    S.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 1;
    Desc.pParameters       = &P;
    Desc.NumStaticSamplers = 1;
    Desc.pStaticSamplers   = &S;
    Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), LayRoot.VoidSlot())))
    {
        RdxLog(TEXT("volumetrics: the composite root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    TComRef<ID3DBlob> Vs, Ps;
    const UINT Flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const size_t Len = strlen(GLayShader);
    if (FAILED(RdxCompile(GLayShader, Len, "VolumetricsLay", nullptr, nullptr, "LayVS", "vs_5_0", Flags, 0,
                          Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GLayShader, Len, "VolumetricsLay", nullptr, nullptr, "LayPS", "ps_5_0", Flags, 0,
                          Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("volumetrics: the composite shaders failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature        = LayRoot.Get();
    Pso.VS.pShaderBytecode    = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength     = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode    = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength     = Ps->GetBufferSize();
    Pso.SampleMask            = UINT_MAX;
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 1;
    Pso.RTVFormats[0]         = SceneFormat;
    Pso.SampleDesc.Count      = 1;
    Pso.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable       = FALSE;
    Pso.DepthStencilState.DepthEnable         = FALSE;
    Pso.DepthStencilState.StencilEnable       = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& B = Pso.BlendState.RenderTarget[0];
    B.BlendEnable           = TRUE;
    B.SrcBlend              = D3D12_BLEND_ONE;
    B.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    B.BlendOp               = D3D12_BLEND_OP_ADD;
    B.SrcBlendAlpha         = D3D12_BLEND_ONE;
    B.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    B.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    B.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), Lay.VoidSlot())))
    {
        RdxLog(TEXT("volumetrics: the composite pipeline could not be made"));
        return 0;
    }
    LayFormat = SceneFormat;
    return 1;
}

void FVolumetrics::RecordComposite(ID3D12GraphicsCommandList* Cmd)
{
    if (!Cmd || !Lay || !Heap || !Result)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(LayRoot.Get());
    Cmd->SetPipelineState(Lay.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE T = Heap->GetGPUDescriptorHandleForHeapStart();
    // Slot 2: the result as a texture. The first two are the depth and the same texture as a UAV.
    T.ptr += 2 * (UINT64)HeapStep;
    Cmd->SetGraphicsRootDescriptorTable(0, T);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

// The heat haze's pass (RecordHaze). The fire's numbers and the view's are its constants, one fire a draw.
static const char* GHazeShader =
"// The air trembling over a standing fire: one fire a draw, over its own part of the screen. The hot air is a\n"
"// column over the flame, widening as it rises: from the flame's upper half to HAZE_TALL times its height. Where\n"
"// the view passes through it in front of what the picture shows, the picture is read a little to one side - by a\n"
"// grain that rises with the air - the more the nearer the view passes to the column's middle, and never from a\n"
"// place that stands in front of the fire.\n"
"#define HAZE_TALL   2.6f\n"
"#define HAZE_PIXELS 5.0f\n"
"#define HAZE_NEAR   160.0f\n"
"Texture2D<float> Depth : register(t0);\n"
"Texture2D        Scene : register(t1);\n"
"SamplerState     Samp  : register(s0);\n"
"cbuffer C : register(b0)\n"
"{\n"
"    float2 Size;     float2 EngPerPixel;\n"
"    float2 Screen;   float  Focal; float Time;\n"
"    float3 EyeWorld; float  Strength;\n"
"    float3 AxisX;    float  Rate;\n"
"    float3 AxisY;    float  Pad1;\n"
"    float3 AxisZ;    float  Pad2;\n"
"    float4 Place;\n"
"    float4 Axes;\n"
"    float4 Bounds;   // the part of the picture this draw may read: its own rectangle, which the copy holds\n"
"};\n"
"float Hash(float3 P)\n"
"{\n"
"    P = frac(P * 0.3183099f + float3(0.1f, 0.2f, 0.3f));\n"
"    P *= 17.0f;\n"
"    return frac(P.x * P.y * P.z * (P.x + P.y + P.z));\n"
"}\n"
"float Noise(float3 P)\n"
"{\n"
"    float3 I = floor(P), F = P - I;\n"
"    float3 W = F * F * (3.0f - 2.0f * F);\n"
"    float A = lerp(Hash(I + float3(0,0,0)), Hash(I + float3(1,0,0)), W.x);\n"
"    float B = lerp(Hash(I + float3(0,1,0)), Hash(I + float3(1,1,0)), W.x);\n"
"    float C = lerp(Hash(I + float3(0,0,1)), Hash(I + float3(1,0,1)), W.x);\n"
"    float D = lerp(Hash(I + float3(0,1,1)), Hash(I + float3(1,1,1)), W.x);\n"
"    return lerp(lerp(A, B, W.y), lerp(C, D, W.y), W.z);\n"
"}\n"
"float4 HazeVS(uint id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 uv = float2((id << 1) & 2, id & 2);\n"
"    return float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"float4 HazePS(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    // The pixel's ray in the world, its outward part held at one: the distance along it is the view depth.\n"
"    const float2 Eng = Pos.xy * EngPerPixel;\n"
"    const float3 Dir = float3((Eng - Screen) / max(Focal, 1e-3f), 1.0f);\n"
"    const float3 R   = AxisX * Dir.x + AxisY * Dir.y + AxisZ * Dir.z;\n"
"    // Where the ray passes nearest the fire's upright axis.\n"
"    const float2 O = EyeWorld.xy - Place.xy;\n"
"    const float  A = dot(R.xy, R.xy);\n"
"    if (A < 1e-8f) discard;\n"
"    const float  s = -dot(O, R.xy) / A;\n"
"    const float  SceneZ = 1.0f / max(Depth.Load(int3(Pos.xy, 0)), 1e-6f);\n"
"    if (s <= 1.0f || s >= SceneZ) discard;\n"
"    const float3 P    = EyeWorld + R * s;\n"
"    const float  Tall = max(Place.w - Place.z, 1.0f);\n"
"    const float  h    = (P.z - Place.z) / Tall;\n"
"    if (h <= 0.5f || h >= HAZE_TALL) discard;\n"
"    const float  Rad  = max(Axes.z, Axes.w) * (1.0f + 0.5f * h);\n"
"    const float  Side = saturate(1.0f - length(O + R.xy * s) / Rad);\n"
"    const float  Up   = saturate((h - 0.5f) * 2.0f) * saturate((HAZE_TALL - h) / (HAZE_TALL - 1.0f));\n"
"    const float  Much = Side * Side * Up * Strength;\n"
"    if (Much <= 0.004f) discard;\n"
"    const float3 G    = float3(P.xy * 0.09f, P.z * 0.06f - Time * Rate);\n"
"    const float2 Bend = float2(Noise(G), Noise(G + float3(17.3f, 5.1f, 9.7f))) - 0.5f;\n"
"    // So many pixels at HAZE_NEAR units, on a picture 1080 high; the same air covers fewer of them further off.\n"
"    const float  Px   = HAZE_PIXELS * (Size.y / 1080.0f) * saturate(HAZE_NEAR / s);\n"
"    float2 At = Pos.xy + Bend * (2.0f * Px * Much);\n"
"    // Never out of the draw's own rectangle: beyond it the copy holds whatever an earlier frame left there.\n"
"    At = clamp(At, Bounds.xy, Bounds.zw);\n"
"    if (1.0f / max(Depth.Load(int3(At, 0)), 1e-6f) < s)\n"
"        At = Pos.xy;\n"
"    return float4(Scene.SampleLevel(Samp, At / Size, 0.0f).rgb, 1.0f);\n"
"}\n";

UBOOL FVolumetrics::InitHaze(ID3D12Device* Device, DXGI_FORMAT SceneFormat)
{
    D3D12_DESCRIPTOR_RANGE R[2];
    appMemzero(R, sizeof(R));
    for (INT i = 0; i < 2; i++)
    {
        R[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        R[i].NumDescriptors     = 1;
        R[i].BaseShaderRegister = (UINT)i;
    }
    D3D12_ROOT_PARAMETER P[3];
    appMemzero(P, sizeof(P));
    P[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P[0].Constants.ShaderRegister = 0;
    P[0].Constants.Num32BitValues = HAZE_CONSTANTS;
    P[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
    for (INT i = 0; i < 2; i++)
    {
        P[1 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        P[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        P[1 + i].DescriptorTable.pDescriptorRanges   = &R[i];
        P[1 + i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC S;
    appMemzero(&S, sizeof(S));
    S.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    S.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    S.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    S.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    S.MaxLOD           = D3D12_FLOAT32_MAX;
    S.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 3;
    Desc.pParameters       = P;
    Desc.NumStaticSamplers = 1;
    Desc.pStaticSamplers   = &S;
    Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), HazeRoot.VoidSlot())))
    {
        RdxLog(TEXT("heat haze: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    TComRef<ID3DBlob> Vs, Ps;
    const UINT   Flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const size_t Len   = strlen(GHazeShader);
    if (FAILED(RdxCompile(GHazeShader, Len, "VolumetricsHaze", nullptr, nullptr, "HazeVS", "vs_5_0", Flags, 0,
                          Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GHazeShader, Len, "VolumetricsHaze", nullptr, nullptr, "HazePS", "ps_5_0", Flags, 0,
                          Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("heat haze: the shaders failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature        = HazeRoot.Get();
    Pso.VS.pShaderBytecode    = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength     = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode    = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength     = Ps->GetBufferSize();
    Pso.SampleMask            = UINT_MAX;
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 1;
    Pso.RTVFormats[0]         = SceneFormat;
    Pso.SampleDesc.Count      = 1;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = FALSE;
    Pso.DepthStencilState.DepthEnable   = FALSE;
    Pso.DepthStencilState.StencilEnable = FALSE;
    Pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), HazePipe.VoidSlot())))
    {
        RdxLog(TEXT("heat haze: the pipeline could not be made"));
        return 0;
    }
    HazePipeFormat = SceneFormat;
    return 1;
}

void FVolumetrics::RecordHaze(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Scene,
                              DXGI_FORMAT SceneFormat, D3D12_CPU_DESCRIPTOR_HANDLE Rtv, const D3D12_RECT& View,
                              const FWalk& W, UBOOL bFurtherView)
{
    const INT FireCount = (W.Fires && W.FireCount > 0) ? W.FireCount : 0;
    if (!Cmd || !Device || !Scene || !Heap || !(bFurtherView ? FurtherDepthSeen : DepthSeen) || FireCount <= 0 ||
        HalfW <= 0 || HalfH <= 0 || View.right <= View.left || View.bottom <= View.top)
        return;
    // The scene's format has changed since the pass was made (the HDR mode switched): made again for the new one.
    if (HazeReady > 0 && HazePipeFormat != SceneFormat)
    {
        HazePipe.Release();
        HazeRoot.Release();
        HazeReady = 0;
    }
    if (HazeReady == 0)
    {
        HazeReady = InitHaze(Device, SceneFormat) ? 1 : -1;
        RdxLog(TEXT("heat haze: the pass %s"),
               (HazeReady > 0) ? TEXT("up - the air over the fires trembles") : TEXT("FAILED - the air stays still"));
    }
    if (HazeReady < 0)
        return;
    const D3D12_RESOURCE_DESC Sd = Scene->GetDesc();
    if (Sd.Width == 0 || Sd.Height == 0)
        return;

    // The engine's pixels a pixel of the picture covers - the walk's own are of its half-size target.
    const FLOAT EngX = W.EngPerPixel[0] * (FLOAT)HalfW / (FLOAT)Sd.Width;
    const FLOAT EngY = W.EngPerPixel[1] * (FLOAT)HalfH / (FLOAT)Sd.Height;
    // Each standing fire's column of hot air as a box, and the box on the screen: the part of the picture its draw
    // may write. A box that reaches behind the eye takes the whole view. Every standing fire of the frame has its
    // own, however many there are - the part is worked out when it is asked for, and no list of them is kept.
    const FLOAT HazeTall = 2.6f;        // the shader's HAZE_TALL
    // Room around a box for the air's bend, which grows with the picture's height as the shader's HAZE_PIXELS does.
    const INT   Margin   = 16 * Max(1, (INT)((Sd.Height + 540) / 1080));
    const FLOAT* const Fires = (const FLOAT*)W.Fires;
    auto PartOf = [&](INT i, D3D12_RECT& Rc) -> UBOOL
    {
        const FLOAT* const F = Fires + i * FIRE_FLOATS;
        if (F[12] > 0.5f)
            return 0;
        const FLOAT Tall = Max(F[3] - F[2], 1.0f);
        const FLOAT Rad  = Max(F[6], F[7]) * (1.0f + 0.5f * HazeTall);
        FLOAT X0 = 1.0e9f, Y0 = 1.0e9f, X1 = -1.0e9f, Y1 = -1.0e9f;
        INT   Front = 0;
        for (INT c = 0; c < 8; c++)
        {
            const FLOAT D[3] = { F[0] + ((c & 1) ? Rad : -Rad) - W.EyeWorld[0],
                                 F[1] + ((c & 2) ? Rad : -Rad) - W.EyeWorld[1],
                                 F[2] + ((c & 4) ? HazeTall : 0.5f) * Tall - W.EyeWorld[2] };
            const FLOAT Vz = D[0] * W.AxisZ[0] + D[1] * W.AxisZ[1] + D[2] * W.AxisZ[2];
            if (Vz <= 1.0f)
                continue;
            const FLOAT Vx = D[0] * W.AxisX[0] + D[1] * W.AxisX[1] + D[2] * W.AxisX[2];
            const FLOAT Vy = D[0] * W.AxisY[0] + D[1] * W.AxisY[1] + D[2] * W.AxisY[2];
            const FLOAT Sx = (W.Screen[0] + W.Focal * Vx / Vz) / EngX;
            const FLOAT Sy = (W.Screen[1] + W.Focal * Vy / Vz) / EngY;
            X0 = Min(X0, Sx); X1 = Max(X1, Sx);
            Y0 = Min(Y0, Sy); Y1 = Max(Y1, Sy);
            Front++;
        }
        if (Front == 0)
            return 0;
        Rc = View;
        if (Front == 8)
        {
            Rc.left   = Max((INT)View.left,   (INT)X0 - Margin);
            Rc.top    = Max((INT)View.top,    (INT)Y0 - Margin);
            Rc.right  = Min((INT)View.right,  (INT)X1 + 1 + Margin);
            Rc.bottom = Min((INT)View.bottom, (INT)Y1 + 1 + Margin);
        }
        return (Rc.right > Rc.left && Rc.bottom > Rc.top) ? 1 : 0;
    };
    INT        N   = 0;
    D3D12_RECT All = { View.right, View.bottom, View.left, View.top };
    for (INT i = 0; i < FireCount; i++)
    {
        D3D12_RECT Rc;
        if (!PartOf(i, Rc))
            continue;
        N++;
        All.left   = Min(All.left, Rc.left);
        All.top    = Min(All.top, Rc.top);
        All.right  = Max(All.right, Rc.right);
        All.bottom = Max(All.bottom, Rc.bottom);
    }
    if (N == 0)
        return;

    // The copy the pass reads: the scene's size and format, made again when those change.
    if (!HazeCopy || HazeW != Sd.Width || HazeH != Sd.Height || HazeFormat != Sd.Format)
    {
        HazeCopy.Release();
        D3D12_HEAP_PROPERTIES Hp;
        appMemzero(&Hp, sizeof(Hp));
        Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC Td = Sd;
        Td.Flags     = D3D12_RESOURCE_FLAG_NONE;
        Td.MipLevels = 1;
        if (FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Td,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   __uuidof(ID3D12Resource), HazeCopy.VoidSlot())))
        {
            HazeReady = -1;
            RdxLog(TEXT("heat haze: its copy of the picture could NOT be made - the air stays still"));
            return;
        }
        HazeW      = Sd.Width;
        HazeH      = Sd.Height;
        HazeFormat = Sd.Format;
        D3D12_SHADER_RESOURCE_VIEW_DESC Vd;
        appMemzero(&Vd, sizeof(Vd));
        Vd.Format                  = SceneFormat;
        Vd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Vd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE H = Heap->GetCPUDescriptorHandleForHeapStart();
        H.ptr += 5 * (SIZE_T)HeapStep;
        Device->CreateShaderResourceView(HazeCopy.Get(), &Vd, H);
    }

    // The picture under all of the fires' parts of the screen, as it stands now.
    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 2; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    B[0].Transition.pResource   = Scene;
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    B[1].Transition.pResource   = HazeCopy.Get();
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    Cmd->ResourceBarrier(2, B);
    D3D12_TEXTURE_COPY_LOCATION Src, Dst;
    appMemzero(&Src, sizeof(Src));
    appMemzero(&Dst, sizeof(Dst));
    Src.pResource = Scene;
    Src.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Dst.pResource = HazeCopy.Get();
    Dst.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX Box = { (UINT)All.left, (UINT)All.top, 0, (UINT)All.right, (UINT)All.bottom, 1 };
    Cmd->CopyTextureRegion(&Dst, (UINT)All.left, (UINT)All.top, 0, &Src, &Box);
    for (INT i = 0; i < 2; i++)
    {
        const D3D12_RESOURCE_STATES Was = B[i].Transition.StateBefore;
        B[i].Transition.StateBefore = B[i].Transition.StateAfter;
        B[i].Transition.StateAfter  = Was;
    }
    Cmd->ResourceBarrier(2, B);

    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(HazeRoot.Get());
    Cmd->SetPipelineState(HazePipe.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE T = Heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE D = T;
    D.ptr += (UINT64)(bFurtherView ? FURTHER : 0) * HeapStep;
    Cmd->SetGraphicsRootDescriptorTable(1, D);              // the view's depth: slot 0, or a further view's
    T.ptr += 5 * (UINT64)HeapStep;
    Cmd->SetGraphicsRootDescriptorTable(2, T);              // slot 5: the copy
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)Sd.Width, (FLOAT)Sd.Height, 0.0f, 1.0f };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    FLOAT K[HAZE_CONSTANTS];
    appMemzero(K, sizeof(K));
    K[0] = (FLOAT)Sd.Width;  K[1] = (FLOAT)Sd.Height; K[2] = EngX; K[3] = EngY;
    K[4] = W.Screen[0];      K[5] = W.Screen[1];      K[6] = W.Focal; K[7] = W.Time;
    K[8] = W.EyeWorld[0];    K[9] = W.EyeWorld[1];    K[10] = W.EyeWorld[2]; K[11] = 1.0f;
    K[12] = W.AxisX[0];      K[13] = W.AxisX[1];      K[14] = W.AxisX[2];    K[15] = W.FireRate;
    K[16] = W.AxisY[0];      K[17] = W.AxisY[1];      K[18] = W.AxisY[2];
    K[20] = W.AxisZ[0];      K[21] = W.AxisZ[1];      K[22] = W.AxisZ[2];
    for (INT i = 0; i < FireCount; i++)
    {
        D3D12_RECT Rc;
        if (!PartOf(i, Rc))
            continue;
        const FLOAT* const F = Fires + i * FIRE_FLOATS;
        for (INT j = 0; j < 8; j++)
            K[24 + j] = F[j];
        K[32] = (FLOAT)Rc.left + 0.5f;
        K[33] = (FLOAT)Rc.top + 0.5f;
        K[34] = (FLOAT)Rc.right - 0.5f;
        K[35] = (FLOAT)Rc.bottom - 0.5f;
        Cmd->RSSetScissorRects(1, &Rc);
        Cmd->SetGraphicsRoot32BitConstants(0, HAZE_CONSTANTS, K, 0);
        Cmd->DrawInstanced(3, 1, 0, 0);
    }
}

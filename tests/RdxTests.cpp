// SecondRevision - tests for the rules the game imposes on us.
//
// These run in milliseconds, need no window, no GPU and no game. They exist because
// every visual defect found so far traced back to one of these expressions, and each
// one took a full build-launch-look cycle to find. The expected values are not
// invented: they come from the SDK headers and from our own measurements, and each
// test names its source.
//
// Build and run:  scripts\test.cmd

#include "../src/RevisionDX12/RenderMath.h"
#include "../src/RevisionDX12/TreeGen.h"
#include "../src/RevisionDX12/Spherify.h"
#include "../src/RevisionDX12/MeshRound.h"
#include "../src/RevisionDX12/LevelRound.h"
#include "../src/RevisionDX12/EdgeBevel.h"
#include "../src/RevisionDX12/PomArchive.h"
#include "../src/Host64/Host64Camera.h"
#include "ArchiveVectors.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <chrono>

using namespace RdxMath;

static int GChecks = 0;
static int GFailed = 0;

static void Check(bool Condition, const char* What)
{
    GChecks++;
    if (!Condition)
    {
        GFailed++;
        printf("  FAILED: %s\n", What);
    }
}

static void CheckNear(float Got, float Expected, float Tolerance, const char* What)
{
    GChecks++;
    if (fabsf(Got - Expected) > Tolerance)
    {
        GFailed++;
        printf("  FAILED: %s  (got %.6f, expected %.6f)\n", What, Got, Expected);
    }
}

// ---------------------------------------------------------------------------

static void TestBlendClassification()
{
    printf("blend mode from the poly flags\n");

    // The exact flag combinations our instrument observed in the game
    // (docs/meres-01 section 1.8 and 3b.3). If the classification of these ever
    // changes, something visible changed with it.
    Check(BlendFor(0x00000000) == Blend_Opaque,      "no flags means opaque");
    Check(BlendFor(0x00100002) == Blend_Opaque,      "SpecialLit+Masked is opaque");
    Check(BlendFor(0x0000010A) == Blend_Opaque,      "TwoSided+NotSolid+Masked is opaque");
    Check(BlendFor(0x40004014) == Blend_Translucent, "RenderFog+Flat+Environment+Translucent -> translucent");
    Check(BlendFor(0x01000806) == Blend_Translucent, "RenderHint+NoSmooth+Translucent+Masked -> translucent");
    Check(BlendFor(0x00000040) == Blend_Modulated,   "Modulated -> modulated");

    // Both set: being seen through beats darkening what is behind.
    Check(BlendFor(0x00000044) == Blend_Translucent, "Translucent wins over Modulated");
}

static void TestModulatedDraw()
{
    printf("rules for modulated draws\n");

    // A modulated texture is a multiplier, so it must not be linearised as if it were a
    // colour, and the vertex colour must not enter the product at all.
    Check(SamplesAsColour(Blend_Modulated) == false, "a modulated texture is not a colour, so not sRGB");
    Check(SamplesAsColour(Blend_Opaque)      == true,  "an opaque texture is a colour");
    Check(SamplesAsColour(Blend_Translucent) == true,  "a translucent texture is a colour too");

    Check(UsesVertexColour(Blend_Modulated)   == false, "a modulated draw does not use vertex colour");
    Check(UsesVertexColour(Blend_Opaque)      == true,  "an opaque one does");
    Check(UsesVertexColour(Blend_Translucent) == true,  "a translucent one does");

    // The measured case: the scope overlay arrives with colour (0,0,0). If that entered
    // the product the whole scope would be black - which is what happened.
    const float BlackVertex = 0.0f;
    Check(UsesVertexColour(Blend_Modulated) == false || BlackVertex > 0.0f,
          "a (0,0,0) vertex colour must not black out the scope");
}

static void TestLightMapEligibility()
{
    printf("does a surface get a light map\n");

    // NOT the cause of the orange night sky - that was the light map channel order
    // (docs/meres-04). Kept because an unlit surface still should not be lit, but the
    // measured visual effect of this rule in the test scene was 0.01.
    Check(WantsLightMap(0x00000000) == true,  "a plain surface does");
    Check(WantsLightMap(0x00100002) == true,  "SpecialLit+Masked does");
    Check(WantsLightMap(0x00400000) == false, "Unlit does NOT");
    Check(WantsLightMap(0x00000080) == false, "FakeBackdrop (the sky) does NOT");
    Check(WantsLightMap(0x00400080) == false, "neither does both together");
}

static void TestDepth()
{
    printf("reversed Z\n");

    // Measured view depth range: 115.6 .. 3022.3 (docs/meres-01 section 1.x).
    const float Near = DepthFromViewZ(115.6f);
    const float Far  = DepthFromViewZ(3022.3f);

    Check(Near > Far, "the nearer point gets the larger depth value");
    Check(Near <= 1.0f && Far > 0.0f, "the range stays between 0 and 1");
    CheckNear(Near, 1.0f / 115.6f, 1e-6f, "near value");
    CheckNear(Far,  1.0f / 3022.3f, 1e-8f, "far value");

    // Degenerate inputs must not produce infinities or negative depth.
    CheckNear(DepthFromViewZ(0.0f),  1.0f, 1e-6f, "zero depth -> nearest");
    CheckNear(DepthFromViewZ(-50.0f), 1.0f, 1e-6f, "negative depth -> nearest");
    CheckNear(DepthFromViewZ(1.0f),  1.0f, 1e-6f, "no division at the boundary case");
}

static void TestTileDepth()
{
    printf("depth of 2D tiles and the depth test\n");

    // Measured: DrawTile is handed Z = 1.0 for everything two-dimensional.
    const float Tile = DepthFromViewZ(1.0f);
    CheckNear(Tile, 1.0f, 1e-6f, "a 2D tile lands at the nearest depth");

    // Two tiles stacked on the same pixel - a label on a highlighted button. The second
    // one must be allowed through, or the label vanishes.
    Check(DepthPasses(Tile, Tile) == true, "a tile at the same depth passes the earlier one");

    // And a tile must never enter the depth buffer at all. Passing the test is not
    // enough: an opaque overlay that WRITES depth blanks everything drawn after it,
    // which is how the sniper scope came out at exactly (0,0,0) while still submitting
    // 4154 triangles of zoomed world.
    Check(WritesDepth(Blend_Opaque, false) == true,  "opaque world geometry writes depth");
    Check(WritesDepth(Blend_Opaque, true)  == false, "an opaque 2D tile does NOT write depth");
    Check(WritesDepth(Blend_Translucent, false) == false, "translucent geometry does not write either");
    Check(WritesDepth(Blend_Modulated,  false) == false, "nor does modulated geometry");
    Check(WritesDepth(Blend_Translucent, true)  == false, "nor does a translucent tile");
    // PF_Occlude: a translucent BSP surface marked as an occluder writes depth (docs/meres-07 14).
    Check(WritesDepth(Blend_Translucent, false, 0x88800004u) == true,  "a translucent floor marked PF_Occlude writes depth");
    Check(WritesDepth(Blend_Translucent, false, 0x00000004u) == false, "a plain translucent surface does not");
    Check(WritesDepth(Blend_Translucent, true,  0x88800004u) == false, "a tile never writes, not even with PF_Occlude");
    Check(WritesDepth(Blend_Opaque,      false, 0x00000000u) == true,  "an opaque surface writes");
    Check(WritesDepth(Blend_Modulated,   false, 0x80000040u) == false, "a modulated occluder is not handled");

    // It must still be covered by anything genuinely nearer, and still cover the world.
    const float World = DepthFromViewZ(500.0f);
    Check(DepthPasses(Tile, World) == true,  "the HUD goes in front of the world");
    Check(DepthPasses(World, Tile) == false, "the world does not cover the HUD");

    // A tile placed out in the world keeps ordinary depth behaviour.
    Check(DepthPasses(DepthFromViewZ(100.0f), DepthFromViewZ(500.0f)) == true,
          "a nearer tile out in the world passes");
    Check(DepthPasses(DepthFromViewZ(500.0f), DepthFromViewZ(100.0f)) == false,
          "a farther tile out in the world does not");
}

static void TestHudTile()
{
    printf("recognising HUD tiles (where supersampling switches over)\n");

    // Measured (docs/terv-latvany-roadmap.md 1.3): the HUD, the menus and the inventory at Z <= 1 in a main
    // scene node, sprites placed in the world deeper than 10, no tile in between.
    Check(IsHudTile(false, 1.0f)   == true,  "main node, Z = 1: HUD");
    Check(IsHudTile(false, 0.5f)   == true,  "main node, Z = 0.5: HUD");
    Check(IsHudTile(false, 10.5f)  == false, "main node, Z = 10.5: a sprite out in the world");
    Check(IsHudTile(false, 900.0f) == false, "main node, Z = 900: a sprite out in the world");
    // A mirror's tiles belong to the mirror's picture, whatever their depth.
    Check(IsHudTile(true, 1.0f)    == false, "child node (a mirror), Z = 1: not HUD");
    Check(IsHudTile(true, 50.0f)   == false, "child node, Z = 50: not HUD");
}

static void TestSphereInView()
{
    printf("light inventory: does a light's sphere reach the view\n");

    // 90 degrees horizontally (tangent 1), 16:9 vertically (tangent 0.5625). Camera space: X right, Y down, Z forward.
    const float TX = 1.0f, TY = 0.5625f;
    Check(SphereTouchesView(0.0f, 0.0f, 500.0f, 10.0f, TX, TY)    == true,  "straight ahead");
    Check(SphereTouchesView(0.0f, 0.0f, -500.0f, 100.0f, TX, TY)  == false, "behind us, small radius");
    Check(SphereTouchesView(0.0f, 0.0f, -50.0f, 100.0f, TX, TY)   == true,  "behind us, but its sphere surrounds us");
    // At depth 100 the right edge is at X = 100; (150, 0, 100) lies 50 / sqrt(2) = 35.4 outside the plane.
    Check(SphereTouchesView(150.0f, 0.0f, 100.0f, 30.0f, TX, TY)  == false, "outside to the right, 35.4 away, radius 30");
    Check(SphereTouchesView(150.0f, 0.0f, 100.0f, 40.0f, TX, TY)  == true,  "outside to the right, 35.4 away, radius 40");
    Check(SphereTouchesView(-150.0f, 0.0f, 100.0f, 40.0f, TX, TY) == true,  "the same on the left (mirrored)");
    // Vertically the edge at depth 100 is Y = 56.25; (0, 100, 100) lies 43.75 / sqrt(1 + 0.5625^2) = 38.1 outside.
    Check(SphereTouchesView(0.0f, 100.0f, 100.0f, 35.0f, TX, TY)  == false, "outside below, 38.1 away, radius 35");
    Check(SphereTouchesView(0.0f, 100.0f, 100.0f, 40.0f, TX, TY)  == true,  "outside below, 38.1 away, radius 40");
}

static void TestSphereInTile()
{
    printf("tiled light list: does a light's sphere reach ONE tile\n");

    // The same 90 / 16:9 view as above (its own copy: the other test's TX/TY are local to it), cut into 4 x 4 tiles.
    // The tile taken is the top-right one: at Z = 1 its X range is 0.5 .. 1.0 and its Y range -0.5625 .. -0.28125
    // (camera space: X right, Y DOWN, Z forward).
    const float TX = 1.0f, TY = 0.5625f;
    const float TileL = 0.5f, TileR = 1.0f, TileT = -0.5625f, TileB = -0.28125f;

    Check(SphereTouchesTile(75.0f, -42.0f, 100.0f, 10.0f, TileL, TileR, TileT, TileB) == true,
          "in the middle of the tile");
    // At depth 100 the tile's left edge is X = 50; a sphere at X = 0 lies 50 / sqrt(1 + 0.5^2) = 44,7 outside it.
    Check(SphereTouchesTile(0.0f, -42.0f, 100.0f, 10.0f, TileL, TileR, TileT, TileB) == false,
          "outside to the left, 44.7 away, radius 10");
    Check(SphereTouchesTile(0.0f, -42.0f, 100.0f, 50.0f, TileL, TileR, TileT, TileB) == true,
          "same place, radius 50 - it reaches");
    Check(SphereTouchesTile(0.0f, 0.0f, -500.0f, 10.0f, TileL, TileR, TileT, TileB) == false,
          "behind us, small radius");
    Check(SphereTouchesTile(0.0f, 0.0f, -50.0f, 100.0f, TileL, TileR, TileT, TileB) == true,
          "behind us, but its sphere surrounds us");

    // THE POINT OF THE WHOLE FUNCTION: a sphere the WHOLE VIEW contains but THIS TILE does not. SphereTouchesView
    // takes the centre's absolute value, so it only knows symmetric frusta and would say yes here - and then the
    // tiled-list measurement would count every pair as surviving, and the idea would look worthless.
    Check(SphereTouchesView(-75.0f, -42.0f, 100.0f, 10.0f, TX, TY)                    == true,
          "the whole view contains it (on the left)");
    Check(SphereTouchesTile(-75.0f, -42.0f, 100.0f, 10.0f, TileL, TileR, TileT, TileB) == false,
          "but the TOP-RIGHT tile does not - this is the difference between the two functions");
}

static void TestSkyBoxDepth()
{
    printf("depth relationship of the sky and the world\n");

    // Measured on the UNATCO island save (docs/meres-05): the sky box is drawn first, at
    // view depths of 1247..2890 units, and the world behind it sits at 3546..9009. In
    // depth terms the SKY IS NEARER THAN THE WORLD, which is why the engine clears depth
    // partway through the frame instead of once at the end.
    const float SkyNearest = DepthFromViewZ(2890.0f);   // the nearest point of the sky box
    const float WorldFar = DepthFromViewZ(9009.0f);
    const float WorldNear = DepthFromViewZ(3546.0f);

    Check(SkyNearest > WorldNear, "in depth terms the sky is NEARER than the world");

    // Without the clear the world is rejected wholesale - measured consequence: trees,
    // lamp posts and people above the horizon all disappeared.
    Check(DepthPasses(WorldNear, SkyNearest) == false, "without the clear the world fails behind the sky");
    Check(DepthPasses(WorldFar, SkyNearest) == false, "its farther part fails too");

    // After the clear the buffer holds the far value and everything passes again.
    const float Cleared = 0.0f;
    Check(DepthPasses(WorldNear, Cleared) == true, "after the clear the world passes");
    Check(DepthPasses(WorldFar, Cleared) == true, "its farther part passes too");
    Check(DepthPasses(SkyNearest,   Cleared) == true, "and the sky would pass as well");
}

static void TestSurfaceCoord()
{
    printf("BSP surface UV from the mapping frame\n");

    // Measured: the mapping axes are not unit vectors - their length carries the
    // texture scale (docs/spec-04 section 2.1). An axis of length 1/16 means sixteen
    // world units span one texel.
    const FVec3 Axis   = { 0.0625f, 0.0f, 0.0f };
    const FVec3 Origin = { 0.0f, 0.0f, 0.0f };

    const FVec3 At16 = { 16.0f, 0.0f, 0.0f };
    CheckNear(SurfaceCoord(Axis, Origin, At16, 0.0f), 1.0f, 1e-5f,
              "16 world units = 1 texel when the axis is 1/16 long");

    // The origin projects to texel zero.
    CheckNear(SurfaceCoord(Axis, Origin, Origin, 0.0f), 0.0f, 1e-6f, "the origin maps to texel zero");

    // A non-zero mapping origin shifts the whole surface, it does not scale it.
    const FVec3 Shifted = { 32.0f, 0.0f, 0.0f };
    CheckNear(SurfaceCoord(Axis, Shifted, At16, 0.0f), -1.0f, 1e-5f,
              "shifting the origin shifts, it does not scale");

    // Pan subtracts in texels.
    CheckNear(SurfaceCoord(Axis, Origin, At16, 0.25f), 0.75f, 1e-5f, "Pan shifts in texels");
}

static void TestLightMapCoords()
{
    printf("light map coordinate and the half-texel shift\n");

    // Measured: for a 32-texel light map the raw projection runs 0 .. 31/32 of the
    // map - it addresses texel corners (docs/spec-04 section 2.5). After the shift
    // the first and last samples must sit on texel CENTRES.
    const int   Size  = 32;
    const float Scale = 32.0f;

    const float Divisor = LightDivisor(Scale, Size);
    CheckNear(Divisor, 1024.0f, 1e-3f, "the divisor is scale times size");

    const FVec3 Axis   = { 1.0f, 0.0f, 0.0f };
    const FVec3 Origin = { 0.0f, 0.0f, 0.0f };
    const float Pan    = LightPan(0.0f, Scale);
    CheckNear(Pan, -16.0f, 1e-4f, "the pan slides by half a texel");

    // First texel: world 0 -> centre of texel 0 -> 0.5/32.
    const FVec3 First = { 0.0f, 0.0f, 0.0f };
    CheckNear(SurfaceCoord(Axis, Origin, First, Pan) / Divisor, 0.5f / 32.0f, 1e-6f,
              "the first sample lands on the centre of texel 0");

    // Last texel: the measured raw extent is (Size-1)*Scale world units.
    const FVec3 Last = { (float)(Size - 1) * Scale, 0.0f, 0.0f };
    CheckNear(SurfaceCoord(Axis, Origin, Last, Pan) / Divisor, 31.5f / 32.0f, 1e-6f,
              "the last sample lands on the centre of texel 31");

    // Without the shift the same points land on corners - which is the bug this
    // whole calculation exists to avoid.
    CheckNear(SurfaceCoord(Axis, Origin, Last, 0.0f) / Divisor, 31.0f / 32.0f, 1e-6f,
              "without the shift it would land on a texel corner (31/32)");

    // A zero scale must not divide by zero.
    CheckNear(LightDivisor(0.0f, 32), 1.0f, 1e-6f, "a zero scale does not divide by zero");

    // The same divisor rule applies to the DIFFUSE texture, not only to light maps.
    // Measured: the city sky line is 2048x1024 with UScale = VScale = 0.125. Dividing by
    // the size alone lands eight times too far along the texture; the buildings live in
    // the lower half (V 0.46..1.0) and the upper half is the transparent sky, so the whole
    // sky line drew nothing (docs/meres-06 section 6).
    CheckNear(LightDivisor(0.125f, 1024), 128.0f, 1e-3f, "the divisor is scale times size");
    const float RawV = -2848.0f;                      // the projected value, in texels
    const float WrongV = RawV / 1024.0f;              // divided by the size alone
    const float GoodV    = RawV / LightDivisor(0.125f, 1024);
    auto Repeat = [](float V) { float F = V - (float)(int)V; return F < 0.0f ? F + 1.0f : F; };
    CheckNear(Repeat(WrongV), 0.2186f, 1e-3f, "with the wrong divisor the sample lands on empty sky");
    CheckNear(Repeat(GoodV),    0.75f,   1e-3f, "with the right divisor it lands in the band of buildings");
    Check(Repeat(GoodV) > 0.46f, "the buildings are in the lower half of the texture");

    // The light maps' blending over the spherifier's soft edges (SoftLightShare, SoftLightMix, LightTexelPlace,
    // SampleLightTexels).
    CheckNear(SoftLightShare(0.0f, 20.0f), 0.5f, 1e-6f, "soft light: half of the light across on the edge");
    CheckNear(SoftLightShare(10.0f, 20.0f), 0.25f, 1e-6f, "soft light: a quarter halfway to the reach");
    CheckNear(SoftLightShare(-2.0f, 20.0f), 0.55f, 1e-6f, "soft light: more than half just past the edge");
    Check(SoftLightShare(20.0f, 20.0f) == 0.0f && SoftLightShare(25.0f, 20.0f) == 0.0f &&
              SoftLightShare(5.0f, 0.0f) == 0.0f && SoftLightShare(-40.0f, 20.0f) == 1.0f,
          "soft light: none at the reach or past it, or with no reach; all of it far past the edge");
    {
        bool bDown = true;
        for (int k = 1; k <= 20; k++)
            bDown = bDown && SoftLightShare((float)k, 20.0f) <= SoftLightShare((float)(k - 1), 20.0f);
        // The two sides of an edge, each its own reach: the light across it runs on without a step.
        const float A = 100.0f, B = 40.0f, RA = 20.0f, RB = 12.0f;
        const float AtA = A + (B - A) * SoftLightShare(0.0f, RA), AtB = B + (A - B) * SoftLightShare(0.0f, RB);
        Check(bDown && fabsf(AtA - AtB) < 1e-4f, "soft light: the share only falls into the polygon; no step on the edge");
    }
    {
        const float Own[4] = { 100.0f, 100.0f, 100.0f, 255.0f };
        const float Across[8] = { 40.0f, 40.0f, 40.0f, 255.0f, 160.0f, 10.0f, 70.0f, 255.0f };
        float       Out[4];
        const float One[1] = { 0.25f };
        SoftLightMix(Own, One, Across, 1, Out);
        CheckNear(Out[0], 100.0f + (40.0f - 100.0f) * 0.25f, 1e-3f, "soft light: one side's share as it is");
        const float Two[2] = { 0.5f, 0.5f };
        SoftLightMix(Own, Two, Across, 2, Out);
        Check(fabsf(Out[0] - 100.0f) < 1e-3f && fabsf(Out[1] - 50.0f) < 1e-3f && fabsf(Out[2] - 70.0f) < 1e-3f &&
                  fabsf(Out[3] - 255.0f) < 1e-3f,
              "soft light: two sides meeting at a corner, half each - the three polygons' mean");
        const float None[2] = { 0.0f, -1.0f };
        SoftLightMix(Own, None, Across, 2, Out);
        Check(Out[0] == 100.0f && Out[1] == 100.0f, "soft light: no share, the texel's own");
    }
    {
        // A texel's place and back: the axes skewed and scaled, panned - the place's coordinate is the texel's middle.
        const FVec3 Base = { 100.0f, -40.0f, 12.0f }, U = { 0.8f, 0.6f, 0.0f }, V = { 0.1f, 0.0f, -1.2f };
        const float PanU = 7.5f, PanV = -3.0f, InvU = 1.0f / (2.0f * 32.0f), InvV = 1.0f / (1.5f * 16.0f);
        bool bBack = true;
        for (int j = 0; j < 16; j += 5)
            for (int i = 0; i < 32; i += 7)
            {
                FVec3 P = { 0.0f, 0.0f, 0.0f };
                bBack = bBack && LightTexelPlace(i, j, 32, 16, PanU, PanV, InvU, InvV, Base, U, V, P);
                const FVec3 D = { P.X - Base.X, P.Y - Base.Y, P.Z - Base.Z };
                bBack = bBack && fabsf((Dot(D, U) - PanU) * InvU - ((float)i + 0.5f) / 32.0f) < 1e-4f &&
                        fabsf((Dot(D, V) - PanV) * InvV - ((float)j + 0.5f) / 16.0f) < 1e-4f;
            }
        FVec3 Q;
        const FVec3 Same = { 0.8f, 0.6f, 0.0f };
        Check(bBack && !LightTexelPlace(0, 0, 32, 16, PanU, PanV, InvU, InvV, Base, U, Same, Q),
              "corner light: a texel's place on its surface lies under the texel's middle; parallel axes give none");
    }
    {
        // Two by two texels: the middle is their mean; a corner's middle is that texel; outside the lit part it holds
        // to the last lit texel.
        const unsigned char T[16] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 126, 0, 2 };
        float O[4];
        SampleLightTexels(T, 2, 2, 2, 2, 0.5f, 0.5f, O);
        const bool bMean = fabsf(O[0] - 60.0f) < 1e-3f && fabsf(O[1] - 69.0f) < 1e-3f && fabsf(O[3] - 53.0f) < 1e-3f;
        SampleLightTexels(T, 2, 2, 2, 2, 0.25f, 0.25f, O);
        const bool bCorner = fabsf(O[0]) < 1e-3f && fabsf(O[2] - 20.0f) < 1e-3f;
        SampleLightTexels(T, 2, 2, 1, 2, 0.9f, 0.25f, O);
        const bool bLit = fabsf(O[0]) < 1e-3f && fabsf(O[3] - 30.0f) < 1e-3f;
        Check(bMean && bCorner && bLit, "corner light: a light map read between its texels, within the lit part");
    }
}

static void TestPerspective()
{
    printf("perspective-correct interpolation\n");

    // The measured view depth range (docs/spec-03 section 3): the near end of a long
    // surface is at 115.6 units, the far end at 3022.3.
    const float Near = 115.6f;
    const float Far  = 3022.3f;

    // W is the view depth, recovered from the reciprocal the vertex carries.
    CheckNear(PerspectiveW(DepthFromViewZ(Near)), Near, 1e-2f, "W is the near view depth");
    CheckNear(PerspectiveW(DepthFromViewZ(Far)),  Far,  1e-1f, "W is the far view depth");
    CheckNear(PerspectiveW(1.0f), 1.0f, 1e-6f, "W stays 1 for 2D tiles");
    CheckNear(PerspectiveW(0.0f), 1.0f, 1e-6f, "zero depth does not divide by zero");

    // This is the test that matters: not the formula, its CONSEQUENCE. Take an edge
    // running from the near end of that range to the far end, carrying texture
    // coordinate 0 at one end and 1 at the other. Halfway across the SCREEN the correct
    // texture coordinate is nowhere near halfway across the texture - almost the whole
    // texture is compressed into the far half of the screen.
    const float WNear = Near, WFar = Far;
    const float Correct = PerspectiveLerp(0.0f, WNear, 1.0f, WFar, 0.5f);
    CheckNear(Correct, 0.0368f, 1e-3f, "halfway across the screen we are 3.7 per cent across the texture");

    // With W = 1 - the fault this test exists to catch - the same point reports 0.5. On screen that shows as
    // textures sliding across surfaces while the player walks.
    const float Affine = PerspectiveLerp(0.0f, 1.0f, 1.0f, 1.0f, 0.5f);
    CheckNear(Affine, 0.5f, 1e-6f, "with W=1 it is affine, that is 0.5");
    Check(Affine > Correct * 10.0f, "the affine error is an order of magnitude, not a subtle difference");

    // Perspective and affine agree only when both ends are at the same depth, which is
    // why 2D tiles and head-on surfaces looked correct and the long wall did not.
    CheckNear(PerspectiveLerp(0.0f, Near, 1.0f, Near, 0.5f), 0.5f, 1e-6f,
              "at equal depth there is no difference");
}

static void TestPalette()
{
    printf("unpacking a palettised texture\n");

    const FRgba Entry = { 200, 100, 50, 0 };

    // Index 0 is transparent only when the surface is masked; the colour itself is
    // never changed (docs/spec-02 section 2.1).
    const FRgba Masked0 = PaletteTexel(0, Entry, true);
    Check(Masked0.A == 0, "index 0 is transparent when masked");
    Check(Masked0.R == 200 && Masked0.G == 100 && Masked0.B == 50, "the colour itself is unchanged");

    Check(PaletteTexel(0, Entry, false).A == 255, "without masking index 0 is visible");
    Check(PaletteTexel(1, Entry, true).A  == 255, "even when masked only index 0 is transparent");

    // RGBA7 doubling, with saturation rather than wraparound.
    Check(Rgba7Channel(0)   == 0,   "RGBA7 zero");
    Check(Rgba7Channel(100) == 200, "RGBA7 doubling");
    Check(Rgba7Channel(127) == 254, "RGBA7 top value");
    Check(Rgba7Channel(200) == 255, "RGBA7 saturates, it does not wrap");
}

static void TestGouraudLight()
{
    printf("vertex light of models\n");

#if RDX_CLASSIC
    // Classic, the default: the engine's vertex light is used as it comes. Measured on the
    // Liberty Island save, the player's weapon: doubled, as the linear pipeline needed it,
    // it came out 2.2 times the stock renderer's value; undoubled it lands at about 0.92.
    CheckNear(GouraudLight(0.5f), 0.5f, 1e-6f, "classic: the vertex light is used as it comes");
#else
    // Linear: a light map texel of 100 unpacks to 200 of 255, and a mesh vertex reporting
    // the same fraction lands in the same place before the gain.
    const float SameTexel = (float)Rgba7Channel(100) / 255.0f;
    CheckNear(GouraudLight(100.0f / 255.0f), SameTexel, 0.005f,
              "linear: the vertex light lands where a light map texel of the same value does");
#endif
    CheckNear(GouraudLight(0.0f), 0.0f, 1e-6f, "dark stays dark");

    // Deliberately not clamped: the engine overbrightens, and the render target is what
    // saturates. Clamping here would flatten every bright light in the game.
    Check(GouraudLight(1.5f) > 1.0f, "overbrightness passes through, it is not clamped");

    // The gain is a calibration, not a constant of nature: it must stay a positive
    // number under one, or it would brighten instead of matching the stock renderer.
    Check(LightGain() > 0.0f && LightGain() <= 1.0f, "the light gain is between 0 and 1");
}

static void TestNeutralLight()
{
    printf("the neutral light map texel and the light scale\n");

    // The neutral light map texel and the light scale are one decision. Whatever the
    // scale, their product must be exactly one, or geometry without a light map - every
    // mesh and every 2D tile - comes out at the wrong brightness while lightmapped walls
    // look fine. That asymmetry is what the defect looked like from the outside.
    Check(NeutralLightTexel(1.0f) == 255, "at scale 1 the neutral is full white");
    Check(NeutralLightTexel(2.0f) == 128, "at scale 2 it is the midpoint");
    Check(NeutralLightTexel(0.0f) == 255, "an invalid scale does not divide by zero");

    // The consequence, checked as a product rather than as a formula.
    const float Scales[3] = { 1.0f, 2.0f, 4.0f };
    for (int i = 0; i < 3; i++)
    {
        const float Product = ((float)NeutralLightTexel(Scales[i]) / 255.0f) * Scales[i];
        CheckNear(Product, 1.0f, 0.01f, "neutral times scale is exactly one");
    }

    // Below one it cannot be cancelled: eight bits cannot hold a value above 255, so the
    // neutral saturates and unlit geometry would come out dark. A scale under one is
    // therefore not a free choice - it needs a brighter neutral than a byte can hold.
    Check(NeutralLightTexel(0.5f) == 255, "below scale 1 it saturates");
    Check(((float)NeutralLightTexel(0.5f) / 255.0f) * 0.5f < 1.0f,
          "and then the product is NOT one - that is the limit, not a fault");

    // The value the project actually ships with must satisfy it too - this is the check
    // that would have caught the regression the moment the scale was recalibrated.
    const float Shipped = ((float)NeutralLightTexel(RDX_LIGHT_SCALE_VALUE) / 255.0f) * RDX_LIGHT_SCALE_VALUE;
    CheckNear(Shipped, 1.0f, 0.01f, "the configured scale and its neutral fit together");
}

static void TestLightMapChannels()
{
    printf("channel order of a light map\n");

    // Measured (docs/meres-04): a light map's RGBA7 texels arrive with red and blue the
    // other way round from a palette entry, and the diffuse path proves it is not FColor
    // that is reversed. A warm light must come out warm.
    FRgba Warm;                        // as the engine hands it over: most light on the "blue" byte
    Warm.R = 20; Warm.G = 40; Warm.B = 100; Warm.A = 127;
    const FRgba L = LightMapTexel(Warm);
    Check(L.R == 200, "the strongest channel comes out on red");
    Check(L.G == 80,  "green stays where it is");
    Check(L.B == 40,  "the weakest one on blue");
    Check(L.A == 255, "a light map is opaque");

    // Neutral light must survive the reordering unchanged - otherwise the swap would be
    // invisible in grey scenes and only show up in coloured ones, which is exactly how
    // this bug hid for so long.
    FRgba Neutral;
    Neutral.R = 60; Neutral.G = 60; Neutral.B = 60; Neutral.A = 127;
    const FRgba N = LightMapTexel(Neutral);
    Check(N.R == 120 && N.G == 120 && N.B == 120, "neutral light stays neutral");

    // The doubling is the RGBA7 expansion, and it saturates rather than wrapping.
    FRgba Saturated;
    Saturated.R = 200; Saturated.G = 200; Saturated.B = 200; Saturated.A = 127;
    const FRgba T = LightMapTexel(Saturated);
    Check(T.R == 255 && T.G == 255 && T.B == 255, "saturation instead of overflow");
}

static void TestRgba7Rows()
{
    printf("RGBA7 rows in one go: byte for byte the same as texel by texel\n");

    // FTextureCache::Convert expands RGBA7 rows with ExpandRgba7Row - four texels at a time where
    // SSE2 is there. Texel by texel it was Rgba7Channel, and LightMapTexel for light maps; the row
    // must give the same bytes: every byte value in every position, both orders, with the alpha made
    // opaque and kept (a fog map's), rows long and short enough to leave every tail, and memory not
    // aligned to 16 bytes.
    static unsigned char Src[4 * 70 + 4], Got[4 * 70 + 4];
    bool Ok = true;
    long Cases = 0;
    for (int Keep = 0; Keep < 2; Keep++)
    for (int Swap = 0; Swap < 2; Swap++)
        for (int v = 0; v < 256; v++)
            for (int Width = 1; Width <= 70; Width += (Width < 9 ? 1 : 13))
            {
                unsigned char* S = Src + 4;          // texel-aligned, not 16-byte aligned
                unsigned char* D = Got + 4;
                for (int i = 0; i < Width * 4; i++)
                    S[i] = (unsigned char)(v + i * 37 + (i & 3) * 85);
                ExpandRgba7Row(S, D, Width, Swap != 0, Keep != 0);
                for (int x = 0; x < Width; x++)
                {
                    FRgba Raw;
                    Raw.R = S[x * 4 + 0]; Raw.G = S[x * 4 + 1]; Raw.B = S[x * 4 + 2]; Raw.A = S[x * 4 + 3];
                    FRgba Want;
                    if (Swap)
                        Want = LightMapTexel(Raw);
                    else
                    {
                        Want.R = Rgba7Channel(Raw.R); Want.G = Rgba7Channel(Raw.G);
                        Want.B = Rgba7Channel(Raw.B); Want.A = 255;
                    }
                    if (Keep)
                        Want.A = Rgba7Channel(Raw.A);
                    if (D[x * 4 + 0] != Want.R || D[x * 4 + 1] != Want.G ||
                        D[x * 4 + 2] != Want.B || D[x * 4 + 3] != Want.A)
                        Ok = false;
                    Cases++;
                }
            }
    printf("  %ld texels\n", Cases);
    Check(Ok, "every byte value in every position, in both orders, with every row tail");
}

static void TestMaskedEdges()
{
    printf("masked edges: the key colour must not leak out\n");

    // A 3x3 patch: one opaque green texel surrounded by the transparent magenta the
    // palette's entry 0 actually holds in Deus Ex.
    const int Pitch = 3 * 4;
    unsigned char Px[3 * 3 * 4];
    for (int i = 0; i < 9; i++)
    {
        Px[i * 4 + 0] = 255; Px[i * 4 + 1] = 0; Px[i * 4 + 2] = 255; Px[i * 4 + 3] = 0;
    }
    unsigned char* Mid = Px + (1 * Pitch) + 1 * 4;
    Mid[0] = 0; Mid[1] = 200; Mid[2] = 0; Mid[3] = 255;

    DilateTransparentEdges(Px, 3, 3, Pitch);

    // Every transparent texel now carries the visible colour, and is still transparent.
    for (int i = 0; i < 9; i++)
    {
        if (i == 4) continue;
        Check(Px[i * 4 + 3] == 0,   "transparency is unchanged");
        Check(Px[i * 4 + 0] == 0,   "magenta's red component is gone");
        Check(Px[i * 4 + 1] == 200, "it took over the visible colour");
        Check(Px[i * 4 + 2] == 0,   "magenta's blue component is gone");
    }
    Check(Px[4 * 4 + 1] == 200 && Px[4 * 4 + 3] == 255, "the visible texel is untouched");

    // The consequence, which is what the user actually saw: a bilinear sampler halfway
    // between the visible texel and its neighbour. Before the pass that average is
    // (127, 100, 127) - a magenta halo. After it there is no red or blue left at all.
    Check((255 + 0) / 2 == 127, "without dilation the midpoint would be magenta");
    Check((Px[0] + Mid[0]) / 2 == 0, "after dilation there is no red at the midpoint");
    Check((Px[2] + Mid[2]) / 2 == 0, "after dilation there is no blue at the midpoint");

    // A patch with nothing opaque in it must be left exactly as it was: there is no
    // sensible colour to spread, and nothing samples it either.
    unsigned char Empty[2 * 2 * 4];
    for (int i = 0; i < 4; i++)
    {
        Empty[i * 4 + 0] = 255; Empty[i * 4 + 1] = 0; Empty[i * 4 + 2] = 255; Empty[i * 4 + 3] = 0;
    }
    DilateTransparentEdges(Empty, 2, 2, 2 * 4);
    Check(Empty[0] == 255 && Empty[2] == 255, "an all-transparent patch is left unchanged");
}

static void TestMaskedEdgeWrap()
{
    printf("masked edges: the texture wraps, so the opposite edge is a neighbour too\n");

    // Every texture is sampled with WRAP addressing, so the row above the first one is the
    // last one. The city sky line is the case that showed it: visible along its bottom,
    // empty sky along its top, and magenta dashes ran along the bottom of the sky line
    // where a bilinear sample reached across that seam.
    const int W = 4, H = 8, Pitch = W * 4;
    unsigned char Px[W * H * 4];
    for (int i = 0; i < W * H; i++)
    {
        Px[i * 4 + 0] = 255; Px[i * 4 + 1] = 0; Px[i * 4 + 2] = 255; Px[i * 4 + 3] = 0;
    }
    for (int x = 0; x < W; x++)
    {
        unsigned char* P = Px + (H - 1) * Pitch + x * 4;
        P[0] = 0; P[1] = 200; P[2] = 0; P[3] = 255;
    }

    DilateTransparentEdges(Px, W, H, Pitch, 1);

    const unsigned char* Top = Px;                    // row 0
    const unsigned char* Bottom  = Px + (H - 1) * Pitch;  // row H-1, the visible one
    Check(Top[0] == 0 && Top[1] == 200 && Top[2] == 0,
          "the top row took the colour of the bottom row");
    Check(Top[3] == 0, "and stayed transparent");
    // What was actually seen: a sample half way across the seam.
    Check((Bottom[0] + Top[0]) / 2 == 0, "no red at the seam");
    Check((Bottom[2] + Top[2]) / 2 == 0, "no blue at the seam");
    // Row 3 is four rows from the visible row in either direction: one pass must not reach it.
    const unsigned char* Middle = Px + 3 * Pitch;
    Check(Middle[0] == 255 && Middle[2] == 255, "one pass spreads by exactly one texel");

    // The pass count is a distance. A pass must not read what it filled itself, or the
    // colour runs on along the scan direction and one pass fills the whole row.
    unsigned char Row[16 * 4];
    for (int i = 0; i < 16; i++)
    {
        Row[i * 4 + 0] = 255; Row[i * 4 + 1] = 0; Row[i * 4 + 2] = 255; Row[i * 4 + 3] = 0;
    }
    Row[0] = 0; Row[1] = 200; Row[2] = 0; Row[3] = 255;
    DilateTransparentEdges(Row, 16, 1, 16 * 4, 2);
    Check(Row[1 * 4 + 1] == 200 && Row[2 * 4 + 1] == 200, "two passes: two texels forward");
    Check(Row[15 * 4 + 1] == 200 && Row[14 * 4 + 1] == 200, "and two texels back, wrapping around");
    Check(Row[3 * 4 + 0] == 255 && Row[3 * 4 + 2] == 255, "the third does not: no run-on");
    Check(Row[13 * 4 + 0] == 255 && Row[13 * 4 + 2] == 255, "nor backwards");
    bool AllZero = true;
    for (int i = 1; i < 16; i++)
        if (Row[i * 4 + 3] != 0) AllZero = false;
    Check(AllZero, "every key texel is gone, transparency unchanged");
    Check(Row[3] == 255, "the visible texel is untouched");
}

// The first version of the dilation, kept as it was as the yardstick: the fast one in
// RenderMath.h must produce exactly this, byte for byte, row padding included.
static void DilateReference(unsigned char* Pixels, int Width, int Height, int Pitch, int Passes)
{
  if (!Pixels || Width <= 0 || Height <= 0)
      return;
  if (Passes > 250)
      Passes = 250;

  for (int Pass = 0; Pass < Passes; Pass++)
  {
    const unsigned char Mark = (unsigned char)(Pass + 1);
    int Filled = 0;
    for (int y = 0; y < Height; y++)
    {
        unsigned char* Row = Pixels + (long)y * Pitch;
        for (int x = 0; x < Width; x++)
        {
            if (Row[x * 4 + 3] != 0)
                continue;

            int Sum[3] = { 0, 0, 0 };
            int Count = 0;
            for (int dy = -1; dy <= 1; dy++)
            {
                int ny = y + dy;
                if (ny < 0) ny += Height; else if (ny >= Height) ny -= Height;
                const unsigned char* N = Pixels + (long)ny * Pitch;
                for (int dx = -1; dx <= 1; dx++)
                {
                    if (dx == 0 && dy == 0)
                        continue;
                    int nx = x + dx;
                    if (nx < 0) nx += Width; else if (nx >= Width) nx -= Width;
                    const unsigned char A = N[nx * 4 + 3];
                    if (A == 0 || A == Mark)
                        continue;
                    Sum[0] += N[nx * 4 + 0];
                    Sum[1] += N[nx * 4 + 1];
                    Sum[2] += N[nx * 4 + 2];
                    Count++;
                }
            }
            if (Count == 0)
                continue;
            Filled++;
            Row[x * 4 + 0] = (unsigned char)(Sum[0] / Count);
            Row[x * 4 + 1] = (unsigned char)(Sum[1] / Count);
            Row[x * 4 + 2] = (unsigned char)(Sum[2] / Count);
            Row[x * 4 + 3] = Mark;
        }
    }
    if (Filled == 0)
        break;
  }

  for (int y = 0; y < Height; y++)
  {
      unsigned char* Row = Pixels + (long)y * Pitch;
      for (int x = 0; x < Width; x++)
          if (Row[x * 4 + 3] != 0 && Row[x * 4 + 3] <= Passes)
              Row[x * 4 + 3] = 0;
  }
}

// A plain linear congruential generator, so every run sees the same images. Any full-period
// one would do; the low bits of such a generator repeat quickly, so they are dropped.
static unsigned int GTestSeed = 20260911u;
static unsigned int TestRand()
{
    GTestSeed = GTestSeed * 69069u + 1u;
    return GTestSeed >> 8;
}

// Random colours everywhere - the row padding too, which must come through untouched - and
// an alpha pattern chosen by Kind.
static void FillDilateCase(unsigned char* Buf, int W, int H, int Pitch, int Kind)
{
    for (int i = 0; i < H * Pitch; i++)
        Buf[i] = (unsigned char)(TestRand() >> 16);
    static const unsigned char Mixed[12] = { 0, 0, 0, 0, 1, 2, 3, 4, 5, 128, 254, 255 };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            unsigned char& A = Buf[y * Pitch + x * 4 + 3];
            const unsigned int R = TestRand() % 100;
            switch (Kind)
            {
                case 0:  A = 0; break;                        // nothing visible
                case 1:  A = 255; break;                      // nothing transparent
                case 2:  A = R < 3  ? 255 : 0; break;         // lone specks: the passes reach far
                case 3:  A = R < 30 ? 255 : 0; break;
                case 4:  A = R < 75 ? 255 : 0; break;
                case 5:  A = 0; break;                        // rectangles, below
                default: A = Mixed[TestRand() % 12]; break;   // alphas a mark can take, too
            }
        }
    if (Kind == 5)
    {
        const int N = 1 + (int)(TestRand() % 3);
        for (int k = 0; k < N; k++)
        {
            const int x0 = (int)(TestRand() % W), y0 = (int)(TestRand() % H);
            const int w  = 1 + (int)(TestRand() % 4), h = 1 + (int)(TestRand() % 4);
            for (int y = y0; y < y0 + h && y < H; y++)
                for (int x = x0; x < x0 + w && x < W; x++)
                    Buf[y * Pitch + x * 4 + 3] = 255;
        }
    }
}

static void TestDilateEquivalence()
{
    printf("masked edges: the fast dilation gives bit for bit what the simple one gives\n");

    bool BitsOk = true;
    for (int b = 0; b < 32; b++)
        if (DilateLowestBit(1u << b) != b || DilateLowestBit((1u << b) | 0x80000000u) != b)
            BitsOk = false;
    Check(BitsOk, "index of the lowest set bit at every position");

    // Every sum a fill can meet (eight neighbours of 255 at most) by every count.
    bool DivOk = true;
    for (unsigned int Count = 1; Count <= 8; Count++)
        for (unsigned int Sum = 0; Sum <= 8 * 255; Sum++)
            if (DilateDivide(Sum, Count) != Sum / Count)
                DivOk = false;
    Check(DivOk, "division by multiplication: exact for every sum and count");

    // Widths around the 32-texel words of the bitmap and the one- and two-texel cases where
    // the wrap counts a neighbour twice; odd row padding; pass counts past the 250 limit.
    static const int Ws[]       = { 1, 2, 3, 5, 8, 31, 32, 33, 64, 65, 70 };
    static const int Hs[]       = { 1, 2, 3, 4, 7, 16, 33, 40 };
    static const int Pads[]     = { 0, 3, 12 };
    static const int PassList[] = { 0, 1, 2, 3, 4, 300 };
    static unsigned char Orig[40 * (70 * 4 + 12)], Ref[sizeof(Orig)], Fast[sizeof(Orig)], Ext[sizeof(Orig)];
    // Work memory from outside, as the texture cache passes it on its worker threads - filled
    // with garbage first: every word of it must be written before it is read.
    static unsigned int Work[3 * 3 * 40];
    Check(DilateWorkWords(70, 40) <= (long)(sizeof(Work) / sizeof(Work[0])), "the external scratch area is large enough");

    int Cases = 0, Mismatch = 0;
    for (int wi = 0; wi < (int)(sizeof(Ws) / sizeof(Ws[0])); wi++)
    for (int hi = 0; hi < (int)(sizeof(Hs) / sizeof(Hs[0])); hi++)
    for (int pi = 0; pi < (int)(sizeof(Pads) / sizeof(Pads[0])); pi++)
    for (int Kind = 0; Kind < 7; Kind++)
    for (int Round = 0; Round < 2; Round++)
    {
        const int W = Ws[wi], H = Hs[hi], Pitch = W * 4 + Pads[pi], Bytes = H * Pitch;
        FillDilateCase(Orig, W, H, Pitch, Kind);
        for (int ni = 0; ni < (int)(sizeof(PassList) / sizeof(PassList[0])); ni++)
        {
            memcpy(Ref,  Orig, Bytes);
            memcpy(Fast, Orig, Bytes);
            memcpy(Ext,  Orig, Bytes);
            for (int k = 0; k < (int)(sizeof(Work) / sizeof(Work[0])); k++)
                Work[k] = TestRand() | 0x80000001u;
            DilateReference(Ref, W, H, Pitch, PassList[ni]);
            DilateTransparentEdges(Fast, W, H, Pitch, PassList[ni]);
            DilateTransparentEdges(Ext, W, H, Pitch, PassList[ni], Work);
            Cases++;
            const bool FastBad = memcmp(Ref, Fast, Bytes) != 0;
            const bool ExtBad  = memcmp(Ref, Ext, Bytes) != 0;
            if (FastBad || ExtBad)
            {
                if (Mismatch == 0)
                {
                    const unsigned char* Bad = FastBad ? Fast : Ext;
                    int i = 0;
                    while (Ref[i] == Bad[i]) i++;
                    printf("  first mismatch (%s): %ix%i, pitch %i, pattern %i, %i passes: row %i, byte %i: "
                           "old %i, new %i\n", FastBad ? "own allocation" : "external scratch",
                           W, H, Pitch, Kind, PassList[ni], i / Pitch, i % Pitch, Ref[i], Bad[i]);
                }
                Mismatch++;
            }
        }
    }
    printf("  %i cases, %i mismatches\n", Cases, Mismatch);
    Check(Mismatch == 0, "identical bit for bit in every case");
}

// ---------------------------------------------------------------------------
// Speed of the dilation. Not a test - run as  RdxTests.exe measure
// ---------------------------------------------------------------------------

// One mip level of a masked texture, alpha shaped by Kind, sizes scaled down with the level:
//   0 wire grid: two texels of wire every eight, so every hole texel is near a wire
//   1 leaves:    discs of radius 2..10 until about half is covered
//   2 sparse:    discs of radius 40..120 over a fifth - most transparent texels are far away
//   3 all visible, 4 all transparent: what the dilation costs with nothing to do
//   5 emblem:    one disc over most of it, transparent in the corners - like a seal
static void MakeMaskedLevel(unsigned char* Px, int S, int Level, int Kind)
{
    for (int i = 0; i < S * S; i++)
    {
        Px[i * 4 + 0] = (unsigned char)(TestRand() >> 16);
        Px[i * 4 + 1] = (unsigned char)(TestRand() >> 16);
        Px[i * 4 + 2] = (unsigned char)(TestRand() >> 16);
        Px[i * 4 + 3] = (Kind == 3) ? 255 : 0;
    }
    if (Kind == 5)
    {
        // The shape of the UNATCO seal that cost 8.7 ms of dilation in one frame while
        // turning (docs/meres-08 6b).
        const long R = (long)S * 45 / 100;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++)
            {
                const long dx = x - S / 2, dy = y - S / 2;
                if (dx * dx + dy * dy <= R * R)
                    Px[(y * S + x) * 4 + 3] = 255;
            }
        return;
    }
    if (Kind >= 3)
        return;
    if (Kind == 0)
    {
        const int Period = (8 >> Level) > 2 ? (8 >> Level) : 2;
        const int Wire   = (2 >> Level) > 1 ? (2 >> Level) : 1;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++)
                if (x % Period < Wire || y % Period < Wire)
                    Px[(y * S + x) * 4 + 3] = 255;
        return;
    }
    int Rmin = (Kind == 1 ? 2 : 40) >> Level, Rmax = (Kind == 1 ? 10 : 120) >> Level;
    if (Rmin < 1) Rmin = 1;
    if (Rmax < Rmin) Rmax = Rmin;
    const long Want = (long)S * S * (Kind == 1 ? 50 : 20) / 100;
    long Covered = 0;
    while (Covered < Want)
    {
        const int cx = (int)(TestRand() % S), cy = (int)(TestRand() % S);
        const int r  = Rmin + (int)(TestRand() % (Rmax - Rmin + 1));
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx * dx + dy * dy <= r * r)
                {
                    const int x = ((cx + dx) % S + S) % S, y = ((cy + dy) % S + S) % S;
                    unsigned char& A = Px[(y * S + x) * 4 + 3];
                    if (A == 0) { A = 255; Covered++; }
                }
    }
}

static double NowMs()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void MeasureDilate()
{
    printf("\ndilation speed: 2048x2048 masked texture, the whole mip chain, %i passes\n"
           "(the new one takes the best of five runs)\n", RDX_DILATE_PASSES);
    static const char* Names[6] = { "grate", "leaves", "sparse", "full", "empty", "emblem" };
    const int Levels = 12;
    for (int Kind = 0; Kind < 6; Kind++)
    {
        unsigned char* Orig[Levels];
        unsigned char* Ref[Levels];
        unsigned char* Fast[Levels];
        for (int m = 0; m < Levels; m++)
        {
            const int S = 2048 >> m;
            Orig[m] = new unsigned char[S * S * 4];
            Ref[m]  = new unsigned char[S * S * 4];
            Fast[m] = new unsigned char[S * S * 4];
            MakeMaskedLevel(Orig[m], S, m, Kind);
            memcpy(Ref[m], Orig[m], S * S * 4);
        }
        const double t0 = NowMs();
        for (int m = 0; m < Levels; m++)
            DilateReference(Ref[m], 2048 >> m, 2048 >> m, (2048 >> m) * 4, RDX_DILATE_PASSES);
        const double Old = NowMs() - t0;

        double Best = 1e30;
        for (int Run = 0; Run < 5; Run++)
        {
            for (int m = 0; m < Levels; m++)
                memcpy(Fast[m], Orig[m], (2048 >> m) * (2048 >> m) * 4);
            const double t1 = NowMs();
            for (int m = 0; m < Levels; m++)
                DilateTransparentEdges(Fast[m], 2048 >> m, 2048 >> m, (2048 >> m) * 4);
            const double Took = NowMs() - t1;
            if (Took < Best) Best = Took;
        }

        // How much of that is the one full read, the visibility bitmap: the same loop as in
        // DilateTransparentEdges, timed alone. A copy for measuring only.
        double Scan = 1e30;
        unsigned int Sink = 0;
        for (int Run = 0; Run < 5; Run++)
        {
            const double t2 = NowMs();
            for (int m = 0; m < Levels; m++)
            {
                const int S = 2048 >> m, Words = (S + 31) >> 5;
                for (int y = 0; y < S; y++)
                {
                    const unsigned char* Row = Orig[m] + (long)y * S * 4;
                    for (int i = 0; i < Words; i++)
                    {
                        const int x0 = i << 5, n = (S - x0 < 32) ? S - x0 : 32;
                        unsigned int Bits = 0, Low = 0;
                        for (int b = 0; b < n; b++)
                        {
                            const unsigned int A = Row[(x0 + b) * 4 + 3];
                            Bits |= (unsigned int)(A != 0) << b;
                            Low  |= (unsigned int)(A - 1u < (unsigned int)RDX_DILATE_PASSES);
                        }
                        Sink += Bits ^ Low;
                    }
                }
            }
            const double Took = NowMs() - t2;
            if (Took < Scan) Scan = Took;
        }

        bool Same = true;
        for (int m = 0; m < Levels; m++)
        {
            const int S = 2048 >> m;
            if (memcmp(Ref[m], Fast[m], S * S * 4) != 0) Same = false;
            delete[] Orig[m];
            delete[] Ref[m];
            delete[] Fast[m];
        }
        printf("  %-8s old %7.1f ms, new %6.1f ms (old, scalar alpha bitmap %5.1f ms), identical: %s%s\n",
               Names[Kind], Old, Best, Scan, Same ? "yes" : "NO", (Sink == 12345u) ? " " : "");
    }
}

static void TestPaletteWords()
{
    printf("a palettised texel in one 32-bit word: the same bytes, in the same order\n");

    // The upload now writes one word per texel instead of four bytes (FTextureCache::Convert).
    // The word must land in memory as R, G, B, A - the R8G8B8A8 texture's byte order - and
    // index 0 of a masked texture must still be the transparent one.
    bool Ok = true;
    for (int Masked = 0; Masked < 2; Masked++)
        for (int i = 0; i < 256; i++)
        {
            FRgba E;
            E.R = (unsigned char)(i * 7 + 3);
            E.G = (unsigned char)(i * 13 + 5);
            E.B = (unsigned char)(i * 29 + 11);
            E.A = (unsigned char)(i * 3);          // the palette's own alpha: not used
            const unsigned int W = PackRgba(PaletteTexel((unsigned char)i, E, Masked != 0));
            unsigned char Bytes[4];
            memcpy(Bytes, &W, 4);
            const unsigned char A = (Masked && i == 0) ? 0 : 255;
            if (Bytes[0] != E.R || Bytes[1] != E.G || Bytes[2] != E.B || Bytes[3] != A)
                Ok = false;
        }
    Check(Ok, "for every entry, masked and not: R, G, B, alpha");
}

static void TestOutputCurve()
{
    printf("the brightness curve on the finished picture\n");

    // The three measured settings.
    CheckNear(OutputExponent(0.64f), 0.625f, 1e-4f, "at brightness 0.64 it is 1/1.6");
    CheckNear(OutputExponent(0.5f),  0.8f,   1e-4f, "at brightness 0.5 it is 0.8");
    CheckNear(OutputExponent(0.8f),  0.5f,   1e-4f, "at brightness 0.8 it is 0.5");
    // Measured between the stock renderer's own captures of one frame.
    CheckNear(OutputExponent(0.64f) / OutputExponent(0.5f), 0.782f, 0.002f, "the 0.5 -> 0.64 ratio, as measured");
    CheckNear(OutputExponent(0.8f)  / OutputExponent(0.5f), 0.625f, 0.002f, "the 0.5 -> 0.8 ratio, as measured");
    // A zero or absurd setting must neither divide by zero nor turn the curve over.
    Check(OutputExponent(0.0f) > 0.0f && OutputExponent(0.0f) < 100.0f, "zero brightness does not divide by zero");
    Check(OutputExponent(100.0f) > 0.0f, "an excessive brightness does not invert it");
}

static void TestUploadGeometry()
{
    printf("upload layout\n");

    Check(AlignRowPitch(1)    == 256,  "even 1 byte rounds up to a full row");
    Check(AlignRowPitch(256)  == 256,  "on the boundary it does not round up");
    Check(AlignRowPitch(257)  == 512,  "above the boundary it goes to the next one");
    Check(AlignRowPitch(1024) == 1024, "a multiple is left unchanged");

    // A 1024x1024 DXT1 texture: 256 blocks per row, 8 bytes each.
    Check(Bc1RowBytes(1024) == 2048, "BC1 row bytes at width 1024");
    Check(Bc1RowCount(1024) == 256,  "BC1 row count at height 1024");
    // Non-multiples of four still round up to whole blocks.
    Check(Bc1RowBytes(1) == 8,  "BC1: even one pixel is a whole block");
    Check(Bc1RowCount(5) == 2,  "BC1: 5 rows are two block rows");

    // Measured polygon sizes: 3..8 points, never degenerate (docs/meres-01 1.6).
    Check(FanVertexCount(3) == 3,  "a triangle is one triangle");
    Check(FanVertexCount(4) == 6,  "a quad is two triangles");
    Check(FanVertexCount(8) == 18, "the largest polygon seen is six triangles");
    Check(FanVertexCount(2) == 0,  "a degenerate polygon produces no geometry");
    Check(FanVertexCount(0) == 0,  "an empty polygon produces no geometry");
}

static void TestShadowPotential()
{
    printf("shadows: the estimated strength of a light on an actor (light selection)\n");

    // Point light, brightness 1: (1 - d/R)^2.
    CheckNear(ShadowPotential(1.0f, 0.0f,   100.0f, false, 0.0f, 0.0f), 1.0f,  1e-5f, "point light, at the light itself: 1");
    CheckNear(ShadowPotential(1.0f, 50.0f,  100.0f, false, 0.0f, 0.0f), 0.25f, 1e-5f, "point light, half a radius away: 0.25");
    CheckNear(ShadowPotential(0.5f, 50.0f,  100.0f, false, 0.0f, 0.0f), 0.125f, 1e-5f, "at half brightness, half as much");
    Check(ShadowPotential(1.0f, 100.0f, 100.0f, false, 0.0f, 0.0f) == 0.0f, "at the edge of the radius it is already 0");
    Check(ShadowPotential(1.0f, 150.0f, 100.0f, false, 0.0f, 0.0f) == 0.0f, "outside the radius it is 0");
    Check(ShadowPotential(1.0f, 10.0f,  0.0f,   false, 0.0f, 0.0f) == 0.0f, "zero radius: 0, and no division by zero");
    // Spot, half angle cos 0.5 (the shader's rule: (cos - cosHalf) / (1 - cosHalf), clamped).
    CheckNear(ShadowPotential(1.0f, 50.0f, 100.0f, true, 1.0f,  0.5f), 0.25f,  1e-5f, "on the cone axis: the same as a point light");
    CheckNear(ShadowPotential(1.0f, 50.0f, 100.0f, true, 0.75f, 0.5f), 0.125f, 1e-5f, "halfway out the cone: half as much");
    Check(ShadowPotential(1.0f, 50.0f, 100.0f, true, 0.5f,  0.5f) == 0.0f, "at the edge of the cone: 0");
    Check(ShadowPotential(1.0f, 50.0f, 100.0f, true, 0.19f, 0.5f) == 0.0f, "outside the cone: 0");
    Check(ShadowPotential(1.0f, 50.0f, 100.0f, true, 1.0f,  1.0f) >= 0.0f, "LightCone 0 (cos 1): no division by zero");
}

static void TestRelativeShadowShare()
{
    printf("shadows: a light's share of the pixel's total light (relative strength)\n");

    CheckNear(RelativeShadowShare(0.03f, 0.03f, 1.0f, 0.9f), 0.9f,  1e-5f, "all of the pixel from this light: the ceiling");
    CheckNear(RelativeShadowShare(0.03f, 0.06f, 1.0f, 1.0f), 0.5f,  1e-5f, "half of it from this light: 0.5");
    CheckNear(RelativeShadowShare(0.03f, 0.6f,  1.0f, 1.0f), 0.05f, 1e-5f, "a small share on a bright pixel");
    CheckNear(RelativeShadowShare(0.03f, 0.0f,  1.0f, 1.0f), 1.0f,  1e-5f, "unlit pixel: no division by zero, clamped at 1");
    CheckNear(RelativeShadowShare(0.03f, 0.06f, 2.0f, 1.0f), 1.0f,  1e-5f, "the calibration multiplies, clamped at 1");
    Check(RelativeShadowShare(0.0f, 0.5f, 1.0f, 0.9f) == 0.0f, "zero contribution: no shadow");
    // The measured case (the shadow test save): brightness 16/255 at (1 - d/R)^2 = 0.55 is 3.5% absolutely - on a street lit ~4% in all.
    Check(RelativeShadowShare(0.0627f * 0.55f, 0.04f, 1.0f, 0.9f) > 0.5f, "a faint light on a dark street: a strong shadow");
}

static void TestCaption()
{
    printf("on-screen caption\n");

    Check(CaptionFontWellFormed(), "every glyph is 7 rows of 5 dots, and none is defined twice");

    // S: 01111 / 10000 / 10000 / 01110 | 00001 / 00001 / 11110 - bit Row * 5 + Column.
    unsigned Lo = 0, Hi = 0;
    Check(CaptionGlyph('S', Lo, Hi), "S is present");
    Check(Lo == 0x7043Eu, "S: the top four rows");
    Check(Hi == 0x3E10u,  "S: the bottom three rows");
    unsigned Lo2 = 1, Hi2 = 1;
    Check(CaptionGlyph('s', Lo2, Hi2) && Lo2 == Lo && Hi2 == Hi, "lower case equals upper case");
    Lo2 = Hi2 = 1;
    Check(!CaptionGlyph(' ', Lo2, Hi2) && Lo2 == 0 && Hi2 == 0, "space: no dots");
    Check(AddressSpaceLow(AddressSpaceLowMB - 1) && AddressSpaceLow(0) && !AddressSpaceLow(AddressSpaceLowMB) &&
          !AddressSpaceLow(-1),
          "the address space is low under its limit, not at it - and never where the system gave no number");
    Lo2 = Hi2 = 1;
    Check(!CaptionGlyph('#', Lo2, Hi2) && Lo2 == 0 && Hi2 == 0, "an unknown character behaves as a space");
    bool bAll = true;
    for (char C = 'A'; C <= 'Z'; C++)
        bAll = bAll && CaptionGlyph(C, Lo2, Hi2) && (Lo2 | Hi2) != 0;
    for (char C = '0'; C <= '9'; C++)
        bAll = bAll && CaptionGlyph(C, Lo2, Hi2) && (Lo2 | Hi2) != 0;
    Check(bAll, "every letter and digit exists, and each has at least one dot");
    bAll = true;
    for (const char* P = ":.,-/%()=+<>"; *P; P++)
        bAll = bAll && CaptionGlyph(*P, Lo2, Hi2) && (Lo2 | Hi2) != 0;
    Check(bAll, "every sign the menu and the readout write exists - a missing one would show as a space");

    Check(CaptionScale(2160) == 4, "4K: one dot is 4 pixels");
    Check(CaptionScale(1080) == 2, "1080: one dot is 2 pixels");
    Check(CaptionScale(100)  == 1, "a small picture: at least 1");
    Check(CaptionScale(3840, 2160) == 4 && CaptionScale(1920, 1080) == 2, "landscape: by the height, as before");
    Check(CaptionScale(1080, 1920) == 2, "a monitor turned upright: by its width, so a line still fits across");
    Check(CaptionScaleToFit(41, 19, 768, 2) == 2, "a block that fits the picture keeps the picture's scale");
    Check(CaptionScaleToFit(46, 19, 768, 2) == 1, "a block taller than the picture is drawn a size smaller");
    Check(CaptionScaleToFit(46, 54, 2160, 4) == 4, "4K: the tallest block fits at its own scale");
    Check(CaptionScaleToFit(46, 7, 300, 1) == 1, "never below one");
    {
        FTargetAsk Ask;
        Ask.SceneW = 7680; Ask.SceneH = 4320; Ask.OutW = 3840; Ask.OutH = 2160; Ask.Colours = 24; Ask.FactorDiv = 2;
        FTargetRefusal Refusal;
        Check(!Refusal.Stands(Ask) && !Refusal.Stands(FTargetAsk()), "a target never refused is held back by nothing");
        Refusal.Note(Ask);
        Check(Refusal.Stands(Ask), "refused: not asked again while everything stands as it was asked");
        FTargetAsk Other = Ask;
        Other.SceneW = 3840; Other.SceneH = 2160;
        Check(!Refusal.Stands(Other), "a smaller scene under the same picture asks again");
        Other = Ask;
        Other.OutW = 1920; Other.OutH = 1080;
        Check(!Refusal.Stands(Other), "another picture size asks again");
        Other = Ask;
        Other.Colours = 28;
        Check(!Refusal.Stands(Other), "other colours ask again");
        Other = Ask;
        Other.FactorDiv = 1;
        Check(!Refusal.Stands(Other), "another size of the shadow factor asks again");
        Refusal.Note(Other);
        Check(!Refusal.Stands(Ask) && Refusal.Stands(Other), "refused again: what was asked before is asked anew");
        // The way there and back: refused at one size, the size changed and changed back.
        FTargetRefusal Back;
        Back.Note(Ask);
        Back.Keep(Ask);
        Check(Back.Stands(Ask), "kept while what was asked stands, frame after frame");
        Back.Keep(Other);
        Check(!Back.Stands(Other), "another ask: nothing holds the target back");
        Back.Keep(Ask);
        Check(!Back.Stands(Ask), "back at the sizes it was refused at: asked again, not left out for good");
    }
    Check(CaptionWidth(CaptionLineChars, CaptionScale(1080, 1920)) <= 1080, "the longest line fits the upright monitor");
    Check(CaptionLineChars > CaptionMaxChars, "a line is longer than one draw - DrawCaption draws it in pieces");
    Check(CaptionWidth(10, 5) == 305, "10 characters are 305 pixels wide at 4K");
    Check(CaptionHeight(5) == 45, "45 pixels tall at 4K");

    // A root signature holds at most 64 32-bit units: with 68 the caption's pipeline was refused (D3D12 ERROR #699,
    //). The shader's G array (15 x 4) must match as well.
    Check(CaptionConstants <= 64, "the caption's constants fit a root signature (64 at most)");
    Check(CaptionConstants == 4 + 2 * CaptionMaxChars, "4 header values plus 2 per character");
    Check(2 * CaptionMaxChars == 15 * 4, "the shader's G array (15 x 4) is exactly the characters' values");
}

static bool IsPowerOfTwo(int V)
{
    return V > 0 && (V & (V - 1)) == 0;
}

static void TestShadowPresets()
{
    printf("shadows: the ShadowQuality presets (Off / Low / Medium / High)\n");

    const FShadowPreset Off  = ShadowPresetFor(ShadowQuality_Off);
    const FShadowPreset Low  = ShadowPresetFor(ShadowQuality_Low);
    const FShadowPreset Med  = ShadowPresetFor(ShadowQuality_Medium);
    const FShadowPreset High = ShadowPresetFor(ShadowQuality_High);
    Check(Off.MaxLights == 0 && Off.MaxCasters == 0, "Off: no shadow maps - the vanilla behaviour");
    // A level is not simply "more lights": past a point more lights wash the shadows out, because each one lifts
    // the pixel's total light and every shadow's share of it shrinks. What separates the levels is a graded
    // selection instead - the nearest MaxLights get shadows, of which FullLights count at full strength and the
    // rest fade with distance.
    Check(Low.MaxLights == 10 && Med.MaxLights == 14 && High.MaxLights == 20, "the levels are 10, 14 and 20 lights");
    Check(Low.FullLights == 5 && Med.FullLights == 7 && High.FullLights == 10, "of those, at full strength: 5, 7 and 10");
    Check(Off.FullLights == 0, "Off: not one light at full strength either");
    CheckNear(Low.Reach,   50.0f * UnitsPerMetre, 0.01f, "Low: 50 metres");
    CheckNear(Med.Reach,   75.0f * UnitsPerMetre, 0.01f, "Medium: 75 metres");
    CheckNear(High.Reach, 100.0f * UnitsPerMetre, 0.01f, "High: 100 metres");
    // The levels differ in the light count and the reach, and in nothing else: every other field already sits at
    // its best value in Low, so raising it would cost without showing.
    Check(Low.ActorMapSize == High.ActorMapSize && Low.WorldMapSize == High.WorldMapSize &&
          Low.MaxCasters == High.MaxCasters && Low.FilterTaps == High.FilterTaps &&
          Low.bFullResolution == High.bFullResolution, "every other field is the same in all three levels");
    // The frame (ShadowMaxLights) is deliberately larger than the top level needs: the shader's array size and the
    // constant buffer come from it, so a level can be raised for an experiment without touching the shader. What
    // matters is that the top level fits inside it.
    Check(High.MaxLights <= ShadowMaxLights, "the top level fits within the frame");
    Check(Low.MaxLights <= Med.MaxLights && Med.MaxLights <= High.MaxLights, "the light count grows with the level");
    Check(Low.Reach <= Med.Reach && Med.Reach <= High.Reach, "the distance grows with the level");
    Check(Low.ActorMapSize <= Med.ActorMapSize && Med.ActorMapSize <= High.ActorMapSize, "the actors' map grows with the level");
    Check(Low.WorldMapSize <= Med.WorldMapSize && Med.WorldMapSize <= High.WorldMapSize, "the level's map grows with the level");
    Check(Low.MaxCasters <= Med.MaxCasters && Med.MaxCasters <= High.MaxCasters, "the caster count grows with the level");
    Check(Low.FilterTaps <= Med.FilterTaps && Med.FilterTaps <= High.FilterTaps, "the soft-edge taps grow with the level");
    bool bSane = true;
    for (int q = ShadowQuality_Low; q <= ShadowQuality_High; q++)
    {
        const FShadowPreset P = ShadowPresetFor((EShadowQuality)q);
        bSane = bSane && P.MaxLights > 0 && P.MaxLights <= ShadowMaxLights && IsPowerOfTwo(P.ActorMapSize) &&
                IsPowerOfTwo(P.WorldMapSize) && P.MaxCasters > 0 && P.FilterTaps > 0 && P.Reach > 0.0f &&
                P.FullLights > 0 && P.FullLights < P.MaxLights;
    }
    Check(bSane, "every preset: 1..ShadowMaxLights lights, power-of-two maps, some casters, at least one tap");

    bool bKnown = false;
    Check(ParseShadowQuality("High", bKnown) == ShadowQuality_High && bKnown, "High");
    Check(ParseShadowQuality(L"medium", bKnown) == ShadowQuality_Medium && bKnown, "medium - lower case, wide characters");
    Check(ParseShadowQuality("LOW", bKnown) == ShadowQuality_Low && bKnown, "LOW");
    Check(ParseShadowQuality("Off", bKnown) == ShadowQuality_Off && bKnown, "Off");
    Check(ParseShadowQuality(" high\t", bKnown) == ShadowQuality_High && bKnown, "spaces and tabs at the edges");
    Check(ParseShadowQuality("0", bKnown) == ShadowQuality_Off && bKnown, "0 = Off");
    Check(ParseShadowQuality("3", bKnown) == ShadowQuality_High && bKnown, "3 = High");
    Check(ParseShadowQuality("RT", bKnown) == ShadowQuality_High && !bKnown, "RT alone is no word any more - unknown, High");
    Check(ParseShadowQuality(" rt high ", bKnown) == ShadowQuality_Rt && bKnown, "rt high, as the menu says it");
    Check(ParseShadowQuality("4", bKnown) == ShadowQuality_Rt && bKnown, "4 = RT High");
    Check(ParseShadowQuality("RT-Low", bKnown) == ShadowQuality_RtLow && bKnown, "RT-Low");
    Check(ParseShadowQuality(" rt low ", bKnown) == ShadowQuality_RtLow && bKnown, "rt low - a space inside counts for nothing");
    Check(ParseShadowQuality(L"RTMEDIUM", bKnown) == ShadowQuality_RtMedium && bKnown, "RTMEDIUM, wide characters");
    Check(ParseShadowQuality("rt_medium", bKnown) == ShadowQuality_RtMedium && bKnown, "rt_medium");
    Check(ParseShadowQuality("RT-High", bKnown) == ShadowQuality_Rt && bKnown, "RT-High is RT");
    Check(ParseShadowQuality("RTHigh", bKnown) == ShadowQuality_Rt && bKnown, "RTHigh, one word - what the file gets");
    Check(ParseShadowQuality("5", bKnown) == ShadowQuality_RtLow && bKnown, "5 = RT Low");
    Check(ParseShadowQuality("6", bKnown) == ShadowQuality_RtMedium && bKnown, "6 = RT Medium");
    Check(ParseShadowQuality("7", bKnown) == ShadowQuality_High && !bKnown, "7 is no quality: High, and it says so");
    Check(ParseShadowQuality("RT-Ultra", bKnown) == ShadowQuality_High && !bKnown, "RT-Ultra: unknown");
    {
        // Each RT quality takes its maps' quality's lights and casters - the same shadows, traced - and that quality runs
        // where rays cannot.
        const EShadowQuality Rts[3]  = { ShadowQuality_RtLow, ShadowQuality_RtMedium, ShadowQuality_Rt };
        const EShadowQuality Maps[3] = { ShadowQuality_Low, ShadowQuality_Medium, ShadowQuality_High };
        bool bSame = true;
        for (int k = 0; k < 3; k++)
        {
            const FShadowPreset R = ShadowPresetFor(Rts[k]), M = ShadowPresetFor(Maps[k]);
            bSame = bSame && R.MaxLights == M.MaxLights && R.FullLights == M.FullLights && R.Reach == M.Reach &&
                    R.MaxCasters == M.MaxCasters && R.bFullResolution == M.bFullResolution &&
                    ShadowMapQualityFor(Rts[k]) == Maps[k] && IsRtShadowQuality(Rts[k]) && !IsRtShadowQuality(Maps[k]);
        }
        Check(bSame, "RT Low, RT Medium, RT High: Low's, Medium's and High's lights and casters, and those in their place");
        Check(ShadowMapQualityFor(ShadowQuality_Off) == ShadowQuality_Off && !IsRtShadowQuality(ShadowQuality_Off),
              "Off stays Off");
        Check(ShadowPresetFor(ShadowQuality_RtLow).MaxLights < ShadowPresetFor(ShadowQuality_RtMedium).MaxLights &&
              ShadowPresetFor(ShadowQuality_RtMedium).MaxLights < ShadowPresetFor(ShadowQuality_Rt).MaxLights,
              "RT Low < RT Medium < RT High in lights");
        // The menu's round: Off, Low, Medium, High, RT Low, RT Medium, RT High.
        EShadowQuality Q = ShadowQuality_Off;
        const EShadowQuality Round[7] = { ShadowQuality_Low, ShadowQuality_Medium, ShadowQuality_High, ShadowQuality_RtLow,
                                          ShadowQuality_RtMedium, ShadowQuality_Rt, ShadowQuality_Off };
        bool bRound = true;
        for (int k = 0; k < 7; k++)
        {
            Q = ShadowQualityStep(Q, 1);
            bRound = bRound && Q == Round[k];
        }
        Check(bRound, "the menu steps Off, Low, Medium, High, RT Low, RT Medium, RT High and round again");
        Check(ShadowQualityStep(ShadowQuality_Off, -1) == ShadowQuality_Rt &&
              ShadowQualityStep(ShadowQuality_RtLow, -1) == ShadowQuality_High, "and back");
    }
    Check(ParseShadowQuality("Ultra", bKnown) == ShadowQuality_High && !bKnown, "unknown: High, and it says so");
    Check(ParseShadowQuality("Highest", bKnown) == ShadowQuality_High && !bKnown, "a longer word does not match");
    Check(ParseShadowQuality("Hig", bKnown) == ShadowQuality_High && !bKnown, "a shorter word does not match");
    Check(ParseShadowQuality("", bKnown) == ShadowQuality_High && !bKnown, "empty: High, and it says so");
    Check(ParseShadowQuality((const char*)0, bKnown) == ShadowQuality_High && !bKnown, "no text at all: High, and it says so");
}

static float Row(const float* M, int R, const FVec3& P)
{
    return M[R * 4] * P.X + M[R * 4 + 1] * P.Y + M[R * 4 + 2] * P.Z + M[R * 4 + 3];
}

static void TestCubeFaces()
{
    printf("shadows: the faces of a cube shadow map (per Direct3D's table)\n");

    bool bOrtho = true;
    for (int f = 0; f < 6; f++)
    {
        FVec3 R, D, F;
        CubeFaceAxes(f, R, D, F);
        bOrtho = bOrtho && fabsf(Dot(R, R) - 1.0f) < 1e-6f && fabsf(Dot(D, D) - 1.0f) < 1e-6f &&
                 fabsf(Dot(F, F) - 1.0f) < 1e-6f && fabsf(Dot(R, D)) < 1e-6f && fabsf(Dot(R, F)) < 1e-6f &&
                 fabsf(Dot(D, F)) < 1e-6f;
    }
    Check(bOrtho, "every face's axes are unit length and perpendicular");

    // Directions over a grid (offset so that no two components are equal): drawn with the matrix of the face the sampler
    // picks, a point lands where the sampler reads it, at the depth CubeDepth gives, and inside no other face's square.
    const FVec3 Light = { 1200.0f, -340.0f, 96.0f };
    const float Near  = 1.0f;
    int Tried = 0, Wrong = 0, WrongDepth = 0, Doubled = 0;
    for (int i = -3; i <= 3; i++)
        for (int j = -3; j <= 3; j++)
            for (int k = -3; k <= 3; k++)
            {
                // The middle one is within Near of the light: cut off, nothing to read there.
                if (i == 0 && j == 0 && k == 0)
                    continue;
                const FVec3 Dir = { (float)i * 37.0f + 0.3f, (float)j * 41.0f - 0.7f, (float)k * 29.0f + 0.11f };
                float S = 0.0f, T = 0.0f;
                const int   Face = CubeFaceLookup(Dir, S, T);
                const FVec3 P    = { Light.X + Dir.X, Light.Y + Dir.Y, Light.Z + Dir.Z };
                float M[16];
                CubeFaceMatrix(Face, Light, Near, M);
                const float X = Row(M, 0, P), Y = Row(M, 1, P), Z = Row(M, 2, P), W = Row(M, 3, P);
                Tried++;
                if (!(W > 0.0f) || fabsf(0.5f * (X / W + 1.0f) - S) > 1e-4f || fabsf(0.5f * (1.0f - Y / W) - T) > 1e-4f)
                    Wrong++;
                if (fabsf(Z / W - CubeDepth(Dir, Near)) > 1e-6f)
                    WrongDepth++;
                for (int g = 0; g < 6; g++)
                {
                    if (g == Face)
                        continue;
                    CubeFaceMatrix(g, Light, Near, M);
                    const float Xg = Row(M, 0, P), Yg = Row(M, 1, P), Wg = Row(M, 3, P);
                    if (Wg > 0.0f && fabsf(Xg / Wg) < 1.0f && fabsf(Yg / Wg) < 1.0f)
                        Doubled++;
                }
            }
    Check(Tried == 342 && Wrong == 0, "the face's matrix draws where the sampler reads from (342 directions)");
    Check(WrongDepth == 0, "depth is Near divided by the largest component");
    Check(Doubled == 0, "one direction falls into exactly one face square");

    // The four corners of +Z's square: s and t 0 or 1, as the table says (s from +x, t from -y).
    float S = 0.0f, T = 0.0f;
    const FVec3 C1 = { -0.999f, 0.999f, 1.0f };
    Check(CubeFaceLookup(C1, S, T) == 4 && S < 0.001f && T < 0.001f, "+Z face, (-x, +y) corner: s = 0, t = 0");
    const FVec3 C2 = { 0.999f, -0.999f, 1.0f };
    Check(CubeFaceLookup(C2, S, T) == 4 && S > 0.999f && T > 0.999f, "+Z face, (+x, -y) corner: s = 1, t = 1");

    FVec3 V1 = { 1.0f, 0.2f, -0.3f };
    CheckNear(CubeDepth(V1, 1.0f), 1.0f, 1e-6f, "1 at the near distance");
    FVec3 V2 = { 0.0f, -400.0f, 10.0f };
    CheckNear(CubeDepth(V2, 1.0f), 1.0f / 400.0f, 1e-7f, "1/400 at 400 units");
    Check(CubeDepth(V2, 1.0f) < CubeDepth(V1, 1.0f), "smaller further away (reversed depth)");
    float M[16];
    CubeFaceMatrix(0, Light, Near, M);
    const FVec3 Close = { Light.X + 0.5f, Light.Y + 0.1f, Light.Z };
    Check(Row(M, 2, Close) / Row(M, 3, Close) > 1.0f, "a point nearer the light than Near drops out (z / w > 1)");

    // Which faces a caster's sphere reaches - FShadowMaps::Render draws it into those only.
    const FVec3 Ahead = { 300.0f, 10.0f, -20.0f };
    Check(SphereTouchesCubeFace(0, Ahead, 16.0f) && !SphereTouchesCubeFace(1, Ahead, 16.0f) &&
          !SphereTouchesCubeFace(2, Ahead, 16.0f) && !SphereTouchesCubeFace(4, Ahead, 16.0f),
          "a sphere out along +X: only the +X face");
    const FVec3 Edge = { 200.0f, 0.0f, 200.0f };
    Check(SphereTouchesCubeFace(0, Edge, 8.0f) && SphereTouchesCubeFace(4, Edge, 8.0f) &&
          !SphereTouchesCubeFace(1, Edge, 8.0f) && !SphereTouchesCubeFace(5, Edge, 8.0f),
          "on the border of +X and +Z: both, and neither opposite face");
    const FVec3 AtLight = { 0.0f, 0.0f, 0.0f };
    bool bAllFaces = true;
    for (int f = 0; f < 6; f++)
        bAllFaces = bAllFaces && SphereTouchesCubeFace(f, AtLight, 10.0f);
    Check(bAllFaces, "a sphere around the light: every face");
    // Never too strict: no point of a sphere (its centre and 26 points on its surface) lands on a face the test leaves out.
    int Missed = 0;
    for (int s = 0; s < 200; s++)
    {
        const FVec3 Ctr = { (float)((s * 37) % 400 - 200), (float)((s * 53) % 400 - 200), (float)((s * 71) % 400 - 200) };
        const float Rad = 5.0f + (float)(s % 60);
        for (int a = -1; a <= 1; a++)
            for (int b = -1; b <= 1; b++)
                for (int c = -1; c <= 1; c++)
                {
                    const float Len = (a || b || c) ? sqrtf((float)(a * a + b * b + c * c)) : 1.0f;
                    const FVec3 Q = { Ctr.X + Rad * (float)a / Len, Ctr.Y + Rad * (float)b / Len, Ctr.Z + Rad * (float)c / Len };
                    if (Q.X == 0.0f && Q.Y == 0.0f && Q.Z == 0.0f)
                        continue;
                    float S0 = 0.0f, T0 = 0.0f;
                    if (!SphereTouchesCubeFace(CubeFaceLookup(Q, S0, T0), Ctr, Rad))
                        Missed++;
                }
    }
    Check(Missed == 0, "no point of the sphere falls on a face the test skips");
    // CubeFacesTouched: face by face the same answer as SphereTouchesCubeFace - on spheres all around the light, and on the
    // faces' borders exactly (the diagonals, radius 0).
    int Differ = 0;
    unsigned Seed = 12345u;
    for (int s = 0; s < 20000; s++)
    {
        FVec3 V;
        Seed = Seed * 1664525u + 1013904223u;
        V.X = (float)((int)((Seed >> 8) % 4001u) - 2000) * 0.37f;
        Seed = Seed * 1664525u + 1013904223u;
        V.Y = (float)((int)((Seed >> 8) % 4001u) - 2000) * 0.37f;
        Seed = Seed * 1664525u + 1013904223u;
        V.Z = (float)((int)((Seed >> 8) % 4001u) - 2000) * 0.37f;
        Seed = Seed * 1664525u + 1013904223u;
        const float Rad = (float)((Seed >> 8) % 3000u) * 0.1f;
        const unsigned Mask = CubeFacesTouched(V, Rad);
        for (int f = 0; f < 6; f++)
            if (((Mask >> f) & 1u) != (SphereTouchesCubeFace(f, V, Rad) ? 1u : 0u))
                Differ++;
    }
    for (int a = -2; a <= 2; a++)
        for (int b = -2; b <= 2; b++)
            for (int c = -2; c <= 2; c++)
                for (int r = 0; r < 2; r++)
                {
                    const FVec3 V = { 50.0f * (float)a, 50.0f * (float)b, 50.0f * (float)c };
                    const float Rad = (r == 0) ? 0.0f : 10.0f;
                    const unsigned Mask = CubeFacesTouched(V, Rad);
                    for (int f = 0; f < 6; f++)
                        if (((Mask >> f) & 1u) != (SphereTouchesCubeFace(f, V, Rad) ? 1u : 0u))
                            Differ++;
                }
    Check(Differ == 0, "CubeFacesTouched agrees with SphereTouchesCubeFace on every face");
}

static void TestShadowOccluders()
{
    printf("shadows: what goes into the shadow maps\n");

    Check(!BspBlocksLight(RDX_PF_Portal), "a zone portal does not occlude");
    Check(BspBlocksLight(0), "an ordinary wall occludes");
    Check(BspBlocksLight(RDX_PF_Masked) && BspBlocksLight(RDX_PF_Translucent) && BspBlocksLight(RDX_PF_Invisible) &&
          BspBlocksLight(RDX_PF_FakeBackdrop), "masked, translucent, invisible, sky: occlude");

    Check(MeshFaceCastsShadow(0), "an ordinary model face casts a shadow");
    Check(!MeshFaceCastsShadow(RDX_PF_Invisible), "an invisible face does not");
    Check(!MeshFaceCastsShadow(RDX_PF_Translucent) && !MeshFaceCastsShadow(RDX_PF_Modulated), "translucent and modulated do not");
    Check(MeshFaceCastsShadow(RDX_PF_Masked) && MeshFaceMasked(RDX_PF_Masked), "masked (foliage): yes, with the texture's mask");
    Check(!MeshFaceMasked(0) && !MeshFaceMasked(RDX_PF_Translucent), "a non-masked face: no mask");

    // DXT1's one-bit alpha (the masks of masked casters).
    unsigned char Block[8] = { 0x00, 0x00, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0xC0 };   // C0 <= C1: index 3 transparent
    Check(Dxt1TexelTransparent(Block, 0, 0) && Dxt1TexelTransparent(Block, 3, 3), "DXT1: index 3 is transparent when C0 <= C1");
    Check(!Dxt1TexelTransparent(Block, 1, 0) && !Dxt1TexelTransparent(Block, 0, 3), "DXT1: index 0 is not");
    Block[0] = 0xFF; Block[1] = 0xFF; Block[2] = 0x00; Block[3] = 0x00;                    // C0 > C1: four colours, no alpha
    Check(!Dxt1TexelTransparent(Block, 0, 0) && !Dxt1TexelTransparent(Block, 3, 3), "DXT1: with C0 > C1 nothing is transparent");

    // The plan's rule, the one behind the marker.
    Check(BspBlocksLightSeeThrough(0), "occlusion rule: an ordinary wall occludes");
    Check(!BspBlocksLightSeeThrough(RDX_PF_Portal) && !BspBlocksLightSeeThrough(RDX_PF_FakeBackdrop),
          "occlusion rule: a portal and the sky do not");
    Check(!BspBlocksLightSeeThrough(RDX_PF_Masked) && !BspBlocksLightSeeThrough(RDX_PF_Translucent) &&
          !BspBlocksLightSeeThrough(RDX_PF_Invisible) && !BspBlocksLightSeeThrough(RDX_PF_Modulated),
          "occlusion rule: masked, translucent, invisible and modulated do not");
    Check(BspBlocksLightSeeThrough(RDX_PF_Translucent | RDX_PF_Occlude),
          "occlusion rule: a translucent surface marked PF_Occlude does");
}

static void TestShadowLightPick()
{
    printf("shadows: choosing the lights (the nearest ones, in every direction, within X metres)\n");

    const float Reach = 50.0f * UnitsPerMetre;
    const FShadowLightChoice C[7] =
    {
        {  400.0f,  300.0f, true,  false },   // 0
        {  100.0f,  200.0f, true,  false },   // 1: the nearest
        {  900.0f,  500.0f, true,  false },   // 2
        {  250.0f,  400.0f, true,  false },   // 3: out of view, and it still counts
        {  300.0f,  400.0f, false, false },   // 4: no caster inside its sphere
        { 5000.0f, 1000.0f, true,  false },   // 5: its sphere ends at 4000, beyond the reach
        { 3000.0f,  500.0f, true,  false },   // 6: its centre is beyond the reach, its sphere reaches back to 2500
    };
    int Out[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    int N = PickShadowLights(C, 7, 8, Reach, 1.0f, Out);
    // Direction must not enter the choice: a lamp standing behind us casts our shadow forward, into view. Light 3
    // is out of view and still belongs in the list, on its distance alone.
    Check(N == 5 && Out[0] == 1 && Out[1] == 3 && Out[2] == 0 && Out[3] == 2 && Out[4] == 6,
          "reachable lights with casters, whatever the direction, nearest first");
    N = PickShadowLights(C, 7, 2, Reach, 1.0f, Out);
    Check(N == 2 && Out[0] == 1 && Out[1] == 3, "at most Max, the nearest ones - including those out of view");
    N = PickShadowLights(C, 7, 0, Reach, 1.0f, Out);
    Check(N == 0, "Max 0: nothing");

    FShadowLightChoice H[2] =
    {
        { 110.0f, 500.0f, true, true  },   // chosen the frame before
        { 100.0f, 500.0f, true, false },
    };
    N = PickShadowLights(H, 2, 1, Reach, 0.85f, Out);
    Check(N == 1 && Out[0] == 0, "one chosen last frame is kept when it is only slightly further (110 x 0.85 < 100)");
    H[0].Distance = 130.0f;
    N = PickShadowLights(H, 2, 1, Reach, 0.85f, Out);
    Check(N == 1 && Out[0] == 1, "but gives up its slot when it is much further (130 x 0.85 > 100)");

    // At the edge of the reach: a light already chosen stays a while beyond Reach, a new one does not enter.
    FShadowLightChoice E[2] =
    {
        { Reach + 500.0f, 300.0f, true, true  },    // its sphere ends 200 beyond Reach: 2825 x 0.85 < Reach
        { Reach + 500.0f, 300.0f, true, false },
    };
    N = PickShadowLights(E, 2, 2, Reach, 0.85f, Out);
    Check(N == 1 && Out[0] == 0, "at the edge of Reach: an already chosen light stays, a new one does not enter");
    E[0].Distance = Reach / 0.85f + 400.0f;               // its sphere ends 100 beyond Reach / 0.85
    N = PickShadowLights(E, 2, 2, Reach, 0.85f, Out);
    Check(N == 0, "beyond Reach / Keep even an already chosen light drops out");

    // The true distance of the nearest light left out - this is where the fading band ends.
    float Rej = 0.0f;
    N = PickShadowLights(C, 7, 2, Reach, 1.0f, Out, &Rej);
    // In: 1 (100) and 3 (250). Light 0 (400) entered and was then displaced by 3; 2 (900) and 6 (3000) never fitted.
    Check(N == 2 && Rej == 400.0f, "two slots: the nearest one left out is the displaced 400, not the 900 that never fitted");
    N = PickShadowLights(C, 7, 8, Reach, 1.0f, Out, &Rej);
    Check(N == 5 && Rej > 1.0e30f, "all fit: none left out, a very large value");
    N = PickShadowLights(C, 7, 0, Reach, 1.0f, Out, &Rej);
    Check(N == 0 && Rej == 100.0f, "zero slots: the nearest eligible one (100) is the one left out; excluded lights do not count");
}

static void TestShadowLightWeight()
{
    printf("shadows: the weight of graded light selection (the nearest at full strength, the rest fading)\n");

    // A row of lights in metres, with the eleventh at 20 m: 1,1,1,2,3 at full strength and 5,6,9,12,17 fading.
    // The range does not enter here - its edge is far away and its fade is zero.
    const float Full = 3.0f, End = 20.0f, Far = 1.0e6f;
    CheckNear(ShadowLightWeight(1.0f,  Full, End, Far, 0.0f), 1.0f,          1e-4f, "1 m: full");
    CheckNear(ShadowLightWeight(3.0f,  Full, End, Far, 0.0f), 1.0f,          1e-4f, "3 m, the last full one: full");
    CheckNear(ShadowLightWeight(5.0f,  Full, End, Far, 0.0f), 15.0f / 17.0f, 1e-4f, "5 m: 88%");
    CheckNear(ShadowLightWeight(9.0f,  Full, End, Far, 0.0f), 11.0f / 17.0f, 1e-4f, "9 m: 65%");
    CheckNear(ShadowLightWeight(17.0f, Full, End, Far, 0.0f),  3.0f / 17.0f, 1e-4f, "17 m: 18%");
    // The point of the whole curve: a light leaves the list at weight zero and a new one enters at zero, so a
    // swap cannot pop.
    CheckNear(ShadowLightWeight(20.0f, Full, End, Far, 0.0f), 0.0f, 1e-6f, "20 m, where the dropped light sits: zero");
    CheckNear(ShadowLightWeight(25.0f, Full, End, Far, 0.0f), 0.0f, 1e-6f, "zero beyond that too, never negative");
    Check(ShadowLightWeight(19.99f, Full, End, Far, 0.0f) < 0.001f, "a hair before the limit: almost zero");

    // No fading band at all - fewer lights than slots, so every one is at full strength...
    CheckNear(ShadowLightWeight(40.0f, 3.0e38f, 3.0e38f, Far, 0.0f), 1.0f, 1e-6f, "outdoors with few lights: all at full strength");
    // ...but the last fifth of the range still fades, or a light would vanish at full strength on the boundary.
    const float Edge = 100.0f, Fade = 20.0f;
    CheckNear(ShadowLightWeight(70.0f,  3.0e38f, 3.0e38f, Edge, Fade), 1.0f, 1e-6f, "well inside the range: full");
    CheckNear(ShadowLightWeight(90.0f,  3.0e38f, 3.0e38f, Edge, Fade), 0.5f, 1e-4f, "halfway through the fade: 50%");
    CheckNear(ShadowLightWeight(100.0f, 3.0e38f, 3.0e38f, Edge, Fade), 0.0f, 1e-6f, "at the edge of the range: zero");
    CheckNear(ShadowLightWeight(17.0f, Full, End, 27.0f, 20.0f), 3.0f / 17.0f, 1e-4f,
              "of the two fades the smaller wins (ranking 18%, range 50%)");
    // Full and End can coincide, and then the band has no width: no division by zero.
    CheckNear(ShadowLightWeight(5.0f, 5.0f, 5.0f, Far, 0.0f), 1.0f, 1e-6f, "Full = End: a full light is full");
    CheckNear(ShadowLightWeight(6.0f, 5.0f, 5.0f, Far, 0.0f), 0.0f, 1e-6f, "Full = End: the rest are zero, with no division");

    // Vertical weighting: the same distance above us counts as twice as far, so lights on our own floor win.
    CheckNear(WeightedLightDistance(3.0f, 4.0f,  0.0f, 2.0f), 5.0f, 1e-4f, "horizontally 3-4: 5, the weight does not enter");
    CheckNear(WeightedLightDistance(0.0f, 0.0f,  3.0f, 2.0f), 6.0f, 1e-4f, "3 units above us, weight 2: 6");
    CheckNear(WeightedLightDistance(0.0f, 0.0f, -3.0f, 2.0f), 6.0f, 1e-4f, "the same below us: 6");
    CheckNear(WeightedLightDistance(0.0f, 0.0f,  3.0f, 0.0f), 3.0f, 1e-4f, "an invalid weight (0): the true distance, not zero");

    // Where the full-strength band ends: the true distance of the FullLights-th nearest light, whatever order
    // the selection happened to produce.
    const FShadowLightChoice G[4] =
    {
        { 5.0f, 100.0f, true, false }, { 1.0f, 100.0f, true, false },
        { 9.0f, 100.0f, true, false }, { 3.0f, 100.0f, true, false },
    };
    const int Order[4] = { 0, 1, 2, 3 };   // deliberately NOT in distance order, as it can be after Keep
    CheckNear(GradedFullDistance(G, Order, 4, 2), 3.0f, 1e-6f, "4 lights, 2 full: the second nearest, 3");
    Check(GradedFullDistance(G, Order, 4, 4) > 1.0e30f, "4 lights, 4 full: no fading band");
    Check(GradedFullDistance(G, Order, 2, 5) > 1.0e30f, "fewer lights than slots: no fading band");

    // The smoothing is time-based, not frame-based: it settles in the same wall-clock time at any frame rate.
    float W60 = 0.0f, W150 = 0.0f;
    for (int f = 0; f < 15; f++) W60  = FollowOverTime(W60,  1.0f, 1.0f / 60.0f,  0.25f);   // 0,25 s
    for (int f = 0; f < 38; f++) W150 = FollowOverTime(W150, 1.0f, 1.0f / 150.0f, 0.25f);   // 0,253 s
    Check(W60 > 0.55f && W60 < 0.70f && W150 > 0.55f && W150 < 0.70f, "a quarter of a second: ~63% at either frame rate");
    Check(W60 - W150 < 0.05f && W150 - W60 < 0.05f, "the difference between the two frame rates is under 5%");
    CheckNear(FollowOverTime(0.3f, 1.0f, 0.0f, 0.25f), 0.3f, 1e-6f, "zero elapsed time: no movement");
    Check(FollowOverTime(0.0f, 1.0f, 1.0f, 0.25f) <= 1.0f, "a one-second hitch: it still does not overshoot");
}

// The flashlight's light as SolidPass.hlsl's BeamLight works it out, from the block's sixteen numbers alone.
static float BeamFromBlock(const float* B, float Dist, float Cos, float Facing)
{
    if (!(B[3] > 0.0f))
        return 0.0f;
    const float Hot   = BeamStep(0.0f, 1.0f, (Cos - B[4]) * B[5]);
    const float Spill = BeamStep(0.0f, 1.0f, (Cos - B[6]) * B[7]);
    const float Cone  = Hot + (1.0f - Hot) * B[11] * Spill;
    const float Near  = BeamStep(0.0f, 1.0f, (Dist - B[9]) * B[10]);
    const float Far   = 1.0f - BeamStep(0.75f * B[3], B[3], Dist);
    const float Fall  = 1.0f / (1.0f + (Dist / B[8]) * (Dist / B[8]));
    const float Faces = (Facing < 0.0f) ? 0.0f : ((Facing > 1.0f) ? 1.0f : Facing);
    return Cone * Near * Far * Fall * (B[15] + (1.0f - B[15]) * Faces);
}

static void TestFlashlightBeam()
{
    printf("flashlight: the cone of light from the eye\n");

    // Along the axis, on a surface that faces the eye: only the distance takes from it.
    CheckNear(BeamLight(BeamHalfDist, 1.0f, 1.0f), 0.5f, 1e-5f, "on the axis at the half distance: half");
    Check(BeamLight(100.0f, 1.0f, 1.0f) > BeamLight(300.0f, 1.0f, 1.0f), "nearer is brighter");
    Check(BeamLight(BeamNearStart, 1.0f, 1.0f) == 0.0f, "nothing where it begins - what is held in the hand");
    Check(BeamLight(0.0f, 1.0f, 1.0f) == 0.0f, "nothing at the eye itself");
    Check(BeamLight(BeamNearFull, 1.0f, 1.0f) > 0.9f, "and all but full where the near fade ends");
    Check(BeamLight(BeamReach, 1.0f, 1.0f) == 0.0f, "nothing at the reach's end");
    Check(BeamLight(BeamReach + 100.0f, 1.0f, 1.0f) == 0.0f, "nor past it");
    Check(BeamLight(0.95f * BeamReach, 1.0f, 1.0f) > 0.0f && BeamLight(1024.0f, 1.0f, 1.0f) > 0.1f,
          "but some until the reach's end - fading over its second half, and plain at 1024 units, the old cut");

    // Across the cone.
    const float Axis = BeamLight(200.0f, 1.0f, 1.0f);
    Check(BeamLight(200.0f, BeamHotCos, 1.0f) == Axis, "the hot spot is even to its edge");
    CheckNear(BeamLight(200.0f, BeamRimCos, 1.0f), BeamSpillShare * Axis, 1e-6f,
              "at the hot spot's rim the spill alone is left, its share");
    Check(BeamLight(200.0f, BeamSpillRimCos, 1.0f) == 0.0f, "nothing at the spill's rim");
    Check(BeamLight(200.0f, 0.5f, 1.0f) == 0.0f, "nothing outside the cone");
    Check(BeamLight(200.0f, -1.0f, 1.0f) == 0.0f, "nothing behind the eye");
    bool  bFalls = true, bInRange = true;
    float Last   = Axis;
    for (float C = 1.0f; C > 0.7f; C -= 0.002f)
    {
        const float V = BeamLight(200.0f, C, 1.0f);
        if (V > Last + 1e-6f)
            bFalls = false;
        if (V < 0.0f || V > 1.0f)
            bInRange = false;
        Last = V;
    }
    Check(bFalls, "from the axis outwards it never grows");
    Check(bInRange, "and it stays between nothing and all");

    // How the surface faces.
    CheckNear(BeamLight(200.0f, 1.0f, 0.0f), BeamFacingFloor * Axis, 1e-6f,
              "a surface seen along its length keeps the floor's share");
    Check(BeamLight(200.0f, 1.0f, 0.5f) > BeamLight(200.0f, 1.0f, 0.0f) && BeamLight(200.0f, 1.0f, 0.5f) < Axis,
          "one turned half away lies between");
    Check(BeamLight(200.0f, 1.0f, 2.0f) == Axis && BeamLight(200.0f, 1.0f, -1.0f) == BeamLight(200.0f, 1.0f, 0.0f),
          "a facing out of range is held to its ends");

    // How far out the mirrors' map is ever looked up.
    Check(BeamMapRimCos(0.0f) == 0.0f, "no projection, no rim: everything is kept");
    Check(BeamMapRimCos(1920.0f) < BeamSpillRimCos, "the map's rim lies outside the spill's own");
    Check(BeamMapRimCos(1920.0f) > BeamMapRimCos(960.0f), "and further outside it in a smaller picture");
    CheckNear(BeamMapRimCos(1.0e9f), BeamSpillRimCos, 1e-5f, "in an endless picture the two are one");
    {
        // A place on the spill's rim, moved outwards by the widest disc: still inside the map's rim.
        const float Focal = 1920.0f;
        const float Tan   = Sqrt(1.0f - BeamSpillRimCos * BeamSpillRimCos) / BeamSpillRimCos + BeamDiscMost / Focal;
        Check(1.0f / Sqrt(1.0f + Tan * Tan) > BeamMapRimCos(Focal),
              "the widest disc about the spill's rim is inside it");
    }

    // The block the shader reads, and the shader's own arithmetic from it.
    float       B[BeamBlockFloats];
    const float Colour[3] = { 1.0f, 0.8f, 0.6f };
    BeamBlock(Colour, 960.0f, 540.0f, 955.0f, B);
    Check(B[0] == 1.0f && B[1] == 0.8f && B[2] == 0.6f && B[3] == BeamReach, "the block: the colour and the reach");
    Check(B[12] == 960.0f && B[13] == 540.0f && B[14] == 955.0f && B[15] == BeamFacingFloor,
          "the block: the view's centre, its projection, the facing's floor");
    bool bSame = true;
    for (float D = 0.0f; D < 1100.0f; D += 37.0f)
        for (float C = 1.0f; C > 0.7f; C -= 0.013f)
            for (float F = 0.0f; F <= 1.0f; F += 0.25f)
            {
                const float A = BeamLight(D, C, F), S = BeamFromBlock(B, D, C, F);
                if (A - S > 1e-5f || S - A > 1e-5f)
                    bSame = false;
            }
    Check(bSame, "the shader's arithmetic from the block gives the same light everywhere");
    BeamBlock(nullptr, 960.0f, 540.0f, 955.0f, B);
    bool bNone = true;
    for (int i = 0; i < BeamBlockFloats; i++)
        if (B[i] != 0.0f)
            bNone = false;
    Check(bNone && BeamFromBlock(B, 200.0f, 1.0f, 1.0f) == 0.0f, "no colour: a block of zeros, and no light from it");
    // What is laid over the picture takes no more of the cone than the game's own light would give it.
    Check(BeamSeeThrough > 0.0f && BeamGain * BeamSeeThrough <= 1.0f + 1e-6f,
          "a see-through surface's share of the cone: some, and no more than the game's own light");
}

static void TestOwnLamps()
{
    printf("flashlight: the three ways, and the own lamps\n");

    // The switch.
    bool bK = false;
    Check(ParseFlashlight("Original", bK) == Flashlight_Original && bK, "Original reads as Original");
    Check(ParseFlashlight("fixed", bK) == Flashlight_Fixed && bK, "a value reads without its case");
    Check(ParseFlashlight("  Everybody", bK) == Flashlight_Everybody && bK, "Everybody, after spaces");
    Check(ParseFlashlight("Everbody", bK) == Flashlight_Fixed && !bK, "anything else: Fixed, and said to be unknown");
    Check(ParseFlashlight((const char*)nullptr, bK) == Flashlight_Fixed && !bK, "nothing: Fixed");
    Check(FlashlightStep(Flashlight_Original, 1) == Flashlight_Fixed &&
          FlashlightStep(Flashlight_Fixed, 1) == Flashlight_Everybody &&
          FlashlightStep(Flashlight_Everybody, 1) == Flashlight_Original &&
          FlashlightStep(Flashlight_Original, -1) == Flashlight_Everybody,
          "the menu steps Original, Fixed, Everybody and round again, either way");
    char Name[16];
    FlashlightName(Flashlight_Everybody, Name, 16);
    Check(strcmp(Name, "Everybody") == 0, "its name as the settings file spells it");
    const FPresetPicture R = PresetPicture(Preset_Recommended), I = PresetPicture(Preset_INeverAskedForThis),
                         V = PresetPicture(Preset_Vanilla);
    Check(R.Flashlight == Flashlight_Fixed && I.Flashlight == Flashlight_Everybody && V.Flashlight == Flashlight_Original,
          "the presets: Fixed in Recommended, Everybody in INeverAskedForThis, Original in Vanilla");

    // A lamp's light: the torch of the cone from the eye, with its own reach.
    Check(OwnLampLight(200.0f, 1.0f, 1.0f) == BeamLight(200.0f, 1.0f, 1.0f) &&
          OwnLampLight(300.0f, BeamRimCos, 1.0f) == BeamLight(300.0f, BeamRimCos, 1.0f),
          "within the first half of its reach the same light as the cone's");
    Check(OwnLampLight(BeamNearStart, 1.0f, 1.0f) == 0.0f && OwnLampLight(OwnLampReach, 1.0f, 1.0f) == 0.0f &&
          OwnLampLight(OwnLampReach + 10.0f, 1.0f, 1.0f) == 0.0f,
          "nothing at the lamp, at its reach's end or past it");
    Check(OwnLampLight(0.9f * OwnLampReach, 1.0f, 1.0f) > 0.0f, "some until the reach's end");
    Check(OwnLampLight(200.0f, BeamSpillRimCos, 1.0f) == 0.0f && OwnLampLight(200.0f, 0.5f, 1.0f) == 0.0f,
          "nothing at the spill's rim or outside it");
    Check(OwnLampLight(200.0f, 1.0f, 0.0f) == 0.0f && OwnLampLight(200.0f, 1.0f, -0.5f) == 0.0f,
          "a face the light runs along or that is turned away takes none");
    CheckNear(OwnLampLight(200.0f, 1.0f, 0.1f), (BeamFacingFloor + (1.0f - BeamFacingFloor) * 0.1f) *
              OwnLampLight(200.0f, 1.0f, 1.0f), 1e-6f, "from the first tenth on, the cone's facing floor");
    Check(OwnLampLight(200.0f, 1.0f, 0.05f) > 0.0f && OwnLampLight(200.0f, 1.0f, 0.05f) <
          OwnLampLight(200.0f, 1.0f, 0.1f), "and below it a ramp from nothing, so nothing jumps where a face turns");
    Check(OwnLampToward(1.0f) == 1.0f && OwnLampToward(BeamSpillRimCos) == 0.0f &&
          OwnLampToward(BeamRimCos) == BeamSpillShare, "the lamp seen along its axis, at its rim, outside it");

    // The map's rows.
    const FVec3 At = { 100.0f, -50.0f, 30.0f };
    const float Rt = 1.0f / Sqrt(3.0f);
    const FVec3 Aims[3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { Rt, Rt, -Rt } };
    bool bAxis = true, bEdge = true, bBehind = true;
    for (int a = 0; a < 3; a++)
    {
        float M[16];
        SpotMatrix(At, Aims[a], OwnLampMapTan, OwnLampNear, M);
        const auto Clip = [&](const FVec3& P, float* C)
        {
            for (int r = 0; r < 4; r++)
                C[r] = M[r * 4] * P.X + M[r * 4 + 1] * P.Y + M[r * 4 + 2] * P.Z + M[r * 4 + 3];
        };
        const FVec3 On = { At.X + Aims[a].X * 500.0f, At.Y + Aims[a].Y * 500.0f, At.Z + Aims[a].Z * 500.0f };
        float C[4];
        Clip(On, C);
        if (fabsf(C[0]) > 1e-3f || fabsf(C[1]) > 1e-3f || fabsf(C[2] - OwnLampNear) > 1e-5f || fabsf(C[3] - 500.0f) > 1e-2f)
            bAxis = false;
        // A place OwnLampMapTan off the axis to the map's right: at its edge.
        const FVec3 Right = { M[0] * OwnLampMapTan, M[1] * OwnLampMapTan, M[2] * OwnLampMapTan };
        const float Off   = 500.0f * OwnLampMapTan;
        const FVec3 Edge  = { On.X + Right.X * Off, On.Y + Right.Y * Off, On.Z + Right.Z * Off };
        Clip(Edge, C);
        if (fabsf(C[0] / C[3] - 1.0f) > 1e-3f || fabsf(C[1] / C[3]) > 1e-3f)
            bEdge = false;
        const FVec3 Back = { At.X - Aims[a].X * 100.0f, At.Y - Aims[a].Y * 100.0f, At.Z - Aims[a].Z * 100.0f };
        Clip(Back, C);
        if (C[3] >= 0.0f)
            bBehind = false;
    }
    Check(bAxis, "the map's rows: the axis in the middle, depth OwnLampNear over the distance - any axis, upright too");
    Check(bEdge, "OwnLampMapTan off the axis lies on the map's edge");
    Check(bBehind, "and what lies behind the lamp behind its near plane");

    // What may reach into a lamp's light.
    const FVec3 Ax = { 1.0f, 0.0f, 0.0f };
    const float Ca = 1.0f / Sqrt(1.0f + 1.4f * 1.4f), Sa = 1.4f * Ca;
    const FVec3 Ahead = { 600.0f, -50.0f, 30.0f }, Aside = { 100.0f, 900.0f, 30.0f }, Behind = { -400.0f, -50.0f, 30.0f },
                Far = { 100.0f + 5000.0f, -50.0f, 30.0f }, Round = { 120.0f, -50.0f, 30.0f };
    Check(SphereTouchesSpot(At, Ax, Ca, Sa, OwnLampReach, Ahead, 10.0f), "a sphere ahead on the axis is in it");
    Check(!SphereTouchesSpot(At, Ax, Ca, Sa, OwnLampReach, Aside, 10.0f), "one square to the axis, far out, is not");
    Check(!SphereTouchesSpot(At, Ax, Ca, Sa, OwnLampReach, Behind, 10.0f), "nor one behind the lamp");
    Check(!SphereTouchesSpot(At, Ax, Ca, Sa, OwnLampReach, Far, 10.0f), "nor one past its reach");
    Check(SphereTouchesSpot(At, Ax, Ca, Sa, OwnLampReach, Round, 30.0f), "but one round the lamp itself is");

    // The rows the passes read, where SolidPass.hlsl's OwnRow and the walk's OwnBeams look for each number.
    {
        float       Rows[4 * (RDX_OWN_HEAD_ROWS + 2 * RDX_OWN_LAMP_ROWS)] = {};
        const float Hue[3] = { 0.5f, 0.6f, 0.7f };
        float       Map[16];
        for (int i = 0; i < 16; i++)
            Map[i] = (float)(i + 1);
        OwnLampRowsHead(Rows, 2);
        OwnLampRowsLamp(Rows, 0, At, Aims[0], Hue, true, Map);
        OwnLampRowsLamp(Rows, 1, At, Aims[1], Hue, false, Map);
        const float* const L1 = Rows + 4 * (RDX_OWN_HEAD_ROWS + RDX_OWN_LAMP_ROWS);
        Check(Rows[0] == 2.0f && Rows[2] == OwnLampNear && Rows[4] == BeamRimCos && Rows[12] == OwnLampReach &&
              Rows[4 * RDX_OWN_HEAD_ROWS + 11] == 1.0f && L1[11] == 0.0f && L1[7] == 1.0f && L1[6] == -1.0f &&
              L1[10] == 0.7f && L1[12] == 1.0f && L1[27] == 16.0f,
              "the rows: the head's count, near and rims; each lamp's axis, slice, colour, the eye's mark and its map");
    }

    // The stretch of a ray in a lamp's cone: the air the walk lights for the lamp's beam.
    {
        const FVec3 X1  = { 1.0f, 0.0f, 0.0f };
        const float C30 = 0.8660254f;
        float       Z0 = 0.0f, Z1 = 0.0f;
        const FVec3 Back0 = { -100.0f, 0.0f, 0.0f }, Along = { 1.0f, 0.0f, 0.0f }, Away = { -1.0f, 0.0f, 0.0f };
        Check(OwnLampSpan(Back0, Along, X1, C30, 1000.0f, 0.0f, 5000.0f, Z0, Z1) && fabsf(Z0 - 100.0f) < 0.05f &&
              fabsf(Z1 - 1100.0f) < 0.05f, "a ray along the axis from behind the lamp: from the lamp to its reach");
        Check(!OwnLampSpan(Back0, Away, X1, C30, 1000.0f, 0.0f, 5000.0f, Z0, Z1),
              "one going off behind the lamp: none, though the cone's equation holds there as well");
        const FVec3 Side = { 500.0f, -1000.0f, 0.0f }, Across = { 0.0f, 2.0f, 0.0f };
        Check(OwnLampSpan(Side, Across, X1, C30, 1000.0f, 0.0f, 5000.0f, Z0, Z1) &&
              fabsf(Z0 - 0.5f * (1000.0f - 288.675f)) < 0.05f && fabsf(Z1 - 0.5f * (1000.0f + 288.675f)) < 0.05f,
              "one across it: between its sides, by a step of any length");
        const FVec3 Over = { 500.0f, -1000.0f, 400.0f };
        Check(!OwnLampSpan(Over, Across, X1, C30, 1000.0f, 0.0f, 5000.0f, Z0, Z1), "one past it: none");
        const FVec3 Inside = { 200.0f, 0.0f, 0.0f };
        Check(OwnLampSpan(Inside, Across, X1, C30, 1000.0f, 0.0f, 25.0f, Z0, Z1) && Z0 == 0.0f && Z1 == 25.0f,
              "one begun and ended inside it: the whole of it");
        const FVec3 Beside = { 500.0f, 100.0f, 0.0f };
        Check(OwnLampSpan(Beside, Away, X1, C30, 1000.0f, 0.0f, 5000.0f, Z0, Z1) && Z0 == 0.0f &&
              fabsf(Z1 - (500.0f - 173.205f)) < 0.05f, "one from inside it towards the lamp, beside the axis: out at "
              "its side");
        // Rays of every kind, held against the places along them asked one by one.
        unsigned   Seed = 12345u;
        const auto Rand = [&]() { Seed = Seed * 1664525u + 1013904223u; return (float)(Seed >> 8) / 16777216.0f; };
        int Bad = 0, Held = 0;
        for (int r = 0; r < 400; r++)
        {
            FVec3       Axis = { Rand() * 2.0f - 1.0f, Rand() * 2.0f - 1.0f, Rand() * 2.0f - 1.0f };
            const float Al   = Sqrt(Dot(Axis, Axis));
            Axis = (Al > 0.1f) ? FVec3{ Axis.X / Al, Axis.Y / Al, Axis.Z / Al } : FVec3{ 0.0f, 0.0f, 1.0f };
            const FVec3 From  = { Rand() * 4000.0f - 2000.0f, Rand() * 4000.0f - 2000.0f, Rand() * 4000.0f - 2000.0f };
            const float Cos   = 0.75f + 0.24f * Rand();
            const float Reach = 200.0f + 2800.0f * Rand();
            FVec3       Ray   = { Rand() * 2.0f - 1.0f, Rand() * 2.0f - 1.0f, Rand() * 2.0f - 1.0f };
            // Every other ray towards a place in or about the cone, so that most of those meet it.
            if (r % 2 == 1)
            {
                const float Out = 50.0f + (Reach - 50.0f) * Rand();
                Ray = { Axis.X * Out + Ray.X * 0.4f * Out - From.X, Axis.Y * Out + Ray.Y * 0.4f * Out - From.Y,
                        Axis.Z * Out + Ray.Z * 0.4f * Out - From.Z };
            }
            const float Vl = Sqrt(Dot(Ray, Ray));
            const float Vs = (0.5f + 2.5f * Rand()) / ((Vl > 0.1f) ? Vl : 1.0f);
            Ray = (Vl > 0.1f) ? FVec3{ Ray.X * Vs, Ray.Y * Vs, Ray.Z * Vs } : FVec3{ Vs, 0.0f, 0.0f };
            const float Za    = 100.0f * Rand();
            const float Zb    = Za + 10.0f + 4000.0f * Rand();
            float       S0 = 0.0f, S1 = 0.0f;
            const bool  bAny = OwnLampSpan(From, Ray, Axis, Cos, Reach, Za, Zb, S0, S1);
            Held += bAny ? 1 : 0;
            for (int k = 0; k <= 2000; k++)
            {
                const float Z  = Za + (Zb - Za) * (float)k / 2000.0f;
                const FVec3 P  = { From.X + Ray.X * Z, From.Y + Ray.Y * Z, From.Z + Ray.Z * Z };
                const float Pl = Sqrt(Dot(P, P));
                if (Pl < 20.0f)
                    continue;
                const float Pc    = Dot(P, Axis) / Pl;
                const bool  bIn   = Pl < Reach - 1.0f && Pc > Cos + 2e-3f;
                const bool  bOut  = Pl > Reach + 1.0f || Pc < Cos - 2e-3f;
                const bool  bSpan = bAny && Z >= S0 && Z <= S1;
                if (((bIn && !bSpan) || (bOut && bSpan)) && Bad++ == 0)
                    printf("  the first ray it fails: %i, at %.3f (in %i, out %i), the stretch %s %.3f to %.3f, the "
                           "place %.1f from the lamp at a cosine of %.5f, the cone's %.5f\n", r, Z, bIn ? 1 : 0,
                           bOut ? 1 : 0, bAny ? "from" : "none,", S0, S1, Pl, Pc, Cos);
            }
        }
        Check(Bad == 0 && Held > 100, "rays of every kind: what the stretch holds is in the cone and within reach, what "
              "it leaves is not");
    }

    // A mirror's view in the level: a place's image behind the mirror, rebuilt as the world's view's numbers have it,
    // comes back as the place itself.
    {
        const FVec3 O = { 300.0f, 200.0f, 64.0f };
        const float Cy = 0.8f, Sy = 0.6f;                        // the view turned in the level, and tipped down
        const FVec3 Zv = { Cy * 0.9f, Sy * 0.9f, -0.43589f };
        const float Zl = Sqrt(Dot(Zv, Zv));
        const FVec3 Z  = { Zv.X / Zl, Zv.Y / Zl, Zv.Z / Zl };
        const FVec3 Xv = { -Z.Y, Z.X, 0.0f };
        const float Xl = Sqrt(Dot(Xv, Xv));
        const FVec3 X  = { Xv.X / Xl, Xv.Y / Xl, 0.0f };
        const FVec3 Y  = { Z.Y * X.Z - Z.Z * X.Y, Z.Z * X.X - Z.X * X.Z, Z.X * X.Y - Z.Y * X.X };
        // The mirror ahead, a little turned: its plane in the view's space, the normal away from the eye.
        const FVec3 Nv = { 0.2f, -0.1f, 0.97468f };
        const float Nl = Sqrt(Dot(Nv, Nv));
        const float Plane[4] = { Nv.X / Nl, Nv.Y / Nl, Nv.Z / Nl, 180.0f };
        FVec3 Om, Xm, Ym, Zm;
        MirrorViewCoords(O, X, Y, Z, Plane, Om, Xm, Ym, Zm);
        // A place in front of the mirror, in the view's space; its image behind the plane; the image rebuilt into the
        // level with the mirror's view's place, and the place itself into the level with the world's view's.
        const FVec3 Pv = { 40.0f, -25.0f, 90.0f };
        const float Side = Plane[0] * Pv.X + Plane[1] * Pv.Y + Plane[2] * Pv.Z - Plane[3];
        const FVec3 Iv = { Pv.X - 2.0f * Side * Plane[0], Pv.Y - 2.0f * Side * Plane[1], Pv.Z - 2.0f * Side * Plane[2] };
        const FVec3 Got  = { Om.X + Iv.X * Xm.X + Iv.Y * Ym.X + Iv.Z * Zm.X, Om.Y + Iv.X * Xm.Y + Iv.Y * Ym.Y + Iv.Z * Zm.Y,
                             Om.Z + Iv.X * Xm.Z + Iv.Y * Ym.Z + Iv.Z * Zm.Z };
        const FVec3 Want = { O.X + Pv.X * X.X + Pv.Y * Y.X + Pv.Z * Z.X, O.Y + Pv.X * X.Y + Pv.Y * Y.Y + Pv.Z * Z.Y,
                             O.Z + Pv.X * X.Z + Pv.Y * Y.Z + Pv.Z * Z.Z };
        Check(fabsf(Got.X - Want.X) < 1e-2f && fabsf(Got.Y - Want.Y) < 1e-2f && fabsf(Got.Z - Want.Z) < 1e-2f,
              "a mirror's view in the level: a place's image rebuilt there is the place itself");
        Check(fabsf(Dot(Xm, Xm) - 1.0f) < 1e-4f && fabsf(Dot(Xm, Ym)) < 1e-4f && fabsf(Dot(Ym, Zm)) < 1e-4f,
              "its axes stay square and of unit length");
    }

    // Who holds the gun up.
    const char* const Up[7]   = { "Shoot", "Shoot2H", "CrouchShoot", "RunShoot2H", "TreadShoot", "Strafe2H", "Strafe" };
    const char* const Down[8] = { "BreatheLight2H", "Walk2H", "Reload", "ReloadEnd", "Attack", "HandToHandAttack", "Run",
                                  "" };
    bool bUp = true, bDown = true;
    for (int i = 0; i < 7; i++)
        bUp = bUp && WeaponLampAims(Up[i]);
    for (int i = 0; i < 8; i++)
        bDown = bDown && !WeaponLampAims(Down[i]);
    Check(bUp, "the shooting and strafing animations hold the gun up");
    Check(bDown && !WeaponLampAims((const char*)nullptr),
          "the gun across the chest, a reload, a blow by hand, running and nothing do not");
}

static void TestPickEdgeShare()
{
    printf("picks: the edge of a pick, eased\n");
    Check(PickEdgeShare(0.5f, 0.0f, 0.25f) == 1.0f, "nothing left out: all of it");
    Check(PickEdgeShare(0.5f, 0.5f, 0.25f) == 0.0f, "level with the strongest left out: nothing");
    Check(PickEdgeShare(0.4f, 0.5f, 0.25f) == 0.0f, "weaker than it: nothing");
    CheckNear(PickEdgeShare(0.5f, 0.45f, 0.25f), 0.4f, 1e-5f, "a tenth ahead where a quarter is asked: two fifths");
    CheckNear(PickEdgeShare(0.8f, 0.6f, 0.25f), 1.0f, 1e-5f, "a quarter ahead: all of it");
    Check(PickEdgeShare(0.9f, 0.6f, 0.25f) == 1.0f && PickEdgeShare(1.0f, 0.1f, 0.25f) == 1.0f,
          "further ahead than that: all of it, and no more");
    Check(PickEdgeShare(0.5f, 0.4f, 0.0f) == 1.0f && PickEdgeShare(0.4f, 0.5f, 0.0f) == 0.0f,
          "no band to ease over: all or nothing");
    bool  bRises = true;
    float Last   = 0.0f;
    for (float Own = 0.5f; Own < 0.8f; Own += 0.01f)
    {
        const float S = PickEdgeShare(Own, 0.5f, 0.25f);
        if (S < Last || S < 0.0f || S > 1.0f)
            bRises = false;
        Last = S;
    }
    Check(bRises && Last == 1.0f, "from level to well ahead it only rises, from nothing to all");
}

static void TestShaftLampPick()
{
    printf("light shafts: the lamps they come from\n");
    int   Order[8];
    float Share[8];

    // Fewer than there are places: all of them, the strongest first, each with all of its light.
    const float Few[3] = { 0.2f, 0.9f, 0.5f };
    Check(ShaftLampPick(Few, 3, 8, Order, Share) == 3 && Order[0] == 1 && Order[1] == 2 && Order[2] == 0,
          "fewer than the places: every one, the strongest first");
    Check(Share[0] == 1.0f && Share[1] == 1.0f && Share[2] == 1.0f, "and each with all of its light");
    Check(ShaftLampPick(Few, 0, 8, Order, Share) == 0, "none to pick from: none picked");

    // More than the places: the strongest, and the edge of the pick eased.
    const float Many[6] = { 1.0f, 0.10f, 0.80f, 0.42f, 0.40f, 0.05f };
    Check(ShaftLampPick(Many, 6, 3, Order, Share) == 3 && Order[0] == 0 && Order[1] == 2 && Order[2] == 3,
          "more than the places: the strongest three");
    Check(Share[0] == 1.0f && Share[1] == 1.0f, "those well ahead of the first left out: all of their light");
    CheckNear(Share[2], (0.42f - 0.40f) / (ShaftLampFade * 0.42f), 1e-5f,
              "the one just ahead of it: a share by how far ahead");

    // Two lamps changing places at the pick's edge are both at nothing as they pass.
    const float Tie[4] = { 1.0f, 0.5f, 0.3f, 0.3f };
    Check(ShaftLampPick(Tie, 4, 3, Order, Share) == 3 && Share[2] == 0.0f, "level with the first left out: nothing");
    float Before[4] = { 1.0f, 0.5f, 0.3001f, 0.3f }, After[4] = { 1.0f, 0.5f, 0.3f, 0.3001f };
    float ShareB[8], ShareA[8];
    int   OrderB[8], OrderA[8];
    ShaftLampPick(Before, 4, 3, OrderB, ShareB);
    ShaftLampPick(After, 4, 3, OrderA, ShareA);
    Check(OrderB[2] == 2 && OrderA[2] == 3 && ShareB[2] < 0.01f && ShareA[2] < 0.01f,
          "either side of the change the one at the edge is all but out");

    // Whatever the order they come in, the same ones are picked.
    const float Up[5] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f }, Down[5] = { 0.5f, 0.4f, 0.3f, 0.2f, 0.1f };
    int OrderU[8], OrderD[8];
    Check(ShaftLampPick(Up, 5, 2, OrderU, Share) == 2 && OrderU[0] == 4 && OrderU[1] == 3 &&
          ShaftLampPick(Down, 5, 2, OrderD, Share) == 2 && OrderD[0] == 0 && OrderD[1] == 1,
          "rising or falling, the two strongest");
    CheckNear(Share[1], 1.0f, 1e-6f, "a quarter ahead of the first left out and more: all of its light");
}

static void TestShadowCanReachView()
{
    printf("shadows: can a cast shadow reach the view at all (filtering the draws)\n");

    // Our own square root - RenderMath.h pulls in no library - needed for unit vectors at segment centres.
    CheckNear(Sqrt(4.0f), 2.0f, 1e-6f, "sqrt(4) = 2");
    CheckNear(Sqrt(2.0f), 1.41421356f, 1e-6f, "sqrt(2)");
    CheckNear(Sqrt(1.0e6f), 1000.0f, 1e-3f, "sqrt(1 million) = 1000");
    Check(Sqrt(0.0f) == 0.0f && Sqrt(-1.0f) == 0.0f, "zero and negative: zero, not NaN");

    const float TX = 1.0f, TY = 1.0f;   // a 90 degree opening in both directions
    const FVec3 InFront   = {   0.0f, 0.0f,  100.0f };
    const FVec3 Behind = {   0.0f, 0.0f, -100.0f };

    // The lamp is behind us and casts the shadow forward, into view: a filter on the caster being in view
    // would throw this away.
    Check(ShadowCanReachView(Behind, InFront, 10.0f, 1000.0f, TX, TY, 4) == true,
          "a shadow cast forward by a lamp behind us is visible");

    // The other way round: light in front, caster behind - the shadow falls further back still, out of view.
    Check(ShadowCanReachView(InFront, Behind, 10.0f, 1000.0f, TX, TY, 4) == false,
          "a shadow cast backwards, away from us, is not");

    // A caster outside the light's range casts nothing from that light at all.
    const FVec3 FarAway = { 0.0f, 0.0f, 5000.0f };
    Check(ShadowCanReachView(Behind, FarAway, 10.0f, 1000.0f, TX, TY, 4) == false,
          "beyond the range of the light there is nothing to cast");

    // Degenerate case: the caster stands on the light, so there is no direction to judge and it is kept.
    Check(ShadowCanReachView(InFront, InFront, 10.0f, 1000.0f, TX, TY, 4) == true,
          "a caster standing on the light is not excluded");

    // Conservative by construction: what one segment would miss, more segments find. The case is a shadow that
    // starts to the side and crosses the view.
    // Light behind and to the right, caster ahead and to the right: the shadow runs forward-left, across the view.
    const FVec3 RightBehind = {  400.0f, 0.0f, -50.0f };
    const FVec3 RightFront  = {  300.0f, 0.0f,  50.0f };
    const bool Seg1   = ShadowCanReachView(RightBehind, RightFront, 5.0f, 2000.0f, TX, TY, 1);
    const bool Seg8 = ShadowCanReachView(RightBehind, RightFront, 5.0f, 2000.0f, TX, TY, 8);
    Check(Seg8 == true, "with more segments a shadow crossing the view is found");
    Check(!(Seg1 == false && Seg8 == false), "raising the segment count cannot lose a shadow");

    // Clamping the segment count: both 0 and 100 stay valid calls.
    Check(ShadowCanReachView(Behind, InFront, 10.0f, 1000.0f, TX, TY, 0) == true, "0 segments: clamped to one, it still works");
    Check(ShadowCanReachView(Behind, InFront, 10.0f, 1000.0f, TX, TY, 100) == true, "100 segments: clamped to eight, it still works");
}

static void TestWorldMapRedraws()
{
    printf("shadows: the state of the level maps and the order they are drawn in\n");

    const FVec3 At = { 100.0f, 200.0f, 300.0f };
    FVec3 Now = At;
    Check(ClassifyWorldMap(false, At, 500.0f, Now, 500.0f) == WorldMap_Missing, "no map: missing");
    Check(ClassifyWorldMap(true, At, 500.0f, Now, 500.0f) == WorldMap_Fresh, "from the same place: fresh");
    Now.X = At.X + 0.9f;
    Check(ClassifyWorldMap(true, At, 500.0f, Now, 500.0f) == WorldMap_Fresh, "within one unit: fresh");
    Now.X = At.X + 5.0f;
    Check(ClassifyWorldMap(true, At, 500.0f, Now, 500.0f) == WorldMap_Stale, "moved 5 units: stale, but usable");
    Now.X = At.X;
    Now.Z = At.Z - 20.0f;
    Check(ClassifyWorldMap(true, At, 500.0f, Now, 500.0f) == WorldMap_Unusable, "moved 20 units: unusable");
    Now = At;
    Check(ClassifyWorldMap(true, At, 500.0f, Now, 520.0f) == WorldMap_Unusable, "its radius grew by 20: unusable");
    Check(ClassifyWorldMap(true, At, 500.0f, Now, 480.0f) == WorldMap_Stale,
          "its radius shrank by 20: stale (the map holds more than it needs)");

    const EWorldMapState S[6] = { WorldMap_Stale, WorldMap_Missing, WorldMap_Unusable, WorldMap_Unusable, WorldMap_Fresh,
                                  WorldMap_Missing };
    const int Age[6] = { 5, 0, 2, 9, 1, 0 };
    int Out[6] = { -1, -1, -1, -1, -1, -1 };
    int N = WorldRedrawOrder(S, Age, 6, 3, Out);
    Check(N == 3 && Out[0] == 1 && Out[1] == 5 && Out[2] == 3,
          "missing ones first, in selection order, then the oldest unusable one");
    N = WorldRedrawOrder(S, Age, 6, 6, Out);
    Check(N == 5 && Out[0] == 1 && Out[1] == 5 && Out[2] == 3 && Out[3] == 2 && Out[4] == 0,
          "everything that is not fresh: missing, unusable, stale - never a fresh one");
    N = WorldRedrawOrder(S, Age, 6, 0, Out);
    Check(N == 0, "with no budget, nothing");
    // Two lights moving every frame ahead of a new one in the pick: the new one's map is drawn all the same.
    const EWorldMapState M[3] = { WorldMap_Stale, WorldMap_Stale, WorldMap_Missing };
    const int MAge[3] = { 1, 1, 0 };
    N = WorldRedrawOrder(M, MAge, 3, 2, Out);
    Check(N == 2 && Out[0] == 2 && Out[1] == 0, "with two moving lights the new light still gets its map");
}

static void TestShadowCompensation()
{
    printf("the two stages of compensation: the lights first, then easing the shadows\n");

    // Nothing to compensate for: no measurement (-1), no darkening (1.0), and shadows switched off (zero strength).
    FShadowCompensation R = CompensateShadows(-1.0f, 1.0f);
    Check(R.Boost == 1.0f && R.Strength == 1.0f, "with no measurement nothing happens");
    R = CompensateShadows(1.0f, 1.0f);
    Check(R.Boost == 1.0f && R.Strength == 1.0f, "with no darkening nothing happens");
    R = CompensateShadows(0.5f, 0.0f);
    Check(R.Boost == 1.0f && R.Strength == 1.0f, "a sample measured with the shadows off is not compensated for");

    // A 5% loss: the lights alone give it back (1/0.95), the shadows are untouched.
    R = CompensateShadows(0.95f, 1.0f);
    CheckNear(R.Boost, 1.0526f, 1e-3f, "a small loss takes a small lift");
    CheckNear(R.Strength, 1.0f, 1e-4f, "and the shadows stay at full strength");

    // A 20% loss: 1/0.80 = 1.25 times the light, still only the first stage.
    R = CompensateShadows(0.80f, 1.0f);
    CheckNear(R.Boost, 1.25f, 1e-4f, "a 20% loss takes 1.25 times the light");
    CheckNear(R.Strength, 1.0f, 1e-4f, "at 20% the shadows are untouched");

    // The turning point: at a third of the light lost, the lift reaches its own ceiling. Below that the second
    // stage never starts.
    R = CompensateShadows(1.0f - 1.0f / 3.0f, 1.0f);
    CheckNear(R.Boost, 1.5f, 1e-3f, "at a third lost the lift reaches its ceiling");
    CheckNear(R.Strength, 1.0f, 1e-3f, "and even here the shadows are untouched");

    // Past that point the lift is spent and the shadows begin to ease, a little at first.
    R = CompensateShadows(0.652f, 1.0f);
    CheckNear(R.Boost, 1.5f, 1e-4f, "the lift stops at its ceiling and goes no further");
    CheckNear(R.Strength, 0.9578f, 1e-3f, "what is left is taken by easing the shadows");

    // A 45% loss.
    R = CompensateShadows(0.55f, 1.0f);
    CheckNear(R.Strength, 0.7407f, 1e-3f, "a larger loss eases them further");

    // A 70% loss: both limits bind and some darkening is left over. That is the end of what can be done.
    R = CompensateShadows(0.30f, 1.0f);
    CheckNear(R.Boost, 1.5f, 1e-4f, "the lift is at its ceiling");
    CheckNear(R.Strength, ShadowStrengthFloor, 1e-4f, "the shadows stop at their floor, they do not vanish");
    Check(ShadowStrengthFloor >= 0.5f, "the floor never goes below 50%, so shadows remain visible");

    // A darkening of 41% measured at 71% strength settles at 58% without reaching the floor, and the darkening
    // is fully gone there.
    R = CompensateShadows(1.0f - 0.41f, 0.71f);
    CheckNear(R.Strength, 0.5772f, 1e-3f, "that scene settles at 58% shadow strength");
    CheckNear(CompensatedDarkening(1.0f - (0.41f / 0.71f) * R.Strength, R.Boost), 0.0f, 5e-3f,
              "and the compensated darkening goes to zero there");

    // The controller must not chase its own output. The same scene, measured with the shadows already eased,
    // has to ask for the same answer - otherwise easing improves the metric, which eases less, which worsens it,
    // and the loop walks in circles.
    const FShadowCompensation Full = CompensateShadows(0.55f, 1.0f);
    const FShadowCompensation Part = CompensateShadows(1.0f - 0.45f * 0.7f, 0.7f);
    CheckNear(Part.Boost, Full.Boost, 1e-3f, "measured with weaker shadows it asks for the same light");
    CheckNear(Part.Strength, Full.Strength, 1e-3f, "measured with weaker shadows it asks for the same strength");

    // The fixed point: measuring what it has just set gives the same answer, so the controller settles instead
    // of drifting.
    const FShadowCompensation A = CompensateShadows(0.30f, 1.0f);
    const FShadowCompensation B = CompensateShadows(1.0f - 0.70f * A.Strength, A.Strength);
    CheckNear(B.Strength, A.Strength, 1e-4f, "the settled state holds itself");
    CheckNear(B.Boost, A.Boost, 1e-4f, "the lift stays where it is too");

    // The order of the two stages, checked across the whole range: below the lift's ceiling the shadows are
    // never touched.
    for (int i = 1; i <= 99; i++)
    {
        const FShadowCompensation S = CompensateShadows((float)i / 100.0f, 1.0f);
        Check(S.Boost >= 1.0f && S.Boost <= 1.0f + ShadowBoostMax, "the lift stays within its own limit");
        Check(S.Strength >= ShadowStrengthFloor && S.Strength <= 1.0f, "the strength stays within its own limit");
        if (S.Strength < 1.0f)
            CheckNear(S.Boost, 1.0f + ShadowBoostMax, 1e-4f, "the shadows are only touched once the lift is at its ceiling");
    }

    // The second number on the readout: what is LEFT of the darkening after the lights have given back what
    // they could.
    Check(CompensatedDarkening(-1.0f, 1.0f) == 0.0f, "zero without a measurement");
    CheckNear(CompensatedDarkening(0.8f, 1.0f), 0.20f, 1e-4f, "with no lift it is the raw darkening");
    CheckNear(CompensatedDarkening(0.652f, 1.3497f), 0.12f, 1e-3f, "what remains of the metric once the lift is applied");
    Check(CompensatedDarkening(0.5f, 2.5f) == 0.0f, "an overshoot gives zero, never a negative");
    // With both limits binding - the largest loss, the lowest strength, the highest lift - a few per cent of
    // darkening survives, and nothing further can be done about it.
    CheckNear(CompensatedDarkening(1.0f - 0.70f * ShadowStrengthFloor, 1.0f + ShadowBoostMax), 0.025f, 1e-3f,
              "even in the worst case only a few per cent remains, with both limits binding");

    // The slow follower moves half the remaining distance each step and stops on the target; both stages use it.
    CheckNear(FollowSlowly(1.0f, 2.0f, 0.5f), 1.5f, 1e-4f, "the follower covers half the gap");
    CheckNear(FollowSlowly(1.5f, 1.5f, 0.5f), 1.5f, 1e-4f, "it stays on the target");
    CheckNear(FollowSlowly(1.0f, 2.0f, 0.0f), 1.0f, 1e-4f, "a zero step does not move it");
    CheckNear(FollowSlowly(1.0f, 0.6f, 0.5f), 0.8f, 1e-4f, "downwards it also covers half the gap");
}

static void TestUpscaling()
{
    printf("upscaling: the modes, the scene size\n");

    Check(UpscaledSceneSize(3840, Upscaling_Quality) == 2560 && UpscaledSceneSize(2160, Upscaling_Quality) == 1440,
          "Quality: 1.5x - 3840x2160 becomes 2560x1440");
    Check(UpscaledSceneSize(3840, Upscaling_Balanced) == 2259 && UpscaledSceneSize(2160, Upscaling_Balanced) == 1271,
          "Balanced: 1.7x, rounded to whole pixels");
    Check(UpscaledSceneSize(3840, Upscaling_Performance) == 1920 && UpscaledSceneSize(2160, Upscaling_Performance) == 1080,
          "Performance: 2x");
    Check(UpscaledSceneSize(3840, Upscaling_Off) == 3840 && UpscaledSceneSize(1, Upscaling_Performance) == 1,
          "Off: the output size; never less than one pixel");

    bool bKnown = false;
    Check(ParseUpscale("NativeAA", bKnown) == Upscaling_NativeAA && bKnown, "Upscale: NativeAA");
    Check(ParseUpscale(" quality ", bKnown) == Upscaling_Quality && bKnown, "quality, with spaces around it");
    Check(ParseUpscale(L"BALANCED", bKnown) == Upscaling_Balanced && bKnown, "BALANCED, as wide characters");
    Check(ParseUpscale("Performance\r", bKnown) == Upscaling_Performance && bKnown,
          "Performance, a carriage return after it");
    Check(ParseUpscale("Off", bKnown) == Upscaling_NativeAA && !bKnown,
          "Off is not a size: NativeAA, and bKnown is false");
    Check(ParseUpscale((const char*)0, bKnown) == Upscaling_NativeAA && !bKnown, "no text: NativeAA");
    {
        bool bRound = true;
        for (int u = (int)Upscaling_NativeAA; u <= (int)Upscaling_Performance; u++)
        {
            bool bOk = false;
            bRound = bRound && ParseUpscale(UpscaleName((EUpscaling)u), bOk) == (EUpscaling)u && bOk;
        }
        Check(bRound, "every size's name reads back the same");
    }
    Check(UpscaleStep(Upscaling_NativeAA, 1) == Upscaling_Quality &&
          UpscaleStep(Upscaling_Performance, 1) == Upscaling_NativeAA &&
          UpscaleStep(Upscaling_NativeAA, -1) == Upscaling_Performance && UpscaleStep(Upscaling_Off, 1) == Upscaling_Quality,
          "the menu's steps go round both ways, never onto Off");

    // NativeAA scales by 1.0: the scene stays at output size and only the temporal smoothing runs on it.
    Check(UpscaledSceneSize(3840, Upscaling_NativeAA) == 3840 && UpscaledSceneSize(2160, Upscaling_NativeAA) == 2160,
          "NativeAA: 1.0 - the scene is the size of the output");

    // The two settings compose: supersampling enlarges the image, and upscaling draws the scene smaller than
    // that. Neither cancels the other. What caps supersampling is the D3D12 texture size limit, not a number of
    // our own choosing.
    Check(MaxSceneScale(3840, 2160) == 4, "4K: at most 4x (15360x8640)");
    Check(MaxSceneScale(1920, 1080) == 4, "1080p: the texture limit would allow 8x, but the setting caps at 4");
    Check(MaxSceneScale(16384, 16384) == 1 && MaxSceneScale(0, 0) >= 1, "already at the limit: 1x; a zero size does not divide by zero");
    Check(AntialiasedSideFor(3840, 2) == 7680 && AntialiasedSideFor(3840, 1) == 3840,
          "the size of the smoothed image: output times SuperSample");
    Check(AntialiasedSideFor(3840, 0) == 3840, "SuperSample 0 is also 1x");
    Check(SceneSideFor(3840, 1, Upscaling_Off) == 3840 && SceneSideFor(3840, 2, Upscaling_Off) == 7680,
          "without upscaling the scene is the supersampled size");
    Check(SceneSideFor(3840, 2, Upscaling_NativeAA) == 7680,
          "NativeAA + 2x supersampling: the scene is 7680, and FSR smooths there too");
    Check(SceneSideFor(3840, 2, Upscaling_Quality) == 5120,
          "Quality + 2x: 7680 / 1.5 = 5120 - the two compose");
    Check(SceneSideFor(3840, 2, Upscaling_Performance) == 3840,
          "Performance + 2x: exactly the output size - allowed, but wasted work");

    // Our own settings file parser: the engine's ini is not ours.
    printf("settings file: our own parser\n");
    {
        static const char* const F =
            "; this is a comment\n"
            "# so is this\n"
            "[SecondRevision]\n"
            "Preset=Recommended\n"
            "  AntiAliasing  =  SSAA4X  \n"
            "Shadows=On\n"
            "FsrSharpness=0.75\n"
            "Empty=\n"
            "; Preset=Vanilla   <- inside a comment it does not count\n";
        char V[64];
        Check(SettingsFind(F, "Preset", V, 64) && strcmp(V, "Recommended") == 0, "key=value");
        Check(SettingsFind(F, "antialiasing", V, 64) && strcmp(V, "SSAA4X") == 0,
              "case does not matter, and spaces are trimmed");
        Check(SettingsFind(F, "Empty", V, 64) && V[0] == 0, "an empty value: found, but empty");
        Check(!SettingsFind(F, "NoSuchKey", V, 64), "a missing key: false");
        Check(!SettingsFind(F, "this", V, 64), "text inside a comment is not a key");
        Check(SettingsBool(F, "Shadows", false), "On -> true");
        Check(SettingsBool("Shadows=True\n", "Shadows", false) == false &&
                  SettingsBool("Shadows=off\n", "Shadows", true) == false,
              "a switch is On or Off, in any case: True is no word of it, and keeps the default");
        Check(SettingsBool(F, "NoSuchKey", true), "a missing key: the default stands");
        Check(SettingsFloat(F, "FsrSharpness", 0.5f) > 0.749f && SettingsFloat(F, "FsrSharpness", 0.5f) < 0.751f,
              "a decimal number");
        Check(SettingsFloat(F, "NoSuchKey", 0.5f) == 0.5f, "a missing number: the default");
        Check(SettingsFloat(F, "Preset", 1.25f) == 1.25f, "not a number: the default");
        // A comment is never a key, not even when it spells out the same name.
        static const char* const G = "; Preset=Vanilla\nPreset=Custom\n";
        Check(SettingsFind(G, "Preset", V, 64) && strcmp(V, "Custom") == 0, "the commented-out line is skipped");
    }

    // The preset is one word for the kind of picture wanted.
    printf("preset: Recommended / Vanilla / Custom\n");
    bool bP = false;
    Check(ParsePreset("Recommended", bP) == Preset_Recommended && bP, "Recommended");
    Check(ParsePreset(" vanilla ", bP) == Preset_Vanilla && bP, "vanilla, with spaces around it");
    Check(ParsePreset(L"CUSTOM", bP) == Preset_Custom && bP, "CUSTOM, as wide characters");
    Check(ParsePreset("ineveraskedforthis", bP) == Preset_INeverAskedForThis && bP, "INeverAskedForThis, any case");
    {
        // The menu's value column: a line of CaptionLineChars, less the marker and the label's fourteen.
        bool bFit = true, bFont = true;
        for (int i = 0; i < (int)Preset_Count; i++)
        {
            const char* C = PresetCaption((EPreset)i);
            bFit = bFit && 1 + 14 + (int)strlen(C) <= CaptionLineChars;
            for (int k = 0; C[k]; k++)
                bFont = bFont && ((C[k] >= 'A' && C[k] <= 'Z') || (C[k] >= '0' && C[k] <= '9') || C[k] == ' ');
        }
        Check(bFit, "every preset's menu name fits its menu line");
        Check(bFont, "and is written in letters the caption font has");
        Check(strcmp(PresetCaption(Preset_INeverAskedForThis), "I NEVER ASKED FOR THIS") == 0, "the whole line");
    }
    Check(ParsePreset("Whatever", bP) == Preset_Custom && !bP, "unknown: Custom, and bKnown is false");
    Check(ParsePreset((const char*)0, bP) == Preset_Custom && !bP,
          "no text: Custom - a preset nobody chose must not override a settings file somebody did");
    {
        bool bRoundTrips = true;
        for (int i = 0; i < (int)Preset_Count; i++)
        {
            char Out[32];
            bool bOk = false;
            PresetName((EPreset)i, Out, 32);
            if (ParsePreset(Out, bOk) != (EPreset)i || !bOk)
                bRoundTrips = false;
        }
        Check(bRoundTrips, "all three preset names read back the same");
    }

    // HDR and ten-bit output: one key, three values, and they are NOT the same thing.
    printf("HDR: the values of the HDR key, and the output maths\n");
    {
        bool bH = false;
        Check(ParseHdrMode("Off", bH) == Hdr_Off && bH, "Off");
        Check(ParseHdrMode(" 10bitsdr ", bH) == Hdr_Ten && bH, "10bitsdr, with spaces around it");
        Check(ParseHdrMode(L"ON", bH) == Hdr_On && bH, "ON, as wide characters");
        Check(ParseHdrMode("True", bH) == Hdr_Off && !bH, "True is no word of the key: unknown, and unknown is Off");
        Check(ParseHdrMode("10bit", bH) == Hdr_Off && !bH, "a near miss is still unknown, and unknown is Off");
        Check(ParseHdrMode((const char*)0, bH) == Hdr_Off && !bH, "no text: Off");
        bool bRoundTrips = true;
        for (int i = 0; i < (int)Hdr_Count; i++)
        {
            char Out[32];
            bool bOk = false;
            HdrName((EHdrMode)i, Out, 32);
            if (ParseHdrMode(Out, bOk) != (EHdrMode)i || !bOk)
                bRoundTrips = false;
        }
        Check(bRoundTrips, "all three HDR names read back the same");

        Check(HdrSceneBits(Hdr_Off) == 8 && HdrSceneBits(Hdr_Ten) == 10 && HdrSceneBits(Hdr_On) == 16,
              "the mode picks the WHOLE chain's precision, not only the swap chain's");

        // The compensation's ceiling is a format limit, not a taste: it exists because SDR stops at 1.0.
        Check(ShadowBoostCeiling(Hdr_Off) == ShadowBoostMax && ShadowBoostCeiling(Hdr_Ten) == ShadowBoostMax,
              "both SDR modes keep the measured ceiling - ten bits is the same range, finer");
        Check(ShadowBoostCeiling(Hdr_On) > ShadowBoostMax, "HDR may turn the lamps up further");
        Check(ShadowStrengthFloorFor(Hdr_On) == 1.0f && ShadowStrengthFloorFor(Hdr_Off) == ShadowStrengthFloor,
              "in HDR the shadows keep full strength: weakening them is a loss of the contrast HDR is for");

        // scRGB: 1.0 is 80 nits by definition.
        CheckNear(PaperWhiteScale(80.0f), 1.0f, 1e-6f, "80 nits is scRGB's unit");
        CheckNear(PaperWhiteScale(200.0f), 2.5f, 1e-6f, "200 nits: two and a half times the unit");
        CheckNear(PaperWhiteScale(0.0f), HdrPaperWhiteNits / ScRgbUnitNits, 1e-6f,
              "a nonsense value falls back to the default");

        // The shoulder. What matters most is the first line: below the knee it must change NOTHING.
        bool bIdentity = true;
        for (int i = 0; i <= 75; i++)
        {
            const float X = (float)i / 100.0f;
            if (HighlightShoulder(X, 0.75f, 2.0f) != X)
                bIdentity = false;
        }
        Check(bIdentity, "below the knee the shoulder is the identity, bit for bit - the diffuse picture is untouched");
        CheckNear(HighlightShoulder(0.75f, 0.75f, 2.0f), 0.75f, 1e-6f, "it meets the identity line at the knee");
        Check(HighlightShoulder(1.0f, 0.75f, 2.0f) > 0.75f && HighlightShoulder(1.0f, 0.75f, 2.0f) < 1.0f,
              "above the knee it bends, and bends downwards");
        Check(HighlightShoulder(4.0f, 0.75f, 2.0f) < 2.0f && HighlightShoulder(4.0f, 0.75f, 2.0f) > 1.85f,
              "it approaches the ceiling, so nothing clips");
        // Far out the curve is still below the ceiling, but by less than a float can hold - so the claim that
        // can be checked is the one that matters: it never goes OVER, however bright the pixel is.
        Check(HighlightShoulder(50.0f, 0.75f, 2.0f) <= 2.0f && HighlightShoulder(1.0e6f, 0.75f, 2.0f) <= 2.0f,
              "no brightness, however large, gets past the ceiling");
        {
            // Monotonic, and no crease at the knee: a step in must never be a step back out.
            bool bRises = true;
            float Prev  = -1.0f;
            for (int i = 0; i <= 400; i++)
            {
                const float V = HighlightShoulder((float)i / 100.0f, 0.75f, 2.0f);
                if (V < Prev)
                    bRises = false;
                Prev = V;
            }
            Check(bRises, "the shoulder never turns back on itself");
            const float D1 = (HighlightShoulder(0.75f, 0.75f, 2.0f) - HighlightShoulder(0.74f, 0.75f, 2.0f)) / 0.01f;
            const float D2 = (HighlightShoulder(0.76f, 0.75f, 2.0f) - HighlightShoulder(0.75f, 0.75f, 2.0f)) / 0.01f;
            Check(D1 > 0.9f && D1 < 1.1f && D2 > 0.9f && D2 < 1.1f,
                  "the slope is one on both sides of the knee, so the join cannot be seen");
        }
        Check(HighlightShoulder(3.0f, 2.0f, 1.0f) == 1.0f && HighlightShoulder(0.5f, 2.0f, 1.0f) == 0.5f,
              "a ceiling below the knee leaves nothing to bend into: the value is only kept under the ceiling");

        // The compensation under each output mode. The limits are what the mode changes, and nothing else.
        {
            // A scene the lamps alone cannot carry in SDR: at full strength the shadows take 45% of the light, so
            // 1/0.55 = 1.82x would be needed and SDR stops at 1.5x. Stage two then eases the shadows for the rest.
            const FShadowCompensation Sdr = CompensateShadows(0.55f, 1.0f, ShadowBoostCeiling(Hdr_Off),
                                                              ShadowStrengthFloorFor(Hdr_Off));
            CheckNear(Sdr.Boost, 1.5f, 1e-4f, "SDR: the lamps stop at their ceiling");
            Check(Sdr.Strength < 1.0f, "SDR: and the shadows are eased for what is left");

            const FShadowCompensation Hdr = CompensateShadows(0.55f, 1.0f, ShadowBoostCeiling(Hdr_On),
                                                              ShadowStrengthFloorFor(Hdr_On));
            CheckNear(Hdr.Boost, 1.0f / 0.55f, 1e-4f, "HDR: the lamps alone carry it - they have the room");
            CheckNear(Hdr.Strength, 1.0f, 1e-6f, "HDR: so the shadows keep every bit of their strength");

            // Even a scene the HDR ceiling cannot carry keeps full strength: easing the shadows is a loss of
            // contrast, and there is range above white to put the light in instead.
            const FShadowCompensation Dark = CompensateShadows(0.2f, 1.0f, ShadowBoostCeiling(Hdr_On),
                                                               ShadowStrengthFloorFor(Hdr_On));
            CheckNear(Dark.Boost, 1.0f + HdrBoostMax, 1e-4f, "HDR: the lamps go to their own ceiling");
            CheckNear(Dark.Strength, 1.0f, 1e-6f, "HDR: and still the shadows are not touched");

            // Ten bits is the same range, only finer - so it compensates exactly as Off does.
            const FShadowCompensation Ten = CompensateShadows(0.55f, 1.0f, ShadowBoostCeiling(Hdr_Ten),
                                                              ShadowStrengthFloorFor(Hdr_Ten));
            Check(Ten.Boost == Sdr.Boost && Ten.Strength == Sdr.Strength,
                  "ten bits compensates exactly as eight does");
        }

        // What is allowed past paper white. The first check is the one that matters: in the SDR modes this has to
        // be one for EVERY flag combination, or the picture Laci calibrated would change behind his back.
        {
            bool bSdrUntouched = true;
            for (unsigned int f = 0; f < 256u; f++)
            {
                const unsigned int Flags = (f & 1 ? RDX_PF_Unlit : 0u) | (f & 2 ? RDX_PF_Translucent : 0u) |
                                           (f & 4 ? RDX_PF_Modulated : 0u) | (f & 8 ? RDX_PF_Masked : 0u) |
                                           (f & 16 ? RDX_PF_FakeBackdrop : 0u) | (f & 32 ? RDX_PF_Invisible : 0u);
                if (EmissiveBoost(Hdr_Off, Flags) != 1.0f || EmissiveBoost(Hdr_Ten, Flags) != 1.0f)
                    bSdrUntouched = false;
            }
            Check(bSdrUntouched, "in Off and 10bitSDR nothing is boosted, whatever the surface is");

            Check(EmissiveBoost(Hdr_On, RDX_PF_Unlit) == HdrEmissiveBoost,
                  "HDR: a surface carrying its own brightness may go past paper white");
            Check(EmissiveBoost(Hdr_On, 0) == 1.0f, "HDR: an ordinary lit surface is left exactly where it was");
            Check(EmissiveBoost(Hdr_On, RDX_PF_FakeBackdrop) == 1.0f,
                  "HDR: the sky is a painted backdrop, not a light source");
            Check(EmissiveBoost(Hdr_On, RDX_PF_Unlit | RDX_PF_Translucent) == 1.0f,
                  "HDR: never a translucent draw - its blend subtracts what is behind when the source exceeds one");
            Check(EmissiveBoost(Hdr_On, RDX_PF_Unlit | RDX_PF_Modulated) == 1.0f,
                  "HDR: never a modulated draw either - it is a multiplier, not a colour");
            Check(EmissiveBoost(Hdr_On, RDX_PF_Unlit | RDX_PF_Masked) == HdrEmissiveBoost,
                  "HDR: masked is still opaque where it draws at all, so a lit sign with holes counts");
        }

        // Reading a wider scene target back on the CPU (FBackend::ReadScene - the engine's screenshots and the
        // snapshot beside every saved game). Worth testing here because that path cannot be reached from a
        // script: Revision binds no console key, so the only thing that calls it is the engine itself.
        {
            float R = 0.0f, G = 0.0f, B = 0.0f;
            UnpackTenBit(0u, R, G, B);
            Check(R == 0.0f && G == 0.0f && B == 0.0f, "ten bits: all zero");
            // Red in the lowest ten bits, then green, then blue - full scale is 1023.
            UnpackTenBit(1023u, R, G, B);
            Check(R == 1.0f && G == 0.0f && B == 0.0f, "ten bits: the low ten are red");
            UnpackTenBit(1023u << 10, R, G, B);
            Check(R == 0.0f && G == 1.0f && B == 0.0f, "ten bits: the next ten are green");
            UnpackTenBit(1023u << 20, R, G, B);
            Check(R == 0.0f && G == 0.0f && B == 1.0f, "ten bits: the next ten are blue");
            UnpackTenBit(0xFFFFFFFFu, R, G, B);
            Check(R == 1.0f && G == 1.0f && B == 1.0f, "ten bits: the top two are alpha, and are ignored");
            UnpackTenBit(512u | (256u << 10) | (128u << 20), R, G, B);
            CheckNear(R, 512.0f / 1023.0f, 1e-6f, "ten bits: a middling red");
            CheckNear(G, 256.0f / 1023.0f, 1e-6f, "ten bits: a middling green");
            CheckNear(B, 128.0f / 1023.0f, 1e-6f, "ten bits: a middling blue");

            // Half floats. The named bit patterns are the ones the standard fixes, so a wrong shift shows up as
            // a wrong number rather than as a plausible one.
            Check(HalfToFloat(0x0000) == 0.0f, "half: zero");
            Check(HalfToFloat(0x8000) == 0.0f, "half: minus zero is still zero");
            Check(HalfToFloat(0x3C00) == 1.0f, "half: one");
            Check(HalfToFloat(0xBC00) == -1.0f, "half: minus one");
            CheckNear(HalfToFloat(0x3800), 0.5f, 1e-6f, "half: a half");
            CheckNear(HalfToFloat(0x4000), 2.0f, 1e-6f, "half: two");
            CheckNear(HalfToFloat(0x4900), 10.0f, 1e-4f, "half: ten");
            CheckNear(HalfToFloat(0x7BFF), 65504.0f, 1.0f, "half: the largest finite one");
            Check(HalfToFloat(0x7C00) == 65504.0f,
                  "half: infinity comes back as the largest finite value - a screenshot has to hand back a number");
            CheckNear(HalfToFloat(0x0001), 5.96e-8f, 1e-9f, "half: the smallest subnormal");
            // Monotonic across the ordinary range: a wrong exponent would show as a step backwards.
            {
                bool bRises = true;
                float Prev = -1.0f;
                for (unsigned short h = 0x0000; h < 0x7C00; h = (unsigned short)(h + 0x40))
                {
                    const float V = HalfToFloat(h);
                    if (V < Prev)
                        bRises = false;
                    Prev = V;
                }
                Check(bRises, "half: the positive range never turns back on itself");
            }
        }

        // The false-colour bands the debug view paints.
        Check(HdrBandOf(0.5f) == HdrBand_Below && HdrBandOf(0.999f) == HdrBand_Below, "below paper white");
        Check(HdrBandOf(1.0f) == HdrBand_One && HdrBandOf(1.9f) == HdrBand_One, "one to two times");
        Check(HdrBandOf(2.0f) == HdrBand_Two && HdrBandOf(3.9f) == HdrBand_Two, "two to four times");
        Check(HdrBandOf(4.0f) == HdrBand_Four && HdrBandOf(100.0f) == HdrBand_Four, "above four times");
    }

    // Antialiasing has a single key: AntiAliasing=
    // The number is always samples per pixel; where a multiplier would mean nothing, there is no number.
    // Ambient occlusion: its key, the curve each neighbour contributes by, and the constants' layout that the two shaders
    // and the backend share.
    printf("ambient occlusion: the key, a neighbour's share, and the passes' constants\n");
    {
        bool bA = false;
        Check(ParseAmbientOcclusion("Off", bA) == Ao_Off && bA, "Off");
        Check(ParseAmbientOcclusion(" ssao ", bA) == Ao_Ssao && bA, "ssao, with spaces around it");
        Check(ParseAmbientOcclusion(L"SSAO", bA) == Ao_Ssao && bA, "SSAO, as wide characters");
        Check(ParseAmbientOcclusion("True", bA) == Ao_Off && !bA, "True is no word of the key: unknown, and unknown is Off");
        Check(ParseAmbientOcclusion("HBAO", bA) == Ao_Off && !bA, "a kind that does not exist is unknown, and unknown is Off");
        Check(ParseAmbientOcclusion(" rtao ", bA) == Ao_Rt && bA, "RTAO, in any case, with spaces around it");
        Check(ParseAmbientOcclusion((const char*)0, bA) == Ao_Off && !bA, "no text: Off");
        bool bRoundTrips = true;
        for (int i = 0; i < Ao_Count; i++)
        {
            char Out[32];
            bool bOk = false;
            AoName((EAmbientOcclusion)i, Out, 32);
            if (ParseAmbientOcclusion(Out, bOk) != (EAmbientOcclusion)i || !bOk)
                bRoundTrips = false;
        }
        Check(bRoundTrips, "every name reads back as itself");

        // The water's key, and how its waves follow the size of the water.
        Check(ParseWater("Off", bA) == Water_Off && bA, "water: Off");
        Check(ParseWater(" low ", bA) == Water_Low && bA, "water: low, with spaces around it");
        Check(ParseWater(L"High", bA) == Water_High && bA, "water: High, as wide characters");
        Check(ParseWater("True", bA) == Water_Off && !bA, "water: True is no word of the key - unknown, Off");
        Check(ParseWater("rt", bA) == Water_High && bA && ParseWater("MEDIUM", bA) == Water_Low && bA,
              "water: a file that says the mirror image by the water's level - RT is High, Medium is Low");
        Check(Water_Off < Water_Low && Water_Low < Water_High && Water_Count == 3,
              "water: three levels, each with what the one below it has");
        Check(!WaterHasCaustics(Water_Off) && !WaterHasCaustics(Water_Low) && WaterHasCaustics(Water_High),
              "water: the caustics at High");
        // The reflections' key, and what a file without it means by its water's level.
        Check(ParseReflections("Default", bA) == Reflections_Default && bA && ParseReflections(" high ", bA) ==
              Reflections_High && bA && ParseReflections(L"rthigh", bA) == Reflections_RtHigh && bA &&
              ParseReflections("RTMedium", bA) == Reflections_RtMedium && bA && ParseReflections(" rtlow", bA) ==
              Reflections_RtLow && bA && ParseReflections("low", bA) == Reflections_Low && bA &&
              ParseReflections(" Medium ", bA) == Reflections_Medium && bA,
              "reflections: Default, Low, Medium, High, RTHigh, RTMedium and RTLow, spaces and case aside");
        Check(ParseReflections("Off", bA) == Reflections_Default && bA && ParseReflections("True", bA) ==
              Reflections_Default && !bA,
              "reflections: Off is Default; True is no word of the key - unknown, Default");
        Check(ParseReflections("RT", bA) == Reflections_Default && !bA && ParseReflections("RTHalf", bA) ==
              Reflections_Default && !bA,
              "reflections: the old RT and RTHalf are no words any more - unknown, Default");
        Check(ParseAmbientOcclusion("RTAO", bA) == Ao_Rt && bA && ParseAmbientOcclusion("RT", bA) == Ao_Off && !bA,
              "ambient occlusion: RTAO; the old RT is no word any more - unknown, Off");
        Check(ParseReflections("Ultra", bA) == Reflections_Default && !bA &&
              ParseReflections((const char*)0, bA) == Reflections_Default && !bA,
              "reflections: what is not a level, and no text, are unknown - and Default");
        Check(!ReflectionsHasMirror(Reflections_Default) && !ReflectionsHasMirror(Reflections_Low) &&
              ReflectionsHasMirror(Reflections_Medium) && ReflectionsHasMirror(Reflections_High) &&
              ReflectionsHasMirror(Reflections_RtHigh) && ReflectionsHasMirror(Reflections_RtMedium) &&
              ReflectionsHasMirror(Reflections_RtLow),
              "reflections: a mirror image from Medium on");
        Check(!ReflectionsWetImage(Reflections_Low) && ReflectionsWetImage(Reflections_Medium) &&
              ReflectionsWetImage(Reflections_High) && ReflectionsWetImage(Reflections_RtLow) &&
              ReflectionsWetCoarse(Reflections_Medium) && !ReflectionsWetCoarse(Reflections_High) &&
              !ReflectionsWetCoarse(Reflections_RtLow),
              "reflections: the wet floors' image from Medium on, the coarse one at Medium alone");
        Check(WaterMirrorSwitch(Reflections_Default) == 0.0f && WaterMirrorSwitch(Reflections_Low) == 0.25f &&
              WaterMirrorSwitch(Reflections_Medium) == 0.75f && WaterMirrorSwitch(Reflections_High) == 1.0f &&
              WaterMirrorSwitch(Reflections_RtLow) == 1.0f && WaterMirrorSwitch(Reflections_RtHigh) == 1.0f,
              "reflections: the water's switch - none, the glints and the far image, the coarse mirror, the full one");
        Check(!ReflectionsGlassTrace(Reflections_Medium) && ReflectionsGlassTrace(Reflections_High) &&
              ReflectionsGlassTrace(Reflections_RtLow),
              "reflections: the glass's picture trace from High on, the probe alone at Medium");
        Check(ReflectionsGlassFar(Reflections_Low) && !ReflectionsGlassFar(Reflections_Default) &&
              !ReflectionsGlassFar(Reflections_Medium) && !ReflectionsGlassFar(Reflections_High) &&
              !ReflectionsGlassFar(Reflections_RtLow),
              "reflections: the glass's far image alone at Low");
        Check(!ReflectionsTraced(Reflections_High) && ReflectionsTraced(Reflections_RtHigh) &&
              ReflectionsTraced(Reflections_RtMedium) && ReflectionsTraced(Reflections_RtLow),
              "reflections: from rays at RTHigh, RTMedium and RTLow");
        Check(ReflectionsStep(Reflections_Default, 1) == Reflections_Low &&
              ReflectionsStep(Reflections_Low, 1) == Reflections_Medium &&
              ReflectionsStep(Reflections_Medium, 1) == Reflections_High && ReflectionsStep(Reflections_High, 1) ==
              Reflections_RtLow && ReflectionsStep(Reflections_RtLow, 1) == Reflections_RtMedium &&
              ReflectionsStep(Reflections_RtMedium, 1) == Reflections_RtHigh &&
              ReflectionsStep(Reflections_RtHigh, 1) == Reflections_Default &&
              ReflectionsStep(Reflections_Default, -1) == Reflections_RtHigh &&
              ReflectionsStep(Reflections_RtMedium, -1) == Reflections_RtLow,
              "reflections: the menu steps Default, Low, Medium, High, RTLow, RTMedium, RTHigh and round again, "
              "either way");
        {
            // The wet floors' list: a line a texture, its kind; remarks, empty lines, carriage returns and the case of
            // the letters aside; a line that says anything else is not read, and counted.
            const char Text[] = "; SecondRevisionWetSurfaces.txt - the floors that get wet\r\n"
                                "Paris.pa_sidewalk_a             stone\r\n"
                                "\r\n"
                                "  Paris.pa_street_a\tSTONE ; the street\r\n"
                                "CoreTexMetal.GripMetlFloor_A    metal\n"
                                "CoreTexWood.Floor_A paper\n"
                                "Paris.pa_brick_a stone again\n"
                                "OnlyAName\n"
                                "CoreTexConcrete.RoadBlacktop_B  Stone";
            FWetSurface List[8];
            int         Bad = -1;
            const int   N   = ParseWetSurfaces(Text, (int)sizeof(Text) - 1, List, 8, Bad);
            Check(N == 4 && Bad == 3, "wet floors: four lines read, three that say something else not read");
            Check(N == 4 && strcmp(List[0].Name, "Paris.pa_sidewalk_a") == 0 && List[0].Kind == Wet_Stone &&
                      strcmp(List[1].Name, "Paris.pa_street_a") == 0 && List[1].Kind == Wet_Stone &&
                      strcmp(List[2].Name, "CoreTexMetal.GripMetlFloor_A") == 0 && List[2].Kind == Wet_Metal &&
                      strcmp(List[3].Name, "CoreTexConcrete.RoadBlacktop_B") == 0 && List[3].Kind == Wet_Stone,
                  "wet floors: the names as written, the kinds whatever their case, the last line without its end");
            Check(WetKindOf(List, N, L"paris.PA_STREET_A") == Wet_Stone &&
                      WetKindOf(List, N, "CoreTexMetal.GripMetlFloor_A") == Wet_Metal &&
                      WetKindOf(List, N, L"Paris.pa_street") == Wet_None &&
                      WetKindOf(List, N, L"Paris.pa_street_a_b") == Wet_None &&
                      WetKindOf(List, N, (const char*)0) == Wet_None,
                  "wet floors: a texture found by its whole name, the case aside, and nothing by a part of it");
            FWetSurface Two[2];
            const int   M = ParseWetSurfaces(Text, (int)sizeof(Text) - 1, Two, 2, Bad);
            Check(M == 2 && Bad == 5, "wet floors: past the room, the lines are counted as not read");
            const char  Wood[] = "UNATCO.un_dock_S wood\nCoreTexWood.BoatHouseWood_A  WOOD ; planks\nBatteryPark.DGW2 woods\n";
            FWetSurface Planks[4];
            const int   W = ParseWetSurfaces(Wood, (int)sizeof(Wood) - 1, Planks, 4, Bad);
            Check(W == 2 && Bad == 1 && Planks[0].Kind == Wet_Wood && Planks[1].Kind == Wet_Wood,
                  "wet floors: wood, whatever its case, and a word that only begins so not read");
        }
        {
            // The lamps' bodies' list: a texture or a class a line, remarks after ';', the case aside.
            const char Text[] = "; the lamps\nBatteryPark.BP_LightLens2   texture ; lanterns\r\nRev_SphereLight class\n"
                                "Paris.pa_streetlight_ TEXTURE\nExtension.SolidRed paint\nA.B texture extra\n\n";
            FLampSurface List[8];
            int          Bad = 0;
            const int    N   = ParseLampSurfaces(Text, (int)sizeof(Text) - 1, List, 8, Bad);
            Check(N == 3 && Bad == 2 && List[0].Kind == Lamp_Texture && List[1].Kind == Lamp_Class &&
                      List[2].Kind == Lamp_Texture,
                  "lamps' list: texture and class, any case; a kind that is neither, or a third word, not read");
            Check(LampListed(List, N, L"batterypark.bp_lightlens2", Lamp_Texture) &&
                      LampListed(List, N, "REV_SPHERELIGHT", Lamp_Class) &&
                      !LampListed(List, N, "Rev_SphereLight", Lamp_Texture) &&
                      !LampListed(List, N, "Paris.pa_streetlight", Lamp_Texture),
                  "lamps' list: a name of its kind only, whole, the case aside");
            // Two panes of one lantern, side by side, are one lamp about both; a lamp across the street stays its own;
            // a small body inside a larger one adds nothing to it.
            FLampBody B[4] = { { 0.0f, 0.0f, 0.0f, 8.0f, 1.0f, 1.0f, 1.0f },
                               { 20.0f, 0.0f, 0.0f, 8.0f, 1.0f, 1.0f, 1.0f },
                               { 400.0f, 0.0f, 0.0f, 8.0f, 1.0f, 1.0f, 1.0f },
                               { 402.0f, 0.0f, 0.0f, 2.0f, 1.0f, 1.0f, 1.0f } };
            const int M = MergeLampBodies(B, 4, 8.0f, 48.0f);
            Check(M == 2 && B[0].X > 9.99f && B[0].X < 10.01f && B[0].Extent > 17.99f && B[0].Extent < 18.01f &&
                      B[1].X == 400.0f && B[1].Extent == 8.0f,
                  "lamps' bodies: a lantern's panes one lamp about both; one inside another adds nothing");
            // A row of windows side by side, each its own: one ball about two of them would be larger than a lamp.
            FLampBody Row[3] = { { 0.0f, 0.0f, 0.0f, 30.0f, 1.0f, 1.0f, 1.0f },
                                 { 62.0f, 0.0f, 0.0f, 30.0f, 1.0f, 1.0f, 1.0f },
                                 { 124.0f, 0.0f, 0.0f, 30.0f, 1.0f, 1.0f, 1.0f } };
            Check(MergeLampBodies(Row, 3, 8.0f, 48.0f) == 3 && Row[0].Extent == 30.0f && Row[1].X == 62.0f,
                  "lamps' bodies: windows in a row stay apart, none grown");
            const float Dist[5] = { 30.0f, 10.0f, 50.0f, 10.0f, 20.0f };
            int         Order[5];
            LampsByDistance(Dist, 5, Order);
            Check(Order[0] == 1 && Order[1] == 3 && Order[2] == 4 && Order[3] == 0 && Order[4] == 2,
                  "lamps' bodies: the nearest first, the equal in their order");
            float Lo = 9.0f, Hi = -9.0f;
            for (int t = 0; t < 1000; t++)
            {
                const float F = LampFlicker((float)t * 0.013f, 3.0f);
                Lo            = (F < Lo) ? F : Lo;
                Hi            = (F > Hi) ? F : Hi;
            }
            Check(Lo >= 1.0f - LampFlickerDepth - 1e-4f && Hi <= 1.0f + LampFlickerDepth + 1e-4f && Hi - Lo > 0.2f,
                  "lamps' bodies: a fire's glow wavers within its depth either way, and does waver");
        }
        {
            // The rain's cover: a roof's sharp edge (0 under it, 255 open) becomes a slope over 2 * Radius + 1 cells,
            // rising all along it; the cells well away from the edge keep their value, and an even grid stays even.
            enum { W = 24, H = 3 };
            unsigned char G[W * H], Work[W * H], Even[W * H];
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    G[y * W + x]    = (x < 12) ? 0 : 255;
                    Even[y * W + x] = 200;
                }
            BlurWetCover(G, W, H, 4, Work);
            BlurWetCover(Even, W, H, 4, Work);
            bool bRises = true, bKept = true, bEven = true;
            for (int y = 0; y < H; y++)
            {
                for (int x = 1; x < W; x++)
                    bRises = bRises && G[y * W + x] >= G[y * W + x - 1];
                bKept = bKept && G[y * W + 0] == 0 && G[y * W + 6] == 0 && G[y * W + 17] == 255 && G[y * W + 23] == 255;
                for (int x = 0; x < W; x++)
                    bEven = bEven && Even[y * W + x] == 200;
            }
            Check(bRises && bKept && G[W + 8] > 0 && G[W + 8] < 128 && G[W + 15] > 128 && G[W + 15] < 255 && bEven,
                  "wet cover: a roof's edge softened into a slope, the cells away from it kept, an even grid even");
            // Grown first by the radius, then blurred by it: nought on the cover's side of its edge and next to nothing
            // on the other - the dry floor meets the wet one all but dry -, the full rain twice the radius out.
            unsigned char E[W * H];
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    E[y * W + x] = (x < 8) ? 0 : 255;
            ErodeWetCover(E, W, H, 3, Work);
            BlurWetCover(E, W, H, 3, Work);
            bool bUp = true;
            for (int x = 1; x < W; x++)
                bUp = bUp && E[W + x] >= E[W + x - 1];
            Check(bUp && E[W + 7] == 0 && E[W + 8] < 40 && E[W + 11] > 100 && E[W + 11] < 160 && E[W + 14] == 255,
                  "wet cover: grown and blurred, all but dry at the cover's edge and the full rain twice the radius out");
            Check(WetCoverRadius(16.0f) == 2 && WetCoverRadius(32.0f) == 1 && WetCoverRadius(256.0f) == 1,
                  "wet cover: the band's half in cells, at least one");
            // A pier under its roof: the roof (100 high) over the cells 6..9 of a row, the pier (0) everywhere. The
            // lower floors' share is none under the roof and the pier's own elsewhere; grown and blurred as the cover
            // is, it eases out from the roof's edge, and the highest floor near each cell reaches as far as that
            // easing does - so past it the pier takes its own share again, the two alike there.
            float         Top[W * H], TopWork[W * H];
            unsigned char L[W * H];
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    const bool bRoof = x >= 6 && x <= 9;
                    Top[y * W + x]   = bRoof ? 100.0f : 0.0f;
                    L[y * W + x]     = (bRoof && 100.0f - 0.0f > WetUnderFloor) ? 0 : 255;
                }
            ErodeWetCover(L, W, H, 2, Work);
            BlurWetCover(L, W, H, 2, Work);
            GrowWetTop(Top, W, H, 2 * 2 + 1, TopWork);
            bool bEase = true;
            for (int x = 10; x < W; x++)
                bEase = bEase && L[W + x] >= L[W + x - 1];
            Check(bEase && L[W + 8] == 0 && L[W + 10] < 64 && L[W + 14] == 255 && L[W + 15] == 255,
                  "wet cover: the pier dries towards its roof's edge, its own rain again twice the radius out");
            Check(Top[W + 14] == 100.0f && Top[W + 15] == 0.0f && Top[W + 1] == 100.0f && Top[W + 0] == 0.0f,
                  "wet cover: the highest floor near reaches as far as the easing, no further");
        }
        {
            // The wet patches' noise: zero on its grid's points, smooth between them, the same from the same place,
            // and as near the same far from the level's middle (whole-number hashing, not a sine of a large number).
            bool bGrid = true, bSmooth = true;
            for (int y = -40; y <= 40; y += 7)
                for (int x = -40; x <= 40; x += 3)
                {
                    bGrid = bGrid && WetNoise((float)x, (float)y) == 0.0f;
                    const float X = (float)x * 0.37f + 0.11f, Y = (float)y * 0.29f - 0.23f;
                    const float D = WetNoise(X + 0.001f, Y) - WetNoise(X, Y);
                    bSmooth = bSmooth && D < 0.01f && D > -0.01f;
                }
            Check(bGrid, "wet patches: the noise is zero on its grid's points");
            Check(bSmooth, "wet patches: a thousandth of a cell along, the noise moves by less than a hundredth");
            Check(WetPatch(1234.5f, -987.25f) == WetPatch(1234.5f, -987.25f) &&
                      WetPatch(30000.0f, 30000.0f) != WetPatch(30080.0f, 30000.0f),
                  "wet patches: the same from the same place, and different half a patch away at the level's edge");
            // Over a large stretch of the level: about zero on the whole, within about a half either way; and the
            // share of the floor the look makes into puddles (the water over the stones' tops), its wettest and driest
            // patches - reported for the plan's record, held only to what makes a picture of patches at all.
            double Sum = 0.0;
            int    N = 0, Over = 0, Puddle = 0, Dry = 0;
            for (int y = 0; y < 400; y++)
                for (int x = 0; x < 400; x++)
                {
                    const float P = WetPatch(-8000.0f + 40.0f * (float)x, 5000.0f + 40.0f * (float)y);
                    Sum += P;
                    N++;
                    Over   += (P > 1.0f || P < -1.0f) ? 1 : 0;
                    Puddle += (WetFloodOf(P) > 1.0f) ? 1 : 0;
                    Dry    += (WetLevelOf(P) < 0.5f) ? 1 : 0;
                }
            const double Mean = Sum / (double)N;
            Check(Mean < 0.05 && Mean > -0.05 && Over == 0, "wet patches: about zero on the whole, never past one");
            printf("  wet patches: %.1f%% of the floor puddles over the stones' tops, %.1f%% wet less than half\n",
                   100.0 * Puddle / N, 100.0 * Dry / N);
            Check(Puddle > N / 50 && Puddle < N / 2, "wet patches: puddles over the stones in some of the floor, not most");
            // The water's edge, the stone's darkening and metal's.
            CheckNear(WetPuddleOf(0.5f, 0.5f), 0.5f, 1e-6f, "wet floors: at the water's line half the pixel is under it");
            Check(WetPuddleOf(0.5f, 0.5f + WetPuddleEdge) == 0.0f && WetPuddleOf(0.5f, 0.5f - WetPuddleEdge) == 1.0f,
                  "wet floors: the edge's width over the water dry, as far under it all water");
            CheckNear(WetDarkOf(true, 1.0f, 0.0f), WetStoneDarkest, 1e-6f, "wet floors: wet stone is darkest");
            CheckNear(WetDarkOf(true, 0.2f, 1.0f), WetStoneDarkest, 1e-6f, "wet floors: stone under water is darkest");
            Check(WetDarkOf(false, 1.0f, 1.0f) == 1.0f && WetDarkOf(true, 0.0f, 0.0f) == 1.0f,
                  "wet floors: metal and dry stone keep their colour");
            // The mark the late pass reads: the nearer floor's the larger, kept at its own depth and lost a few steps
            // away, the share under water within a step of its 63, the kind; nothing in a mark of zero.
            const float    Near = 1.0f / 300.0f, Far = 1.0f / 301.0f;
            const unsigned A = WetMarkPack(Near, 0.4f, false), B = WetMarkPack(Far, 1.0f, true);
            Check(A > B, "wet mark: the nearer floor's mark is the larger, whatever it holds");
            Check(WetMarkHolds(A, Near) && !WetMarkHolds(A, Far) && !WetMarkHolds(0u, Near),
                  "wet mark: it holds at its own depth, not at a third of a percent off, and zero holds nowhere");
            CheckNear(WetMarkPuddle(A), 0.4f, 0.5f / 63.0f, "wet mark: the share under water within half a step");
            Check(!WetMarkMetal(A) && WetMarkMetal(B) && WetMarkPuddle(B) == 1.0f,
                  "wet mark: the kind, and all of the pixel under water");
            Check(WetMarkPack(1.0f / 50000.0f, 0.0f, false) != 0u, "wet mark: the farthest floor still marks");
            // The relief's normal beside it: back within a step of its 1022 each way, upright and level both; the
            // nearer floor's word the larger; none from a flat floor, nor from another floor's word than the mark's.
            const float Tilts[5][2] = { { 0.0f, 0.0f }, { 0.31f, -0.12f }, { -0.7f, 0.5f }, { 0.999f, 0.0f },
                                        { -0.05f, -0.64f } };
            float       Worst = 0.0f;
            bool        bAll  = true;
            for (int t = 0; t < 5; t++)
            {
                float X, Y, Z;
                bAll = WetTiltNormal(WetTiltPack(Near, Tilts[t][0], Tilts[t][1], true), A, X, Y, Z) && bAll;
                Worst = (fabsf(X - Tilts[t][0]) > Worst) ? fabsf(X - Tilts[t][0]) : Worst;
                Worst = (fabsf(Y - Tilts[t][1]) > Worst) ? fabsf(Y - Tilts[t][1]) : Worst;
                bAll  = fabsf(X * X + Y * Y + Z * Z - 1.0f) < 0.01f && Z >= 0.0f && bAll;
            }
            Check(bAll, "wet tilt: every normal read back, of unit length, upwards");
            Check(Worst <= 1.0f / (float)WetTiltSteps + 1e-6f, "wet tilt: X and Y within half a step");
            float X, Y, Z;
            Check(WetTiltPack(Near, -1.0f, -1.0f, true) > WetTiltPack(Far / 2.0f, 1.0f, 1.0f, true),
                  "wet tilt: the nearer floor's word is the larger, whatever it holds");
            Check(!WetTiltNormal(WetTiltPack(Near, 0.2f, 0.2f, false), A, X, Y, Z) && Z == 1.0f,
                  "wet tilt: a flat floor's word holds no normal, and the level's up stands in");
            Check(!WetTiltNormal(WetTiltPack(Near / 4.0f, 0.2f, 0.2f, true), A, X, Y, Z),
                  "wet tilt: a farther floor's word is not the mark's floor's");
        }
        {
            // What a texel takes in from across a soft side (SoftLightAcross): with each polygon's light even, the light
            // across itself; with the light running on unbroken across the side, the texel's own - nothing mirrored
            // over; and mixed in by half at the side, the two sides' mean.
            const float Even[4] = { 40.0f, 50.0f, 60.0f, 255.0f }, Other[4] = { 100.0f, 110.0f, 120.0f, 255.0f };
            float       Out[4];
            SoftLightAcross(Even, Even, Other, Out);
            Check(Out[0] == 100.0f && Out[1] == 110.0f && Out[2] == 120.0f,
                  "soft light: even light on both polygons - the light across, as before");
            const float Here[4] = { 200.0f, 180.0f, 160.0f, 255.0f }, AtSide[4] = { 90.0f, 80.0f, 70.0f, 255.0f };
            SoftLightAcross(Here, AtSide, AtSide, Out);
            Check(Out[0] == 200.0f && Out[1] == 180.0f && Out[2] == 160.0f,
                  "soft light: the light running on unbroken across the side - the texel keeps its own");
            float Mixed[4];
            const float Half = 0.5f;
            SoftLightAcross(Even, Even, Other, Out);
            SoftLightMix(Even, &Half, Out, 1, Mixed);
            CheckNear(Mixed[0], 70.0f, 1e-4f, "soft light: at the side, by half - the two sides' mean");
        }
        {
            // The geometry pass's vertex room (VertexRoomFor): kept while the frames ask for three quarters of it or
            // less, else half as much again as asked in whole steps - never past the most, never less than held.
            Check(VertexRoomFor(300000, VertexRoomFirst) == VertexRoomFirst,
                  "vertex room: three quarters asked keeps the room as it is");
            const int Grown = VertexRoomFor(300001, VertexRoomFirst);
            Check(Grown >= 450001 && Grown % VertexRoomStep == 0 && Grown < 450001 + VertexRoomStep,
                  "vertex room: past three quarters, half as much again as asked, in whole steps");
            Check(VertexRoomFor(1900000, 1500000) == VertexRoomMost && VertexRoomFor(2500000, 400000) == VertexRoomMost,
                  "vertex room: never past the most");
            Check(VertexRoomFor(100, VertexRoomMost) == VertexRoomMost, "vertex room: never less than is held");
            Check(VertexRoomFor(2500000, 400000) < 2500000,
                  "vertex room: past the most, less than asked - the caller drops what does not fit");
        }
        {
            // The grass's ground less a floor laid on it (GrassSubtract): the pieces' area is the ground's less the
            // floor's part over it, no point of a piece lies under the floor, and the floor's way round does not matter.
            static float Pieces[GrassPieceMost * 2 * GrassPieceCorners];
            int          Counts[GrassPieceMost];
            const float  Lawn[8] = { 0.0f, 0.0f, 10.0f, 0.0f, 10.0f, 10.0f, 0.0f, 10.0f };
            auto Area = [&](int N)
            {
                float Sum = 0.0f;
                for (int k = 0; k < N; k++)
                    Sum += 0.5f * fabsf(GrassTwiceArea(Pieces + 2 * GrassPieceCorners * k, Counts[k]));
                return Sum;
            };
            // Whether any of the pieces holds a point inside the box (X0..X1, Y0..Y1), the box's edges left out.
            auto Under = [&](int N, float X0, float Y0, float X1, float Y1)
            {
                for (int k = 0; k < N; k++)
                {
                    const float* const P = Pieces + 2 * GrassPieceCorners * k;
                    for (float y = Y0 + 0.05f; y < Y1; y += 0.1f)
                        for (float x = X0 + 0.05f; x < X1; x += 0.1f)
                        {
                            bool bOut = false, bLeft = GrassTwiceArea(P, Counts[k]) > 0.0f;
                            for (int e = 0; e < Counts[k] && !bOut; e++)
                            {
                                const int   f = (e + 1 < Counts[k]) ? e + 1 : 0;
                                const float C = (P[2 * f] - P[2 * e]) * (y - P[2 * e + 1]) -
                                                (P[2 * f + 1] - P[2 * e + 1]) * (x - P[2 * e]);
                                bOut = bLeft ? (C < -1e-4f) : (C > 1e-4f);
                            }
                            if (!bOut)
                                return true;
                        }
                }
                return false;
            };
            const float Hole[8] = { 2.0f, 2.0f, 4.0f, 2.0f, 4.0f, 4.0f, 2.0f, 4.0f };
            int N = GrassSubtract(Lawn, 4, Hole, 4, Pieces, Counts, GrassPieceMost);
            Check(N >= 1 && N <= 4, "grass ground: a floor inside the lawn leaves at most four pieces");
            CheckNear(Area(N), 96.0f, 1e-3f, "grass ground: the lawn less a floor in it keeps the rest of its area");
            Check(!Under(N, 2.0f, 2.0f, 4.0f, 4.0f), "grass ground: no piece reaches under the floor");
            const float Far[8] = { 20.0f, 20.0f, 30.0f, 20.0f, 30.0f, 30.0f, 20.0f, 30.0f };
            N = GrassSubtract(Lawn, 4, Far, 4, Pieces, Counts, GrassPieceMost);
            Check(N == 1 && fabsf(Area(N) - 100.0f) < 1e-3f, "grass ground: a floor elsewhere leaves the lawn whole");
            const float All[8] = { -1.0f, -1.0f, 11.0f, -1.0f, 11.0f, 11.0f, -1.0f, 11.0f };
            N = GrassSubtract(Lawn, 4, All, 4, Pieces, Counts, GrassPieceMost);
            Check(N == 0, "grass ground: a floor over all of it leaves nothing");
            // A path across the lawn, its corners the other way round.
            const float Path[8] = { 4.0f, -5.0f, 4.0f, 15.0f, 6.0f, 15.0f, 6.0f, -5.0f };
            N = GrassSubtract(Lawn, 4, Path, 4, Pieces, Counts, GrassPieceMost);
            Check(N == 2, "grass ground: a path across the lawn leaves its two sides");
            CheckNear(Area(N), 80.0f, 1e-3f, "grass ground: the two sides keep the lawn's area less the path's");
            Check(!Under(N, 4.0f, 0.0f, 6.0f, 10.0f), "grass ground: no piece reaches under the path");
        }
        Check(ReflectionsOfOldWater("Off") == Reflections_Default &&
              ReflectionsOfOldWater("Low") == Reflections_Default &&
              ReflectionsOfOldWater(" medium") == Reflections_High &&
              ReflectionsOfOldWater("High") == Reflections_High && ReflectionsOfOldWater("RT") == Reflections_RtHigh &&
              ReflectionsOfOldWater("") == Reflections_Default &&
              ReflectionsOfOldWater((const char*)0) == Reflections_Default,
              "reflections: a file without the key - Medium and High are the picture's, RT the rays', the rest none");
        {
            bool bRound = true;
            for (int i = 0; i < Reflections_Count; i++)
            {
                char Out[32];
                bool bOk = false;
                ReflectionsName((EReflections)i, Out, 32);
                if (ParseReflections(Out, bOk) != (EReflections)i || !bOk)
                    bRound = false;
            }
            Check(bRound, "reflections: every name reads back as itself");
        }
        {
            // The mirror rays' rows say for any place what the surface's own vertices get from SurfaceCoord.
            const FVec3 AxisU = { 0.5f, 0.0f, 0.25f }, AxisV = { 0.0f, -2.0f, 0.0f }, Origin = { 100.0f, -40.0f, 8.0f };
            const FVec3 Place = { 37.0f, 11.0f, -5.0f };
            const float InvU = 1.0f / 256.0f, InvV = 1.0f / 128.0f;
            float Row[8];
            RtSurfaceRows(AxisU, AxisV, Origin, 3.0f, -7.0f, InvU, InvV, Row);
            const float U = Row[0] * Place.X + Row[1] * Place.Y + Row[2] * Place.Z + Row[3];
            const float V = Row[4] * Place.X + Row[5] * Place.Y + Row[6] * Place.Z + Row[7];
            Check(fabsf(U - SurfaceCoord(AxisU, Origin, Place, 3.0f) * InvU) < 1e-5f &&
                  fabsf(V - SurfaceCoord(AxisV, Origin, Place, -7.0f) * InvV) < 1e-5f,
                  "RT: a surface's rows put a place of the level where its own vertices' coordinates would");
            Check(fabsf(RtSurfaceDensity(Row, 256, 128) - 2.0f) < 1e-5f && RtRecordUints * 4 == RDX_RT_RECORD_BYTES &&
                  RtRecordRows * 4 == 16 && RtRecordDensity * 4 == 48,
                  "RT: texels to a unit along the denser way; the record's parts where the shader reads them");
        }
        {
            // A caster's vertex for the mirror rays: the face's key and own light ride in the slot's fraction, the
            // normal in u's and v's - and the masks' readers still get the slot and the place they had.
            bool bRound = true, bSmall = true;
            for (int Slot = 0; Slot < ShadowMaxMasks; Slot++)
                for (int Key = -1; Key <= RtCasterKeys + 1; Key++)
                    for (int Own = 0; Own < 2; Own++)
                    {
                        const float W    = PackCasterSlot(Slot, Key, Own != 0);
                        const int   Want = (Key >= 0 && Key < RtCasterKeys) ? Key : -1;
                        if (CasterSlotOf(W) != Slot || CasterKeyOf(W) != Want || CasterOwnLightOf(W) != (Own != 0))
                            bRound = false;
                        if (W - (float)Slot >= 0.5f || (Slot == 0 && W >= 0.5f))
                            bSmall = false;
                    }
            Check(bRound, "RT: a face's mask slot, its key and its own light read back out of one number");
            Check(bSmall, "RT: what rides in the slot's fraction stays under a half - the slot rounds to itself");
            bool bPlace = true;
            for (int Byte = 0; Byte < 256; Byte++)
                for (int Oct = 0; Oct < RtNormalSteps; Oct++)
                {
                    const float U = (float)Byte / 256.0f, P = PackCasterPlace(U, Oct);
                    if (CasterOctOf(P) != Oct || P - U < 0.0f || P - U >= 1.0f / 4096.0f)
                        bPlace = false;
                }
            Check(bPlace, "RT: a normal's step reads back out of a texture place, and moves it by under a 4096th");
            float Worst = 1.0f;
            for (int a = 0; a < 40; a++)
                for (int b = 0; b < 21; b++)
                {
                    const float Turn = 6.2831853f * (float)a / 40.0f, Rise = 3.1415927f * ((float)b / 20.0f - 0.5f);
                    const FVec3 N = { cosf(Rise) * cosf(Turn), cosf(Rise) * sinf(Turn), sinf(Rise) };
                    int X = 0, Y = 0;
                    PackOctNormal(N, X, Y);
                    const FVec3 Back = UnpackOctNormal(X, Y);
                    const float Cos  = Dot(N, Back);
                    if (X < 0 || X >= RtNormalSteps || Y < 0 || Y >= RtNormalSteps)
                        Worst = -1.0f;
                    if (Cos < Worst)
                        Worst = Cos;
                }
            Check(Worst > 0.995f, "RT: a normal comes back out of its two steps within six degrees, all round");
            float Cube[RtCubeFloats];
            for (int i = 0; i < RtCubeFloats; i++)
                Cube[i] = 0.0f;
            const FVec3 Up = { 0.0f, 0.0f, 1.0f }, Down = { 0.0f, 0.0f, -1.0f }, East = { 1.0f, 0.0f, 0.0f };
            RtCubeAddLamp(Cube, Up, 0.5f, 0.25f, 1.0f);
            const FVec3 Lit = RtCubeLight(Cube, Up), Dark = RtCubeLight(Cube, Down), Side = RtCubeLight(Cube, East);
            Check(fabsf(Lit.X - 0.5f) < 1e-6f && fabsf(Lit.Y - 0.25f) < 1e-6f && fabsf(Lit.Z - 1.0f) < 1e-6f &&
                  Dark.X == 0.0f && Dark.Z == 0.0f && Side.X == 0.0f && Side.Z == 0.0f,
                  "RT: a lamp overhead lights the face that looks up, and neither its side nor its underside");
            RtCubeAddAll(Cube, 0.1f, 0.1f, 0.1f);
            const FVec3 Under = RtCubeLight(Cube, Down);
            const FVec3 Slant = { 0.6f, 0.0f, 0.8f };
            const FVec3 Half  = RtCubeLight(Cube, Slant);
            Check(fabsf(Under.X - 0.1f) < 1e-6f && fabsf(Half.X - (0.1f + 0.5f * 0.64f)) < 1e-5f,
                  "RT: a light from all round reaches every face; a slanted face gets the lamp's by its tilt");
            Check(RtLampFalloff(0.0f) == 1.0f && RtLampFalloff(1.0f) == 0.0f && RtLampFalloff(2.0f) == 0.0f &&
                  RtLampFalloff(-1.0f) == 1.0f && RtLampFalloff(0.3f) > RtLampFalloff(0.6f) &&
                  RtLampFalloff(0.6f) > RtLampFalloff(0.9f) && RtLampFalloff(0.9f) > 0.0f,
                  "RT: a lamp's light is whole close by, less the further off, and gone at its reach's end");
            Check(RtGeoUints * 4 == RDX_RT_GEO_BYTES && RtGeoScale < RtGeoCube &&
                  RtGeoCube + RtCubeWays <= RtGeoTextures &&
                  RtGeoTextures + RtCasterKeys <= RtGeoUints && RtGeoDensity > RtGeoColour &&
                  RtGeoAmbient > RtGeoDensity && RtGeoScale > RtGeoAmbient && RtRecordAmbient < RtRecordUints &&
                  RtRecordNormal + 3 <= RtRecordAmbient && RtRecordNormal >= RtRecordLightEdge + 4,
                  "RT: a caster's row holds its parts without one lying on another");
        }
        // The levels' glass: the game's window glass by the texture's own name, and only where the level draws it
        // see-through.
        Check(GlassTextureName("WindOpacStrek_A") && GlassTextureName("WindOpacStrek_B") &&
              GlassTextureName("windopacstrek_e") && GlassTextureName(L"07WindOpacStrek") &&
              !GlassTextureName("PrivBlueGlass_A") && !GlassTextureName("OldeStanGlass_A") &&
              !GlassTextureName("Window_B") && !GlassTextureName("Fountain_FX") && !GlassTextureName("") &&
              !GlassTextureName((const char*)0) && !GlassTextureName("WindOpacStre"),
              "glass: the window glass in all its variants, case aside - and no other texture called glass or window");
        Check(IsGlassSurface(RDX_PF_Translucent, true) && !IsGlassSurface(RDX_PF_Translucent, false) &&
              !IsGlassSurface(0u, true) && !IsGlassSurface(RDX_PF_Modulated, true) &&
              IsGlassSurface(RDX_PF_Translucent | RDX_PF_Modulated, true) &&
              !IsGlassSurface(RDX_PF_Translucent | RDX_PF_Mirrored, true),
              "glass: a see-through surface with such a name - not a solid one, and not one the engine mirrors");
        {
            // A pane across the view, three units ahead: its normal looks back at the camera, whichever way its
            // corners go round; and one the camera sees edge on from the other side turns the other way.
            const FVec3 Round[4] = { { -1.0f, -1.0f, 3.0f }, { 1.0f, -1.0f, 3.0f }, { 1.0f, 1.0f, 3.0f },
                                     { -1.0f, 1.0f, 3.0f } };
            const FVec3 Back[4]  = { Round[3], Round[2], Round[1], Round[0] };
            // The first three corners on one line: the cut a view's edge leaves.
            const FVec3 Cut[4]   = { { 0.0f, 0.0f, 3.0f }, { 1.0f, 0.0f, 3.0f }, { 2.0f, 0.0f, 3.0f },
                                     { 2.0f, 1.0f, 3.0f } };
            const FVec3 Side[3]  = { { 2.0f, 0.0f, 1.0f }, { 2.0f, 0.0f, 5.0f }, { 2.0f, 1.0f, 3.0f } };
            const FVec3 Line[3]  = { { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 1.0f }, { 2.0f, 0.0f, 1.0f } };
            FVec3 A = { 0.0f, 0.0f, 0.0f }, B = A, C = A, S = A, L = A;
            Check(PaneNormalToEye(Round, 4, A) && PaneNormalToEye(Back, 4, B) && PaneNormalToEye(Cut, 4, C) &&
                  fabsf(A.Z + 1.0f) < 1e-5f && fabsf(B.Z + 1.0f) < 1e-5f && fabsf(C.Z + 1.0f) < 1e-5f &&
                  fabsf(A.X) < 1e-5f && fabsf(A.Y) < 1e-5f,
                  "glass: a pane's normal looks back at the camera, however its corners go round or were cut");
            Check(PaneNormalToEye(Side, 3, S) && fabsf(S.X + 1.0f) < 1e-5f && !PaneNormalToEye(Line, 3, L) &&
                  !PaneNormalToEye(Round, 2, L),
                  "glass: a pane to the right faces left; corners on one line, or two of them, say no normal");
        }
        Check(SettingIsWord("  RT ", "rt") && !SettingIsWord("RTX", "rt") && !SettingIsWord("R", "rt") &&
              !SettingIsWord("", "rt") && SettingIsWord(L"High", "HIGH"),
              "a setting's text is a word only whole, spaces and case aside");
        Check(CausticMaskValue(false, 5) * 255.0f == 1.0f && CausticMaskValue(true, 0) * 255.0f == 2.0f &&
              CausticMaskValue(true, 63) * 255.0f == 65.0f,
              "caustics: the mask's value - out of a liquid is one, into a zone is the zone and two");
        Check(ParseWater("Ultra", bA) == Water_Off && !bA, "water: a level that does not exist is unknown, and unknown is Off");
        Check(ParseWater((const char*)0, bA) == Water_Off && !bA, "water: no text is Off");
        bool bWaterRoundTrips = true;
        for (int i = 0; i < Water_Count; i++)
        {
            char Out[32];
            bool bOk = false;
            WaterName((EWater)i, Out, 32);
            if (ParseWater(Out, bOk) != (EWater)i || !bOk)
                bWaterRoundTrips = false;
        }
        Check(bWaterRoundTrips, "water: every name reads back as itself");
        Check(WaterWaveStrength(300.0f) < WaterWaveStrength(5000.0f), "water: a pool is calmer than the sea");
        Check(WaterWaveStrength(0.0f) == WaterWaveStrength(512.0f) && WaterWaveStrength(4096.0f) == WaterWaveStrength(1.0e6f),
              "water: the strength stays within its two ends");
        Check(WaterWaveStrength(1000.0f) <= WaterWaveStrength(2000.0f), "water: never calmer for a larger body");
        Check(WaterWaveHeight(300.0f) < WaterWaveHeight(5000.0f) && WaterWaveHeight(1.0e6f) <= 4.0f,
              "water: a pool's real waves are lower than the sea's, and never stormy");
        Check(WaterLampRank(1.0f, 500.0f, 1000.0f, 100.0f) == 0.0f && WaterLampRank(0.0f, 500.0f, 10.0f, 100.0f) == 0.0f,
              "water lamps: a lamp too far over the water, or one without light, does not glint");
        Check(WaterLampRank(1.0f, 500.0f, 600.0f, 100.0f) > 0.0f,
              "water lamps: a lamp whose light barely reaches the water is still mirrored in it");
        Check(WaterLampRank(1.0f, 500.0f, 100.0f, 100.0f) > WaterLampRank(1.0f, 500.0f, 400.0f, 100.0f),
              "water lamps: the nearer the water, the higher it ranks");
        Check(WaterLampRank(1.0f, 500.0f, 100.0f, 100.0f) > WaterLampRank(1.0f, 500.0f, 100.0f, 3000.0f),
              "water lamps: a lamp near the camera ranks above the same lamp far off");
        Check(WaterLampRank(1.0f, 500.0f, 100.0f, 100.0f) > WaterLampRank(0.3f, 500.0f, 100.0f, 100.0f),
              "water lamps: the brighter lamp ranks higher");
        Check(SplashStrength(200.0f, 20.0f) < SplashStrength(600.0f, 20.0f),
              "splashes: the faster the fall, the harder the throw");
        Check(SplashStrength(600.0f, 8.0f) < SplashStrength(600.0f, 40.0f),
              "splashes: the bigger the thing, the harder the throw");
        Check(SplashStrength(1.0e5f, 1.0e4f) <= 380.0f && SplashStrength(0.0f, 20.0f) > 0.0f,
              "splashes: never beyond the cap, and a gentle fall still throws a little");

        // The ripples' field: on the texel grid, around the camera; the steps; the push.
        CheckNear(RippleOrigin(0.0f), -1024.0f, 1.0e-3f, "ripples: the field is centred on the camera");
        CheckNear(RippleOrigin(5.0f), -1024.0f, 1.0e-3f, "ripples: a small move keeps the grid");
        CheckNear(RippleOrigin(9.0f), -1016.0f, 1.0e-3f, "ripples: a texel's move moves the field by a texel");
        CheckNear(RippleOrigin(-3.0f), -1032.0f, 1.0e-3f, "ripples: below zero the grid holds too");
        {
            float Carry = 0.0f;
            int   Total = 0;
            // 240 frames: a quarter step each, which a float holds exactly (a fifth of a step would drift by one).
            for (int f = 0; f < 240; f++)
                Total += RippleSteps(Carry, 1.0f / 240.0f);
            Check(Total == 60, "ripples: 240 frames of a second make 60 steps");
            Carry = 0.0f;
            Check(RippleSteps(Carry, 5.0f) == RippleMaxSteps && Carry == 0.0f, "ripples: a stall is not made up for");
            Carry = 0.0f;
            Check(RippleSteps(Carry, -1.0f) == 0, "ripples: time going back makes no step");
        }
        Check(RipplePush(0.0f, 0.0f) == 0.0f, "ripples: lying still pushes nothing");
        Check(RipplePush(100.0f, 0.0f) < RipplePush(500.0f, 0.0f) && RipplePush(1.0e6f, 0.0f) == 4.0f,
              "ripples: faster pushes harder, to a cap");
        Check(RipplePush(0.0f, 100.0f) > 0.0f && RipplePush(0.0f, -100.0f) == -RipplePush(0.0f, 100.0f),
              "ripples: going in raises the water and coming out lowers it by as much - a bob evens out");
        Check(RipplePush(0.0f, -1.0e6f) == -4.0f, "ripples: the push is capped both ways");
        Check(RippleLimit > 4.0f && RippleLimit < 64.0f, "ripples: the field's bound lets a ring rise, not a tower");
        Check(RippleSpeed2 < 0.5f, "ripples: the wave equation is stable at this speed");

        // The textures' mip bias under FSR: AMD's rule, log2(render / output) - 1, and nothing where it does not apply.
        CheckNear(FsrMipBias(Upscaling_Quality, false), (float)(log(1.0 / 1.5) / log(2.0)) - 1.0f, 1.0e-5f, "mip bias: Quality");
        CheckNear(FsrMipBias(Upscaling_Balanced, false), (float)(log(1.0 / 1.7) / log(2.0)) - 1.0f, 1.0e-5f, "mip bias: Balanced");
        CheckNear(FsrMipBias(Upscaling_Performance, false), -2.0f, 1.0e-6f, "mip bias: Performance");
        Check(FsrMipBias(Upscaling_NativeAA, false) == 0.0f, "mip bias: NativeAA untouched unless asked");
        Check(FsrMipBias(Upscaling_NativeAA, true) == -1.0f, "mip bias: NativeAA asked - the rule's -1");
        Check(FsrMipBias(Upscaling_Off, true) == 0.0f, "mip bias: no upscaler, no bias");

        // A neighbour's share, with the surface's normal pointing straight up.
        const float R = AoRadius;
        CheckNear(AoTap(0.0f, 0.0f, R * 0.5f, 0.0f, 0.0f, 1.0f, R), 0.75f, 1.0e-5f,
                  "straight out at half the radius: 1 - 0.5 squared");
        Check(AoTap(R * 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, R) == 0.0f, "in the surface's own plane: nothing");
        Check(AoTap(0.0f, 0.0f, -R * 0.5f, 0.0f, 0.0f, 1.0f, R) == 0.0f, "behind the surface: nothing");
        Check(AoTap(0.0f, 0.0f, R, 0.0f, 0.0f, 1.0f, R) == 0.0f, "exactly at the radius: nothing");
        Check(AoTap(0.0f, 0.0f, R * 1.01f, 0.0f, 0.0f, 1.0f, R) == 0.0f, "beyond the radius: nothing");
        // Five degrees out of the plane: its cosine against the normal is about 0.087, under the bias.
        Check(AoTap(10.0f, 0.0f, 0.875f, 0.0f, 0.0f, 1.0f, R) == 0.0f,
              "a hair out of the plane is under the bias - a flat wall does not occlude itself");
        Check(AoTap(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, R) > AoTap(0.0f, 0.0f, 12.0f, 0.0f, 0.0f, 1.0f, R) &&
              AoTap(0.0f, 0.0f, 12.0f, 0.0f, 0.0f, 1.0f, R) > AoTap(0.0f, 0.0f, 20.0f, 0.0f, 0.0f, 1.0f, R),
              "the nearer straight out, the more it takes");
        Check(AoTap(0.0f, 5.0f, 5.0f, 0.0f, 0.0f, 1.0f, R) < AoTap(0.0f, 0.0f, 7.0710678f, 0.0f, 0.0f, 1.0f, R),
              "at the same distance a slant takes less than straight out");

        Check(AoFromSum(0.0f, AoSamples) == 1.0f, "no share at all: open, 1");
        CheckNear(AoFromSum((float)AoSamples * 0.1f, AoSamples), 1.0f - AoStrength * 0.1f, 1.0e-6f,
                  "a tenth of the full share: 1 less a tenth of the strength");
        {
            // Straight down to the knee, then K / (1 + (K - A) / K): the same value and slope there, never black.
            const float AtKnee = (1.0f - AoKnee) / AoStrength;            // the taps' average where the knee is
            const float Full   = 1.0f - AoStrength;                        // every neighbour full, kept straight
            CheckNear(AoFromSum((float)AoSamples, AoSamples),
                      (Full >= AoKnee) ? Full : AoKnee / (1.0f + (AoKnee - Full) / AoKnee), 1.0e-6f,
                      "every neighbour full: past the knee, bent - not black");
            Check(AoFromSum((float)AoSamples, AoSamples) > 0.0f, "every neighbour full: still above zero");
            CheckNear(AoFromSum((float)AoSamples * AtKnee, AoSamples), AoKnee, 1.0e-5f, "at the knee: the knee's value");
            const float D = 1.0e-3f;
            const float Below = (AoFromSum((float)AoSamples * (AtKnee + D), AoSamples) - AoKnee) / D;
            CheckNear(Below, -AoStrength, 0.02f * AoStrength, "just past the knee: the straight part's slope");
            CheckNear(AoFromSum((float)AoSamples / (2.0f * AoStrength), AoSamples), 0.5f, 1.0e-5f,
                      "a right-angled crease (about 0.5) is on the straight part, unchanged");
            float Prev = 1.0f;
            bool bFalls = true;
            for (int s = 1; s <= 40; s++)
            {
                const float A = AoFromSum((float)AoSamples * (float)s / 40.0f, AoSamples);
                bFalls = bFalls && A < Prev && A > 0.0f;
                Prev = A;
            }
            Check(bFalls, "more share, darker - every step, and never zero");
        }
        Check(AoFromSum(1.0f, 0) == 1.0f, "no samples: open");
        Check(AoFromSum(1.0e6f, AoSamples) >= 0.0f, "never under zero");

        // The constants: whole registers, within a root signature's 64 values, and the rectangles where the count says.
        Check(AoCbCount % 4 == 0, "the constants fill whole registers");
        Check(AoCbCount <= 64, "within a root signature's 64 values");
        Check(AoCbSkipRect + 4 * AoSkipRects == AoCbSkipCount, "the rectangles end where their count begins");
        Check(AoCbSkipCount < AoCbRadius && AoCbRadius < AoCbStrength && AoCbStrength < AoCbCosBias &&
              AoCbCosBias < AoCbCount, "the last register in the shader's order");
        Check(AoSamples == RDX_AO_SAMPLES, "the sample count is the one pasted into the shader");
    }

    // Which preset changes need other shadow MAPS (FBackend::SetShadowQuality): only the lights and the cube sizes. The
    // menu applies its whole state on every change - so a wrong answer here either rebuilds hundreds of MB for an AO
    // switch, or keeps maps of the wrong size.
    printf("shadows: which preset changes need the maps made again\n");
    {
        const FShadowPreset High = ShadowPresetFor(ShadowQuality_High);
        const FShadowPreset Med  = ShadowPresetFor(ShadowQuality_Medium);
        FShadowPreset Half = High;
        Half.bFullResolution = !High.bFullResolution;
        FShadowPreset Taps = High;
        Taps.FilterTaps = High.FilterTaps / 2;
        FShadowPreset Reach = High;
        Reach.Reach = High.Reach * 0.5f;
        FShadowPreset Actor = High;
        Actor.ActorMapSize = High.ActorMapSize / 2;
        FShadowPreset World = High;
        World.WorldMapSize = High.WorldMapSize * 2;
        Check(ShadowMapsSame(High, High), "the same preset keeps the maps");
        Check(ShadowMapsSame(High, Half), "the half factor keeps the maps");
        Check(ShadowMapsSame(High, Taps), "the filter's taps keep the maps");
        Check(ShadowMapsSame(High, Reach), "the reach keeps the maps");
        Check(!ShadowMapsSame(High, Med), "High to Medium: fewer lights, the maps are made again");
        Check(!ShadowMapsSame(High, Actor), "another actor cube size: made again");
        Check(!ShadowMapsSame(High, World), "another level cube size: made again");
    }

    // The output's steps down (HdrModeForDisplay): HDR only on a display in HDR mode, ten bits only over a ten-bit
    // connection, the setting itself untouched - and the reason given for the readout.
    printf("HDR: the output steps down to what the display can take\n");
    {
        EHdrFallback W;
        Check(HdrModeForDisplay(Hdr_On, true, 1, 10, W) == Hdr_On && W == HdrFallback_None, "HDR display: HDR");
        Check(HdrModeForDisplay(Hdr_On, true, 1, 8, W) == Hdr_On && W == HdrFallback_None,
              "a display in HDR mode runs HDR whatever its bits");
        Check(HdrModeForDisplay(Hdr_On, false, 0, 10, W) == Hdr_Ten && W == HdrFallback_NeedsHdrDisplay,
              "SDR ten-bit display: ten bits, and it needs an HDR display");
        Check(HdrModeForDisplay(Hdr_On, false, 1, 10, W) == Hdr_Ten && W == HdrFallback_HdrOffInWindows,
              "HDR-capable display with HDR off in Windows: ten bits, and turn it on");
        Check(HdrModeForDisplay(Hdr_On, false, 0, 8, W) == Hdr_Off && W == HdrFallback_NeedsHdrDisplay,
              "SDR eight-bit display: two steps down to eight bits, the first reason kept");
        Check(HdrModeForDisplay(Hdr_Ten, false, 0, 10, W) == Hdr_Ten && W == HdrFallback_None, "10bitSDR on ten bits");
        Check(HdrModeForDisplay(Hdr_Ten, false, 0, 8, W) == Hdr_Off && W == HdrFallback_Needs10BitDisplay,
              "10bitSDR on eight bits: eight, and it needs a ten-bit display");
        Check(HdrModeForDisplay(Hdr_Ten, true, 1, 10, W) == Hdr_Ten && W == HdrFallback_None,
              "10bitSDR stays ten bits on an HDR display - asking for less is never stepped up");
        Check(HdrModeForDisplay(Hdr_Ten, false, 0, 0, W) == Hdr_Ten && W == HdrFallback_None,
              "bits unknown: taken as able");
        Check(HdrModeForDisplay(Hdr_Off, false, 0, 8, W) == Hdr_Off && W == HdrFallback_None, "Off is Off");
        Check(HdrModeForDisplay(Hdr_Off, true, 1, 10, W) == Hdr_Off && W == HdrFallback_None,
              "Off stays Off on the best display");
        Check(HdrModeForDisplay((EHdrMode)7, true, 1, 10, W) == Hdr_Off, "a value out of range: Off");
    }

    // The menu key (DebugKey): every way of naming one, the keys that cannot be used, and that what a first start
    // writes out is read back as the same key.
    printf("menu key: the DebugKey values\n");
    {
        int Vk = 0;
        Check(ParseMenuKey("ScrollLock", Vk) == MenuKey_Ok && Vk == 0x91, "ScrollLock");
        Check(ParseMenuKey(" scroll lock ", Vk) == MenuKey_Ok && Vk == 0x91, "scroll lock, with a space and blanks around");
        Check(ParseMenuKey("scrollock", Vk) == MenuKey_Ok && Vk == 0x91, "scrollock - the double L misspelt");
        Check(ParseMenuKey("SCROLL_LOCK", Vk) == MenuKey_Ok && Vk == 0x91, "SCROLL_LOCK");
        Check(ParseMenuKey(L"ScrLk", Vk) == MenuKey_Ok && Vk == 0x91, "ScrLk, as wide characters");
        Check(ParseMenuKey("0x91", Vk) == MenuKey_Ok && Vk == 0x91, "0x91");
        Check(ParseMenuKey("0X91", Vk) == MenuKey_Ok && Vk == 0x91, "0X91");
        Check(ParseMenuKey("145", Vk) == MenuKey_Ok && Vk == 0x91, "145, decimal");
        Check(ParseMenuKey("Pause", Vk) == MenuKey_Ok && Vk == 0x13, "Pause");
        Check(ParseMenuKey("page-down", Vk) == MenuKey_Ok && Vk == 0x22, "page-down");
        Check(ParseMenuKey("PgUp", Vk) == MenuKey_Ok && Vk == 0x21, "PgUp");
        Check(ParseMenuKey("f10", Vk) == MenuKey_Ok && Vk == 0x79, "f10");
        Check(ParseMenuKey("F1", Vk) == MenuKey_Ok && Vk == 0x70, "F1");
        Check(ParseMenuKey("F24", Vk) == MenuKey_Ok && Vk == 0x87, "F24");
        Check(ParseMenuKey("F25", Vk) == MenuKey_Unknown && Vk == 0x91, "F25 is no key - the default stays");
        Check(ParseMenuKey("F0", Vk) == MenuKey_Unknown, "F0 is no key");
        Check(ParseMenuKey("k", Vk) == MenuKey_Ok && Vk == 'K', "a letter is its capital's code");
        Check(ParseMenuKey("F", Vk) == MenuKey_Ok && Vk == 'F', "F alone is the letter F");
        Check(ParseMenuKey("7", Vk) == MenuKey_Ok && Vk == '7', "a single digit is the digit key, not code 7");
        Check(ParseMenuKey("", Vk) == MenuKey_Empty && Vk == 0x91, "empty: the default, nothing to say");
        Check(ParseMenuKey("   ", Vk) == MenuKey_Empty && Vk == 0x91, "blanks only: empty");
        Check(ParseMenuKey((const char*)nullptr, Vk) == MenuKey_Empty && Vk == 0x91, "no text at all: empty");
        Check(ParseMenuKey("Ctrl+F1", Vk) == MenuKey_Unknown && Vk == 0x91, "a combination is not a key");
        Check(ParseMenuKey("0x", Vk) == MenuKey_Unknown, "0x with no digits");
        Check(ParseMenuKey("0x9G", Vk) == MenuKey_Unknown, "a hex number with a stray letter");
        Check(ParseMenuKey("12ab", Vk) == MenuKey_Unknown, "digits then letters");
        // Refused - with the key left in Vk, so the log can name it.
        Check(ParseMenuKey("Esc", Vk) == MenuKey_Unknown, "Esc has no name here, and would be refused anyway");
        Check(ParseMenuKey("0x1B", Vk) == MenuKey_Refused && Vk == 0x1B, "Esc by code: refused, and named");
        Check(ParseMenuKey("0x25", Vk) == MenuKey_Refused && Vk == 0x25, "left arrow: refused");
        Check(ParseMenuKey("40", Vk) == MenuKey_Refused && Vk == 0x28, "down arrow, decimal: refused");
        Check(ParseMenuKey("0x90", Vk) == MenuKey_Refused, "Num Lock: refused");
        Check(ParseMenuKey("0x2C", Vk) == MenuKey_Refused, "Print Screen: refused");
        Check(ParseMenuKey("0x10", Vk) == MenuKey_Refused, "Shift: refused");
        Check(ParseMenuKey("0xA3", Vk) == MenuKey_Refused, "right Ctrl: refused");
        Check(ParseMenuKey("0x5B", Vk) == MenuKey_Refused, "the Windows key: refused");
        Check(ParseMenuKey("0x01", Vk) == MenuKey_Refused, "a mouse button: refused");
        Check(ParseMenuKey("0x100", Vk) == MenuKey_Refused, "past the last key code: refused");
        Check(ParseMenuKey("0xFFFFF", Vk) == MenuKey_Unknown, "too many digits");
        Check(MenuKeyRefusal(0x91) == nullptr && MenuKeyRefusal(0x08) == nullptr, "Scroll Lock and Backspace can be used");
        Check(MenuKeyRefusal(0x07) != nullptr, "nothing below Backspace can");

        // What a first start writes, read back: every usable code comes back as itself, every refused one as refused.
        int  RoundTrip = 0, Refused = 0;
        bool bAll = true;
        for (int V = 0; V <= 0x1FF; V++)
        {
            char Name[32];
            MenuKeyIniName(V, Name, 32);
            int Back = 0;
            const EMenuKeyParse R = ParseMenuKey(Name, Back);
            if (V <= 0xFE && !MenuKeyRefusal(V))
            {
                bAll = bAll && R == MenuKey_Ok && Back == V;
                RoundTrip++;
            }
            else if (V <= 0xFE)
            {
                bAll = bAll && R == MenuKey_Refused && Back == V;
                Refused++;
            }
        }
        Check(bAll, "every key code written out is read back as the same key, or refused as the same key");
        Check(RoundTrip > 200 && Refused > 10, "and the loop covered both kinds");
        char Ini[32];
        MenuKeyIniName(0x91, Ini, 32);
        Check(strcmp(Ini, "ScrollLock") == 0, "Scroll Lock is written ScrollLock");
        MenuKeyIniName(0x79, Ini, 32);
        Check(strcmp(Ini, "F10") == 0, "F10");
        MenuKeyIniName(0xBA, Ini, 32);
        Check(strcmp(Ini, "0xBA") == 0, "a key with no name as 0x and two hex digits");

        // On screen: in capitals, and never longer than the menu's last line can hold.
        char Cap[24];
        MenuKeyCaption(0x91, Cap, 24);
        Check(strcmp(Cap, "SCROLL LOCK") == 0, "SCROLL LOCK");
        MenuKeyCaption(0x87, Cap, 24);
        Check(strcmp(Cap, "F24") == 0, "F24");
        MenuKeyCaption('K', Cap, 24);
        Check(strcmp(Cap, "K") == 0, "K");
        MenuKeyCaption(0xBA, Cap, 24);
        Check(strcmp(Cap, "KEY 186") == 0, "KEY 186");
        bool bFits = true, bFont = true;
        for (int V = 0; V <= 0xFF; V++)
        {
            MenuKeyCaption(V, Cap, 24);
            // "ARROWS - " and " CLOSES" around it.
            bFits = bFits && (int)strlen(Cap) + 16 <= CaptionLineChars;
            for (int k = 0; Cap[k]; k++)
                bFont = bFont && ((Cap[k] >= 'A' && Cap[k] <= 'Z') || (Cap[k] >= '0' && Cap[k] <= '9') || Cap[k] == ' ');
        }
        Check(bFits, "every key's name fits the menu's last line");
        Check(bFont, "and is written in letters the caption font has");
    }

    printf("antialiasing: the AntiAliasing key's picks, and what they come to on each card\n");
    bool bAa = false;
    Check(ParseAaChoice("Auto", bAa) == AaChoice_Auto && bAa, "Auto");
    Check(ParseAaChoice(" fsr4 ", bAa) == AaChoice_Fsr4 && bAa, "fsr4, with spaces around it");
    Check(ParseAaChoice(L"DLSS", bAa) == AaChoice_Dlss && bAa, "DLSS, as wide characters");
    Check(ParseAaChoice("ssaa16x", bAa) == AaChoice_Ssaa16 && bAa, "ssaa16x");
    Check(ParseAaChoice("MSAA8X\r", bAa) == AaChoice_Msaa8 && bAa, "MSAA8X, a carriage return after it");
    Check(ParseAaChoice("FSRNativeAA", bAa) == AaChoice_Auto && !bAa,
          "the old FSRNativeAA is no pick any more: Auto, and bKnown is false");
    Check(ParseAaChoice((const char*)0, bAa) == AaChoice_Auto && !bAa, "no text: Auto");
    {
        bool bRound = true;
        for (int i = 0; i < (int)AaChoice_Count; i++)
        {
            char Out[16];
            bool bOk = false;
            AaChoiceName((EAaChoice)i, Out, 16);
            bRound = bRound && ParseAaChoice(Out, bOk) == (EAaChoice)i && bOk;
        }
        Check(bRound, "every pick's name reads back the same");
        char Name[16];
        AaChoiceName(AaChoice_Fsr3, Name, 16);
        Check(strcmp(Name, "FSR3") == 0, "the name reads back too: FSR3");
    }
    Check(AaChoiceStep(AaChoice_Auto, -1) == AaChoice_Msaa16 && AaChoiceStep(AaChoice_Msaa16, 1) == AaChoice_Auto &&
          AaChoiceStep(AaChoice_Off, 1) == AaChoice_Fsr3, "the menu's steps go round both ways");
    Check(AaChoiceIsTemporal(AaChoice_Auto) && AaChoiceIsTemporal(AaChoice_Fsr3) && AaChoiceIsTemporal(AaChoice_Fsr4) &&
          AaChoiceIsTemporal(AaChoice_Dlss) && !AaChoiceIsTemporal(AaChoice_Off) && !AaChoiceIsTemporal(AaChoice_Smaa),
          "the four temporal picks");

    Check(GpuVendorOf(0x1002) == Gpu_Amd && GpuVendorOf(0x10DE) == Gpu_Nvidia && GpuVendorOf(0x8086) == Gpu_Intel &&
          GpuVendorOf(0x1414) == Gpu_Other, "the card's maker, from its vendor id");
    Check(AaChoiceOn(AaChoice_Auto, Gpu_Amd) == AaChoice_Fsr4 && AaChoiceOn(AaChoice_Auto, Gpu_Nvidia) == AaChoice_Dlss &&
          AaChoiceOn(AaChoice_Auto, Gpu_Intel) == AaChoice_Fsr3 && AaChoiceOn(AaChoice_Auto, Gpu_Other) == AaChoice_Fsr3,
          "Auto: FSR4 on AMD, DLSS on NVIDIA, FSR3 on any other card");
    Check(AaChoiceOn(AaChoice_Dlss, Gpu_Amd) == AaChoice_Dlss && AaChoiceOn(AaChoice_Smaa, Gpu_Nvidia) == AaChoice_Smaa,
          "every other pick stays itself, on any card");

    Check(AaRunFor(AaChoice_Fsr4, Upscaling_Quality) == AA_FsrQuality &&
          AaRunFor(AaChoice_Auto, Upscaling_NativeAA) == AA_FsrNativeAA &&
          AaRunFor(AaChoice_Dlss, Upscaling_Performance) == AA_FsrPerformance &&
          AaRunFor(AaChoice_Fsr3, Upscaling_Balanced) == AA_FsrBalanced,
          "a temporal pick runs the mode of its size");
    Check(AaRunFor(AaChoice_Ssaa9, Upscaling_Quality) == AA_Ssaa9 &&
          AaRunFor(AaChoice_Msaa4, Upscaling_Quality) == AA_Msaa4 &&
          AaRunFor(AaChoice_Off, Upscaling_Quality) == AA_Off &&
          AaRunFor(AaChoice_Fxaa, Upscaling_NativeAA) == AA_Fxaa,
          "the others run as they are, and the size means nothing to them");
    Check(AaRunFor(AaChoice_Fsr4, Upscaling_Off) == AA_FsrNativeAA, "a size of Off is taken as NativeAA");

    Check(UpscalerFor(AaChoice_Fsr4, Gpu_Amd) == Upscaler_Auto && UpscalerFor(AaChoice_Auto, Gpu_Amd) == Upscaler_Auto,
          "FSR4 on AMD: the host's FSR");
    Check(UpscalerFor(AaChoice_Fsr4, Gpu_Nvidia) == Upscaler_Builtin &&
          UpscalerFor(AaChoice_Dlss, Gpu_Amd) == Upscaler_Builtin,
          "FSR4 or DLSS on another maker's card: the built-in FSR 3.1 in its place");
    Check(UpscalerFor(AaChoice_Dlss, Gpu_Nvidia) == Upscaler_Dlss &&
          UpscalerFor(AaChoice_Auto, Gpu_Nvidia) == Upscaler_Dlss,
          "DLSS on NVIDIA: the host's DLSS");
    Check(UpscalerFor(AaChoice_Fsr3, Gpu_Amd) == Upscaler_Builtin &&
          UpscalerFor(AaChoice_Auto, Gpu_Intel) == Upscaler_Builtin, "FSR3: the built-in one");
    Check(UpscalerFor(AaChoice_Xess, Gpu_Amd) == Upscaler_Xess &&
          UpscalerFor(AaChoice_Xess, Gpu_Nvidia) == Upscaler_Xess &&
          UpscalerFor(AaChoice_Xess, Gpu_Intel) == Upscaler_Xess && AaChoiceIsTemporal(AaChoice_Xess) &&
          AaNeed(AaChoice_Xess, Gpu_Other) == Need_None && FrameGenNeed(AaChoice_Xess, Gpu_Intel) == Need_Fsr4OrDlss,
          "XeSS: the host's XeSS on any card, temporal, with no frame generation of ours");
    {
        bool bX = false;
        char N[16];
        UpscalerName(Upscaler_Xess, N, 16);
        Check(ParseAaChoice("xess", bX) == AaChoice_Xess && bX && strcmp(N, "XeSS") == 0, "XeSS: its names");
    }
    Check(XessMipBias(Upscaling_Performance) == -1.0f && XessMipBias(Upscaling_NativeAA) == 0.0f &&
          XessMipBias(Upscaling_Quality) > FsrMipBias(Upscaling_Quality, false),
          "XeSS's mip bias: log2 of the ratio alone, a step less than FSR's");

    // What each line needs, in its order: the antialiasing, then the card. None of them waits for the shadows.
    Check(AaNeed(AaChoice_Fsr4, Gpu_Nvidia) == Need_AmdChip && AaNeed(AaChoice_Dlss, Gpu_Amd) == Need_NvidiaChip &&
          AaNeed(AaChoice_Auto, Gpu_Intel) == Need_None && AaNeed(AaChoice_Fsr4, Gpu_Amd) == Need_None &&
          AaNeed(AaChoice_Fsr3, Gpu_Other) == Need_None && AaNeed(AaChoice_Ssaa4, Gpu_Other) == Need_None,
          "the antialiasing: FSR4 and DLSS need their maker's card; the others need nothing");
    Check(VectorsNeed(AaChoice_Fxaa) == Need_FsrOrDlss && VectorsNeed(AaChoice_Off) == Need_FsrOrDlss &&
          VectorsNeed(AaChoice_Dlss) == Need_None && VectorsNeed(AaChoice_Auto) == Need_None,
          "the upscale and the motion blur: a temporal upscaler");
    Check(FrameGenNeed(AaChoice_Dlss, Gpu_Nvidia) == Need_None &&
          FrameGenNeed(AaChoice_Dlss, Gpu_Amd) == Need_NvidiaChip &&
          FrameGenNeed(AaChoice_Fsr3, Gpu_Amd) == Need_Fsr4OrDlss &&
          FrameGenNeed(AaChoice_Fsr4, Gpu_Nvidia) == Need_AmdChip &&
          FrameGenNeed(AaChoice_Auto, Gpu_Amd) == Need_None &&
          FrameGenNeed(AaChoice_Auto, Gpu_Nvidia) == Need_None &&
          FrameGenNeed(AaChoice_Auto, Gpu_Intel) == Need_Fsr4OrDlss,
          "the frame generation: FSR4 on an AMD card or DLSS on an NVIDIA one");
    {
        // Every word of theirs goes on screen: the caption font's letters only, with room for them after a label.
        bool bFont = true, bFits = true;
        for (int n = 1; n < (int)Need_Count; n++)
        {
            const char* T = NeedText((ENeed)n);
            bFits = bFits && T[0] && 20 + (int)strlen(T) <= CaptionLineChars;
            for (int k = 0; T[k]; k++)
                bFont = bFont && ((T[k] >= 'A' && T[k] <= 'Z') || (T[k] >= '0' && T[k] <= '9') || T[k] == ' ');
        }
        Check(bFont && bFits, "every need is said in the caption font's letters, and fits a line after its label");
    }

    Check(AaIsPost(AA_Fxaa) && AaIsPost(AA_Smaa) && !AaIsPost(AA_Ssaa4), "the two post-process filters");
    Check(AaIsSsaa(AA_Ssaa4) && AaIsSsaa(AA_Ssaa16) && !AaIsSsaa(AA_Msaa4), "the supersampling values");
    Check(AaIsFsr(AA_FsrQuality) && !AaIsFsr(AA_Ssaa9), "the FSR values");

    // Samples per pixel: 4 = 2x2, 9 = 3x3, 16 = 4x4.
    Check(AaSceneScale(AA_Ssaa4) == 2 && AaSceneScale(AA_Ssaa9) == 3 && AaSceneScale(AA_Ssaa16) == 4,
          "SSAA: the name carries the sample count, the drawing scale is per side");
    Check(AaSceneScale(AA_Off) == 1 && AaSceneScale(AA_Fxaa) == 1 && AaSceneScale(AA_FsrQuality) == 1,
          "anything that is not supersampling draws at 1x");
    Check(AaMsaaSamples(AA_Msaa2) == 2 && AaMsaaSamples(AA_Msaa8) == 8 && AaMsaaSamples(AA_Msaa16) == 16 &&
          AaMsaaSamples(AA_Ssaa16) == 0, "MSAA: a real hardware sample count");
    Check(AaPostSpan(AA_Fxaa) == 8 && AaPostSpan(AA_Ssaa4) == 0,
          "the filter's search length is an internal constant, not a setting");

    Check(AaUpscaling(AA_FsrNativeAA) == Upscaling_NativeAA && AaUpscaling(AA_FsrPerformance) == Upscaling_Performance &&
          AaUpscaling(AA_Off) == Upscaling_Off && AaUpscaling(AA_Ssaa16) == Upscaling_Off,
          "the FSR values map to the upscaling modes, the rest do not");
    Check(AaFromUpscaling(Upscaling_Quality) == AA_FsrQuality && AaFromUpscaling(Upscaling_Off) == AA_Off,
          "and back again");

    {
        char Name[32];
        AaName(AA_Ssaa16, Name, 32);
        Check(strcmp(Name, "SSAA16X") == 0, "the name reads back too: SSAA16X");
        AaName(AA_Fxaa, Name, 32);
        Check(strcmp(Name, "FXAA") == 0, "the name reads back too: FXAA");
    }

    float One = 1.0f;
    unsigned int OneBits = 0;
    memcpy(&OneBits, &One, sizeof(OneBits));
    Check(FloatBits(1.0f) == OneBits && FloatBits(0.0f) == 0 && FloatBits(-2.5f) == 0xC0200000u, "the float's bits");

    // The jitter FSR 2/3 expects: Halton(2, 3), in pixels, between -0.5 and 0.5.
    CheckNear(HaltonAt(1, 2), 0.5f, 1e-6f, "Halton(1, 2) = 1/2");
    CheckNear(HaltonAt(2, 2), 0.25f, 1e-6f, "Halton(2, 2) = 1/4");
    CheckNear(HaltonAt(3, 2), 0.75f, 1e-6f, "Halton(3, 2) = 3/4");
    CheckNear(HaltonAt(1, 3), 1.0f / 3.0f, 1e-6f, "Halton(1, 3) = 1/3");
    CheckNear(HaltonAt(3, 3), 1.0f / 9.0f, 1e-6f, "Halton(3, 3) = 1/9");
    Check(FsrJitterPhaseCount(2560, 3840) == 18 && FsrJitterPhaseCount(2259, 3840) == 23 &&
          FsrJitterPhaseCount(1920, 3840) == 32 && FsrJitterPhaseCount(1280, 3840) == 72,
          "the phase count: 1.5x -> 18, 1.7x -> 23, 2x -> 32, 3x -> 72");
    Check(FsrJitterPhaseCount(0, 3840) == 1 && FsrJitterPhaseCount(3840, 3840) == 8, "boundaries: zero width, 1x");
    float Jx = 0.0f, Jy = 0.0f;
    FsrJitterOffset(0, 18, Jx, Jy);
    CheckNear(Jx, 0.0f, 1e-6f, "the first jitter's x: 1/2 - 1/2");
    CheckNear(Jy, 1.0f / 3.0f - 0.5f, 1e-6f, "the first jitter's y: 1/3 - 1/2");
    float Ax = 0.0f, Ay = 0.0f, Bx = 0.0f, By = 0.0f;
    FsrJitterOffset(5, 18, Ax, Ay);
    FsrJitterOffset(5 + 18, 18, Bx, By);
    Check(Ax == Bx && Ay == By, "the sequence repeats after the phase count");
    bool bInside = true;
    for (int i = 0; i < 64; i++)
    {
        float X = 0.0f, Y = 0.0f;
        FsrJitterOffset(i, 32, X, Y);
        if (X < -0.5f || X > 0.5f || Y < -0.5f || Y > 0.5f)
            bInside = false;
    }
    Check(bInside, "every jitter is smaller than half a pixel");
}

static void TestFrameShadowView()
{
    printf("shadows: the world's view taken from the scene node (place, field of view)\n");

    float Rect[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    float TanX = 0.0f, TanY = 0.0f;
    // The whole 3840x2160 screen at 90 degrees: Proj.Z = FX2 / tan(45) (UnRender.h, FTransform::Project).
    bool bOk = FrameShadowView(0, 0, 3840, 2160, 3840, 2160, 1920.0f, 1080.0f, 1920.0f, Rect, TanX, TanY);
    Check(bOk, "fullscreen: there is a view");
    CheckNear(Rect[0], 0.0f, 1e-6f, "fullscreen: x");
    CheckNear(Rect[1], 0.0f, 1e-6f, "fullscreen: y");
    CheckNear(Rect[2], 1.0f, 1e-6f, "fullscreen: width");
    CheckNear(Rect[3], 1.0f, 1e-6f, "fullscreen: height");
    CheckNear(TanX, 1.0f, 1e-5f, "fullscreen, 90 degrees: TanX");
    CheckNear(TanY, 0.5625f, 1e-5f, "fullscreen, 90 degrees: TanY");
    // The spy drone's window as measured - 960x540 at XB 16, YB 810 - on a 3840x2160 output, at 90 degrees.
    bOk = FrameShadowView(16, 810, 960, 540, 3840, 2160, 480.0f, 270.0f, 480.0f, Rect, TanX, TanY);
    Check(bOk, "the drone's window: there is a view");
    CheckNear(Rect[0], 16.0f / 3840.0f, 1e-6f, "the drone's window: x");
    CheckNear(Rect[1], 810.0f / 2160.0f, 1e-6f, "the drone's window: y");
    CheckNear(Rect[2], 0.25f, 1e-6f, "the drone's window: width");
    CheckNear(Rect[3], 0.25f, 1e-6f, "the drone's window: height");
    CheckNear(TanY, 0.5625f, 1e-5f, "the drone's window: TanY from its own size");
    Check(!FrameShadowView(0, 0, 0, 2160, 3840, 2160, 0.0f, 1080.0f, 1920.0f, Rect, TanX, TanY), "an empty scene node: no view");
    Check(!FrameShadowView(0, 0, 3840, 2160, 3840, 2160, 1920.0f, 1080.0f, 0.0f, Rect, TanX, TanY), "Proj.Z is zero: no view");
}

// The exe's compatibility string with "Override high DPI scaling behavior - Application" in it (the renderer writes
// it when Windows scales the game window).
static void TestCompatLayers()
{
    using namespace RdxMath;
    char O[64];
    Check(CompatLayersForHighDpi("", O, 64) == CompatLayers_Changed && strcmp(O, "~ HIGHDPIAWARE") == 0,
          "compat: nothing set yet - the Compatibility tab's own string");
    Check(CompatLayersForHighDpi(nullptr, O, 64) == CompatLayers_Changed && strcmp(O, "~ HIGHDPIAWARE") == 0,
          "compat: no value at all");
    Check(CompatLayersForHighDpi("   ", O, 64) == CompatLayers_Changed && strcmp(O, "~ HIGHDPIAWARE") == 0,
          "compat: only spaces");
    Check(CompatLayersForHighDpi("~", O, 64) == CompatLayers_Changed && strcmp(O, "~ HIGHDPIAWARE") == 0,
          "compat: a lone tilde");
    Check(CompatLayersForHighDpi("HIGHDPIAWARE", O, 64) == CompatLayers_Same, "compat: already set, without the tilde");
    Check(CompatLayersForHighDpi("~ HIGHDPIAWARE", O, 64) == CompatLayers_Same, "compat: already set by the tab");
    Check(CompatLayersForHighDpi("~ highdpiaware", O, 64) == CompatLayers_Same, "compat: in any case");
    Check(CompatLayersForHighDpi("~ RUNASADMIN HIGHDPIAWARE", O, 64) == CompatLayers_Same,
          "compat: already set among others");
    Check(CompatLayersForHighDpi("~ DPIUNAWARE", O, 64) == CompatLayers_Changed && strcmp(O, "~ HIGHDPIAWARE") == 0,
          "compat: System becomes Application");
    Check(CompatLayersForHighDpi("~ GDIDPISCALING DPIUNAWARE", O, 64) == CompatLayers_Changed &&
          strcmp(O, "~ HIGHDPIAWARE") == 0, "compat: System (Enhanced) becomes Application");
    Check(CompatLayersForHighDpi("~ RUNASADMIN DPIUNAWARE HIGHDPIAWARE", O, 64) == CompatLayers_Changed &&
          strcmp(O, "~ RUNASADMIN HIGHDPIAWARE") == 0, "compat: of two contradicting words, Application stays");
    Check(CompatLayersForHighDpi("~ WIN8RTM RUNASADMIN", O, 64) == CompatLayers_Changed &&
          strcmp(O, "~ WIN8RTM RUNASADMIN HIGHDPIAWARE") == 0, "compat: the other settings stay, in their order");
    Check(CompatLayersForHighDpi("WINXPSP3", O, 64) == CompatLayers_Changed && strcmp(O, "WINXPSP3 HIGHDPIAWARE") == 0,
          "compat: a string without the tilde gets none");
    Check(CompatLayersForHighDpi("  ~   RUNASADMIN\t", O, 64) == CompatLayers_Changed &&
          strcmp(O, "~ RUNASADMIN HIGHDPIAWARE") == 0, "compat: spaces and tabs between the words");
    Check(CompatLayersForHighDpi("~ DPIUNAWAREX", O, 64) == CompatLayers_Changed &&
          strcmp(O, "~ DPIUNAWAREX HIGHDPIAWARE") == 0, "compat: only whole words are taken out");
    Check(CompatLayersForHighDpi("", O, 15) == CompatLayers_Changed && strcmp(O, "~ HIGHDPIAWARE") == 0,
          "compat: fits exactly");
    Check(CompatLayersForHighDpi("", O, 14) == CompatLayers_TooLong && O[0] == 0, "compat: one character short");
    Check(CompatLayersForHighDpi("~ RUNASADMIN", O, 20) == CompatLayers_TooLong && O[0] == 0,
          "compat: the other settings do not fit with it");

    // When the player is told to restart, and when the renderer keeps quiet.
    Check(DpiNoticeWanted(false, true, true, true), "dpi notice: written, read back, and scaled right now - say it");
    Check(!DpiNoticeWanted(true, false, false, true), "dpi notice: the setting was already there - silence");
    Check(!DpiNoticeWanted(false, false, false, true), "dpi notice: the write failed - silence");
    Check(!DpiNoticeWanted(false, true, false, true), "dpi notice: written but reads back differently - silence");
    Check(!DpiNoticeWanted(false, true, true, false), "dpi notice: nothing is scaled at the moment - silence");
}

// The shader cache's file: what is written back of the shaders it holds.
static void TestShaderCache()
{
    using namespace RdxMath;
    typedef unsigned long long U64;
    const U64          Day  = 24ull * 3600ull;
    const U64          Now  = 1000 * Day;
    const U64          Keep = 30 * Day;
    const unsigned int Head = 100;
    unsigned char      K[8];

    Check(ShaderCacheWritten(nullptr, nullptr, nullptr, 0, Now, Keep, 1000, Head, K) == 0,
          "shader cache: nothing held");
    {
        // Asked for in this process: written whatever their age, and whatever the file comes to.
        const U64           Asked[3]    = { 0, Now - 400 * Day, Now };
        const unsigned int  Size[3]     = { 5000, 5000, 5000 };
        const unsigned char AskedNow[3] = { 1, 1, 1 };
        Check(ShaderCacheWritten(Asked, Size, AskedNow, 3, Now, Keep, 1000, Head, K) == 3 && K[0] && K[1] && K[2],
              "shader cache: what this process asked for is written, old or over the size");
    }
    {
        // Not asked for now: the age decides, to the moment.
        const U64           Asked[5]    = { Now - Keep, Now - Keep - 1, Now - Day, Now + 5 * Day, 0 };
        const unsigned int  Size[5]     = { 10, 10, 10, 10, 10 };
        const unsigned char AskedNow[5] = { 0, 0, 0, 0, 0 };
        Check(ShaderCacheWritten(Asked, Size, AskedNow, 5, Now, Keep, 100000, Head, K) == 3,
              "shader cache: three of the five are young enough");
        Check(K[0] == 1, "shader cache: asked for exactly as long ago as is kept - written");
        Check(K[1] == 0, "shader cache: a moment longer ago - left out");
        Check(K[2] == 1, "shader cache: asked for yesterday - written");
        Check(K[3] == 1, "shader cache: a time ahead of now (the clock set back) - written");
        Check(K[4] == 0, "shader cache: never asked for in living memory - left out");
    }
    {
        // Over the size: the ones asked for longest ago go first, this process's own never.
        const U64           Asked[6]    = { Now - 5 * Day, Now - 9 * Day, Now - 2 * Day, Now - 9 * Day, Now - 20 * Day,
                                            Now - Day };
        const unsigned int  Size[6]     = { 900, 900, 900, 900, 900, 900 };
        const unsigned char AskedNow[6] = { 0, 0, 0, 0, 1, 0 };
        Check(ShaderCacheWritten(Asked, Size, AskedNow, 6, Now, Keep, 6000, Head, K) == 6,
              "shader cache: six shaders of a thousand bytes fit in six thousand");
        Check(ShaderCacheWritten(Asked, Size, AskedNow, 6, Now, Keep, 5999, Head, K) == 5 && K[1] == 0 && K[3] == 1,
              "shader cache: one byte short - of the two oldest, the earlier in the list goes");
        Check(K[4] == 1, "shader cache: this process's own stays though it is the oldest of all");
        Check(ShaderCacheWritten(Asked, Size, AskedNow, 6, Now, Keep, 4000, Head, K) == 4 && !K[1] && !K[3] && K[0],
              "shader cache: room for four - both of the oldest go, the five-day-old stays");
        Check(ShaderCacheWritten(Asked, Size, AskedNow, 6, Now, Keep, 2000, Head, K) == 2 && K[4] && K[5] &&
              !K[0] && !K[2], "shader cache: room for two - this process's own and the youngest");
        Check(ShaderCacheWritten(Asked, Size, AskedNow, 6, Now, Keep, 500, Head, K) == 1 && K[4] == 1 && K[5] == 0,
              "shader cache: room for none - this process's own is written all the same");
    }
    {
        // What a shader costs is its head and its bytes.
        const U64           Asked[2]    = { Now - Day, Now - 2 * Day };
        const unsigned int  Size[2]     = { 0, 0 };
        const unsigned char AskedNow[2] = { 0, 0 };
        Check(ShaderCacheWritten(Asked, Size, AskedNow, 2, Now, Keep, 199, Head, K) == 1 && K[0] && !K[1],
              "shader cache: two empty shaders are two heads long");
    }
}

// The frame rate limit: what the setting says, and what it comes to on a given screen.
static void TestFpsLimit()
{
    using namespace RdxMath;
    Check(ParseFpsLimit("Off", FpsLimit_Auto) == FpsLimit_Off, "fps: Off");
    Check(ParseFpsLimit("off", FpsLimit_Auto) == FpsLimit_Off, "fps: off, in any case");
    Check(ParseFpsLimit("False", FpsLimit_Auto) == FpsLimit_Auto, "fps: False is no word of the key - the default");
    Check(ParseFpsLimit("Auto", FpsLimit_Off) == FpsLimit_Auto, "fps: Auto");
    Check(ParseFpsLimit("  auto ", FpsLimit_Off) == FpsLimit_Auto, "fps: spaces around it");
    Check(ParseFpsLimit("60", FpsLimit_Auto) == 60, "fps: a number is that number");
    Check(ParseFpsLimit("144", FpsLimit_Auto) == 144, "fps: a bigger number");
    Check(ParseFpsLimit("3", FpsLimit_Auto) == 10, "fps: below ten nothing is playable");
    Check(ParseFpsLimit("5000", FpsLimit_Auto) == 1000, "fps: no display goes above a thousand");
    Check(ParseFpsLimit("", FpsLimit_Auto) == FpsLimit_Auto, "fps: an empty setting keeps the default");
    Check(ParseFpsLimit(nullptr, 60) == 60, "fps: no setting at all keeps the default");
    Check(ParseFpsLimit("nonsense", 60) == 60, "fps: unreadable keeps the default");

    Check(FpsLimitTarget(FpsLimit_Off, 60, Vrr_On) == 0, "fps target: Off limits nothing");
    Check(FpsLimitTarget(60, 144, Vrr_On) == 60, "fps target: a number is kept, whatever the screen does");
    Check(FpsLimitTarget(240, 60, Vrr_Off) == 240, "fps target: even above the refresh rate");
    Check(FpsLimitTarget(FpsLimit_Auto, 60, Vrr_Off) == 60, "fps target: Auto on a fixed screen is its refresh rate");
    Check(FpsLimitTarget(FpsLimit_Auto, 60, Vrr_On) == 57, "fps target: Auto with variable refresh stays three below");
    Check(FpsLimitTarget(FpsLimit_Auto, 144, Vrr_On) == 141, "fps target: three below, on a fast screen too");
    Check(FpsLimitTarget(FpsLimit_Auto, 0, Vrr_On) == 0, "fps target: with no refresh rate to go by, Auto limits nothing");
    Check(FpsLimitTarget(FpsLimit_Auto, 31, Vrr_On) == 30, "fps target: Auto never drops below thirty");
    Check(FpsLimitTarget(FpsLimit_Auto, 60, Vrr_NoData) == 57,
          "fps target: with no answer from any driver, Auto behaves as if the refresh varied");

    {
        // The limit's slots: a short frame waits out its period, a late one goes on at once - never a period more.
        const double P = 1.0 / 57.0;
        Check(NextFrameSlot(0.0, 5.0, P) == 5.0, "limit: the first frame starts at once");
        CheckNear((float)(NextFrameSlot(1.0, 1.010, P) - 1.0), (float)P, 1.0e-6f, "limit: a short frame waits out its period");
        Check(NextFrameSlot(1.0, 1.020, P) == 1.020, "limit: a frame later than its period goes on at once");
        Check(NextFrameSlot(1.0, 3.0, P) == 3.0, "limit: after a stall (a level load) at once, the count starting again");
        // A hundred frames of a given length, each starting at its slot or when the one before was done.
        const double Lengths[2] = { 0.020, 0.010 };
        double Mean[2];
        for (int k = 0; k < 2; k++)
        {
            double Slot = 0.0, Start = 0.0;
            for (int f = 0; f < 100; f++)
            {
                const double Now = Start + Lengths[k];
                Slot  = NextFrameSlot(Slot, Now, P);
                Start = (Slot > Now) ? Slot : Now;
            }
            Mean[k] = Start / 100.0;
        }
        CheckNear((float)Mean[0], 0.020f, 1.0e-4f, "limit: frames of 20 ms under 57 run at 50, as the card allows");
        CheckNear((float)Mean[1], (float)P, 1.0e-4f, "limit: frames of 10 ms are held to 57");
    }
}

static float TriangleArea(const float* A, const float* B, const float* C)
{
    const float U[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] };
    const float V[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
    const float X = U[1] * V[2] - U[2] * V[1], Y = U[2] * V[0] - U[0] * V[2], Z = U[0] * V[1] - U[1] * V[0];
    return 0.5f * sqrtf(X * X + Y * Y + Z * Z);
}

// Which textures are drawn as a volume of smoke instead of as the flat card the game sent. The rule is the GROUP -
// the game's own artists put every cloud in Effects.Smoke - and what has to be proved is BOTH halves of it: that the
// seven clouds are found, and that the things merely NAMED after smoke are not. A no-smoking sign drawn as a ball of
// gas would be the funnier failure, but the quiet one - a cloud the rule misses - is the one this would hide.
static void TestClouds()
{
    using namespace RdxMath;

    // Every texture the group holds, as the packages spell them (read out 2026-09-23).
    Check(IsFieldTexture("BobPage.BP_FX_02"), "force field: the cabinets' own");
    Check(IsFieldTexture("bobpage.bp_fx_02"), "force field: whatever the letters' case");
    Check(IsFieldTexture("BobPage.BP_FX_01") && IsFieldTexture("BobPage.BP_FX_03"),
          "force field: the large sheets' and the cells' doorways' too");
    Check(!IsFieldTexture("BobPage.BP_FX_00") && !IsFieldTexture("BobPage.BP_FX_04") &&
          !IsFieldTexture("BobPage.BP_FX_0"), "force field: the package's three, no others");
    Check(FieldKind("BobPage.BP_FX_01") == 1 && FieldKind("bobpage.bp_fx_02") == 2 && FieldKind("BobPage.BP_FX_03") == 3 &&
          FieldKind("BobPage.BP_FX_04") == 0 && FieldKind("BobPage.BP_FX") == 0,
          "force field: which of the three a texture is");
    Check(FieldBrightOf(2) == 0.5f && FieldBrightOf(1) == 0.3f && FieldBrightOf(3) == 0.3f && FieldBrightOf(0) == 0.0f,
          "force field: the cabinets' at a half, the sheets' and the doorways' at three tenths");
    Check(!IsFieldTexture("BobPage.BP_FX_021"), "force field: the whole name");
    Check(!IsFieldTexture("Effects.BobPage.BP_FX_02"), "force field: from the name's start");
    Check(IsCloudTexture("Effects.Smoke.Gas_Poison"), "cloud: poison gas");
    Check(IsCloudTexture("Effects.Smoke.Gas_Poison_A"), "cloud: poison gas, the moving one");
    Check(IsCloudTexture("Effects.Smoke.Gas_Tear"), "cloud: tear gas");
    Check(IsCloudTexture("Effects.Smoke.Gas_Tear_A"), "cloud: tear gas, the moving one");
    Check(IsCloudTexture("Effects.Smoke.SmokePuff"), "cloud: a puff of smoke");
    Check(IsCloudTexture("Effects.Smoke.SmokePuff1"), "cloud: a puff of smoke, the engine's own");
    Check(IsCloudTexture("Effects.Smoke.SmokeSpray1"), "cloud: sprayed smoke");
    // The engine spells names in wide characters; the rule has to hold for both.
    Check(IsCloudTexture(L"Effects.Smoke.Gas_Tear"), "cloud: the same name in wide characters");
    // And however the case comes out.
    Check(IsCloudTexture("EFFECTS.SMOKE.GAS_TEAR"), "cloud: case does not matter");

    // A puff is as thick through its middle whatever its size: the density times the path through it is the depth.
    {
        const float Sizes[5] = { 0.8f, 3.2f, 16.0f, 47.0f, 120.0f };
        for (int i = 0; i < 5; i++)
            CheckNear(CloudPuffDensity(Sizes[i], 1.0f, CloudPuffDepth) * CloudPuffPath * Sizes[i], CloudPuffDepth,
                      1e-4f, "cloud: a puff's middle is as thick whatever its size");
    }
    {
        const float Whole = CloudPuffDensity(10.0f, 1.0f, CloudPuffDepth);
        CheckNear(CloudPuffDensity(10.0f, 0.5f, CloudPuffDepth), 0.5f * Whole, 1e-6f,
                  "cloud: a card faded to a half is half the smoke");
        Check(CloudPuffDensity(10.0f, 0.0f, CloudPuffDepth) == 0.0f, "cloud: a card faded out is no smoke");
        Check(CloudPuffDensity(10.0f, -1.0f, CloudPuffDepth) == 0.0f, "cloud: a fade below nothing is nothing");
        Check(CloudPuffDensity(10.0f, 3.0f, CloudPuffDepth) == Whole, "cloud: a fade past one is one");
    }
    Check(CloudPuffDensity(0.0f, 1.0f, CloudPuffDepth) == CloudPuffDensity(0.25f, 1.0f, CloudPuffDepth),
          "cloud: a puff of no size is no thicker than the smallest");

    // The tiles a puff is listed in: a 1920x1080 target of 32-pixel tiles, the engine's picture twice that.
    {
        const float Screen[2] = { 1920.0f, 1080.0f };
        const float Per[2]    = { 2.0f, 2.0f };
        const float Focal     = 1920.0f;
        const int   TX = 60, TY = 34;
        int X0 = -1, Y0 = -1, X1 = -1, Y1 = -1;
        const float Ahead[3] = { 0.0f, 0.0f, 500.0f };
        Check(CloudPuffTiles(Ahead, 5.0f, Focal, Screen, Per, TX, TY, X0, Y0, X1, Y1) &&
              X0 == 29 && X1 == 30 && Y0 == 16 && Y1 == 17,
              "cloud tiles: a small puff straight ahead is in the middle four");
        const float AtEye[3] = { 3.0f, -2.0f, 20.0f };
        Check(CloudPuffTiles(AtEye, 30.0f, Focal, Screen, Per, TX, TY, X0, Y0, X1, Y1) &&
              X0 == 0 && Y0 == 0 && X1 == TX - 1 && Y1 == TY - 1,
              "cloud tiles: a puff the eye stands in is in every tile");
        const float Behind[3] = { 0.0f, 0.0f, -100.0f };
        Check(!CloudPuffTiles(Behind, 30.0f, Focal, Screen, Per, TX, TY, X0, Y0, X1, Y1),
              "cloud tiles: a puff behind the eye is in none");
        const float Aside[3] = { 900.0f, 0.0f, 500.0f };
        Check(!CloudPuffTiles(Aside, 20.0f, Focal, Screen, Per, TX, TY, X0, Y0, X1, Y1),
              "cloud tiles: a puff off the side of the screen is in none");
        const float Edge[3] = { 499.0f, 0.0f, 500.0f };
        Check(CloudPuffTiles(Edge, 20.0f, Focal, Screen, Per, TX, TY, X0, Y0, X1, Y1) && X1 == TX - 1 && X0 < TX - 1,
              "cloud tiles: a puff at the screen's edge ends at the last tile");
        // Never fewer tiles than the ball covers: every point of it, projected, lies in the rectangle.
        const float Ball[3] = { -120.0f, 60.0f, 300.0f };
        const float Reach   = 45.0f;
        bool bIn = CloudPuffTiles(Ball, Reach, Focal, Screen, Per, TX, TY, X0, Y0, X1, Y1);
        for (int i = 0; i < 400 && bIn; i++)
        {
            const float U = (float)(i % 20) / 19.0f * 6.2831853f, V = (float)(i / 20) / 19.0f * 3.1415927f;
            const float P[3] = { Ball[0] + Reach * (float)(sin(V) * cos(U)), Ball[1] + Reach * (float)(sin(V) * sin(U)),
                                 Ball[2] + Reach * (float)cos(V) };
            const float Px = (Screen[0] + Focal * P[0] / P[2]) / Per[0];
            const float Py = (Screen[1] + Focal * P[1] / P[2]) / Per[1];
            const int   Tx = (int)(Px / (float)CloudTile), Ty = (int)(Py / (float)CloudTile);
            if (Px >= 0.0f && Py >= 0.0f && Tx < TX && Ty < TY)
                bIn = Tx >= X0 && Tx <= X1 && Ty >= Y0 && Ty <= Y1;
        }
        Check(bIn, "cloud tiles: every point of the ball lies in the tiles it is listed in");
    }

    // The lists by tile: every tile's list holds exactly the puffs whose rectangle covers it.
    {
        enum { TX = 7, TY = 5, PUFFS = 40 };
        int Rects[PUFFS * 4];
        unsigned Seed = 4242u;
        auto Next = [&Seed](int Below) -> int
        {
            Seed = Seed * 1664525u + 1013904223u;
            return (int)((Seed >> 10) % (unsigned)Below);
        };
        for (int i = 0; i < PUFFS; i++)
        {
            int* const R = Rects + i * 4;
            R[0] = Next(TX); R[2] = R[0] + Next(TX - R[0]);
            R[1] = Next(TY); R[3] = R[1] + Next(TY - R[1]);
            if (i % 9 == 4)
            {
                R[0] = R[1] = 0;
                R[2] = R[3] = -1;       // a puff that shows in no tile
            }
        }
        const int Numbers = CloudTileListsSize(Rects, PUFFS, TX, TY);
        static unsigned int Lists[TX * TY * 2 + PUFFS * TX * TY];
        const int Most = CloudTileListsFill(Rects, PUFFS, TX, TY, Lists);
        bool bRight = true;
        int  Longest = 0, End = TX * TY * 2;
        for (int t = 0; t < TX * TY; t++)
        {
            const int x = t % TX, y = t / TX;
            const unsigned First = Lists[t * 2], Held = Lists[t * 2 + 1];
            bRight  = bRight && (int)First == End;      // the lists lie one after another, in the tiles' order
            End    += (int)Held;
            Longest = ((int)Held > Longest) ? (int)Held : Longest;
            int Want = 0;
            for (int i = 0; i < PUFFS; i++)
            {
                const int* const R = Rects + i * 4;
                const bool bCovers = R[2] >= R[0] && x >= R[0] && x <= R[2] && y >= R[1] && y <= R[3];
                bool bListed = false;
                for (unsigned k = 0; k < Held; k++)
                    bListed = bListed || Lists[First + k] == (unsigned)i;
                bRight = bRight && bCovers == bListed;
                Want  += bCovers ? 1 : 0;
            }
            bRight = bRight && Want == (int)Held;
        }
        Check(bRight, "cloud tile lists: each tile lists the puffs over it, each once, and no other");
        Check(End == Numbers, "cloud tile lists: as many numbers as were counted");
        Check(Most == Longest, "cloud tile lists: the longest list is told");
        Check(CloudTileListsSize(Rects, 0, TX, TY) == TX * TY * 2, "cloud tile lists: no puffs, only the tiles' table");
    }

    // The lists' first room: the walk reads, at every pixel of its target, the two numbers of the pixel's tile - so the
    // room holds them for every tile before there is a single puff, at any size of target.
    {
        const int Least = 16384;
        const int Sizes[][2] = { { 1, 1 }, { 32, 32 }, { 33, 65 }, { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 },
                                 { 5760, 3240 }, { 7680, 4320 } };
        bool bHeld = true, bLeast = true;
        for (int s = 0; s < (int)(sizeof(Sizes) / sizeof(Sizes[0])); s++)
        {
            const int W = Sizes[s][0], H = Sizes[s][1];
            const int Room = CloudTileFirstRoom(W, H, Least);
            // The last pixel's tile, as the walk finds it: its second number is the furthest it reads.
            const int Last = 2 * ((W - 1) / CloudTile + ((H - 1) / CloudTile) * CloudTilesAlong(W)) + 1;
            bHeld  = bHeld && Last < Room;
            bLeast = bLeast && Room >= Least &&
                     Room >= CloudTileListsSize(nullptr, 0, CloudTilesAlong(W), CloudTilesAlong(H));
        }
        Check(bHeld, "cloud tile room: the last pixel's tile is inside the first room at every size");
        Check(bLeast, "cloud tile room: never under the least, never under the tiles' own table");
        Check(CloudTilesAlong(1920) == 60 && CloudTilesAlong(1080) == 34 && CloudTilesAlong(1) == 1 &&
              CloudTilesAlong(0) == 0, "cloud tiles: a side's tiles, the last one whole or not");
        Check(CloudTileFirstRoom(1920, 1080, Least) == Least, "cloud tile room: a 4K picture's walk fits the least");
        Check(CloudTileFirstRoom(3840, 2160, Least) == Least, "cloud tile room: and so does twice that a side");
        Check(CloudTileFirstRoom(5760, 3240, Least) == 36720, "cloud tile room: three times a side needs more");
        Check(CloudTileFirstRoom(7680, 4320, Least) == 64800, "cloud tile room: four times a side needs more still");
    }

    // Named after smoke, and not smoke. These are the ones a list of NAMES would have caught by mistake.
    Check(!IsCloudTexture("CoreTexMisc.Metal.DangerNOsmoke_A"), "cloud: a no-smoking sign is not a cloud");
    Check(!IsCloudTexture("NewYorkCity.Signs.HolySmokesBB"), "cloud: a billboard is not a cloud");
    Check(!IsCloudTexture("Palettes.Grey.Jsmoke1"), "cloud: a palette is not a cloud");
    Check(!IsCloudTexture("Palettes.Misc.Smokey"), "cloud: nor is that one");
    // Fire is a volume of its own (TestFlames): it has its own light, which smoke has not.
    Check(!IsCloudTexture("Effects.Fire.Flame_J"), "cloud: fire is not smoke");
    Check(!IsCloudTexture("Effects.Smoke"), "cloud: the group alone is not a texture in it");
    Check(!IsCloudTexture(""), "cloud: an empty name");
    Check(!IsCloudTexture((const char*)0), "cloud: no name at all");
}

// Which of a liquid zone's borders are its surface.
static void TestWaterSurface()
{
    using namespace RdxMath;
    Check(WaterBorderIsSurface(1.0f, true), "water surface: a level border with the liquid behind an upward plane");
    Check(WaterBorderIsSurface(-1.0f, false), "water surface: or in front of a downward one - the same pool, seen so");
    Check(!WaterBorderIsSurface(1.0f, false), "water surface: not a hatch in the floor of a flooded room");
    Check(!WaterBorderIsSurface(-1.0f, true), "water surface: whichever way its plane was made");
    Check(!WaterBorderIsSurface(0.0f, true) && !WaterBorderIsSurface(0.0f, false),
          "water surface: not an upright pane between the water and a room");
    Check(!WaterBorderIsSurface(0.9f, true), "water surface: nor a sloping one");
    Check(WaterBorderIsSurface(0.995f, true), "water surface: a river laid a hair off level is still one");
    Check(WaterSurfaceStep > 4.0f && WaterSurfaceStep < 100.0f,
          "water surface: a river's pieces a few units apart are one surface, two basins are two");
    // A lamp across the water's surface.
    CheckNear(WaterLampThrough(1.0f, false), 0.98f, 1e-4f, "lamp through water: straight down, all but a fiftieth");
    CheckNear(WaterLampThrough(1.0f, true), 0.98f, 1e-4f, "lamp through water: and straight up from under it");
    Check(WaterLampThrough(0.0f, false) < 0.01f, "lamp through water: along the surface it gives all of it back");
    Check(WaterLampThrough(0.6f, true) == 0.0f,
          "lamp through water: from below, past the window, the surface is a mirror");
    Check(WaterLampThrough(0.7f, true) > 0.0f && WaterLampThrough(0.7f, true) < WaterLampThrough(0.9f, true),
          "lamp through water: inside the window it comes, less towards the window's rim");
    Check(WaterLampThrough(-0.9f, true) == WaterLampThrough(0.9f, true),
          "lamp through water: whichever way the line runs");
    // Liquid zones that touch are one water.
    {
        int Body[8];
        WaterBodiesBegin(Body, 8);
        Check(WaterBodyOf(Body, 8, 5) == 5, "water bodies: a zone alone is its own water");
        WaterBodiesJoin(Body, 8, 4, 6);
        WaterBodiesJoin(Body, 8, 6, 1);
        Check(WaterBodyOf(Body, 8, 4) == 1 && WaterBodyOf(Body, 8, 6) == 1 && WaterBodyOf(Body, 8, 1) == 1,
              "water bodies: zones that touch one through another are named by the least of them");
        WaterBodiesJoin(Body, 8, 5, 7);
        WaterBodiesJoin(Body, 8, 7, 3);
        WaterBodiesJoin(Body, 8, 2, 3);
        Check(WaterBodyOf(Body, 8, 7) == 2 && WaterBodyOf(Body, 8, 5) == 2 && WaterBodyOf(Body, 8, 3) == 2,
              "water bodies: whichever order they were joined in");
        Check(WaterBodyOf(Body, 8, 4) == 1 && WaterBodyOf(Body, 8, 0) == 0,
              "water bodies: and another water stays another");
        WaterBodiesJoin(Body, 8, 3, 9);
        WaterBodiesJoin(Body, 8, -1, 3);
        Check(WaterBodyOf(Body, 8, 3) == 2 && WaterBodyOf(Body, 8, 9) == 9,
              "water bodies: a zone the list does not hold joins nothing");
    }
    // A mirror of several surfaces is drawn once a surface: the same view again, which goes on as one.
    {
        const float View[17] = { 100.0f, 200.0f, -1783.9f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f,
                                 2500.0f, 1920.0f, 1080.0f, 3840.0f, 2160.0f };
        const float Plane[4] = { 0.0f, 1.0f, 0.0f, 87.9f };
        float Other[17], OtherPlane[4];
        for (int i = 0; i < 17; i++)
            Other[i] = View[i];
        for (int i = 0; i < 4; i++)
            OtherPlane[i] = Plane[i];
        Check(MirrorViewsAlike(View, Plane, Other, OtherPlane), "mirror views: the same view again is one picture");
        Other[2] += 0.01f;
        Check(MirrorViewsAlike(View, Plane, Other, OtherPlane), "mirror views: and so is one a rounding away");
        Other[2] = View[2] + 2.0f;
        Check(!MirrorViewsAlike(View, Plane, Other, OtherPlane), "mirror views: another eye is another picture");
        Other[2]      = View[2];
        OtherPlane[3] = Plane[3] + 1.0f;
        Check(!MirrorViewsAlike(View, Plane, Other, OtherPlane), "mirror views: so is another plane");
        OtherPlane[3] = Plane[3];
        Other[9]      = 0.01f;
        Check(!MirrorViewsAlike(View, Plane, Other, OtherPlane), "mirror views: and another way of looking");
        Other[9]  = View[9];
        Other[15] = 1920.0f;
        Check(!MirrorViewsAlike(View, Plane, Other, OtherPlane), "mirror views: and another part of the screen");
    }
    // Which side of its mirror's plane a polygon of a mirror's view is kept on.
    {
        Check(MirrorKeptSide(320.0f, 4, 87.9f) == 1.0f && MirrorKeptSide(-320.0f, 4, 87.9f) == -1.0f,
              "mirror cut: the side the engine's own piece lies on");
        Check(MirrorKeptSide(0.001f, 7, 87.9f) == 1.0f && MirrorKeptSide(-0.02f, 7, 87.9f) == 1.0f,
              "mirror cut: a piece in the plane itself tells none - the side the eye is not on");
        Check(MirrorKeptSide(0.0f, 7, -87.9f) == -1.0f, "mirror cut: whichever way the plane's normal looks");
        Check(MirrorKeptSide(0.0f, 7, 0.0f) == 0.0f, "mirror cut: and with the eye in the plane neither tells");
        Check(MirrorKeptSide(-5.0f, 4, 87.9f) == -1.0f, "mirror cut: a piece that does tell is believed");
    }
    // The glows that are no flame, and a card of one as a ball of its own colour.
    Check(GlowName("Effects.Fire.Spark_Electric") && GlowName("effects.fire.PROJ_PRIFLE"),
          "glow name: the electric sparks' puffs and what a plasma bolt throws");
    Check(GlowName("Effects.Fire.flmethrwr_flme") && !GlowName("Effects.Fire.flmethrwr_fire"),
          "glow name: the plasma's fire where a bolt struck - not the flamethrower's stream");
    {
        float Keep = 0.0f;
        CheckNear(GlowWallPush(40.0f, 10.0f, Keep), 0.0f, 1e-5f, "glow at a wall: one that stands clear stays");
        CheckNear(Keep, 1.0f, 1e-5f, "glow at a wall: ... with all its light");
        CheckNear(GlowWallPush(0.0f, 10.0f, Keep), GlowWallClear * 10.0f + GlowWallRelief, 1e-5f,
                  "glow at a wall: one with its middle on the wall is moved clear of it");
        CheckNear(Keep, 1.0f, 1e-5f, "glow at a wall: ... and keeps its light");
        CheckNear(GlowWallPush(-5.0f, 10.0f, Keep), GlowWallClear * 10.0f + GlowWallRelief + 5.0f, 1e-5f,
                  "glow at a wall: one half sunk in it comes out as far, from where it lies");
        CheckNear(Keep, 0.5f, 1e-5f, "glow at a wall: ... with half its light");
        CheckNear(GlowWallPush(-10.0f, 10.0f, Keep), 0.0f, 1e-5f, "glow at a wall: one wholly inside is left there");
        CheckNear(Keep, 0.0f, 1e-5f, "glow at a wall: ... and gives no light");
    }
    {
        float Thin = 0.0f, High = 0.0f;
        Check(!GlowTongue(56.0f, 56.0f, Thin, High), "glow tongue: a card as wide as it is tall is a ball's");
        Check(GlowTongue(52.0f, 104.0f, Thin, High), "glow tongue: one twice as tall as it is wide is a tongue");
        CheckNear(Thin, 0.45f * 52.0f, 1e-4f, "glow tongue: as thin as its card, a little less");
        CheckNear(High, 0.45f * 104.0f, 1e-4f, "glow tongue: ... and as tall");
        Check(!GlowTongue(104.0f, 52.0f, Thin, High), "glow tongue: a wide, flat card is none");
        // How white a glow's densest part may turn rides on the piece, and on the body made of it.
        const float Lime[3] = { 0.37f, 1.0f, 0.08f };
        const FFlamePiece Ball = FlameGlowPiece(0.0f, 0.0f, 0.0f, 20.0f, 20.0f, 0.5f, 1.0f, 100.0f, Lime, 0.07f);
        float One[FlameBodyFloats];
        int   Group[1];
        Check(FlameBodiesMake(&Ball, 1, One, 1, 0.01f, Group) == 1, "glow white: a ball of it is a body");
        CheckNear(One[15], 0.07f, 1e-5f, "glow white: a green picture's ball turns hardly towards white at all");
        const FFlamePiece Bolt2 = FlameGlowMeshPiece(0.0f, 0.0f, 0.0f, 20.0f, 4.0f, 4.0f, 100.0f, Lime, 0.07f);
        Check(FlameBodiesMake(&Bolt2, 1, One, 1, 0.01f, Group) == 1 && One[15] > 0.069f && One[15] < 0.071f,
              "glow white: and so a mesh of it");
    }
    Check(!GlowName("Effects.Fire.flame_b") && !GlowName("Effects.Fire.Wepn_Prifle_SFX") &&
          !GlowName("Effects.Fire.Spark_Electric2") && !GlowName((const char*)0),
          "glow name: no flame, not the rifle's own glow, and nothing merely named alike");
    Check(FlameKind("Effects.Fire.Spark_Electric") == Flame_None && FlameKind("Effects.Fire.proj_Prifle") == Flame_None,
          "glow name: a glow is no flame to the fires' rules");
    {
        const float Blue[3] = { 0.4f, 0.6f, 1.0f };
        const FFlamePiece G = FlameGlowPiece(10.0f, 20.0f, 30.0f, 16.0f, 12.0f, 0.5f, 0.8f, 200.0f, Blue);
        Check(G.Kind == FlameShape_Glow && G.bRound != 0, "glow piece: a round piece of its own kind");
        CheckNear(G.Half, 5.6f, 1e-4f, "glow piece: wider than the picture on the card by what the walk fades");
        CheckNear(G.Bottom, 24.4f, 1e-4f, "glow piece: a ball round the card's middle - its foot");
        CheckNear(G.Top, 35.6f, 1e-4f, "glow piece: ... and its top");
        const FFlamePiece F = FlameCardPiece(Flame_Ball, 80.0f, 20.0f, 30.0f, 16.0f, 12.0f, 1.0f, 200.0f);
        const FFlamePiece Two[2] = { G, F };
        float Bodies[2 * FlameBodyFloats];
        int   Groups[2];
        Check(FlameBodiesMake(Two, 2, Bodies, 2, 0.01f, Groups) == 2, "glow piece: a body of its own beside a flame");
        CheckNear(Bodies[8], 0.4f, 1e-4f, "glow body: its picture's colour - red");
        CheckNear(Bodies[10], 1.0f, 1e-4f, "glow body: ... and blue");
        CheckNear(Bodies[12], (float)FlameShape_Glow, 1e-4f, "glow body: walked as a ball of glow");
        CheckNear(Bodies[13], 1.0f, 1e-4f, "glow body: as dense as any - the game's fade is its strength");
        CheckNear(Bodies[11], 0.8f, 1e-4f, "glow body: no seed - the game's fade of its card, for its discharges");
        CheckNear(Bodies[FlameBodyFloats + 11], 0.0f, 1e-4f, "glow body: the flame beside it keeps its own seed");
        CheckNear(Bodies[14], FireGlowThrough * 0.8f / 5.6f, 1e-4f,
                  "glow body: as strong as the game's fade of its card, and less by the unit the larger it is");
        CheckNear(Bodies[FlameBodyFloats + 8], FireTintR, 1e-4f, "glow body: the flame beside it keeps its tint");
        CheckNear(Bodies[FlameBodyFloats + 9], FireTintG, 1e-4f, "glow body: ... all of it");
        CheckNear(Bodies[FlameBodyFloats + 14], 1.0f, 1e-4f, "glow body: ... and its own strength");
        float Mid[3];
        const float Reach = FlameBodyBall(Bodies, false, Mid);
        Check(Reach >= Bodies[6] && Reach < 1.05f * Bodies[6] + 0.1f,
              "glow body: the ball its tiles are worked out from is its own, and a hair more");
        CheckNear(Mid[2], 30.0f, 1e-4f, "glow body: ... round its middle");
        const float Long[FlameBodyFloats] = { 0.0f, 0.0f, 10.0f, 16.0f, 1.0f, 0.0f, 20.0f, 3.0f, 0.0f, 1.0f, 0.0f,
                                              0.0f, (float)FlameShape_Glow, 1.0f, 1.0f, 1.0f };
        Check(FlameBodyBall(Long, false, Mid) >= 20.0f && FlameBodyBall(Long, false, Mid) < 21.0f,
              "glow body: a drawn-out one's ball is as far out as its longest half");
    }
    // Balls of glow all but inside one another are one ball, the brighter of them.
    {
        const float Blue[3] = { 0.4f, 0.6f, 1.0f };
        FFlamePiece  Held[8];
        FFlameGather Gather;
        Gather.Hold(Held, 8);
        Gather.Begin();
        Check(Gather.Add(FlameGlowPiece(0.0f, 0.0f, 0.0f, 20.0f, 20.0f, 0.5f, 0.4f, 100.0f, Blue)) &&
              Gather.Add(FlameGlowPiece(1.5f, 0.0f, 1.0f, 22.0f, 22.0f, 0.5f, 0.9f, 100.0f, Blue)) && Gather.Count == 1,
              "glow gather: a ball all but inside another is that other ball");
        CheckNear(Held[0].Bright, 0.9f, 1e-4f, "glow gather: as bright as the brighter of the two");
        CheckNear(Held[0].Half, 7.0f, 1e-4f, "glow gather: where and as large as the first");
        Check(Gather.Add(FlameGlowPiece(5.0f, 0.0f, 0.0f, 20.0f, 20.0f, 0.5f, 0.5f, 100.0f, Blue)) && Gather.Count == 2,
              "glow gather: one further off than a third of its half-width is a ball of its own");
        Check(Gather.Add(FlameGlowPiece(0.5f, 0.0f, 0.0f, 40.0f, 40.0f, 0.5f, 0.5f, 100.0f, Blue)) && Gather.Count == 3,
              "glow gather: and so is one twice as wide, wherever it lies");
        const float Green[3] = { 0.3f, 1.0f, 0.4f };
        Check(Gather.Add(FlameGlowPiece(0.2f, 0.0f, 0.0f, 20.0f, 20.0f, 0.5f, 0.5f, 100.0f, Green)) &&
              Gather.Count == 4, "glow gather: and one of another colour");
    }
    // A mesh of glow: its polygons that touch are one body, as large as all of them.
    Check(GlowMeshName("Effects.Fire.OneFlame_G") && !GlowMeshName("Effects.Fire.OneFlame_J") &&
          !GlowName("Effects.Fire.OneFlame_G") && FlameKind("Effects.Fire.OneFlame_G") == Flame_None,
          "glow mesh name: the plasma bolt's skin, no card's and no flame");
    {
        const float Green[3] = { 0.3f, 1.0f, 0.4f };
        const FFlamePiece Bolt[3] = { FlameGlowMeshPiece(0.0f, 0.0f, 10.0f, 20.0f, 4.0f, 14.0f, 300.0f, Green),
                                      FlameGlowMeshPiece(18.0f, 0.0f, 10.0f, 40.0f, 6.0f, 16.0f, 300.0f, Green),
                                      FlameGlowMeshPiece(400.0f, 0.0f, 10.0f, 420.0f, 4.0f, 14.0f, 300.0f, Green) };
        float Bodies[3 * FlameBodyFloats];
        int   Groups[3];
        Check(FlameBodiesMake(Bolt, 3, Bodies, 3, 0.01f, Groups) == 2,
              "glow mesh: the polygons that touch are one body, one far off another");
        const float* const One = (Bodies[6] > 15.0f) ? Bodies : Bodies + FlameBodyFloats;
        CheckNear(One[0], 20.0f, 1e-3f, "glow mesh: the body's middle is the middle of all its polygons");
        CheckNear(One[6], 20.0f, 1e-3f, "glow mesh: as long as all of them");
        CheckNear(One[7], 3.0f, 1e-3f, "glow mesh: ... and as wide");
        CheckNear(One[2], 10.0f, 1e-3f, "glow mesh: its foot the lowest of them");
        CheckNear(One[3], 16.0f, 1e-3f, "glow mesh: ... and its top the highest");
        CheckNear(One[12], (float)FlameShape_Glow, 1e-4f, "glow mesh: walked as a body of glow");
        CheckNear(One[9], 1.0f, 1e-4f, "glow mesh: of its picture's colour");
        CheckNear(One[11], 1.0f, 1e-4f, "glow mesh: no seed - the fade of it, which a mesh has none of");
    }
    // A waterfall's sheet by its texture's name: the game's own textures.
    Check(FallNameKind("WtrFall_A01") == FallName_Falling, "fall name: the game's waterfall");
    Check(FallNameKind("drtywater_a") == FallName_Water,
          "fall name: water running down a wall is named for water only");
    Check(FallNameKind("bluewater_a") == FallName_Water, "fall name: and so is a tank's side");
    Check(FallNameKind("AtmPlusEmpty2") == FallName_None && FallNameKind("CC_kernel2") == FallName_None,
          "fall name: a computer room's scrolling screens are named for no water");
    Check(FallNameKind("Cascade01") == FallName_Falling && FallNameKind("WATERSPOUT") == FallName_Falling,
          "fall name: a cascade and a spout fall, in any case");
    Check(FallNameKind("Fountain_FX") == FallName_Falling, "fall name: an ornamental fountain's sheet falls");
    Check(IsFountainName("Fountain_FX") && IsFountainName(L"FOUNTAIN_fx"), "fountain: by its name, in any case");
    Check(!IsFountainName("WtrFall_A01") && !IsFountainName((const char*)0),
          "fountain: not a stream's fall, and not no name");
    // A fountain's pool: the level surface at the height of the curtain's lowest edge, under that edge's middle.
    Check(FountainFootInPool(-480.0f, 799.0f, -152.0f, -536.0f, 776.0f, -424.0f, 816.0f, -152.0f),
          "fountain pool: the surface under the curtain's foot");
    Check(FountainFootInPool(-1200.0f, -97.0f, -137.0f, -1256.0f, -120.0f, -1144.0f, -80.0f, -136.0f),
          "fountain pool: a foot a little under the surface's height");
    Check(!FountainFootInPool(-480.0f, 799.0f, -152.0f, -1256.0f, -120.0f, -1144.0f, -80.0f, -136.0f),
          "fountain pool: not another fountain's");
    Check(!FountainFootInPool(-480.0f, 799.0f, -152.0f + FountainPoolReach + 1.0f, -536.0f, 776.0f, -424.0f, 816.0f,
                              -152.0f),
          "fountain pool: not a surface well under the foot");
    Check(!FountainFootInPool(-424.0f + FountainPoolMargin + 1.0f, 799.0f, -152.0f, -536.0f, 776.0f, -424.0f, 816.0f,
                              -152.0f),
          "fountain pool: not a surface the foot's middle is beside");
    Check(FountainPoolCarriesOn(420.0f, -1182.0f, 476.0f, -1164.0f, -12.0f, 420.0f, -1204.0f, 476.0f, -1182.0f, -12.0f),
          "fountain pool: the basin's next piece carries it on");
    Check(!FountainPoolCarriesOn(420.0f, -1182.0f, 476.0f, -1164.0f, -4.0f, 420.0f, -1204.0f, 476.0f, -1182.0f, -12.0f),
          "fountain pool: not a surface at another height");
    Check(!FountainPoolCarriesOn(420.0f, -900.0f, 476.0f, -880.0f, -12.0f, 420.0f, -1204.0f, 476.0f, -1182.0f, -12.0f),
          "fountain pool: not a surface apart from it");
    Check(FountainPoolSheet < 0.0f, "fountain pool: its mark is below zero");
    // A fountain's chute: the lying pieces its water runs down to the slot.
    Check(FountainChuteSheet < FountainPoolSheet - 0.5f, "fountain chute: its mark is apart from a pool's");
    Check(fabsf(ChuteSpeed(0.0f) - ChuteLevelSpeed) < 1e-4f && ChuteSpeed(-1.0f) == ChuteSpeed(0.0f),
          "fountain chute: a level piece's water goes at the level's speed");
    Check(ChuteSpeed(0.2f) > ChuteSpeed(0.0f) && ChuteSpeed(0.43f) > ChuteSpeed(0.2f),
          "fountain chute: the steeper the piece, the faster its water");
    Check(fabsf(ChuteLength(40.0f, 0.2f) - 40.792f) < 0.01f && fabsf(ChuteLength(25.0f, 0.0f) - 25.0f) < 1e-4f &&
          ChuteLength(-3.0f, 0.2f) == 0.0f,
          "fountain chute: a sloping piece holds more film than it is long on the level");
    Check(fabsf(ChuteTime(25.0f, 0.0f) - 25.0f / ChuteLevelSpeed) < 1e-4f &&
          fabsf(ChuteTime(40.0f, 0.2f) * ChuteSpeed(0.2f) - ChuteLength(40.0f, 0.2f)) < 1e-3f,
          "fountain chute: the time over a piece is its film at its speed");
    // The metro station's lower fountain: the last slope ends at the curtain's top, the level stretch at the
    // slope's upper edge, the first slope at the level stretch.
    Check(ChuteStartsAt(-1.0f, 432.0f, -1207.0f, 464.0f, -1189.0f, -1.0f, 448.0f, -1189.0f),
          "fountain chute: the piece that ends at the curtain's top starts the chute");
    Check(!ChuteStartsAt(7.0f, 432.0f, -1232.0f, 464.0f, -1207.0f, -1.0f, 448.0f, -1189.0f) &&
          !ChuteStartsAt(-1.0f, 432.0f, -1207.0f, 464.0f, -1189.0f, -1.0f, 448.0f, -1100.0f),
          "fountain chute: not a piece at another height, nor one the top's middle is not over");
    Check(ChuteCarriesOn(7.0f, 432.0f, -1232.0f, 464.0f, -1207.0f, 7.0f, 432.0f, -1207.0f, 464.0f, -1189.0f) &&
          ChuteCarriesOn(7.0f, 432.0f, -1272.0f, 464.0f, -1232.0f, 7.0f, 432.0f, -1232.0f, 464.0f, -1207.0f),
          "fountain chute: the piece that ends where the one below begins carries it on");
    Check(!ChuteCarriesOn(7.0f, 432.0f, -1272.0f, 464.0f, -1232.0f, 7.0f, 432.0f, -1207.0f, 464.0f, -1189.0f) &&
          !ChuteCarriesOn(15.0f, 432.0f, -1232.0f, 464.0f, -1207.0f, 7.0f, 432.0f, -1207.0f, 464.0f, -1189.0f),
          "fountain chute: not a piece apart from it, nor one that begins higher up");
    Check(CurtainAmount > 0.0f && CurtainAmount < 1.0f, "fountain drops: between the thinnest and the fullest");
    // A fountain's drops: a drop let go at the start's speed is at the pool after the fall's time, and no sooner.
    {
        const float Rise = 42.0f, T = CurtainFallTime(Rise);
        const float Fell = CurtainStartSpeed * T + 0.5f * CurtainGravity * T * T;
        Check(fabsf(Fell - Rise) < 0.01f, "fountain drops: the fall's time takes a drop down to the pool");
        Check(CurtainFallTime(11.0f) < T && CurtainFallTime(0.0f) > 0.0f, "fountain drops: a lower fall is shorter");
        // The solid pass's flame body has these written out (SolidPass.hlsl, BODY_REACH and the two after it).
        Check(FireWalkReach == 1.8f && FireBallOut == 1.45f && FireBallTall == 1.3f,
              "a flame body's reach, written twice");
        Check(CurtainDrops == CurtainStreams * (CurtainStreamDrops + ChuteStreamDrops) + CurtainSpray,
              "fountain drops: the count's parts");
        Check((124 + CurtainMax * 12) * 4 <= 1024, "fountain drops: the fountains fit the splashes' constants");
    }
    Check(FallNameKind(L"WtrFall_A01") == FallName_Falling, "fall name: in the engine's wide letters too");
    Check(FallNameKind("") == FallName_None && FallNameKind((const char*)0) == FallName_None, "fall name: no name");
    Check(FallSheetByName(FallName_Falling, false) && FallSheetByName(FallName_Falling, true),
          "fall sheet: a waterfall's texture is a fall, running down or not");
    Check(FallSheetByName(FallName_Water, true), "fall sheet: water that the level runs down is a fall");
    Check(!FallSheetByName(FallName_Water, false), "fall sheet: water standing still is a tank's side");
    Check(!FallSheetByName(FallName_None, true), "fall sheet: a screen that runs down is not water");
    Check(TextHas("Effects.water.Splash_A01", "splash") && !TextHas("Effects.water.Splash_A01", "foam"),
          "text: a word is found anywhere in a name, and only where it is");
    {
        const float At[2]   = { 0.0f, 0.0f };
        const float Near[2] = { 20.0f, 0.0f };
        const float Far[2]  = { 40.0f, 0.0f };
        Check(FallsJoin(At, 32.0f, 0.0f, 200.0f, At, 32.0f, 0.0f, 200.0f), "falls join: a fall's crossed sheets");
        Check(FallsJoin(At, 32.0f, 0.0f, 200.0f, Near, 32.0f, 0.0f, 200.0f),
              "falls join: sheets on one another's middle");
        Check(!FallsJoin(At, 32.0f, 0.0f, 200.0f, Far, 32.0f, 0.0f, 200.0f), "falls join: not the fall beside it");
        Check(!FallsJoin(At, 32.0f, 0.0f, 200.0f, At, 32.0f, 190.0f, 400.0f), "falls join: nor the one above it");
    }
    // A level's fall as a column of water.
    {
        auto Sheet = [](float X, float Y, float Wide, float Angle, float Bottom, float Top) -> FFallSheetPiece
        {
            FFallSheetPiece P;
            P.Mid[0]    = X;
            P.Mid[1]    = Y;
            P.Across[0] = cosf(Angle);
            P.Across[1] = sinf(Angle);
            P.Half      = 0.5f * Wide;
            P.Bottom    = Bottom;
            P.Top       = Top;
            P.Hue[0]    = 0.1f;
            P.Hue[1]    = 0.2f;
            P.Hue[2]    = 0.3f;
            P.bFoot     = 0;
            return P;
        };
        // The canal's fall as the level has it: four sheets crossing round one spot, 448 units tall.
        FFallSheetPiece Canal[4] = { Sheet(-230.0f, -612.0f, 41.0f, 1.5708f, -1280.0f, -832.0f),
                                     Sheet(-215.0f, -619.0f, 42.0f, 0.7679f, -1280.0f, -832.0f),
                                     Sheet(-232.0f, -604.0f, 64.0f, 0.0f, -1280.0f, -832.0f),
                                     Sheet(-247.0f, -618.0f, 45.0f, 2.4435f, -1280.0f, -832.0f) };
        Canal[2].bFoot = 1;
        int Column[4];
        Check(FallColumnsOf(Canal, 4, Column) == 1 && Column[0] == 0 && Column[3] == 0,
              "fall column: the canal's four sheets are one column, though two of them do not reach one another");
        Check(!FallPiecesJoin(Canal[1], Canal[3]), "fall column: the two that are joined through the others");
        float Out[4 * FallColumnFloats];
        Check(FallColumnsMake(Canal, 4, Out, 4) == 1, "fall column: one column of water");
        CheckNear(Out[0], -231.0f, 1e-3f, "fall column: its middle the sheets' middles averaged");
        CheckNear(Out[1], -613.25f, 1e-3f, "fall column: its middle the sheets' middles averaged, the other way");
        Check(Out[2] == -1280.0f && Out[3] == -832.0f, "fall column: from the sheets' foot to their top");
        CheckNear(Out[6], 32.0f * WaterfallColumnWidth, 1e-4f,
                  "fall column: round, as wide as its widest sheet's share");
        Check(Out[7] == Out[6], "fall column: as wide either way");
        Check(Out[11] == 1.0f, "fall column: with a foot where any of its sheets comes down into water");
        CheckNear(Out[8], 0.1f * WaterfallAlbedo, 1e-7f, "fall column: its water's hue the sheets' averaged");
        Check(Out[15] == WaterfallGlint, "fall column: with the glint's share of the lamp");
        {
            bool bSame  = true;
            int  Orders = 0;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++)
                    for (int c = 0; c < 4; c++)
                    {
                        const int d = 6 - a - b - c;
                        if (a == b || a == c || b == c || d < 0 || d > 3 || d == a || d == b || d == c)
                            continue;
                        const FFallSheetPiece Mixed[4] = { Canal[a], Canal[b], Canal[c], Canal[d] };
                        float Again[FallColumnFloats];
                        bSame = bSame && FallColumnsMake(Mixed, 4, Again, 1) == 1;
                        // All but the direction, which a round column has no use for; the sums to a rounding.
                        for (int k = 0; k < FallColumnFloats; k++)
                            if (k != 4 && k != 5)
                                bSame = bSame && Again[k] - Out[k] < 1e-3f && Out[k] - Again[k] < 1e-3f;
                        Orders++;
                    }
            Check(Orders == 24 && bSame, "fall column: the same column in every order its sheets can come in");
        }
        // What a frame that draws only some of the sheets would make of them: another column - so the whole fall
        // is handed over whichever of them is drawn.
        {
            float Part[FallColumnFloats];
            Check(FallColumnsMake(Canal, 1, Part, 1) == 1 && Part[6] != Part[7],
                  "fall column: one of its sheets alone is a ribbon");
            const FFallSheetPiece Two[2] = { Canal[0], Canal[2] };
            Check(FallColumnsMake(Two, 2, Part, 1) == 1 && Part[6] == Part[7] && Part[1] - Out[1] > 5.0f,
                  "fall column: two of them are round, but stand elsewhere");
        }
        // A sheet on its own, and sheets that run the same way: a ribbon along the first.
        {
            const FFallSheetPiece Wide = Sheet(0.0f, 0.0f, 240.0f, 0.3f, 0.0f, 100.0f);
            float Ribbon[2 * FallColumnFloats];
            Check(FallColumnsMake(&Wide, 1, Ribbon, 2) == 1 && Ribbon[4] == Wide.Across[0] &&
                  Ribbon[5] == Wide.Across[1], "fall ribbon: a sheet on its own, along itself");
            CheckNear(Ribbon[6], 120.0f * WaterfallRibbonWidth, 1e-4f, "fall ribbon: most of the sheet's width");
            CheckNear(Ribbon[7], 120.0f * WaterfallRibbonWidth * WaterfallRibbonThick, 1e-4f,
                      "fall ribbon: and a share of that thick");
            const FFallSheetPiece Thin = Sheet(0.0f, 0.0f, 20.0f, 0.0f, 0.0f, 100.0f);
            Check(FallColumnsMake(&Thin, 1, Ribbon, 2) == 1 && Ribbon[7] == WaterfallRibbonMin && Ribbon[11] == 0.0f,
                  "fall ribbon: never thinner than the least, and with no foot where it comes down onto no water");
            const FFallSheetPiece Pair[2] = { Sheet(0.0f, 0.0f, 64.0f, 0.2f, 0.0f, 100.0f),
                                              Sheet(4.0f, 0.0f, 64.0f, 0.2f + 3.14159f, 0.0f, 100.0f) };
            Check(FallColumnsMake(Pair, 2, Ribbon, 2) == 1 && Ribbon[6] != Ribbon[7] && Ribbon[4] == Pair[0].Across[0],
                  "fall ribbon: two sheets back to back are one ribbon, along the first");
            const FFallSheetPiece Apart[2] = { Sheet(0.0f, 0.0f, 64.0f, 0.0f, 0.0f, 100.0f),
                                               Sheet(300.0f, 0.0f, 64.0f, 0.0f, 0.0f, 100.0f) };
            Check(FallColumnsOf(Apart, 2, Column) == 2 && Column[0] == 0 && Column[1] == 1 &&
                  FallColumnsMake(Apart, 2, Ribbon, 2) == 2, "fall column: two falls apart are two");
            Ribbon[FallColumnFloats] = -77.0f;
            Check(FallColumnsMake(Apart, 2, Ribbon, 1) == 1 && Ribbon[FallColumnFloats] == -77.0f,
                  "fall column: no more made than there is room for");
            Check(FallColumnsMake(Apart, 0, Ribbon, 2) == 0 && FallColumnsOf(Apart, 0, Column) == 0,
                  "fall column: no sheets, no columns");
        }
        // A fall is taken whole or not at all.
        {
            static FFallGather G;
            G.Begin();
            Check(G.AddTogether(Canal, 4, 16) && G.Count == 4 && G.Columns == 1,
                  "whole fall: its four sheets, one column");
            const FFallSheetPiece Other = Sheet(500.0f, 0.0f, 64.0f, 0.0f, 0.0f, 100.0f);
            Check(!G.AddTogether(&Other, 1, 1) && G.Count == 4, "whole fall: no room for one more column, none taken");
            Check(G.AddTogether(Canal, 2, 1) && G.Columns == 1 && G.Count == 6,
                  "whole fall: sheets standing with a column taken make no new one");
            Check(G.AddTogether(&Other, 1, 2) && G.Columns == 2 && G.Count == 7,
                  "whole fall: with room, a second column");
            Check(!G.AddTogether(Canal, 0, 16) && !G.AddTogether(Canal, FallPiecesMost + 1, 16) && G.Count == 7,
                  "whole fall: no sheets, or more than a frame holds, is no fall to take");
            static FFallSheetPiece Many[FallPiecesMost];
            for (int i = 0; i < FallPiecesMost; i++)
                Many[i] = Sheet(-900.0f, 0.0f, 64.0f, 0.1f * (float)i, 0.0f, 100.0f);
            Check(!G.AddTogether(Many, FallPiecesMost - 6, 16) && G.Count == 7,
                  "whole fall: with no room for all its sheets none is taken");
            G.Begin();
            Check(G.AddTogether(Many, FallPiecesMost, 16) && G.Count == FallPiecesMost && G.Columns == 1,
                  "whole fall: as many sheets as a frame holds are still one fall");
        }
    }
    // The sea: the game's own are thousands of units across, the pools that stand by a sky window a few hundred.
    Check(WaterIsSeaSized(3352.0f, 6976.0f) && WaterIsSeaSized(2975.0f, 3994.0f) && WaterIsSeaSized(26304.0f, 23360.0f),
          "sea: the airfield's, the dockyard's and the island's waters are seas");
    Check(!WaterIsSeaSized(116.0f, 117.0f) && !WaterIsSeaSized(352.0f, 352.0f) && !WaterIsSeaSized(568.0f, 1368.0f),
          "sea: a roof's tank, a bunker's basin and a tunnel's pool are not");
    Check(!WaterIsSeaSized(5000.0f, 1999.0f), "sea: nor a long ditch");
}

// The frame's raised planes, each kept once for the decals that lie on them.
static void TestReliefPlanes()
{
    using namespace RdxMath;
    FVec3 N;
    N.X = 0.6f; N.Y = 0.0f; N.Z = -0.8f;
    Check(ReliefSamePlane(N, 312.5f, N, 312.5f), "relief planes: a plane is itself");
    Check(ReliefPlaneKey(N, 312.5f) == ReliefPlaneKey(N, 312.5f), "relief planes: and is looked for in one place");
    FVec3 Hair = N;
    Hair.X += 0.0004f;
    Hair.Z -= 0.0003f;
    Check(ReliefSamePlane(N, 312.5f, Hair, 312.52f),
          "relief planes: two surfaces of one wall, a hair apart, are one plane");
    Check(!ReliefSamePlane(N, 312.5f, N, 313.5f), "relief planes: a wall a unit further off is another");
    Check(!ReliefSamePlane(N, 312.5f, N, 312.5f + 2.0f * ReliefPlaneSameD), "relief planes: and one a step behind it");
    FVec3 Turned = N;
    Turned.X = 0.62f;
    Check(!ReliefSamePlane(N, 312.5f, Turned, 312.5f), "relief planes: a wall at an angle to it is another");
    FVec3 Back;
    Back.X = -N.X; Back.Y = -N.Y; Back.Z = -N.Z;
    Check(!ReliefSamePlane(N, 312.5f, Back, -312.5f), "relief planes: and so is its other side");
    // Different planes are looked for in different places, nearly always.
    {
        enum { PLANES = 300, HEADS = 512 };
        static int Held[HEADS];
        for (int h = 0; h < HEADS; h++)
            Held[h] = 0;
        unsigned Seed = 777u;
        auto Next = [&Seed]() -> float
        {
            Seed = Seed * 1664525u + 1013904223u;
            return (float)((Seed >> 8) & 0xFFFF) / 65535.0f;
        };
        int Longest = 0;
        for (int i = 0; i < PLANES; i++)
        {
            // Walls stand in a few directions at many distances: the planes of a street.
            static const float Dirs[6][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { -1, 0, 0 },
                                              { 0.6f, 0, 0.8f }, { 0, -0.8f, 0.6f } };
            const int   d = (int)(Next() * 5.999f);
            FVec3 M;
            M.X = Dirs[d][0]; M.Y = Dirs[d][1]; M.Z = Dirs[d][2];
            const int h = (int)(ReliefPlaneKey(M, -2000.0f + Next() * 4000.0f) & (unsigned)(HEADS - 1));
            Held[h]++;
            Longest = (Held[h] > Longest) ? Held[h] : Longest;
        }
        Check(Longest <= 6, "relief planes: three hundred walls spread over the table, no place holding many");
    }
    Check(ReliefPlaneKey(N, 5.0e7f) == ReliefPlaneKey(N, 1.0e6f),
          "relief planes: a distance past any level is held in");
}

// The fires walked as bodies of flame: which textures are flames, which level surfaces are their sheets, how small a
// flame is one tongue and how brightly it glows, which is too small on the screen to walk, which pieces stand
// together, and what is done when there are more than the walk takes.
static void TestFlames()
{
    using namespace RdxMath;

    // The flames, by name - in any case, as the engine's names are.
    Check(FlameKind("Effects.Fire.flame_b") == Flame_Standing, "flame: a fireplace's sheet");
    Check(FlameKind("Effects.Fire.Flame_J") == Flame_Standing, "flame: the burning barrel's planes");
    Check(FlameKind("EFFECTS.FIRE.FLAME_B") == Flame_Standing, "flame: in any case");
    Check(FlameKind("HDTPanim.BarrelFire.BarrelFire0001") == Flame_Standing, "flame: HDTP's barrel, its first frame");
    Check(FlameKind("HDTPanim.BarrelFire.BarrelFire0021") == Flame_Standing, "flame: and its last");
    Check(FlameKind("Effects.Fire.OnFire_J") == Flame_Ball, "flame: the fireball on a burning body");
    Check(FlameKind("Effects.Fire.OneFlame_J") == Flame_Lick, "flame: a lick, a candle's flame");
    Check(FlameKind(L"Effects.Fire.OneFlame_J") == Flame_Lick, "flame: the same in the engine's wide letters");
    // The rest of the group is not flame: these are what a rule by the group took for fire.
    {
        static const char* const NotFlames[] =
        {
            "Effects.Fire.SparkFX1", "Effects.Fire.Spark_Electric", "Effects.Fire.proj_Prifle",
            "Effects.Fire.Wepn_Prifle_SFX", "Effects.Fire.OneFlame_G", "Effects.Fire.Fireball1",
            "Effects.Fire.FireballWhite", "Effects.Fire.flmethrwr_fire", "Effects.Fire.flmethrwr_flme"
        };
        bool bNone = true;
        for (int i = 0; i < 9; i++)
            bNone = bNone && FlameKind(NotFlames[i]) == Flame_None;
        Check(bNone, "flame: sparks, plasma, a rocket's exhaust and the flamethrower's stream are not flames to walk");
    }
    Check(FlameKind("Effects.Fire.flame_b2") == Flame_None, "flame: a longer name is another texture");
    Check(FlameKind("Effects.Fire.flame_") == Flame_None, "flame: and a shorter one");
    Check(FlameKind("Effects.Fire") == Flame_None, "flame: the group alone is not a texture in it");
    Check(FlameKind("MyMod.Effects.Fire.flame_b") == Flame_None, "flame: the name is the whole name, from its start");
    Check(FlameKind("HDTPanim.BarrelFire") == Flame_None, "flame: HDTP's group alone is no frame of it");
    Check(FlameKind("HDTPanim.Effects.HDTPFlrFlame") == Flame_None, "flame: the flare's jet is not a fire");
    Check(FlameKind("Effects.Smoke.SmokePuff1") == Flame_None, "flame: smoke is not fire");
    Check(FlameKind("") == Flame_None, "flame: an empty name");
    Check(FlameKind((const char*)0) == Flame_None, "flame: no name at all");

    // A flame's sheet: see-through, and standing.
    {
        const unsigned int Sheet = 0x0040010Cu;      // see-through, lets things through, two-sided, its own light
        Check(FlameSheetStands(Sheet, 0.0f), "flame sheet: an upright see-through sheet");
        Check(FlameSheetStands(Sheet, 0.6f) && FlameSheetStands(Sheet, -0.6f),
              "flame sheet: a leaning one still stands");
        Check(!FlameSheetStands(Sheet, 1.0f) && !FlameSheetStands(Sheet, -1.0f),
              "flame sheet: one lying flat does not");
        Check(!FlameSheetStands(Sheet, FlameSheetLean), "flame sheet: nor one at the limit");
        Check(!FlameSheetStands(0x00400000u, 0.0f), "flame sheet: a solid wall with a light of its own is a wall");
        Check(!FlameSheetStands(RDX_PF_Masked, 0.0f), "flame sheet: a cut-out picture is not see-through");
        Check(!FlameSheetStands(RDX_PF_Modulated, 0.0f), "flame sheet: nor is one that darkens what is behind it");
    }

    // One tongue: as bright through its middle whatever its size.
    Check(FireTongueGain(FireTongueHalf) == 1.0f && FireTongueGain(2.0f * FireTongueHalf) == 1.0f,
          "tongue: a flame as wide as a fire's own glows as it is");
    {
        const float Halves[4] = { 0.7f, 1.0f, 2.0f, 3.5f };
        for (int i = 0; i < 4; i++)
            CheckNear(Halves[i] * FireTongueGain(Halves[i]), FireTongueHalf, 1e-4f,
                      "tongue: a small flame's middle is as bright as a large tongue's");
    }
    Check(FireTongueGain(0.0f) == FireTongueGainMost && FireTongueGain(0.01f) == FireTongueGainMost,
          "tongue: the gain has a most");
    Check(FireTongueGain(1.0f) > FireTongueGain(2.0f), "tongue: the narrower, the stronger by the unit");

    // How wide a flame shows on the walk's target.
    CheckNear(FlamePixels(2.0f, 500.0f, 1000.0f, 1920.0f, 960), 2.0f, 1e-5f,
              "flame pixels: units to the engine's pixels at that depth, then to the target's");
    Check(FlamePixels(2.0f, 250.0f, 1000.0f, 1920.0f, 960) > FlamePixels(2.0f, 500.0f, 1000.0f, 1920.0f, 960),
          "flame pixels: nearer is wider");
    Check(FlamePixels(2.0f, 0.5f, 1000.0f, 1920.0f, 960) > 1.0e8f,
          "flame pixels: at the eye it is as wide as anything");
    Check(FlamePixels(2.0f, 500.0f, 1000.0f, 0.0f, 960) == 0.0f,
          "flame pixels: with no view there is nothing to show on");
    Check(FlamePixels(2.0f, 500.0f, 1000.0f, 1920.0f, 0) == 0.0f, "flame pixels: nor with no target");
    Check(FlamePixels(2.0f, 500.0f, 0.0f, 1920.0f, 960) < FireKeepPixels,
          "flame pixels: nor before the projection is known");
    Check(FireKeepPixels < FireMinPixels, "flame pixels: a flame is kept a little smaller than it is taken");
    {
        // A candle's flame, a unit and a half wide, on a screen 3840 across seen through 75 degrees.
        const float Focal = 1920.0f / 0.76733f;
        Check(FlamePixels(1.4f, 400.0f, Focal, 3840.0f, 1920) >= FireMinPixels,
              "flame pixels: a candle near is walked");
        const float Far = FlamePixels(1.4f, 500.0f, Focal, 3840.0f, 1920);
        Check(Far < FireMinPixels && Far >= FireKeepPixels, "flame pixels: further off it is not taken, but kept");
        Check(FlamePixels(1.4f, 800.0f, Focal, 3840.0f, 1920) < FireKeepPixels,
              "flame pixels: and far off it is let go");
    }

    // A body smaller than the walk can draw: widened, holding as much less by the unit as it grew.
    {
        float Across = 1.0f, Up = 1.0f;
        Check(VolumeWiden(Across, Up, 0.5f) == 1.0f && Across == 1.0f && Up == 1.0f,
              "small body: one large enough is left as it is");
        const float Keep = VolumeWiden(Across, Up, 2.0f);
        Check(Across == 2.0f && Up == 2.0f, "small body: widened and raised to the least");
        CheckNear(Keep * Across * Across * Up, 1.0f, 1e-5f, "small body: as much of it in all as there was");
        Across = 1.0f;
        Up     = 5.0f;
        CheckNear(VolumeWiden(Across, Up, 2.0f), 0.25f, 1e-6f,
                  "small body: one tall enough is thinned for its width alone");
        Check(Up == 5.0f, "small body: and keeps its height");
    }
    Check(CloudPuffMinPixels >= 2.0f, "small body: a puff is walked at least two pixels of the target across");

    // A tongue further off than the walk can draw it: widened to a few pixels, with the same light in all.
    {
        float Half = 0.7f, Up = 1.35f;
        Check(FireTongueWiden(Half, Up, 0.1f) == 1.0f && Half == 0.7f && Up == 1.35f,
              "tongue far off: one wide enough on the target is left as it is");
        Half = 0.7f;
        Up   = 1.35f;
        const float Dim = FireTongueWiden(Half, Up, 1.0f);
        Check(Half == 0.5f * FireTongueMinPixels && Up == 0.5f * FireTongueMinPixels,
              "tongue far off: widened and raised to the fewest pixels");
        // The light it gives: by the unit times its footprint times its height.
        CheckNear(Dim * Half * Half * Up, 0.7f * 0.7f * 1.35f, 1e-4f, "tongue far off: the same light in all");
        Half = 0.7f;
        Up   = 4.0f;
        const float Narrow = FireTongueWiden(Half, Up, 1.0f);
        Check(Up == 4.0f, "tongue far off: one tall enough keeps its height");
        CheckNear(Narrow * Half * Half, 0.7f * 0.7f, 1e-4f, "tongue far off: and is dimmed for its width alone");
        Half = 0.0f;
        Up   = 0.0f;
        Check(FireTongueWiden(Half, Up, 1.0f) == 0.0f && Half > 0.0f, "tongue far off: no flame gives no light");
    }
    Check(FireTongueMinPixels < FireKeepPixels,
          "tongue far off: a candle's sheet is let go before its tongue would have to be widened");

    // A large fire is the flame made larger, by its narrow way.
    Check(FireBodyScale(24.35f, 9.74f) == 1.0f && FireBodyScale(10.0f, 10.0f) == 1.0f &&
          FireBodyScale(20.0f, 20.0f) == 1.0f, "large fire: a fireplace, a torch and a burning floor are as they were");
    Check(FireBodyScale(FireScaleHalf, 500.0f) == 1.0f && FireBodyScale(500.0f, FireScaleHalf) == 1.0f,
          "large fire: a long fire no wider than the measure is not a large one");
    CheckNear(FireBodyScale(274.0f, 109.6f), 109.6f / FireScaleHalf, 1e-5f, "large fire: a blaze, by its width");
    CheckNear(FireBodyScale(36.4f, 58.6f), 36.4f / FireScaleHalf, 1e-5f, "large fire: whichever way round it lies");
    Check(FireScaleHalf >= 22.0f, "large fire: the measure is above the fires the look was settled on");

    // Which pieces stand together.
    {
        const float At[2]   = { 100.0f, 50.0f };
        const float Near[2] = { 130.0f, 50.0f };
        const float Off[2]  = { 140.0f, 50.0f };
        Check(FlamesJoin(At, 16.0f, 0.0f, 32.0f, false, At, 12.0f, 0.0f, 32.0f, false),
              "flames join: a fireplace's sheets crossing in one spot");
        Check(FlamesJoin(At, 16.0f, 0.0f, 32.0f, false, Near, 16.0f, 0.0f, 32.0f, false),
              "flames join: two sheets whose ends meet");
        Check(!FlamesJoin(At, 16.0f, 0.0f, 32.0f, false, Off, 16.0f, 0.0f, 32.0f, false),
              "flames join: not two with a gap between them");
        Check(FlamesJoin(At, 16.0f, 0.0f, 32.0f, false, At, 16.0f, 20.0f, 52.0f, false),
              "flames join: sheets sharing a good part of their height");
        Check(!FlamesJoin(At, 16.0f, 0.0f, 32.0f, false, At, 16.0f, 30.0f, 62.0f, false),
              "flames join: not sheets that barely share any");
        Check(FlamesJoin(At, 16.0f, 0.0f, 32.0f, true, At, 16.0f, 32.0f, 64.0f, true),
              "flames join: round pieces that touch in height");
        Check(!FlamesJoin(At, 16.0f, 0.0f, 32.0f, true, At, 16.0f, 33.0f, 64.0f, true),
              "flames join: not round pieces one above the other");
        Check(FlamesJoin(At, 16.0f, 0.0f, 32.0f, false, Near, 16.0f, 0.0f, 32.0f, true) ==
              FlamesJoin(Near, 16.0f, 0.0f, 32.0f, true, At, 16.0f, 0.0f, 32.0f, false),
              "flames join: the same either way round");
        // Candles on one stand: each flame's two sheets are one flame, two flames four units apart are two.
        const float CandleA[2] = { 0.0f, 0.0f };
        const float CandleB[2] = { 4.0f, 0.0f };
        Check(FlamesJoin(CandleA, 1.0f, 10.0f, 13.0f, false, CandleA, 1.0f, 10.0f, 13.0f, false),
              "flames join: a candle's crossed sheets");
        Check(!FlamesJoin(CandleA, 1.0f, 10.0f, 13.0f, false, CandleB, 1.0f, 10.0f, 13.0f, false),
              "flames join: not the next candle's");
    }

    // The bodies counted as the pieces come: the same as counting them once all are there.
    {
        enum { PIECES = 96 };
        float Mid[PIECES][2], Half[PIECES], Bottom[PIECES], Top[PIECES];
        bool  bRound[PIECES];
        unsigned Seed = 12345u;
        auto Next = [&Seed]() -> float
        {
            Seed = Seed * 1664525u + 1013904223u;
            return (float)((Seed >> 8) & 0xFFFF) / 65535.0f;
        };
        bool bSame = true, bRoots = true;
        for (int Trial = 0; Trial < 20; Trial++)
        {
            for (int i = 0; i < PIECES; i++)
            {
                Mid[i][0] = Next() * 400.0f;
                Mid[i][1] = Next() * 400.0f;
                Half[i]   = 2.0f + Next() * 24.0f;
                Bottom[i] = Next() * 40.0f;
                Top[i]    = Bottom[i] + 4.0f + Next() * 40.0f;
                bRound[i] = Next() > 0.5f;
            }
            auto Joins = [&](int a, int b) -> bool
            {
                return FlamesJoin(Mid[a], Half[a], Bottom[a], Top[a], bRound[a], Mid[b], Half[b], Bottom[b], Top[b],
                                  bRound[b]);
            };
            // As they come.
            int Group[PIECES], Bodies = 0;
            for (int i = 0; i < PIECES; i++)
            {
                int Stands[PIECES], StandCount = 0;
                for (int j = 0; j < i; j++)
                    if (Joins(j, i))
                        Stands[StandCount++] = j;
                FlameBodyAdd(Group, i, Bodies, Stands, StandCount);
            }
            // All at once: flooding out from each piece not reached yet.
            int  Label[PIECES], Count = 0;
            for (int i = 0; i < PIECES; i++)
                Label[i] = -1;
            for (int i = 0; i < PIECES; i++)
            {
                if (Label[i] >= 0)
                    continue;
                int Stack[PIECES], Depth = 0;
                Stack[Depth++] = i;
                Label[i]       = Count;
                while (Depth > 0)
                {
                    const int a = Stack[--Depth];
                    for (int b = 0; b < PIECES; b++)
                        if (Label[b] < 0 && Joins(a, b))
                        {
                            Label[b]       = Count;
                            Stack[Depth++] = b;
                        }
                }
                Count++;
            }
            bSame = bSame && Bodies == Count;
            for (int a = 0; a < PIECES; a++)
                for (int b = a + 1; b < PIECES; b++)
                    bRoots = bRoots && ((FlameRoot(Group, a) == FlameRoot(Group, b)) == (Label[a] == Label[b]));
        }
        Check(bSame, "flame bodies: counted as the pieces come, as many as there are in the end");
        Check(bRoots, "flame bodies: and the same pieces together");
        int Group[4], Bodies = 0;
        FlameBodyAdd(Group, 0, Bodies, nullptr, 0);
        FlameBodyAdd(Group, 1, Bodies, nullptr, 0);
        Check(Bodies == 2, "flame bodies: two pieces apart are two");
        const int Both[2] = { 0, 1 };
        FlameBodyAdd(Group, 2, Bodies, Both, 2);
        Check(Bodies == 1, "flame bodies: a piece between them makes the three one");
        const int Twice[2] = { 0, 1 };
        FlameBodyAdd(Group, 3, Bodies, Twice, 2);
        Check(Bodies == 1, "flame bodies: a piece standing with two of one body adds none and takes none away");
    }

    // The pieces as the draw path makes them.
    {
        const FFlamePiece S = FlameSheetPiece(10.0f, 20.0f, 0.0f, 1.0f, 30.5f, 100.0f, 128.0f, 400.0f);
        Check(S.Kind == FlameShape_Standing && !S.bRound && S.Bright == 1.0f && S.Depth == 400.0f,
              "flame piece: a level's sheet is a standing fire's, lying its own way");
        Check(S.Mid[0] == 10.0f && S.Mid[1] == 20.0f && S.Across[0] == 0.0f && S.Across[1] == 1.0f &&
              S.Half == 30.5f && S.Bottom == 100.0f && S.Top == 128.0f, "flame piece: with the sheet's place and size");
        CheckNear(S.Tongue[0], 30.5f * FireCardWidth, 1e-5f, "flame piece: the tongue painted on a sheet is narrower");
        CheckNear(S.Tongue[2] - S.Tongue[1], FireTongueTall * 28.0f, 1e-4f, "flame piece: and not quite as tall");
        CheckNear(0.5f * (S.Tongue[1] + S.Tongue[2]), 114.0f, 1e-4f, "flame piece: round the sheet's middle");

        const FFlamePiece M = FlameMeshPiece(-10.0f, 0.0f, 5.0f, 10.0f, 4.0f, 35.0f, 200.0f);
        Check(M.bRound && M.Kind == FlameShape_Standing && M.Mid[0] == 0.0f && M.Mid[1] == 2.0f && M.Depth == 200.0f,
              "flame piece: a mesh's polygon is a round one, in the middle of its box");
        CheckNear(M.Half, 10.0f * FireCardWidth, 1e-5f,
                  "flame piece: as wide as the flame's share of the box's wider side");
        Check(M.Bottom == 5.0f && M.Top == 5.0f + 30.0f * FireMeshTall && M.Tongue[0] == M.Half &&
              M.Tongue[1] == M.Bottom && M.Tongue[2] == M.Top,
              "flame piece: as tall as the polygon, and its tongue the whole of it");

        const FFlamePiece B = FlameCardPiece(Flame_Ball, 1.0f, 2.0f, 50.0f, 32.0f, 32.0f, 0.5f, 300.0f);
        Check(B.Kind == FlameShape_Ball && B.bRound && B.Bright == 0.5f && B.Depth == 300.0f && B.Mid[0] == 1.0f &&
              B.Mid[1] == 2.0f, "flame piece: a fireball's card, with the game's fade of it");
        CheckNear(B.Half, 14.4f, 1e-4f, "flame piece: a ball over most of the card's width");
        CheckNear(B.Bottom, 37.2f, 1e-4f, "flame piece: from below the card's middle");
        CheckNear(B.Top, 62.8f, 1e-4f, "flame piece: to as far above it");
        const FFlamePiece L = FlameCardPiece(Flame_Lick, 1.0f, 2.0f, 50.0f, 11.2f, 22.4f, 1.0f, 300.0f);
        Check(L.Kind == FlameShape_Lick && L.bRound, "flame piece: a lick's card");
        CheckNear(L.Half, 3.92f, 1e-4f, "flame piece: a third of the card's width");
        CheckNear(L.Top - L.Bottom, 0.9f * 22.4f, 1e-4f, "flame piece: nearly all its height");
        const FFlamePiece F = FlameCardPiece(Flame_Standing, 1.0f, 2.0f, 50.0f, 40.0f, 60.0f, 1.0f, 300.0f);
        Check(F.Kind == FlameShape_Standing && F.bRound, "flame piece: a burning floor's card");
        CheckNear(F.Half, 20.0f * FireCardWidth, 1e-4f, "flame piece: the flame's share of the card's width");
        CheckNear(F.Bottom, 20.0f, 1e-4f, "flame piece: standing on the card's lower edge");
        CheckNear(F.Top, 20.0f + 60.0f * FireCardTall, 1e-4f, "flame piece: its body part of the card's height");

        // A card sunk in the floor: the flame is the part of the card above the floor, standing on it.
        const FFlamePiece Whole = FlameCardPiece(Flame_Standing, 1.0f, 2.0f, 1.0f, 57.0f, 57.0f, 1.0f, 300.0f);
        const FFlamePiece Low   = FlameCardPiece(Flame_Standing, 1.0f, 2.0f, 1.0f, 57.0f, 57.0f, 1.0f, 300.0f, -40.0f);
        const FFlamePiece Edge  = FlameCardPiece(Flame_Standing, 1.0f, 2.0f, 1.0f, 57.0f, 57.0f, 1.0f, 300.0f, -27.5f);
        const FFlamePiece Sunk  = FlameCardPiece(Flame_Standing, 1.0f, 2.0f, 1.0f, 57.0f, 57.0f, 1.0f, 300.0f, 0.0f);
        const FFlamePiece Over  = FlameCardPiece(Flame_Standing, 1.0f, 2.0f, 1.0f, 57.0f, 57.0f, 1.0f, 300.0f, 40.0f);
        CheckNear(Whole.Bottom, -27.5f, 1e-4f, "flame card: with no floor named the card's lower edge is the foot");
        Check(Low.Bottom == Whole.Bottom && Low.Top == Whole.Top && Low.Tongue[1] == Whole.Tongue[1] &&
              Low.Tongue[2] == Whole.Tongue[2], "flame card: a floor under the card's foot changes nothing");
        Check(Edge.Bottom == Whole.Bottom && Edge.Top == Whole.Top,
              "flame card: a floor at the card's foot changes nothing");
        CheckNear(Sunk.Bottom, 0.0f, 1e-4f, "flame card: sunk to its middle, the flame stands on the floor");
        CheckNear(Sunk.Top, 29.5f * FireCardTall, 1e-4f, "flame card: and its body is part of what shows");
        CheckNear(Sunk.Tongue[1], 14.75f - 0.5f * FireTongueTall * 29.5f, 1e-4f,
                  "flame card: its tongue is what shows too, from this");
        CheckNear(Sunk.Tongue[2], 14.75f + 0.5f * FireTongueTall * 29.5f, 1e-4f, "flame card: to this");
        Check(Sunk.Half == Whole.Half, "flame card: as wide as before");
        Check(Over.Bottom == Whole.Bottom && Over.Top == Whole.Top,
              "flame card: a level above the whole card is not its floor");
        const FFlamePiece BallOn = FlameCardPiece(Flame_Ball, 1.0f, 2.0f, 1.0f, 32.0f, 32.0f, 1.0f, 300.0f, 0.0f);
        const FFlamePiece BallNo = FlameCardPiece(Flame_Ball, 1.0f, 2.0f, 1.0f, 32.0f, 32.0f, 1.0f, 300.0f);
        Check(BallOn.Bottom == BallNo.Bottom && BallOn.Top == BallNo.Top,
              "flame card: a fireball is where its card is, floor or no floor");
    }

    // Which fire each of a level's sheets stands in.
    {
        const FFlamePiece CandleA = FlameSheetPiece(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 10.0f, 13.0f, 0.0f);
        const FFlamePiece CandleA2 = FlameSheetPiece(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 10.0f, 13.0f, 0.0f);
        const FFlamePiece CandleB = FlameSheetPiece(4.0f, 0.0f, 1.0f, 0.0f, 1.0f, 10.0f, 13.0f, 0.0f);
        const FFlamePiece CandleB2 = FlameSheetPiece(4.0f, 0.0f, 0.0f, 1.0f, 1.0f, 10.0f, 13.0f, 0.0f);
        const FFlamePiece Hearth = FlameSheetPiece(500.0f, 0.0f, 1.0f, 0.0f, 24.0f, 0.0f, 32.0f, 0.0f);
        const FFlamePiece Hearth2 = FlameSheetPiece(500.0f, 0.0f, 0.9f, 0.4359f, 24.0f, 0.0f, 32.0f, 0.0f);
        const FFlamePiece Six[6] = { CandleA, CandleB, CandleA2, Hearth, CandleB2, Hearth2 };
        int Fire[6];
        Check(FlameFiresOf(Six, 6, Fire) == 3, "level fires: two candles and a fireplace are three fires");
        Check(Fire[0] == 0 && Fire[1] == 1 && Fire[2] == 0 && Fire[3] == 2 && Fire[4] == 1 && Fire[5] == 2,
              "level fires: numbered in the order of their first sheets, each sheet with its own");
        // A row of sheets end to end: the first and the last do not reach one another, and are one fire all the same.
        const FFlamePiece Row[3] = { FlameSheetPiece(0.0f, 0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 30.0f, 0.0f),
                                     FlameSheetPiece(70.0f, 0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 30.0f, 0.0f),
                                     FlameSheetPiece(35.0f, 0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 30.0f, 0.0f) };
        Check(!FlamePiecesJoin(Row[0], Row[1]) && FlameFiresOf(Row, 3, Fire) == 1 && Fire[0] == 0 && Fire[1] == 0 &&
              Fire[2] == 0, "level fires: sheets joined through another are one fire");
        Check(FlameFiresOf(Row, 2, Fire) == 2 && Fire[0] == 0 && Fire[1] == 1,
              "level fires: without the one between them they are two");
        Check(FlameFiresOf(Row, 0, Fire) == 0, "level fires: no sheets, no fires");
        const FFlamePiece Balls[2] = { FlameCardPiece(Flame_Ball, 0.0f, 0.0f, 10.0f, 32.0f, 32.0f, 1.0f, 0.0f),
                                       FlameCardPiece(Flame_Ball, 1.0f, 0.0f, 10.0f, 32.0f, 32.0f, 1.0f, 0.0f) };
        Check(!FlamePiecesJoin(Balls[0], Balls[1]) && !FlamePiecesJoin(Balls[0], CandleA),
              "level fires: a fireball joins nothing, not even another on the same spot");
    }

    // A frame's flames made into the bodies the walk takes.
    {
        // The gather's list is the caller's, and so is the room the bodies are made in: a number a piece.
        FFlameGather G;
        FFlamePiece  Held[32];
        int          Room[32];
        G.Hold(Held, 32);
        float Body[8 * FlameBodyFloats];
        const float Near = 0.0005f;         // the level's units a pixel covers at a depth of one, on a large target

        // A fireplace as the levels have it: two sheets 48.7 wide and 32 high, crossing at 19 degrees in one spot.
        const float Lean = 0.3316f;
        const FFlamePiece HearthA = FlameSheetPiece(832.0f, 532.0f, cosf(0.0f), sinf(0.0f), 24.35f, 244.0f, 276.0f,
                                                    300.0f);
        const FFlamePiece HearthB = FlameSheetPiece(832.0f, 532.0f, cosf(Lean), sinf(Lean), 24.35f, 244.0f, 276.0f,
                                                    310.0f);
        const FFlamePiece Hearth[2]  = { HearthA, HearthB };
        const FFlamePiece Turned[2]  = { HearthB, HearthA };
        G.Begin();
        Check(G.AddTogether(Hearth, 2) && G.Count == 2, "fireplace: its two sheets are taken together");
        Check(FlameBodiesMake(G.Pieces, G.Count, Body, 8, Near, Room) == 1, "fireplace: one body of flame");
        Check(Body[12] == (float)FlameShape_Standing && Body[13] == 1.0f && Body[14] == 1.0f && Body[15] == 1.0f,
              "fireplace: a fire standing on its foot, as bright as it is and with the grain it was shaped on");
        CheckNear(Body[0], 832.0f, 0.01f, "fireplace: in the sheets' middle");
        CheckNear(Body[1], 532.0f, 0.01f, "fireplace: in the sheets' middle, the other way");
        Check(Body[2] == 244.0f && Body[3] == 276.0f, "fireplace: from the sheets' foot to their top");
        Check(Body[4] == HearthA.Across[0] && Body[5] == HearthA.Across[1],
              "fireplace: of two sheets that fit it alike, it lies along the first");
        CheckNear(Body[6], 24.35f, 1e-3f, "fireplace: as long as a sheet");
        CheckNear(Body[7], 0.4f * 24.35f, 1e-3f, "fireplace: with a round to it though its sheets are nearly flat");
        float Other[FlameBodyFloats];
        Check(FlameBodiesMake(Turned, 2, Other, 1, Near, Room) == 1 && Other[4] == HearthB.Across[0] &&
              Other[5] == HearthB.Across[1],
              "fireplace: handed over the other way round it lies along the other sheet - so a level's fire is "
              "always handed over in one order");
        CheckNear(Other[6], Body[6], 1e-3f, "fireplace: as long either way");
        CheckNear(Other[7], Body[7], 1e-3f, "fireplace: and as wide");

        // A torch: two sheets crossing at right angles. Either way round it is the same round body.
        const FFlamePiece Torch[2] = { FlameSheetPiece(0.0f, 0.0f, 1.0f, 0.0f, 10.0f, 0.0f, 39.0f, 100.0f),
                                       FlameSheetPiece(0.0f, 0.0f, 0.0f, 1.0f, 10.0f, 0.0f, 39.0f, 100.0f) };
        Check(FlameBodiesMake(Torch, 2, Body, 8, Near, Room) == 1 && Body[12] == (float)FlameShape_Standing &&
              Body[6] == 10.0f && Body[7] == 10.0f && Body[0] == 0.0f && Body[1] == 0.0f,
              "torch: crossed sheets make a round fire as wide as they are");
        // One sheet alone still has a round to it, and is never thinner than a fire is.
        Check(FlameBodiesMake(Torch, 1, Body, 8, Near, Room) == 1 && Body[6] == 10.0f && Body[7] == FireBodyMinHalf,
              "one sheet: a flame with a round to it");

        // A wreck's long blaze as a level has it: three sheets end to end and a fourth across their end. The body
        // lies along the blaze whichever sheet comes first - not along the one across.
        const FFlamePiece Blaze[4] = { FlameSheetPiece(-6002.0f, -5515.0f, 0.0f, 1.0f, 130.0f, 216.0f, 416.0f, 500.0f),
                                       FlameSheetPiece(-6010.0f, -5371.0f, 0.0f, 1.0f, 130.0f, 219.0f, 371.0f, 520.0f),
                                       FlameSheetPiece(-6026.0f, -5227.0f, 0.0f, 1.0f, 130.0f, 216.0f, 416.0f, 540.0f),
                                       FlameSheetPiece(-6092.0f, -5609.0f, 0.98686f, 0.16160f, 99.3f, 238.0f, 390.0f,
                                                       480.0f) };
        int Fire[4];
        Check(FlameFiresOf(Blaze, 4, Fire) == 1, "long blaze: its four sheets are one fire");
        Check(FlameBodiesMake(Blaze, 4, Body, 8, Near, Room) == 1, "long blaze: one body");
        Check(Body[4] == 0.0f && Body[5] == 1.0f, "long blaze: lying along its three sheets");
        CheckNear(Body[6], 274.0f, 0.1f, "long blaze: as long as the three end to end");
        CheckNear(Body[7], 109.6f, 0.1f, "long blaze: and two fifths of that across");
        CheckNear(Body[0], -6092.0f, 0.5f, "long blaze: its middle between the sheet across and the three");
        CheckNear(Body[1], -5371.0f, 0.5f, "long blaze: and halfway along them");
        Check(Body[2] == 216.0f && Body[3] == 416.0f, "long blaze: from the lowest foot to the highest top");
        CheckNear(Body[15], 109.6f / FireScaleHalf, 0.01f, "long blaze: its grain as much coarser as it is wider");
        CheckNear(Body[14] * Body[15], 1.0f, 1e-5f, "long blaze: and its glow as much thinner by the unit");
        {
            // A burning floor: four of the game's flame cards standing together, a body wider than a fireplace. The
            // game lights and puts out such flames as it goes, and the body's box changes with every one: its grain
            // stays the plain one, only its glow is thinner. With a level's sheet among them it is a large fire again.
            const FFlamePiece Floor[5] = { FlameCardPiece(Flame_Standing, 0.0f, 0.0f, 40.0f, 80.0f, 80.0f, 1.0f, 300.0f),
                                           FlameCardPiece(Flame_Standing, 50.0f, 0.0f, 40.0f, 80.0f, 80.0f, 1.0f, 300.0f),
                                           FlameCardPiece(Flame_Standing, 0.0f, 50.0f, 40.0f, 80.0f, 80.0f, 1.0f, 300.0f),
                                           FlameCardPiece(Flame_Standing, 50.0f, 50.0f, 40.0f, 80.0f, 80.0f, 1.0f, 300.0f),
                                           FlameSheetPiece(25.0f, 25.0f, 1.0f, 0.0f, 40.0f, 0.0f, 44.0f, 300.0f) };
            float Cards[FlameBodyFloats * 8];
            Check(FlameBodiesMake(Floor, 4, Cards, 8, Near, Room) == 1, "burning floor: its four cards are one body");
            Check(Cards[6] > FireScaleHalf && Cards[7] > FireScaleHalf, "burning floor: a body wider than a fireplace");
            Check(Cards[15] == 1.0f, "burning floor: the game's own flames keep the plain grain");
            CheckNear(Cards[14], 1.0f / FireBodyScale(Cards[6], Cards[7]), 1e-5f,
                      "burning floor: its glow as much thinner as the body is larger");
            Check(FlameBodiesMake(Floor, 5, Cards, 8, Near, Room) == 1 && Cards[15] > 1.0f,
                  "burning floor: with a level's sheet in it, a large fire's coarser grain");
        }
        {
            bool bSame  = true;
            int  Orders = 0;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++)
                    for (int c = 0; c < 4; c++)
                    {
                        const int d = 6 - a - b - c;
                        if (a == b || a == c || b == c || d < 0 || d > 3 || d == a || d == b || d == c)
                            continue;
                        const FFlamePiece Mixed[4] = { Blaze[a], Blaze[b], Blaze[c], Blaze[d] };
                        float Again[FlameBodyFloats];
                        bSame = bSame && FlameBodiesMake(Mixed, 4, Again, 1, Near, Room) == 1;
                        for (int k = 0; k < FlameBodyFloats; k++)
                            bSame = bSame && Again[k] == Body[k];
                        Orders++;
                    }
            Check(Orders == 24 && bSame,
                  "long blaze: the same body, to the last number, in every order its sheets can come in");
        }

        // A candle: two small sheets crossing. One tongue of flame of its own size, glowing as much stronger by the
        // unit as it is narrower than a fire's tongue.
        const FFlamePiece Candle[2] = { FlameSheetPiece(100.0f, 50.0f, 1.0f, 0.0f, 1.0f, 10.0f, 13.0f, 100.0f),
                                        FlameSheetPiece(100.0f, 50.0f, 0.0f, 1.0f, 1.0f, 10.0f, 13.0f, 100.0f) };
        Check(FlameBodiesMake(Candle, 2, Body, 8, Near, Room) == 1 && Body[12] == (float)FlameShape_Lick,
              "candle: one tongue of flame");
        Check(Body[4] == 1.0f && Body[5] == 0.0f, "candle: a tongue lies no way in particular");
        CheckNear(Body[0], 100.0f, 1e-3f, "candle: where it stands");
        CheckNear(Body[1], 50.0f, 1e-3f, "candle: where it stands, the other way");
        CheckNear(Body[6], FireCardWidth, 1e-4f, "candle: as wide as the flame painted on its sheets");
        Check(Body[7] == Body[6], "candle: and round");
        CheckNear(Body[2], 11.5f - 0.5f * FireTongueTall * 3.0f, 1e-4f, "candle: from the painted flame's foot");
        CheckNear(Body[3], 11.5f + 0.5f * FireTongueTall * 3.0f, 1e-4f, "candle: to its top");
        CheckNear(Body[14], FireTongueCore * FireTongueHalf / FireCardWidth, 1e-3f,
                  "candle: through its middle a fire's tongue's glow, and a lone flame's core over it");
        Check(Body[15] == 1.0f, "candle: a tongue has no grain to make coarser");
        Check(Body[13] == 1.0f, "candle: as bright as the game has it");
        {
            // The same candle a thousand units off, on a target where a pixel covers two units there. As far off as
            // its farthest sheet: the other one standing nearer changes nothing.
            FFlamePiece Far[2] = { Candle[0], Candle[1] };
            Far[0].Depth = 1000.0f;
            Far[1].Depth = 400.0f;
            float Wide[FlameBodyFloats];
            Check(FlameBodiesMake(Far, 2, Wide, 1, 0.002f, Room) == 1 && Wide[7] == Wide[6],
                  "candle far off: still one round tongue");
            CheckNear(Wide[6], 3.0f, 1e-4f, "candle far off: widened to a few pixels of the target");
            CheckNear(Wide[3] - Wide[2], 6.0f, 1e-4f, "candle far off: and raised to as many");
            CheckNear(Wide[14] * Wide[6] * Wide[6] * 0.5f * (Wide[3] - Wide[2]),
                      Body[14] * Body[6] * Body[6] * 0.5f * (Body[3] - Body[2]), 1e-3f,
                      "candle far off: giving the same light in all");
        }

        // A burning barrel's flame: a mesh's crossing planes, two triangles each - the same box four times over.
        G.Begin();
        bool bTook = true;
        for (int i = 0; i < 2; i++)
        {
            const FFlamePiece AlongX = FlameMeshPiece(46.0f, 160.0f, -40.0f, 74.0f, 160.0f, -10.0f, 160.0f);
            const FFlamePiece AlongY = FlameMeshPiece(60.0f, 146.0f, -40.0f, 60.0f, 174.0f, -10.0f, 160.0f);
            bTook = bTook && G.Add(AlongX) && G.Add(AlongY);
        }
        Check(bTook && G.Count == 1, "barrel: its planes' polygons are one piece, taken each time");
        Check(FlameBodiesMake(G.Pieces, G.Count, Body, 8, Near, Room) == 1 && Body[12] == (float)FlameShape_Standing &&
              Body[4] == 1.0f && Body[5] == 0.0f, "barrel: one standing fire, lying along the world's X");
        CheckNear(Body[6], 14.0f * FireCardWidth, 1e-4f, "barrel: as wide as the flame's share of its planes");
        CheckNear(Body[7], 14.0f * FireCardWidth, 1e-4f, "barrel: and round");
        // Sixteen polygons of a flame that are not alike are still one body.
        G.Begin();
        for (int i = 0; i < 16; i++)
            G.Add(FlameMeshPiece(50.0f + (float)i, 150.0f - (float)i, -40.0f + (float)i, 70.0f + (float)i, 170.0f,
                                 -10.0f, 160.0f));
        Check(G.Count == 16 && FlameBodiesMake(G.Pieces, G.Count, Body, 8, Near, Room) == 1,
              "barrel: sixteen polygons of one flame are one body");

        // A burning body: the game's fireballs and licks, each a flame of its own though they overlap.
        G.Begin();
        for (int i = 0; i < 4; i++)
        {
            G.Add(FlameCardPiece(Flame_Ball, (float)i, 220.0f, 10.0f * (float)i, 32.0f, 32.0f, 0.8f, 220.0f));
            G.Add(FlameCardPiece(Flame_Lick, (float)i, 221.0f, 10.0f * (float)i, 11.2f, 22.4f, 0.6f, 221.0f));
        }
        Check(G.Count == 8, "burning body: every card is taken");
        {
            int  Balls = 0, Licks = 0;
            bool bAsGiven = FlameBodiesMake(G.Pieces, G.Count, Body, 8, Near, Room) == 8;
            for (int i = 0; i < 8 && bAsGiven; i++)
            {
                const float* const F = Body + i * FlameBodyFloats;
                Balls += (F[12] == (float)FlameShape_Ball) ? 1 : 0;
                Licks += (F[12] == (float)FlameShape_Lick) ? 1 : 0;
                bAsGiven = bAsGiven && F[13] == ((F[12] == (float)FlameShape_Ball) ? 0.8f : 0.6f) && F[14] == 1.0f &&
                           F[6] >= 2.0f && F[6] == F[7];
            }
            Check(bAsGiven && Balls == 4 && Licks == 4, "burning body: each card a flame of its own - four fireballs "
                                                        "and four licks, as bright as the game has each");
        }
        // A burning floor's card standing where a fireplace's sheets do is of the fireplace's body; a lick there
        // is not.
        G.Begin();
        G.Add(FlameCardPiece(Flame_Standing, 832.0f, 532.0f, 260.0f, 30.0f, 30.0f, 1.0f, 300.0f));
        G.Add(FlameCardPiece(Flame_Lick, 832.0f, 532.0f, 260.0f, 11.2f, 22.4f, 1.0f, 300.0f));
        Check(FlameBodiesMake(G.Pieces, G.Count, Body, 8, Near, Room) == 2 && G.AddTogether(Hearth, 2) &&
              G.Count == 4 && FlameBodiesMake(G.Pieces, G.Count, Body, 8, Near, Room) == 2,
              "fireplace: with a card of standing fire in it, it makes no new body");

        // No more bodies than asked for, and nothing written past them.
        {
            FFlamePiece Five[5];
            for (int i = 0; i < 5; i++)
                Five[i] = FlameSheetPiece(100.0f * (float)i, 0.0f, 1.0f, 0.0f, 10.0f, 0.0f, 30.0f, 100.0f);
            float Few[4 * FlameBodyFloats];
            for (int i = 0; i < 4 * FlameBodyFloats; i++)
                Few[i] = -77.0f;
            bool bKept = FlameBodiesMake(Five, 5, Few, 3, Near, Room) == 3;
            for (int i = 3 * FlameBodyFloats; i < 4 * FlameBodyFloats; i++)
                bKept = bKept && Few[i] == -77.0f;
            Check(bKept, "flame bodies: no more made than there is room for");
            Check(FlameBodiesMake(Five, 0, Few, 3, Near, Room) == 0, "flame bodies: no pieces, no bodies");
        }
    }

    // A frame holds as many flames as the game lights. The gather's list is the caller's, and one made longer takes
    // more: a chapel's candles, a hundred and sixty of them, and with them a room of burning things' licks, a thousand.
    {
        enum { CANDLES = 160, LICKS = 1000, MOST = 2 * CANDLES + LICKS };
        static FFlamePiece Held[MOST];
        static int         Room[MOST];
        static float       Body[MOST * FlameBodyFloats];
        FFlameGather G;
        const FFlamePiece Lone = FlameSheetPiece(900.0f, 0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 30.0f, 100.0f);
        Check(!G.Add(Lone) && !G.AddTogether(&Lone, 1) && G.Count == 0, "flame list: with no list, nothing is taken");
        // A first room of sixteen pieces. The seventeenth finds none, and is taken once the list is longer - what was
        // held before with it.
        G.Hold(Held, 16);
        int Took = 0;
        for (int i = 0; i < 16; i++)
            Took += G.Add(FlameCardPiece(Flame_Lick, 10.0f * (float)i, 0.0f, 10.0f, 11.2f, 22.4f, 1.0f, 100.0f))
                    ? 1 : 0;
        const FFlamePiece Next = FlameCardPiece(Flame_Lick, 500.0f, 0.0f, 10.0f, 11.2f, 22.4f, 1.0f, 100.0f);
        Check(Took == 16 && G.Count == 16 && !G.Add(Next) && G.Count == 16, "flame list: a full list takes no more");
        Check(G.Add(Held[3]) && G.Count == 16, "flame list: but a piece it holds already is still that piece");
        G.Hold(Held, MOST);
        Check(G.Add(Next) && G.Count == 17 && Held[16].Mid[0] == 500.0f && Held[0].Mid[0] == 0.0f,
              "flame list: made longer, it takes the piece and keeps what it held");

        // The candles, the far ones offered first as the engine sends them: every one is taken, and each is a body.
        G.Begin();
        Took = 0;
        for (int i = CANDLES - 1; i >= 0; i--)
        {
            const float At = 100.0f + 5.0f * (float)i;
            const float X  = 10.0f * (float)i;
            const FFlamePiece Two[2] = { FlameSheetPiece(X, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 3.0f, At),
                                         FlameSheetPiece(X, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 3.0f, At) };
            Took += G.AddTogether(Two, 2) ? 1 : 0;
        }
        Check(Took == CANDLES && G.Count == 2 * CANDLES, "candles: every one of a hundred and sixty is taken");
        Check(FlameBodiesMake(G.Pieces, G.Count, Body, G.Count, 0.0005f, Room) == CANDLES,
              "candles: and each is a body of flame");
        CheckNear(Body[0], 10.0f * (float)(CANDLES - 1), 0.01f, "candles: the farthest, offered first, the first body");
        CheckNear(Body[(CANDLES - 1) * FlameBodyFloats], 0.0f, 1e-3f,
                  "candles: and the nearest, offered last, is among them");
        // The licks with them, each a flame of its own.
        for (int i = 0; i < LICKS; i++)
            Took += G.Add(FlameCardPiece(Flame_Lick, 7.0f * (float)(i % 40), 300.0f + 9.0f * (float)(i / 40), 10.0f,
                                         11.2f, 22.4f, 1.0f, 300.0f)) ? 1 : 0;
        Check(Took == CANDLES + LICKS && G.Count == MOST, "licks: a thousand more are taken with the candles");
        Check(FlameBodiesMake(G.Pieces, G.Count, Body, G.Count, 0.0005f, Room) == CANDLES + LICKS,
              "licks: each a body of its own, beside the candles'");

        // A fire is taken whole or not at all.
        const FFlamePiece Pair[2] = { FlameSheetPiece(-500.0f, 0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 30.0f, 50.0f),
                                      FlameSheetPiece(-500.0f, 0.0f, 0.0f, 1.0f, 20.0f, 0.0f, 30.0f, 400.0f) };
        Check(!G.AddTogether(Pair, 2) && G.Count == MOST,
              "whole fire: with no room in the list for all its sheets none is taken");
        Check(!G.AddTogether(Pair, 0) && G.Count == MOST, "whole fire: no sheets is no fire to take");
        G.Begin();
        Check(G.AddTogether(Pair, 2) && G.Count == 2 && FlameBodiesMake(G.Pieces, G.Count, Body, 2, 0.0005f, Room) == 1,
              "whole fire: with room it is taken, one body");
        G.Begin();
        enum { SHEETS = 300 };
        static FFlamePiece Many[SHEETS];
        for (int i = 0; i < SHEETS; i++)
            Many[i] = FlameSheetPiece(0.0f, 0.0f, 1.0f, 0.0f, 20.0f + 0.1f * (float)i, 0.0f, 30.0f, 100.0f);
        Check(G.AddTogether(Many, SHEETS) && G.Count == SHEETS &&
              FlameBodiesMake(G.Pieces, G.Count, Body, G.Count, 0.0005f, Room) == 1,
              "whole fire: three hundred sheets on one spot are still one fire");
    }

    // The ball a flame body's light stays inside, and the tiles of the walk's target it is listed in: never fewer
    // than its flame shows in, or the walk - which asks a pixel only its tile's bodies - cuts the flame off there.
    {
        float Mid[3];
        // A fire standing on its foot, 40 long and 30 across, its sheets from 100 up to 160.
        float Stands[FlameBodyFloats] = { 10.0f, 20.0f, 100.0f, 160.0f, 1.0f, 0.0f, 20.0f, 15.0f };
        Stands[12] = (float)FlameShape_Standing;
        float Reach = FlameBodyBall(Stands, false, Mid);
        Check(Mid[0] == 10.0f && Mid[1] == 20.0f, "flame ball: round the body's middle on the level");
        CheckNear(Mid[2], 0.5f * (94.0f + 100.0f + FireWalkReach * 60.0f), 1e-3f,
                  "flame ball: from under the foot to as high as the tongues reach, halfway");
        CheckNear(Reach, sqrtf(20.0f * 20.0f + 57.0f * 57.0f), 1e-3f, "flame ball: out to the cylinder's rim");
        const float Size = 60.0f / 32.0f;
        CheckNear(FlameBodyBall(Stands, true, Mid), (160.0f - Mid[2]) + 20.0f + 60.0f + FireSparkFly * Size, 1e-2f,
                  "flame ball: with sparks, as far as they fly from the flame's top");
        // A fireball of the game's, 16 units from its middle: its heat is read further out than its cylinder.
        float Ball[FlameBodyFloats] = { 0.0f, 0.0f, 34.0f, 66.0f, 1.0f, 0.0f, 16.0f, 16.0f };
        Ball[12] = (float)FlameShape_Ball;
        Reach = FlameBodyBall(Ball, false, Mid);
        CheckNear(Mid[2], 50.0f, 1e-3f, "fireball: its ball round its own middle");
        CheckNear(Reach, FireBallOut * 16.0f * sqrtf(1.0f + FireBallTall * FireBallTall), 1e-2f,
                  "fireball: out to where its heat is read, across and up");
        Check(FlameBodyBall(Ball, true, Mid) == Reach, "fireball: it throws no sparks");
        // A lick, 5.6 from its middle and 22.4 tall.
        float Lick[FlameBodyFloats] = { 0.0f, 0.0f, 0.0f, 22.4f, 1.0f, 0.0f, 5.6f, 5.6f };
        Lick[12] = (float)FlameShape_Lick;
        Reach = FlameBodyBall(Lick, false, Mid);
        CheckNear(Mid[2], 0.5f * (-2.24f + 22.4f + 5.6f + 2.0f), 1e-3f, "lick: from under its foot to over its top");
        Check(Reach * Reach >= 5.6f * 5.6f + (30.0f - Mid[2]) * (30.0f - Mid[2]) - 1e-3f,
              "lick: its ball holds the stretch a ray is walked through");

        // The view of the tool that walks without the game: from the world's origin along +Y, +Z up, on a target of
        // 640 by 360 that is half the engine's picture.
        const float Eye[3] = { 0.0f, 0.0f, 0.0f }, AxisX[3] = { 1.0f, 0.0f, 0.0f }, AxisY[3] = { 0.0f, 0.0f, -1.0f };
        const float AxisZ[3] = { 0.0f, 1.0f, 0.0f }, Screen[2] = { 640.0f, 360.0f }, PerPixel[2] = { 2.0f, 2.0f };
        const float Focal = 640.0f;
        const int   TilesX = CloudTilesAlong(640), TilesY = CloudTilesAlong(360);
        // Whether every point of a ring of the body - Out from its middle on the level, at height Z - that shows on
        // the target shows in one of the body's tiles.
        auto RingIn = [&](const float* Body, float Out, float Z, int X0, int Y0, int X1, int Y1) -> bool
        {
            for (int a = 0; a < 16; a++)
            {
                const float Turn = 6.2831853f * (float)a / 16.0f;
                const float Wx = Body[0] + Out * cosf(Turn), Wy = Body[1] + Out * sinf(Turn);
                if (Wy <= 1.0f)
                    continue;       // beside the eye or behind it
                const float Px = (Screen[0] + Focal * Wx / Wy) / PerPixel[0];
                const float Py = (Screen[1] + Focal * -Z / Wy) / PerPixel[1];
                if (Px < 0.0f || Py < 0.0f || Px >= 640.0f || Py >= 360.0f)
                    continue;       // off the target
                const int Tx = (int)Px / CloudTile, Ty = (int)Py / CloudTile;
                if (Tx < X0 || Tx > X1 || Ty < Y0 || Ty > Y1)
                    return false;
            }
            return true;
        };
        int  X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
        bool bAll = true;
        int  Shown = 0, Narrow = 0;
        for (int i = 0; i < 60; i++)
        {
            // The three bodies at sixty places before the eye, near and far, low and high, to both sides.
            const float Dx = -400.0f + 137.0f * (float)(i % 7), Dy = 60.0f + 95.0f * (float)(i % 11);
            const float Dz = -120.0f + 53.0f * (float)(i % 5);
            float B[3][FlameBodyFloats];
            for (int k = 0; k < FlameBodyFloats; k++)
            {
                B[0][k] = Stands[k];
                B[1][k] = Ball[k];
                B[2][k] = Lick[k];
            }
            for (int b = 0; b < 3; b++)
            {
                B[b][0] += Dx; B[b][1] += Dy; B[b][2] += Dz; B[b][3] += Dz;
                const float* const F = B[b];
                if (!FlameBodyTiles(F, false, Eye, AxisX, AxisY, AxisZ, Focal, Screen, PerPixel, TilesX, TilesY, X0, Y0,
                                    X1, Y1))
                {
                    // In no tile: then no ring of it may show on the target either.
                    X0 = Y0 = 0;
                    X1 = Y1 = -1;
                }
                else
                {
                    Shown++;
                    Narrow += ((X1 - X0 + 1) * (Y1 - Y0 + 1) < TilesX * TilesY) ? 1 : 0;
                }
                const float Tall = (F[3] - F[2] > 1.0f) ? F[3] - F[2] : 1.0f;
                const float Cz   = 0.5f * (F[2] + F[3]);
                if (b == 0)
                    bAll = bAll && RingIn(F, 20.0f, F[2] - 2.0f, X0, Y0, X1, Y1) &&
                           RingIn(F, 20.0f, F[2] + FireWalkReach * Tall, X0, Y0, X1, Y1);
                else if (b == 1)
                    bAll = bAll && RingIn(F, FireBallOut * 16.0f, Cz, X0, Y0, X1, Y1) &&
                           RingIn(F, 0.0f, Cz + FireBallTall * FireBallOut * 16.0f, X0, Y0, X1, Y1) &&
                           RingIn(F, 0.0f, Cz - FireBallTall * FireBallOut * 16.0f, X0, Y0, X1, Y1) &&
                           RingIn(F, 16.0f, F[3] + 0.25f * (F[3] - F[2]) + 2.0f, X0, Y0, X1, Y1);
                else
                    bAll = bAll && RingIn(F, 5.6f, F[2] - 0.1f * Tall, X0, Y0, X1, Y1) &&
                           RingIn(F, 5.6f, F[3] + 0.25f * (F[3] - F[2]) + 2.0f, X0, Y0, X1, Y1);
            }
        }
        Check(bAll, "flame tiles: wherever a body's flame can show on the target, the body is listed");
        Check(Shown > 60 && Narrow * 10 > Shown * 8,
              "flame tiles: and most bodies are listed in a few tiles, not in all");
        // A standing fire with its sparks: listed as far as they fly.
        float Far[FlameBodyFloats];
        for (int k = 0; k < FlameBodyFloats; k++)
            Far[k] = Stands[k];
        Far[1] += 900.0f;
        int Sx0 = 0, Sy0 = 0, Sx1 = 0, Sy1 = 0;
        Check(FlameBodyTiles(Far, false, Eye, AxisX, AxisY, AxisZ, Focal, Screen, PerPixel, TilesX, TilesY, X0, Y0, X1,
                             Y1) &&
              FlameBodyTiles(Far, true, Eye, AxisX, AxisY, AxisZ, Focal, Screen, PerPixel, TilesX, TilesY, Sx0, Sy0,
                             Sx1, Sy1) &&
              Sx0 <= X0 && Sy0 <= Y0 && Sx1 >= X1 && Sy1 >= Y1 && (Sx1 - Sx0) > (X1 - X0),
              "flame tiles: with sparks a standing fire is listed in more tiles, its own among them");
        // Behind the eye a body is in no tile; one the eye stands in is in every tile.
        float Behind[FlameBodyFloats], Around[FlameBodyFloats];
        for (int k = 0; k < FlameBodyFloats; k++)
            Behind[k] = Around[k] = Lick[k];
        Behind[1] = -300.0f;
        Check(!FlameBodyTiles(Behind, false, Eye, AxisX, AxisY, AxisZ, Focal, Screen, PerPixel, TilesX, TilesY, X0, Y0,
                              X1, Y1), "flame tiles: a body behind the eye is in no tile");
        Around[2] = -10.0f;
        Around[3] = 12.4f;
        Check(FlameBodyTiles(Around, false, Eye, AxisX, AxisY, AxisZ, Focal, Screen, PerPixel, TilesX, TilesY, X0, Y0,
                             X1, Y1) && X0 == 0 && Y0 == 0 && X1 == TilesX - 1 && Y1 == TilesY - 1,
              "flame tiles: a body the eye stands in is in every tile");
    }
}

// A few flat rectangles in view space and what the eye sees of them - the covers' depth ReliefClearance reads. A
// rectangle lies where coordinate Axis (0 X, 1 Y, 2 Z) is At, within A0..A1 and B0..B1 of the next two coordinates
// around (Y and Z for X, Z and X for Y, X and Y for Z).
struct FCoverScene
{
    struct FRect { int Axis; float At, A0, A1, B0, B1; };
    FRect Rects[8];
    int   Num = 0;
    void Add(int Axis, float At, float A0, float A1, float B0, float B1)
    {
        if (Num < 8)
            Rects[Num++] = { Axis, At, A0, A1, B0, B1 };
    }
    // The view depth of the nearest rectangle on the eye's ray through Q, or a huge number where the ray meets none.
    float operator()(const FVec3& Q) const
    {
        float       Best  = 1.0e6f;
        const float V[3]  = { Q.X, Q.Y, Q.Z };
        for (int r = 0; r < Num; r++)
        {
            const FRect& R = Rects[r];
            if (V[R.Axis] == 0.0f)
                continue;
            const float T = R.At / V[R.Axis];
            const float A = V[(R.Axis + 1) % 3] * T, B = V[(R.Axis + 2) % 3] * T;
            if (T > 0.0f && A >= R.A0 && A <= R.A1 && B >= R.B0 && B <= R.B1 && Q.Z * T < Best)
                Best = Q.Z * T;
        }
        return Best;
    }
};

static void TestPom()
{
    using namespace RdxMath;

    // A wall standing in the level's X-Z plane, its texture running along X and Z. The mapping axes are NOT unit
    // vectors: their length carries the texture's scale, which is exactly what has to come out the other end.
    FVec3 MapX, MapY;
    MapX.X = 64.0f; MapX.Y = 0.0f; MapX.Z = 0.0f;       // 64 texture units to one of the level's
    MapY.X = 0.0f;  MapY.Y = 0.0f; MapY.Z = 32.0f;
    const FPomFrame F = PomFrameFrom(MapX, MapY, 1.0f / 256.0f, 1.0f / 256.0f);
    Check(F.T.X > 0.999f && F.T.Y < 0.001f, "relief: the axis along the texture comes out a unit vector");
    Check(F.B.Z > 0.999f, "relief: and the one across it");
    Check(F.Du > 0.249f && F.Du < 0.251f, "relief: 64 texture units over 256 is a quarter of the texture a unit");
    Check(F.Dv > 0.124f && F.Dv < 0.126f, "relief: and half that across it");
    Check(F.N.Y * F.N.Y > 0.999f, "relief: the way out of the surface is square to both");

    // Straight on: the eye is on the surface's own normal, so nothing slides at all.
    FVec3 Straight;
    Straight.X = 0.0f; Straight.Y = 100.0f; Straight.Z = 0.0f;
    float PX = 9.0f, PY = 9.0f, PN = 0.0f, PD = 0.0f;
    PomVertex(F, Straight, PX, PY, PN, PD);
    Check(PX > -0.001f && PX < 0.001f && PY > -0.001f && PY < 0.001f,
          "relief: seen straight on, a point one unit deep does not slide");
    Check(PN > 99.9f && PN < 100.1f, "relief: and its outward part is the eye's height over the surface");
    CheckNear(PD, Sqrt(0.25f * 0.125f), 1e-4f, "relief: with it the texture's own rate, the mean of its two");

    // From the side: the further round the eye goes, the further a deep point slides.
    FVec3 Near, Far;
    Near.X = 40.0f; Near.Y = 100.0f; Near.Z = 0.0f;
    Far.X  = 90.0f; Far.Y  = 100.0f; Far.Z  = 0.0f;
    float NearX = 0.0f, NearY = 0.0f, NearN = 0.0f, NearD = 0.0f;
    float FarX = 0.0f, FarY = 0.0f, FarN = 0.0f, FarD = 0.0f;
    PomVertex(F, Near, NearX, NearY, NearN, NearD);
    PomVertex(F, Far, FarX, FarY, FarN, FarD);
    Check(NearX * NearX > 1e-6f, "relief: seen from the side, a deep point does slide");
    Check(FarX * FarX > NearX * NearX, "relief: and further the more the surface is turned away");
    CheckNear(FarN, NearN, 1e-3f, "relief: the eye's height over a flat surface is the same at every corner");
    // The way to the eye is not brought to length one: half way between two corners it is half way between theirs,
    // so the slide worked out per pixel is the slide of that very place - whatever corners the polygon is cut to.
    {
        FVec3 Mid;
        Mid.X = 65.0f; Mid.Y = 100.0f; Mid.Z = 0.0f;
        float MidX = 0.0f, MidY = 0.0f, MidN = 0.0f, MidD = 0.0f;
        PomVertex(F, Mid, MidX, MidY, MidN, MidD);
        CheckNear(0.5f * (NearX + FarX) / (0.5f * (NearN + FarN)), MidX / MidN, 1e-5f,
                  "relief: what two corners hand over, halved, is the slide of the place half way between them");
        CheckNear(MidX / MidN, -65.0f / 100.0f * 0.25f, 1e-5f,
                  "relief: ... the sideways way over the outward one, at the texture's rate");
    }

    // The division that could run away is done per pixel in the shader: edge on the outward part goes towards
    // nought, and the sideways parts stay what they are.
    FVec3 Edge;
    Edge.X = 1000.0f; Edge.Y = 1.0f; Edge.Z = 0.0f;
    float EdgeX = 0.0f, EdgeY = 0.0f, EdgeN = 0.0f, EdgeD = 0.0f;
    PomVertex(F, Edge, EdgeX, EdgeY, EdgeN, EdgeD);
    CheckNear(EdgeX, -250.0f, 1e-2f, "relief: edge on, the sideways part is the way along the surface at its rate");
    CheckNear(EdgeN, 1.0f, 1e-4f, "relief: and the outward part the little the eye stands over the surface");
    // A surface facing away is turned round rather than handed over negative, so the shader has one case.
    FVec3 Behind;
    Behind.X = 0.0f; Behind.Y = -100.0f; Behind.Z = 0.0f;
    float BehX = 0.0f, BehY = 0.0f, BehN = 0.0f, BehD = 0.0f;
    PomVertex(F, Behind, BehX, BehY, BehN, BehD);
    Check(BehN > 99.9f, "relief: a surface facing away still has the eye over it, not under");

    // The slide follows the texture's own axes, whichever way their cross product points. In the engine's view space
    // X runs right, Y down and Z away from the eye, so an upright texture on a wall in front (U right, V down) has
    // U x V pointing into the wall. A point to the right of the eye has the eye towards minus U there, and on a
    // mirrored texture towards plus U - with the outward part positive both times.
    FVec3 AxisRight, AxisDown, AxisLeft, RightOfEye;
    AxisRight.X = 1.0f;  AxisRight.Y = 0.0f; AxisRight.Z = 0.0f;
    AxisDown.X  = 0.0f;  AxisDown.Y  = 1.0f; AxisDown.Z  = 0.0f;
    AxisLeft.X  = -1.0f; AxisLeft.Y  = 0.0f; AxisLeft.Z  = 0.0f;
    RightOfEye.X = 40.0f; RightOfEye.Y = 0.0f; RightOfEye.Z = 100.0f;
    float WX = 0.0f, WY = 0.0f, WN = 0.0f, WD = 0.0f;
    PomVertex(PomFrameFrom(AxisRight, AxisDown, 1.0f, 1.0f), RightOfEye, WX, WY, WN, WD);
    Check(WX < -30.0f && WN > 90.0f, "relief: seen from its left, an upright texture has the eye towards minus U");
    PomVertex(PomFrameFrom(AxisLeft, AxisDown, 1.0f, 1.0f), RightOfEye, WX, WY, WN, WD);
    Check(WX > 30.0f && WN > 90.0f, "relief: and on a mirrored one towards plus U - the slide follows the texture");

    // Which set of height maps: the name, and stepping between them.
    bool bKnown = false;

    Check(PomMapSide(2048, 2) == 1024 && PomMapSide(2048, 4) == 512, "relief: a smaller map's side");
    Check(PomMapSide(256, 4) == 128 && PomMapSide(512, 4) == 128, "relief: no side goes below 128");
    Check(PomMapSide(128, 2) == 128 && PomMapSide(64, 4) == 64, "relief: a side at 128 or under stays as it is");
    Check(PomMapSide(1024, 1) == 1024, "relief: the full-size sets keep the texture's size");

    // How the relief is drawn.
    Check(ParseGeometry("DisplacementMapping", bKnown) == Geometry_Tess && bKnown,
          "geometry: DisplacementMapping is understood");
    Check(ParseGeometry("pom", bKnown) == Geometry_Pom && bKnown, "geometry: and POM, whatever the case");
    Check(ParseGeometry("Off", bKnown) == Geometry_Off && bKnown, "geometry: and Off");
    Check(ParseGeometry("True", bKnown) == Geometry_Off && !bKnown, "geometry: anything else is Off, and says so");
    Check(GeometryStep(Geometry_Tess, 1) == Geometry_Off && GeometryStep(Geometry_Off, -1) == Geometry_Tess,
          "geometry: the menu steps round both ways");

    // A texture laid on once, and one that tiles.
    float TileU = 0.0f, TileV = 0.0f;
    Check(PomOnceMapped(0.0f, 1.0f, 0.0f, 1.0f, TileU, TileV) && TileU == 0.0f && TileV == 0.0f,
          "relief: a sign mapped exactly once is laid on once");
    Check(PomOnceMapped(2.995f, 4.004f, -1.0f, -0.5f, TileU, TileV) && TileU == 3.0f && TileV == -1.0f,
          "relief: and so is one a hair over its tile, in the tile it is in, below zero too");
    Check(!PomOnceMapped(0.3f, 1.3f, 0.0f, 1.0f, TileU, TileV), "relief: one that crosses a tile's edge tiles");
    Check(!PomOnceMapped(0.0f, 4.0f, 0.0f, 1.0f, TileU, TileV), "relief: and a wall repeating it four times does");

    // The relief list's lines: a plain name, a quoted one with a space in it, and what is not a line at all.
    char LKey[96];
    bool LOn = false;
    float LDepth = 0.0f;
    Check(ParsePomListLine("UNATCO.un_BlastDoor_A   on   6.0", LKey, 96, LOn, LDepth) && LOn && LDepth > 5.99f &&
          LDepth < 6.01f && LKey[0] == 'u' && LKey[20] == 'a' && LKey[21] == 0, "relief list: name, on, depth");
    Check(ParsePomListLine("\"CoreTexCeramic.Clay ReddRoofT\"   on   2.0", LKey, 96, LOn, LDepth) && LOn &&
          LKey[19] == ' ' && LKey[28] == 't' && LKey[29] == 0, "relief list: a quoted name keeps its space");
    Check(ParsePomListLine("\"CoreTexEarth.ClenLtan Adobe_\" off", LKey, 96, LOn, LDepth) && !LOn &&
          LDepth > PomDepthWorld - 0.01f && LDepth < PomDepthWorld + 0.01f, "relief list: off, with no depth given");
    Check(ParsePomListLine("Pkg.Wagon on 2,5 # a note", LKey, 96, LOn, LDepth) && LOn && LDepth > 2.49f &&
          LDepth < 2.51f && LKey[8] == 'n' && LKey[9] == 0, "relief list: a comma, a note, a name ending in on");
    Check(!ParsePomListLine("# only a note", LKey, 96, LOn, LDepth) && !ParsePomListLine("   ", LKey, 96, LOn, LDepth) &&
          !ParsePomListLine("Pkg.Name", LKey, 96, LOn, LDepth) && !ParsePomListLine("Pkg.Name onward", LKey, 96, LOn, LDepth),
          "relief list: a blank line, a note, a name alone, a word that only starts with on");
    Check(!ParsePomListLine("\"Pkg.Name on 2.0", LKey, 96, LOn, LDepth) &&
          !ParsePomListLine("CoreTexCeramic.Clay ReddRoofT on 2.0", LKey, 96, LOn, LDepth),
          "relief list: an unclosed quote, and a space in a name without quotes, are not lines");

    // Geometry=Tessellation: which edges a surface shares with itself. A wall 200 wide, 100 deep in front of the eye,
    // cut by the BSP into a left and a right half - and the right half cut again, top and bottom, so its corner lies in
    // the middle of the left half's edge (a T-junction). Wound the same way round, as one surface's polygons are.
    {
        const FVec3 W[] = {
            { -100.0f, -50.0f, 100.0f }, { 0.0f, -50.0f, 100.0f }, { 0.0f, 50.0f, 100.0f }, { -100.0f, 50.0f, 100.0f },
            { 0.0f, -50.0f, 100.0f }, { 100.0f, -50.0f, 100.0f }, { 100.0f, 0.0f, 100.0f }, { 0.0f, 0.0f, 100.0f },
            { 0.0f, 0.0f, 100.0f }, { 100.0f, 0.0f, 100.0f }, { 100.0f, 50.0f, 100.0f }, { 0.0f, 50.0f, 100.0f } };
        const int  WS[3] = { 0, 4, 8 };
        const int  WC[3] = { 4, 4, 4 };
        bool Sh[12];
        ReliefSharedEdges(W, WS, WC, 3, 0.1f, Sh);
        Check(!Sh[0] && Sh[1] && !Sh[2] && !Sh[3], "relief tess: the left half shares only the edge the cut made");
        Check(!Sh[4] && !Sh[5] && Sh[6] && Sh[7], "relief tess: the lower right quarter shares its top and its left");
        Check(Sh[8] && !Sh[9] && !Sh[10] && Sh[11], "relief tess: and the upper right one its bottom and its left");
        // The same halves, the right one moved a unit out of the plane: not the same line any more.
        FVec3 Off[12];
        for (int i = 0; i < 12; i++)
        {
            Off[i] = W[i];
            if (i >= 4)
                Off[i].Z += 1.0f;
        }
        ReliefSharedEdges(Off, WS, WC, 3, 0.1f, Sh);
        Check(!Sh[1] && !Sh[7] && !Sh[11], "relief tess: an edge a unit off the line is not shared");
        // Two polygons side by side along the screen's edge, both clipped there: their clipped edges lie on one line
        // but run the same way, and do not overlap - neither is shared.
        const FVec3 Clip[] = {
            { -50.0f, -50.0f, 50.0f }, { 0.0f, -50.0f, 50.0f }, { 0.0f, 0.0f, 50.0f }, { -50.0f, 0.0f, 50.0f },
            { -50.0f, 0.0f, 50.0f }, { 0.0f, 0.0f, 50.0f }, { 0.0f, 50.0f, 50.0f }, { -50.0f, 50.0f, 50.0f } };
        const int CS[2] = { 0, 4 };
        const int CC[2] = { 4, 4 };
        bool CSh[8];
        ReliefSharedEdges(Clip, CS, CC, 2, 0.1f, CSh);
        Check(!CSh[3] && !CSh[7] && CSh[2] && CSh[4], "relief tess: edges along one line side by side are not shared");
        // A short edge - a third of a unit - on the surface's own rim stays held at half a unit's tolerance: only an
        // edge of no length at all is let go for having none.
        const FVec3 Thin[] = { { 0.0f, 0.0f, 20.0f }, { 0.3f, 0.0f, 20.0f }, { 0.3f, 10.0f, 20.0f }, { 0.0f, 10.0f, 20.0f } };
        const int  TS[1] = { 0 };
        const int  TC[1] = { 4 };
        bool TSh[4];
        ReliefSharedEdges(Thin, TS, TC, 1, 0.5f, TSh);
        Check(!TSh[0] && !TSh[1] && !TSh[2] && !TSh[3], "relief tess: a short edge of the rim is held all the same");

        FVec3 N;
        Check(ReliefNormalTowardsEye(W, WS, WC, 3, N) && N.Z < -0.999f,
              "relief tess: a wall in front of the eye has its normal towards the eye");
        // The same wall wound the other way round: the normal still faces the eye.
        const FVec3 R[] = { { -100.0f, 50.0f, 100.0f }, { 100.0f, 50.0f, 100.0f }, { 100.0f, -50.0f, 100.0f },
                            { -100.0f, -50.0f, 100.0f } };
        const int RS[1] = { 0 };
        const int RC[1] = { 4 };
        Check(ReliefNormalTowardsEye(R, RS, RC, 1, N) && N.Z < -0.999f, "relief tess: whichever way it is wound");
        // A floor under the eye (view Y is down): its normal points up the screen, towards negative Y.
        const FVec3 Floor[] = { { -100.0f, 40.0f, 50.0f }, { 100.0f, 40.0f, 50.0f }, { 100.0f, 40.0f, 500.0f },
                                { -100.0f, 40.0f, 500.0f } };
        Check(ReliefNormalTowardsEye(Floor, RS, RC, 1, N) && N.Y < -0.999f, "relief tess: a floor's normal points up");
        const FVec3 Line[] = { { 0.0f, 0.0f, 10.0f }, { 1.0f, 0.0f, 10.0f }, { 2.0f, 0.0f, 10.0f } };
        const int LC[1] = { 3 };
        Check(!ReliefNormalTowardsEye(Line, RS, LC, 1, N), "relief tess: a polygon with no area has no normal");

        // A square cut by the plane X >= 0: half of it is left, and the one edge the cut made is marked as made.
        FVec3 Sq[8] = { { -10.0f, -10.0f, 50.0f }, { 10.0f, -10.0f, 50.0f }, { 10.0f, 10.0f, 50.0f },
                        { -10.0f, 10.0f, 50.0f } };
        bool  SqMade[8] = { false, false, false, false };
        const FVec3 CutX = { 1.0f, 0.0f, 0.0f };
        const int Left = ReliefClipByPlane(Sq, SqMade, 4, 8, CutX, 0.0f);
        int Cuts = 0, Own = 0;
        bool Inside = true;
        for (int i = 0; i < Left; i++)
        {
            Cuts += SqMade[i] ? 1 : 0;
            Own  += SqMade[i] ? 0 : 1;
            Inside = Inside && Sq[i].X >= -1e-4f;
        }
        Check(Left == 4 && Cuts == 1 && Own == 3 && Inside, "relief tess: a cut leaves half a square, one edge its own");
        // The made edge is the one along X = 0.
        bool MadeOnLine = false;
        for (int i = 0; i < Left; i++)
            if (SqMade[i])
                MadeOnLine = Sq[i].X > -1e-4f && Sq[i].X < 1e-4f && Sq[(i + 1) % Left].X > -1e-4f &&
                             Sq[(i + 1) % Left].X < 1e-4f;
        Check(MadeOnLine, "relief tess: and the edge the cut made runs along the cut");
        // A second cut keeps the first one's mark; a plane missing it changes nothing; one with all of it outside
        // leaves nothing.
        const FVec3 CutY = { 0.0f, 1.0f, 0.0f };
        const int Quarter = ReliefClipByPlane(Sq, SqMade, Left, 8, CutY, 0.0f);
        Cuts = 0;
        for (int i = 0; i < Quarter; i++)
            Cuts += SqMade[i] ? 1 : 0;
        Check(Quarter == 4 && Cuts == 2, "relief tess: two cuts, a quarter left, two edges made");
        Check(ReliefClipByPlane(Sq, SqMade, Quarter, 8, CutX, 100.0f) == Quarter, "relief tess: a plane that misses cuts nothing");
        Check(ReliefClipByPlane(Sq, SqMade, Quarter, 8, CutX, -100.0f) == 0, "relief tess: one that takes all leaves nothing");

        // The view's planes, widened by two units: the screen's middle is in; at a depth where a unit is ten pixels, a
        // point two pixels past the left edge is still in, and one thirty pixels past it - over two units out - is not.
        FVec3 VN[5];
        float VD[5];
        ReliefViewPlanes(1000.0f, 960.0f, 540.0f, 1920.0f, 1080.0f, 2.0f, VN, VD);
        auto InView = [&](const FVec3& P) {
            for (int i = 0; i < 5; i++)
                if (Dot(VN[i], P) + VD[i] < 0.0f)
                    return false;
            return true;
        };
        const FVec3 Mid  = { 0.0f, 0.0f, 100.0f };
        const FVec3 Past2 = { (-2.0f - 960.0f) * 100.0f / 1000.0f, 0.0f, 100.0f };
        const FVec3 Past30 = { (-30.0f - 960.0f) * 100.0f / 1000.0f, 0.0f, 100.0f };
        const FVec3 Eye  = { 0.0f, 0.0f, 0.5f };
        Check(InView(Mid) && InView(Past2) && !InView(Past30), "relief tess: the view widened by the relief's reach");
        Check(!InView(Eye), "relief tess: and nothing nearer than a unit to the eye");

        // The distance to a held edge: across its middle, past either end, and on it.
        const FVec3 SegA = { 0.0f, 0.0f, 10.0f }, SegB = { 10.0f, 0.0f, 10.0f };
        const FVec3 Above = { 5.0f, 3.0f, 10.0f }, Past = { 13.0f, 4.0f, 10.0f }, OnIt = { 7.0f, 0.0f, 10.0f };
        Check(ReliefSegmentDistance(Above, SegA, SegB) > 2.999f && ReliefSegmentDistance(Above, SegA, SegB) < 3.001f &&
              ReliefSegmentDistance(Past, SegA, SegB) > 4.999f && ReliefSegmentDistance(Past, SegA, SegB) < 5.001f &&
              ReliefSegmentDistance(OnIt, SegA, SegB) < 1e-5f && ReliefSegmentDistance(SegB, SegA, SegB) == 0.0f,
              "relief tess: the distance to a held edge, across it, past its end and on it");
    }

    // The rim at a held edge, by the wall beyond it: up to a wall standing at the edge, with the relief's top moved to
    // lie on it when it leans; the band where the wall is flatter than the band, or stands off by a whole gap.
    {
        const float Deg = 3.14159265f / 180.0f;
        float Width = -1.0f, Cot = -1.0f;
        ReliefRimFromWall(0.0f, 1.0f, 0.0f, 2.0f, 4.0f, Width, Cot);
        Check(Width == 0.0f && Cot == 0.0f, "relief rim: a wall square to the surface at the edge takes it up to itself");
        ReliefRimFromWall(cosf(110.0f * Deg), sinf(110.0f * Deg), 0.0f, 2.0f, 4.0f, Width, Cot);
        Check(Width == 0.0f && Cot < -0.36f && Cot > -0.37f, "relief rim: a wall leaning away moves the top over its foot");
        ReliefRimFromWall(cosf(70.0f * Deg), sinf(70.0f * Deg), 0.0f, 2.0f, 4.0f, Width, Cot);
        Check(Width == 0.0f && Cot > 0.36f && Cot < 0.37f, "relief rim: a wall leaning over moves the top in under it");
        ReliefRimFromWall(cosf(160.0f * Deg), sinf(160.0f * Deg), 0.0f, 2.0f, 4.0f, Width, Cot);
        Check(Width == 1.0f && Cot == 0.0f, "relief rim: a wall flatter than the band leaves the band");
        ReliefRimFromWall(cosf(10.0f * Deg), sinf(10.0f * Deg), 0.0f, 2.0f, 4.0f, Width, Cot);
        Check(Width == 0.0f && Cot == 2.0f, "relief rim: a wall leaning far over is taken at the band's slope");
        ReliefRimFromWall(0.0f, 1.0f, 2.0f, 2.0f, 4.0f, Width, Cot);
        Check(Width == 0.5f && Cot == 0.0f, "relief rim: a gap half open brings half the band back");
        ReliefRimFromWall(cosf(110.0f * Deg), sinf(110.0f * Deg), 1.0f, 2.0f, 4.0f, Width, Cot);
        Check(Width == 0.25f && Cot < -0.27f && Cot > -0.28f, "relief rim: the top leaves the wall as the gap opens");
        ReliefRimFromWall(0.0f, 1.0f, 4.0f, 2.0f, 4.0f, Width, Cot);
        Check(Width == 1.0f && Cot == 0.0f, "relief rim: a whole gap is the band");
        ReliefRimFromWall(1.0f, 0.0f, 0.0f, 2.0f, 4.0f, Width, Cot);
        Check(Width == 1.0f && Cot == 0.0f, "relief rim: nothing rising is no wall");
    }

    // The relief under what stands above it (ReliefClearance): walked up the point's own line in view space, held under
    // a poster, a mirror or a rug - and never by a wall beyond the surface's edge, nor by anything between the eye and it.
    {
        const float Hair  = 4096.0f / 4095.0f;      // the flat surface under the patches, a 4096th further back
        const float Pixel = 1.0f / 1920.0f;         // a 4K picture's pixel, 90 degrees across
        const float Lift  = 2.0f;
        const FVec3 Back  = { 0.0f, 0.0f, -1.0f };  // a wall facing the eye, lifted towards it
        const FVec3 Up    = { 0.0f, -1.0f, 0.0f };  // a floor below the eye, lifted up

        FCoverScene Wall;
        Wall.Add(2, 100.0f * Hair, -200.0f, 200.0f, -200.0f, 200.0f);
        const FVec3 Mid = { 0.0f, 0.0f, 100.0f };
        Check(ReliefClearance(Mid, Back, Back, Lift, Pixel, Wall) == Lift, "relief cover: a bare wall lifts all the way");

        FCoverScene Poster = Wall;
        Poster.Add(2, 99.5f, -10.0f, 10.0f, -10.0f, 10.0f);
        Poster.Add(2, 99.5f, 30.0f, 50.0f, -10.0f, 10.0f);
        const float Under = ReliefClearance(Mid, Back, Back, Lift, Pixel, Poster);
        Check(Under > 0.38f && Under < 0.41f, "relief cover: a poster half a unit out holds the relief under it");
        const FVec3 Beside = { 15.0f, 0.0f, 100.0f };
        Check(ReliefClearance(Beside, Back, Back, Lift, Pixel, Poster) == Lift,
              "relief cover: beside the poster the relief is whole");
        const FVec3 Slanted = { 40.0f, 0.0f, 100.0f }, NearEdge = { 30.02f, 0.0f, 100.0f }, FarEdge = { 49.98f, 0.0f, 100.0f };
        const float AtSlant = ReliefClearance(Slanted, Back, Back, Lift, Pixel, Poster);
        const float AtNear  = ReliefClearance(NearEdge, Back, Back, Lift, Pixel, Poster);
        const float AtFar   = ReliefClearance(FarEdge, Back, Back, Lift, Pixel, Poster);
        Check(AtSlant > 0.38f && AtSlant < 0.41f && AtNear > 0.38f && AtNear < 0.41f && AtFar > 0.38f && AtFar < 0.41f,
              "relief cover: seen at a slant, up to both of the poster's edges");
        const FVec3 Past = { 50.5f, 0.0f, 100.0f };
        Check(ReliefClearance(Past, Back, Back, Lift, Pixel, Poster) == Lift,
              "relief cover: past the poster's far edge the relief is whole");
        Check(ReliefClearance(Mid, Back, Back, 0.3f, Pixel, Poster) == 0.3f,
              "relief cover: a relief that stays below the poster is left alone");

        FCoverScene Mirror = Wall;
        Mirror.Add(2, 99.8f, -10.0f, 10.0f, -10.0f, 10.0f);
        const float UnderMirror = ReliefClearance(Mid, Back, Back, Lift, Pixel, Mirror);
        Check(UnderMirror > 0.08f && UnderMirror < 0.11f, "relief cover: a mirror a fifth of a unit out, the same");

        FCoverScene Crate = Wall;
        Crate.Add(2, 50.0f, -5.0f, 5.0f, -5.0f, 5.0f);
        Check(ReliefClearance(Mid, Back, Back, Lift, Pixel, Crate) == Lift,
              "relief cover: a crate between the eye and the wall does not hold it down");

        // A floor ten units below the eye, seen far off at a graze and near at a steeper angle, with a wall beyond it.
        FCoverScene Floor;
        Floor.Add(1, 10.0f * Hair, 0.0f, 102.0f, -50.0f, 50.0f);
        Floor.Add(2, 102.0f, -50.0f, 50.0f, -50.0f, 10.0f * Hair);
        FCoverScene Rug = Floor;
        Rug.Add(1, 9.5f, 0.0f, 101.0f, -20.0f, 20.0f);
        const FVec3 Graze = { 0.0f, 10.0f, 100.0f }, Close = { 0.0f, 10.0f, 20.0f };
        const float UnderRugFar   = ReliefClearance(Graze, Up, Up, Lift, Pixel, Rug);
        const float UnderRugClose = ReliefClearance(Close, Up, Up, Lift, Pixel, Rug);
        Check(UnderRugFar > 0.38f && UnderRugFar < 0.41f && UnderRugClose > 0.43f && UnderRugClose < 0.45f,
              "relief cover: a rug holds the floor's relief under it, at a graze and nearer");
        const FVec3 ByWall = { 0.0f, 10.0f, 101.0f };
        Check(ReliefClearance(Graze, Up, Up, Lift, Pixel, Floor) == Lift &&
              ReliefClearance(ByWall, Up, Up, Lift, Pixel, Floor) == Lift,
              "relief cover: the wall beyond the floor's edge does not hold the floor down");
        FCoverScene Box = Floor;
        Box.Add(2, 98.0f, -5.0f, 5.0f, 8.0f, 10.0f * Hair);
        Check(ReliefClearance(Graze, Up, Up, Lift, Pixel, Box) == Lift,
              "relief cover: nor does a box standing on the floor in front of the point");
        Check(ReliefClearance(Mid, Back, Back, 0.0f, Pixel, Poster) == 0.0f, "relief cover: nothing lifted, nothing held");
    }

    // LampShadows: the relief is lit by the shadows' own pick, and every light of the largest one fits its rows.
    Check(ShadowPresetFor(ShadowQuality_High).MaxLights <= ReliefLampMax &&
          ShadowPresetFor(ShadowQuality_Medium).MaxLights <= ReliefLampMax &&
          ShadowPresetFor(ShadowQuality_Low).MaxLights <= ReliefLampMax,
          "relief lamps: every light of each quality's pick has a row");

    // The fog of a volumetric light: the integral of 1 - r^2/R^2 through its sphere, over R.
    {
        const FVec3 Eye = { -200.0f, 0.0f, 0.0f }, Dir = { 1.0f, 0.0f, 0.0f }, Centre = { 0.0f, 0.0f, 0.0f };
        CheckNear(FogTau(Eye, Dir, 1000.0f, Centre, 100.0f), 4.0f / 3.0f, 1e-4f,
                  "fog: a ray through the whole sphere, through its middle, is 4/3");
        CheckNear(FogTau(Eye, Dir, 200.0f, Centre, 100.0f), 2.0f / 3.0f, 1e-4f,
                  "fog: a ray that stops at the middle is half of it");
        Check(FogTau(Eye, Dir, 50.0f, Centre, 100.0f) == 0.0f, "fog: a ray that stops before the sphere has none");
        const FVec3 Off = { -200.0f, 150.0f, 0.0f };
        Check(FogTau(Off, Dir, 1000.0f, Centre, 100.0f) == 0.0f, "fog: a ray that passes the sphere has none");
        const FVec3 Inside = { 0.0f, 0.0f, 0.0f };
        CheckNear(FogTau(Inside, Dir, 1000.0f, Centre, 100.0f), 2.0f / 3.0f, 1e-4f,
                  "fog: from the middle outwards is half");
        float C[3], A;
        FogFactorsGuess(50, 60, 140, 50, C, A);
        Check(A > 0.33f && A < 0.36f && C[2] > C[1] && C[1] > C[0] && C[0] > 0.0f,
              "fog: the guess for a thick blue light: blue strongest, the alpha as measured");
        FogFactorsGuess(91, 5, 156, 220, C, A);
        Check(A > 0.011f && A < 0.015f, "fog: the guess for a thin light: hardly any alpha, as measured");
    }

    // The Bloom key: Off and On, the earlier values, and what it does with anything else.
    {
        bool bK = false;
        Check(ParseBloom("Off", bK) == Bloom_Off && bK && ParseBloom(" on", bK) == Bloom_On && bK &&
              ParseBloom("ON", bK) == Bloom_On && bK, "bloom: the values read in any case, spaces before them allowed");
        Check(ParseBloom("True", bK) == Bloom_Off && !bK &&
              ParseBloom("Full", bK) == Bloom_On && bK && ParseBloom("coronas", bK) == Bloom_On && bK,
              "bloom: the earlier Full and Coronas are On; True is no word of the key - unknown, Off");
        Check(ParseBloom("Coronasish", bK) == Bloom_Off && !bK && ParseBloom("Onn", bK) == Bloom_Off && !bK &&
              ParseBloom("", bK) == Bloom_Off && !bK, "bloom: anything else is Off, and said to be unknown");
        Check(BloomStep(Bloom_Off, 1) == Bloom_On && BloomStep(Bloom_On, 1) == Bloom_Off &&
              BloomStep(Bloom_Off, -1) == Bloom_On, "bloom: the menu steps round both ways");
        wchar_t Wide[16];
        BloomName(Bloom_On, Wide, 16);
        Check(Wide[0] == L'O' && Wide[1] == L'n' && Wide[2] == 0, "bloom: the name the file is written with");
    }

    // The LensFlare key: Off and On, the earlier values, and anything else.
    {
        bool bK = false;
        Check(ParseLensFlare("Off", bK) == LensFlare_Off && bK && ParseLensFlare(" on", bK) == LensFlare_On && bK &&
              ParseLensFlare("ON", bK) == LensFlare_On && bK,
              "lens flare: the values read in any case, spaces before them allowed");
        Check(ParseLensFlare("True", bK) == LensFlare_Off && !bK &&
              ParseLensFlare("CORONAS", bK) == LensFlare_On && bK &&
              ParseLensFlare("picture", bK) == LensFlare_On && bK,
              "lens flare: the old switch's True and the earlier Coronas and Picture are On, False is Off");
        Check(ParseLensFlare("Lamps", bK) == LensFlare_Off && !bK, "lens flare: anything else is Off, and said to be unknown");
        Check(LensFlareStep(LensFlare_Off, 1) == LensFlare_On && LensFlareStep(LensFlare_On, 1) == LensFlare_Off &&
              LensFlareStep(LensFlare_Off, -1) == LensFlare_On, "lens flare: the menu steps round both ways");
        wchar_t Wide[16];
        LensFlareName(LensFlare_On, Wide, 16);
        Check(Wide[0] == L'O' && Wide[1] == L'n' && Wide[2] == 0, "lens flare: the name the file is written with");
    }

    // The LampShadows key: its four values, the old True and False, and what it does with anything else.
    {
        bool bK = false;
        Check(ParseLampShadows("Off", bK) == LampShadows_Off && bK && ParseLampShadows(" strong", bK) == LampShadows_Strong &&
              bK && ParseLampShadows("SOFT", bK) == LampShadows_Soft && bK,
              "lamp shadows: the values read in any case, spaces before them allowed");
        Check(ParseLampShadows("True", bK) == LampShadows_Medium && !bK, "lamp shadows: True is no word of the key");
        Check(ParseLampShadows("Brightest", bK) == LampShadows_Medium && !bK &&
              ParseLampShadows("Mediumish", bK) == LampShadows_Medium && !bK,
              "lamp shadows: anything else is Medium, and said to be unknown");
        Check(LampShadowsStep(LampShadows_Strong, 1) == LampShadows_Off &&
              LampShadowsStep(LampShadows_Off, -1) == LampShadows_Strong, "lamp shadows: the menu steps round both ways");
        Check(ParseGrass("Off", bK) == Grass_Off && bK && ParseGrass(" medium", bK) == Grass_Medium && bK &&
                  ParseGrass("HIGH", bK) == Grass_High && bK && ParseGrass("True", bK) == Grass_Low && !bK,
              "grass: the values read in any case; True is no word of the key - unknown, Low");
        Check(ParseGrass("Lush", bK) == Grass_Low && !bK, "grass: anything else is Low, and said to be unknown");
        Check(GrassStep(Grass_High, 1) == Grass_Off && GrassStep(Grass_Off, -1) == Grass_High &&
                  GrassStep(Grass_Medium, 1) == Grass_High,
              "grass: the menu steps round both ways, High the last");
        Check(GrassReachOf(Grass_Low) == 1.0f && GrassReachOf(Grass_Medium) == 1.5f && GrassReachOf(Grass_High) == 2.0f,
              "grass: Low as far as the grass as it was made, Medium half as far again, High twice");
        bool bSameGrass = GrassBladesOf(Grass_Off) == 0 && GrassThinFarOf(Grass_High) == 2.5f;
        for (int L = Grass_Low; L < Grass_Count; L++)
            bSameGrass = bSameGrass && GrassClumpsOf((EGrass)L) == 2 &&
                         GrassBladesOf((EGrass)L) == GrassClumpsOf((EGrass)L) * (GrassBladesPerRoot + 2) &&
                         GrassThinFarOf((EGrass)L) > 1.0f &&
                         fabsf(GrassReachOf((EGrass)L) / GrassThinFarOf((EGrass)L) -
                               GrassReachOf(Grass_High) / GrassThinFarOf(Grass_High)) < 1e-6f;
        Check(bSameGrass, "grass: Low and Medium High's grass near the eye - two clumps to a root's cell, the further "
                          "blades thinning out where High's do - only the grass as it was made reaching less far");
        Check(ParseTrees("Off") == Trees_Off && ParseTrees(" low") == Trees_Low && ParseTrees("MEDIUM") == Trees_Medium &&
                  ParseTrees("High") == Trees_High,
              "trees: the levels read in any case");
        Check(ParseTrees("True") < 0 && ParseTrees("Lush") < 0, "trees: any other word unknown");
        {
            bool bNames = true;
            for (int L = 0; L < Trees_Count; L++)
            {
                char N[16];
                TreesName(L, N, 16);
                bNames = bNames && ParseTrees(N) == L;
            }
            Check(bNames, "trees: each level read back as it is written");
        }
        Check(TreeFullReachOf(Trees_High) == 10.0f && TreeLeastShareOf(Trees_High) == 0.12f &&
                  TreeThinPixelsOf(Trees_High) == 0.0f,
              "trees: High's far plants as they always were - every leaf within ten crowns' reach, 0.12 of them at the "
              "least, the bark whole at every distance");
        Check(TreeFullReachOf(Trees_Low) < TreeFullReachOf(Trees_Medium) &&
                  TreeFullReachOf(Trees_Medium) < TreeFullReachOf(Trees_High) &&
                  TreeLeastShareOf(Trees_Low) < TreeLeastShareOf(Trees_Medium) &&
                  TreeLeastShareOf(Trees_Medium) < TreeLeastShareOf(Trees_High) &&
                  TreeThinPixelsOf(Trees_Low) > TreeThinPixelsOf(Trees_Medium) && TreeThinPixelsOf(Trees_Medium) > 0.0f,
              "trees: Low fewer leaves than Medium, from nearer on, and only the thicker wood from nearer");
        {
            // The trees' wind (TreeGen.h's SwayOf): a branch's first node sways as the node it grows from - its foot
            // keeps to it - in both ways of growing; and the sums go through halfs nearly whole.
            Check(RdxTree::FloatOfHalf(RdxTree::HalfOf(1.5f)) == 1.5f &&
                      RdxTree::FloatOfHalf(RdxTree::HalfOf(-0.25f)) == -0.25f &&
                      fabsf(RdxTree::FloatOfHalf(RdxTree::HalfOf(3.14159f)) - 3.14159f) < 0.002f &&
                      RdxTree::FloatOfHalf(RdxTree::HalfOf(0.0f)) == 0.0f &&
                      fabsf(RdxTree::FloatOfHalf(RdxTree::HalfOf(1e-6f)) - 1e-6f) < 1e-7f,
                  "trees' wind: a half round trip");
            for (int Way = 0; Way < 2; Way++)
            {
                const RdxTree::FShape S = RdxTree::ShapeOf(RdxTree::Kind_Tree1, 250.0f, 120.0f);
                static RdxTree::FGrid Grid;
                RdxTree::FPlant       P;
                RdxTree::FMesh        M;
                const bool bGrown = Way ? RdxTree::GrowEz(S, 1234u, P, M, 48.0f) : RdxTree::Grow(S, 1234u, P, Grid);
                RdxTree::TList<RdxTree::FSway> Sw;
                bool  bOk    = bGrown && RdxTree::SwayOf(P, Sw) && Sw.Num == P.Nodes.Num;
                int   Joints = 0;
                float Most   = 0.0f;
                for (int i = 0; bOk && i < P.Nodes.Num; i++)
                {
                    const RdxTree::FNode& N = P.Nodes[i];
                    Most = (Sw[i].S0 > Most) ? Sw[i].S0 : Most;
                    if (N.Branch != i || N.Parent < 0)
                        continue;
                    Joints++;
                    const RdxTree::FSway& Own  = Sw[i];
                    const RdxTree::FSway& From = Sw[N.Parent];
                    bOk = Own.S0 == From.S0 && Own.C1 == From.C1 && Own.S1 == From.S1 && Own.C2 == From.C2 &&
                          Own.S2 == From.S2 && Own.C3 == From.C3 && Own.S3 == From.S3;
                }
                Check(bOk && Joints > 0 && Most > 0.0f,
                      Way ? "trees' wind: a branch's foot keeps to its parent (EZ-Tree)"
                          : "trees' wind: a branch's foot keeps to its parent (the own way)");
            }
        }
        Check(LampShadowsStrength(LampShadows_Off) == 0.0f &&
              LampShadowsStrength(LampShadows_Soft) < LampShadowsStrength(LampShadows_Medium) &&
              LampShadowsStrength(LampShadows_Medium) < LampShadowsStrength(LampShadows_Strong) &&
              LampShadowsStrength(LampShadows_Strong) == 1.0f, "lamp shadows: the strengths rise with the values");
        wchar_t Wide[16];
        LampShadowsName(LampShadows_Medium, Wide, 16);
        Check(Wide[0] == L'M' && Wide[6] == 0, "lamp shadows: the name, spelled out");
    }

    // Which surface the crosshair is on. A square, wound one way and then the other - both have to work, because the
    // engine hands polygons over in whichever order the level was built with.
    const float Sx[4] = { 10.0f, 30.0f, 30.0f, 10.0f };
    const float Sy[4] = { 10.0f, 10.0f, 30.0f, 30.0f };
    Check(PointInFan(Sx, Sy, 4, 20.0f, 20.0f), "crosshair: the middle of a square is in it");
    Check(!PointInFan(Sx, Sy, 4, 5.0f, 20.0f), "crosshair: a point to the left of it is not");
    Check(!PointInFan(Sx, Sy, 4, 20.0f, 40.0f), "crosshair: nor one below it");
    const float Rx[4] = { 10.0f, 10.0f, 30.0f, 30.0f };
    const float Ry[4] = { 10.0f, 30.0f, 30.0f, 10.0f };
    Check(PointInFan(Rx, Ry, 4, 20.0f, 20.0f), "crosshair: wound the other way, the middle is still in it");
    // A triangle, and a point just outside one of its edges.
    const float Tx[3] = { 0.0f, 100.0f, 0.0f };
    const float Ty[3] = { 0.0f, 0.0f, 100.0f };
    Check(PointInFan(Tx, Ty, 3, 10.0f, 10.0f), "crosshair: inside a triangle");
    Check(!PointInFan(Tx, Ty, 3, 60.0f, 60.0f), "crosshair: past its long edge, outside");
    Check(!PointInFan(Tx, Ty, 2, 1.0f, 1.0f), "crosshair: fewer than three points is nothing to be inside");
}

static void TestRtWaterColours()
{
    using namespace RdxMath;
    const unsigned int A = PackRtAlbedo(10, 200, 255, true);
    Check((A & 255u) == 10u && ((A >> 8) & 255u) == 200u && ((A >> 16) & 255u) == 255u, "rt water: the albedo's bytes");
    Check((A & RtAlbedoOwnLight) != 0u && (PackRtAlbedo(1, 2, 3, false) & RtAlbedoOwnLight) == 0u,
          "rt water: the own-light flag, and only when asked");
    const unsigned int L = PackRtLight(0.5f, 2.0f, 9.0f);
    Check(UnpackRtLight(L, 0) > 0.49f && UnpackRtLight(L, 0) < 0.51f && UnpackRtLight(L, 1) > 1.99f &&
          UnpackRtLight(L, 1) < 2.01f, "rt water: a light reads back within a step");
    Check(UnpackRtLight(L, 2) > RtLightRange - 0.01f && UnpackRtLight(L, 2) < RtLightRange + 0.01f,
          "rt water: a light above the range stops at it");
    Check(PackRtLight(99.0f, 99.0f, 99.0f) != RtLightUnknown, "rt water: the brightest light is not 'unknown'");
    // Two texels: bytes B, G, R, A as the engine stores a light map's - red 100 and 20 (doubled: 200, 40), green 127 (254),
    // blue 0 - averaged, at a gain of 2.
    const unsigned char T[8] = { 0, 127, 100, 0, 0, 127, 20, 0 };
    const unsigned int  Avg  = RtLightAverage(T, 2, 2.0f);
    Check(UnpackRtLight(Avg, 0) > 0.93f && UnpackRtLight(Avg, 0) < 0.95f, "rt water: a light map's red, averaged and doubled");
    Check(UnpackRtLight(Avg, 1) > 1.98f && UnpackRtLight(Avg, 1) < 2.0f, "rt water: its green");
    Check(UnpackRtLight(Avg, 2) == 0.0f, "rt water: its blue");
    Check(RtLightAverage(T, 0, 1.0f) == RtLightUnknown, "rt water: no texels, no light");
    Check(RtAlbedoByte(0) == 0 && RtAlbedoByte(255) == 255, "rt water: black and white stay");
}

static void TestLevelSplit()
{
    using namespace RdxMath;
    const float A[3] = { 0.0f, 0.0f, 0.0f }, B[3] = { 50.0f, 0.0f, 0.0f }, C[3] = { 0.0f, 50.0f, 0.0f };
    Check(SplitTriangleCount(A, B, C, 100.0f * 100.0f, LevelSplitDepth) == 1, "split: a small triangle stays whole");

    // A sliver: 1000 long, 4 wide.
    const float S0[3] = { 0.0f, 0.0f, 0.0f }, S1[3] = { 1000.0f, 0.0f, 0.0f }, S2[3] = { 500.0f, 4.0f, 0.0f };
    const int   N = SplitTriangleCount(S0, S1, S2, 100.0f * 100.0f, LevelSplitDepth);
    Check(N >= 10 && N <= 64, "split: a sliver ten times too long comes apart into a handful of pieces");
    static float Out[64 * 9];
    const float* End = SplitTriangle(S0, S1, S2, 100.0f * 100.0f, LevelSplitDepth, Out);
    Check((int)((End - Out) / 9) == N, "split: as many pieces written as counted");
    float Area = 0.0f, Longest = 0.0f;
    bool bFlipped = false;
    for (int t = 0; t < N; t++)
    {
        const float* P = Out + t * 9;
        Area += TriangleArea(P, P + 3, P + 6);
        const float E[3] = { SplitDist2(P, P + 3), SplitDist2(P + 3, P + 6), SplitDist2(P + 6, P) };
        for (int k = 0; k < 3; k++)
            Longest = (E[k] > Longest) ? E[k] : Longest;
        // The winding: the normal's z keeps its sign (the sliver lies in the z = 0 plane, counter-clockwise).
        const float Nz = (P[3] - P[0]) * (P[7] - P[1]) - (P[4] - P[1]) * (P[6] - P[0]);
        bFlipped = bFlipped || (Nz <= 0.0f);
    }
    Check(fabsf(Area - TriangleArea(S0, S1, S2)) < 0.01f * TriangleArea(S0, S1, S2), "split: the pieces cover the triangle");
    Check(Longest <= 100.0f * 100.0f + 1.0f, "split: no piece has an edge over the limit");
    Check(!bFlipped, "split: every piece keeps the triangle's winding");

    // The depth limit: a giant face stops at 2^LevelSplitDepth pieces.
    const float G0[3] = { 0.0f, 0.0f, 0.0f }, G1[3] = { 65536.0f, 0.0f, 0.0f }, G2[3] = { 0.0f, 65536.0f, 0.0f };
    Check(SplitTriangleCount(G0, G1, G2, 1.0f, LevelSplitDepth) == (1 << LevelSplitDepth),
          "split: a sky box's face stops at the depth limit");

    // The edge chosen for a level: the finest within the budget, 0 when nothing fits.
    const float Level[18] = { 0.0f, 0.0f, 0.0f, 1000.0f, 0.0f, 0.0f, 500.0f, 4.0f, 0.0f,
                              0.0f, 0.0f, 0.0f, 50.0f, 0.0f, 0.0f, 0.0f, 50.0f, 0.0f };
    Check(LevelSplitEdge(Level, 2, 1000) == 128.0f, "split: with room to spare, the finest edge");
    Check(LevelSplitEdge(Level, 2, 2) == 0.0f || LevelSplitEdge(Level, 2, 2) >= 1024.0f,
          "split: with no room, a coarse edge or none");
    Check(LevelSplitEdge(Level, 2, 1) == 0.0f, "split: below the level's own count, nothing fits");
}

// The height map files: a zip, deflate and a DDS, read as another's tools wrote them (ArchiveVectors.h).
static void TestArchive()
{
    using namespace RdxArchive;
    static unsigned char Out[2048];
    Check(Inflate(ArchiveStored, sizeof(ArchiveStored), Out, ArchiveStoredOf) &&
          memcmp(Out, ArchiveText, ArchiveStoredOf) == 0, "archive: a stored block unpacks to its text");
    Check(Inflate(ArchiveFixed, sizeof(ArchiveFixed), Out, ArchiveFixedOf) &&
          memcmp(Out, ArchiveText, ArchiveFixedOf) == 0, "archive: and a block with the fixed codes");
    Check(Inflate(ArchiveDynamic, sizeof(ArchiveDynamic), Out, sizeof(ArchiveText)) &&
          memcmp(Out, ArchiveText, sizeof(ArchiveText)) == 0, "archive: and one with its codes sent along");
    Check(!Inflate(ArchiveDynamic, sizeof(ArchiveDynamic), Out, sizeof(ArchiveText) - 1) &&
          !Inflate(ArchiveDynamic, sizeof(ArchiveDynamic), Out, sizeof(ArchiveText) + 1),
          "archive: a stream that does not come out at the size asked for is refused");
    Check(!Inflate(ArchiveDynamic, sizeof(ArchiveDynamic) / 2, Out, sizeof(ArchiveText)),
          "archive: and so is one cut short - never read past its end");

    unsigned Items = 0, DirOffset = 0, DirBytes = 0;
    Check(ZipEnd(ArchiveZip, sizeof(ArchiveZip), Items, DirOffset, DirBytes) && Items == 2 &&
          DirOffset + DirBytes <= sizeof(ArchiveZip), "archive: the zip's directory is found from its end");
    FZipItem List[4];
    const int Got = ZipItems(ArchiveZip + DirOffset, (int)DirBytes, List, 4);
    Check(Got == 2 && strcmp(List[0].Name, "header.txt") == 0 && List[0].Method == 0 &&
          strcmp(List[1].Name, "Stone.dds") == 0 && List[1].Method == 8 && List[1].Size == 1360u - 700u,
          "archive: its two items, one stored and one deflated, with their sizes");
    unsigned Start = 0;
    Check(Got == 2 && ZipDataStart(ArchiveZip + List[0].Local, List[0].Local, Start) &&
          memcmp(ArchiveZip + Start, "Pkg.Stone\tStone.dds\r\n", 21) == 0, "archive: the stored item read in place");
    Check(Got == 2 && ZipDataStart(ArchiveZip + List[1].Local, List[1].Local, Start) &&
          Inflate(ArchiveZip + Start, (int)List[1].Packed, Out, (int)List[1].Size) &&
          memcmp(Out, ArchiveText, List[1].Size) == 0, "archive: and the deflated one unpacked");
    Check(ZipItems(ArchiveText, 100, List, 4) == -1, "archive: what is not a directory is said to be none");

    // A DDS header: the DX10 form naming BC4, 16 x 8 with three sizes, and the old four-character code.
    unsigned char Dds[160];
    memset(Dds, 0, sizeof(Dds));
    auto Put = [&](int At, unsigned V) { Dds[At] = (unsigned char)V; Dds[At + 1] = (unsigned char)(V >> 8);
                                         Dds[At + 2] = (unsigned char)(V >> 16); Dds[At + 3] = (unsigned char)(V >> 24); };
    memcpy(Dds, "DDS ", 4);
    Put(4, 124);
    Put(8, 0x1u | 0x2u | 0x4u | 0x1000u | 0x20000u);
    Put(12, 8);
    Put(16, 16);
    Put(28, 3);
    Put(76, 32);
    Put(80, 0x4);
    memcpy(Dds + 84, "DX10", 4);
    Put(128, 80);
    int W = 0, H = 0, Mips = 0, At = 0;
    Check(DdsBc4(Dds, sizeof(Dds), W, H, Mips, At) && W == 16 && H == 8 && Mips == 3 && At == 148,
          "archive: a DDS naming BC4 in its DX10 header");
    memcpy(Dds + 84, "ATI1", 4);
    Check(DdsBc4(Dds, sizeof(Dds), W, H, Mips, At) && At == 128, "archive: and one with the old code for it");
    memcpy(Dds + 84, "DX10", 4);
    Put(128, 81);
    Check(!DdsBc4(Dds, sizeof(Dds), W, H, Mips, At), "archive: a signed BC4, or any other format, is not taken");
    Check(Bc4Bytes(16, 8, 3) == 64 + 16 + 8 && Bc4Bytes(1024, 512, 1) == 256 * 128 * 8,
          "archive: a BC4 map's bytes, eight a block, its halvings counted");
}

// ---------------------------------------------------------------------------

// The level rules run on sheets read from a file, for scripts/level_rules_check.py: the game's own levels' flame
// and fall sheets go in, the fires and the columns the renderer's rules make of them come out, and the script holds
// them against its own reading of the rules. A line in: F or W (a flame's sheet, a fall's), the level's number, the
// surface, the middle (x, y), the direction across (x, y), the half-width, the foot, the top - a level's lines
// together, in the order of its surfaces. A line out: the same letter and level, the first surface of the fire or
// the column, how many sheets, and the body: 0 a standing fire, 2 one tongue; 1 a round column, 0 a ribbon.
static int LevelRules(const char* In, const char* Out)
{
    FILE* Read  = nullptr;
    FILE* Write = nullptr;
    if (fopen_s(&Read, In, "r") != 0 || !Read || fopen_s(&Write, Out, "w") != 0 || !Write)
    {
        printf("the file could not be opened\n");
        return 2;
    }
    enum { MOST = 4096 };
    static FFlamePiece     Flames[MOST];
    static FFallSheetPiece Falls[MOST];
    static int             Surf[MOST], Group[MOST];
    char Kind = 0, Next = 0;
    int  Level = -1, NextLevel = -1, Count = 0, Lines = 0, Made = 0;
    for (;;)
    {
        int   S = 0;
        float V[7];
        const int Got = fscanf_s(Read, " %c %d %d %f %f %f %f %f %f %f", &Next, 1, &NextLevel, &S, &V[0], &V[1],
                                 &V[2], &V[3], &V[4], &V[5], &V[6]);
        const bool bMore = Got == 10;
        if (Count > 0 && (!bMore || Next != Kind || NextLevel != Level || Count >= MOST))
        {
            // A level's sheets of one kind are all in: what the rules make of them.
            const int Groups = (Kind == 'F') ? FlameFiresOf(Flames, Count, Group) : FallColumnsOf(Falls, Count, Group);
            for (int g = 0; g < Groups; g++)
            {
                static FFlamePiece     OneFire[MOST];
                static FFallSheetPiece OneFall[MOST];
                int First = -1, Members = 0;
                for (int i = 0; i < Count; i++)
                    if (Group[i] == g)
                    {
                        First = (First < 0) ? Surf[i] : First;
                        OneFire[Members] = Flames[i];
                        OneFall[Members] = Falls[i];
                        Members++;
                    }
                if (Kind == 'F')
                {
                    static int Scratch[MOST];
                    float      Body[FlameBodyFloats];
                    const int  Bodies = FlameBodiesMake(OneFire, Members, Body, 1, 0.0f, Scratch);
                    fprintf(Write, "F %d %d %d %d %d %.3f %.3f %.3f %.3f %.5f %.5f %.4f\n", Level, First, Members,
                            Bodies, (int)Body[12], Body[0], Body[1], Body[6], Body[7], Body[4], Body[5], Body[15]);
                }
                else
                {
                    float Column[FallColumnFloats];
                    const int Columns = FallColumnsMake(OneFall, Members, Column, 1);
                    fprintf(Write, "W %d %d %d %d %d %.3f %.3f %.3f %.3f %.3f %.3f\n", Level, First, Members, Columns,
                            (Column[6] == Column[7]) ? 1 : 0, Column[0], Column[1], Column[6], Column[7], Column[2],
                            Column[3]);
                }
                Made++;
            }
            Count = 0;
        }
        if (!bMore)
            break;
        Kind  = Next;
        Level = NextLevel;
        Surf[Count]   = S;
        Flames[Count] = FlameSheetPiece(V[0], V[1], V[2], V[3], V[4], V[5], V[6], 0.0f);
        FFallSheetPiece& W = Falls[Count];
        W.Mid[0]    = V[0];
        W.Mid[1]    = V[1];
        W.Across[0] = V[2];
        W.Across[1] = V[3];
        W.Half      = V[4];
        W.Bottom    = V[5];
        W.Top       = V[6];
        W.Hue[0]    = W.Hue[1] = W.Hue[2] = 0.0f;
        W.bFoot     = 0;
        Count++;
        Lines++;
    }
    fclose(Read);
    fclose(Write);
    printf("%d sheets read, %d fires and columns written\n", Lines, Made);
    return 0;
}

// Where a moving body's vertices were the frame before (MotionPrevPlaces): two vertices of a body that stepped from
// x = 100 to x = 110 while one of them swung up by 4.
static void TestMotionPrevPlaces()
{
    printf("motion: where each vertex of a moving body was the frame before\n");
    // The shapes, each less the body's place when it was made: the first vertex stands still in the body, the second
    // went from z = 10 to z = 14.
    const float Before[6] = { 1.0f, 2.0f, 3.0f,   5.0f, 0.0f, 10.0f };
    const float Now[6]    = { 1.0f, 2.0f, 3.0f,   5.0f, 0.0f, 14.0f };
    const float WasAt[3]  = { 100.0f, 0.0f, 0.0f };
    float Out[6];
    MotionPrevPlaces(Now, Before, 6, 1, WasAt, Out);
    Check(Out[0] == 101.0f && Out[1] == 2.0f && Out[2] == 3.0f,
          "a frame apart: the earlier shape at the body's old place");
    Check(Out[3] == 105.0f && Out[4] == 0.0f && Out[5] == 10.0f, "... the swung vertex where it was before it swung");
    // The same change over two frames: half of it belongs to the last frame.
    MotionPrevPlaces(Now, Before, 6, 2, WasAt, Out);
    CheckNear(Out[5], 12.0f, 1e-5f, "two frames apart: one frame's share of the swing");
    CheckNear(Out[3], 105.0f, 1e-5f, "... and the body's own step whole");
    MotionPrevPlaces(Now, Before, 6, 4, WasAt, Out);
    CheckNear(Out[5], 13.0f, 1e-5f, "four frames apart: a quarter of it");
    // Frames below one counts as one, and a count that is no whole vertex writes no part of one.
    float Short[6] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
    MotionPrevPlaces(Now, Before, 5, 0, WasAt, Short);
    Check(Short[0] == 101.0f && Short[3] == -1.0f && Short[5] == -1.0f, "no frames: as one; five floats: one vertex");
    // A body that did not change at all was where it is.
    const float Here[3] = { 0.0f, 0.0f, 0.0f };
    MotionPrevPlaces(Now, Now, 6, 3, Here, Out);
    Check(Out[0] == Now[0] && Out[5] == Now[5], "no change: the same places");
}

// The GPU key: which of a machine's cards the renderer runs on. A laptop's two cards, as the system lists them with
// the high performance one first, and a third that cannot run the renderer.
static void TestGpuKey()
{
    printf("GPU key: the card a setting asks for, by its number or by a piece of its name\n");
    const wchar_t* const Cards[3] = { L"NVIDIA GeForce RTX 5060 Laptop GPU", L"Intel(R) Arc(TM) Graphics",
                                      L"Old Card 2" };
    const bool Usable[3] = { true, true, false };
    bool bAsked = true;
    Check(GpuAskedFor("Auto", Cards, Usable, 3, bAsked) == -1 && !bAsked, "Auto asks for none");
    Check(GpuAskedFor(" auto\t", Cards, Usable, 3, bAsked) == -1 && !bAsked, "... in any case, spaces around it");
    Check(GpuAskedFor("", Cards, Usable, 3, bAsked) == -1 && !bAsked, "nothing asks for none");
    Check(GpuAskedFor((const char*)nullptr, Cards, Usable, 3, bAsked) == -1 && !bAsked, "no text asks for none");
    Check(GpuAskedFor("1", Cards, Usable, 3, bAsked) == 0 && bAsked, "a number: the card's place in the list, from 1");
    Check(GpuAskedFor(" 2 ", Cards, Usable, 3, bAsked) == 1 && bAsked, "... the second");
    Check(GpuAskedFor("3", Cards, Usable, 3, bAsked) == -1 && bAsked, "... a card that cannot run it is not taken");
    Check(GpuAskedFor("intel", Cards, Usable, 3, bAsked) == 1 && bAsked, "a piece of a name, in either case");
    Check(GpuAskedFor("RTX 5060", Cards, Usable, 3, bAsked) == 0 && bAsked, "... with a space in it");
    Check(GpuAskedFor("5060", Cards, Usable, 3, bAsked) == 0 && bAsked,
          "a number that is no card's place is a name's piece");
    Check(GpuAskedFor("4", Cards, Usable, 3, bAsked) == -1 && bAsked, "... and names none where no name holds it");
    Check(GpuAskedFor("g", Cards, Usable, 3, bAsked) == 0 && bAsked, "two names hold it: the first in the list");
    Check(GpuAskedFor("Old", Cards, Usable, 3, bAsked) == -1 && bAsked, "a name's piece of a card that cannot run it");
    Check(GpuAskedFor("Radeon", Cards, Usable, 3, bAsked) == -1 && bAsked, "a card that is not there: none, and asked");
    Check(GpuAskedFor(L"Arc(TM)", Cards, Usable, 3, bAsked) == 1 && bAsked,
          "the key's text wide, as the engine has it");
    Check(GpuAskedFor("1", Cards, Usable, 0, bAsked) == -1 && bAsked, "no cards at all: none");
    const char* const Narrow[1] = { "AMD Radeon RX 9070 XT" };
    const bool One[1] = { true };
    Check(GpuAskedFor("radeon rx", Narrow, One, 1, bAsked) == 0 && bAsked, "narrow names too");
    Check(GpuAskedFor("2", Narrow, One, 1, bAsked) == -1 && bAsked, "one card, the second asked for: none");
}

// The host's camera as matrices (Host64Camera.h): what DLSS's frame generation is told about how the camera moved. Held
// to their own algebra - a point carried by hand through each camera has to land where the matrices put it.
static void TestHostCamera()
{
    printf("host camera: a frame's camera as matrices, and one frame's clip space into another's\n");
    auto Carry = [](const float V[4], const Host64::FMat& M, float Out[4])
    {
        for (int j = 0; j < 4; j++)
            Out[j] = V[0] * M.M[0][j] + V[1] * M.M[1][j] + V[2] * M.M[2][j] + V[3] * M.M[3][j];
    };
    auto NearIdentity = [](const Host64::FMat& M)
    {
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                if (fabsf(M.M[i][j] - ((i == j) ? 1.0f : 0.0f)) > 1.0e-4f)
                    return false;
        return true;
    };

    // A camera looking along +y with z up, as a level has it, and the frame before it: stepped aside, turned a little.
    Host64::FFrame Now = {};
    Now.RenderWidth  = 1920;
    Now.RenderHeight = 1080;
    Now.FovY         = 1.0f;
    Now.NearZ        = 1.0f;
    const float Place[3] = { 100.0f, 200.0f, 50.0f }, Right[3] = { 1.0f, 0.0f, 0.0f }, Up[3] = { 0.0f, 0.0f, 1.0f },
                Ahead[3] = { 0.0f, 1.0f, 0.0f };
    Host64::FFrame Before = Now;
    const float Angle = 0.1f, BeforePlace[3] = { 90.0f, 190.0f, 55.0f };
    const float BeforeRight[3] = { cosf(Angle), -sinf(Angle), 0.0f };
    const float BeforeAhead[3] = { sinf(Angle), cosf(Angle), 0.0f };
    for (int k = 0; k < 3; k++)
    {
        Now.CameraPosition[k]    = Place[k];
        Now.CameraRight[k]       = Right[k];
        Now.CameraUp[k]          = Up[k];
        Now.CameraForward[k]     = Ahead[k];
        Before.CameraPosition[k] = BeforePlace[k];
        Before.CameraRight[k]    = BeforeRight[k];
        Before.CameraUp[k]       = Up[k];
        Before.CameraForward[k]  = BeforeAhead[k];
    }

    Check(NearIdentity(Host64::MatMul(Host64::ViewToClip(Now), Host64::ClipToView(Now))) &&
          NearIdentity(Host64::MatMul(Host64::WorldToView(Before), Host64::ViewToWorld(Before))),
          "host camera: each matrix and its way back make no change together");
    Check(NearIdentity(Host64::ClipToClip(Now, Now)), "host camera: a frame's clip space into its own is no change");

    // A point 30 to the camera's right, 40 above it and 500 ahead: by hand, then by the matrices.
    const float Point[4] = { 130.0f, 700.0f, 90.0f, 1.0f };
    const float Ys = 1.0f / tanf(0.5f), Xs = Ys / (1920.0f / 1080.0f);
    float View[4], Clip[4];
    Carry(Point, Host64::WorldToView(Now), View);
    Check(fabsf(View[0] - 30.0f) < 1.0e-3f && fabsf(View[1] - 40.0f) < 1.0e-3f && fabsf(View[2] - 500.0f) < 1.0e-3f,
          "host camera: the camera's space is x to its right, y up, z ahead");
    Carry(View, Host64::ViewToClip(Now), Clip);
    CheckNear(Clip[0] / Clip[3], 30.0f * Xs / 500.0f, 1.0e-5f,
              "host camera: right of the camera is right in clip space");
    CheckNear(Clip[1] / Clip[3], 40.0f * Ys / 500.0f, 1.0e-5f, "host camera: above the camera is up in clip space");
    CheckNear(Clip[2] / Clip[3], 1.0f / 500.0f, 1.0e-7f,
              "host camera: the depth is Near / z - 1 nearest, 0 at infinity");

    // The same point in the frame before, by that camera's own matrices; and the current clip point carried over.
    float ViewBefore[4], ClipBefore[4], Carried[4], Back[4];
    Carry(Point, Host64::WorldToView(Before), ViewBefore);
    Carry(ViewBefore, Host64::ViewToClip(Before), ClipBefore);
    Carry(Clip, Host64::ClipToClip(Now, Before), Carried);
    Carry(ClipBefore, Host64::ClipToClip(Before, Now), Back);
    Check(fabsf(Carried[0] / Carried[3] - ClipBefore[0] / ClipBefore[3]) < 1.0e-4f &&
          fabsf(Carried[1] / Carried[3] - ClipBefore[1] / ClipBefore[3]) < 1.0e-4f &&
          fabsf(Carried[2] / Carried[3] - ClipBefore[2] / ClipBefore[3]) < 1.0e-6f,
          "host camera: this frame's clip point lands where the frame before saw it");
    Check(fabsf(Back[0] / Back[3] - Clip[0] / Clip[3]) < 1.0e-4f &&
          fabsf(Back[1] / Back[3] - Clip[1] / Clip[3]) < 1.0e-4f &&
          fabsf(Back[2] / Back[3] - Clip[2] / Clip[3]) < 1.0e-6f,
          "host camera: and the frame before's lands where this one sees it");
    // The frame before looked further to the right than this one: what stands still was further left in its picture.
    Check(ClipBefore[0] / ClipBefore[3] < Clip[0] / Clip[3] - 0.05f,
          "host camera: a camera that looked further right saw a standing point further left");
}

// The hologram fix's three ways (HdtpHoloFix), and what the game's script puts on a hologram's skin.
static void TestHoloFix()
{
    using namespace RdxMath;
    bool bK = false;
    Check(ParseHoloFix("Off", bK) == HoloFix_Off && bK && ParseHoloFix(" on", bK) == HoloFix_On && bK &&
          ParseHoloFix("OLD", bK) == HoloFix_Old && bK, "hologram fix: its three ways, whatever the case");
    Check(ParseHoloFix("True", bK) == HoloFix_On && !bK, "hologram fix: True is no word of the key - unknown, On");
    Check(ParseHoloFix("Older", bK) == HoloFix_On && !bK && ParseHoloFix("", bK) == HoloFix_On && !bK,
          "hologram fix: an unknown word is the default, and said to be unknown");
    Check(HoloFixStep(HoloFix_Off, 1) == HoloFix_On && HoloFixStep(HoloFix_On, 1) == HoloFix_Old &&
          HoloFixStep(HoloFix_Old, 1) == HoloFix_Off && HoloFixStep(HoloFix_Off, -1) == HoloFix_Old,
          "hologram fix: the menu steps round all three, both ways");
    char Name[16];
    HoloFixName(HoloFix_Old, Name, 16);
    Check(Name[0] == 'O' && Name[1] == 'l' && Name[2] == 'd' && Name[3] == 0, "hologram fix: the original model is Old");

    // A translucent character (the engine's style 3): the masks and an empty skin become the black mask.
    Check(HologramSkin(3, HoloSkin_None) == HoloSkin_Black && HologramSkin(3, HoloSkin_Pink) == HoloSkin_Black &&
          HologramSkin(3, HoloSkin_Gray) == HoloSkin_Black && HologramSkin(3, HoloSkin_Black) == HoloSkin_Black,
          "hologram skin: a translucent one's masks and empty skins are the black mask");
    Check(HologramSkin(3, HoloSkin_Other) == HoloSkin_Other && HologramSkin(4, HoloSkin_Other) == HoloSkin_Other,
          "hologram skin: a real texture stays what it is");
    Check(HologramSkin(4, HoloSkin_None) == HoloSkin_Gray && HologramSkin(4, HoloSkin_Pink) == HoloSkin_Gray &&
          HologramSkin(4, HoloSkin_Black) == HoloSkin_Black,
          "hologram skin: a modulated one's are the grey mask, and the black mask stays black");
    Check(HologramSkin(2, HoloSkin_Gray) == HoloSkin_Pink && HologramSkin(1, HoloSkin_None) == HoloSkin_Black,
          "hologram skin: a masked one's are the pink mask, any other style's the black");

    // A hologram's reach: by the larger measure of its cylinder, grown with a character drawn larger than its class,
    // never shrunk with one drawn smaller.
    const auto Near = [](float A, float B) { return A - B < 1e-3f && B - A < 1e-3f; };
    Check(Near(HologramReach(20.0f, 47.5f, 1.0f), 134.75f) && Near(HologramReach(62.0f, 58.28f, 1.0f), 171.0f),
          "hologram reach: two and a half times the larger of the cylinder's measures, and a little more");
    Check(Near(HologramReach(20.0f, 47.5f, 2.0f), 253.5f) && Near(HologramReach(62.0f, 58.28f, 0.9f), 171.0f),
          "hologram reach: grows with a character drawn larger than its class, stays for one drawn smaller");
    Check(HologramReach(0.0f, 0.0f, 1.0f) > 0.0f, "hologram reach: a character with no cylinder still has some");
}

// The water's polygons cut up before the GPU lifts them (CutConvex, WaterCutPolygon).
static void TestWaterCut()
{
    using namespace RdxMath;
    enum { S = 5 };     // a corner here: its place, one number carried along, its mark
    const auto Near = [](float A, float B) { return A - B < 1e-3f && B - A < 1e-3f; };
    // Twice the area of a polygon lying level, from above.
    const auto Area2 = [](const float* P, int Count)
    {
        float Sum = 0.0f;
        for (int i = 0; i < Count; i++)
        {
            const float* const A = P + i * S;
            const float* const B = P + ((i + 1) % Count) * S;
            Sum += A[0] * B[2] - B[0] * A[2];
        }
        return (Sum < 0.0f) ? -Sum : Sum;
    };
    // A square of water under the eye's height, 100 to 300 units ahead and 100 to either side; the number carried
    // along is its depth doubled.
    const float Square[4 * S] = { -100.0f, 50.0f, 100.0f, 200.0f, 1.0f,   100.0f, 50.0f, 100.0f, 200.0f, 2.0f,
                                  100.0f,  50.0f, 300.0f, 600.0f, 3.0f,  -100.0f, 50.0f, 300.0f, 600.0f, 4.0f };
    float Front[8 * S], Back[8 * S];
    int   FrontCount = 0, BackCount = 0;
    CutConvex<S>(Square, 4, 0.0f, 0.0f, 1.0f, -200.0f, 0.001f, Front, FrontCount, Back, BackCount);
    Check(FrontCount == 4 && BackCount == 4, "water cut: a square cut across is two four-cornered parts");
    Check(Near(Area2(Front, FrontCount) + Area2(Back, BackCount), Area2(Square, 4)) &&
          Near(Area2(Front, FrontCount), Area2(Back, BackCount)),
          "water cut: the two parts are the square, half of it each");
    bool bNew = true, bOld = true;
    int  NewCorners = 0;
    for (int i = 0; i < FrontCount; i++)
    {
        const float* const C = Front + i * S;
        if (C[4] == 0.0f)
        {
            NewCorners++;
            bNew = bNew && Near(C[2], 200.0f) && Near(C[3], 400.0f) && Near(C[1], 50.0f);
        }
        else
            bOld = bOld && C[2] >= 200.0f;
    }
    Check(NewCorners == 2 && bNew,
          "water cut: a new corner is on the plane, with what it carries from between the ends");
    Check(bOld, "water cut: the corners kept on a side are that side's, their marks with them");
    // The same square the other way round: the new corners are the very same numbers.
    const float Turned[4 * S] = { -100.0f, 50.0f, 300.0f, 600.0f, 4.0f,  100.0f, 50.0f, 300.0f, 600.0f, 3.0f,
                                  100.0f,  50.0f, 100.0f, 200.0f, 2.0f, -100.0f, 50.0f, 100.0f, 200.0f, 1.0f };
    float Front2[8 * S], Back2[8 * S];
    int   Front2Count = 0, Back2Count = 0;
    CutConvex<S>(Turned, 4, 0.3f, 0.0f, 1.0f, -190.0f, 0.001f, Front2, Front2Count, Back2, Back2Count);
    CutConvex<S>(Square, 4, 0.3f, 0.0f, 1.0f, -190.0f, 0.001f, Front, FrontCount, Back, BackCount);
    int Same = 0;
    for (int i = 0; i < FrontCount; i++)
        for (int j = 0; j < Front2Count; j++)
            if (Front[i * S + 4] == 0.0f && Front2[j * S + 4] == 0.0f && Front[i * S] == Front2[j * S] &&
                Front[i * S + 1] == Front2[j * S + 1] && Front[i * S + 2] == Front2[j * S + 2] &&
                Front[i * S + 3] == Front2[j * S + 3])
                Same++;
    Check(Same == 2, "water cut: a polygon going round the other way gets the very same new corners");
    // A plane through two corners: nothing new, and the corners on it are both parts'.
    CutConvex<S>(Square, 4, 1.0f, 0.0f, 1.0f, -200.0f, 0.001f, Front, FrontCount, Back, BackCount);
    Check(FrontCount == 3 && BackCount == 3,
          "water cut: a plane through two corners makes two triangles, no new corner");
    // A plane that misses: all of it on one side.
    CutConvex<S>(Square, 4, 0.0f, 0.0f, 1.0f, -500.0f, 0.001f, Front, FrontCount, Back, BackCount);
    Check(FrontCount == 0 && BackCount == 4, "water cut: a plane beyond the polygon leaves it whole, behind");

    // The polygon cut by the view's planes: a view whose picture is 3840 by 2160, its axis through the middle. A
    // small polygon, within one band and one cell of the screen: itself.
    static float Out[4096 * S];
    static int   Sizes[1024];
    const FWaterCutView View = { 1920.0f, 1080.0f, 1920.0f, 0.0f, 0.0f, 3840.0f, 2160.0f };
    const float Small[3 * S] = { 10.0f, 20.0f, 100.0f, 0.0f, 1.0f,  14.0f, 20.0f, 100.0f, 0.0f, 2.0f,
                                 12.0f, 20.0f, 105.0f, 0.0f, 3.0f };
    int Pieces = WaterCutPolygon<S>(Small, 3, View, 1600.0f, Out, 4096, Sizes, 1024);
    Check(Pieces == 1 && Sizes[0] == 3 && Out[4] == 1.0f && Out[S + 4] == 2.0f && Out[2 * S + 4] == 3.0f,
          "water cut: a polygon no plane crosses is one piece, its own corners with their marks");
    // A long one, seen flat from 60 units over it: from 110 to 1500 units ahead, as wide as the view all the way.
    const float Long[4 * S] = { -99.0f,  60.0f, 110.0f, 0.0f, 1.0f,     99.0f,   60.0f, 110.0f, 0.0f, 2.0f,
                                1350.0f, 60.0f, 1500.0f, 0.0f, 3.0f,  -1350.0f, 60.0f, 1500.0f, 0.0f, 4.0f };
    Pieces = WaterCutPolygon<S>(Long, 4, View, 1600.0f, Out, 4096, Sizes, 1024);
    float Sum    = 0.0f;
    bool  bBands = true, bCells = true;
    int   At     = 0;
    for (int p = 0; p < Pieces; p++)
    {
        const float* const Piece = Out + At * S;
        Sum += Area2(Piece, Sizes[p]);
        float Zmin = Piece[2], Zmax = Piece[2], Xmin = 0.0f, Xmax = 0.0f, Ymin = 0.0f, Ymax = 0.0f;
        for (int i = 0; i < Sizes[p]; i++)
        {
            const float* const C = Piece + i * S;
            const float X = 1920.0f + C[0] * 1920.0f / C[2];
            const float Y = 1080.0f + C[1] * 1920.0f / C[2];
            Zmin = (C[2] < Zmin) ? C[2] : Zmin;
            Zmax = (C[2] > Zmax) ? C[2] : Zmax;
            Xmin = (i == 0 || X < Xmin) ? X : Xmin;
            Xmax = (i == 0 || X > Xmax) ? X : Xmax;
            Ymin = (i == 0 || Y < Ymin) ? Y : Ymin;
            Ymax = (i == 0 || Y > Ymax) ? Y : Ymax;
        }
        bBands = bBands && Zmax <= Zmin * WaterBandStep * 1.001f;
        bCells = bCells && Xmax - Xmin <= WaterCutSpan + 0.5f && Ymax - Ymin <= WaterCutSpan + 0.5f;
        At += Sizes[p];
    }
    Check(Pieces > 10 && Near(Sum / Area2(Long, 4), 1.0f),
          "water cut: a long polygon's pieces are the polygon, no more, no less");
    Check(bBands, "water cut: no piece is deeper than one band - a step times as far at its far end as at its near");
    Check(bCells, "water cut: no piece is wider or taller on the screen than a cell of its grid");
    // Beyond the reach the depth planes stop: the far part is one piece however far it goes.
    const float Far[4 * S] = { -50.0f, 60.0f, 3000.0f, 0.0f, 1.0f,    50.0f, 60.0f, 3000.0f, 0.0f, 2.0f,
                               50.0f,  60.0f, 30000.0f, 0.0f, 3.0f,  -50.0f, 60.0f, 30000.0f, 0.0f, 4.0f };
    Check(WaterCutPolygon<S>(Far, 4, View, 1600.0f, Out, 4096, Sizes, 1024) == 1,
          "water cut: what lies beyond the lifting's reach is not cut by depth");
    Check(WaterCutPolygon<S>(Far, 4, View, 16000.0f, Out, 4096, Sizes, 1024) > 3,
          "water cut: within a longer reach it is cut");
    // Too little room is said, not written past.
    Check(WaterCutPolygon<S>(Long, 4, View, 1600.0f, Out, 8, Sizes, 1024) == -1 &&
          WaterCutPolygon<S>(Long, 4, View, 1600.0f, Out, 4096, Sizes, 2) == -1,
          "water cut: too little room for the pieces is told");

    // A sea all round the eye, a hundred thousand units to every side - far more than the picture, and not clipped
    // to it: it is cut along the picture's own lines only, so its pieces are no more than the view has cells for,
    // and the room said to be always enough is enough. A smaller picture here, for a smaller room.
    const FWaterCutView Little = { 640.0f, 360.0f, 640.0f, 0.0f, 0.0f, 1280.0f, 720.0f };
    const int           Most   = WaterCutMostPieces(Little, 1600.0f);
    enum { SeaRoom = 4096 * 16 };
    static float SeaOut[SeaRoom * S];
    static int   SeaSizes[4096];
    const float Sea[4 * S] = { -100000.0f, 40.0f, 4.0f, 0.0f, 1.0f,       100000.0f,  40.0f, 4.0f, 0.0f, 2.0f,
                               100000.0f,  40.0f, 100000.0f, 0.0f, 3.0f,  -100000.0f, 40.0f, 100000.0f, 0.0f, 4.0f };
    Check(Most > 26 && Most <= 4096, "water cut: the most pieces a view has cells for is a number a room can be for");
    Pieces = WaterCutPolygon<S>(Sea, 4, Little, 1600.0f, SeaOut, SeaRoom, SeaSizes, 4096);
    Check(Pieces > 100 && Pieces <= Most,
          "water cut: a polygon far larger than the picture is cut into no more pieces than the view has cells for");
    bool bInView = true, bCorners = true;
    At = 0;
    for (int p = 0; p < Pieces; p++)
    {
        const float* const Piece = SeaOut + At * S;
        float Xmin = 0.0f, Xmax = 0.0f, Ymin = 0.0f, Ymax = 0.0f;
        for (int i = 0; i < SeaSizes[p]; i++)
        {
            const float* const C = Piece + i * S;
            const float X = Little.CentreX + C[0] * Little.Focal / C[2];
            const float Y = Little.CentreY + C[1] * Little.Focal / C[2];
            Xmin = (i == 0 || X < Xmin) ? X : Xmin;
            Xmax = (i == 0 || X > Xmax) ? X : Xmax;
            Ymin = (i == 0 || Y < Ymin) ? Y : Ymin;
            Ymax = (i == 0 || Y > Ymax) ? Y : Ymax;
        }
        // A piece any of which is in the picture: no larger than a cell, give or take what such large numbers lose.
        if (Xmax > Little.Left && Xmin < Little.Right && Ymax > Little.Top && Ymin < Little.Bottom)
            bInView = bInView && Xmax - Xmin <= WaterCutSpan + 8.0f && Ymax - Ymin <= WaterCutSpan + 8.0f;
        bCorners = bCorners && SeaSizes[p] >= 3 && SeaSizes[p] <= 16;
        At += SeaSizes[p];
    }
    Check(bInView, "water cut: what of it is in the picture is cut as finely as any polygon - a cell at the most");
    Check(bCorners, "water cut: its pieces are polygons of a few corners each");
    // A view with no picture to it cuts by depth alone, and counts no cells.
    const FWaterCutView None = { 640.0f, 360.0f, 640.0f, 0.0f, 0.0f, -2000.0f, -2000.0f };
    Pieces = WaterCutPolygon<S>(Sea, 4, None, 1600.0f, SeaOut, SeaRoom, SeaSizes, 4096);
    Check(Pieces > 1 && Pieces <= WaterCutMostPieces(None, 1600.0f),
          "water cut: a view with no picture leaves a polygon one piece to a band");
}

// The force field's three ways (ForceField), and what a vertex of its surface hands the shader for the field in depth.
static void TestForceField()
{
    using namespace RdxMath;
    const auto Near = [](float A, float B, float Tolerance) { return A - B < Tolerance && B - A < Tolerance; };
    bool bK = false;
    Check(ParseForceField("Original", bK) == ForceField_Engine && bK && ParseForceField(" 3d", bK) == ForceField_Volume &&
          bK, "force field: its two ways, whatever the case");
    Check(ParseForceField("Arcsy", bK) == ForceField_Volume && !bK && ParseForceField("", bK) == ForceField_Volume &&
          !bK, "force field: an unknown word is the default, and said to be unknown");
    Check(ForceFieldStep(ForceField_Volume, 1) == ForceField_Engine &&
          ForceFieldStep(ForceField_Engine, -1) == ForceField_Volume &&
          ForceFieldStep(ForceField_Engine, 1) == ForceField_Volume, "force field: the menu steps round both ways");
    char Name[16];
    ForceFieldName(ForceField_Volume, Name, 16);
    Check(Name[0] == '3' && Name[1] == 'D' && Name[2] == 0, "force field: the field in depth is written 3D");

    // The volume's polygons as its pass takes them. A square in the plane Z = 100 of the world, the eye at
    // (0, 0, 40): rows from the eye, a ball about it with the half thickness, corners that go round so that an edge
    // crossed with the normal points out of the polygon - whichever way they were handed over.
    const FVec3 Sq[4]  = { { -10.0f, -10.0f, 100.0f }, { 10.0f, -10.0f, 100.0f }, { 10.0f, 10.0f, 100.0f },
                           { -10.0f, 10.0f, 100.0f } };
    const FVec3 Rev[4] = { Sq[3], Sq[2], Sq[1], Sq[0] };
    const FVec3 Eye    = { 0.0f, 0.0f, 40.0f };
    const float Hue[4] = { 0.25f, 0.5f, 1.0f, 0.3f };
    for (int Way = 0; Way < 2; Way++)
    {
        float Rows[FieldRows * 4];
        const bool bMade = FieldPolygonRows(Way ? Rev : Sq, 4, Eye, FieldHalf, Hue, Rows);
        Check(bMade && Rows[3] == 4.0f && Near(Rows[0], 0.0f, 1e-6f) && Near(Rows[1], 0.0f, 1e-6f) &&
              Near(Rows[2] * Rows[2], 1.0f, 1e-5f), "field volume: the plane's normal and the number of corners");
        Check(Near(Rows[4], 0.0f, 1e-5f) && Near(Rows[5], 0.0f, 1e-5f) && Near(Rows[6], 60.0f, 1e-4f) &&
              Near(Rows[7], 14.142136f + FieldHalf, 1e-3f),
              "field volume: the ball - its middle from the eye, its reach");
        Check(bMade && Rows[8] == 0.25f && Rows[9] == 0.5f && Rows[10] == 1.0f && Rows[11] == 0.3f,
              "field volume: the polygon's hue and how bright it is laid on, in a row of their own");
        bool bOut = bMade, bFromEye = bMade;
        for (int i = 0; i < 4 && bMade; i++)
        {
            const float* const A = Rows + (3 + i) * 4;
            const float* const B = Rows + (3 + (i + 1) % 4) * 4;
            const FVec3 E = { B[0] - A[0], B[1] - A[1], B[2] - A[2] };
            const FVec3 N = { Rows[0], Rows[1], Rows[2] };
            const FVec3 O = { E.Y * N.Z - E.Z * N.Y, E.Z * N.X - E.X * N.Z, E.X * N.Y - E.Y * N.X };
            // From the polygon's middle to the edge's: the same way as the edge crossed with the normal.
            const FVec3 M = { A[0] + 0.5f * E.X - Rows[4], A[1] + 0.5f * E.Y - Rows[5],
                              A[2] + 0.5f * E.Z - Rows[6] };
            bOut     = bOut && Dot(O, M) > 0.0f;
            bFromEye = bFromEye && Near(A[2], 60.0f, 1e-4f);
        }
        Check(bOut, "field volume: an edge crossed with the normal points out of the polygon, either way round");
        Check(bFromEye, "field volume: the corners are from the eye");
    }
    float None[FieldRows * 4];
    const FVec3 Line[3] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f } };
    Check(!FieldPolygonRows(Sq, 2, Eye, FieldHalf, Hue, None) && !FieldPolygonRows(Line, 3, Eye, FieldHalf, Hue, None) &&
          !FieldPolygonRows(Sq, FieldCorners + 1, Eye, FieldHalf, Hue, None),
          "field volume: two corners, corners in a line and too many corners are no polygon");

    // A field's hue held from frame to frame: a texture that is a few sparks on black shows now a spark, now nothing.
    float Held[3] = { 0.75f, 0.55f, 1.0f };
    bool  bHeld   = false;
    const float Dark[3]  = { 0.0f, 0.0f, 0.1f };
    const float Spark[3] = { 0.59f, 0.68f, 1.0f };
    const float Many[3]  = { 20.0f, 20.0f, 40.0f };
    FieldHueHold(Dark, Held, bHeld);
    Check(!bHeld && Held[0] == 0.75f && Held[1] == 0.55f && Held[2] == 1.0f,
          "field hue: a sample with next to no light in it changes nothing");
    FieldHueHold(Spark, Held, bHeld);
    Check(bHeld && Near(Held[0], 0.59f, 1e-5f) && Near(Held[1], 0.68f, 1e-5f) && Held[2] == 1.0f,
          "field hue: the first sample with light in it sets the hue outright");
    FieldHueHold(Dark, Held, bHeld);
    Check(bHeld && Near(Held[0], 0.59f, 1e-5f) && Near(Held[1], 0.68f, 1e-5f),
          "field hue: a dark frame after it leaves the hue as it is");
    FieldHueHold(Many, Held, bHeld);
    Check(Near(Held[0], 0.59f + (0.5f - 0.59f) * 0.1f, 1e-5f) && Near(Held[1], 0.68f + (0.5f - 0.68f) * 0.1f, 1e-5f) &&
          Held[2] == 1.0f, "field hue: a later sample, brought to full strength, moves it a tenth of the way");

    // The field's polygons made as few as they will go. A rectangle of the plane X = 500 of the world, from (Y, Z)
    // to (Y + W, Z + H), far from the world's middle as a level's are; its corners go round either way.
    const auto Rect = [](FVec3* Out, float Y, float Z, float W, float H, bool bTurn)
    {
        const FVec3 P[4] = { { 500.0f, Y, Z }, { 500.0f, Y + W, Z }, { 500.0f, Y + W, Z + H }, { 500.0f, Y, Z + H } };
        for (int i = 0; i < 4; i++)
            Out[i] = P[bTurn ? 3 - i : i];
    };
    // The area of a polygon of that plane, and whether every corner of it is on the plane.
    const auto Flat = [&](const FVec3* P, int Count, bool& bOn)
    {
        float Sum = 0.0f;
        bOn = true;
        for (int i = 0; i < Count; i++)
        {
            const FVec3& A = P[i];
            const FVec3& B = P[(i + 1) % Count];
            Sum += A.Y * B.Z - B.Y * A.Z;
            bOn = bOn && Near(A.X, 500.0f, 1e-3f);
        }
        return 0.5f * ((Sum < 0.0f) ? -Sum : Sum);
    };
    FVec3 Pa[FieldCorners], Pb[FieldCorners], One[FieldCorners];
    bool  bOn = false;
    Rect(Pa, 3000.0f, -200.0f, 80.0f, 176.0f, false);
    Rect(Pb, 3080.0f, -200.0f, 40.0f, 176.0f, true);
    int Num = FieldMergedPolygon(Pa, 4, Pb, 4, One);
    Check(Num == 4 && Near(Flat(One, Num, bOn), 120.0f * 176.0f, 0.5f) && bOn,
          "field merge: two halves of a rectangle are the rectangle - four corners, the cut's ends gone");
    Check(FieldMergedPolygon(Pb, 4, Pa, 4, One) == 4, "field merge: and the other way about");
    Rect(Pb, 3080.0f, -200.0f, 40.0f, 100.0f, false);
    Check(FieldMergedPolygon(Pa, 4, Pb, 4, One) == 0, "field merge: two that make an L stay two");
    Rect(Pb, 3090.0f, -200.0f, 40.0f, 176.0f, false);
    Check(FieldMergedPolygon(Pa, 4, Pb, 4, One) == 0, "field merge: two with a gap between them stay two");
    Rect(Pb, 3000.0f, -200.0f, 80.0f, 176.0f, true);
    Num = FieldMergedPolygon(Pa, 4, Pb, 4, One);
    Check(Num == 4 && Near(Flat(One, Num, bOn), 80.0f * 176.0f, 0.5f),
          "field merge: the same polygon twice - a field's two faces - is one");
    Rect(Pb, 3040.0f, -200.0f, 80.0f, 176.0f, false);
    Num = FieldMergedPolygon(Pa, 4, Pb, 4, One);
    Check(Num == 4 && Near(Flat(One, Num, bOn), 120.0f * 176.0f, 0.5f),
          "field merge: two that lie partly over one another and are convex together are one");
    Rect(Pb, 3040.0f, -100.0f, 80.0f, 176.0f, false);
    Check(FieldMergedPolygon(Pa, 4, Pb, 4, One) == 0,
          "field merge: two that lie partly over one another and are not convex together stay two");
    Rect(Pb, 3080.0f, -200.0f, 40.0f, 176.0f, false);
    for (int i = 0; i < 4; i++)
        Pb[i].X += 1.0f;
    Check(FieldMergedPolygon(Pa, 4, Pb, 4, One) == 0, "field merge: two in planes a unit apart stay two");
    const FVec3 Up[4] = { { 500.0f, 3080.0f, -200.0f }, { 540.0f, 3080.0f, -200.0f }, { 540.0f, 3080.0f, -24.0f },
                          { 500.0f, 3080.0f, -24.0f } };
    Check(FieldMergedPolygon(Pa, 4, Up, 4, One) == 0, "field merge: two that meet at a corner of a cabinet stay two");
    // A triangle and the rest of its rectangle: five corners and three, one polygon of four.
    const FVec3 Rest[5] = { { 500.0f, 3000.0f, -200.0f }, { 500.0f, 3080.0f, -200.0f }, { 500.0f, 3080.0f, -100.0f },
                            { 500.0f, 3040.0f, -24.0f }, { 500.0f, 3000.0f, -24.0f } };
    const FVec3 Tip[3]  = { { 500.0f, 3080.0f, -100.0f }, { 500.0f, 3080.0f, -24.0f }, { 500.0f, 3040.0f, -24.0f } };
    Num = FieldMergedPolygon(Rest, 5, Tip, 3, One);
    Check(Num == 4 && Near(Flat(One, Num, bOn), 80.0f * 176.0f, 0.5f) && bOn,
          "field merge: a corner cut off a rectangle goes back on");

    // A whole list: a sheet cut into a three by three grid of one kind, with one more piece beside it of another
    // kind; and an L of three pieces. The grid is one polygon, the other kind's piece stays, the L is two.
    FVec3 Grid[13 * FieldCorners];
    int   Sizes[13], Kinds[13];
    for (int i = 0; i < 9; i++)
    {
        Rect(Grid + i * FieldCorners, 3000.0f + 200.0f * (float)(i % 3), -300.0f + 200.0f * (float)(i / 3), 200.0f,
             200.0f, (i & 1) != 0);
        Sizes[i] = 4;
        Kinds[i] = 0;
    }
    Rect(Grid + 9 * FieldCorners, 3600.0f, -300.0f, 200.0f, 600.0f, false);
    Sizes[9] = 4;
    Kinds[9] = 1;
    Rect(Grid + 10 * FieldCorners, 5000.0f, 0.0f, 100.0f, 100.0f, false);
    Rect(Grid + 11 * FieldCorners, 5100.0f, 0.0f, 100.0f, 100.0f, false);
    Rect(Grid + 12 * FieldCorners, 5000.0f, 100.0f, 100.0f, 100.0f, false);
    for (int i = 10; i < 13; i++)
    {
        Sizes[i] = 4;
        Kinds[i] = 0;
    }
    const int Left = FieldMergeAll(Grid, Sizes, Kinds, 13);
    float Whole = 0.0f, Other = 0.0f, Ell = 0.0f;
    int   Wholes = 0, Others = 0, Ells = 0;
    bool  bFour = true;
    for (int i = 0; i < Left; i++)
    {
        const float Area = Flat(Grid + i * FieldCorners, Sizes[i], bOn);
        bFour = bFour && Sizes[i] == 4 && bOn;
        if (Kinds[i] == 1)
        {
            Other += Area;
            Others++;
        }
        else if (Grid[i * FieldCorners].Y >= 4999.0f)
        {
            Ell += Area;
            Ells++;
        }
        else
        {
            Whole += Area;
            Wholes++;
        }
    }
    Check(Left == 4 && Wholes == 1 && Near(Whole, 600.0f * 600.0f, 2.0f) && bFour,
          "field merge: a sheet cut into nine is one polygon of four corners again");
    Check(Others == 1 && Near(Other, 200.0f * 600.0f, 1.0f),
          "field merge: a piece of another kind beside it is not taken into it");
    Check(Ells == 2 && Near(Ell, 3.0f * 100.0f * 100.0f, 1.0f),
          "field merge: an L of three pieces is two polygons, with nothing added and nothing lost");

    // The pass's noise: numbers of no pattern - about half on the whole, each channel its own.
    double Sum[4] = { 0.0, 0.0, 0.0, 0.0 };
    int    Same   = 0;
    for (int z = 0; z < 16; z++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
            {
                for (int c = 0; c < 4; c++)
                    Sum[c] += (double)FieldNoiseAt(x, y, z, c) / 65535.0;
                Same += (FieldNoiseAt(x, y, z, 0) == FieldNoiseAt(x, y, z, 1)) ? 1 : 0;
            }
    bool bEven = true;
    for (int c = 0; c < 4; c++)
        bEven = bEven && Sum[c] / 4096.0 > 0.47 && Sum[c] / 4096.0 < 0.53;
    Check(bEven && Same < 8,
          "field volume: the noise is about half on the whole, and its channels are not one another's");
    Check(FieldNoiseAt(3, 5, 7, 2) == FieldNoiseAt(3, 5, 7, 2) &&
          FieldNoiseAt(3, 5, 7, 2) != FieldNoiseAt(4, 5, 7, 2),
          "field volume: the noise is the same number for the same place, another for the next");
}

// The depth of field's focus (RdxMath::DofFocusFor): on what the camera sees of the speaker's head, in front of the
// eyes' place, and never nearer than the nearest focus.
static void TestDofFocus()
{
    printf("depth of field: the focus set on the speaker's face\n");
    Check(fabsf(DofFocusFor(100.0f) - (100.0f - DofFaceLead)) < 1.0e-4f, "dof focus: in front of the eyes' place");
    Check(DofFocusFor(DofNearestFocus + DofFaceLead * 0.5f) == DofNearestFocus, "dof focus: never nearer than allowed");
    Check(DofFocusFor(-50.0f) == DofNearestFocus, "dof focus: a speaker behind the camera asks for the nearest");
    // What the lead is for: in a close-up the side of a head and a face - a little and a little more in front of the
    // eyes' place - both lie within the band that stays sharp, which the face does not with the focus on that place.
    const float Along = 40.0f, Side = Along - 3.0f, Face = Along - 5.5f;
    Check(fabsf(1.0f - DofFocusFor(Along) / Side) < DofSharpBand, "dof focus: a head from the side is sharp");
    Check(fabsf(1.0f - DofFocusFor(Along) / Face) < DofSharpBand, "dof focus: a close-up's face is sharp");
    Check(fabsf(1.0f - Along / Face) > DofSharpBand, "dof focus: the face is not, with the focus on the eyes' place");
    Check(DofFocusFor(Along, 0.0f) == Along && DofFocusFor(Along, 10.0f) == Along - 10.0f, "dof focus: a lead given");
    // Far away the lead moves nothing out of the band.
    Check(fabsf(1.0f - DofFocusFor(300.0f) / 300.0f) < DofSharpBand * 0.5f, "dof focus: far, the head stays sharp");
}

// The game's loading picture laid on a screen (SplashView): at the size at which its middle just fits, in the middle.
static bool SplashSees(float ScreenW, float ScreenH, float PictureW, float PictureH, float WantU, float WantV,
                       float WantUL, float WantVL, float WantX, float WantY, float WantW, float WantH)
{
    float U = -1.0f, V = -1.0f, UL = -1.0f, VL = -1.0f, X = -1.0f, Y = -1.0f, W = -1.0f, H = -1.0f;
    return SplashView(ScreenW, ScreenH, PictureW, PictureH, U, V, UL, VL, X, Y, W, H) && U == WantU && V == WantV &&
           UL == WantUL && VL == WantVL && X == WantX && Y == WantY && W == WantW && H == WantH;
}

static void TestSplashView()
{
    const float PW = 4096.0f, PH = 2048.0f;
    Check(SplashSees(3840.0f, 2160.0f, PW, PH, 1408.0f, 664.0f, 1280.0f, 720.0f, 0.0f, 0.0f, 3840.0f, 2160.0f),
          "splash view: on 3840x2160 the picture's middle 1280x720 fills the screen");
    Check(SplashSees(1920.0f, 1080.0f, PW, PH, 1408.0f, 664.0f, 1280.0f, 720.0f, 0.0f, 0.0f, 1920.0f, 1080.0f),
          "splash view: on 1920x1080 the same middle fills the screen");
    Check(SplashSees(1280.0f, 720.0f, PW, PH, 1408.0f, 664.0f, 1280.0f, 720.0f, 0.0f, 0.0f, 1280.0f, 720.0f),
          "splash view: on 1280x720 the middle stands pixel for pixel, as the game draws it");
    Check(SplashSees(7680.0f, 4320.0f, PW, PH, 1408.0f, 664.0f, 1280.0f, 720.0f, 0.0f, 0.0f, 7680.0f, 4320.0f),
          "splash view: on 7680x4320 the same middle, made larger");
    Check(SplashSees(1920.0f, 1200.0f, PW, PH, 1408.0f, 624.0f, 1280.0f, 800.0f, 0.0f, 0.0f, 1920.0f, 1200.0f),
          "splash view: on a taller 1920x1200 the width decides, and 1280x800 of the picture is seen");
    Check(SplashSees(1080.0f, 1920.0f, PW, PH, 1408.0f, 0.0f, 1280.0f, 2048.0f, 0.0f, 96.0f, 1080.0f, 1728.0f),
          "splash view: on a standing 1080x1920 the width decides, all the picture's height is seen, black beyond it");
    Check(SplashSees(3440.0f, 1440.0f, PW, PH, 1188.0f, 664.0f, 1720.0f, 720.0f, 0.0f, 0.0f, 3440.0f, 1440.0f),
          "splash view: on a wider 3440x1440 the height decides, and more of the backdrop shows at the sides");
    Check(SplashSees(16384.0f, 1440.0f, PW, PH, 0.0f, 664.0f, 4096.0f, 720.0f, 4096.0f, 0.0f, 8192.0f, 1440.0f),
          "splash view: on a very wide screen all the picture's width is seen, black at both sides");
    Check(SplashSees(1920.0f, 1080.0f, 1024.0f, 512.0f, 0.0f, 0.0f, 1024.0f, 512.0f, 0.0f, 60.0f, 1920.0f, 960.0f),
          "splash view: a picture smaller than the middle is all of it, in its own shape");
    float U, V, UL, VL, X, Y, W, H;
    Check(!SplashView(0.0f, 2160.0f, PW, PH, U, V, UL, VL, X, Y, W, H) &&
          !SplashView(3840.0f, 2160.0f, PW, 0.0f, U, V, UL, VL, X, Y, W, H),
          "splash view: a screen or a picture without a size is refused");
    Check(SplashMaxHeight == 2048.0f && SplashMaxWidth == 4096.0f,
          "splash view: the game's limits are its pictures' size");
}

static void TestGrass()
{
    Check(GrassKindOf("CoreTexFoliage.Foliage.ClenGrenGrass_A") == 2 && GrassKindOf(L"clengrengrass_a") == 2 &&
          GrassKindOf("LiteGrenGrass_A") == 2, "grass: the lawns are the long grass, in any case");
    Check(GrassKindOf("BatteryPark.Foliage.NGrass") == 1 && GrassKindOf(L"ngrass") == 1 &&
          GrassKindOf("DarkGrenGrass_A") == 1, "grass: Battery Park's and the darker patches' are the short grass");
    Check(GrassKindOf("BatteryPark.Foliage.NgrassB") == 0 && GrassKindOf(L"NGRASSB") == 0,
          "grass: Battery Park's patch of bare earth grows none");
    Check(GrassKindOf("NGrassC") == 0 && GrassKindOf("Grass") == 0 && GrassKindOf("ClenGrenGrass_AB") == 0 &&
          GrassKindOf("Foliage.Leaves") == 0 && GrassKindOf((const char*)nullptr) == 0,
          "grass: nothing else is grass, not a longer name either");

    const FVec3 A = { 0.0f, 0.0f, 10.0f }, B = { 100.0f, 0.0f, 10.0f }, C = { 0.0f, 100.0f, 10.0f };
    float Row[9];
    Check(GrassTriangle(A, B, C, Row), "grass: a flat triangle is ground");
    CheckNear(Row[8] + Row[6] * 50.0f + Row[7] * 20.0f, 10.0f, 1e-4f, "grass: a flat triangle's height everywhere");
    Check(GrassInside(Row, 10.0f, 10.0f) && GrassInside(Row, 0.0f, 0.0f) && !GrassInside(Row, 60.0f, 60.0f) &&
          !GrassInside(Row, -1.0f, 5.0f), "grass: a root inside the triangle, its corner kept, none outside");
    Check(GrassTriangleTouchesBox(Row, 10.0f, 10.0f, 20.0f, 20.0f) && GrassTriangleTouchesBox(Row, 40.0f, 40.0f, 70.0f, 70.0f) &&
          GrassTriangleTouchesBox(Row, -10.0f, -10.0f, 200.0f, 200.0f),
          "grass: a box inside the triangle, one across its long edge and one round it all are touched");
    Check(!GrassTriangleTouchesBox(Row, 60.0f, 60.0f, 90.0f, 90.0f) &&
          !GrassTriangleTouchesBox(Row, -50.0f, 0.0f, -10.0f, 10.0f),
          "grass: a box past the long edge and one beside the footprint are not");
    {
        // A triangle's tiles by row: in each row's run exactly the tiles the box test finds touched, for triangles of
        // every slant and size, across nought too.
        unsigned Seed  = 777u;
        bool     bSame = true, bAll = true;
        static short Rows[2 * 4096];
        for (int n = 0; n < 300; n++)
        {
            float P[6];
            for (int k = 0; k < 6; k++)
            {
                Seed = Seed * 1664525u + 1013904223u;
                P[k] = (float)((int)((Seed >> 8) % 4000u) - 2000) * ((n % 3 == 0) ? 0.05f : 0.37f);
            }
            const FVec3 Ta = { P[0], P[1], 0.0f }, Tb = { P[2], P[3], 5.0f }, Tc = { P[4], P[5], 2.0f };
            float       R[9];
            if (!GrassTriangle(Ta, Tb, Tc, R))
                continue;
            int X0, Y0, X1, Y1;
            GrassTriangleSpan(R, X0, Y0, X1, Y1);
            if (Y1 - Y0 + 1 > 4096 || !GrassTriangleRows(R, X0, Y0, X1, Y1, Rows))
            {
                bAll = false;
                continue;
            }
            for (int y = Y0; y <= Y1; y++)
                for (int x = X0; x <= X1; x++)
                {
                    const float Lx = (float)x * GrassTile, Ly = (float)y * GrassTile;
                    const bool  bIn = x >= Rows[2 * (y - Y0)] && x <= Rows[2 * (y - Y0) + 1];
                    bSame = bSame && bIn == GrassTriangleTouchesBox(R, Lx, Ly, Lx + GrassTile, Ly + GrassTile);
                }
        }
        Check(bSame && bAll, "grass: a triangle's tiles by row are the tiles the box test finds, each row one run");
    }
    // The presets: each a whole picture - Laci's table.
    {
        const FPresetPicture R = PresetPicture(Preset_Recommended), I = PresetPicture(Preset_INeverAskedForThis);
        const FPresetPicture V = PresetPicture(Preset_Vanilla);
        Check(R.Aa == AaChoice_Auto && R.Upscale == Upscaling_NativeAA && R.ShadowQ == ShadowQuality_High && R.bShadows &&
              R.Hdr == Hdr_Ten && R.Ao == Ao_Ssao && R.Water == Water_High && R.Reflections == Reflections_High &&
              R.Geometry == Geometry_Pom && R.bSoft && R.bVolumetric && R.bDof && R.bEndless && R.bFire && R.bFogRooms &&
              R.Flashlight == Flashlight_Fixed && R.ForceField == ForceField_Volume && R.Grass == Grass_High && R.Trees == Trees_High &&
              R.bWind && !R.bBlur && !R.bRays && !R.bAberr && !R.bVign && !R.bGrain && R.Bloom == Bloom_Off &&
              R.Flare == LensFlare_Off && !R.bFrameGen && !R.bVsync && R.FpsLimit == FpsLimit_Auto,
              "presets: Recommended, line by line");
        Check(I.ShadowQ == ShadowQuality_Rt && I.Hdr == Hdr_On && I.Ao == Ao_Rt && I.Reflections == Reflections_RtMedium &&
              I.Geometry == Geometry_Tess && I.Grass == R.Grass && I.Trees == R.Trees && I.bWind == R.bWind &&
              I.bVolumetric == R.bVolumetric && I.Water == R.Water && I.Flashlight == Flashlight_Everybody,
              "presets: INeverAskedForThis - the rays, HDR and the displacement mapping, the rest Recommended's");
        Check(V.Aa == AaChoice_Off && V.Upscale == Upscaling_Off && !V.bShadows && V.Hdr == Hdr_Off && V.Ao == Ao_Off &&
              V.Water == Water_Off && V.Reflections == Reflections_Default && V.FpsLimit == FpsLimit_Off &&
              V.Geometry == Geometry_Off && !V.bSoft && !V.bVolumetric && !V.bDof && !V.bEndless && !V.bFire &&
              !V.bFogRooms && V.Flashlight == Flashlight_Original && V.ForceField == ForceField_Engine && V.Grass == Grass_Off &&
              V.Trees == Trees_Off && !V.bWind,
              "presets: Vanilla - the game as it shipped, every extra off");
        Check(OriginalOrFixed("Fixed") == 1 && OriginalOrFixed(" original") == 0 && OriginalOrFixed("FIXED\r") == 1 &&
              OriginalOrFixed("True") < 0 && OriginalOrFixed("Fixedx") < 0,
              "settings: Original or Fixed, in any case, nothing else");
        Check(!R.bAntiLag && !I.bAntiLag && !V.bAntiLag, "presets: the AntiLag off in all three");
        Check(OnOrOff("On") == 1 && OnOrOff(" off") == 0 && OnOrOff("ON\r") == 1 && OnOrOff("OFF ") == 0 &&
              OnOrOff("True") < 0 && OnOrOff("Onx") < 0 && OnOrOff("o") < 0 && OnOrOff("") < 0,
              "settings: On or Off, in any case, nothing else");
    }
    // The settings file the menu writes: a key's own line changed in place, a missing key added with the file's own
    // line end.
    {
        char Out[256];
        Check(SettingsSet("; Grass3D=Low\r\nA=1\r\n  grass3d =  Low  \r\nB=2\r\n", "Grass3D", "High", Out, 256) > 0 &&
              strcmp(Out, "; Grass3D=Low\r\nA=1\r\n  grass3d =  High  \r\nB=2\r\n") == 0,
              "settings: the key's own line changed - its spaces, its end and the comment above it as they were");
        Check(SettingsSet("A=1\r\n", "Wind", "On", Out, 256) > 0 && strcmp(Out, "A=1\r\nWind=On\r\n") == 0 &&
              SettingsSet("A=1", "Wind", "On", Out, 256) > 0 && strcmp(Out, "A=1\nWind=On\n") == 0,
              "settings: a missing key added at the end, with the file's own line end");
        Check(SettingsSet("Grass3DX=1\nGrass3D=2\n", "Grass3D", "9", Out, 256) > 0 &&
              strcmp(Out, "Grass3DX=1\nGrass3D=9\n") == 0 && SettingsSet("A=1\n", "Wind", "On", Out, 8) < 0,
              "settings: a longer key is another key; a text that does not fit is refused");
    }
    // A level's plant sheets: two crossed at their middles one plant, a piece set over its trunk one plant; the next
    // bush of a dense row, its sheets reaching over this one's, a plant of its own.
    Check(SheetsShareFoot(0.0f, 0.0f, 20.0f, 0.3f, -0.2f, 20.0f) && SheetsShareFoot(5.0f, 5.0f, 16.0f, 6.0f, 6.5f, 120.0f),
          "plant sheets: crossed at their middles, and a piece over its trunk, are one plant");
    Check(!SheetsShareFoot(0.0f, 0.0f, 20.0f, 0.0f, 11.3f, 20.0f) && !SheetsShareFoot(440.0f, 720.0f, 32.0f, 420.6f, 724.0f, 32.0f),
          "plant sheets: the next bush of a dense row, and of a row of small trees, is a plant of its own");
    float Turned[9];
    Check(GrassTriangle(A, C, B, Turned) && GrassInside(Turned, 10.0f, 10.0f) && !GrassInside(Turned, 60.0f, 60.0f),
          "grass: the corners the other way round are the same ground");
    const FVec3 Up = { 0.0f, 100.0f, 60.0f };
    Check(GrassTriangle(A, B, Up, Row), "grass: a slope of one in two is ground");
    CheckNear(Row[8] + Row[6] * 30.0f + Row[7] * 40.0f, 30.0f, 1e-3f, "grass: a slope's height read off its plane");
    const FVec3 Wall = { 50.0f, 0.0f, 200.0f }, Steep = { 0.0f, 100.0f, 200.0f };
    Check(!GrassTriangle(A, B, Wall, Row) && !GrassTriangle(A, B, Steep, Row) && !GrassTriangle(A, A, B, Row),
          "grass: a wall, a slope steeper than sixty degrees and a triangle with no area grow nothing");

    int X0, Y0, X1, Y1;
    GrassTileSpan(-1.0f, 0.0f, GrassTile - 0.1f, 2.0f * GrassTile, X0, Y0, X1, Y1);
    Check(X0 == -1 && X1 == 0 && Y0 == 0 && Y1 == 2, "grass: the tiles a footprint touches, both ends kept");
    {
        // The grass's pairs put in order by their numbers' bytes: the comparison's order - the band, the row, the
        // column, the triangle, negative rows and columns before the rest - and every pair still there.
        static unsigned long long Keys[2 * 3000];
        unsigned Seed = 12345u;
        bool     bFit = true;
        unsigned long long Sum = 0, Xor = 0;
        for (int i = 0; i < 3000; i++)
        {
            Seed = Seed * 1664525u + 1013904223u;
            const int Band = (int)((Seed >> 8) % 10u);
            Seed = Seed * 1664525u + 1013904223u;
            const int X = (int)((Seed >> 8) % 6000u) - 3000;
            Seed = Seed * 1664525u + 1013904223u;
            const int Y = (int)((Seed >> 8) % 60u) - 30;
            Seed = Seed * 1664525u + 1013904223u;
            const int Tri = (int)((Seed >> 8) % 200000u);
            bFit = bFit && GrassPairKey(Band, X, Y, Tri, Keys[i]);
            Sum += Keys[i];
            Xor ^= Keys[i];
        }
        const unsigned long long* const Got = SortByBytes(Keys, Keys + 3000, 3000);
        bool bOrder = true;
        unsigned long long Sum2 = Got[0], Xor2 = Got[0];
        for (int i = 1; i < 3000; i++)
        {
            int B0, X0k, Y0k, T0, B1, X1k, Y1k, T1;
            GrassPairOfKey(Got[i - 1], B0, X0k, Y0k, T0);
            GrassPairOfKey(Got[i], B1, X1k, Y1k, T1);
            const bool bBefore = (B0 != B1) ? B0 < B1 : (Y0k != Y1k) ? Y0k < Y1k : (X0k != X1k) ? X0k < X1k : T0 <= T1;
            bOrder = bOrder && bBefore;
            Sum2 += Got[i];
            Xor2 ^= Got[i];
        }
        unsigned long long Big = 0;
        Check(bFit && bOrder && Sum2 == Sum && Xor2 == Xor && !GrassPairKey(0, 40000, 0, 0, Big) &&
                  !GrassPairKey(16, 0, 0, 0, Big) && !GrassPairKey(0, 0, 0, 1 << 28, Big),
              "grass: the pairs sorted by their numbers' bytes in the comparison's order, every pair kept; a pair "
              "that does not fit in its number is said so");
        int Bk, Xk, Yk, Tk;
        GrassPairOfKey(Keys[0], Bk, Xk, Yk, Tk);
        unsigned long long K1 = 0;
        Check(GrassPairKey(9, -3000, -30, 199999, K1) && (GrassPairOfKey(K1, Bk, Xk, Yk, Tk), Bk == 9 && Xk == -3000 &&
              Yk == -30 && Tk == 199999), "grass: a pair's number read back is the pair");
    }
    Check(GrassTile == GrassCell * (float)GrassTileCells && GrassTileRoots == 64 &&
          GrassHeadRows + GrassPushMax + GrassTilesPerBlock * GrassTileRows <= GrassBlockRows &&
          GrassHeadRows + GrassPushMax + (GrassTilesPerBlock + 1) * GrassTileRows > GrassBlockRows,
          "grass: a block holds as many tiles as a constant buffer's room has");

    Check(GrassKeep(10.0f) == 1.0f && GrassKeep(GrassNear) == 1.0f && GrassKeep(GrassFar) == 0.0f &&
          GrassKeep(2.0f * GrassNear) < 0.3f && GrassKeep(2.0f * GrassNear) > GrassKeep(3.0f * GrassNear),
          "grass: every root near, fewer further off, none past the far edge");

    float Worst = 0.0f;
    for (int i = -400; i <= 400; i++)
    {
        const float X = (float)i / 100.0f;
        const float D = fabsf(SinTurns(X) - sinf(6.2831853f * X));
        Worst = (D > Worst) ? D : Worst;
    }
    Check(Worst < 0.002f, "grass: a turn's sine within two thousandths");

    const FWind W = WindForLevel(0x1234567u);
    CheckNear(sqrtf(W.DirX * W.DirX + W.DirY * W.DirY), 1.0f, 1e-4f, "wind: its direction is a unit");
    bool bAlong = true, bWithin = true;
    float Least = 10.0f, Most = 0.0f;
    for (int i = 0; i < 2000; i++)
    {
        float Ox, Oy;
        WindAt(W, (float)(i * 37 % 3000), (float)(i * 53 % 2000), (float)i * 0.05f, Ox, Oy);
        const float Along = Ox * W.DirX + Oy * W.DirY, Across = -Ox * W.DirY + Oy * W.DirX;
        bAlong  = bAlong && fabsf(Across) < 1e-5f;
        bWithin = bWithin && Along >= W.Base - 1e-5f && Along <= W.Base + W.Gust + 1e-5f;
        Least   = (Along < Least) ? Along : Least;
        Most    = (Along > Most) ? Along : Most;
    }
    Check(bAlong && bWithin, "wind: it blows its own way, between its base and a full gust");
    Check(Most - Least > 0.5f * W.Gust, "wind: the gusts come and go");
    const FWind W2 = WindForLevel(0x1234567u);
    Check(W2.DirX == W.DirX && W2.DirY == W.DirY, "wind: the same level, the same wind");

    // Its strength over ten minutes of a level, a tenth of a second apart: never below its calmest, never above the
    // level's own; down near the calmest and up near the full at times; slow - no faster than from calm to full in
    // ten seconds.
    float Low = 2.0f, High = -1.0f, Was = WindStrength(0.0f, 0x1234567u), Fastest = 0.0f;
    for (int i = 1; i <= 6000; i++)
    {
        const float S = WindStrength((float)i * 0.1f, 0x1234567u);
        Low   = (S < Low) ? S : Low;
        High  = (S > High) ? S : High;
        Fastest = (fabsf(S - Was) > Fastest) ? fabsf(S - Was) : Fastest;
        Was   = S;
    }
    Check(Low >= WindCalmest - 1e-5f && High <= 1.0f + 1e-5f, "wind: its strength between its calmest and the full");
    Check(Low < WindCalmest + 0.05f && High > 0.9f, "wind: it dies down to all but still, and blows up to the full");
    Check(Fastest < 0.01f, "wind: its strength changes slowly");
    Check(WindStrength(123.4f, 0x1234567u) != WindStrength(123.4f, 0x7654321u), "wind: each level its own");
}

// ---------------------------------------------------------------------------
// The models rounded (MeshRound.h)
// ---------------------------------------------------------------------------

struct FTestMesh
{
    RdxSphere::TList<RdxMeshRound::FV> P;
    RdxSphere::TList<int>              C;
    int Add(float X, float Y, float Z)
    {
        P.Add(RdxMeshRound::V(X, Y, Z));
        return P.Num - 1;
    }
    void Face(int A, int B, int D)
    {
        C.Add(A);
        C.Add(B);
        C.Add(D);
    }
    RdxMeshRound::FPose Pose() const
    {
        RdxMeshRound::FPose R;
        R.Base   = (const char*)P.Data;
        R.Stride = (int)sizeof(RdxMeshRound::FV);
        return R;
    }
};

// The most two faces of one smooth surface turn by at their edge, as the spherifier's angle has it by default.
static const float TestTurn = 180.0f - (float)RdxMath::SpherifierAngleDefault;

// A test mesh as a model and its shape, posed as it stands - with no cap on the move unless one is given.
struct FTestShape
{
    RdxMeshRound::FModel M;
    RdxMeshRound::FShape S;
    bool Make(const FTestMesh& T, const unsigned char* Round = nullptr, float Turn = TestTurn, float Cap = 1e6f)
    {
        if (!M.Make(T.C.Data, T.C.Num / 3, T.P.Num) || !S.Make(M, Round, T.Pose(), Turn, Cap))
            return false;
        S.Pose(T.Pose(), 1);
        return true;
    }
};

// A point of face F's rounded surface at the shares B0, B1, B2 of its corners, with Shape of the curve.
static RdxMeshRound::FV RoundedAt(RdxMeshRound::FShape& S, int F, float B0, float B1, float B2, float Shape)
{
    using namespace RdxMeshRound;
    const int* const C  = S.M->Corner.Data;
    const FV         P0 = S.P.At(C[F * 3]), P1 = S.P.At(C[F * 3 + 1]), P2 = S.P.At(C[F * 3 + 2]);
    const FEdgeTerms E[3] = { S.EdgeTerms(F, 0), S.EdgeTerms(F, 1), S.EdgeTerms(F, 2) };
    return P0 * B0 + P1 * B1 + P2 * B2 + PatchMoveAt(E, B0, B1, B2) * Shape;
}

// Every edge two faces share has the very same curve from both: the terms of one face's way along it are the other's
// the other way round. Each worked out afresh, not the ones kept for the other face.
static bool TermsAgree(RdxMeshRound::FShape& S)
{
    const int* const C = S.M->Corner.Data;
    for (int F = 0; F < S.M->Faces; F++)
        for (int e = 0; e < 3; e++)
        {
            const int A = S.Weld[C[F * 3 + e]], B = S.Weld[C[F * 3 + (e + 1) % 3]];
            for (int G = F + 1; G < S.M->Faces; G++)
                for (int k = 0; k < 3; k++)
                {
                    const int GA = S.Weld[C[G * 3 + k]], GB = S.Weld[C[G * 3 + (k + 1) % 3]];
                    if (!(GA == B && GB == A))
                        continue;
                    S.Pose(S.P, S.Stamp + 1);
                    const RdxMeshRound::FEdgeTerms X = S.EdgeTerms(F, e);
                    S.Pose(S.P, S.Stamp + 1);
                    const RdxMeshRound::FEdgeTerms Y = S.EdgeTerms(G, k);
                    for (int q = 0; q < RdxMeshRound::EDGE_SLOTS; q++)
                    {
                        const int o = (q + 2) % RdxMeshRound::EDGE_SLOTS;
                        if (memcmp(&X.T[q], &Y.T[o], sizeof(X.T[q])) != 0 || X.Room[q] != Y.Room[o])
                            return false;
                    }
                }
        }
    return true;
}

// The most any edge's curve stands off its straight edge, as a share of the edge's length (Long: in units instead).
static float MostBend(RdxMeshRound::FShape& S, bool Long = false)
{
    const int* const C    = S.M->Corner.Data;
    float            Most = 0.0f;
    for (int F = 0; F < S.M->Faces; F++)
        for (int e = 0; e < 3; e++)
        {
            const RdxMeshRound::FEdgeTerms E = S.EdgeTerms(F, e);
            const float L = RdxMeshRound::Length(S.P.At(C[F * 3 + (e + 1) % 3]) - S.P.At(C[F * 3 + e]));
            for (int t = 1; t < 32; t++)
            {
                const float D = RdxMeshRound::Length(RdxMeshRound::EdgeCurveAt(E, (float)t / 32.0f));
                Most          = fmaxf(Most, Long ? D : D / fmaxf(L, 1e-6f));
            }
        }
    return Most;
}

// A prism of N sides round the upright, R from its axis: its side in rings at the heights Z[0 .. Rings), each ring of N
// quads to the next, and a lid of a fan at each end (Bottom, Top: 0 none). The vertex of side i at ring j is j * N + i.
static void TestPrism(FTestMesh& M, int N, float R, const float* Z, int Rings, bool Bottom, bool Top,
                      const float* Radius = nullptr)
{
    for (int j = 0; j < Rings; j++)
        for (int i = 0; i < N; i++)
        {
            const float A  = 2.0f * 3.14159265f * (float)i / (float)N;
            const float Rr = Radius ? Radius[j] : R;
            M.Add(Rr * cosf(A), Rr * sinf(A), Z[j]);
        }
    for (int j = 0; j + 1 < Rings; j++)
        for (int i = 0; i < N; i++)
        {
            const int B0 = j * N + i, B1 = j * N + (i + 1) % N;
            M.Face(B0, B1, B1 + N);
            M.Face(B0, B1 + N, B0 + N);
        }
    if (Bottom)
    {
        const int Low = M.Add(0.0f, 0.0f, Z[0]);
        for (int i = 0; i < N; i++)
            M.Face(Low, (i + 1) % N, i);
    }
    if (Top)
    {
        const int High = M.Add(0.0f, 0.0f, Z[Rings - 1]);
        for (int i = 0; i < N; i++)
            M.Face(High, (Rings - 1) * N + i, (Rings - 1) * N + (i + 1) % N);
    }
}

static void TestMeshRound()
{
    using namespace RdxMeshRound;
    printf("models rounded\n");
    const float Shape = 1.0f;
    CheckNear(AngleOf(1.0f, 1.0f), 0.785398f, 1e-4f, "models rounded: an angle of 45 degrees");
    CheckNear(AngleOf(0.5f, -0.866025f), 2.617994f, 1e-4f, "models rounded: an angle of 150 degrees");
    CheckNear(AngleOf(0.0f, -2.0f), 3.141593f, 1e-4f, "models rounded: a straight angle");
    // The setting's angle: held to its range, on its five-degree steps; the distance held to its range.
    Check(RdxMath::SpherifierAngleOf(105) == 105 && RdxMath::SpherifierAngleOf(107) == 105 &&
              RdxMath::SpherifierAngleOf(108) == 110 && RdxMath::SpherifierAngleOf(10) == 90 &&
              RdxMath::SpherifierAngleOf(400) == 175,
          "models rounded: the angle setting is held to 90..175 in steps of five");
    Check(RdxMath::SpherifierDistanceOf(8) == 8 && RdxMath::SpherifierDistanceOf(0) == 1 &&
              RdxMath::SpherifierDistanceOf(-3) == 1 && RdxMath::SpherifierDistanceOf(40) == 16,
          "models rounded: the distance setting is held to 1..16");
    // The presets switch it, nothing more: off in Vanilla, on in the other two.
    Check(RdxMath::PresetPicture(RdxMath::Preset_Recommended).bSpherifier &&
              RdxMath::PresetPicture(RdxMath::Preset_INeverAskedForThis).bSpherifier &&
              !RdxMath::PresetPicture(RdxMath::Preset_Vanilla).bSpherifier,
          "models rounded: the spherifier is on in Recommended and INeverAskedForThis, off in Vanilla");
    // A flat polygon of several faces moved along its plane as one (PolyPlaneMoveAt): on its outline as the outline's own
    // curve there, along the plane alone; nothing at a corner; and smoothly on from the outline inside.
    {
        const FV Ends[8] = { V(0.0f, 0.0f, 0.0f),   V(10.0f, 0.0f, 0.0f), V(10.0f, 0.0f, 0.0f), V(10.0f, 10.0f, 0.0f),
                             V(10.0f, 10.0f, 0.0f), V(0.0f, 10.0f, 0.0f), V(0.0f, 10.0f, 0.0f), V(0.0f, 0.0f, 0.0f) };
        FEdgeTerms Terms[4];
        for (int g = 0; g < 4; g++)
        {
            // Each bend out of the square along its plane, and up off it.
            const FV Way = Ends[g * 2 + 1] - Ends[g * 2], Out = V(Way.Y, -Way.X, 0.0f) * 0.1f;
            for (int k = 0; k < EDGE_SLOTS; k++)
            {
                Terms[g].T[k]    = (k == 0 || k == 2) ? Out + V(0.0f, 0.0f, 0.5f) : V(0.0f, 0.0f, 0.0f);
                Terms[g].Room[k] = 0.5f;
            }
        }
        const FV Up    = V(0.0f, 0.0f, 1.0f);
        FV       Curve = EdgeCurveAt(Terms[0], 0.3f);
        Curve          = Curve - Up * Dot(Curve, Up);
        Check(Length(PolyPlaneMoveAt(Terms, Ends, 4, Up, V(3.0f, 0.0f, 0.0f)) - Curve) < 1e-5f,
              "models rounded: a flat polygon's one move is its outline's own on the outline, along its plane");
        Check(Length(PolyPlaneMoveAt(Terms, Ends, 4, Up, V(10.0f, 10.0f, 0.0f))) < 1e-6f,
              "models rounded: ... nothing at a corner");
        Check(Length(PolyPlaneMoveAt(Terms, Ends, 4, Up, V(3.0f, 0.01f, 0.0f)) - Curve) < 0.01f * Length(Curve) + 1e-4f,
              "models rounded: ... and goes on smoothly from the outline inside");
        const FV Middle = PolyPlaneMoveAt(Terms, Ends, 4, Up, V(5.0f, 5.0f, 0.0f));
        Check(fabsf(Dot(Middle, Up)) < 1e-6f && Length(Middle) < 0.5f * Length(Curve),
              "models rounded: ... along its plane only, and its sides' opposite bows meet in its middle");
    }
    // A box: every edge a crease along a straight line, the faces' own diagonals flat - nothing bows.
    {
        FTestMesh M;
        for (int i = 0; i < 8; i++)
            M.Add((i & 1) ? 10.0f : -10.0f, (i & 2) ? 10.0f : -10.0f, (i & 4) ? 10.0f : -10.0f);
        const int Q[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
        for (int q = 0; q < 6; q++)
        {
            M.Face(Q[q][0], Q[q][1], Q[q][2]);
            M.Face(Q[q][0], Q[q][2], Q[q][3]);
        }
        FTestShape T;
        Check(T.Make(M), "models rounded: a box makes a shape");
        Check(T.S.Fans == 24, "models rounded: a box's corners fall into a fan for each face at each corner");
        Check(MostBend(T.S) < 1e-5f, "models rounded: nothing of a box bows");
        Check(T.M.FindFace(Q[2][2], Q[2][0], Q[2][1]) == 4 && T.M.FindFace(0, 7, 3) < 0,
              "models rounded: a face is found by its three vertices in any order, and no other");
        // Known by how far apart its vertices are numbered: face 4 is 0, 1, 5 - one and five after the first.
        const int* First = nullptr;
        const int  Count = T.M.Signature(1, 5, &First);
        bool       Has   = false;
        for (int i = 0; i < Count; i++)
            Has = Has || First[i] == 4;
        Check(Count >= 1 && Has, "models rounded: a face is found by how far apart its vertices are numbered");
        Check(T.M.Signature(3, 2, &First) == 0, "models rounded: ... and no face by numbers that cannot be");
        const FV N = T.M.PlainNormal(M.Pose(), 7);
        CheckNear(N.X, 0.57735f, 1e-4f, "models rounded: a box corner's plain normal leans out between its faces");
        int Inner = 0;
        for (int E = 0; E < T.S.Edges; E++)
            Inner += (T.S.EdgeKind[E] == FShape::Edge_Inner) ? 1 : 0;
        Check(Inner == 6, "models rounded: a box's faces' own diagonals are inner edges, its edges creases");
    }
    // A square pot, wider at the top, with a floor: a box's turns, sloping - nothing bows either.
    {
        FTestMesh M;
        for (int i = 0; i < 4; i++)
        {
            const float X = (i & 1) ? 1.0f : -1.0f, Y = (i & 2) ? 1.0f : -1.0f;
            M.Add(X * 8.0f, Y * 8.0f, 0.0f);
            M.Add(X * 12.0f, Y * 12.0f, 20.0f);
        }
        // Bottom corners 0, 2, 4, 6 at (-,-) (+,-) (-,+) (+,+), each with its top corner after it.
        const int Ring[4] = { 0, 2, 6, 4 };
        for (int k = 0; k < 4; k++)
        {
            const int B0 = Ring[k], B1 = Ring[(k + 1) % 4];
            M.Face(B0, B1, B1 + 1);
            M.Face(B0, B1 + 1, B0 + 1);
        }
        M.Face(0, 6, 2);
        M.Face(0, 4, 6);
        FTestShape T;
        Check(T.Make(M), "models rounded: a square pot makes a shape");
        Check(MostBend(T.S) < 1e-5f, "models rounded: a square pot keeps its flat sides");
    }
    // A can of twelve sides with its lids: the side round, the rims round in the lids' planes, the lids flat, the side
    // straight from rim to rim.
    {
        FTestMesh   M;
        const int   N = 12;
        const float R = 10.0f, H = 30.0f;
        const float Z[2] = { 0.0f, H };
        TestPrism(M, N, R, Z, 2, true, true);
        FTestShape T;
        Check(T.Make(M), "models rounded: a can makes a shape");
        FShape& S = T.S;
        Check(TermsAgree(S), "models rounded: both faces of each of a can's edges bend it alike");
        // Faces: 2 N side, then N bottom, then N top (corners: middle, ring i, ring i + 1).
        float RimWorst = 0.0f, RimLift = 0.0f, Upright = 0.0f, Lid = 0.0f, SideWorst = 0.0f, FlatWorst = 0.0f;
        for (int i = 0; i < N; i++)
        {
            const int Side = i * 2, Cap = 3 * N + i;
            // The top rim: the lid's edge from corner 1 to 2.
            const FV    Mid = RoundedAt(S, Cap, 0.0f, 0.5f, 0.5f, Shape);
            const float Rr  = sqrtf(Mid.X * Mid.X + Mid.Y * Mid.Y);
            RimWorst        = fmaxf(RimWorst, fabsf(Rr - R) / R);
            RimLift         = fmaxf(RimLift, fabsf(Mid.Z - H));
            // The side's upright edge: the first side face's from corner 1 (B1) to 2 (B1 + N).
            const FEdgeTerms U  = S.EdgeTerms(Side, 1);
            const FEdgeTerms Sp = S.EdgeTerms(Cap, 0);
            for (int k = 0; k < EDGE_SLOTS; k++)
            {
                Upright = fmaxf(Upright, Length(U.T[k]));
                // The lid's spokes bend in the lid's plane alone, as its rim does.
                Lid = fmaxf(Lid, fabsf(Sp.T[k].Z));
            }
            Lid                 = fmaxf(Lid, fabsf(RoundedAt(S, Cap, 0.4f, 0.3f, 0.3f, Shape).Z - H));
            // The side at half height, across a face: on the circle within a little.
            for (int t = 1; t < 8; t++)
            {
                const float s = (float)t / 8.0f;
                const FV    P = RoundedAt(S, Side, 0.5f - 0.5f * s, 0.5f * s, 0.5f, Shape);
                const FV    F = RoundedAt(S, Side, 0.5f - 0.5f * s, 0.5f * s, 0.5f, 0.0f);
                SideWorst     = fmaxf(SideWorst, fabsf(sqrtf(P.X * P.X + P.Y * P.Y) - R) / R);
                FlatWorst     = fmaxf(FlatWorst, fabsf(sqrtf(F.X * F.X + F.Y * F.Y) - R) / R);
            }
        }
        Check(RimWorst < 0.004f, "models rounded: a can's rim bows onto its circle");
        Check(RimLift < 1e-3f, "models rounded: a can's rim bows in the lid's plane");
        Check(Upright < 1e-3f, "models rounded: a can's side stays straight from rim to rim");
        Check(Lid < 1e-3f, "models rounded: a can's lid stays flat");
        Check(SideWorst < 0.006f && FlatWorst > 0.03f, "models rounded: a can's side comes out round");
    }
    // A bottle: twelve sides, a long side between a short bevel at its foot and a short shoulder up to a flat top. The
    // side stays straight from bevel to shoulder - no barrel - and round across; the shoulder rounds out.
    {
        FTestMesh   M;
        const int   N = 12;
        const float R = 10.0f;
        const float Z[4]      = { 0.0f, 2.0f, 40.0f, 43.0f };
        const float Radius[4] = { 8.5f, 10.0f, 10.0f, 7.0f };
        TestPrism(M, N, R, Z, 4, true, true, Radius);
        FTestShape T;
        Check(T.Make(M), "models rounded: a bottle makes a shape");
        FShape& S = T.S;
        Check(TermsAgree(S), "models rounded: both faces of each of a bottle's edges bend it alike");
        // The side's faces are the second ring of quads: faces 2 N .. 4 N, a quad's first triangle B0, B1, B1 + N.
        float Barrel = 0.0f, Round = 0.0f, Flat = 0.0f;
        for (int i = 0; i < N; i++)
        {
            const int F = 2 * N + i * 2;
            // Up the side's upright edge, from a fifth to four fifths of its height: on the side's straight line.
            const FV A = S.P.At(N + (i + 1) % N), B = S.P.At(2 * N + (i + 1) % N);
            for (int t = 1; t < 5; t++)
            {
                const float u = 0.2f + 0.15f * (float)t;
                const FV    P = RoundedAt(S, F, 0.0f, 1.0f - u, u, Shape);
                const FV    L = A * (1.0f - u) + B * u;
                Barrel        = fmaxf(Barrel, Length(P - L));
            }
            // Across the face's middle, below half height: round.
            const FV P  = RoundedAt(S, F, 0.5f, 0.1f, 0.4f, Shape);
            const FV Fl = RoundedAt(S, F, 0.5f, 0.1f, 0.4f, 0.0f);
            Round       = fmaxf(Round, fabsf(sqrtf(P.X * P.X + P.Y * P.Y) - R) / R);
            Flat        = fmaxf(Flat, fabsf(sqrtf(Fl.X * Fl.X + Fl.Y * Fl.Y) - R) / R);
        }
        Check(Barrel < 0.02f, "models rounded: a bottle's long side stays straight between its short bevel and shoulder");
        Check(Round < 0.01f && Flat > 0.03f, "models rounded: ... and round across");
        // The shoulder's quads (third ring, faces 4 N .. 6 N): the middle of a shoulder's upright edge stands out of
        // the straight edge, rounding it.
        const FV A = S.P.At(2 * N + 1), B = S.P.At(3 * N + 1);
        const FV P = RoundedAt(S, 4 * N, 0.0f, 0.5f, 0.5f, Shape);
        Check(Length(P - (A + B) * 0.5f) > 0.05f, "models rounded: ... while its shoulder rounds");
    }
    // A ball of 16 x 8: smooth all over, every point nearer its sphere than the flat faces were.
    {
        FTestMesh   M;
        const int   Around = 16, Up = 8;
        const float R = 20.0f;
        const int   South = M.Add(0.0f, 0.0f, -R);
        for (int j = 1; j < Up; j++)
            for (int i = 0; i < Around; i++)
            {
                const float Th = 3.14159265f * (float)j / (float)Up - 1.5707963f;
                const float Ph = 2.0f * 3.14159265f * (float)i / (float)Around;
                M.Add(R * cosf(Th) * cosf(Ph), R * cosf(Th) * sinf(Ph), R * sinf(Th));
            }
        const int North = M.Add(0.0f, 0.0f, R);
        auto      At    = [&](int j, int i) { return 1 + (j - 1) * Around + (i % Around); };
        for (int i = 0; i < Around; i++)
        {
            M.Face(South, At(1, i + 1), At(1, i));
            M.Face(North, At(Up - 1, i), At(Up - 1, i + 1));
            for (int j = 1; j < Up - 1; j++)
            {
                M.Face(At(j, i), At(j, i + 1), At(j + 1, i + 1));
                M.Face(At(j, i), At(j + 1, i + 1), At(j + 1, i));
            }
        }
        FTestShape T;
        Check(T.Make(M), "models rounded: a ball makes a shape");
        FShape& S = T.S;
        Check(S.Fans == M.P.Num, "models rounded: a ball has one fan at each vertex");
        Check(TermsAgree(S), "models rounded: both faces of each of a ball's edges bend it alike");
        float       Worst = 0.0f, FlatWorst = 0.0f;
        const float Bs[6][3] = { { 0.5f, 0.5f, 0.0f }, { 0.0f, 0.5f, 0.5f }, { 0.5f, 0.0f, 0.5f },
                                 { 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f }, { 0.6f, 0.2f, 0.2f }, { 0.2f, 0.6f, 0.2f } };
        for (int F = 0; F < T.M.Faces; F++)
            for (int b = 0; b < 6; b++)
            {
                Worst     = fmaxf(Worst, fabsf(Length(RoundedAt(S, F, Bs[b][0], Bs[b][1], Bs[b][2], Shape)) - R) / R);
                FlatWorst = fmaxf(FlatWorst, fabsf(Length(RoundedAt(S, F, Bs[b][0], Bs[b][1], Bs[b][2], 0.0f)) - R) / R);
            }
        Check(Worst < 0.01f && FlatWorst > 0.03f, "models rounded: a ball comes out round");
        // Its plain normals lean as a ball's do: out from its middle.
        float Lean = 1.0f;
        for (int v = 0; v < M.P.Num; v++)
        {
            const FV N = T.M.PlainNormal(M.Pose(), v), P = M.P[v];
            Lean       = fminf(Lean, Dot(N, P * (1.0f / R)));
        }
        Check(Lean > 0.98f, "models rounded: a ball's plain normals point out from its middle");
        // Moved and turned as a whole, it bends the same, turned with it - read a stride apart, as the engine's samples.
        struct FSample
        {
            FV    At;
            float Rest[19];
        };
        RdxSphere::TList<FSample> Turned;
        Turned.Resize(M.P.Num);
        const float Ca = cosf(0.7f), Sa = sinf(0.7f);
        for (int v = 0; v < M.P.Num; v++)
        {
            const FV& P  = M.P[v];
            Turned[v].At = V(Ca * P.X - Sa * P.Z + 100.0f, P.Y - 50.0f, Sa * P.X + Ca * P.Z + 300.0f);
        }
        const FEdgeTerms Before = S.EdgeTerms(5, 1);
        FPose            Moved;
        Moved.Base   = (const char*)Turned.Data;
        Moved.Stride = (int)sizeof(FSample);
        S.Pose(Moved, 2);
        const FEdgeTerms After = S.EdgeTerms(5, 1);
        CheckNear(Length(After.T[0]), Length(Before.T[0]), 1e-3f, "models rounded: a ball moved and turned bends as much");
        CheckNear(After.T[0].Y, Before.T[0].Y, 1e-3f, "models rounded: ... turned with it");
        CheckNear(After.T[0].X, Ca * Before.T[0].X - Sa * Before.T[0].Z, 1e-3f, "models rounded: ... the same way");
        // Bent once at rest: the moved, turned and twice as large ball fits it, and takes its terms turned and doubled.
        S.Pose(M.Pose(), 3);
        S.MakeRest();
        Check(S.bRest, "models rounded: a ball's rest is made");
        for (int v = 0; v < M.P.Num; v++)
        {
            const FV& P  = M.P[v];
            Turned[v].At = V(2.0f * (Ca * P.X - Sa * P.Z) + 100.0f, 2.0f * P.Y - 50.0f, 2.0f * (Sa * P.X + Ca * P.Z) + 300.0f);
        }
        S.Pose(Moved, 4);
        const FEdgeTerms Fresh = S.EdgeTerms(5, 1);
        S.Pose(Moved, 5);
        Check(S.Fit(), "models rounded: a ball moved, turned and scaled fits its rest");
        const FEdgeTerms Kept = S.EdgeTerms(5, 1);
        float            Off  = 0.0f;
        for (int k = 0; k < EDGE_SLOTS; k++)
            Off += Length(Kept.T[k] - Fresh.T[k]);
        CheckNear(Off, 0.0f, 2e-3f * Length(Fresh.T[0]), "models rounded: ... and bends as worked out afresh");
        // A part of it moved in itself - one vertex, a hair more than the fit allows - and it does not fit.
        Turned[At(4, 3)].At = Turned[At(4, 3)].At + V(0.0f, 0.0f, 0.1f);
        S.Pose(Moved, 6);
        Check(!S.Fit(), "models rounded: a ball with one vertex out of its rest does not fit it");
    }
    // A face that is not drawn keeps the edges it shares: neither side bows them; and an open edge at a sharp corner of
    // its outline stays straight.
    {
        FTestMesh M;
        M.Add(0.0f, 0.0f, 0.0f);
        M.Add(10.0f, 0.0f, 0.0f);
        M.Add(0.0f, 10.0f, 1.0f);
        M.Add(10.0f, 10.0f, 3.0f);
        M.Face(0, 1, 2);
        M.Face(1, 3, 2);
        const unsigned char Round[2] = { 1, 0 };
        FTestShape T;
        Check(T.Make(M, Round), "models rounded: a bent sheet makes a shape");
        Check(MostBend(T.S) < 1e-6f, "models rounded: an edge beside a face not drawn, and an open one at sharp "
                                     "corners, stay straight");
    }
    // A hexagonal opening in a flat ring: free, its outline turning softly (60 degrees) at every corner - it rounds into
    // a circle in the ring's plane; the ring's outer outline, a hexagon as well, too.
    {
        FTestMesh M;
        for (int i = 0; i < 6; i++)
        {
            const float A = 2.0f * 3.14159265f * (float)i / 6.0f;
            M.Add(4.0f * cosf(A), 4.0f * sinf(A), 0.0f);
            M.Add(10.0f * cosf(A), 10.0f * sinf(A), 0.0f);
        }
        for (int i = 0; i < 6; i++)
        {
            const int I0 = 2 * i, I1 = 2 * ((i + 1) % 6);
            M.Face(I0, I0 + 1, I1 + 1);
            M.Face(I0, I1 + 1, I1);
        }
        FTestShape T;
        Check(T.Make(M), "models rounded: a ring makes a shape");
        // The inner outline's edge (I0 to I1) is the second quad triangle's edge from corner 2 to 0, I1 to I0.
        float Worst = 0.0f, Lift = 0.0f;
        for (int i = 0; i < 6; i++)
        {
            const FV P = RoundedAt(T.S, 2 * i + 1, 0.5f, 0.0f, 0.5f, Shape);
            Worst      = fmaxf(Worst, fabsf(sqrtf(P.X * P.X + P.Y * P.Y) - 4.0f) / 4.0f);
            Lift       = fmaxf(Lift, fabsf(P.Z));
        }
        Check(Worst < 0.04f && Lift < 1e-4f, "models rounded: a free hexagonal opening rounds into a circle in its plane");
        Check(TermsAgree(T.S), "models rounded: ... both faces of each inner edge bending it alike");
    }
    // A knob laid on a plate: an eight-sided cylinder standing on a square plate, its foot an open outline lying on the
    // plate - the foot rounds in the plate's plane and stays on it, the knob's side rounds; the plate's own square
    // outline, turning sharply, stays.
    {
        FTestMesh   M;
        const float Plate[4][2] = { { -10.0f, -10.0f }, { 10.0f, -10.0f }, { 10.0f, 10.0f }, { -10.0f, 10.0f } };
        for (int k = 0; k < 4; k++)
            M.Add(Plate[k][0], Plate[k][1], 0.0f);
        M.Face(0, 1, 2);
        M.Face(0, 2, 3);
        const float Z[2] = { 0.0f, 3.0f };
        FTestMesh   K;
        TestPrism(K, 8, 3.0f, Z, 2, false, true);
        const int Base = M.P.Num;
        for (int v = 0; v < K.P.Num; v++)
            M.Add(K.P[v].X, K.P[v].Y, K.P[v].Z);
        for (int c = 0; c < K.C.Num; c += 3)
            M.Face(Base + K.C[c], Base + K.C[c + 1], Base + K.C[c + 2]);
        FTestShape T;
        Check(T.Make(M), "models rounded: a knob on a plate makes a shape");
        // The foot: the knob's first ring of side faces, edge from corner 0 to 1 (B0 to B1, at the plate).
        float Foot = 0.0f, Side = 0.0f;
        for (int i = 0; i < 8; i++)
        {
            const FEdgeTerms E = T.S.EdgeTerms(2 + i * 2, 0);
            for (int k = 0; k < EDGE_SLOTS; k++)
                Foot = fmaxf(Foot, fabsf(E.T[k].Z));
            Foot = fmaxf(Foot, fabsf(RoundedAt(T.S, 2 + i * 2, 0.5f, 0.5f, 0.0f, Shape).Z));
            const FV P         = RoundedAt(T.S, 2 + i * 2, 0.5f, 0.05f, 0.45f, Shape);
            const FV F         = RoundedAt(T.S, 2 + i * 2, 0.5f, 0.05f, 0.45f, 0.0f);
            Side               = fmaxf(Side, Length(P - F));
        }
        Check(Foot < 1e-5f, "models rounded: a knob's foot laid on a plate rounds on the plate's plane");
        Check(Side > 0.01f, "models rounded: ... while its side rounds");
        Check(MostBend(T.S) < 1.0f && Length(T.S.EdgeTerms(0, 0).T[0]) < 1e-6f,
              "models rounded: ... and the plate's sharp square outline stays");
    }
    // A sharp cone of eight sides: its tip's fan looks along the cone, nearly down its edges - no edge bends by more than
    // an end bending by half a soft turn gives, however the fans fall.
    {
        FTestMesh M;
        const int Tip = M.Add(0.0f, 0.0f, 30.0f);
        for (int i = 0; i < 8; i++)
        {
            const float A = 2.0f * 3.14159265f * (float)i / 8.0f;
            M.Add(5.0f * cosf(A), 5.0f * sinf(A), 0.0f);
        }
        for (int i = 0; i < 8; i++)
            M.Face(Tip, 1 + i, 1 + (i + 1) % 8);
        FTestShape T;
        Check(T.Make(M), "models rounded: a sharp cone makes a shape");
        Check(MostBend(T.S) <= 2.0f * BendTop * EndSinOf(TestTurn) + 1e-4f,
              "models rounded: no edge of a sharp cone bends like a spike");
    }
    // A long wall round a soft corner (a polygon of two long sides, 30 degrees apart, standing 20 high): with a cap of 8
    // the bend is done near the corner - no point of it moves more than about 8, and half way along the walls are where
    // they were.
    {
        FTestMesh   M;
        const float L = 1000.0f, Ca = cosf(0.2618f), Sa = sinf(0.2618f);
        const int   A0 = M.Add(-L, 0.0f, 0.0f), A1 = M.Add(-L, 0.0f, 20.0f);
        const int   B0 = M.Add(0.0f, 0.0f, 0.0f), B1 = M.Add(0.0f, 0.0f, 20.0f);
        const int   C0 = M.Add(L * Ca, L * Sa, 0.0f), C1 = M.Add(L * Ca, L * Sa, 20.0f);
        M.Face(A0, B0, B1);
        M.Face(A0, B1, A1);
        M.Face(B0, C0, C1);
        M.Face(B0, C1, B1);
        FTestShape T;
        Check(T.Make(M, nullptr, TestTurn, 8.0f), "models rounded: a long wall round a soft corner makes a shape");
        const float Most = MostBend(T.S, true);
        const FV    Half = RoundedAt(T.S, 0, 0.5f, 0.5f, 0.0f, Shape);
        Check(Most < 8.5f && Most > 1.0f, "models rounded: a long wall's bend at a soft corner moves it by about the cap");
        Check(fabsf(Half.Y) < 0.01f, "models rounded: ... and half way along the wall stands where it was");
    }
    // Two vertices at one place are one: the fans and the bends go across them - and so are two a hair apart.
    {
        FTestMesh M;
        M.Add(0.0f, 0.0f, 0.0f);
        M.Add(10.0f, 0.0f, 2.0f);
        M.Add(0.0f, 10.0f, 2.0f);
        M.Add(10.0f, 0.0f, 2.0f);           // 3 at 1's place
        M.Add(0.0f, 10.0f, 2.0f + 1e-5f);   // 4 a hair from 2's place
        M.Add(10.0f, 10.0f, 6.0f);
        M.Face(0, 1, 2);
        M.Face(3, 5, 4);
        FTestShape T;
        Check(T.Make(M), "models rounded: a sheet split at a seam makes a shape");
        Check(T.S.Weld[3] == 1 && T.S.Weld[4] == 2 && T.S.EdgeKind[T.S.EdgeOf[1]] == FShape::Edge_Soft,
              "models rounded: the seam's vertices are welded, its faces across each other");
        Check(TermsAgree(T.S), "models rounded: the seam bends alike from both sides");
    }
    // A hexagonal prism - the cooler's recess, a six-sided pillar: its faces meet at 120 degrees, a turn of 60. Rounded at
    // the default angle, the side comes out round and the rims round in the lids' planes; at an angle sharper than 120 is
    // wide (130: a turn of at most 50), it stays a hexagon.
    {
        FTestMesh   M;
        const float R = 10.0f, H = 6.0f;
        const float Z[2] = { 0.0f, H };
        TestPrism(M, 6, R, Z, 2, true, true);
        FTestShape T;
        Check(T.Make(M), "models rounded: a hexagonal prism makes a shape");
        Check(TermsAgree(T.S), "models rounded: both faces of each of a hexagon's edges bend it alike");
        // The side's middle across a face, against the circle through its corners: the flat face stands 13 per cent
        // inside it; rounded, the middle comes out to within a few per cent of it.
        const FV    Mid  = RoundedAt(T.S, 0, 0.5f, 0.25f, 0.25f, Shape);
        const FV    Flat = RoundedAt(T.S, 0, 0.5f, 0.25f, 0.25f, 0.0f);
        const float Out = sqrtf(Mid.X * Mid.X + Mid.Y * Mid.Y) / R, In = sqrtf(Flat.X * Flat.X + Flat.Y * Flat.Y) / R;
        Check(In < 0.88f && Out > 0.95f && Out < 1.01f, "models rounded: a hexagon's side comes out round");
        // The top lid's triangle (middle, ring i, ring i + 1) is face 18 on: its edge from corner 1 to 2 is the rim.
        const FV Rim = RoundedAt(T.S, 18, 0.0f, 0.5f, 0.5f, Shape);
        Check(fabsf(Rim.Z - H) < 1e-3f && sqrtf(Rim.X * Rim.X + Rim.Y * Rim.Y) / R > 0.95f,
              "models rounded: a hexagon's rim bows round in its lid's plane");
        FTestShape Sharp;
        Check(Sharp.Make(M, nullptr, 50.0f), "models rounded: a hexagonal prism makes a shape at a sharper angle");
        Check(MostBend(Sharp.S) < 1e-5f, "models rounded: a hexagon stays one where the angle is set sharper than 120");
    }
}

// ---------------------------------------------------------------------------
// The level rounded (LevelRound.h)
// ---------------------------------------------------------------------------

// A level made of polygons for the tests: places, and polygons by their places in order round them (their normals
// from that order).
struct FTestLevel
{
    RdxSphere::TList<RdxSphere::FV3>     Pts;
    RdxSphere::TList<RdxSphere::FPolyIn> Polys;
    void Poly(const RdxSphere::FV3* Q, int N)
    {
        RdxSphere::FPolyIn P;
        P.First = Pts.Num;
        P.Count = N;
        for (int k = 0; k < N; k++)
            Pts.Add(Q[k]);
        // Newell's normal.
        double X = 0.0, Y = 0.0, Z = 0.0;
        for (int k = 0; k < N; k++)
        {
            const RdxSphere::FV3& A = Q[k];
            const RdxSphere::FV3& B = Q[(k + 1) % N];
            X += (A.Y - B.Y) * (A.Z + B.Z);
            Y += (A.Z - B.Z) * (A.X + B.X);
            Z += (A.X - B.X) * (A.Y + B.Y);
        }
        const double L = sqrt(X * X + Y * Y + Z * Z);
        P.Nx  = X / L;
        P.Ny  = Y / L;
        P.Nz  = Z / L;
        P.W   = P.Nx * Q[0].X + P.Ny * Q[0].Y + P.Nz * Q[0].Z;
        P.Use = RdxSphere::Use_Wall;
        Polys.Add(P);
    }
    // A pillar of N sides, R from its axis, H high, standing on the floor: its sides, its top and its foot's polygon.
    void Pillar(int N, double R, double H)
    {
        for (int i = 0; i < N; i++)
        {
            const double A0 = 2.0 * RdxSphere::Pi * i / N, A1 = 2.0 * RdxSphere::Pi * (i + 1) / N;
            const RdxSphere::FV3 Q[4] = { RdxSphere::V3(R * cos(A0), R * sin(A0), 0.0),
                                          RdxSphere::V3(R * cos(A1), R * sin(A1), 0.0),
                                          RdxSphere::V3(R * cos(A1), R * sin(A1), H),
                                          RdxSphere::V3(R * cos(A0), R * sin(A0), H) };
            Poly(Q, 4);
        }
        RdxSphere::FV3 Top[64], Foot[64];
        for (int i = 0; i < N; i++)
        {
            const double A = 2.0 * RdxSphere::Pi * i / N;
            Top[i]          = RdxSphere::V3(R * cos(A), R * sin(A), H);
            Foot[N - 1 - i] = RdxSphere::V3(R * cos(A), R * sin(A), 0.0);
        }
        Poly(Top, N);
        Poly(Foot, N);
    }
};

// Whether the triangles of the result close up: every edge between two of their places used by exactly two triangles.
static bool LevelClosed(const RdxSphere::FResult& R)
{
    struct FEdge
    {
        int A, B, Count;
    };
    RdxSphere::TList<FEdge> Edges;
    for (int t = 0; t < R.Tri.Num / 3; t++)
        for (int k = 0; k < 3; k++)
        {
            const int A = R.Tri[t * 3 + k], B = R.Tri[t * 3 + (k + 1) % 3];
            const int Lo = (A < B) ? A : B, Hi = (A < B) ? B : A;
            int       e  = 0;
            while (e < Edges.Num && !(Edges[e].A == Lo && Edges[e].B == Hi))
                e++;
            if (e == Edges.Num)
            {
                FEdge New = { Lo, Hi, 0 };
                Edges.Add(New);
            }
            Edges[e].Count++;
        }
    for (int e = 0; e < Edges.Num; e++)
        if (Edges[e].Count != 2)
            return false;
    return Edges.Num > 0;
}

using RdxLevelRound::PackedMoveAt;

static void TestLevelRound()
{
    printf("level rounded\n");
    const double Angle = (double)RdxMath::SpherifierAngleDefault;
    // A pillar of twelve sides: its sides come out round, its top and foot stay in their planes, rounding with them; the
    // whole stays closed, and nothing moves further than the cap.
    {
        FTestLevel        L;
        RdxSphere::FResult R;
        const double       Rad = 64.0, H = 128.0;
        L.Pillar(12, Rad, H);
        Check(RdxLevelRound::Round(L.Pts.Data, L.Pts.Num, L.Polys.Data, L.Polys.Num, 8.0, Angle, R),
              "level rounded: a pillar is made");
        bool bAll = true;
        for (int p = 0; p < L.Polys.Num; p++)
            bAll = bAll && R.PolyTris[p] > 0;
        Check(bAll && R.Touched == 14, "level rounded: every polygon of a pillar moves");
        Check(LevelClosed(R), "level rounded: a pillar rounded stays closed - nothing opens between its polygons");
        double Worst = 0.0, FlatWorst = 0.0, Lid = 0.0;
        for (int p = 0; p < 12; p++)
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                for (int k = 0; k < 3; k++)
                {
                    const RdxSphere::FV3& M = R.Moved[R.Tri[t * 3 + k]];
                    const RdxSphere::FV3& O = R.Orig[R.Tri[t * 3 + k]];
                    Worst     = fmax(Worst, fabs(sqrt(M.X * M.X + M.Y * M.Y) - Rad) / Rad);
                    FlatWorst = fmax(FlatWorst, fabs(sqrt(O.X * O.X + O.Y * O.Y) - Rad) / Rad);
                }
        for (int p = 12; p < 14; p++)
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                for (int k = 0; k < 3; k++)
                {
                    const RdxSphere::FV3& M = R.Moved[R.Tri[t * 3 + k]];
                    Lid = fmax(Lid, fmin(fabs(M.Z), fabs(M.Z - H)));
                }
        Check(Worst < 0.006 && FlatWorst > 0.02, "level rounded: a pillar's sides come out round");
        Check(Lid < 1e-3, "level rounded: a pillar's top and foot stay in their planes");
        Check(R.LargestMove <= 8.0 + 1e-4 && R.LargestMove > 0.5, "level rounded: no point moves further than the cap");
        // Its light sides: each side polygon along its two soft edges, with the polygon across, taking that one's light
        // in half its own width; the top and foot along none (their edges with the sides are creases).
        const double Width  = 2.0 * Rad * sin(RdxSphere::Pi / 12.0);
        bool         bSides = R.PolySideCount.Num == 14;
        for (int p = 0; p < 12 && bSides; p++)
        {
            bSides = R.PolySideCount[p] == 2;
            for (int i = R.PolySideFirst[p]; i < R.PolySideFirst[p] + R.PolySideCount[p] && bSides; i++)
            {
                const RdxSphere::FLightSide& Sd = R.LightSides[i];
                const RdxSphere::FLightPart& Pt = R.LightParts[Sd.PartFirst];
                bSides = Sd.PartCount == 1 && (Pt.Other == (p + 1) % 12 || Pt.Other == (p + 11) % 12) &&
                         fabs(Pt.Reach - 0.5 * Width) < 1e-3 && fabs(Pt.Depth - Width) < 1e-3 && fabs(Pt.T0) < 1e-6 &&
                         fabs(Pt.T1 - H) < 1e-6 && fabs(RdxSphere::Dot(Sd.Into, Sd.Into) - 1.0) < 1e-9;
                // The way into the polygon points from the side to its other soft edge.
                const RdxSphere::FV3 Mid = (L.Pts[L.Polys[p].First] + L.Pts[L.Polys[p].First + 2]) * 0.5;
                bSides = bSides && RdxSphere::Dot(Mid - Sd.A, Sd.Into) > 0.4 * Width;
            }
        }
        Check(bSides && R.PolySideCount[12] == 0 && R.PolySideCount[13] == 0,
              "level rounded: a pillar's sides take the light across their soft edges in, half their width deep");
        // The card's copy, read as the relief's shader reads it: every place on the sides moves as the CPU moves it
        // (MoveOnPolygon), and a place off the pillar on none.
        RdxSphere::TList<unsigned int> Words;
        const bool                     bPacked = RdxLevelRound::PackRound(R, Words);
        double                         Off     = 0.0;
        bool                           bSame   = bPacked;
        for (int p = 0; p < 12 && bSame; p++)
        {
            const RdxSphere::FV3* const Q = &L.Pts[L.Polys[p].First];
            for (int i = 1; i < 10 && bSame; i++)
                for (int j = 1; j < 10 && bSame; j++)
                {
                    const RdxSphere::FV3 P = Q[0] + (Q[1] - Q[0]) * (i / 10.0) + (Q[3] - Q[0]) * (j / 10.0);
                    RdxSphere::FV3       Cpu = RdxSphere::V3(0.0, 0.0, 0.0), Gpu = Cpu;
                    bSame = RdxLevelRound::MoveOnPolygon(R, p, P, 2.0, Cpu) && PackedMoveAt(Words, P, Gpu);
                    if (bSame)
                        Off = fmax(Off, RdxSphere::Length(Cpu - Gpu));
                }
        }
        RdxSphere::FV3 None;
        Check(bSame && Off < 1e-3 && !PackedMoveAt(Words, RdxSphere::V3(0.0, 0.0, 0.5 * H), None),
              "level rounded: the card's copy moves a place as the polygon does, and one off the polygons not at all");
    }
    // The relief's floor (PackRound, PackedPieceMoveAt): a pillar whose sides the relief may be drawn on - a place
    // well inside any of their drawn triangles moves on the card to where that triangle is drawn, and one off the
    // pillar onto none.
    {
        FTestLevel         L;
        RdxSphere::FResult R;
        L.Pillar(12, 64.0, 128.0);
        for (int p = 0; p < 12; p++)
            L.Polys[p].Use |= RdxSphere::Use_Relief;
        RdxSphere::TList<unsigned int> Words;
        bool bSame = RdxLevelRound::Round(L.Pts.Data, L.Pts.Num, L.Polys.Data, L.Polys.Num, 8.0, Angle, R) &&
                     RdxLevelRound::PackRound(R, Words);
        double Off = 0.0;
        int    Samples = 0;
        for (int p = 0; p < 12 && bSame; p++)
        {
            const RdxSphere::FV3 Np = RdxSphere::V3(L.Polys[p].Nx, L.Polys[p].Ny, L.Polys[p].Nz);
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p] && bSame; t++)
            {
                const RdxSphere::FV3 O0 = R.Orig[R.Tri[t * 3]], O1 = R.Orig[R.Tri[t * 3 + 1]];
                const RdxSphere::FV3 O2 = R.Orig[R.Tri[t * 3 + 2]];
                const RdxSphere::FV3 Ar = RdxSphere::Cross(O1 - O0, O2 - O0);
                if (RdxSphere::Dot(Ar, Ar) < 1e-6 * RdxSphere::Dot(O1 - O0, O1 - O0) * RdxSphere::Dot(O2 - O0, O2 - O0))
                    continue;
                const RdxSphere::FV3 M0 = R.Moved[R.Tri[t * 3]], M1 = R.Moved[R.Tri[t * 3 + 1]];
                const RdxSphere::FV3 M2 = R.Moved[R.Tri[t * 3 + 2]];
                for (int i = 1; i <= 2 && bSame; i++)
                    for (int j = 1; i + j <= 3 && bSame; j++)
                    {
                        const double         B1 = i / 4.0, B2 = j / 4.0, B0 = 1.0 - B1 - B2;
                        const RdxSphere::FV3 W  = O0 * B0 + O1 * B1 + O2 * B2;
                        RdxSphere::FV3       Mv = RdxSphere::V3(0.0, 0.0, 0.0);
                        bSame = RdxLevelRound::PackedPieceMoveAt(Words, W, Np, Mv);
                        Off   = fmax(Off, RdxSphere::Length(W + Mv - (M0 * B0 + M1 * B1 + M2 * B2)));
                        Samples++;
                    }
            }
        }
        RdxSphere::FV3 None;
        Check(bSame && Samples > 100 && Off < 1e-3 &&
                  !RdxLevelRound::PackedPieceMoveAt(Words, RdxSphere::V3(0.0, 0.0, 64.0), RdxSphere::V3(1.0, 0.0, 0.0),
                                                    None),
              "relief floor: the card moves a place of a pillar's side to where its drawn triangle has it, and one off "
              "the pillar not at all");
        // And its normal, what the relief is lifted along: at the middle of each rounded edge between two sides, a step
        // in from either side, turned off each side's plane towards the other's, and the same from both - the two sides
        // lift their shared edge alike; at a side's middle its plane's own.
        double MostApart = 0.0, LeastTurn = 180.0, MostMid = 0.0;
        int    Edges = 0;
        for (int p = 0; p < 12 && bSame; p++)
        {
            const RdxSphere::FPolyIn& P  = L.Polys[p];
            const RdxSphere::FV3      Np = RdxSphere::V3(P.Nx, P.Ny, P.Nz);
            RdxSphere::FV3            Mid = RdxSphere::V3(0.0, 0.0, 0.0);
            for (int c = 0; c < P.Count; c++)
                Mid = Mid + L.Pts[P.First + c] * (1.0 / P.Count);
            RdxSphere::FV3 Mv = RdxSphere::V3(0.0, 0.0, 0.0), Nm = Np;
            bSame     = RdxLevelRound::PackedPieceMoveAt(Words, Mid, Np, Mv, nullptr, &Nm);
            MostMid   = fmax(MostMid, acos(fmin(1.0, RdxSphere::Dot(Nm, Np))) * 57.29578);
            for (int c = 0; c < P.Count && bSame; c++)
            {
                const RdxSphere::FV3 A = L.Pts[P.First + c], B = L.Pts[P.First + (c + 1) % P.Count];
                if (fabs(A.Z - B.Z) < 1.0)
                    continue;
                // The side beside it along that edge.
                int q = -1;
                for (int o = 0; o < 12 && q < 0; o++)
                    for (int d = 0; d < L.Polys[o].Count && o != p && q < 0; d++)
                        if (RdxSphere::Length(L.Pts[L.Polys[o].First + d] - B) < 1e-6 &&
                            RdxSphere::Length(L.Pts[L.Polys[o].First + (d + 1) % L.Polys[o].Count] - A) < 1e-6)
                            q = o;
                if (q < 0)
                    continue;
                const RdxSphere::FV3 Nq = RdxSphere::V3(L.Polys[q].Nx, L.Polys[q].Ny, L.Polys[q].Nz);
                const RdxSphere::FV3 E  = (A + B) * 0.5;
                RdxSphere::FV3       Qm = RdxSphere::V3(0.0, 0.0, 0.0);
                for (int d = 0; d < L.Polys[q].Count; d++)
                    Qm = Qm + L.Pts[L.Polys[q].First + d] * (1.0 / L.Polys[q].Count);
                RdxSphere::FV3 Np2 = Np, Nq2 = Nq;
                bSame =RdxLevelRound::PackedPieceMoveAt(Words, E + (Mid - E) * (0.05 / RdxSphere::Length(Mid - E)), Np, Mv,
                                                         nullptr, &Np2) &&
                        RdxLevelRound::PackedPieceMoveAt(Words, E + (Qm - E) * (0.05 / RdxSphere::Length(Qm - E)), Nq, Mv,
                                                         nullptr, &Nq2);
                MostApart = fmax(MostApart, acos(fmin(1.0, RdxSphere::Dot(Np2, Nq2))) * 57.29578);
                LeastTurn = fmin(LeastTurn, acos(fmin(1.0, RdxSphere::Dot(Np2, Np))) * 57.29578);
                Edges++;
            }
        }
        Check(bSame && Edges == 24 && MostApart < 1.0 && LeastTurn > 5.0 && MostMid < 0.5,
              "relief floor: a pillar's rounded edge turns the relief's lift off either side towards the other, alike from "
              "both, and a side's middle lifts along its plane");
    }
    // The relief's outline (ChordsOf): a pillar's top that the relief may be drawn on gets a record for each of its edges,
    // from its corner to the next along the sides its triangles are drawn with; and the card's copy moves a place on
    // such an edge as those sides do - onto the straight side between the two points round it, not onto the curve.
    {
        FTestLevel         L;
        RdxSphere::FResult R;
        L.Pillar(12, 64.0, 128.0);
        L.Polys[12].Use |= RdxSphere::Use_Relief;
        Check(RdxLevelRound::Round(L.Pts.Data, L.Pts.Num, L.Polys.Data, L.Polys.Num, 8.0, Angle, R) &&
              R.PolyChordFirst.Num == L.Polys.Num, "relief outline: a pillar with a top for the relief is made");
        bool bRing = true;
        for (int p = 0; p < L.Polys.Num; p++)
            bRing = bRing && (p == 12 || R.PolyChordFirst[p] == -1);
        const int First = R.PolyChordFirst[12];
        bRing           = bRing && First >= 0 && First + 12 == R.ChordFirst.Num;
        // The top's triangles' sides that no other of them has, moved: each record's point lies on one of them, and
        // the records go once round.
        RdxSphere::TList<int> Ring;
        for (int t = R.PolyFirst[12]; bRing && t < R.PolyFirst[12] + R.PolyTris[12]; t++)
            for (int k = 0; k < 3; k++)
            {
                const int A = R.Tri[t * 3 + k], B = R.Tri[t * 3 + (k + 1) % 3];
                bool      bBack = false;
                for (int u = R.PolyFirst[12]; u < R.PolyFirst[12] + R.PolyTris[12] && !bBack; u++)
                    for (int j = 0; j < 3 && !bBack; j++)
                        bBack = R.Tri[u * 3 + j] == B && R.Tri[u * 3 + (j + 1) % 3] == A;
                if (!bBack)
                    Ring.Add(A);
            }
        int Points = 0;
        for (int k = 0; k < 12 && bRing; k++)
        {
            const int            e = First + k, F0 = R.ChordFirst[e], N = R.ChordCount[e];
            const RdxSphere::FV3 A = R.ChordEnds[2 * e], B = R.ChordEnds[2 * e + 1];
            bRing = N >= 2 && R.ChordT[F0] == 0.0f && R.ChordT[F0 + N - 1] == 1.0f;
            for (int i = 0; i < N && bRing; i++)
            {
                bRing = i == 0 || R.ChordT[F0 + i] >= R.ChordT[F0 + i - 1];
                const RdxSphere::FV3 Moved = A + (B - A) * R.ChordT[F0 + i] + R.ChordMove[F0 + i];
                bool                 bOn   = false;
                for (int r = 0; r < Ring.Num && !bOn; r++)
                    bOn = RdxSphere::Length(R.Moved[Ring[r]] - Moved) < 1e-3;
                bRing = bRing && bOn;
            }
            Points += N - 1;
        }
        Check(bRing && Points == Ring.Num,
              "relief outline: each edge of the pillar's top is a record of its triangles' sides from corner to corner");
        // The card's copy, at each record's points and halfway between them; and where the curve itself would put the
        // halfway place, off the straight side.
        RdxSphere::TList<unsigned int> Words;
        bool   bSame = bRing && RdxLevelRound::PackRound(R, Words);
        double Off = 0.0, Bow = 0.0;
        for (int k = 0; k < 12 && bSame; k++)
        {
            const int            e = First + k, F0 = R.ChordFirst[e], N = R.ChordCount[e];
            const RdxSphere::FV3 A = R.ChordEnds[2 * e], B = R.ChordEnds[2 * e + 1];
            for (int i = 0; i + 1 < N && bSame; i++)
            {
                const RdxSphere::FV3 P0 = A + (B - A) * R.ChordT[F0 + i], P1 = A + (B - A) * R.ChordT[F0 + i + 1];
                const RdxSphere::FV3 M0 = R.ChordMove[F0 + i], M1 = R.ChordMove[F0 + i + 1];
                RdxSphere::FV3       G0 = RdxSphere::V3(0.0, 0.0, 0.0), Gh = G0, Curve = G0;
                bSame = RdxLevelRound::PackedChordAt(Words, (unsigned int)e, P0, G0) &&
                        RdxLevelRound::PackedChordAt(Words, (unsigned int)e, (P0 + P1) * 0.5, Gh);
                if (!bSame)
                    break;
                Off = fmax(Off, fmax(RdxSphere::Length(G0 - M0), RdxSphere::Length(Gh - (M0 + M1) * 0.5)));
                if (PackedMoveAt(Words, (P0 + P1) * 0.5, Curve))
                    Bow = fmax(Bow, RdxSphere::Length(Curve - Gh));
            }
        }
        RdxSphere::FV3       None;
        const RdxSphere::FV3 A0 = R.ChordEnds[2 * First], B0 = R.ChordEnds[2 * First + 1];
        const RdxSphere::FV3 Mid0 = (A0 + B0) * 0.5, Away = Mid0 * (1.0 + 2.0 / RdxSphere::Length(Mid0 - RdxSphere::V3(0.0, 0.0, Mid0.Z)));
        Check(bSame && Off < 1e-3 && Bow > 1e-4 && Bow <= (double)RdxLevelRound::Sag + 0.05 &&
                  !RdxLevelRound::PackedChordAt(Words, (unsigned int)First, RdxSphere::V3(Away.X, Away.Y, Mid0.Z), None) &&
                  !RdxLevelRound::PackedChordAt(Words, (unsigned int)R.ChordFirst.Num, Mid0, None),
              "relief outline: the card's copy puts a place of the edge on the triangles' side, off the curve, and none "
              "off the edge or of no record");
    }
    // A pillar one side of which is two polygons, one over the other: its neighbours' sides along it in two runs, each
    // with its own polygon across.
    {
        FTestLevel        L;
        RdxSphere::FResult R;
        const int    N = 12;
        const double Rad = 64.0, H = 128.0;
        auto At = [&](int i, double Z) { return RdxSphere::V3(Rad * cos(2.0 * RdxSphere::Pi * i / N),
                                                              Rad * sin(2.0 * RdxSphere::Pi * i / N), Z); };
        for (int i = 0; i < N; i++)
        {
            if (i == 0)
            {
                const RdxSphere::FV3 Low[4] = { At(0, 0.0), At(1, 0.0), At(1, 0.5 * H), At(0, 0.5 * H) };
                const RdxSphere::FV3 High[4] = { At(0, 0.5 * H), At(1, 0.5 * H), At(1, H), At(0, H) };
                L.Poly(Low, 4);
                L.Poly(High, 4);
                continue;
            }
            const RdxSphere::FV3 Q[4] = { At(i, 0.0), At(i + 1, 0.0), At(i + 1, H), At(i, H) };
            L.Poly(Q, 4);
        }
        RdxSphere::FV3 Top[N], Foot[N];
        for (int i = 0; i < N; i++)
        {
            Top[i]          = At(i, H);
            Foot[N - 1 - i] = At(i, 0.0);
        }
        L.Poly(Top, N);
        L.Poly(Foot, N);
        const bool bMade = RdxLevelRound::Round(L.Pts.Data, L.Pts.Num, L.Polys.Data, L.Polys.Num, 8.0, Angle, R);
        // Polygon 2 is the side after the split one: its side along the split one, in two runs.
        bool bRuns = false;
        if (bMade && R.PolySideCount.Num == L.Polys.Num && R.PolySideCount[2] == 2)
            for (int i = R.PolySideFirst[2]; i < R.PolySideFirst[2] + 2; i++)
            {
                const RdxSphere::FLightSide& Sd = R.LightSides[i];
                if (Sd.PartCount != 2)
                    continue;
                const RdxSphere::FLightPart& P0 = R.LightParts[Sd.PartFirst];
                const RdxSphere::FLightPart& P1 = R.LightParts[Sd.PartFirst + 1];
                bRuns = ((P0.Other == 0 && P1.Other == 1) || (P0.Other == 1 && P1.Other == 0)) && fabs(P0.T0) < 1e-6 &&
                        fabs(P0.T1 - 0.5 * H) < 1e-6 && fabs(P1.T0 - 0.5 * H) < 1e-6 && fabs(P1.T1 - H) < 1e-6;
            }
        Check(bMade && R.TPoints == 2 && bRuns,
              "level rounded: a side along two polygons across takes each one's light in along its own run");
        // The two halves: each along its two soft edges, nothing along the edge between them (in one plane).
        Check(R.PolySideCount[0] == 2 && R.PolySideCount[1] == 2, "level rounded: ... and the halves along their own");
    }
    // A big tower of twelve sides, 600 from its axis: its sides would stand out 20 - held to the cap of 8.
    {
        FTestLevel        L;
        RdxSphere::FResult R;
        L.Pillar(12, 600.0, 256.0);
        const bool bMade = RdxLevelRound::Round(L.Pts.Data, L.Pts.Num, L.Polys.Data, L.Polys.Num, 8.0, Angle, R);
        Check(bMade && R.LargestMove <= 8.0 + 1e-4 && LevelClosed(R),
              "level rounded: a big tower's sides held to the cap, closed");
        Check(R.Tri.Num / 3 < 12000, "level rounded: ... in no more pieces than the capped curve needs");
    }
    // A box: every corner a right angle - nothing moves, every polygon left to the engine.
    {
        FTestLevel L;
        RdxSphere::FResult R;
        const double S = 100.0;
        const RdxSphere::FV3 C[8] = { RdxSphere::V3(0, 0, 0), RdxSphere::V3(S, 0, 0), RdxSphere::V3(S, S, 0),
                                      RdxSphere::V3(0, S, 0), RdxSphere::V3(0, 0, S), RdxSphere::V3(S, 0, S),
                                      RdxSphere::V3(S, S, S), RdxSphere::V3(0, S, S) };
        const int F[6][4] = { { 3, 2, 1, 0 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 },
                              { 3, 0, 4, 7 } };
        for (int f = 0; f < 6; f++)
        {
            const RdxSphere::FV3 Q[4] = { C[F[f][0]], C[F[f][1]], C[F[f][2]], C[F[f][3]] };
            L.Poly(Q, 4);
        }
        Check(RdxLevelRound::Round(L.Pts.Data, L.Pts.Num, L.Polys.Data, L.Polys.Num, 8.0, Angle, R) && R.Touched == 0,
              "level rounded: nothing of a box moves");
    }
    // A pillar whose foot polygon is split in two by a cut through two of its corners' middles - its corners lie on the
    // sides' bottom edges then, T-junctions: they are put into those edges, and the whole stays closed.
    {
        FTestLevel        L;
        RdxSphere::FResult R;
        const int    N = 12;
        const double Rad = 64.0, H = 128.0;
        for (int i = 0; i < N; i++)
        {
            const double A0 = 2.0 * RdxSphere::Pi * i / N, A1 = 2.0 * RdxSphere::Pi * (i + 1) / N;
            const RdxSphere::FV3 Q[4] = { RdxSphere::V3(Rad * cos(A0), Rad * sin(A0), 0.0),
                                          RdxSphere::V3(Rad * cos(A1), Rad * sin(A1), 0.0),
                                          RdxSphere::V3(Rad * cos(A1), Rad * sin(A1), H),
                                          RdxSphere::V3(Rad * cos(A0), Rad * sin(A0), H) };
            L.Poly(Q, 4);
        }
        RdxSphere::FV3 Top[N];
        for (int i = 0; i < N; i++)
            Top[i] = RdxSphere::V3(Rad * cos(2.0 * RdxSphere::Pi * i / N), Rad * sin(2.0 * RdxSphere::Pi * i / N), H);
        L.Poly(Top, N);
        // The foot cut from the middle of side 0's bottom edge to the middle of side 6's: two halves, each with two
        // corners on the sides' edges.
        auto Corner = [&](int i) { return RdxSphere::V3(Rad * cos(2.0 * RdxSphere::Pi * i / N),
                                                        Rad * sin(2.0 * RdxSphere::Pi * i / N), 0.0); };
        const RdxSphere::FV3 M0 = (Corner(0) + Corner(1)) * 0.5, M6 = (Corner(6) + Corner(7)) * 0.5;
        RdxSphere::FV3 HalfA[16], HalfB[16];
        int            NA = 0, NB = 0;
        // Seen from below (the foot faces down): the corners in the other order.
        HalfA[NA++] = M0;
        for (int i = 0; i >= -5; i--)
            HalfA[NA++] = Corner((i + N) % N);
        HalfA[NA++] = M6;
        HalfB[NB++] = M6;
        for (int i = 6; i >= 1; i--)
            HalfB[NB++] = Corner(i);
        HalfB[NB++] = M0;
        L.Poly(HalfA, NA);
        L.Poly(HalfB, NB);
        Check(RdxLevelRound::Round(L.Pts.Data, L.Pts.Num, L.Polys.Data, L.Polys.Num, 8.0, Angle, R),
              "level rounded: a pillar with a cut foot is made");
        Check(R.TPoints == 2, "level rounded: the cut foot's two corners are put into the sides' edges");
        Check(LevelClosed(R), "level rounded: ... and the pillar stays closed");
    }
    // A glass table: a rim of twelve sides four units wide - its inner and outer walls, its top and its foot - a glass
    // across the inner walls' middle, and a leg standing out under side 0's foot, the foot cut round the leg's top. The
    // glass goes along with the walls at every side, and side 0 bows as the others do: the rim is a closed ring, which
    // the leg under it does not hold flat there.
    {
        FTestLevel         L;
        RdxSphere::FResult R;
        const int    N = 12;
        const double Ri = 32.0, Ro = 36.0, Z0 = 0.0, Z1 = 4.0, Zg = 2.0, Zl = -10.0;
        auto At = [&](double Rad, int i, double Z)
        {
            const double A = 2.0 * RdxSphere::Pi * i / N;
            return RdxSphere::V3(Rad * cos(A), Rad * sin(A), Z);
        };
        for (int i = 0; i < N; i++)
        {
            // The inner wall faces the middle, the outer away from it, the top up and the foot down.
            const RdxSphere::FV3 In[4]   = { At(Ri, i + 1, Z0), At(Ri, i, Z0), At(Ri, i, Z1), At(Ri, i + 1, Z1) };
            const RdxSphere::FV3 Out[4]  = { At(Ro, i, Z0), At(Ro, i + 1, Z0), At(Ro, i + 1, Z1), At(Ro, i, Z1) };
            const RdxSphere::FV3 Top[4]  = { At(Ri, i, Z1), At(Ro, i, Z1), At(Ro, i + 1, Z1), At(Ri, i + 1, Z1) };
            const RdxSphere::FV3 Foot[4] = { At(Ri, i + 1, Z0), At(Ro, i + 1, Z0), At(Ro, i, Z0), At(Ri, i, Z0) };
            L.Poly(In, 4);
            L.Poly(Out, 4);
            L.Poly(Top, 4);
            if (i > 0)
                L.Poly(Foot, 4);
        }
        // Side 0 by how far out from the middle (U) and along it (V): its foot in four round the leg's top, which reaches
        // from a unit past the inner edge to a unit short of the outer, three units either way along.
        const double Ca = cos(RdxSphere::Pi / N), Sa = sin(RdxSphere::Pi / N), Ui = Ri * Ca, Uo = Ro * Ca;
        auto Side = [&](double U, double V, double Z)
        { return RdxSphere::V3(U * Ca - V * Sa, U * Sa + V * Ca, Z); };
        const RdxSphere::FV3 Pieces[4][4] = {
            { Side(Ui, -Ri * Sa, Z0), Side(Ui, -3.0, Z0), Side(Uo, -3.0, Z0), Side(Uo, -Ro * Sa, Z0) },
            { Side(Ui, 3.0, Z0), Side(Ui, Ri * Sa, Z0), Side(Uo, Ro * Sa, Z0), Side(Uo, 3.0, Z0) },
            { Side(Ui, -3.0, Z0), Side(Ui, 3.0, Z0), Side(Ui + 1.0, 3.0, Z0), Side(Ui + 1.0, -3.0, Z0) },
            { Side(Uo - 1.0, -3.0, Z0), Side(Uo - 1.0, 3.0, Z0), Side(Uo, 3.0, Z0), Side(Uo, -3.0, Z0) } };
        for (int k = 0; k < 4; k++)
            L.Poly(Pieces[k], 4);
        const double LegU[4] = { Ui + 1.0, Uo - 1.0, Uo - 1.0, Ui + 1.0 }, LegV[4] = { -3.0, -3.0, 3.0, 3.0 };
        for (int k = 0; k < 4; k++)
        {
            const RdxSphere::FV3 Q[4] = { Side(LegU[k], LegV[k], Zl), Side(LegU[(k + 1) % 4], LegV[(k + 1) % 4], Zl),
                                          Side(LegU[(k + 1) % 4], LegV[(k + 1) % 4], Z0), Side(LegU[k], LegV[k], Z0) };
            L.Poly(Q, 4);
        }
        const RdxSphere::FV3 LegFoot[4] = { Side(LegU[3], LegV[3], Zl), Side(LegU[2], LegV[2], Zl),
                                            Side(LegU[1], LegV[1], Zl), Side(LegU[0], LegV[0], Zl) };
        L.Poly(LegFoot, 4);
        RdxSphere::FV3 Glass[N];
        for (int i = 0; i < N; i++)
            Glass[i] = At(Ri, i, Zg);
        const int GlassPoly = L.Polys.Num;
        L.Poly(Glass, N);
        L.Polys[GlassPoly].Use |= RdxSphere::Use_Sheet;
        Check(RdxLevelRound::Round(L.Pts.Data, L.Pts.Num, L.Polys.Data, L.Polys.Num, 8.0, Angle, R) &&
                  R.PolyTris[GlassPoly] > 0,
              "level rounded: a glass table is made, its glass with it");
        // How far each side of the glass bows out: its places between its two corners, out from the middle.
        double Bow[N];
        for (int i = 0; i < N; i++)
            Bow[i] = 0.0;
        for (int t = R.PolyFirst[GlassPoly]; t < R.PolyFirst[GlassPoly] + R.PolyTris[GlassPoly]; t++)
            for (int k = 0; k < 3; k++)
            {
                const RdxSphere::FV3& O = R.Orig[R.Tri[t * 3 + k]];
                const RdxSphere::FV3& M = R.Moved[R.Tri[t * 3 + k]];
                for (int i = 0; i < N; i++)
                {
                    const RdxSphere::FV3 A = At(Ri, i, Zg), B = At(Ri, i + 1, Zg), AB = B - A;
                    const double         T = RdxSphere::Dot(O - A, AB) / RdxSphere::Dot(AB, AB);
                    if (T < 0.1 || T > 0.9 || RdxSphere::Length(A + AB * T - O) > 1e-3)
                        continue;
                    const double Am = 2.0 * RdxSphere::Pi * (i + 0.5) / N;
                    Bow[i] = fmax(Bow[i], RdxSphere::Dot(M - O, RdxSphere::V3(cos(Am), sin(Am), 0.0)));
                }
            }
        double Least = 1e30;
        for (int i = 1; i < N; i++)
            Least = fmin(Least, Bow[i]);
        Check(Least > 0.3, "level rounded: a glass table's glass bows out at its sides with the rim's walls");
        Check(Bow[0] > 0.8 * Least, "level rounded: ... and as far over the leg as at the others - the ring is not held");
    }
}

// The wall held back before a still polygon (LevelRound.h's HoldBehind): an eight-sided pillar, which the round bows out
// into its circle, and a thin sign before one of its sides - standing half a unit off it with a back of its own, or
// laid flat on it as the levels' signs are, whose back is the side itself and no polygon (the level's own geometry has
// none there). Without the hold the side's middle comes out through the sign. Held: the side under the sign stays
// behind it - behind its back where it has one, behind its front where it lies flat -, every place further than the
// spread from the sign moves to the bit as without the sign, no triangle of the side turns over, and the sign itself
// stays still.
static void TestLevelHold()
{
    printf("level rounded, held behind a still polygon\n");
    const double Angle = (double)RdxMath::SpherifierAngleDefault, Cap = 8.0, Rad = 64.0, H = 128.0;
    const double         C = cos(RdxSphere::Pi / 8.0), S = sin(RdxSphere::Pi / 8.0), Apothem = Rad * C;
    const RdxSphere::FV3 U = RdxSphere::V3(C, S, 0.0), T = RdxSphere::V3(-S, C, 0.0), Z = RdxSphere::V3(0.0, 0.0, 1.0);
    const double Half = 12.0, Z0 = 52.0, Z1 = 76.0, Thick = 2.0;
    auto At = [&](double s, double w, double z) { return U * s + T * w + Z * z; };
    // A quad facing Out: its corners turned round if they come the other way.
    auto Quad = [&](FTestLevel& L, RdxSphere::FV3 A, RdxSphere::FV3 B, RdxSphere::FV3 Cc, RdxSphere::FV3 D,
                    RdxSphere::FV3 Out) {
        RdxSphere::FV3 Q[4] = { A, B, Cc, D };
        if (RdxSphere::Dot(RdxSphere::Cross(B - A, Cc - A), Out) < 0.0)
        {
            Q[1] = D;
            Q[3] = B;
        }
        L.Poly(Q, 4);
    };
    FTestLevel Plain;
    Plain.Pillar(8, Rad, H);
    RdxSphere::FResult R0;
    Check(RdxLevelRound::Round(Plain.Pts.Data, Plain.Pts.Num, Plain.Polys.Data, Plain.Polys.Num, Cap, Angle, R0),
          "level held: the pillar is made without a sign");
    // The drawn surface of side 0 under the sign: on a grid over the sign's outline, how far out the triangle there
    // has moved it (its corners' moves, by the spot's shares of the triangle where it was).
    auto Surface = [&](const RdxSphere::FResult& R, double& Most, int& Seen) {
        Most = -1e9;
        Seen = 0;
        for (int i = 0; i <= 24; i++)
            for (int j = 0; j <= 24; j++)
            {
                const double w = -Half + 2.0 * Half * i / 24.0, z = Z0 + (Z1 - Z0) * j / 24.0;
                for (int t = R.PolyFirst[0]; t < R.PolyFirst[0] + R.PolyTris[0]; t++)
                {
                    const int    a = R.Tri[t * 3], b = R.Tri[t * 3 + 1], c = R.Tri[t * 3 + 2];
                    const double Aw = RdxSphere::Dot(R.Orig[a], T), Az = R.Orig[a].Z;
                    const double Bw = RdxSphere::Dot(R.Orig[b], T), Bz = R.Orig[b].Z;
                    const double Cw = RdxSphere::Dot(R.Orig[c], T), Cz = R.Orig[c].Z;
                    const double Den = (Bw - Aw) * (Cz - Az) - (Cw - Aw) * (Bz - Az);
                    if (fabs(Den) < 1e-12)
                        continue;
                    const double L1 = ((w - Aw) * (Cz - Az) - (Cw - Aw) * (z - Az)) / Den;
                    const double L2 = ((Bw - Aw) * (z - Az) - (w - Aw) * (Bz - Az)) / Den;
                    if (L1 < -1e-9 || L2 < -1e-9 || L1 + L2 > 1.0 + 1e-9)
                        continue;
                    const double Out = RdxSphere::Dot(R.Moved[a] - R.Orig[a], U) * (1.0 - L1 - L2) +
                                       RdxSphere::Dot(R.Moved[b] - R.Orig[b], U) * L1 +
                                       RdxSphere::Dot(R.Moved[c] - R.Orig[c], U) * L2;
                    Most = fmax(Most, Out);
                    Seen++;
                    break;
                }
            }
    };
    double PlainOut;
    int    PlainSeen;
    Surface(R0, PlainOut, PlainSeen);
    for (int Flat = 0; Flat < 2; Flat++)
    {
        const double      Back = Apothem + (Flat ? 0.0 : 0.5), Front = Back + Thick;
        const char* const Kind = Flat ? "laid flat" : "standing off";
        char              What[200];
        auto              Say = [&](const char* Text) {
            snprintf(What, sizeof(What), "level held, %s: %s", Kind, Text);
            return (const char*)What;
        };
        FTestLevel Signed;
        Signed.Pillar(8, Rad, H);
        const int SignFirst = Signed.Polys.Num;
        Quad(Signed, At(Front, -Half, Z0), At(Front, Half, Z0), At(Front, Half, Z1), At(Front, -Half, Z1), U);
        if (!Flat)
            Quad(Signed, At(Back, -Half, Z0), At(Back, Half, Z0), At(Back, Half, Z1), At(Back, -Half, Z1), U * -1.0);
        Quad(Signed, At(Back, Half, Z0), At(Front, Half, Z0), At(Front, Half, Z1), At(Back, Half, Z1), T);
        Quad(Signed, At(Back, -Half, Z0), At(Front, -Half, Z0), At(Front, -Half, Z1), At(Back, -Half, Z1), T * -1.0);
        Quad(Signed, At(Back, -Half, Z1), At(Back, Half, Z1), At(Front, Half, Z1), At(Front, -Half, Z1), Z);
        Quad(Signed, At(Back, -Half, Z0), At(Back, Half, Z0), At(Front, Half, Z0), At(Front, -Half, Z0), Z * -1.0);
        RdxSphere::FResult R1;
        Check(RdxLevelRound::Round(Signed.Pts.Data, Signed.Pts.Num, Signed.Polys.Data, Signed.Polys.Num, Cap, Angle, R1),
              Say("the pillar is made with the sign"));
        Check(PlainSeen == 625 && PlainOut > Front - Apothem, Say("without the hold the side comes out through the sign"));
        double HeldOut;
        int    HeldSeen;
        Surface(R1, HeldOut, HeldSeen);
        Check(HeldSeen == 625 && HeldOut < (Flat ? Front : Back) - Apothem,
              Say("under the sign the drawn side stays behind it, between the places too"));
        Check(R1.HeldStopped > 0, Say("places are stopped before the sign"));
        // Further than the rim and the spread from the sign: as without the sign, to the bit.
        const double Spread = (RdxLevelRound::HoldRimCaps + RdxLevelRound::HoldSpreadCaps) * Cap;
        int          Far = 0, FarSame = 0;
        for (int v = 0; v < R1.Orig.Num; v++)
        {
            const RdxSphere::FV3& O  = R1.Orig[v];
            const double          dw = fmax(0.0, fabs(RdxSphere::Dot(O, T)) - Half);
            const double          dz = fmax(0.0, fmax(Z0 - O.Z, O.Z - Z1));
            const double          ds = fmax(0.0, Back - RdxSphere::Dot(O, U));
            if (sqrt(dw * dw + dz * dz + ds * ds) <= Spread + 1.0)
                continue;
            for (int u = 0; u < R0.Orig.Num; u++)
                if (R0.Orig[u].X == O.X && R0.Orig[u].Y == O.Y && R0.Orig[u].Z == O.Z)
                {
                    Far++;
                    FarSame += (R0.Moved[u].X == R1.Moved[v].X && R0.Moved[u].Y == R1.Moved[v].Y &&
                                R0.Moved[u].Z == R1.Moved[v].Z) ? 1 : 0;
                    break;
                }
        }
        Check(Far > 0 && FarSame == Far, Say("further than the spread from the sign, every place moves as without it"));
        // Side 0's triangles: none turned over.
        int Flipped = 0;
        for (int t = R1.PolyFirst[0]; t < R1.PolyFirst[0] + R1.PolyTris[0]; t++)
        {
            const RdxSphere::FV3& A = R1.Moved[R1.Tri[t * 3]];
            const RdxSphere::FV3  N = RdxSphere::Cross(R1.Moved[R1.Tri[t * 3 + 1]] - A, R1.Moved[R1.Tri[t * 3 + 2]] - A);
            Flipped += (RdxSphere::Dot(N, U) <= 0.0) ? 1 : 0;
        }
        Check(R1.PolyTris[0] > 0 && Flipped == 0, Say("no triangle of the held side turns over"));
        bool bSignStill = true;
        for (int p = SignFirst; p < Signed.Polys.Num; p++)
            bSignStill = bSignStill && R1.PolyTris[p] == 0;
        Check(bSignStill, Say("the sign itself is not moved"));
        // A thing laid on the side - a decal: under the sign it goes with the drawn side, behind the sign; far from it,
        // as the faces' curve moves it, to the bit.
        const RdxSphere::FV3 Under = At(Apothem, 0.0, 0.5 * (Z0 + Z1)), Away = At(Apothem, 0.0, 12.0);
        RdxSphere::FV3       MoveUnder, MoveAway;
        const bool           bUnder = RdxLevelRound::MoveOnPolygon(R1, 0, Under, 2.0, MoveUnder);
        RdxLevelRound::HoldOnPolygon(R1, 0, Under, MoveUnder);
        const bool           bAway = RdxLevelRound::MoveOnPolygon(R1, 0, Away, 2.0, MoveAway);
        const RdxSphere::FV3 Curve = MoveAway;
        RdxLevelRound::HoldOnPolygon(R1, 0, Away, MoveAway);
        Check(bUnder && RdxSphere::Dot(MoveUnder, U) < (Flat ? Front : Back) - Apothem,
              Say("a thing laid on the side under the sign stays behind it"));
        Check(bAway && MoveAway.X == Curve.X && MoveAway.Y == Curve.Y && MoveAway.Z == Curve.Z,
              Say("... and far from it moves as the faces' curve moves it"));
        printf("  %s: side 0 under the sign out by %.2f without the hold, %.2f held; %d stopped, %d eased; %d far ones "
               "alike\n", Kind, PlainOut, HeldOut, R1.HeldStopped, R1.HeldEased, FarSame);
    }
}

// A hard edge's fine bevel (EdgeBevel.h): one right-angled edge and one that turns by 45 degrees - the band's far line
// stays, the edge goes to the round's middle, every point of the band lies on the round, nothing folds or goes outward;
// an inner edge and a nearly flat one are no edges; a wall rising beside a ledge does not follow the ledge's round; a
// plate thinner than twice the radius has no worn solid; and a box of twelve edges, its faces cut into a grid - every
// point the radius off the box worn down by it (a quarter cylinder at each edge, an eighth of a ball at each corner),
// the faces still meeting point for point, nothing turned over or outside the box. With RDX_BEVEL_OBJ naming a folder,
// the boxes are written there as OBJ files for the eye: fine (a band in four slices) and as the level would be cut
// (two slices), for radii 1, 2 and 4.
static RdxSphere::FV3 BevelPoint(double X, double Y, double Z) { return RdxSphere::V3(X, Y, Z); }

static void BevelBox(double S, double H, double R, bool bFine, int& Flipped, int& Outside, int& Seams, double& OffRound,
                     double& Corner, const char* ObjPath)
{
    using RdxBevel::FEdge;
    FEdge Edges[12];
    int   Count = 0;
    const double Least = 0.1, Most = 2.5;
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++)
        {
            const double         Y = a ? S : 0.0, Z = b ? H : 0.0, X = a ? S : 0.0, Y2 = b ? S : 0.0;
            const RdxSphere::FV3 NZ = BevelPoint(0, 0, b ? 1 : -1), NY = BevelPoint(0, a ? 1 : -1, 0);
            const RdxSphere::FV3 NX = BevelPoint(a ? 1 : -1, 0, 0), NY2 = BevelPoint(0, b ? 1 : -1, 0);
            Count += RdxBevel::MakeEdge(BevelPoint(0, Y, Z), BevelPoint(S, Y, Z), NZ, NY, BevelPoint(S / 2, S / 2, Z),
                                        Least, Most, Edges[Count]) ? 1 : 0;
            Count += RdxBevel::MakeEdge(BevelPoint(X, 0, Z), BevelPoint(X, S, Z), NZ, NX, BevelPoint(S / 2, S / 2, Z),
                                        Least, Most, Edges[Count]) ? 1 : 0;
            Count += RdxBevel::MakeEdge(BevelPoint(X, Y2, 0), BevelPoint(X, Y2, H), NX, NY2, BevelPoint(X, S / 2, H / 2),
                                        Least, Most, Edges[Count]) ? 1 : 0;
        }
    Check(Count == 12, "bevel: a box has twelve outward edges");
    // The cuts across each side: the bands' lines, and a few between them.
    auto Lines = [&](double L, double* Out) {
        int N = 0;
        const int In = bFine ? 4 : 2;
        for (int k = 0; k <= In; k++)
            Out[N++] = R * k / In;
        for (int j = 1; j < 4; j++)
            Out[N++] = R + (L - 2 * R) * j / 4;
        for (int k = In; k >= 0; k--)
            Out[N++] = L - R * k / In;
        return N;
    };
    double LX[16], LY[16], LZ[16];
    const int NX = Lines(S, LX), NY = Lines(S, LY), NZ = Lines(H, LZ);
    // Each face: its fixed axis and side, its two axes' lines; the moved points, and two triangles a cell.
    RdxSphere::TList<RdxSphere::FV3> Moved, Orig;
    RdxSphere::TList<int>            Tris;
    Flipped = Outside = Seams = 0;
    OffRound = Corner = 0.0;
    for (int Axis = 0; Axis < 3; Axis++)
        for (int Side = 0; Side < 2; Side++)
        {
            const double*        U   = (Axis == 0) ? LY : LX;
            const int            NU  = (Axis == 0) ? NY : NX;
            const double*        V   = (Axis == 2) ? LY : LZ;
            const int            NV  = (Axis == 2) ? NY : NZ;
            const double         At  = Side ? ((Axis == 2) ? H : S) : 0.0;
            const double         Out = Side ? 1.0 : -1.0;
            const RdxSphere::FV3 Normal = (Axis == 0) ? BevelPoint(Out, 0, 0) : (Axis == 1) ? BevelPoint(0, Out, 0)
                                                                                        : BevelPoint(0, 0, Out);
            const int First = Moved.Num;
            for (int j = 0; j < NV; j++)
                for (int i = 0; i < NU; i++)
                {
                    const RdxSphere::FV3 P = (Axis == 0) ? BevelPoint(At, U[i], V[j])
                                           : (Axis == 1) ? BevelPoint(U[i], At, V[j]) : BevelPoint(U[i], V[j], At);
                    RdxSphere::FV3 M, N = Normal;
                    RdxBevel::Round(Edges, Count, P, R, M, N);
                    // The same place from another face moves the same.
                    for (int k = 0; k < Orig.Num; k++)
                        if (Orig[k].X == P.X && Orig[k].Y == P.Y && Orig[k].Z == P.Z &&
                            RdxSphere::Length(Moved[k] - M) > 1e-12)
                            Seams++;
                    if (M.X < -1e-9 || M.Y < -1e-9 || M.Z < -1e-9 || M.X > S + 1e-9 || M.Y > S + 1e-9 ||
                        M.Z > H + 1e-9)
                        Outside++;
                    // The radius off the worn box, whether moved or not.
                    const double DX = fmax(fmax(R - M.X, M.X - (S - R)), 0.0);
                    const double DY = fmax(fmax(R - M.Y, M.Y - (S - R)), 0.0);
                    const double DZ = fmax(fmax(R - M.Z, M.Z - (H - R)), 0.0);
                    OffRound = fmax(OffRound, fabs(sqrt(DX * DX + DY * DY + DZ * DZ) - R));
                    if (P.X == 0.0 && P.Y == 0.0 && P.Z == 0.0)
                        Corner = M.X;
                    Orig.Add(P);
                    Moved.Add(M);
                }
            for (int j = 0; j + 1 < NV; j++)
                for (int i = 0; i + 1 < NU; i++)
                {
                    const int A = First + j * NU + i, B = A + 1, C = A + NU, D = C + 1;
                    const int T[6] = { A, B, D, A, D, C };
                    for (int t = 0; t < 2; t++)
                    {
                        int I0 = T[t * 3], I1 = T[t * 3 + 1], I2 = T[t * 3 + 2];
                        // Wound so that the face's own normal is its front.
                        const RdxSphere::FV3 N0 = RdxSphere::Cross(Orig[I1] - Orig[I0], Orig[I2] - Orig[I0]);
                        if (RdxSphere::Dot(N0, Normal) < 0.0)
                        {
                            const int Keep = I1;
                            I1 = I2;
                            I2 = Keep;
                        }
                        const RdxSphere::FV3 N1 = RdxSphere::Cross(Moved[I1] - Moved[I0], Moved[I2] - Moved[I0]);
                        if (RdxSphere::Dot(N1, Normal) <= 0.0)
                            Flipped++;
                        Tris.Add(I0);
                        Tris.Add(I1);
                        Tris.Add(I2);
                    }
                }
        }
    if (ObjPath)
    {
        FILE* F = nullptr;
        if (fopen_s(&F, ObjPath, "w") == 0 && F)
        {
            for (int k = 0; k < Moved.Num; k++)
                fprintf(F, "v %.6f %.6f %.6f\n", Moved[k].X, Moved[k].Y, Moved[k].Z);
            for (int k = 0; k + 2 < Tris.Num; k += 3)
                fprintf(F, "f %d %d %d\n", Tris[k] + 1, Tris[k + 1] + 1, Tris[k + 2] + 1);
            fclose(F);
        }
    }
}

// A face cut along its edges' bands (EdgeBevel.h's CutBands): a 64-unit square, its four sides right-angled outward
// edges, cut at the band's width and half of it from each - a five by five grid of pieces over six by six points, each
// piece convex, the pieces' areas the square's; with one edge, three strips along it. Radius 1.
static void TestBevelCut()
{
    printf("a face cut along its edges' bands\n");
    using RdxBevel::FEdge;
    using RdxSphere::V3;
    const RdxSphere::FV3 Q[4] = { V3(0, 0, 0), V3(64, 0, 0), V3(64, 64, 0), V3(0, 64, 0) };
    const RdxSphere::FV3 Up = V3(0, 0, 1), In = V3(32, 32, 0);
    const RdxSphere::FV3 Side[4] = { V3(0, -1, 0), V3(1, 0, 0), V3(0, 1, 0), V3(-1, 0, 0) };
    FEdge Edges[4];
    bool  bMade = true;
    for (int k = 0; k < 4; k++)
        bMade = bMade && RdxBevel::MakeEdge(Q[k], Q[(k + 1) % 4], Up, Side[k], In, 0.1, 2.5, Edges[k]);
    Check(bMade, "bevel cut: the square's four sides are outward edges");
    RdxSphere::TList<RdxSphere::FV3> Points;
    RdxSphere::TList<int>            Pieces;
    // Each piece: convex (its corners turn one way round Up), and its area by the shoelace over its corners.
    auto Look = [&](int& Count, double& Area, bool& bConvex) {
        Count   = 0;
        Area    = 0.0;
        bConvex = true;
        for (int p = 0; p < Pieces.Num; p += Pieces[p] + 1)
        {
            const int n = Pieces[p];
            Count++;
            double A = 0.0;
            for (int k = 0; k < n; k++)
            {
                const RdxSphere::FV3& P0 = Points[Pieces[p + 1 + k]];
                const RdxSphere::FV3& P1 = Points[Pieces[p + 1 + (k + 1) % n]];
                const RdxSphere::FV3& P2 = Points[Pieces[p + 1 + (k + 2) % n]];
                A += P0.X * P1.Y - P1.X * P0.Y;
                bConvex = bConvex && RdxSphere::Dot(RdxSphere::Cross(P1 - P0, P2 - P1), Up) >= -1e-9;
            }
            Area += 0.5 * A;
        }
    };
    int    Count;
    double Area;
    bool   bConvex;
    Check(RdxBevel::CutBands(Q, 4, Up, Edges, 4, 1.0, Points, Pieces), "bevel cut: the square is cut");
    Look(Count, Area, bConvex);
    bool bOnGrid = true;
    for (int i = 0; i < Points.Num; i++)
    {
        auto Grid = [](double V) {
            const double Lines[6] = { 0.0, 0.5, 1.0, 63.0, 63.5, 64.0 };
            for (int k = 0; k < 6; k++)
                if (fabs(V - Lines[k]) < 1e-9)
                    return true;
            return false;
        };
        bOnGrid = bOnGrid && Grid(Points[i].X) && Grid(Points[i].Y) && Points[i].Z == 0.0;
    }
    Check(Count == 25 && Points.Num == 36, "bevel cut: four edges - a five by five grid of pieces over six by six points");
    Check(fabs(Area - 64.0 * 64.0) < 1e-6 && bConvex, "bevel cut: ... each piece convex, together the whole square");
    Check(bOnGrid, "bevel cut: ... every point on the bands' lines and the square's sides");
    const int FourPieces = Count, FourPoints = Points.Num;
    Check(RdxBevel::CutBands(Q, 4, Up, Edges, 1, 1.0, Points, Pieces), "bevel cut: the square is cut along one edge");
    Look(Count, Area, bConvex);
    Check(Count == 3 && Points.Num == 8 && fabs(Area - 64.0 * 64.0) < 1e-6 && bConvex,
          "bevel cut: one edge - three strips along it");
    printf("  four edges: %d pieces, %d points; one edge: %d pieces, %d points\n", FourPieces, FourPoints, Count,
           Points.Num);
}

static void TestEdgeBevel()
{
    printf("a hard edge's fine bevel\n");
    using RdxBevel::FEdge;
    const double Least = 0.1, Most = 2.5;

    // An edge along Z: face A the top (up), reaching towards -X; face B the side, at a right angle or turned by 45
    // degrees from the top, reaching down.
    for (int Turn = 0; Turn < 2; Turn++)
    {
        const double         R  = 2.0;
        const RdxSphere::FV3 NA = BevelPoint(0, 1, 0);
        const RdxSphere::FV3 NB = Turn ? RdxBevel::Unit(BevelPoint(1, 1, 0)) : BevelPoint(1, 0, 0);
        FEdge E;
        Check(RdxBevel::MakeEdge(BevelPoint(0, 0, 0), BevelPoint(0, 0, 10), NA, NB, BevelPoint(-5, 0, 5), Least, Most, E),
              Turn ? "bevel: a 45-degree edge is an edge" : "bevel: a right-angled edge is an edge");
        const double         Band  = RdxBevel::BandWidth(E, R);
        const RdxSphere::FV3 Inner = BevelPoint(0, 0, 5) + E.IntoA * Band - NA * R;
        CheckNear((float)RdxSphere::Dot(Inner, NB), (float)-R, 1e-6f, "bevel: the round's middle is the radius in from both");
        RdxSphere::FV3 M, N;
        RdxSphere::FV3 P = BevelPoint(0, 0, 5) + E.IntoA * (Band * 0.999999);
        RdxBevel::Round(&E, 1, P, R, M, N);
        Check(RdxSphere::Length(M - P) < 1e-4, "bevel: the band's far line stays where it is");
        P = BevelPoint(0, 0, 5) + E.IntoA * (Band * 1.01);
        Check(RdxBevel::Round(&E, 1, P, R, M, N) == 0 && RdxSphere::Length(M - P) == 0.0,
              "bevel: past the band nothing moves");
        Check(RdxBevel::Round(&E, 1, BevelPoint(0, 0, 5), R, M, N) == 2, "bevel: the edge itself is in the band");
        const RdxSphere::FV3 Bisector = RdxBevel::Unit(NA + NB);
        CheckNear((float)RdxSphere::Length(M - (Inner + Bisector * R)), 0.0f, 1e-6f,
                  "bevel: the edge goes to the round's middle");
        CheckNear((float)RdxSphere::Dot(N, Bisector), 1.0f, 1e-6f, "bevel: ... and its normal is halfway between");
        // Every point of both bands on the round, the normal turning one way across, nothing outward.
        double Worst = 0.0, LastAngle = -1.0;
        bool   bOrder = true, bInward = true;
        for (int s = 0; s <= 40; s++)
        {
            const bool   bA     = s <= 20;
            const double Across = (bA ? (20 - s) : (s - 20)) / 20.0 * Band * 0.999999;
            P = BevelPoint(0, 0, 5) + (bA ? E.IntoA : E.IntoB) * Across;
            N = bA ? NA : NB;
            RdxBevel::Round(&E, 1, P, R, M, N);
            const RdxSphere::FV3 Off   = M - Inner;
            const double         Along = RdxSphere::Dot(Off, E.Way);
            Worst = fmax(Worst, fabs(sqrt(RdxSphere::Dot(Off, Off) - Along * Along) - R));
            const double Angle = acos(fmax(-1.0, fmin(1.0, RdxSphere::Dot(N, NA))));
            bOrder &= Angle >= LastAngle - 1e-9;
            LastAngle = Angle;
            bInward &= RdxSphere::Dot(M - P, NA) <= 1e-9 && RdxSphere::Dot(M - P, NB) <= 1e-9;
        }
        CheckNear((float)Worst, 0.0f, 1e-6f, "bevel: every point of the band lies on the round");
        Check(bOrder, "bevel: across the band the round turns one way only - nothing folds");
        Check(bInward, "bevel: nothing goes outward of either face");
        // A wall rising at the ledge's end, in line with the side above the edge: not the ledge's to round.
        P = BevelPoint(0, 1.0, 10);
        Check(RdxBevel::Round(&E, 1, P, R, M, N) == 0, "bevel: a wall rising beside a ledge does not follow its round");
    }
    // An inner edge (a floor meeting a wall going up) is none, nor a nearly flat one.
    {
        FEdge E;
        Check(!RdxBevel::MakeEdge(BevelPoint(0, 0, 0), BevelPoint(0, 0, 10), BevelPoint(0, 1, 0), BevelPoint(-1, 0, 0),
                                  BevelPoint(-5, 0, 5), Least, Most, E),
              "bevel: an inner edge is not bevelled");
        Check(!RdxBevel::MakeEdge(BevelPoint(0, 0, 0), BevelPoint(0, 0, 10), BevelPoint(0, 1, 0),
                                  RdxBevel::Unit(BevelPoint(0.05, 1, 0)), BevelPoint(-5, 0, 5), Least, Most, E),
              "bevel: a nearly flat edge is not bevelled");
    }
    // A plate 1.5 thick: its front between its top's and its bottom's edges has no worn solid for a radius of 1.
    {
        FEdge E[2];
        RdxBevel::MakeEdge(BevelPoint(0, 0, 1.5), BevelPoint(64, 0, 1.5), BevelPoint(0, 0, 1), BevelPoint(0, -1, 0),
                           BevelPoint(32, 32, 1.5), Least, Most, E[0]);
        RdxBevel::MakeEdge(BevelPoint(0, 0, 0), BevelPoint(64, 0, 0), BevelPoint(0, 0, -1), BevelPoint(0, -1, 0),
                           BevelPoint(32, 32, 0), Least, Most, E[1]);
        RdxSphere::FV3 M, N;
        Check(RdxBevel::Round(E, 2, BevelPoint(32, 0, 0.75), 1.0, M, N) == -1,
              "bevel: a plate thinner than twice the radius has no worn solid");
        Check(RdxBevel::Round(E, 2, BevelPoint(32, 0, 0.3), 0.7, M, N) > 0, "bevel: ... and a radius under half its thickness rounds it");
    }
    // A box: every point the radius off the worn box; the faces meet point for point, nothing turns over or leaves the
    // box; the corner goes onto the ball of the radius round the worn box's corner.
    char*  Folder = nullptr;
    size_t FolderLength = 0;
    if (_dupenv_s(&Folder, &FolderLength, "RDX_BEVEL_OBJ") != 0)
        Folder = nullptr;
    for (int f = 0; f < 2; f++)
        for (int r = 0; r < 3; r++)
        {
            const double R = (r == 0) ? 1.0 : (r == 1) ? 2.0 : 4.0;
            char Path[512] = "";
            if (Folder)
                snprintf(Path, sizeof(Path), "%s\\bevel-box-r%d-%s.obj", Folder, (int)R, f ? "fine" : "slices");
            int    Flipped, Outside, Seams;
            double OffRound, Corner;
            BevelBox(64.0, 32.0, R, f != 0, Flipped, Outside, Seams, OffRound, Corner, Folder ? Path : nullptr);
            CheckNear((float)OffRound, 0.0f, 1e-6f, "bevel: every point of a box lies the radius off the worn box");
            Check(Seams == 0, "bevel: a box's faces still meet point for point");
            Check(Flipped == 0, "bevel: no piece of a box's faces turns over");
            Check(Outside == 0, "bevel: nothing of a box goes outside it");
            CheckNear((float)Corner, (float)(R * (1.0 - 1.0 / sqrt(3.0))), 1e-6f,
                      "bevel: a box's corner goes onto the ball round the worn corner");
        }
    free(Folder);
}

// The probe gathered from the picture (RdxMath::Probe*; docs/tukrozodes-szonda-terv.md): the cube's faces as Direct3D
// lays them, there and back; the nearest point a texel keeps; a point carved out once the eye sees past it; a coarser
// level that unknown texels do not darken; and a ray marched through what a probe in a box-shaped room holds, against
// where the ray really leaves the room.
static void TestProbe()
{
    using namespace RdxMath;
    // There and back: every texel's middle on every face, and directions of every kind.
    bool  bBack = true;
    float Worst = 0.0f;
    const int Side = 16;
    for (int f = 0; f < 6; f++)
        for (int y = 0; y < Side; y++)
            for (int x = 0; x < Side; x++)
            {
                float U, V;
                ProbeTexelMiddle(x, y, Side, U, V);
                const FVec3 D = ProbeDirOf(f, U, V);
                int   F2 = -1, X2 = -1, Y2 = -1;
                float U2 = 0.0f, V2 = 0.0f;
                bBack = bBack && ProbeFaceOf(D, F2, U2, V2) && F2 == f;
                ProbeTexelOf(U2, V2, Side, X2, Y2);
                bBack = bBack && X2 == x && Y2 == y;
                const float E = fabsf(U2 - U) + fabsf(V2 - V);
                Worst = (E > Worst) ? E : Worst;
            }
    Check(bBack && Worst < 1e-5f, "probe: a texel's direction falls back in that texel, on its own face");
    // The faces' axes: the middle of each face looks along its own axis, as Direct3D's cube is laid out.
    {
        const FVec3 Axis[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        bool bAxes = true;
        for (int f = 0; f < 6; f++)
        {
            const FVec3 D = ProbeDirOf(f, 0.0f, 0.0f);
            bAxes = bAxes && D.X == Axis[f].X && D.Y == Axis[f].Y && D.Z == Axis[f].Z;
        }
        int   F;
        float U, V;
        Check(bAxes && !ProbeFaceOf(FVec3{ 0.0f, 0.0f, 0.0f }, F, U, V),
              "probe: each face's middle looks along its own axis; a direction of no length has no face");
    }
    // The nearest point wins; the same one again refreshes; a further one is behind; an unknown texel takes any.
    {
        FProbeTexel T = { 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0 };
        ProbeGather(T, 100.0f, FVec3{ 1.0f, 0.0f, 0.0f }, 1);
        const bool bFirst = T.Trust == 1.0f && T.Dist == 100.0f && T.Frame == 1;
        ProbeGather(T, 50.0f, FVec3{ 0.0f, 1.0f, 0.0f }, 2);
        const bool bNearer = T.Dist == 50.0f && T.Colour.Y == 1.0f && T.Frame == 2;
        ProbeGather(T, 80.0f, FVec3{ 0.0f, 0.0f, 1.0f }, 3);
        const bool bFurther = T.Dist == 50.0f && T.Colour.Y == 1.0f && T.Frame == 2;
        ProbeGather(T, 50.5f, FVec3{ 0.5f, 0.5f, 0.0f }, 4);
        const bool bSame = T.Dist == 50.0f && T.Colour.X == 0.5f && T.Frame == 4;
        Check(bFirst && bNearer && bFurther && bSame,
              "probe: a texel keeps the nearest point, refreshes the same one, and a further one does not take it");
    }
    Check(ProbeCarved(100.0f, 140.0f, 4.0f) && !ProbeCarved(100.0f, 102.0f, 4.0f) && !ProbeCarved(100.0f, 60.0f, 4.0f),
          "probe: a point is carved out where the eye sees past it, and kept where something nearer covers it");
    {
        const FProbeTexel Four[4] = { { 30.0f, { 1.0f, 0.0f, 0.0f }, 1.0f, 5 }, { 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0 },
                                      { 20.0f, { 0.0f, 1.0f, 0.0f }, 1.0f, 7 }, { 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0 } };
        const FProbeTexel M = ProbeMipOf(Four);
        Check(fabsf(M.Colour.X - 0.5f) < 1e-6f && fabsf(M.Colour.Y - 0.5f) < 1e-6f && M.Colour.Z == 0.0f &&
                  M.Trust == 0.5f && M.Dist == 20.0f && M.Frame == 7,
              "probe: a coarser texel averages the known ones only, keeps the nearest point and the latest frame");
    }
    // The pyramid's levels follow one another: each starts where the six faces of the one before end.
    {
        int  Sum    = 0;
        bool bStart = true;
        for (int k = 0; k < ProbeLevels; k++)
        {
            bStart = bStart && ProbeLevelStart(k) == Sum;
            Sum += 6 * (ProbeSide >> k) * (ProbeSide >> k);
        }
        Check(bStart && ProbeEntries() == Sum && (ProbeSide >> (ProbeLevels - 1)) == 1,
              "probe: each level of the pyramid starts where the one before ends, and the last is a texel a face");
    }
    // Where it stands: before the largest pane, out on the camera's side. While the camera is near it stays, however
    // the view turns; once the camera has gone off, a pane clearly larger than its own takes it after a while.
    {
        const FVec3 MidA = { 100.0f, 0.0f, 50.0f }, NormA = { -1.0f, 0.0f, 0.0f };
        const FVec3 MidB = { 0.0f, 1600.0f, 50.0f }, NormB = { 0.0f, -1.0f, 0.0f };
        const FVec3 EyeA = { -100.0f, 0.0f, 50.0f }, EyeB = { 0.0f, 1400.0f, 50.0f };
        FProbePlace P;
        // No pane in sight: it stands where the camera is, and follows it once it is off by more than the follow reach.
        const FVec3 EyeNear = { EyeA.X + 100.0f, EyeA.Y, EyeA.Z }, EyeOff = { EyeA.X + 500.0f, EyeA.Y, EyeA.Z };
        const bool bNone = P.Choose(EyeA) && P.bPlaced && !P.bAtPane && P.Centre.X == EyeA.X && !P.Choose(EyeNear) &&
                           P.Choose(EyeOff) && P.Centre.X == EyeOff.X && !P.bAtPane;
        // The first pane drawn takes it at once.
        P.Note(MidA, NormA, 100.0f);
        const bool bFirst = P.Choose(EyeA) && P.bPlaced && P.bAtPane && P.Centre.X == MidA.X - ProbeAhead &&
                            P.Centre.Y == 0.0f && P.Centre.Z == MidA.Z && P.PaneNormal.X == NormA.X &&
                            P.PaneMiddle.X == MidA.X;
        // Near it, a far larger pane does not take it - nor with its own out of sight.
        bool bStays = true;
        for (int f = 0; f < 100; f++)
        {
            if (f & 1)
                P.Note(MidA, NormA, 100.0f);
            P.Note(MidB, NormB, 1000.0f);
            bStays = bStays && !P.Choose(EyeA);
        }
        // Gone off: a pane not clearly larger than its own still does not.
        for (int f = 0; f < 100; f++)
        {
            P.Note(MidA, NormA, 100.0f);
            P.Note(MidB, NormB, 120.0f);
            bStays = bStays && !P.Choose(EyeB);
        }
        int Moved = -1;
        for (int f = 0; f < 2 * ProbeMoveFrames && Moved < 0; f++)
        {
            P.Note(MidA, NormA, 100.0f);
            P.Note(MidB, NormB, 200.0f);
            if (P.Choose(EyeB))
                Moved = f + 1;
        }
        const bool bToB = Moved == ProbeMoveFrames && P.Centre.Y == MidB.Y - ProbeAhead && P.PaneNormal.Y == NormB.Y &&
                          P.PaneMiddle.Y == MidB.Y;
        // Away from the new place, its own pane out of sight a moment, then back: it does not go.
        bool bWaits = true;
        for (int f = 0; f < ProbeMoveFrames - 1; f++)
        {
            P.Note(MidA, NormA, 50.0f);
            bWaits = bWaits && !P.Choose(EyeA);
        }
        P.Note(MidB, NormB, 50.0f);
        bWaits = bWaits && !P.Choose(EyeA);
        P.Note(MidA, NormA, 50.0f);
        bWaits = bWaits && !P.Choose(EyeA) && P.Centre.Y == MidB.Y - ProbeAhead;
        // The camera off, and no pane drawn: after a while it follows the camera again.
        bool bFollows = true;
        for (int f = 0; f < ProbeMoveFrames - 1; f++)
            bFollows = bFollows && !P.Choose(EyeA) && P.bAtPane;
        bFollows = bFollows && P.Choose(EyeA) && !P.bAtPane && P.Centre.X == EyeA.X && P.Centre.Y == EyeA.Y;
        P.Forget();
        Check(bNone && bFirst && bStays && bToB && bWaits && bFollows && !P.bPlaced,
              "probe: with no pane it follows the camera; the first pane takes it, and it stays while the camera is "
              "near; gone off, a clearly larger pane takes it after a while, or with none it follows the camera "
              "again; nowhere after a new level");
    }
    // A box-shaped room round the probe, every texel the point where its middle's line from the centre leaves the room.
    // Rays from places all over the room, every way: where the march says each leaves, against where it does.
    {
        const int   Cube   = 64;
        const FVec3 Centre = { 0.0f, 0.0f, 0.0f };
        const FVec3 Lo = { -200.0f, -150.0f, -100.0f }, Hi = { 300.0f, 250.0f, 120.0f };
        auto Leaves = [&](const FVec3& S, const FVec3& R)
        {
            float T = 1e30f;
            const float Sv[3] = { S.X, S.Y, S.Z }, Rv[3] = { R.X, R.Y, R.Z };
            const float Lv[3] = { Lo.X, Lo.Y, Lo.Z }, Hv[3] = { Hi.X, Hi.Y, Hi.Z };
            for (int a = 0; a < 3; a++)
                if (Rv[a] != 0.0f)
                {
                    const float Ta = ((Rv[a] > 0.0f ? Hv[a] : Lv[a]) - Sv[a]) / Rv[a];
                    T = (Ta < T) ? Ta : T;
                }
            return T;
        };
        static FProbeTexel Probe[6][64][64];
        for (int f = 0; f < 6; f++)
            for (int y = 0; y < Cube; y++)
                for (int x = 0; x < Cube; x++)
                {
                    float U, V;
                    ProbeTexelMiddle(x, y, Cube, U, V);
                    FVec3 D = ProbeDirOf(f, U, V);
                    const float L = sqrtf(Dot(D, D));
                    D = FVec3{ D.X / L, D.Y / L, D.Z / L };
                    Probe[f][y][x] = { Leaves(Centre, D), { (float)f, 0.0f, 0.0f }, 1.0f, 1 };
                }
        auto Lookup = [&](int f, int x, int y) { return &Probe[f][y][x]; };
        unsigned int Seed = 12345u;
        auto Rand = [&]() { Seed = Seed * 1664525u + 1013904223u; return (float)(Seed >> 8) / 16777216.0f; };
        const int Rays = 4000;
        int       Within[3] = { 0, 0, 0 };
        const int StepsOf[3] = { 8, 12, 16 };
        for (int r = 0; r < Rays; r++)
        {
            // A place in the room, a fifth of its size in from every wall; a way of any kind.
            const FVec3 S = { Lo.X + (Hi.X - Lo.X) * (0.2f + 0.6f * Rand()), Lo.Y + (Hi.Y - Lo.Y) * (0.2f + 0.6f * Rand()),
                              Lo.Z + (Hi.Z - Lo.Z) * (0.2f + 0.6f * Rand()) };
            FVec3 R = { Rand() * 2.0f - 1.0f, Rand() * 2.0f - 1.0f, Rand() * 2.0f - 1.0f };
            const float L = sqrtf(Dot(R, R));
            if (L < 0.1f)
                continue;
            R = FVec3{ R.X / L, R.Y / L, R.Z / L };
            const float Truth = Leaves(S, R);
            const FVec3 At    = { S.X + R.X * Truth, S.Y + R.Y * Truth, S.Z + R.Z * Truth };
            for (int k = 0; k < 3; k++)
            {
                float T = 0.0f;
                if (!ProbeMarch(Centre, Cube, S, R, 900.0f, StepsOf[k], 8, Lookup, T))
                    continue;
                const FVec3 Got = { S.X + R.X * T, S.Y + R.Y * T, S.Z + R.Z * T };
                const FVec3 D   = { Got.X - At.X, Got.Y - At.Y, Got.Z - At.Z };
                // The texel spans this much at the point's distance from the centre: an error within two of them is the
                // probe's own grain.
                const float Grain = 2.0f * (2.0f / (float)Cube) * sqrtf(Dot(At, At));
                Within[k] += (sqrtf(Dot(D, D)) <= 2.0f * Grain + 1.0f) ? 1 : 0;
            }
        }
        Check(Within[2] >= Rays * 9 / 10 && Within[2] >= Within[0],
              "probe: a ray marched through a box room's probe leaves where it really does, nine in ten within the "
              "probe's grain");

        // A pillar in the room, off the centre: the centre does not see behind it, and rays that meet its back or
        // what stands behind it as the centre sees it are not to be had - most others are. And the same probe with
        // three texels in ten unknown, scattered: a ray goes through those, and may find what lies further.
        const FVec3 PLo = { 80.0f, 40.0f, -100.0f }, PHi = { 140.0f, 100.0f, 120.0f };
        auto Meets = [&](const FVec3& S, const FVec3& R)
        {
            // The room's walls from inside, the pillar from outside: the nearest.
            float T = Leaves(S, R);
            float T0 = 0.0f, T1 = 1e30f;
            const float Sv[3] = { S.X, S.Y, S.Z }, Rv[3] = { R.X, R.Y, R.Z };
            const float Lv[3] = { PLo.X, PLo.Y, PLo.Z }, Hv[3] = { PHi.X, PHi.Y, PHi.Z };
            bool bHit = true;
            for (int a = 0; a < 3 && bHit; a++)
            {
                if (Rv[a] == 0.0f)
                {
                    bHit = Sv[a] >= Lv[a] && Sv[a] <= Hv[a];
                    continue;
                }
                float Ta = (Lv[a] - Sv[a]) / Rv[a], Tb = (Hv[a] - Sv[a]) / Rv[a];
                if (Ta > Tb) { const float K = Ta; Ta = Tb; Tb = K; }
                T0 = (Ta > T0) ? Ta : T0;
                T1 = (Tb < T1) ? Tb : T1;
                bHit = T0 <= T1;
            }
            if (bHit && T0 > 0.0f && T0 < T)
                T = T0;
            return T;
        };
        auto Inside = [&](const FVec3& Q)
        {
            return Q.X >= PLo.X && Q.X <= PHi.X && Q.Y >= PLo.Y && Q.Y <= PHi.Y && Q.Z >= PLo.Z && Q.Z <= PHi.Z;
        };
        // Three probes: whole; three texels in ten unknown, scattered; and one wide patch unknown, a part of the room
        // never seen. Three ways to read them: a texel that knows nothing stops no ray; the coarser level stands in for
        // it; or up to three levels up, the first that knows something.
        static FProbeTexel Coarse[3][6][32][32];
        auto LevelOf = [&](int k, int f, int x, int y) -> const FProbeTexel*
        {
            return k ? &Coarse[k - 1][f][y][x] : &Probe[f][y][x];
        };
        // The wide patch: once where a wall faces the centre, once round a corner of the room.
        const FVec3 PatchWays[2] = { { -0.97f, -0.2f, 0.1f }, { -0.71f, -0.53f, 0.43f } };
        FVec3       HoleWay      = PatchWays[0];
        const float HoleCos = cosf(15.0f * 3.14159265f / 180.0f);
        const int   UpOf[3] = { 0, 1, 3 };
        const int   StepsOf2[2] = { 8, 16 };
        for (int Holes = 0; Holes <= 3; Holes++)
        {
            {
                const FVec3 W = PatchWays[(Holes == 3) ? 1 : 0];
                const float L = sqrtf(Dot(W, W));
                HoleWay = FVec3{ W.X / L, W.Y / L, W.Z / L };
            }
            for (int f = 0; f < 6; f++)
                for (int y = 0; y < Cube; y++)
                    for (int x = 0; x < Cube; x++)
                    {
                        float U, V;
                        ProbeTexelMiddle(x, y, Cube, U, V);
                        FVec3 D = ProbeDirOf(f, U, V);
                        const float L = sqrtf(Dot(D, D));
                        D = FVec3{ D.X / L, D.Y / L, D.Z / L };
                        Probe[f][y][x] = { Meets(Centre, D), { (float)f, 0.0f, 0.0f }, 1.0f, 1 };
                        if ((Holes == 1 && Rand() < 0.3f) || (Holes >= 2 && Dot(D, HoleWay) > HoleCos))
                            Probe[f][y][x].Trust = 0.0f;
                    }
            for (int k = 1; k <= 3; k++)
                for (int f = 0; f < 6; f++)
                    for (int y = 0; y < (Cube >> k); y++)
                        for (int x = 0; x < (Cube >> k); x++)
                        {
                            const FProbeTexel Four[4] = { *LevelOf(k - 1, f, 2 * x, 2 * y), *LevelOf(k - 1, f, 2 * x + 1, 2 * y),
                                                          *LevelOf(k - 1, f, 2 * x, 2 * y + 1),
                                                          *LevelOf(k - 1, f, 2 * x + 1, 2 * y + 1) };
                            Coarse[k - 1][f][y][x] = ProbeMipOf(Four);
                        }
            int Tried = 0, TriedIn = 0;
            int Found[3][2] = {}, Fine[3][2] = {}, Near[3][2] = {}, FineIn[3][2] = {}, NearIn[3][2] = {}, FoundIn[3][2] = {};
            for (int r = 0; r < Rays; r++)
            {
                const FVec3 S = { Lo.X + (Hi.X - Lo.X) * (0.2f + 0.6f * Rand()), Lo.Y + (Hi.Y - Lo.Y) * (0.2f + 0.6f * Rand()),
                                  Lo.Z + (Hi.Z - Lo.Z) * (0.2f + 0.6f * Rand()) };
                if (Inside(S))
                    continue;
                FVec3 R = { Rand() * 2.0f - 1.0f, Rand() * 2.0f - 1.0f, Rand() * 2.0f - 1.0f };
                if (Holes >= 2)
                {
                    // Aimed at what the centre sees inside the patch.
                    FVec3 D;
                    do
                    {
                        D = FVec3{ HoleWay.X + (Rand() - 0.5f) * 0.6f, HoleWay.Y + (Rand() - 0.5f) * 0.6f,
                                   HoleWay.Z + (Rand() - 0.5f) * 0.6f };
                        const float LD = sqrtf(Dot(D, D));
                        D = FVec3{ D.X / LD, D.Y / LD, D.Z / LD };
                    } while (Dot(D, HoleWay) <= HoleCos);
                    const float TW = Meets(Centre, D);
                    R = FVec3{ D.X * TW - S.X, D.Y * TW - S.Y, D.Z * TW - S.Z };
                }
                const float L = sqrtf(Dot(R, R));
                if (L < 0.1f)
                    continue;
                R = FVec3{ R.X / L, R.Y / L, R.Z / L };
                Tried++;
                const float Truth = Meets(S, R);
                const FVec3 At    = { S.X + R.X * Truth, S.Y + R.Y * Truth, S.Z + R.Z * Truth };
                const float AtLen = sqrtf(Dot(At, At));
                const bool  bIn   = Dot(At, HoleWay) > HoleCos * AtLen;
                TriedIn += bIn ? 1 : 0;
                const float Grain = 2.0f * (2.0f / (float)Cube) * AtLen;
                for (int m = 0; m < 3; m++)
                {
                    const int Up = UpOf[m];
                    auto Read = [&](int f, int x, int y) -> const FProbeTexel*
                    {
                        for (int k = 0; k <= Up; k++)
                        {
                            const FProbeTexel* T = LevelOf(k, f, x >> k, y >> k);
                            if (T->Trust > 0.0f)
                                return T;
                        }
                        return &Probe[f][y][x];
                    };
                    for (int k = 0; k < 2; k++)
                    {
                        float T = 0.0f;
                        if (!ProbeMarch(Centre, Cube, S, R, 900.0f, StepsOf2[k], 8, Read, T))
                            continue;
                        const FVec3 Got = { S.X + R.X * T, S.Y + R.Y * T, S.Z + R.Z * T };
                        const FVec3 D   = { Got.X - At.X, Got.Y - At.Y, Got.Z - At.Z };
                        const float E   = sqrtf(Dot(D, D));
                        Found[m][k]++;
                        Fine[m][k] += (E <= 2.0f * Grain + 1.0f) ? 1 : 0;
                        Near[m][k] += (E <= 8.0f * Grain + 1.0f) ? 1 : 0;
                        if (bIn)
                        {
                            FoundIn[m][k]++;
                            FineIn[m][k] += (E <= 2.0f * Grain + 1.0f) ? 1 : 0;
                            NearIn[m][k] += (E <= 8.0f * Grain + 1.0f) ? 1 : 0;
                        }
                    }
                }
            }
            if (Holes == 0)
                Check(Found[0][0] == Tried && Fine[0][0] >= Tried * 8 / 10 && Fine[1][0] == Fine[0][0] &&
                          Fine[2][0] == Fine[0][0] && Fine[2][1] == Fine[0][1],
                      "probe: with a pillar the centre cannot see behind, four rays in five still leave where they do; "
                      "where every texel is known, the coarser levels change nothing");
            else if (Holes == 1)
                Check(Fine[0][0] >= Tried * 45 / 100 && Fine[0][1] >= Fine[0][0] && Fine[1][0] >= Tried * 78 / 100 &&
                          Fine[1][1] >= Tried * 78 / 100 && Fine[2][0] >= Fine[1][0] * 99 / 100,
                      "probe: with scattered unknown texels nearly half the rays still leave where they do; with the "
                      "coarser level standing in, as many as with none unknown");
            else
                Check(NearIn[2][0] >= TriedIn * ((Holes == 2) ? 75 : 65) / 100 && NearIn[2][0] > NearIn[1][0] &&
                          NearIn[1][0] >= NearIn[0][0] && FineIn[2][0] >= TriedIn * ((Holes == 2) ? 60 : 40) / 100,
                      "probe: into a wide unknown patch, three levels up stand in well enough - most rays end near "
                      "where they do, against a fifth with none");
        }
    }
}

static void TestTrees()
{
    using namespace RdxTree;
    static FGrid Grid;
    for (int k = 0; k < Kind_Count; k++)
    {
        const bool  bBush  = IsBush((EKind)k);
        const float Height = bBush ? 80.0f : 360.0f, Radius = bBush ? 40.0f : 120.0f;
        const FShape S = ShapeOf((EKind)k, Height, Radius);
        FPlant Plant, Again;
        FMesh  Mesh;
        const bool bGrown = Grow(S, 1234u, Plant, Grid) && Build(Plant, Mesh, 64.0f);
        Check(bGrown, "trees: every kind grows and builds");
        if (!bGrown)
            continue;
        // A plant grown stem by stem (StemPlant) has a few stems and some tens of leaves, a crown's hundreds of
        // branches and thousands.
        const bool bStem = IsStemPlant((EKind)k);
        Check((bStem ? Plant.Nodes.Num > 10 && Plant.Leaves.Num > 15 : Plant.Nodes.Num > 100 && Plant.Leaves.Num > 1000) &&
                  Mesh.BarkIndex.Num > 0 && Mesh.Leaf.Num == Plant.Leaves.Num,
              "trees: a plant has branches and leaves, every leaf an instance of the blade");
        bool bIndices = (Mesh.BarkIndex.Num % 3) == 0;
        for (int i = 0; i < Mesh.BarkIndex.Num; i++)
            bIndices = bIndices && (int)Mesh.BarkIndex[i] < Mesh.Bark.Num;
        Check(bIndices, "trees: whole triangles, every index a vertex of its own list");
        {
            // Its bark far off (Build's bPlain and Least, a far plant's at the trees' lower levels): whole triangles of
            // its own vertices and no leaves, nowhere out of the whole bark's room; only the wood from two twigs'
            // radius on a tree's a fifth of the whole's faces at the most - the thinner wood is most of the bark - and
            // from four no more than from two. EZ-Tree's way the same from the same seed: the same plant, its far bark
            // no more than its whole.
            float Lo[3] = { 1e9f, 1e9f, 1e9f }, Hi[3] = { -1e9f, -1e9f, -1e9f };
            for (int i = 0; i < Mesh.Bark.Num; i++)
            {
                const float P[3] = { Mesh.Bark[i].X, Mesh.Bark[i].Y, Mesh.Bark[i].Z };
                for (int c = 0; c < 3; c++)
                {
                    Lo[c] = (P[c] < Lo[c]) ? P[c] : Lo[c];
                    Hi[c] = (P[c] > Hi[c]) ? P[c] : Hi[c];
                }
            }
            FMesh Far[2];
            bool  bFar = true;
            for (int f = 0; f < 2; f++)
            {
                bFar = bFar && Build(Plant, Far[f], 64.0f, true, S.Twig * (f ? 4.0f : 2.0f)) &&
                       (Far[f].BarkIndex.Num % 3) == 0 && Far[f].Leaf.Num == 0;
                for (int i = 0; bFar && i < Far[f].BarkIndex.Num; i++)
                    bFar = (int)Far[f].BarkIndex[i] < Far[f].Bark.Num;
                for (int i = 0; bFar && i < Far[f].Bark.Num; i++)
                {
                    const float P[3] = { Far[f].Bark[i].X, Far[f].Bark[i].Y, Far[f].Bark[i].Z };
                    for (int c = 0; c < 3; c++)
                        bFar = bFar && P[c] >= Lo[c] - 1.0f && P[c] <= Hi[c] + 1.0f;
                }
            }
            Check(bFar && Far[1].BarkIndex.Num <= Far[0].BarkIndex.Num &&
                      (bBush ? Far[0].BarkIndex.Num <= Mesh.BarkIndex.Num
                             : Far[0].BarkIndex.Num > 0 && 5 * Far[0].BarkIndex.Num <= Mesh.BarkIndex.Num),
                  "trees: the far bark - whole triangles, no leaves, within the whole bark's room; from two twigs' "
                  "radius on a tree's a fifth of the whole at the most, from four no more");
            if (EzGrows((EKind)k))
            {
                FPlant EzPlant, EzAgain;
                FMesh  EzMesh, EzFar;
                const bool bEz = GrowEz(S, 1234u, EzPlant, EzMesh, 64.0f) &&
                                 GrowEz(S, 1234u, EzAgain, EzFar, 64.0f, S.Twig * 2.0f);
                Check(bEz && EzAgain.Nodes.Num == EzPlant.Nodes.Num && EzFar.Leaf.Num == 0 && EzFar.BarkIndex.Num > 0 &&
                          EzFar.BarkIndex.Num <= EzMesh.BarkIndex.Num,
                      "trees: EZ-Tree's far bark - the same plant, no leaves, no more than its whole bark");
            }
        }
        float Low = 1e9f, High = -1e9f, Wide = 0.0f;
        for (int i = 0; i < Mesh.Leaf.Num; i++)
        {
            const FLeafInstance& V = Mesh.Leaf[i];
            Low  = (V.Z < Low) ? V.Z : Low;
            High = (V.Z > High) ? V.Z : High;
            const float D = sqrtf(V.X * V.X + V.Y * V.Y);
            Wide = (D > Wide) ? D : Wide;
        }
        // A pink's or a lily's stems stand narrower than the room: it is no crown.
        Check(High < Height * 1.15f && High > Height * (bStem ? 0.5f : 0.7f) && Wide < Radius * 1.6f &&
                  (bStem || Wide > Radius * 0.5f),
              "trees: the leaves fill about the room the plant was given, no further");
        // A card's corner reaches half its size below its middle, which stands within a step of its twig.
        Check(Low > S.CrownBase * Height * 0.85f - S.LeafSize - S.Step, "trees: no leaves below the crown");
        float Foot = 0.0f;
        for (int i = 0; i < Plant.Nodes.Num; i++)
            if (Plant.Nodes[i].Parent < 0 && Plant.Nodes[i].Radius > Foot)
                Foot = Plant.Nodes[i].Radius;
        // A stem plant's stems as thin as a flower's, but every node of theirs drawn - a stem with no child as thick as
        // Build draws one - so a stem reaches its flower.
        bool bDrawn = true;
        for (int i = 0; bStem && i < Plant.Nodes.Num; i++)
            bDrawn = bDrawn && Plant.Nodes[i].Radius >= S.Twig * 1.25f;
        Check(bStem ? (Foot >= S.Twig * 1.25f && bDrawn) : Foot > S.Twig * 3.0f,
              "trees: the foot of the stems thicker than a twig");
        // A lily's and a pink's flowers: their petals first among the leaves (PetalsOf), a lily's six, a pink's five, a
        // stem's each.
        if (k == Kind_Lily || k == Kind_Pink)
        {
            int Stems = 0;
            for (int i = 0; i < Plant.Nodes.Num; i++)
                Stems += (Plant.Nodes[i].Parent < 0) ? 1 : 0;
            Check(PetalsOf(Mesh) == Stems * ((k == Kind_Lily) ? 6 : 5), "trees: a lily's and a pink's flowers, one a stem");
        }
        Check(Grow(S, 1234u, Again, Grid) && Again.Nodes.Num == Plant.Nodes.Num && Again.Leaves.Num == Plant.Leaves.Num &&
              Again.Nodes[Again.Nodes.Num - 1].At.X == Plant.Nodes[Plant.Nodes.Num - 1].At.X,
              "trees: the same seed, the same plant");
    }
    {
        FPlant Tree;
        float  Trunk[24][4];
        const int N = Grow(ShapeOf(Kind_Tree2, 360.0f, 120.0f), 77u, Tree, Grid) ? TrunkOf(Tree, Trunk, 24) : 0;
        bool bUp = N >= 4;
        for (int k = 1; k < N; k++)
            bUp = bUp && Trunk[k][2] > Trunk[k - 1][2];
        Check(bUp && Trunk[0][3] >= Trunk[N - 1][3], "trees: the trunk goes up from the foot, no thicker as it goes");
        // The game's model's trunk twice as thick at the foot as the grown one's: the foot as thick, the limbs thicker
        // with it, the twigs as fine.
        FShape Wide = ShapeOf(Kind_Tree2, 360.0f, 120.0f);
        Wide.Bark[0] = Trunk[0][3] * 2.0f;
        FPlant Thick;
        float  Foot2[24][4];
        const int M = Grow(Wide, 77u, Thick, Grid) ? TrunkOf(Thick, Foot2, 24) : 0;
        float Twig = 1e9f, ThinBefore = 1e9f;
        for (int i = 0; i < Thick.Nodes.Num; i++)
            Twig = (Thick.Nodes[i].Radius < Twig) ? Thick.Nodes[i].Radius : Twig;
        for (int i = 0; i < Tree.Nodes.Num; i++)
            ThinBefore = (Tree.Nodes[i].Radius < ThinBefore) ? Tree.Nodes[i].Radius : ThinBefore;
        Check(M >= 4 && fabsf(Foot2[0][3] - Wide.Bark[0]) < Wide.Bark[0] * 0.02f && Thick.Shape.Pipe < Wide.Pipe &&
                  fabsf(Twig - ThinBefore) < ThinBefore * 0.01f,
              "trees: a game model's thicker trunk: the foot as thick, the twigs as fine");
    }
    {
        // The crown shades itself (FOpenness): its leaves are not all as open; one out on the crown's +x side is more
        // open towards +x than towards -x, one on the -x side the other way round; and at the trunk's foot, under the
        // crown, the way out leans downwards.
        FPlant Crown;
        FMesh  CrownMesh;
        const bool bMade = Grow(ShapeOf(Kind_Tree2, 360.0f, 120.0f), 77u, Crown, Grid) && Build(Crown, CrownMesh, 64.0f);
        int    Least = 255, Most = 0, EastN = 0, WestN = 0;
        double East = 0.0, West = 0.0;
        for (int i = 0; bMade && i < CrownMesh.Leaf.Num; i++)
        {
            const FLeafInstance& L = CrownMesh.Leaf[i];
            Least = (L.Open[0] < Least) ? L.Open[0] : Least;
            Most  = (L.Open[0] > Most) ? L.Open[0] : Most;
            const float Lean = OpenTowards(L.Open, Vec(1.0f, 0.0f, 0.0f)) - OpenTowards(L.Open, Vec(-1.0f, 0.0f, 0.0f));
            if (L.X > 60.0f)
            {
                East += Lean;
                EastN++;
            }
            if (L.X < -60.0f)
            {
                West += Lean;
                WestN++;
            }
        }
        Check(bMade && Most - Least > 60 && EastN > 50 && WestN > 50 && East / EastN > 0.2 && West / WestN < -0.2,
              "trees: the crown shades itself - a leaf is open towards the side it is on, the inner ones least");
        const FVertex& Foot = CrownMesh.Bark[0];
        Check(bMade && Foot.Z < 20.0f && Foot.Open[3] < 120,
              "trees: under the crown the trunk's way out leans downwards");
    }
    {
        // A fork's other child starts on its parent's bark: an upright trunk of thin wood going on past the fork, and a
        // branch off it a third of the way up (its last node) - none of the branch's points inside the trunk's girth by
        // more than its crevices' depth, at the trunk's thinner there (TreeGen.h's Build, the seam).
        FPlant Fork, Bare;
        Fork.Shape = Bare.Shape = ShapeOf(Kind_Tree2, 360.0f, 120.0f);
        const FVec  At[5]   = { Vec(0.0f, 0.0f, 0.0f), Vec(0.0f, 0.0f, 10.0f), Vec(0.0f, 0.0f, 20.0f),
                                Vec(0.0f, 0.0f, 30.0f), Vec(10.0f, 0.0f, 16.0f) };
        const int   Up[5]   = { -1, 0, 1, 2, 1 };
        const int   Kids1[5] = { 1, 2, 1, 0, 0 };
        const float Rad[5]  = { 4.0f, 4.0f, 3.5f, 3.0f, 2.0f };
        for (int k = 0; k < 5; k++)
        {
            FNode N    = {};
            N.At       = At[k];
            N.Parent   = Up[k];
            N.Children = Kids1[k];
            N.Radius   = Rad[k];
            N.Next     = -1;
            Fork.Nodes.Add(N);
            if (k < 4)
            {
                N.Children = (k == 1) ? 1 : Kids1[k];
                Bare.Nodes.Add(N);
            }
        }
        FMesh      ForkMesh, BareMesh;
        const bool bBuilt = Build(Fork, ForkMesh, 64.0f) && Build(Bare, BareMesh, 64.0f) &&
                            ForkMesh.Bark.Num > BareMesh.Bark.Num;
        float Nearest = 1e9f;
        for (int v = bBuilt ? BareMesh.Bark.Num : 0; bBuilt && v < ForkMesh.Bark.Num; v++)
        {
            const FVertex& V = ForkMesh.Bark[v];
            const float    D = sqrtf(V.X * V.X + V.Y * V.Y);
            Nearest = (D < Nearest) ? D : Nearest;
        }
        Check(bBuilt && Nearest > 3.5f * (1.0f - ReliefDepth) - 0.05f,
              "trees: a branch starts on its parent's bark, none of it inside");
        // A thick trunk going on past a fork and a limb leaving it shallowly: its first two steps inside the trunk's
        // wood, not drawn; the third comes out, seamed onto the bark - the trunk's tube on from the fork, a little within
        // its bark (Build's Margin, a fiftieth of the fork's girth).
        FPlant Thick, Trunk;
        Thick.Shape = Trunk.Shape = ShapeOf(Kind_Tree2, 360.0f, 120.0f);
        const FVec  At2[7]  = { Vec(0.0f, 0.0f, 0.0f), Vec(0.0f, 0.0f, 10.0f), Vec(0.0f, 0.0f, 34.0f),
                                Vec(0.0f, 0.0f, 50.0f), Vec(3.0f, 0.0f, 15.0f), Vec(6.0f, 0.0f, 20.0f),
                                Vec(14.0f, 0.0f, 30.0f) };
        const int   Up2[7]  = { -1, 0, 1, 2, 1, 4, 5 };
        const int   Kids[7] = { 1, 2, 1, 0, 1, 1, 0 };
        const float Rad2[7] = { 10.0f, 10.0f, 9.0f, 8.0f, 2.0f, 2.0f, 1.8f };
        for (int k = 0; k < 7; k++)
        {
            FNode N    = {};
            N.At       = At2[k];
            N.Parent   = Up2[k];
            N.Children = Kids[k];
            N.Radius   = Rad2[k];
            N.Next     = -1;
            Thick.Nodes.Add(N);
            if (k < 4)
            {
                N.Children = (k == 1) ? 1 : Kids[k];
                Trunk.Nodes.Add(N);
            }
        }
        FMesh      ThickMesh, TrunkMesh;
        const bool bThick = Build(Thick, ThickMesh, 64.0f) && Build(Trunk, TrunkMesh, 64.0f) &&
                            ThickMesh.Bark.Num > TrunkMesh.Bark.Num;
        float Closest = 1e9f, Highest = -1e9f;
        for (int v = bThick ? TrunkMesh.Bark.Num : 0; bThick && v < ThickMesh.Bark.Num; v++)
        {
            const FVertex& V = ThickMesh.Bark[v];
            const float    D = sqrtf(V.X * V.X + V.Y * V.Y);
            Closest = (D < Closest) ? D : Closest;
            Highest = (V.Z > Highest) ? V.Z : Highest;
        }
        Check(bThick && Closest > 9.0f * (1.0f - ReliefDepth) - 0.02f * 10.0f - 0.05f && Highest > 29.0f,
              "trees: a limb's steps inside a thick trunk are not drawn; it comes out on the trunk's bark");
        // Where the trunk ends past the fork - its last tube narrowing to its tip - the branch's first ring lies within
        // that narrowing wood, not on the line of a trunk going on as thick: there it would stand out of the bark, open.
        FPlant Ends, EndsBare;
        Ends.Shape = EndsBare.Shape = ShapeOf(Kind_Tree2, 360.0f, 120.0f);
        const FVec  At3[4]  = { Vec(0.0f, 0.0f, 0.0f), Vec(0.0f, 0.0f, 10.0f), Vec(0.0f, 0.0f, 20.0f),
                                Vec(10.0f, 0.0f, 16.0f) };
        const int   Up3[4]  = { -1, 0, 1, 1 };
        const float Rad3[4] = { 4.0f, 4.0f, 3.5f, 2.0f };
        for (int k = 0; k < 4; k++)
        {
            FNode N    = {};
            N.At       = At3[k];
            N.Parent   = Up3[k];
            N.Children = (k == 0) ? 1 : ((k == 1) ? 2 : 0);
            N.Radius   = Rad3[k];
            N.Next     = -1;
            Ends.Nodes.Add(N);
            if (k < 3)
            {
                N.Children = (k < 2) ? 1 : 0;
                EndsBare.Nodes.Add(N);
            }
        }
        FMesh      EndsMesh, EndsBareMesh;
        const bool bEnds = Build(Ends, EndsMesh, 64.0f) && Build(EndsBare, EndsBareMesh, 64.0f) &&
                           EndsMesh.Bark.Num > EndsBareMesh.Bark.Num;
        // The trunk's corners' girth at its top ring, and its faces' middles a tenth-sided tube's cosine within; the
        // branch's first ring its first corners, as many as a thin run of its start's girth has sides.
        float Top = 0.0f;
        for (int v = 0; bEnds && v < EndsBareMesh.Bark.Num; v++)
        {
            const FVertex& V = EndsBareMesh.Bark[v];
            if (fabsf(V.Z - 20.0f) < 0.01f)
                Top = fmaxf(Top, sqrtf(V.X * V.X + V.Y * V.Y));
        }
        const int Corners = (int)(RunStart(2.0f, 4.0f, false) * 2.0f) + 5 + 1;
        float     Stands  = -1e9f;
        for (int v = 0; bEnds && v < Corners; v++)
        {
            const FVertex& V    = EndsMesh.Bark[EndsBareMesh.Bark.Num + v];
            const float    T    = (V.Z - 10.0f) / 10.0f;
            const float    Wood = cosf(3.14159265f / 10.0f) * ((T <= 0.0f) ? 4.0f : 4.0f + (Top - 4.0f) * T);
            Stands              = fmaxf(Stands, sqrtf(V.X * V.X + V.Y * V.Y) - Wood);
        }
        Check(bEnds && Top < 3.0f && Stands < 0.01f,
              "trees: a branch off a trunk that ends past the fork starts within the trunk's narrowing wood");
    }
    {
        // A carved trunk thinning past carving into a thin top that ends: the top a run of its own, joined onto the
        // trunk's end ring and narrowing to a point - the bark closed all over but for its foot. Corners at one place
        // are one (the joint's rings are the same corners laid twice); an edge only one triangle has is a hole.
        FPlant Tall;
        Tall.Shape = ShapeOf(Kind_Tree2, 360.0f, 120.0f);
        const FVec  At3[3]  = { Vec(0.0f, 0.0f, 0.0f), Vec(0.0f, 0.0f, 20.0f), Vec(2.0f, 0.0f, 40.0f) };
        const float Rad3[3] = { 8.0f, 7.0f, 1.5f };
        for (int k = 0; k < 3; k++)
        {
            FNode N    = {};
            N.At       = At3[k];
            N.Parent   = k - 1;
            N.Children = (k < 2) ? 1 : 0;
            N.Radius   = Rad3[k];
            N.Next     = -1;
            Tall.Nodes.Add(N);
        }
        FMesh      TallMesh;
        const bool bTall = Build(Tall, TallMesh, 64.0f);
        // The corners by place, a hundredth of a unit apart at most.
        TList<int> One;
        for (int v = 0; bTall && v < TallMesh.Bark.Num; v++)
        {
            int Same = v;
            for (int w = 0; w < v; w++)
            {
                const FVertex& A = TallMesh.Bark[v];
                const FVertex& B = TallMesh.Bark[w];
                if (fabsf(A.X - B.X) < 0.01f && fabsf(A.Y - B.Y) < 0.01f && fabsf(A.Z - B.Z) < 0.01f)
                {
                    Same = One[w];
                    break;
                }
            }
            One.Add(Same);
        }
        // Each edge's triangles: an edge as its two ends, the lower first; those of the foot (below a unit) and those
        // of no length (the point's) left out.
        TList<long long> Edges;
        for (int t = 0; bTall && t + 2 < TallMesh.BarkIndex.Num; t += 3)
            for (int e = 0; e < 3; e++)
            {
                const unsigned int Ia = TallMesh.BarkIndex[t + e], Ib = TallMesh.BarkIndex[t + (e + 1) % 3];
                const int A = One[(int)Ia], B = One[(int)Ib];
                if (A == B)
                    continue;
                const FVertex& Va = TallMesh.Bark[Ia];
                const FVertex& Vb = TallMesh.Bark[Ib];
                if (Va.Z < 1.0f && Vb.Z < 1.0f)
                    continue;
                Edges.Add((A < B) ? ((long long)A << 32 | (unsigned int)B) : ((long long)B << 32 | (unsigned int)A));
            }
        int Open = 0;
        for (int a = 0; a < Edges.Num; a++)
        {
            int Seen = 0;
            for (int b = 0; b < Edges.Num; b++)
                Seen += (Edges[b] == Edges[a]) ? 1 : 0;
            Open += (Seen == 1) ? 1 : 0;
        }
        Check(bTall && Edges.Num > 0 && Open == 0,
              "trees: a trunk thinning into a top of its own is closed - the joint and the tip - but for its foot");
    }
    {
        // A thick run turning a right angle at a node bends along a curve (Build's Bend): its outer side rounded, well
        // inside the corner a mitred joint would stand out to, and the bark closed all over - the foot's dome, the
        // bend, the tip.
        FPlant Knee;
        Knee.Shape = ShapeOf(Kind_Tree2, 360.0f, 120.0f);
        const FVec  At4[4]  = { Vec(0.0f, 0.0f, 0.0f), Vec(0.0f, 0.0f, 12.0f), Vec(12.0f, 0.0f, 12.0f),
                                Vec(24.0f, 0.0f, 12.0f) };
        const float Rad4[4] = { 3.0f, 3.0f, 2.8f, 2.6f };
        for (int k = 0; k < 4; k++)
        {
            FNode N    = {};
            N.At       = At4[k];
            N.Parent   = k - 1;
            N.Children = (k < 3) ? 1 : 0;
            N.Radius   = Rad4[k];
            N.Next     = -1;
            Knee.Nodes.Add(N);
        }
        FMesh      KneeMesh;
        const bool bKnee = Build(Knee, KneeMesh, 64.0f);
        // How far the bark reaches out of the turn's outer side: a mitred joint's outer corner at (-3, 0, 15).
        float Outer = -1e9f;
        for (int v = 0; bKnee && v < KneeMesh.Bark.Num; v++)
            Outer = fmaxf(Outer, KneeMesh.Bark[v].Z - KneeMesh.Bark[v].X);
        TList<int> Same;
        for (int v = 0; bKnee && v < KneeMesh.Bark.Num; v++)
        {
            int One = v;
            for (int w = 0; w < v; w++)
            {
                const FVertex& A = KneeMesh.Bark[v];
                const FVertex& B = KneeMesh.Bark[w];
                if (fabsf(A.X - B.X) < 0.01f && fabsf(A.Y - B.Y) < 0.01f && fabsf(A.Z - B.Z) < 0.01f)
                {
                    One = Same[w];
                    break;
                }
            }
            Same.Add(One);
        }
        TList<long long> KneeEdges;
        for (int t = 0; bKnee && t + 2 < KneeMesh.BarkIndex.Num; t += 3)
            for (int e = 0; e < 3; e++)
            {
                const int A = Same[(int)KneeMesh.BarkIndex[t + e]], B = Same[(int)KneeMesh.BarkIndex[t + (e + 1) % 3]];
                if (A != B)
                    KneeEdges.Add((A < B) ? ((long long)A << 32 | (unsigned int)B) : ((long long)B << 32 | (unsigned int)A));
            }
        int KneeOpen = 0;
        for (int a = 0; a < KneeEdges.Num; a++)
        {
            int Seen = 0;
            for (int b = 0; b < KneeEdges.Num; b++)
                Seen += (KneeEdges[b] == KneeEdges[a]) ? 1 : 0;
            KneeOpen += (Seen == 1) ? 1 : 0;
        }
        Check(bKnee && Outer > 12.0f && Outer < 16.0f && KneeEdges.Num > 0 && KneeOpen == 0,
              "trees: a thick run turning a right angle bends along a curve, closed all over");
    }
    {
        // A bush's stem leaving the ground at a slant: its foot, half out of the ground, closed by a low dome back
        // along its axis (Build's root tube) - no edge of the bark open, the dome's top behind the first ring.
        FPlant Stem;
        Stem.Shape = ShapeOf(Kind_Bush1, 88.0f, 53.0f);
        const FVec  At6[3]  = { Vec(0.0f, 0.0f, 0.0f), Vec(16.0f, 0.0f, 4.0f), Vec(30.0f, 0.0f, 10.0f) };
        const float Rad6[3] = { 1.6f, 1.6f, 1.2f };
        for (int k = 0; k < 3; k++)
        {
            FNode N    = {};
            N.At       = At6[k];
            N.Parent   = k - 1;
            N.Children = (k < 2) ? 1 : 0;
            N.Radius   = Rad6[k];
            N.Next     = -1;
            Stem.Nodes.Add(N);
        }
        FMesh      StemMesh;
        const bool bStem = Build(Stem, StemMesh, 64.0f);
        // The foot's dome top: the point furthest back along the stem's way.
        const FVec Way  = Unit(At6[1] - At6[0], Vec(1.0f, 0.0f, 0.0f));
        float      Back = 1e9f;
        for (int v = 0; bStem && v < StemMesh.Bark.Num; v++)
            Back = fminf(Back, Dot(Vec(StemMesh.Bark[v].X, StemMesh.Bark[v].Y, StemMesh.Bark[v].Z), Way));
        TList<int> Same;
        for (int v = 0; bStem && v < StemMesh.Bark.Num; v++)
        {
            int One = v;
            for (int w = 0; w < v; w++)
            {
                const FVertex& A = StemMesh.Bark[v];
                const FVertex& B = StemMesh.Bark[w];
                if (fabsf(A.X - B.X) < 0.01f && fabsf(A.Y - B.Y) < 0.01f && fabsf(A.Z - B.Z) < 0.01f)
                {
                    One = Same[w];
                    break;
                }
            }
            Same.Add(One);
        }
        TList<long long> StemEdges;
        for (int t = 0; bStem && t + 2 < StemMesh.BarkIndex.Num; t += 3)
            for (int e = 0; e < 3; e++)
            {
                const int A = Same[(int)StemMesh.BarkIndex[t + e]], B = Same[(int)StemMesh.BarkIndex[t + (e + 1) % 3]];
                if (A != B)
                    StemEdges.Add((A < B) ? ((long long)A << 32 | (unsigned int)B) : ((long long)B << 32 | (unsigned int)A));
            }
        int StemOpen = 0;
        for (int a = 0; a < StemEdges.Num; a++)
        {
            int Seen = 0;
            for (int b = 0; b < StemEdges.Num; b++)
                Seen += (StemEdges[b] == StemEdges[a]) ? 1 : 0;
            StemOpen += (Seen == 1) ? 1 : 0;
        }
        Check(bStem && StemEdges.Num > 0 && StemOpen == 0 && Back < -0.3f && Back > -1.6f,
              "trees: a stem leaving the ground at a slant is closed at its foot by a low dome");
    }
    {
        // A branch off a trunk that turns at the fork: the trunk bends along its curve there (Build's Bend), and the
        // branch's first ring lies within that curved wood - its faces' middles, a tenth-sided tube's cosine in.
        FPlant Turn, TurnBare;
        Turn.Shape = TurnBare.Shape = ShapeOf(Kind_Tree2, 360.0f, 120.0f);
        const FVec  At5[5]   = { Vec(0.0f, 0.0f, 0.0f), Vec(0.0f, 0.0f, 10.0f), Vec(8.66f, 0.0f, 15.0f),
                                 Vec(17.32f, 0.0f, 20.0f), Vec(-8.0f, 0.0f, 14.0f) };
        const int   Up5[5]   = { -1, 0, 1, 2, 1 };
        const int   Kids5[5] = { 1, 2, 1, 0, 0 };
        const float Rad5[5]  = { 4.0f, 4.0f, 3.5f, 3.0f, 2.0f };
        for (int k = 0; k < 5; k++)
        {
            FNode N    = {};
            N.At       = At5[k];
            N.Parent   = Up5[k];
            N.Children = Kids5[k];
            N.Radius   = Rad5[k];
            N.Next     = -1;
            Turn.Nodes.Add(N);
            if (k < 4)
            {
                N.Children = (k == 1) ? 1 : Kids5[k];
                TurnBare.Nodes.Add(N);
            }
        }
        FMesh      TurnMesh, TurnBareMesh;
        const bool bTurn = Build(Turn, TurnMesh, 64.0f) && Build(TurnBare, TurnBareMesh, 64.0f) &&
                           TurnMesh.Bark.Num > TurnBareMesh.Bark.Num;
        // The trunk's middle: up to the bend, the bend's quadratic curve (four units either way of the node - the
        // trunk's girth there), on to the next node; its girth four, narrowing to 3.5 there.
        FVec  Line[20];
        float Wide[20];
        int   Steps = 0;
        const FVec E = Vec(0.0f, 0.0f, 6.0f), P = Vec(0.0f, 0.0f, 10.0f), X = Vec(3.464f, 0.0f, 12.0f);
        Line[Steps] = Vec(0.0f, 0.0f, 0.0f);
        Wide[Steps++] = 4.0f;
        for (int u = 0; u <= 16; u++)
        {
            const float U = (float)u / 16.0f;
            Line[Steps]   = E * ((1.0f - U) * (1.0f - U)) + P * (2.0f * U * (1.0f - U)) + X * (U * U);
            Wide[Steps++] = 4.0f;
        }
        Line[Steps] = At5[2];
        Wide[Steps++] = 3.5f;
        const int Corners = (int)(RunStart(2.0f, 4.0f, false) * 2.0f) + 5 + 1;
        float     Stands  = -1e9f;
        for (int v = 0; bTurn && v < Corners; v++)
        {
            const FVertex& Vx = TurnMesh.Bark[TurnBareMesh.Bark.Num + v];
            const FVec     Q  = Vec(Vx.X, Vx.Y, Vx.Z);
            float          In = 1e9f;
            for (int k = 0; k + 1 < Steps; k++)
            {
                const FVec  Ab = Line[k + 1] - Line[k];
                float       T  = Dot(Q - Line[k], Ab) / fmaxf(Dot(Ab, Ab), 1e-6f);
                T              = (T < 0.0f) ? 0.0f : ((T > 1.0f) ? 1.0f : T);
                const float D  = Length(Q - (Line[k] + Ab * T)) -
                                cosf(3.14159265f / 10.0f) * (Wide[k] + (Wide[k + 1] - Wide[k]) * T);
                In             = fminf(In, D);
            }
            Stands = fmaxf(Stands, In);
        }
        Check(bTurn && Stands < 0.01f, "trees: a branch off a trunk that bends at the fork starts within the curved wood");
    }
    {
        // A run of three tubes, a little bent: each starts on its parent's end ring - the same places round, the same
        // place in the picture.
        FPlant Line;
        Line.Shape = ShapeOf(Kind_Tree2, 360.0f, 120.0f);
        const float Xs[4] = { 0.0f, 0.0f, 1.5f, 4.0f }, Rs[4] = { 14.0f, 13.0f, 12.0f, 11.0f };
        for (int k = 0; k < 4; k++)
        {
            FNode N    = {};
            N.At       = Vec(Xs[k], 0.0f, 12.0f * (float)k);
            N.Parent   = k - 1;
            N.Children = (k < 3) ? 1 : 0;
            N.Radius   = Rs[k];
            N.Next     = -1;
            Line.Nodes.Add(N);
        }
        FMesh      LineMesh;
        bool       bJoined = Build(Line, LineMesh, 64.0f) && LineMesh.Bark.Num > 2 && (LineMesh.Bark.Num - 2) % 3 == 0;
        // The three tubes are as long, so as many rings each; the first tube's foot closed by one more point after it,
        // its dome's bottom, and the run's end by one more, the dome's top; a ring: the vertices from a tube's first on
        // with its V.
        const int  Tube    = (LineMesh.Bark.Num - 2) / 3;
        auto       TubeAt  = [&](int t) { return t * Tube + ((t >= 1) ? 1 : 0); };
        int        Ring    = 0;
        while (bJoined && Ring < Tube && LineMesh.Bark[Ring].V == LineMesh.Bark[0].V)
            Ring++;
        bJoined = bJoined && Ring >= 4 && Tube % Ring == 0;
        for (int t = 1; t < 3 && bJoined; t++)
            for (int k = 0; k < Ring; k++)
            {
                const FVertex& End   = LineMesh.Bark[TubeAt(t - 1) + Tube - Ring + k];
                const FVertex& Start = LineMesh.Bark[TubeAt(t) + k];
                bJoined = bJoined && fabsf(End.X - Start.X) + fabsf(End.Y - Start.Y) + fabsf(End.Z - Start.Z) < 1e-3f &&
                          End.U == Start.U && End.V == Start.V;
            }
        Check(bJoined, "trees: a trunk's tubes start on their parents' end rings - one surface");
        const FVertex& Top = LineMesh.Bark[LineMesh.Bark.Num - 1];
        Check(bJoined && Top.Z > 36.0f && Top.Z < 36.0f + 11.0f, "trees: a run's end is closed by a low dome");
        const FVertex& Foot = LineMesh.Bark[bJoined ? Tube : 0];
        Check(bJoined && Foot.Z < -1.0f && Foot.Z > -14.0f && Foot.NZ < -0.9f,
              "trees: a trunk's foot is closed by a low dome below its first ring");
        // Thick wood is carved: its first ring in and out of the radius, into crevices about a seventh of it deep.
        float Near = 1e9f, Far = 0.0f;
        for (int v = 0; v < Ring; v++)
        {
            const FVertex& V = LineMesh.Bark[v];
            const float    D = sqrtf(V.X * V.X + V.Y * V.Y);
            Near = (D < Near) ? D : Near;
            Far  = (D > Far) ? D : Far;
        }
        Check(bJoined && Far <= 14.0f + 1e-3f && Far - Near > 14.0f * ReliefDepth * 0.5f &&
                  Near > 14.0f * (1.0f - ReliefDepth) - 1e-3f,
              "trees: thick wood is carved into its bark's crevices, no deeper than their depth");
    }
    static unsigned char Mark[32 * 32 * 4];
    DrawBulletMark(Mark, 32);
    const int Edge = Mark[0], Middle = Mark[4 * (16 * 32 + 16)];
    Check(Edge > 120 && Edge < 136 && Middle < 64, "trees: a bullet's mark leaves the bark at its edge, dark in the hole");
    static unsigned char Bark[64 * 128 * 4];
    DrawBark(Bark, 64, 128);
    // Round the trunk the last column meets the first as two neighbouring columns meet, on average; along it the last
    // row the first, as two neighbouring rows do.
    auto Gap = [&](int Ax, int Ay, int Bx, int By)
    {
        int D = 0;
        for (int c = 0; c < 3; c++)
            D += abs((int)Bark[4 * (Ay * 64 + Ax) + c] - (int)Bark[4 * (By * 64 + Bx) + c]);
        return D;
    };
    double SeamU = 0.0, NearU = 0.0, SeamV = 0.0, NearV = 0.0;
    for (int y = 0; y < 128; y++)
    {
        SeamU += Gap(63, y, 0, y);
        for (int x = 0; x < 63; x++)
            NearU += Gap(x, y, x + 1, y) / 63.0;
    }
    for (int x = 0; x < 64; x++)
    {
        SeamV += Gap(x, 127, x, 0);
        for (int y = 0; y < 127; y++)
            NearV += Gap(x, y, x, y + 1) / 127.0;
    }
    Check(SeamU <= NearU * 2.0 + 64.0 && SeamV <= NearV * 2.0 + 64.0,
          "trees: the bark's picture meets itself round the trunk and along it");
    static unsigned char Leaves[512 * 256 * 4];
    DrawLeaves(Leaves, 512, 256);
    int Shown = 0;
    for (int i = 0; i < 512 * 256; i++)
        Shown += (Leaves[4 * i + 3] >= 128) ? 1 : 0;
    Check(Shown > 512 * 256 / 4 && Shown < 512 * 256 * 3 / 4, "trees: each leaf covers much of its picture, not all");
    {
        // A leaf's blade: its corners where the frame says - the foot at its foot, the tip a length along it, each half
        // risen from the midrib by its fold - and every index of the frame one of its twelve corners.
        FLeafInstance I = {};
        I.Size     = 10.0f;
        I.Face[2]  = 32767;
        I.Along[0] = 32767;
        I.Fold     = 255;
        FVec  Foot, Tip, Rim, Nrm;
        float U, V;
        LeafCorner(I, 1, Foot, Nrm, U, V);
        LeafCorner(I, 10, Tip, Nrm, U, V);
        LeafCorner(I, 3, Rim, Nrm, U, V);
        bool bFrame = fabsf(Foot.X) + fabsf(Foot.Y) + fabsf(Foot.Z) < 1e-4f && fabsf(Tip.X - 10.0f) < 1e-3f &&
                      Rim.Z > 0.5f && V == 1.0f - 1.0f / 3.0f;
        for (int k = 0; k < LeafFrameIndices; k++)
            bFrame = bFrame && LeafFrame[k] < LeafCorners;
        Check(bFrame, "trees: a leaf's blade from its foot to its tip, its halves risen, its frame whole");
        // The flat blade: two triangles over the frame's four outer corners, each once or twice.
        int Seen[LeafCorners] = {};
        for (int k = 0; k < LeafFlatIndices; k++)
            Seen[LeafFrame[LeafFlat + k] % LeafCorners]++;
        Check(LeafFlat + LeafFlatIndices == (int)(sizeof(LeafFrame) / sizeof(LeafFrame[0])) && Seen[0] == 1 &&
                  Seen[2] == 2 && Seen[9] == 2 && Seen[11] == 1,
              "trees: a far leaf's flat blade spans the frame's outer corners");
    }
}

int main(int argc, char** argv)
{
    if (argc == 4 && !strcmp(argv[1], "level-rules"))
        return LevelRules(argv[2], argv[3]);

    printf("SecondRevision tests\n");
    printf("====================\n\n");

    TestBlendClassification();
    TestArchive();
    TestModulatedDraw();
    TestLightMapEligibility();
    TestDepth();
    TestTileDepth();
    TestHudTile();
    TestSphereInView();
    TestSphereInTile();
    TestShadowPotential();
    TestRelativeShadowShare();
    TestSkyBoxDepth();
    TestSurfaceCoord();
    TestLightMapCoords();
    TestPerspective();
    TestPalette();
    TestPaletteWords();
    TestGouraudLight();
    TestNeutralLight();
    TestLightMapChannels();
    TestRgba7Rows();
    TestMaskedEdges();
    TestMaskedEdgeWrap();
    TestDilateEquivalence();
    TestOutputCurve();
    TestUploadGeometry();
    TestCaption();
    TestShadowPresets();
    TestCubeFaces();
    TestShadowOccluders();
    TestShadowLightPick();
    TestShadowLightWeight();
    TestShadowCanReachView();
    TestFlashlightBeam();
    TestOwnLamps();
    TestPickEdgeShare();
    TestShaftLampPick();
    TestWorldMapRedraws();
    TestFrameShadowView();
    TestShadowCompensation();
    TestUpscaling();
    TestCompatLayers();
    TestShaderCache();
    TestFpsLimit();
    TestLevelSplit();
    TestRtWaterColours();
    TestPom();
    TestReliefPlanes();
    TestWaterSurface();
    TestClouds();
    TestFlames();
    TestMotionPrevPlaces();
    TestGpuKey();
    TestHostCamera();
    TestDofFocus();
    TestForceField();
    TestHoloFix();
    TestWaterCut();
    TestSplashView();
    TestGrass();
    TestTrees();
    TestMeshRound();
    TestLevelRound();
    TestLevelHold();
    TestEdgeBevel();
    TestBevelCut();
    TestProbe();

    printf("\n%d checks, %d failed\n", GChecks, GFailed);
    if (argc > 1 && strcmp(argv[1], "measure") == 0)
        MeasureDilate();
    return GFailed ? 1 : 0;
}

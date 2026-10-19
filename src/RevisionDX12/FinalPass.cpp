#include "Pch.h"
#include "FinalPass.h"
#include "Log.h"
// The shaders' arrays are declared with numbers from the maths header - pasted into a source string, or handed over as a
// macro (ShadowFactor.hlsl) - so that a shader and the C++ side filling its constants cannot disagree about a length.
#include "RenderMath.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"
// Made by the build out of ShadowFactor.hlsl (RevisionDX12.vcxproj): its text and the text it includes, for the run-time
// compile of the maps' factor.
#include "ShadowFactorText.h"

// The plain bilinear upscale: where no temporal upscaler runs, and for an A/B against one.
static const char* GUpscaleLinearShader =
"Texture2D Scene : register(t0);\n"
"SamplerState Lin : register(s0);\n"
"cbuffer Output : register(b0) { float InvW; float InvH; };\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    return float4(Scene.SampleLevel(Lin, Pos.xy * float2(InvW, InvH), 0.0f).rgb, 1.0f);\n"
"}\n";

// FXAA (AntiAliasing=FXAA2X/4X/8X/16X) - a post-process filter on the FINISHED picture: it has no geometry and no
// depth, only colours, so it finds edges by contrast and blurs ACROSS them, never along them.
//
// How it works, in the order the shader does it:
//   1. Luma of the pixel and of its four diagonal neighbours. Luma, not colour: the eye judges edges by brightness,
//      and a green-to-red edge of equal brightness needs no smoothing.
//   2. If the brightest and the darkest of those five are close together, there is no edge here - the pixel is left
//      exactly as it was. Without this the whole picture would soften.
//   3. The edge's direction from the diagonal differences, then two pairs of samples across it, and a blend of the
//      one that stayed closest to the local range. Sampling ACROSS the edge is what fills in the staircase.
//
// The number in the name is Span: how many pixels the filter may reach along the edge. FXAA2X reaches two, FXAA16X
// sixteen - a long, shallow edge (the top of a letter, a distant railing) needs the long reach, and a short one does
// not. This is the classic FXAA parameter, so the name says what it does instead of an invented "quality level".
static const char* GFinalFxaaShader =
"Texture2D Scene : register(t0);\n"
"SamplerState Lin : register(s0);\n"
"cbuffer Fxaa : register(b0) { float InvW; float InvH; float Span; float Pad; };\n"
"\n"
"float Luma(float3 C) { return dot(C, float3(0.299f, 0.587f, 0.114f)); }\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    float2 Inv = float2(InvW, InvH);\n"
"    float2 Uv  = Pos.xy * Inv;\n"
"    float3 Cm  = Scene.SampleLevel(Lin, Uv, 0.0f).rgb;\n"
"    float  Lm  = Luma(Cm);\n"
"    float  Lnw = Luma(Scene.SampleLevel(Lin, Uv + float2(-0.5f, -0.5f) * Inv, 0.0f).rgb);\n"
"    float  Lne = Luma(Scene.SampleLevel(Lin, Uv + float2( 0.5f, -0.5f) * Inv, 0.0f).rgb);\n"
"    float  Lsw = Luma(Scene.SampleLevel(Lin, Uv + float2(-0.5f,  0.5f) * Inv, 0.0f).rgb);\n"
"    float  Lse = Luma(Scene.SampleLevel(Lin, Uv + float2( 0.5f,  0.5f) * Inv, 0.0f).rgb);\n"
"    float  Lmin = min(Lm, min(min(Lnw, Lne), min(Lsw, Lse)));\n"
"    float  Lmax = max(Lm, max(max(Lnw, Lne), max(Lsw, Lse)));\n"
"    // No edge here: below an eighth of the local brightness, and never below a twenty-fourth of full white.\n"
"    // Noise on a flat surface must not count as an edge, or the whole picture smears.\n"
"    if ((Lmax - Lmin) < max(0.0416f, Lmax * 0.125f))\n"
"        return float4(Cm, 1.0f);\n"
"    float2 Dir;\n"
"    Dir.x = -((Lnw + Lne) - (Lsw + Lse));\n"
"    Dir.y =  ((Lnw + Lsw) - (Lne + Lse));\n"
"    float Scale = 1.0f / (min(abs(Dir.x), abs(Dir.y)) + max((Lnw + Lne + Lsw + Lse) * 0.03125f, 0.0078f));\n"
"    Dir = clamp(Dir * Scale, -Span, Span) * Inv;\n"
"    float3 A = 0.5f * (Scene.SampleLevel(Lin, Uv + Dir * (1.0f / 3.0f - 0.5f), 0.0f).rgb +\n"
"                       Scene.SampleLevel(Lin, Uv + Dir * (2.0f / 3.0f - 0.5f), 0.0f).rgb);\n"
"    float3 B = A * 0.5f + 0.25f * (Scene.SampleLevel(Lin, Uv + Dir * -0.5f, 0.0f).rgb +\n"
"                                   Scene.SampleLevel(Lin, Uv + Dir *  0.5f, 0.0f).rgb);\n"
"    float Lb = Luma(B);\n"
"    return float4((Lb < Lmin || Lb > Lmax) ? A : B, 1.0f);\n"
"}\n";

// SMAA (AntiAliasing=SMAA2X/4X/8X/16X) - two passes, and a different idea from FXAA's.
//
// FXAA finds a contrast and blurs across it. This one finds where the edge RUNS, measures how long it is, and works
// out how much of THIS pixel the true edge covers - then blends by that much. It reconstructs a coverage instead of
// softening a contrast, so a long shallow edge comes out as a clean ramp and the texture beside it is left alone.
//
//   Pass 1 (edges): for every pixel, is there an edge on its left and on its top? Written as red and green.
//   Pass 2 (blend): walk along the edge to both of its ends (at most Span steps - the number in the name), and from
//                   the two distances work out the covered area, the way MLAA does: an edge that ends far away on
//                   both sides covers half the pixel, one that ends right here covers almost none.
//
// The official SMAA looks that area up in a 160 KB table that ships with it. We COMPUTE it instead, from the same
// geometry the table was baked from: a straight edge crossing a pixel covers a trapezium, and its area is one
// multiplication. That keeps the renderer free of a binary blob it did not build, and the result is the same shape.
// It is therefore not byte-identical with AMD's or Jimenez's SMAA, and the name in the log says so.
static const char* GFinalSmaaEdgeShader =
"Texture2D Scene : register(t0);\n"
"SamplerState Lin : register(s0);\n"
"cbuffer Smaa : register(b0) { float InvW; float InvH; float Span; float Threshold; };\n"
"\n"
"float Luma(float3 C) { return dot(C, float3(0.299f, 0.587f, 0.114f)); }\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    int2 P = int2(Pos.xy);\n"
"    float L  = Luma(Scene.Load(int3(P, 0)).rgb);\n"
"    float Ll = Luma(Scene.Load(int3(max(P - int2(1, 0), int2(0, 0)), 0)).rgb);\n"
"    float Lt = Luma(Scene.Load(int3(max(P - int2(0, 1), int2(0, 0)), 0)).rgb);\n"
"    // A difference is an edge only when it also stands out against its surroundings, or texture noise counts too.\n"
"    float2 D = float2(abs(L - Ll), abs(L - Lt));\n"
"    float2 E = step(Threshold, D);\n"
"    return float4(E, 0.0f, 1.0f);\n"
"}\n";

static const char* GFinalSmaaBlendShader =
"Texture2D Scene : register(t0);\n"
"Texture2D Edges : register(t1);\n"
"SamplerState Lin : register(s0);\n"
"cbuffer Smaa : register(b0) { float InvW; float InvH; float Span; float Threshold; };\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"// How far does the edge run in this direction? At most Span pixels are examined.\n"
"float EdgeRun(int2 P, int2 Step, int Axis, int Max)\n"
"{\n"
"    int i = 1;\n"
"    [loop] for (; i <= Max; i++)\n"
"    {\n"
"        float2 E = Edges.Load(int3(P + Step * i, 0)).rg;\n"
"        if ((Axis == 0 ? E.r : E.g) < 0.5f)\n"
"            break;\n"
"    }\n"
"    return (float)(i - 1);\n"
"}\n"
"\n"
"// How much of this pixel does the edge cover? From the distance to both of its ends, the way MLAA works it out:\n"
"// half a pixel in the middle, falling off linearly towards the ends.\n"
"float Coverage(float A, float B)\n"
"{\n"
"    float H = A + B + 1.0f;\n"
"    if (H <= 1.0f)\n"
"        return 0.0f;\n"
"    float T = (A + 0.5f) / H;          // where we stand along the edge, 0..1\n"
"    return 0.5f * saturate(1.0f - abs(2.0f * T - 1.0f));\n"
"}\n"
"\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    int2   P   = int2(Pos.xy);\n"
"    float3 C   = Scene.Load(int3(P, 0)).rgb;\n"
"    float2 E   = Edges.Load(int3(P, 0)).rg;\n"
"    int    Max = (int)Span;\n"
"    float3 Out = C;\n"
"    // A horizontal edge sits on the TOP of the pixel: it runs left to right, so we blend with the pixel above.\n"
"    if (E.g > 0.5f)\n"
"    {\n"
"        float Left  = EdgeRun(P, int2(-1, 0), 1, Max);\n"
"        float Right = EdgeRun(P, int2( 1, 0), 1, Max);\n"
"        float W     = Coverage(Left, Right);\n"
"        Out = lerp(Out, Scene.Load(int3(max(P - int2(0, 1), int2(0, 0)), 0)).rgb, W);\n"
"    }\n"
"    // A vertical edge sits on the LEFT of the pixel: it runs up and down, so we blend with the pixel to the left.\n"
"    if (E.r > 0.5f)\n"
"    {\n"
"        float Up   = EdgeRun(P, int2(0, -1), 0, Max);\n"
"        float Down = EdgeRun(P, int2(0,  1), 0, Max);\n"
"        float W    = Coverage(Up, Down);\n"
"        Out = lerp(Out, Scene.Load(int3(max(P - int2(1, 0), int2(0, 0)), 0)).rgb, W);\n"
"    }\n"
"    return float4(Out, 1.0f);\n"
"}\n";

// Motion vectors (docs/terv-latvany-roadmap.md 1.6): where the pixel was on the previous frame, in pixels. The world's
// depth gives the distance (the geometry pass stores 1/W), the current camera gives the point in the world, and the
// previous camera says where that point was on the screen a frame ago. Camera space is the engine's: X right, Y down,
// Z forward, and a camera-space vector goes to the world as AxisX * v.x + AxisY * v.y + AxisZ * v.z (the same way the
// shadow factor shader does it). Only the camera's own movement is in it: what moved on its own gets the movement of
// what is behind it, which a temporal upscaler shows as a smear on that object.
static const char* GFinalMotionShader =
"Texture2D<float> Depth : register(t0);\n"
"cbuffer Motion : register(b0)\n"
"{\n"
"    float3 AxisX;     float TanX;\n"
"    float3 AxisY;     float TanY;\n"
"    float3 AxisZ;     float SizeX;\n"
"    float3 Eye;       float SizeY;\n"
"    float3 PrevAxisX; float PrevTanX;\n"
"    float3 PrevAxisY; float PrevTanY;\n"
"    float3 PrevAxisZ; float Probe;\n"
"    float3 PrevEye;   float Over;\n"
"};\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    // The self-test writes a fixed ten pixels to the right, without reading anything: if even that does not show up in\n"
"    // the picture of the vectors, the fault is in the writing or in the view, not in the depth or the cameras.\n"
"    if (Probe > 2.5f)\n"
"    {\n"
"        // The depth itself, a thousand times over: whatever was drawn turns red, and only what has no depth stays at\n"
"        // zero. That tells a bad depth read from a camera that did not move.\n"
"        float Dp = Depth.Load(int3(int2(Pos.xy), 0));\n"
"        return float4(Dp * 1000.0f, 0.0f, 0.0f, 0.0f);\n"
"    }\n"
"    if (Probe > 1.5f)\n"
"        return float4(10.0f, 0.0f, 0.0f, 0.0f);\n"
"    int2  P = int2(Pos.xy);\n"
"    float D = Depth.Load(int3(P, 0));\n"
"    // Over: this draw goes over the vectors already there, from the see-through water's own depth (FBackend's\n"
"    // WaterSurface) in place of the world's - where there is no water, what is there stays.\n"
"    if (D <= 0.0f && Over > 0.5f)\n"
"        discard;\n"
"    if (D <= 0.0f)\n"
"        return float4(0.0f, 0.0f, 0.0f, 0.0f);            // nothing was drawn here\n"
"    float2 Size = float2(SizeX, SizeY);\n"
"    float2 Here = float2(P) + 0.5f;\n"
"    float2 Ndc  = Here / Size * 2.0f - 1.0f;\n"
"    float  W    = 1.0f / D;\n"
"    float3 Vc   = float3(Ndc.x * TanX * W, Ndc.y * TanY * W, W);\n"
"    float3 Pw   = Eye + AxisX * Vc.x + AxisY * Vc.y + AxisZ * Vc.z;\n"
"    float3 R    = Pw - PrevEye;\n"
"    float3 Vp   = float3(dot(R, PrevAxisX), dot(R, PrevAxisY), dot(R, PrevAxisZ));\n"
"    if (Vp.z <= 0.0001f)\n"
"        return float4(0.0f, 0.0f, 0.0f, 0.0f);            // behind the previous camera: nothing to say\n"
"    float2 Was = (float2(Vp.x / (PrevTanX * Vp.z), Vp.y / (PrevTanY * Vp.z)) * 0.5f + 0.5f) * Size;\n"
"    return float4(Was - Here, 0.0f, 0.0f);\n"
"}\n";

// The see-through water's own depth into the world's (FFinalPass::InitWaterStamp): 0 where there is no water, which is
// left alone.
static const char* GFinalWaterStampShader =
"Texture2D<float> Water : register(t0);\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"float PSMain(float4 Pos : SV_Position) : SV_Depth\n"
"{\n"
"    const float D = Water.Load(int3(int2(Pos.xy), 0));\n"
"    if (D <= 0.0f)\n"
"        discard;\n"
"    return D;\n"
"}\n";

// The motion vectors of a body that moves BY ITSELF (docs/fsr3-atadas.md 5/3.). The full-screen pass above knows only the
// camera: it moves every pixel as if the world stood still, so a walking character or an opening door gets its background's
// vector, and a temporal upscaler smears it. This one draws those bodies' own triangles - the shapes the shadow maps already
// hold, in world units - and writes, per pixel, where that piece of the body was in the PREVIOUS frame.
//
// M0..M2: the body's own movement, an affine 3x4 that takes a point from where the body is NOW to where the same point of it
// was BEFORE. For a body that only moved and turned this is exact; a walking character's limbs get the body's movement, which
// is far closer to the truth than the background's. The jitter is in the clip position (the raster must land where the scene
// was drawn) but NOT in the vector: the upscaler is told the vectors carry no jitter.
//
// VSMainVerts is for a body whose every vertex comes with where it was the frame before, in a second stream: an arm
// that swings and a door that turns are then as exact as the body that only moved.
static const char* GFinalMotionObjShader =
"cbuffer MotionObj : register(b0)\n"
"{\n"
"    float3 AxisX;     float TanX;\n"
"    float3 AxisY;     float TanY;\n"
"    float3 AxisZ;     float SizeX;\n"
"    float3 Eye;       float SizeY;\n"
"    float3 PrevAxisX; float PrevTanX;\n"
"    float3 PrevAxisY; float PrevTanY;\n"
"    float3 PrevAxisZ; float JitterX;\n"
"    float3 PrevEye;   float JitterY;\n"
"    float4 M0; float4 M1; float4 M2;\n"
"    float  Nearer;    float3 Spare;  // the share of its depth the body is drawn nearer by, for the depth test alone\n"
"};\n"
"\n"
"struct VSOut\n"
"{\n"
"    float4 Pos: SV_Position;\n"
"    float4 Prev : TEXCOORD0;   // where this point was, in the previous frame's clip space\n"
"    float4 Now: TEXCOORD1;   // and where it is now, without the jitter\n"
"};\n"
"\n"
"// P: where the point is now; Pp: where it was the frame before - both in the world.\n"
"VSOut Both(float3 P, float3 Pp)\n"
"{\n"
"    VSOut Out;\n"
"    float3 R = P - Eye;\n"
"    float3 V = float3(dot(R, AxisX), dot(R, AxisY), dot(R, AxisZ));\n"
"    float  W = max(V.z, 1e-4f);\n"
"    // The scene's own projection (FSolidPass VSMain): z/w = 1/W, and the jitter in the clip position. The depth a\n"
"    // share nearer: the shape drawn here and the body the scene drew are the same triangles by two routes.\n"
"    Out.Pos  = float4(V.x / TanX + JitterX * W, -V.y / TanY + JitterY * W, 1.0f + Nearer, W);\n"
"    Out.Now  = float4(V.x / TanX, -V.y / TanY, 1.0f, W);\n"
"    float3 Rp = Pp - PrevEye;\n"
"    float3 Vp = float3(dot(Rp, PrevAxisX), dot(Rp, PrevAxisY), dot(Rp, PrevAxisZ));\n"
"    Out.Prev = float4(Vp.x / PrevTanX, -Vp.y / PrevTanY, 1.0f, Vp.z);\n"
"    return Out;\n"
"}\n"
"\n"
"VSOut VSMain(float3 P : POSITION)\n"
"{\n"
"    float4 P1 = float4(P, 1.0f);\n"
"    return Both(P, float3(dot(M0, P1), dot(M1, P1), dot(M2, P1)));\n"
"}\n"
"\n"
"VSOut VSMainVerts(float3 P : POSITION, float3 Pp : PREVPOS)\n"
"{\n"
"    return Both(P, Pp);\n"
"}\n"
"\n"
"float4 PSMain(VSOut In) : SV_Target\n"
"{\n"
"    // Behind the previous camera there is nothing to say: leave what the camera pass wrote.\n"
"    if (In.Prev.w <= 1e-4f)\n"
"        discard;\n"
"    float2 Size = float2(SizeX, SizeY);\n"
"    float2 Was  = ((In.Prev.xy / In.Prev.w) * float2(0.5f, -0.5f) + 0.5f) * Size;\n"
"    float2 Here = ((In.Now.xy  / In.Now.w)  * float2(0.5f, -0.5f) + 0.5f) * Size;\n"
"    return float4(Was - Here, 0.0f, 0.0f);\n"
"}\n";

// The motion vectors as a picture, to check them by eye: grey where nothing moved, red to the right, green downwards,
// blue the distance moved.
static const char* GFinalMotionViewShader =
"Texture2D<float2> Motion : register(t0);\n"
"cbuffer View : register(b0) { float Scale; float ScaleX; float ScaleY; float Pad; };\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    int2   P = int2(Pos.xy * float2(ScaleX, ScaleY));\n"
"    float2 M = Motion.Load(int3(P, 0)) / max(Scale, 0.0001f);\n"
"    return float4(saturate(0.5f + 0.5f * M.x), saturate(0.5f + 0.5f * M.y), saturate(length(M)), 1.0f);\n"
"}\n";

static const char* GFinalShader =
"Texture2D Scene : register(t0);\n"
"cbuffer Output : register(b0) { float Exponent; };\n"
"\n"
"// One triangle that covers the screen: (-1,1), (3,1), (-1,-3). No vertex buffer.\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"// One texel per pixel and no filtering: the scene target is exactly the frame's size.\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    float3 C = Scene.Load(int3(Pos.xy, 0)).rgb;\n"
"    return float4(pow(max(C, 0.0f), Exponent), 1.0f);\n"
"}\n";

// HDR (the HDR key at On): the same last pass, with the output transform after it. Its own shader, so the
// plain one above stays exactly what it was for the two SDR modes (docs/szabaly-vanilla.md 2.).
//
// The four steps, and why each is where it is:
//
//   1. The brightness curve, as always. What comes out is the picture the player would have seen - except
//      that in this mode the scene target is half floats, so a surface a lamp lit past white is still past
//      white here instead of having been flattened on the way in.
//   2. Undo the encoding. scRGB is LINEAR light and the scene is in the engine's display space, so the
//      numbers have to be turned back into light before they mean anything to the compositor.
//   3. The shoulder, which is identity below the knee. This is the promise the mode makes: everything the
//      compensation anchors around paper white - walls, floors, faces, the HUD - comes out bit for bit what
//      it was, and only what is ABOVE the knee is bent, into whatever headroom the display actually has.
//   4. Paper white. In scRGB 1.0 is 80 nits by definition, so the frame's white is multiplied up to where it
//      should sit. On a display that is NOT in HDR mode the compositor takes 1.0 for white, so that scale is
//      one and step 3 is what keeps the lamps from clipping - which is exactly what makes this mode
//      inspectable on an ordinary monitor.
//
// FalseColour replaces the picture with where each pixel sits against paper white (RdxMath::EHdrBand): blue
// below it, then green, yellow, red. It is the one diagnostic that works on a display with no HDR at all, and
// it answers the question the whole design turns on - are the walls staying put and only the lamps going up.
static const char* GFinalHdrShader =
"Texture2D Scene : register(t0);\n"
"cbuffer Output : register(b0)\n"
"{\n"
"    float Exponent;      // the game's brightness curve\n"
"    float DecodeGamma;   // display space -> linear light\n"
"    float Knee;          // below this the transform is the identity\n"
"    float Ceiling;       // the most the display can show, in multiples of paper white\n"
"    float PaperWhite;    // paper white in scRGB units (nits / 80), or 1 on an SDR display\n"
"    uint  FalseColour;\n"
"    float Pad0;\n"
"    float Pad1;\n"
"};\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"// The same curve as RdxMath::HighlightShoulder, which is where it is worked out and where the tests check it:\n"
"// the identity below the knee, meeting the identity line there with slope one, and approaching the ceiling\n"
"// without ever reaching it. This side has exp() in hardware; that side has to build it out of a series.\n"
"float Shoulder(float X)\n"
"{\n"
"    if (X <= Knee || Ceiling <= Knee)\n"
"        return min(X, Ceiling);\n"
"    float Span = Ceiling - Knee;\n"
"    return Knee + Span * (1.0f - exp(-(X - Knee) / Span));\n"
"}\n"
"\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    float3 C = max(Scene.Load(int3(Pos.xy, 0)).rgb, 0.0f);\n"
"    C = pow(C, Exponent);\n"
"    float3 L = pow(C, DecodeGamma);\n"
"    if (FalseColour != 0)\n"
"    {\n"
"        // Luminance and not the brightest channel: a saturated red lamp and a white one of the same\n"
"        // strength must land in the same band, or the picture reads as a hue map instead of a level map.\n"
"        float Y = dot(L, float3(0.2126f, 0.7152f, 0.0722f));\n"
"        float3 Band = (Y < 1.0f) ? float3(0.0f, 0.15f, 0.6f)\n"
"                    : ((Y < 2.0f) ? float3(0.0f, 0.5f, 0.0f)\n"
"                    : ((Y < 4.0f) ? float3(0.6f, 0.5f, 0.0f) : float3(0.7f, 0.0f, 0.0f)));\n"
"        return float4(Band * PaperWhite, 1.0f);\n"
"    }\n"
"    // Per channel, not on the luminance. A luminance shoulder would keep the hue of a very bright saturated\n"
"    // colour better, at the price of darkening the other two channels with it; per channel is what clipping\n"
"    // did before, only without the cliff - so nothing that used to look right starts looking different.\n"
"    L = float3(Shoulder(L.r), Shoulder(L.g), Shoulder(L.b));\n"
"    return float4(L * PaperWhite, 1.0f);\n"
"}\n";

// Supersampling (docs/terv-latvany-roadmap.md 1.2 and 2.1). The scene target is Scale times the
// output's size in each direction, and an output pixel is the average of its Scale x Scale block.
// A shader of its own, so the vanilla one above stays exactly what it was.
//
// The picture's top and left edge lie half an output pixel inside the target. At 1x that line runs
// exactly through the first pixel centre, which counts as covered; at 2x the first scene row and
// column lie outside the picture and hold only the clear colour - measured: output row 0 and
// column 0 took the bring-up blue, and nothing else did (5999 pixels, 3840 + 2160 - 1). So the
// samples are clamped to the first covered scene pixel: an edge pixel averages its covered half
// instead of blending in a colour that is not part of the picture. Sub-views (the drone camera,
// mirrors) may have the same half-pixel edge inside the frame - not measured, not handled here.
static const char* GFinalBoxShader =
"Texture2D Scene : register(t0);\n"
"cbuffer Output : register(b0) { float Exponent; uint Scale; uint Curve; uint LinearMix; };\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"// Curve: through the brightness curve, or not - into the output target, which the final pass takes through\n"
"// the curve once the HUD is on it too (FBackend::ResolveScene).\n"
"// LinearMix: average in approximately linear light (2.2) - the default - or, off,\n"
"// in the stored numbers.\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    int2 Base = int2(Pos.xy) * int(Scale);\n"
"    int  Edge = int(Scale) / 2;\n"
"    float3 Sum = 0.0f;\n"
"    [loop] for (uint j = 0; j < Scale; j++)\n"
"    {\n"
"        [loop] for (uint i = 0; i < Scale; i++)\n"
"        {\n"
"            int2 P = max(Base + int2(int(i), int(j)), int2(Edge, Edge));\n"
"            float3 S = max(Scene.Load(int3(P, 0)).rgb, 0.0f);\n"
"            Sum += (LinearMix != 0) ? pow(S, 2.2f) : S;\n"
"        }\n"
"    }\n"
"    float3 C = Sum / float(Scale * Scale);\n"
"    if (LinearMix != 0)\n"
"        C = pow(C, 1.0f / 2.2f);\n"
"    return float4((Curve != 0) ? pow(C, Exponent) : C, 1.0f);\n"
"}\n";

// G-buffer, first step (docs/terv-latvany-roadmap.md 1.4): the depth buffer as a picture. The geometry pass stores 1/W,
// W the engine's view-space depth (SolidPass: Pos.z = 1, Pos.w = W), so the distance is the reciprocal, and a cleared
// pixel (0) is where nothing was drawn. The normal is rebuilt from the neighbours' camera-space positions; on each axis
// the side with the smaller depth step is taken, so a silhouette edge does not bend the surface's normal.
static const char* GFinalDepthShader =
"Texture2D<float> Depth : register(t0);\n"
"cbuffer View : register(b0) { uint Mode; uint Scale; float TanX; float TanY; };\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"// Camera space as the engine has it: X right, Y down, Z forward.\n"
"float3 ViewPos(int2 P, float2 Size)\n"
"{\n"
"    float D = Depth.Load(int3(P, 0));\n"
"    float W = (D > 0.0f) ? 1.0f / D : 1.0e6f;\n"
"    float2 Ndc = (float2(P) + 0.5f) / Size * 2.0f - 1.0f;\n"
"    return float3(Ndc.x * TanX * W, Ndc.y * TanY * W, W);\n"
"}\n"
"\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    // 3: the shadows' factor, or their share map, as it is - the output's size or half of it (Scale 1 or 2).\n"
"    if (Mode == 3)\n"
"        return float4(Depth.Load(int3(int2(uint2(Pos.xy) / Scale), 0)).xxx, 1.0f);\n"
"    // 4: the scene-sized light target (the light each surface receives), as it is.\n"
"    if (Mode == 4)\n"
"        return float4(Depth.Load(int3(int2(Pos.xy) * int(Scale), 0)).xxx, 1.0f);\n"
"    uint Wd, Ht;\n"
"    Depth.GetDimensions(Wd, Ht);\n"
"    float2 Size = float2(Wd, Ht);\n"
"    int2 P = int2(Pos.xy) * int(Scale);\n"
"    float D = Depth.Load(int3(P, 0));\n"
"    if (D <= 0.0f)\n"
"        return float4(0.0f, 0.0f, 0.25f, 1.0f);\n"
"    if (Mode == 1)\n"
"    {\n"
"        float T = saturate(log2((1.0f / D) / 16.0f) / 10.0f);\n"
"        return float4((1.0f - T).xxx, 1.0f);\n"
"    }\n"
"    int2 Mx = int2(int(Wd) - 1, int(Ht) - 1);\n"
"    float3 C  = ViewPos(P, Size);\n"
"    float3 Rt = ViewPos(min(P + int2(1, 0), Mx), Size) - C;\n"
"    float3 Lf = C - ViewPos(max(P - int2(1, 0), int2(0, 0)), Size);\n"
"    float3 Dn = ViewPos(min(P + int2(0, 1), Mx), Size) - C;\n"
"    float3 Up = C - ViewPos(max(P - int2(0, 1), int2(0, 0)), Size);\n"
"    // At the target's edge one neighbour is the pixel itself: the other side's step (as in the shadows' factor).\n"
"    float3 Dx = (P.x >= Mx.x) ? Lf : ((P.x <= 0) ? Rt : ((abs(Rt.z) < abs(Lf.z)) ? Rt : Lf));\n"
"    float3 Dy = (P.y >= Mx.y) ? Up : ((P.y <= 0) ? Dn : ((abs(Dn.z) < abs(Up.z)) ? Dn : Up));\n"
"    float3 Cr = cross(Dy, Dx);\n"
"    float CrL = length(Cr);\n"
"    float3 N = (CrL > 1.0e-12f) ? Cr / max(CrL, 1.0e-12f) : float3(0.0f, 0.0f, -1.0f);\n"
"    return float4(N * 0.5f + 0.5f, 1.0f);\n"
"}\n";

// Both passes have the same shape: the scene target as one SRV, NumConstants 32-bit values at b0,
// one full-screen triangle into the back buffer's format. The vanilla pass is built with exactly
// the description it always had; only the box pass (supersampling) asks for more constants.
static UBOOL BuildFinalPipeline(ID3D12Device* Device, const char* Source, const char* SourceName,
                                UINT NumConstants, DXGI_FORMAT RtFormat, const TCHAR* What,
                                TComRef<ID3D12RootSignature>& OutRoot,
                                TComRef<ID3D12PipelineState>& OutPipeline)
{
    D3D12_DESCRIPTOR_RANGE Range;
    appMemzero(&Range, sizeof(Range));
    Range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors     = 1;
    Range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER Params[2];
    appMemzero(Params, sizeof(Params));
    Params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[0].DescriptorTable.NumDescriptorRanges = 1;
    Params[0].DescriptorTable.pDescriptorRanges   = &Range;
    Params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    Params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[1].Constants.ShaderRegister = 0;
    Params[1].Constants.Num32BitValues = NumConstants;
    Params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters = 2;
    Desc.pParameters   = Params;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), OutRoot.VoidSlot())))
    {
        RdxLog(TEXT("%s: root signature failed"), What);
        return 0;
    }

    TComRef<ID3DBlob> Vs, Ps;
    const SIZE_T Len = strlen(Source);
    if (FAILED(RdxCompile(Source, Len, SourceName, nullptr, nullptr, "VSMain", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(Source, Len, SourceName, nullptr, nullptr, "PSMain", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("%s: shader failed: %s"), What,
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature     = OutRoot.Get();
    Pso.VS.pShaderBytecode = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength  = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength  = Ps->GetBufferSize();
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 1;
    // The back buffer receives the finished, already encoded numbers: no conversion.
    Pso.RTVFormats[0]         = RtFormat;
    Pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    Pso.SampleDesc.Count      = 1;
    Pso.SampleMask            = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = TRUE;
    Pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    Pso.DepthStencilState.DepthEnable   = FALSE;

    if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), OutPipeline.VoidSlot())))
    {
        RdxLog(TEXT("%s: pipeline state failed"), What);
        return 0;
    }
    return 1;
}

UBOOL FFinalPass::Init(ID3D12Device* Device)
{
    return BuildFinalPipeline(Device, GFinalShader, "FinalPass", 1, Colours.BackBuffer, TEXT("final pass"),
                              RootSignature, Pipeline);
}

UBOOL FFinalPass::InitBox(ID3D12Device* Device)
{
    return BuildFinalPipeline(Device, GFinalBoxShader, "FinalPassBox", 4, Colours.BackBuffer,
                              TEXT("final pass (box)"),
                              BoxRootSignature, BoxPipeline);
}

UBOOL FFinalPass::InitHdr(ID3D12Device* Device)
{
    return BuildFinalPipeline(Device, GFinalHdrShader, "FinalPassHdr", 8, Colours.BackBuffer,
                              TEXT("final pass (HDR)"),
                              HdrRootSignature, HdrPipeline);
}

void FFinalPass::DrawHdr(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                         D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv, const FHdrOutput& Out)
{
    if (!HdrPipeline || !Heap)
        return;
    // The cbuffer's layout, in its own order - the shader reads them as one block of eight.
    struct FConstants
    {
        FLOAT Exponent, DecodeGamma, Knee, Ceiling, PaperWhite;
        UINT  FalseColour;
        FLOAT Pad0, Pad1;
    } C;
    C.Exponent    = Out.Exponent;
    C.DecodeGamma = Out.DecodeGamma;
    C.Knee        = Out.Knee;
    C.Ceiling     = Out.Ceiling;
    C.PaperWhite  = Out.PaperWhite;
    C.FalseColour = Out.bFalseColour ? 1u : 0u;
    C.Pad0        = 0.0f;
    C.Pad1        = 0.0f;

    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(HdrRootSignature.Get());
    Cmd->SetPipelineState(HdrPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, SceneSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 8, &C, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

// The shadow factor is a file of its own, ShadowFactor.hlsl - the RT shadows' factor is built from the same source, by dxc
// at build time. D3DCompile gets its text out of the DLL (ShadowFactorText.h), and the statements it includes the same way:
// ShadowFactorSurface.hlsli, served by this. RenderLimits.h, the other file it may include, is never asked for here - its
// number comes in as a macro.
class FShadowFactorInclude : public ID3DInclude
{
public:
    STDMETHOD(Open)(D3D_INCLUDE_TYPE, LPCSTR Name, LPCVOID, LPCVOID* Data, UINT* Bytes)
    {
        if (Name && lstrcmpA(Name, "ShadowFactorSurface.hlsli") == 0)
        {
            *Data  = GShadowFactorSurfaceText;
            *Bytes = (UINT)(sizeof(GShadowFactorSurfaceText) - 1);
            return S_OK;
        }
        return E_FAIL;
    }
    STDMETHOD(Close)(LPCVOID)
    {
        return S_OK;
    }
};

// The shadows onto the picture: picture x factor + added x (1 - factor), by the blend (ONE, SRC_ALPHA). The added target
// (FSolidPass, the third target) holds what the blended draws laid over the surfaces made of the picture - blended as the
// picture, from zero - so the surfaces underneath get their shadow, and a flame, steam or glass over them keeps its light.
// The old way, measured in the second round: a translucent pixel was left out of the shadow whole, and the
// shadow behind the steam ended at the steam's outline.
// A see-through water pixel (Water: FBackend::ApplyShadows has this frame's): what the shadows may take of it is the bed
// seen through the water and the water's own light (SolidPass.hlsl's WaterShade). The bed takes the factor of the
// place it was seen at, the water's own light the factor of the surface's own place (WaterFactor), each by its share;
// what comes off the surface is in the added target. A pixel is the water's where its own depth stands in front of the
// world's, and nothing drawn after the engine's ClearZ covers it (NoOnTop: there was none this frame).
static const char* GFinalShadowApplyShader =
"Texture2D<float> Factor : register(t0);\n"
"Texture2D<float4> Added : register(t1);\n"
"Texture2D<float> WaterDepth : register(t2);\n"
"Texture2D<float> OnTopDepth : register(t3);\n"
"Texture2D<float4> WaterShade : register(t5);\n"
"Texture2D<float> WaterFactor : register(t8);\n"
"Texture2D<float> WorldDepth : register(t9);\n"
"SamplerState Lin : register(s0);\n"
"cbuffer Size : register(b0) { float InvW; float InvH; float Water; float NoOnTop; };\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    const float2 Uv = Pos.xy * float2(InvW, InvH);\n"
"    const int3   P  = int3(int2(Pos.xy), 0);\n"
"    float F = Factor.SampleLevel(Lin, Uv, 0.0f);\n"
"    if (Water > 0.5f)\n"
"    {\n"
"        const float Surface = WaterDepth.Load(P);\n"
"        if (Surface > 0.0f && Surface >= WorldDepth.Load(P) && (NoOnTop > 0.5f || OnTopDepth.Load(P) <= 0.0f))\n"
"        {\n"
"            const float4 Shade = WaterShade.Load(P);\n"
"            F = lerp(WaterFactor.SampleLevel(Lin, Uv, 0.0f), Factor.SampleLevel(Lin, Uv + Shade.zw, 0.0f),\n"
"                     saturate(Shade.y));\n"
"        }\n"
"    }\n"
"    float3 A = Added.Load(P).rgb;\n"
"    return float4(A * (1.0f - F), F);\n"
"}\n";

// The water surface's factor's input (FBackend::ApplyShadows): the water's own depth (t0) and the light its surface
// receives (t1's x), each taken at the middle of a pack pixel's Scale x Scale block of the scene's pixels, as
// ShadowFactorSurface.hlsli takes the world's. The water surface's factor reads it in the world depth's place and the
// light target's, by the same rule again: a small part of the scene's pixels' work, and a small copy for the 64-bit
// host where the scene-sized two would be.
static const char* GFinalWaterPackShader =
"Texture2D<float> WaterDepth : register(t0);\n"
"Texture2D<float4> WaterShade : register(t1);\n"
"cbuffer Pack : register(b0) { uint Scale; };\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"float2 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    uint Wd, Ht;\n"
"    WaterDepth.GetDimensions(Wd, Ht);\n"
"    const int2 P = min(int2(Pos.xy) * int(Scale) + int(Scale / 2u), int2(int(Wd) - 1, int(Ht) - 1));\n"
"    return float2(WaterDepth.Load(int3(P, 0)), WaterShade.Load(int3(P, 0)).x);\n"
"}\n";

// The shadow, caption and upscaling pipelines: NumSrvs views at t0.. (none: the constants are root parameter 0),
// NumConstants 32-bit values at b0, optionally one linear clamping sampler at s0, one full-screen triangle into RtFormat.
// Blend 0 replaces the target, 1 multiplies into it (dest * src colour), 2 lays over it by the source's alpha, 3 adds the
// source's colour to the target scaled by the source's alpha (src + dest * src alpha) - 1 to 3 keep the destination alpha.
// Include: where the shader's #include files come from, if it has any.
static UBOOL BuildPassPipeline(ID3D12Device* Device, const char* Source, const char* SourceName, UINT NumSrvs,
                               UINT NumConstants, UBOOL bSampler, DXGI_FORMAT RtFormat, INT Blend,
                               const TCHAR* What, TComRef<ID3D12RootSignature>& OutRoot,
                               TComRef<ID3D12PipelineState>& OutPipeline, ID3DInclude* Include = nullptr)
{
    D3D12_DESCRIPTOR_RANGE Range;
    appMemzero(&Range, sizeof(Range));
    Range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors     = NumSrvs;
    Range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER Params[2];
    appMemzero(Params, sizeof(Params));
    Params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[0].DescriptorTable.NumDescriptorRanges = 1;
    Params[0].DescriptorTable.pDescriptorRanges   = &Range;
    Params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    Params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[1].Constants.ShaderRegister = 0;
    Params[1].Constants.Num32BitValues = NumConstants;
    Params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC Sampler;
    appMemzero(&Sampler, sizeof(Sampler));
    Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    Sampler.ShaderRegister   = 0;
    Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = NumSrvs ? 2 : 1;
    Desc.pParameters       = NumSrvs ? Params : &Params[1];
    Desc.NumStaticSamplers = bSampler ? 1 : 0;
    Desc.pStaticSamplers   = bSampler ? &Sampler : nullptr;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), OutRoot.VoidSlot())))
    {
        RdxLog(TEXT("%s: root signature failed"), What);
        return 0;
    }

    TComRef<ID3DBlob> Vs, Ps;
    const SIZE_T Len = strlen(Source);
    if (FAILED(RdxCompile(Source, Len, SourceName, nullptr, Include, "VSMain", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(Source, Len, SourceName, nullptr, Include, "PSMain", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("%s: shader failed: %s"), What,
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature     = OutRoot.Get();
    Pso.VS.pShaderBytecode = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength  = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength  = Ps->GetBufferSize();
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 1;
    Pso.RTVFormats[0]         = RtFormat;
    Pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    Pso.SampleDesc.Count      = 1;
    Pso.SampleMask            = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = TRUE;
    Pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (Blend == 1 || Blend == 2 || Blend == 3)
    {
        D3D12_RENDER_TARGET_BLEND_DESC& B = Pso.BlendState.RenderTarget[0];
        B.BlendEnable    = TRUE;
        B.SrcBlend       = (Blend == 1) ? D3D12_BLEND_ZERO : ((Blend == 2) ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE);
        B.DestBlend      = (Blend == 1) ? D3D12_BLEND_SRC_COLOR
                                        : ((Blend == 2) ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_SRC_ALPHA);
        B.BlendOp        = D3D12_BLEND_OP_ADD;
        B.SrcBlendAlpha  = D3D12_BLEND_ZERO;
        B.DestBlendAlpha = D3D12_BLEND_ONE;
        B.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    }
    Pso.DepthStencilState.DepthEnable = FALSE;

    if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), OutPipeline.VoidSlot())))
    {
        RdxLog(TEXT("%s: pipeline state failed"), What);
        return 0;
    }
    return 1;
}

// The shadow factor's root signature (ShadowFactor.hlsl): six views at t0-t5, the constants in a buffer at b0 - 144
// values, more than a root signature's 64 - and a linear comparison sampler at s0.
static UBOOL BuildShadowFactorRoot(ID3D12Device* Device, TComRef<ID3D12RootSignature>& OutRoot, const TCHAR* What)
{
    D3D12_DESCRIPTOR_RANGE Range;
    appMemzero(&Range, sizeof(Range));
    Range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors     = 6;
    Range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER Params[2];
    appMemzero(Params, sizeof(Params));
    Params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[0].DescriptorTable.NumDescriptorRanges = 1;
    Params[0].DescriptorTable.pDescriptorRanges   = &Range;
    Params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    Params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    Params[1].Descriptor.ShaderRegister = 0;
    Params[1].Descriptor.RegisterSpace  = 0;
    Params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC Sampler;
    appMemzero(&Sampler, sizeof(Sampler));
    Sampler.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    Sampler.ShaderRegister   = 0;
    Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 2;
    Desc.pParameters       = Params;
    Desc.NumStaticSamplers = 1;
    Desc.pStaticSamplers   = &Sampler;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), OutRoot.VoidSlot())))
    {
        RdxLog(TEXT("%s: root signature failed"), What);
        return 0;
    }
    return 1;
}

// The factor's pipeline state around a root signature and two shaders, into an R8 target.
static UBOOL BuildShadowFactorState(ID3D12Device* Device, ID3D12RootSignature* Root, const void* Vs, SIZE_T VsBytes,
                                    const void* Ps, SIZE_T PsBytes, TComRef<ID3D12PipelineState>& OutPipeline,
                                    const TCHAR* What)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature     = Root;
    Pso.VS.pShaderBytecode = Vs;
    Pso.VS.BytecodeLength  = VsBytes;
    Pso.PS.pShaderBytecode = Ps;
    Pso.PS.BytecodeLength  = PsBytes;
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 1;
    Pso.RTVFormats[0]         = DXGI_FORMAT_R8_UNORM;
    Pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    Pso.SampleDesc.Count      = 1;
    Pso.SampleMask            = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = TRUE;
    Pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    Pso.DepthStencilState.DepthEnable = FALSE;
    if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), OutPipeline.VoidSlot())))
    {
        RdxLog(TEXT("%s: pipeline state failed"), What);
        return 0;
    }
    return 1;
}

// The maps' factor: ShadowFactor.hlsl without RDX_RT, compiled here from the text in the DLL - with RDX_RT it is the
// rays' factor, which the 64-bit host builds and runs. The arrays' length comes in as a macro - the number
// RdxMath::ShadowMaxLights is.
static UBOOL BuildShadowFactorPipeline(ID3D12Device* Device, TComRef<ID3D12RootSignature>& OutRoot,
                                       TComRef<ID3D12PipelineState>& OutPipeline)
{
    const TCHAR* const What = TEXT("shadow factor");
    if (!BuildShadowFactorRoot(Device, OutRoot, What))
        return 0;
    const D3D_SHADER_MACRO Macros[2] = { { "RDX_SHADOW_MAX_LIGHTS", RDX_STRINGIFY(RDX_SHADOW_MAX_LIGHTS) },
                                         { nullptr, nullptr } };
    FShadowFactorInclude Include;
    TComRef<ID3DBlob> Vs, Ps, Error;
    const SIZE_T Len = sizeof(GShadowFactorText) - 1;
    if (FAILED(RdxCompile(GShadowFactorText, Len, "ShadowFactor.hlsl", Macros, &Include, "VSMain", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GShadowFactorText, Len, "ShadowFactor.hlsl", Macros, &Include, "PSMain", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("%s: shader failed: %s"), What,
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    return BuildShadowFactorState(Device, OutRoot.Get(), Vs->GetBufferPointer(), Vs->GetBufferSize(),
                                  Ps->GetBufferPointer(), Ps->GetBufferSize(), OutPipeline, What);
}

UBOOL FFinalPass::InitShadow(ID3D12Device* Device, DXGI_FORMAT SceneFormat)
{
    return BuildShadowFactorPipeline(Device, ShadowFactorRoot, ShadowFactorPipeline) &&
           BuildPassPipeline(Device, GFinalShadowApplyShader, "ShadowApply", 10, 4, 1, SceneFormat, 3,
                             TEXT("shadow apply"), ShadowApplyRoot, ShadowApplyPipeline);
}

void FFinalPass::DrawShadowFactor(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                                  D3D12_GPU_DESCRIPTOR_HANDLE Table, D3D12_GPU_VIRTUAL_ADDRESS Constants)
{
    if (!ShadowFactorPipeline || !Heap || !Constants)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(ShadowFactorRoot.Get());
    Cmd->SetPipelineState(ShadowFactorPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, Table);
    Cmd->SetGraphicsRootConstantBufferView(1, Constants);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

void FFinalPass::DrawShadowApply(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                                 D3D12_GPU_DESCRIPTOR_HANDLE FactorSrv, FLOAT InvW, FLOAT InvH, FLOAT Water,
                                 FLOAT NoOnTop)
{
    if (!ShadowApplyPipeline || !Heap)
        return;
    const FLOAT Values[4] = { InvW, InvH, Water, NoOnTop };
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(ShadowApplyRoot.Get());
    Cmd->SetPipelineState(ShadowApplyPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, FactorSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 4, Values, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

UBOOL FFinalPass::InitWaterPack(ID3D12Device* Device)
{
    if (WaterPackPipeline)
        return 1;
    return BuildPassPipeline(Device, GFinalWaterPackShader, "WaterPack", 2, 1, 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                             TEXT("water pack"), WaterPackRoot, WaterPackPipeline);
}

void FFinalPass::DrawWaterPack(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                               D3D12_GPU_DESCRIPTOR_HANDLE Table, UINT Scale)
{
    if (!WaterPackPipeline || !Heap)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(WaterPackRoot.Get());
    Cmd->SetPipelineState(WaterPackPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, Table);
    Cmd->SetGraphicsRoot32BitConstants(1, 1, &Scale, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

// Ambient occlusion - see RdxMath::EAmbientOcclusion for what it is and why it darkens the shadow factor. Both passes open
// with the same text: the constants (RdxMath::AoCb*), the full-screen triangle, and the rules for which pixels to leave
// out, which are the shadow factor's own. The occlusion must not reach what the shadows do not reach: the weapon in the
// hand is drawn over a world that goes on behind it, and with the world's depth under it, it would pick up the creases of
// whatever wall it happened to be held in front of.
#define RDX_AO_COMMON \
"Texture2D<float> WorldDepth : register(t0);\n" \
"Texture2D<float> OnTopDepth : register(t1);\n" \
"Texture2D<float> SmallDepth : register(t2);   // the world's depth, one value a texel of this target\n" \
"cbuffer Ao : register(b0)\n" \
"{\n" \
"    float TanX; float TanY; uint Scale; uint NoOnTop;\n" \
"    float4 ViewRect;                 // the world view's part of the target: x y width height, 0..1\n" \
"    float4 SkipRect[4];              // views drawn later onto other parts of the screen: left out\n" \
"    uint SkipCount; float Radius; float Strength; float CosBias;\n" \
"};\n" \
"\n" \
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n" \
"{\n" \
"    float2 P = float2((Id << 1) & 2, Id & 2);\n" \
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n" \
"}\n" \
"\n" \
"// The scene pixel a texel of this target stands for - the shadow factor's own rule, so the two agree on every pixel.\n" \
"int2 ScenePixel(int2 Q, uint Wd, uint Ht)\n" \
"{\n" \
"    return min(Q * int(Scale) + int(Scale / 2u), int2(int(Wd) - 1, int(Ht) - 1));\n" \
"}\n" \
"\n" \
"// Outside the world view, inside a view drawn later onto another part of the screen (the spy drone's window), under\n" \
"// anything drawn on top of the world, and the sky, which has no depth.\n" \
"bool LeftOut(int2 P, float2 Size)\n" \
"{\n" \
"    float2 Uv = (float2(P) + 0.5f) / Size;\n" \
"    if (any(Uv < ViewRect.xy) || any(Uv >= ViewRect.xy + ViewRect.zw))\n" \
"        return true;\n" \
"    [loop] for (uint r = 0; r < SkipCount; r++)\n" \
"    {\n" \
"        if (all(Uv >= SkipRect[r].xy) && all(Uv < SkipRect[r].xy + SkipRect[r].zw))\n" \
"            return true;\n" \
"    }\n" \
"    if (NoOnTop == 0u && OnTopDepth.Load(int3(P, 0)) > 0.0f)\n" \
"        return true;\n" \
"    return !(WorldDepth.Load(int3(P, 0)) > 0.0f);\n" \
"}\n" \
"\n"

// The world's depth copied down to this target's size: for every texel, the depth of the very scene pixel the texel
// stands for (ScenePixel) - so the blur's weights read exactly what they read from the full buffer, and the raw pass's
// samples read it from a sixteenth of the memory (FBackend::EnsureAoTarget says why that matters).
// What casts none of our shadows (surface kind 3, the intro's Ion Storm logo) is no depth here: it hides nothing from
// the occlusion's samples, as it is not in the RT occlusion's scene.
static const char* GFinalAoDepthShader =
RDX_AO_COMMON
"Texture2D<uint2> WorldStencil : register(t5);   // the surface kinds (FSolidPass::SetKind)\n"
"float PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    uint Wd, Ht;\n"
"    WorldDepth.GetDimensions(Wd, Ht);\n"
"    const int3 P = int3(ScenePixel(int2(Pos.xy), Wd, Ht), 0);\n"
"    return (WorldStencil.Load(P).g == 3u) ? 0.0f : WorldDepth.Load(P);\n"
"}\n";

// The raw occlusion, one value a factor texel, into an R8 target of its own. For each texel: the surface and its normal
// from the full depth, exactly as the shadow factor finds them; then RDX_AO_SAMPLES neighbours on a spiral within the
// world radius, as it falls on screen at that distance, read from the small depth; and each neighbour's share by
// RdxMath::AoTap's curve.
static const char* GFinalAoShader =
RDX_AO_COMMON
"float2 ViewNdc(int2 P, float2 Size)\n"
"{\n"
"    return ((float2(P) + 0.5f) / Size - ViewRect.xy) / ViewRect.zw * 2.0f - 1.0f;\n"
"}\n"
"\n"
"float3 ViewFrom(int2 P, float D, float2 Size)\n"
"{\n"
"    float W = 1.0f / D;\n"
"    float2 Ndc = ViewNdc(P, Size);\n"
"    return float3(Ndc.x * TanX * W, Ndc.y * TanY * W, W);\n"
"}\n"
"\n"
"float3 ViewAt(int2 P, float2 Size)\n"
"{\n"
"    float D = WorldDepth.Load(int3(P, 0));\n"
"    float W = (D > 0.0f) ? 1.0f / D : 1.0e6f;\n"
"    float2 Ndc = ViewNdc(P, Size);\n"
"    return float3(Ndc.x * TanX * W, Ndc.y * TanY * W, W);\n"
"}\n"
"\n"
"float PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    uint Wd, Ht;\n"
"    WorldDepth.GetDimensions(Wd, Ht);\n"
"    float2 Size = float2(Wd, Ht);\n"
"    int2 Q = int2(Pos.xy);\n"
"    int2 P = ScenePixel(Q, Wd, Ht);\n"
"    if (LeftOut(P, Size))\n"
"        return 1.0f;\n"
"    float3 S = ViewAt(P, Size);\n"
"    // The normal from the depth, as the shadow factor finds it: along each axis the neighbour nearer in depth, so\n"
"    // that an edge does not bend the normal of the surface beside it.\n"
"    int2 Mx = int2(int(Wd) - 1, int(Ht) - 1);\n"
"    float3 Rt = ViewAt(min(P + int2(1, 0), Mx), Size) - S;\n"
"    float3 Lf = S - ViewAt(max(P - int2(1, 0), int2(0, 0)), Size);\n"
"    float3 Dn = ViewAt(min(P + int2(0, 1), Mx), Size) - S;\n"
"    float3 Ab = S - ViewAt(max(P - int2(0, 1), int2(0, 0)), Size);\n"
"    float3 Dx = (P.x >= Mx.x) ? Lf : ((P.x <= 0) ? Rt : ((abs(Rt.z) < abs(Lf.z)) ? Rt : Lf));\n"
"    float3 Dy = (P.y >= Mx.y) ? Ab : ((P.y <= 0) ? Dn : ((abs(Dn.z) < abs(Ab.z)) ? Dn : Ab));\n"
"    float3 Cr = cross(Dy, Dx);\n"
"    float CrL = length(Cr);\n"
"    if (!(CrL > 1.0e-12f))\n"
"        return 1.0f;\n"
"    float3 N = Cr / CrL;\n"
"    // The radius on screen: the world radius at this distance, in scene pixels. Held under a bound, so that a surface\n"
"    // just in front of the camera does not send its samples across half the screen; and where it has shrunk under a\n"
"    // pixel, nothing near enough is left to find.\n"
"    float Rpx = Radius / (S.z * TanX) * (0.5f * ViewRect.z * Size.x);\n"
"    if (Rpx < 1.0f)\n"
"        return 1.0f;\n"
"    Rpx = min(Rpx, 64.0f * float(Scale));\n"
"    // A turn of the spiral a texel, in a four-by-four tile: sixteen turns. The blur after this pass averages whole tiles,\n"
"    // two each way, so the pattern never reaches the picture. The turn is ONE rotation applied to every sample's direction:\n"
"    // each direction's own angle is a constant once the loop is unrolled, so no sample pays for a sine of its own.\n"
"    float2 Rot;\n"
"    uint Tile = uint(Q.x & 3) + (uint(Q.y & 3) << 2);\n"
"    sincos(float(Tile) * (6.2831853f / 16.0f), Rot.y, Rot.x);\n"
"    // And a step outwards a texel, the tile's sixteen in the bits' reverse order (so that it does not go round with the\n"
"    // turn): each of the sixteen puts its samples at radii of their own, and the tile's 16 x RDX_AO_SAMPLES cover the\n"
"    // disc evenly instead of in RDX_AO_SAMPLES rings - fewer of the rings' patterns left over for the blur.\n"
"    float Step = (float(reversebits(Tile) >> 28) + 0.5f) / 16.0f;\n"
"    uint Aw, Ah;\n"
"    SmallDepth.GetDimensions(Aw, Ah);\n"
"    int2 Last = int2(int(Aw) - 1, int(Ah) - 1);\n"
"    float R2 = Radius * Radius;\n"
"    float Sum = 0.0f;\n"
"    [unroll] for (uint k = 0; k < " RDX_STRINGIFY(RDX_AO_SAMPLES) "u; k++)\n"
"    {\n"
"        float2 B = float2(cos(float(k) * 2.39996323f), sin(float(k) * 2.39996323f));\n"
"        float2 Dir = float2(B.x * Rot.x - B.y * Rot.y, B.x * Rot.y + B.y * Rot.x);\n"
"        float Rk = sqrt((float(k) + Step) / " RDX_STRINGIFY(RDX_AO_SAMPLES) ".0f) * Rpx;\n"
"        // The sample's texel in the small depth, and the scene pixel that texel stands for: the sample snaps to this\n"
"        // target's grid, which moves it by less than a texel. Where the whole radius is only a few texels, the\n"
"        // occlusion is too small on screen for that to show.\n"
"        int2 Tq = clamp(int2((float2(P) + Dir * Rk) / float(Scale)), int2(0, 0), Last);\n"
"        float Dt = SmallDepth.Load(int3(Tq, 0));\n"
"        if (!(Dt > 0.0f))\n"
"            continue;\n"
"        float3 V = ViewFrom(ScenePixel(Tq, Wd, Ht), Dt, Size) - S;\n"
"        // RdxMath::AoTap: nothing from the surface's own plane or behind it, nothing from beyond the radius.\n"
"        float VV = dot(V, V);\n"
"        if (VV >= R2 || VV < 1.0e-6f)\n"
"            continue;\n"
"        float C = dot(V, N) * rsqrt(VV);\n"
"        Sum += saturate((C - CosBias) / (1.0f - CosBias)) * (1.0f - VV / R2);\n"
"    }\n"
"    // RdxMath::AoFromSum: straight down to the knee, then slowly towards black.\n"
"    float A = 1.0f - Strength * Sum / " RDX_STRINGIFY(RDX_AO_SAMPLES) ".0f;\n"
"    float K = " RDX_STRINGIFY(RDX_AO_KNEE) ";\n"
"    return (A >= K) ? min(A, 1.0f) : K / (1.0f + (K - A) / K);\n"
"}\n";

// The raw occlusion blurred, and put into the shadow factor. The blur is two four-by-four tiles each way - each of the raw
// pass's sixteen turns four times - weighted by how near each neighbour lies in depth, so that the occlusion along a wall's
// foot does not bleed onto the floor in front of it, nor the floor's onto a box standing on it. One tile is too little
// where the true occlusion changes within a tile - something floating a little in front of a surface: the turns' values
// differ there for more than their turn, and what one tile's average leaves over repeats tile by tile, a woven grain. The
// depths come from the small depth: for every texel it holds the depth of the very scene pixel the texel stands for,
// which is all the weights need.
static const char* GFinalAoBlurShader =
RDX_AO_COMMON
"Texture2D<float> Raw : register(t3);   // GFinalAoShader's output\n"
"\n"
"float PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    uint Wd, Ht, Aw, Ah;\n"
"    WorldDepth.GetDimensions(Wd, Ht);\n"
"    Raw.GetDimensions(Aw, Ah);\n"
"    float2 Size = float2(Wd, Ht);\n"
"    int2 Q = int2(Pos.xy);\n"
"    int2 P = ScenePixel(Q, Wd, Ht);\n"
"    if (LeftOut(P, Size))\n"
"        return 1.0f;\n"
"    float W = 1.0f / SmallDepth.Load(int3(Q, 0));\n"
"    int2 Last = int2(int(Aw) - 1, int(Ah) - 1);\n"
"    float Sum = 0.0f;\n"
"    float Weights = 0.0f;\n"
"    [unroll] for (int y = -3; y <= 4; y++)\n"
"    {\n"
"        [unroll] for (int x = -3; x <= 4; x++)\n"
"        {\n"
"            int2 Qs = clamp(Q + int2(x, y), int2(0, 0), Last);\n"
"            float Ds = SmallDepth.Load(int3(Qs, 0));\n"
"            if (!(Ds > 0.0f))\n"
"                continue;\n"
"            float Wt = saturate(1.0f - abs(1.0f / Ds - W) / (0.05f * W));\n"
"            Sum += Raw.Load(int3(Qs, 0)) * Wt;\n"
"            Weights += Wt;\n"
"        }\n"
"    }\n"
"    return (Weights > 0.0f) ? Sum / Weights : 1.0f;\n"
"}\n";

// The RT shadows' factor smoothed, in place of the shadow factor. A pixel traces three rays where a shadow's edge crosses
// it (ShadowFactor.hlsl, RDX_RT), and alone that is a grainy edge - three samples of a light's disc. So the texels of a
// four-by-four tile each take their own turn of the disc, and this averages the tile: every edge from the tile's
// forty-eight samples. Weighted by how near each neighbour lies in depth, as the occlusion's blur is, so that a shadow
// does not bleed across an edge of the geometry; what is left out of the shadows (the sky, what is drawn on top) is left
// out of the average. The depth is the world's own at each texel's scene pixel: the small depth is made after this.
static const char* GFinalRtFactorBlurShader =
RDX_AO_COMMON
"Texture2D<float> FactorRaw : register(t4);   // the RT factor as the rays gave it\n"
"\n"
"float PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    uint Wd, Ht, Fw, Fh;\n"
"    WorldDepth.GetDimensions(Wd, Ht);\n"
"    FactorRaw.GetDimensions(Fw, Fh);\n"
"    float2 Size = float2(Wd, Ht);\n"
"    int2 Q = int2(Pos.xy);\n"
"    float F = FactorRaw.Load(int3(Q, 0));\n"
"    int2 P = ScenePixel(Q, Wd, Ht);\n"
"    if (LeftOut(P, Size))\n"
"        return F;\n"
"    float W = 1.0f / WorldDepth.Load(int3(P, 0));\n"
"    int2 Last = int2(int(Fw) - 1, int(Fh) - 1);\n"
"    float Sum = 0.0f;\n"
"    float Weights = 0.0f;\n"
"    [unroll] for (int y = -1; y <= 2; y++)\n"
"    {\n"
"        [unroll] for (int x = -1; x <= 2; x++)\n"
"        {\n"
"            int2 Qs = clamp(Q + int2(x, y), int2(0, 0), Last);\n"
"            int2 Ps = ScenePixel(Qs, Wd, Ht);\n"
"            float Ds = WorldDepth.Load(int3(Ps, 0));\n"
"            if (!(Ds > 0.0f) || (NoOnTop == 0u && OnTopDepth.Load(int3(Ps, 0)) > 0.0f))\n"
"                continue;\n"
"            float Wt = saturate(1.0f - abs(1.0f / Ds - W) / (0.05f * W));\n"
"            Sum += FactorRaw.Load(int3(Qs, 0)) * Wt;\n"
"            Weights += Wt;\n"
"        }\n"
"    }\n"
"    return (Weights > 0.0f) ? Sum / Weights : F;\n"
"}\n";

#undef RDX_AO_COMMON

// Four pipelines from the three shaders: the small depth and the raw pass each replacing their own target; the blur
// MULTIPLYING into the shadow factor (blend 1: factor x occlusion), which is the one that runs; and the blur REPLACING it
// (blend 0), for the view that shows the occlusion alone. Each takes as much of the table as it reads (FinalPass.h). No
// sampler: every read is a Load of one texel.
UBOOL FFinalPass::InitAo(ID3D12Device* Device)
{
    return BuildPassPipeline(Device, GFinalAoDepthShader, "AmbientOcclusionDepth", 6, RdxMath::AoCbCount, 0,
                             DXGI_FORMAT_R32_FLOAT, 0, TEXT("ambient occlusion, small depth"), AoDepthRoot,
                             AoDepthPipeline) &&
           BuildPassPipeline(Device, GFinalAoShader, "AmbientOcclusion", 3, RdxMath::AoCbCount, 0,
                             DXGI_FORMAT_R8_UNORM, 0, TEXT("ambient occlusion"), AoRoot, AoPipeline) &&
           BuildPassPipeline(Device, GFinalAoBlurShader, "AmbientOcclusionBlur", 4, RdxMath::AoCbCount, 0,
                             DXGI_FORMAT_R8_UNORM, 1, TEXT("ambient occlusion, into the factor"), AoBlurRoot,
                             AoBlurPipeline) &&
           BuildPassPipeline(Device, GFinalAoBlurShader, "AmbientOcclusionBlur", 4, RdxMath::AoCbCount, 0,
                             DXGI_FORMAT_R8_UNORM, 0, TEXT("ambient occlusion, alone"), AoAloneRoot, AoAlonePipeline);
}

void FFinalPass::DrawAoDepth(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                             D3D12_GPU_DESCRIPTOR_HANDLE Table, const UINT* Constants)
{
    if (!AoDepthPipeline || !Heap || !Constants)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(AoDepthRoot.Get());
    Cmd->SetPipelineState(AoDepthPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, Table);
    Cmd->SetGraphicsRoot32BitConstants(1, RdxMath::AoCbCount, Constants, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

void FFinalPass::DrawAo(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE Table,
                        const UINT* Constants)
{
    if (!AoPipeline || !Heap || !Constants)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(AoRoot.Get());
    Cmd->SetPipelineState(AoPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, Table);
    Cmd->SetGraphicsRoot32BitConstants(1, RdxMath::AoCbCount, Constants, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

void FFinalPass::DrawAoBlur(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                            D3D12_GPU_DESCRIPTOR_HANDLE Table, const UINT* Constants, UBOOL bAlone)
{
    ID3D12RootSignature* const Root     = bAlone ? AoAloneRoot.Get() : AoBlurRoot.Get();
    ID3D12PipelineState* const Pipeline = bAlone ? AoAlonePipeline.Get() : AoBlurPipeline.Get();
    if (!Root || !Pipeline || !Heap || !Constants)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(Root);
    Cmd->SetPipelineState(Pipeline);
    Cmd->SetGraphicsRootDescriptorTable(0, Table);
    Cmd->SetGraphicsRoot32BitConstants(1, RdxMath::AoCbCount, Constants, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

// Five views of the occlusion's table (t0-t4), its constants, into the R8 shadow factor, replacing it.
UBOOL FFinalPass::InitRtFactorBlur(ID3D12Device* Device)
{
    return BuildPassPipeline(Device, GFinalRtFactorBlurShader, "RtShadowFactorBlur", 5, RdxMath::AoCbCount, 0,
                             DXGI_FORMAT_R8_UNORM, 0, TEXT("RT shadow factor, smoothed"), RtFactorBlurRoot,
                             RtFactorBlurPipeline);
}

void FFinalPass::DrawRtFactorBlur(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                                  D3D12_GPU_DESCRIPTOR_HANDLE Table, const UINT* Constants)
{
    if (!RtFactorBlurPipeline || !Heap || !Constants)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(RtFactorBlurRoot.Get());
    Cmd->SetPipelineState(RtFactorBlurPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, Table);
    Cmd->SetGraphicsRoot32BitConstants(1, RdxMath::AoCbCount, Constants, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

UBOOL FFinalPass::InitDepth(ID3D12Device* Device)
{
    return BuildFinalPipeline(Device, GFinalDepthShader, "FinalPassDepth", 4, Colours.BackBuffer,
                              TEXT("final pass (depth view)"),
                              DepthRootSignature, DepthPipeline);
}

// The caption (FBackend::ShowCaption): a dark box, and in it the characters' dots - one dot of margin, six dots a
// character (the sixth is the gap), seven rows. Two values a character in G, as RdxMath::CaptionGlyph packs them.
static const char* GFinalCaptionShader =
"cbuffer Caption : register(b0)\n"
"{\n"
"    uint OriginX; uint OriginY; uint Scale; uint Count;\n"
"    uint4 G[15];                 // two values a character, at most 30: a root signature holds 64 values\n"
"};\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"float4 PSMain(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    float4 Back = float4(0.0f, 0.0f, 0.0f, 0.6f);\n"
"    int2 D = (int2(Pos.xy) - int2(OriginX, OriginY)) / int(max(Scale, 1u)) - 1;\n"
"    if (D.x < 0 || D.y < 0 || D.y > 6)\n"
"        return Back;\n"
"    uint K = uint(D.x) / 6u;\n"
"    uint X = uint(D.x) % 6u;\n"
"    if (X > 4u || K >= Count)\n"
"        return Back;\n"
"    uint Bit = uint(D.y) * 5u + X;\n"
"    // The character's two values, picked out with CONSTANT indices and a compare, never G[K / 2]. The pixels of one\n"
"    // wave can belong to two characters, and a register index that differs across a wave is not safe on these root\n"
"    // constants: one register is read for the whole wave, and the first dot of a character comes out as a sliver of\n"
"    // its neighbour's - a stray bar beside a letter, a bent S.\n"
"    uint Lo = 0u, Hi = 0u;\n"
"    [unroll] for (uint i = 0u; i < 15u; i++)\n"
"    {\n"
"        if (i == K / 2u)\n"
"        {\n"
"            Lo = ((K & 1u) != 0u) ? G[i].z : G[i].x;\n"
"            Hi = ((K & 1u) != 0u) ? G[i].w : G[i].y;\n"
"        }\n"
"    }\n"
"    uint Lit = (Bit < 20u) ? ((Lo >> Bit) & 1u) : ((Hi >> (Bit - 20u)) & 1u);\n"
"    return (Lit != 0u) ? float4(0.95f, 0.95f, 0.95f, 1.0f) : Back;\n"
"}\n";

// 64 = RdxMath::CaptionConstants, the most a root signature holds (D3D12 ERROR #699 with 68).
UBOOL FFinalPass::InitCaption(ID3D12Device* Device)
{
    return BuildPassPipeline(Device, GFinalCaptionShader, "Caption", 0, 64, 0, Colours.BackBuffer, 2,
                             TEXT("caption"), CaptionRoot, CaptionPipeline);
}

void FFinalPass::DrawCaption(ID3D12GraphicsCommandList* Cmd, const UINT* Constants)
{
    if (!CaptionPipeline)
        return;
    Cmd->SetGraphicsRootSignature(CaptionRoot.Get());
    Cmd->SetPipelineState(CaptionPipeline.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, 64, Constants, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

void FFinalPass::Shutdown()
{
    WaterStampPipeline.Release();
    WaterStampRoot.Release();
    DepthNearerPipeline[0].Release();
    DepthNearerPipeline[1].Release();
    DepthNearerRoot.Release();
    // The post filters' and the multisampled depth's too: InitFxaa and InitSmaa return at once while theirs stand, and
    // a change of the colour format (FBackend::RebuildForColours) counts on finding none here.
    FxaaPipeline.Release();
    FxaaRoot.Release();
    SmaaEdgePipeline.Release();
    SmaaEdgeRoot.Release();
    SmaaBlendPipeline.Release();
    SmaaBlendRoot.Release();
    MsaaDepthPipeline.Release();
    MsaaDepthRoot.Release();
    WaterStampFormat = DXGI_FORMAT_UNKNOWN;
    MotionViewPipeline.Release();
    MotionViewRoot.Release();
    MotionPipeline.Release();
    MotionRoot.Release();
    MotionObjPipeline.Release();
    MotionObjVertsPipeline.Release();
    MotionObjRoot.Release();
    MotionObjFormat = DXGI_FORMAT_UNKNOWN;
    LinearPipeline.Release();
    LinearRoot.Release();
    CaptionPipeline.Release();
    CaptionRoot.Release();
    ShadowApplyPipeline.Release();
    ShadowApplyRoot.Release();
    WaterPackPipeline.Release();
    WaterPackRoot.Release();
    ShadowFactorPipeline.Release();
    ShadowFactorRoot.Release();
    AoDepthPipeline.Release();
    AoDepthRoot.Release();
    AoPipeline.Release();
    AoRoot.Release();
    AoBlurPipeline.Release();
    AoBlurRoot.Release();
    AoAlonePipeline.Release();
    AoAloneRoot.Release();
    RtFactorBlurPipeline.Release();
    RtFactorBlurRoot.Release();
    DepthPipeline.Release();
    DepthRootSignature.Release();
    HdrPipeline.Release();
    HdrRootSignature.Release();
    BoxPipeline.Release();
    BoxRootSignature.Release();
    Pipeline.Release();
    RootSignature.Release();
}

void FFinalPass::Draw(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                      D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv, FLOAT Exponent)
{
    if (!Pipeline || !Heap)
        return;

    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(RootSignature.Get());
    Cmd->SetPipelineState(Pipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, SceneSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 1, &Exponent, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

void FFinalPass::DrawBox(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                         D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv, FLOAT Exponent, INT Scale,
                         UBOOL bCurve, UBOOL bLinear)
{
    if (!BoxPipeline || !Heap)
        return;

    // b0 is { float Exponent; uint Scale; uint Curve; uint LinearMix; }: the float goes in as its bits.
    UINT Values[4];
    appMemcpy(&Values[0], &Exponent, sizeof(FLOAT));
    Values[1] = (UINT)((Scale > 1) ? Scale : 1);
    Values[2] = bCurve  ? 1 : 0;
    Values[3] = bLinear ? 1 : 0;

    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(BoxRootSignature.Get());
    Cmd->SetPipelineState(BoxPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, SceneSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 4, Values, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

// FXAA: one pass, one texture, four constants (1/width, 1/height, the span, a pad). The linear sampler is what lets
// it read between pixels, which is where the smoothing comes from.
// ---------------------------------------------------------------------------
// The MSAA depth resolve
//
// Colour, light and the added target come down to one sample in hardware (ID3D12GraphicsCommandList::ResolveSubresource).
// The depth cannot: D3D12 resolves colour formats only. So this pass reads the multisampled depth as a texture and
// writes sample 0 into an ordinary depth buffer through SV_Depth - and with the shadows on it carries the surface kind
// over as well, through SV_StencilRef, which is the one thing here that needs a capability of the card
// (PSSpecifiedStencilRef; where it is missing, the MSAA goes and the shadows stay - the rule).
static const char* const GFinalMsaaDepthShader =
"// The MSAA depth resolve: sample 0 of the multisampled depth into a single-sample depth buffer, and with the\n// shadows on the surface kind beside it, through SV_StencilRef.\n//\n// Sample 0 and not an average: a depth is a distance, and the average of two distances is a place where nothing\n// is. Every renderer that resolves depth picks one sample; the shadow pass reads this to put a pixel back into the\n// world, and a made-up distance would put it inside the wall behind the edge.\nTexture2DMS<float>  MsDepth: register(t0);\nTexture2DMS<uint2>  MsStencil : register(t1);\n\nfloat4 VSMain(uint Id : SV_VertexID) : SV_Position\n{\n    float2 P = float2((Id << 1) & 2, Id & 2);\n    return float4(P * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n}\n\nvoid PSMain(float4 Pos : SV_Position, out float OutDepth : SV_Depth\n#if RDX_STENCIL\n, out uint OutRef : SV_StencilRef\n#endif\n           )\n{\n    int2 P = int2(Pos.xy);\n    OutDepth = MsDepth.Load(P, 0).r;\n#if RDX_STENCIL\n    OutRef = MsStencil.Load(P, 0).g;\n#endif\n}";

UBOOL FFinalPass::InitMsaaDepth(ID3D12Device* Device, INT Samples, UBOOL bStencil)
{
    // Made for one depth format: with the stencil beside the depth, or the depth alone. The shadows switched in the
    // running game change that format, and the pass is made again for it - kept, every frame's resolve is refused
    // (D3D12 ERROR #615), and nothing that reads the world's depth has one.
    if (MsaaDepthPipeline && MsaaDepthStencil == (bStencil ? 1 : 0))
        return 1;
    MsaaDepthPipeline.Release();
    MsaaDepthRoot.Release();
    MsaaDepthStencil = -1;
    if (Samples <= 1)
        return 0;

    D3D12_DESCRIPTOR_RANGE Range;
    appMemzero(&Range, sizeof(Range));
    Range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors     = 2;                 // the depth, and its stencil plane
    Range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER Param;
    appMemzero(&Param, sizeof(Param));
    Param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Param.DescriptorTable.NumDescriptorRanges = 1;
    Param.DescriptorTable.pDescriptorRanges   = &Range;
    Param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC RootDesc;
    appMemzero(&RootDesc, sizeof(RootDesc));
    RootDesc.NumParameters = 1;
    RootDesc.pParameters   = &Param;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), MsaaDepthRoot.VoidSlot())))
    {
        RdxLog(TEXT("MSAA depth resolve: the root signature could not be created"));
        return 0;
    }

    // SV_StencilRef only exists from shader model 5.1 - hence ps_5_1 here, where the rest of the renderer's passes
    // are happy with 5.0.
    const D3D_SHADER_MACRO Macros[2] = { { "RDX_STENCIL", bStencil ? "1" : "0" }, { nullptr, nullptr } };
    TComRef<ID3DBlob> Vs, Ps;
    const SIZE_T Len = strlen(GFinalMsaaDepthShader);
    if (FAILED(RdxCompile(GFinalMsaaDepthShader, Len, "MsaaDepth", Macros, nullptr, "VSMain", "vs_5_1",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GFinalMsaaDepthShader, Len, "MsaaDepth", Macros, nullptr, "PSMain", "ps_5_1",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("MSAA depth resolve: shader failed to compile: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature        = MsaaDepthRoot.Get();
    Pso.VS.pShaderBytecode    = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength     = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode    = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength     = Ps->GetBufferSize();
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 0;                // depth only
    Pso.DSVFormat             = bStencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    Pso.SampleDesc.Count      = 1;                // the DESTINATION is single-sample; the source is a texture here
    Pso.SampleMask            = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = FALSE;  // the shader writes SV_Depth; nothing may clip it away
    // Write every pixel's depth, and compare nothing: this is a copy, not a draw.
    Pso.DepthStencilState.DepthEnable    = TRUE;
    Pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    Pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;
    if (bStencil)
    {
        Pso.DepthStencilState.StencilEnable    = TRUE;
        Pso.DepthStencilState.StencilReadMask  = 0xFF;
        Pso.DepthStencilState.StencilWriteMask = 0xFF;
        Pso.DepthStencilState.FrontFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
        Pso.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        Pso.DepthStencilState.FrontFace.StencilPassOp      = D3D12_STENCIL_OP_REPLACE;   // with SV_StencilRef
        Pso.DepthStencilState.FrontFace.StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS;
        Pso.DepthStencilState.BackFace = Pso.DepthStencilState.FrontFace;
    }

    if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), MsaaDepthPipeline.VoidSlot())))
    {
        RdxLog(TEXT("MSAA depth resolve: the pipeline could not be created (%i samples, stencil %s)"),
               Samples, bStencil ? TEXT("yes") : TEXT("no"));
        return 0;
    }
    MsaaDepthStencil = bStencil ? 1 : 0;
    return 1;
}

void FFinalPass::DrawMsaaDepth(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                               D3D12_GPU_DESCRIPTOR_HANDLE Srv)
{
    if (!MsaaDepthPipeline || !Heap)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(MsaaDepthRoot.Get());
    Cmd->SetPipelineState(MsaaDepthPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, Srv);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

// What was drawn after the world, by its own depth, into the depth bound: where it holds anything, the depth test
// keeps the nearer of the two (the far plane is 0).
static const char* const GFinalDepthNearerShader =
"Texture2D<float> After : register(t0);\n"
"\n"
"float4 VSMain(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
"}\n"
"\n"
"float PSMain(float4 Pos : SV_Position) : SV_Depth\n"
"{\n"
"    float D = After.Load(int3(int2(Pos.xy), 0));\n"
"    if (D <= 0.0)\n"
"        discard;\n"
"    return D;\n"
"}\n";

UBOOL FFinalPass::InitDepthNearer(ID3D12Device* Device)
{
    if (DepthNearerRoot)
        return (DepthNearerPipeline[0] || DepthNearerPipeline[1]) ? 1 : 0;

    D3D12_DESCRIPTOR_RANGE Range;
    appMemzero(&Range, sizeof(Range));
    Range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors     = 1;
    Range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER Param;
    appMemzero(&Param, sizeof(Param));
    Param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Param.DescriptorTable.NumDescriptorRanges = 1;
    Param.DescriptorTable.pDescriptorRanges   = &Range;
    Param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC RootDesc;
    appMemzero(&RootDesc, sizeof(RootDesc));
    RootDesc.NumParameters = 1;
    RootDesc.pParameters   = &Param;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), DepthNearerRoot.VoidSlot())))
    {
        RdxLog(TEXT("depth after the world: the root signature could not be created"));
        return 0;
    }

    TComRef<ID3DBlob> Vs, Ps;
    const SIZE_T Len = strlen(GFinalDepthNearerShader);
    if (FAILED(RdxCompile(GFinalDepthNearerShader, Len, "DepthNearer", nullptr, nullptr, "VSMain", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GFinalDepthNearerShader, Len, "DepthNearer", nullptr, nullptr, "PSMain", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("depth after the world: the shader failed to compile: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature        = DepthNearerRoot.Get();
    Pso.VS.pShaderBytecode    = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength     = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode    = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength     = Ps->GetBufferSize();
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 0;                // depth only
    Pso.SampleDesc.Count      = 1;
    Pso.SampleMask            = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = FALSE;  // the shader writes SV_Depth; nothing may clip it away
    // Reversed-Z: the nearer is the greater.
    Pso.DepthStencilState.DepthEnable    = TRUE;
    Pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    Pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_GREATER;
    for (INT k = 0; k < 2; k++)
    {
        Pso.DSVFormat = (k == 1) ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
        if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState),
                                                       DepthNearerPipeline[k].VoidSlot())))
            RdxLog(TEXT("depth after the world: the pipeline could not be created (%s)"),
                   (k == 1) ? TEXT("depth and stencil") : TEXT("depth alone"));
    }
    return (DepthNearerPipeline[0] || DepthNearerPipeline[1]) ? 1 : 0;
}

void FFinalPass::DrawDepthNearer(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                                 D3D12_GPU_DESCRIPTOR_HANDLE Srv, UBOOL bStencil)
{
    ID3D12PipelineState* const Pipe = DepthNearerPipeline[bStencil ? 1 : 0].Get();
    if (!Pipe || !Heap)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(DepthNearerRoot.Get());
    Cmd->SetPipelineState(Pipe);
    Cmd->SetGraphicsRootDescriptorTable(0, Srv);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

UBOOL FFinalPass::InitFxaa(ID3D12Device* Device)
{
    if (FxaaPipeline)
        return 1;
    return BuildPassPipeline(Device, GFinalFxaaShader, "FinalFxaa", 1, 4, 1, Colours.BackBuffer, 0,
                             TEXT("antialiasing (FXAA)"), FxaaRoot, FxaaPipeline);
}

void FFinalPass::DrawFxaa(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                          D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv, FLOAT InvW, FLOAT InvH, FLOAT Span)
{
    if (!FxaaPipeline || !Heap)
        return;
    const FLOAT Values[4] = { InvW, InvH, Span, 0.0f };
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(FxaaRoot.Get());
    Cmd->SetPipelineState(FxaaPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, SceneSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 4, Values, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

// SMAA: two pipelines. The edge pass writes an R8G8 picture, the blend pass reads the colour AND that picture -
// hence two SRVs in its table, in that order.
UBOOL FFinalPass::InitSmaa(ID3D12Device* Device)
{
    if (SmaaEdgePipeline && SmaaBlendPipeline)
        return 1;
    if (!SmaaEdgePipeline &&
        !BuildPassPipeline(Device, GFinalSmaaEdgeShader, "FinalSmaaEdge", 1, 4, 1, DXGI_FORMAT_R8G8_UNORM, 0,
                           TEXT("antialiasing (SMAA, edges)"), SmaaEdgeRoot, SmaaEdgePipeline))
        return 0;
    if (!SmaaBlendPipeline &&
        !BuildPassPipeline(Device, GFinalSmaaBlendShader, "FinalSmaaBlend", 2, 4, 1, Colours.BackBuffer, 0,
                           TEXT("antialiasing (SMAA, blend)"), SmaaBlendRoot, SmaaBlendPipeline))
        return 0;
    return 1;
}

void FFinalPass::DrawSmaaEdges(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                               D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv, FLOAT InvW, FLOAT InvH, FLOAT Span,
                               FLOAT Threshold)
{
    if (!SmaaEdgePipeline || !Heap)
        return;
    const FLOAT Values[4] = { InvW, InvH, Span, Threshold };
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(SmaaEdgeRoot.Get());
    Cmd->SetPipelineState(SmaaEdgePipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, SceneSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 4, Values, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

void FFinalPass::DrawSmaaBlend(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                               D3D12_GPU_DESCRIPTOR_HANDLE PairSrv, FLOAT InvW, FLOAT InvH, FLOAT Span,
                               FLOAT Threshold)
{
    if (!SmaaBlendPipeline || !Heap)
        return;
    const FLOAT Values[4] = { InvW, InvH, Span, Threshold };
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(SmaaBlendRoot.Get());
    Cmd->SetPipelineState(SmaaBlendPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, PairSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 4, Values, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

UBOOL FFinalPass::InitUpscale(ID3D12Device* Device)
{
    if (!LinearPipeline &&
        !BuildPassPipeline(Device, GUpscaleLinearShader, "UpscaleLinear", 1, 2, 1, Colours.BackBuffer, 0,
                           TEXT("upscaling (bilinear)"), LinearRoot, LinearPipeline))
        return 0;
    return 1;
}

void FFinalPass::DrawLinear(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv,
                            FLOAT InvW, FLOAT InvH)
{
    if (!LinearPipeline || !Heap)
        return;

    const FLOAT Values[2] = { InvW, InvH };
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(LinearRoot.Get());
    Cmd->SetPipelineState(LinearPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, SceneSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 2, Values, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

UBOOL FFinalPass::InitMotion(ID3D12Device* Device)
{
    if (MotionPipeline)
        return 1;
    return BuildPassPipeline(Device, GFinalMotionShader, "Motion", 1, 32, 0, DXGI_FORMAT_R16G16_FLOAT, 0,
                             TEXT("motion vectors"), MotionRoot, MotionPipeline);
}

void FFinalPass::DrawMotion(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                            D3D12_GPU_DESCRIPTOR_HANDLE DepthSrv, const FLOAT* Constants)
{
    if (!MotionPipeline || !Heap || !Constants)
        return;

    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(MotionRoot.Get());
    Cmd->SetPipelineState(MotionPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, DepthSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 32, Constants, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

UBOOL FFinalPass::InitWaterStamp(ID3D12Device* Device, DXGI_FORMAT DsvFormat)
{
    if (WaterStampPipeline && WaterStampFormat == DsvFormat)
        return 1;
    WaterStampPipeline.Release();
    WaterStampFormat = DXGI_FORMAT_UNKNOWN;
    if (!WaterStampRoot)
    {
        D3D12_DESCRIPTOR_RANGE Range;
        appMemzero(&Range, sizeof(Range));
        Range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Range.NumDescriptors     = 1;
        Range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER Param;
        appMemzero(&Param, sizeof(Param));
        Param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Param.DescriptorTable.NumDescriptorRanges = 1;
        Param.DescriptorTable.pDescriptorRanges   = &Range;
        Param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC RootDesc;
        appMemzero(&RootDesc, sizeof(RootDesc));
        RootDesc.NumParameters = 1;
        RootDesc.pParameters   = &Param;
        TComRef<ID3DBlob> Blob, Error;
        if (FAILED(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
            FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                               __uuidof(ID3D12RootSignature), WaterStampRoot.VoidSlot())))
        {
            RdxLog(TEXT("water depth for the upscaler: root signature failed"));
            return 0;
        }
    }

    TComRef<ID3DBlob> Vs, Ps, Error;
    const SIZE_T Len = strlen(GFinalWaterStampShader);
    if (FAILED(RdxCompile(GFinalWaterStampShader, Len, "WaterStamp", nullptr, nullptr, "VSMain", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GFinalWaterStampShader, Len, "WaterStamp", nullptr, nullptr, "PSMain", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("water depth for the upscaler: shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature        = WaterStampRoot.Get();
    Pso.VS.pShaderBytecode    = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength     = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode    = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength     = Ps->GetBufferSize();
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 0;
    Pso.DSVFormat             = DsvFormat;
    Pso.SampleDesc.Count      = 1;
    Pso.SampleMask            = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = TRUE;
    // Reversed depth (z/w = 1/W): nearer is greater. The water was only written where it was in front, so this
    // passes wherever there is water - held to it all the same, so nothing already nearer is ever pushed back.
    Pso.DepthStencilState.DepthEnable    = TRUE;
    Pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    Pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), WaterStampPipeline.VoidSlot())))
    {
        RdxLog(TEXT("water depth for the upscaler: pipeline state failed"));
        return 0;
    }
    WaterStampFormat = DsvFormat;
    return 1;
}

void FFinalPass::DrawWaterStamp(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                                D3D12_GPU_DESCRIPTOR_HANDLE WaterSrv)
{
    if (!WaterStampPipeline || !Heap)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(WaterStampRoot.Get());
    Cmd->SetPipelineState(WaterStampPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, WaterSrv);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

// The per-body motion pass (GFinalMotionObjShader). Unlike every other pass here it draws GEOMETRY: a vertex buffer of
// world-space positions, depth-tested against the scene's own depth so that only what is really seen is written - and never
// writing depth. The constants (48 values: the two cameras, the sizes, the jitter, the body's own 3x4 and the share
// nearer) fit in the root signature, so there is no buffer to keep. bNoDepthTest: a measurement's - every body whole,
// seen or not.
UBOOL FFinalPass::InitMotionObjects(ID3D12Device* Device, DXGI_FORMAT DsvFormat, UBOOL bNoDepthTest)
{
    // The depth's format is not the same with the shadows on and off: both pipelines are made again for another.
    if (MotionObjPipeline && MotionObjVertsPipeline && MotionObjFormat == DsvFormat)
        return 1;
    MotionObjPipeline.Release();
    MotionObjVertsPipeline.Release();
    MotionObjFormat = DXGI_FORMAT_UNKNOWN;

    TComRef<ID3DBlob> Blob, Error;
    if (!MotionObjRoot)
    {
        D3D12_ROOT_PARAMETER Param;
        appMemzero(&Param, sizeof(Param));
        Param.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        Param.Constants.ShaderRegister = 0;
        Param.Constants.Num32BitValues = MOTION_OBJ_CONSTANTS;
        Param.ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC RootDesc;
        appMemzero(&RootDesc, sizeof(RootDesc));
        RootDesc.NumParameters = 1;
        RootDesc.pParameters   = &Param;
        RootDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        if (FAILED(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
            FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                               __uuidof(ID3D12RootSignature), MotionObjRoot.VoidSlot())))
        {
            RdxLog(TEXT("motion vectors (objects): root signature failed"));
            return 0;
        }
    }

    TComRef<ID3DBlob> Vs, VsVerts, Ps;
    const SIZE_T Len = strlen(GFinalMotionObjShader);
    if (FAILED(RdxCompile(GFinalMotionObjShader, Len, "MotionObj", nullptr, nullptr, "VSMain", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GFinalMotionObjShader, Len, "MotionObj", nullptr, nullptr, "VSMainVerts", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, VsVerts.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GFinalMotionObjShader, Len, "MotionObj", nullptr, nullptr, "PSMain", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("motion vectors (objects): shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    // The second element is VSMainVerts' alone: where each vertex was, from a stream of its own.
    D3D12_INPUT_ELEMENT_DESC Layout[2];
    appMemzero(Layout, sizeof(Layout));
    Layout[0].SemanticName = "POSITION";
    Layout[0].Format       = DXGI_FORMAT_R32G32B32_FLOAT;
    Layout[1].SemanticName = "PREVPOS";
    Layout[1].Format       = DXGI_FORMAT_R32G32B32_FLOAT;
    Layout[1].InputSlot    = 1;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature        = MotionObjRoot.Get();
    Pso.VS.pShaderBytecode    = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength     = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode    = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength     = Ps->GetBufferSize();
    Pso.InputLayout.pInputElementDescs = Layout;
    Pso.InputLayout.NumElements        = 1;
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 1;
    Pso.RTVFormats[0]         = DXGI_FORMAT_R16G16_FLOAT;
    Pso.DSVFormat             = DsvFormat;
    Pso.SampleDesc.Count      = 1;
    Pso.SampleMask            = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = TRUE;
    Pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // Reversed depth (z/w = 1/W): nearer is greater, as in FSolidPass. Never written - the scene's depth stays as it is.
    Pso.DepthStencilState.DepthEnable    = TRUE;
    Pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    Pso.DepthStencilState.DepthFunc      = bNoDepthTest ? D3D12_COMPARISON_FUNC_ALWAYS
                                                        : D3D12_COMPARISON_FUNC_GREATER_EQUAL;

    if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), MotionObjPipeline.VoidSlot())))
    {
        RdxLog(TEXT("motion vectors (objects): pipeline state failed"));
        return 0;
    }
    Pso.VS.pShaderBytecode      = VsVerts->GetBufferPointer();
    Pso.VS.BytecodeLength       = VsVerts->GetBufferSize();
    Pso.InputLayout.NumElements = 2;
    if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState),
                                                   MotionObjVertsPipeline.VoidSlot())))
    {
        RdxLog(TEXT("motion vectors (objects): pipeline state failed (the one with a place for every vertex)"));
        MotionObjPipeline.Release();
        return 0;
    }
    MotionObjFormat = DsvFormat;
    return 1;
}

// One body: its constants, its vertices where they already lie (the shadow maps' own buffer or arena, or the motion
// buffer), and nothing else.
void FFinalPass::DrawMotionObject(ID3D12GraphicsCommandList* Cmd, const D3D12_VERTEX_BUFFER_VIEW& Vb,
                                  INT First, INT Count, const FLOAT* Constants)
{
    if (!MotionObjPipeline || !Constants || Count < 3)
        return;
    Cmd->SetGraphicsRootSignature(MotionObjRoot.Get());
    Cmd->SetPipelineState(MotionObjPipeline.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, MOTION_OBJ_CONSTANTS, Constants, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->IASetVertexBuffers(0, 1, &Vb);
    Cmd->DrawInstanced((UINT)Count, 1, (UINT)First, 0);
}

// One body with where each of its vertices was: both views begin at its first vertex, Count vertices each. The body's
// own 3x4 in the constants is not read.
void FFinalPass::DrawMotionObjectVerts(ID3D12GraphicsCommandList* Cmd, const D3D12_VERTEX_BUFFER_VIEW& Now,
                                       const D3D12_VERTEX_BUFFER_VIEW& Before, INT Count, const FLOAT* Constants)
{
    if (!MotionObjVertsPipeline || !Constants || Count < 3)
        return;
    const D3D12_VERTEX_BUFFER_VIEW Views[2] = { Now, Before };
    Cmd->SetGraphicsRootSignature(MotionObjRoot.Get());
    Cmd->SetPipelineState(MotionObjVertsPipeline.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, MOTION_OBJ_CONSTANTS, Constants, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->IASetVertexBuffers(0, 2, Views);
    Cmd->DrawInstanced((UINT)Count, 1, 0, 0);
}

UBOOL FFinalPass::InitMotionView(ID3D12Device* Device)
{
    if (MotionViewPipeline)
        return 1;
    return BuildPassPipeline(Device, GFinalMotionViewShader, "MotionView", 1, 4, 0, Colours.BackBuffer, 0,
                             TEXT("motion vector view"), MotionViewRoot, MotionViewPipeline);
}

void FFinalPass::DrawMotionView(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                                D3D12_GPU_DESCRIPTOR_HANDLE MotionSrv, FLOAT Scale, FLOAT ScaleX, FLOAT ScaleY)
{
    if (!MotionViewPipeline || !Heap)
        return;

    const FLOAT Values[4] = { Scale, ScaleX, ScaleY, 0.0f };
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(MotionViewRoot.Get());
    Cmd->SetPipelineState(MotionViewPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, MotionSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 4, Values, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

void FFinalPass::DrawDepth(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                           D3D12_GPU_DESCRIPTOR_HANDLE DepthSrv, INT Mode, INT Scale, FLOAT TanX, FLOAT TanY)
{
    if (!DepthPipeline || !Heap)
        return;

    // b0 is { uint Mode; uint Scale; float TanX; float TanY; }: the floats go in as their bits.
    UINT Values[4];
    Values[0] = (UINT)Mode;
    Values[1] = (UINT)((Scale > 1) ? Scale : 1);
    appMemcpy(&Values[2], &TanX, sizeof(FLOAT));
    appMemcpy(&Values[3], &TanY, sizeof(FLOAT));

    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(DepthRootSignature.Get());
    Cmd->SetPipelineState(DepthPipeline.Get());
    Cmd->SetGraphicsRootDescriptorTable(0, DepthSrv);
    Cmd->SetGraphicsRoot32BitConstants(1, 4, Values, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

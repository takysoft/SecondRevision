// SecondRevision - the plants' own motion vectors (Grass3D, Trees3D): what the grass's and the trees' shaders share for
// the motion vectors' pass (FSolidPass::DrawPlantMotion). The camera's vector alone would have a temporal upscaler
// take a blade or a leaf in the wind for still, and smear it: so each is laid again, at this frame's time and at the
// frame before's, its place now and then into the two cameras - as the bodies' pass has them (FinalPass.cpp's
// GFinalMotionObjShader, the same 48 constants, here in b3 so as not to meet the geometry pass's own) - and the pixel's
// vector written where the plant is what the scene's depth holds. A leaf's blade keeps its picture's cut-out: where
// the leaf is not, the pixel keeps what is behind it.
//
// Plain ASCII, as the C strings they are.
#pragma once

#define RDX_PLANT_MOTION \
"cbuffer PlantMotion : register(b3)\n" \
"{\n" \
"    float3 MAxisX;     float MTanX;\n" \
"    float3 MAxisY;     float MTanY;\n" \
"    float3 MAxisZ;     float MSizeX;\n" \
"    float3 MEye;       float MSizeY;\n" \
"    float3 MPrevAxisX; float MPrevTanX;\n" \
"    float3 MPrevAxisY; float MPrevTanY;\n" \
"    float3 MPrevAxisZ; float MJitterX;\n" \
"    float3 MPrevEye;   float MJitterY;\n" \
"    float4 MBody[3];\n" \
"    float  MNearer;    float3 MSpare;\n" \
"};\n" \
"Texture2D    MotionPicture : register(t0);\n" \
"SamplerState MotionSampler : register(s0);\n" \
"\n" \
"struct MotionOut\n" \
"{\n" \
"    float4 Pos  : SV_Position;\n" \
"    float4 Prev : TEXCOORD0;   // where the point was, in the frame before's clip space\n" \
"    float4 Now  : TEXCOORD1;   // and where it is, without the jitter\n" \
"    float2 Uv   : TEXCOORD2;   // a leaf's place on its picture, for its cut-out\n" \
"};\n" \
"\n" \
"// P: where the point is now; Pp: where it was the frame before - both in the world. The scene's own projection, the\n" \
"// jitter in the clip position but not in the vector, the depth a share nearer for the test alone.\n" \
"MotionOut MotionBoth(float3 P, float3 Pp, float2 Uv)\n" \
"{\n" \
"    MotionOut O;\n" \
"    const float3 R  = P - MEye;\n" \
"    const float3 V  = float3(dot(R, MAxisX), dot(R, MAxisY), dot(R, MAxisZ));\n" \
"    const float  W  = max(V.z, 1e-4f);\n" \
"    O.Pos  = float4(V.x / MTanX + MJitterX * W, -V.y / MTanY + MJitterY * W, 1.0f + MNearer, W);\n" \
"    O.Now  = float4(V.x / MTanX, -V.y / MTanY, 1.0f, W);\n" \
"    const float3 Rp = Pp - MPrevEye;\n" \
"    const float3 Vp = float3(dot(Rp, MPrevAxisX), dot(Rp, MPrevAxisY), dot(Rp, MPrevAxisZ));\n" \
"    O.Prev = float4(Vp.x / MPrevTanX, -Vp.y / MPrevTanY, 1.0f, Vp.z);\n" \
"    O.Uv   = Uv;\n" \
"    return O;\n" \
"}\n" \
"MotionOut MotionNone()\n" \
"{\n" \
"    MotionOut O = (MotionOut)0;\n" \
"    O.Pos = float4(0.0f, 0.0f, -1.0f, 1.0f);\n" \
"    return O;\n" \
"}\n" \
"float4 MotionVector(MotionOut In)\n" \
"{\n" \
"    const float2 Size = float2(MSizeX, MSizeY);\n" \
"    const float2 Was  = ((In.Prev.xy / In.Prev.w) * float2(0.5f, -0.5f) + 0.5f) * Size;\n" \
"    const float2 Here = ((In.Now.xy / In.Now.w) * float2(0.5f, -0.5f) + 0.5f) * Size;\n" \
"    return float4(Was - Here, 0.0f, 0.0f);\n" \
"}\n" \
"// A blade's or the bark's vector; behind the frame before's camera there is nothing to say.\n" \
"float4 PSPlantMotion(MotionOut In) : SV_Target\n" \
"{\n" \
"    if (In.Prev.w <= 1e-4f)\n" \
"        discard;\n" \
"    return MotionVector(In);\n" \
"}\n" \
"// A leaf's: only where its picture has the leaf.\n" \
"float4 PSLeafMotion(MotionOut In) : SV_Target\n" \
"{\n" \
"    if (In.Prev.w <= 1e-4f || MotionPicture.Sample(MotionSampler, In.Uv).a < 0.5f)\n" \
"        discard;\n" \
"    return MotionVector(In);\n" \
"}\n"

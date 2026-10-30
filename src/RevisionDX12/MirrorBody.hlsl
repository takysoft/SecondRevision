// SecondRevision - the player's own body as the pane of glass the mirror probe stands before gives it back, where no
// rays run (Reflections=High; FMirrorBody; docs/tukrozodes-szonda-terv.md 9). First person never draws the body, and the
// probe holds no actor; the body's shape and skins are the shadows' (the caster marked as the own body). A flat pane's
// mirror image is the world mirrored by its plane and seen by the eye itself: so the body's vertices are mirrored by
// the pane's plane and drawn with the scene's own projection into a picture of the scene's size - its colour, and in
// alpha the view depth there, which on a flat mirror is the eye's path to the pane and on to the body. The glass's
// layer reads that picture where the pane is (SolidPass.hlsl, GlassMirror). Compiled at run time (vs_5_1, ps_5_1).
// Plain ASCII: the build refuses anything else (embed-text.ps1).

cbuffer BodyConsts : register(b0)
{
    float3 AxisX;  float TanX;      // the world view's axes in the world, and the scene's tangents (FFinalPass's
    float3 AxisY;  float TanY;      //   motion vectors take the same)
    float3 AxisZ;  float JitterX;   // and the temporal upscaler's jitter, in clip space
    float3 Eye;    float JitterY;
    float3 PlaneN; float PlaneD;    // the pane's plane in the world: dot(PlaneN, P) == PlaneD, PlaneN of length one
    float  Boost;                   // the lamps' strength (SolidPass.hlsl's LightBoost)
    uint   Engine;                  // 1: the caster's six ways hold the engine's own light on it (RtAlbedoEngineLit)
    uint   Average;                 // its skins' average colour (RdxMath::PackRtAlbedo), for a face whose skin is not known
    uint   Spare;
    uint4  Cube[2];                 // the light along +x -x +y -y +z -z, packed as RdxMath::PackRtLight (six used)
    uint4  Skins[4];                // the faces' textures by key: the texture heap's index plus one (RtCasterKeys used)
};
ByteAddressBuffer Positions : register(t0);     // the caster's vertices, three floats each, in the world
ByteAddressBuffer Places    : register(t1);     // and their texture places, u, v and the face's slot (CasterUvw)
Texture2D         Skin[]    : register(t0, space1);
SamplerState      Samp      : register(s0);

struct VSOut
{
    float4 Pos    : SV_Position;
    float2 Uv     : TEXCOORD0;
    float3 Normal : TEXCOORD1;      // mirrored, in the world
    float  Depth  : TEXCOORD2;      // the view depth
    nointerpolation uint Face : TEXCOORD3;      // twice the key plus one, and the own light's bit (PackCasterSlot)
};

// The packed light's three numbers (RdxMath::PackRtLight), as SolidPass.hlsl's RtLight.
float3 Light(uint V)
{
    return float3(float(V & 1023u), float((V >> 10) & 1023u), float((V >> 20) & 1023u)) * (4.0f / 1023.0f);
}
uint CubeWay(uint W)
{
    return Cube[W >> 2][W & 3u];
}
// A corner's normal from the fractions of its u and v (RdxMath::PackCasterPlace, UnpackOctNormal).
float3 PlaceNormal(float2 Uv)
{
    const float  Steps = float(RDX_RT_NORMAL_STEPS);
    const float2 F     = frac(Uv * 4096.0f);
    const float2 Oct   = floor(F * Steps + 0.5f);
    float2 N = Oct / (Steps - 1.0f) * 2.0f - 1.0f;
    const float Z = 1.0f - abs(N.x) - abs(N.y);
    if (Z < 0.0f)
        N = (1.0f - abs(N.yx)) * float2((N.x >= 0.0f) ? 1.0f : -1.0f, (N.y >= 0.0f) ? 1.0f : -1.0f);
    return normalize(float3(N, Z));
}

VSOut VSBody(uint Id : SV_VertexID)
{
    VSOut Out;
    const float3 P  = asfloat(Positions.Load3(Id * 12u));
    const float3 T  = asfloat(Places.Load3(Id * 12u));
    // The vertex mirrored by the pane's plane, then seen as the scene sees the world (FFinalPass's motion vectors).
    const float3 M  = P - 2.0f * (dot(PlaneN, P) - PlaneD) * PlaneN;
    const float3 R  = M - Eye;
    const float3 V  = float3(dot(R, AxisX), dot(R, AxisY), dot(R, AxisZ));
    const float  W  = max(V.z, 1e-4f);
    Out.Pos    = float4(V.x / TanX + JitterX * W, -V.y / TanY + JitterY * W, 1.0f, W);
    Out.Uv     = T.xy;
    const float3 N = PlaceNormal(T.xy);
    Out.Normal = N - 2.0f * dot(N, PlaneN) * PlaneN;
    Out.Depth  = V.z;
    Out.Face   = uint(frac(T.z) * 64.0f + 0.5f);
    return Out;
}

float4 PSBody(VSOut In) : SV_Target
{
    // The face's skin where the texture heap holds it, the skins' average colour otherwise - as the mirror rays show
    // an actor (SolidPass.hlsl).
    // A branch, not a choice of two values: both sides of a choice may be worked out, and a skin read with no skin
    // reads a view far past the heap's end - the card hangs on it.
    const uint Key  = (In.Face >> 1) & 15u;
    const uint Slot = (Key != 0u) ? Key - 1u : 0u;
    const uint Tex  = (Key != 0u) ? Skins[Slot >> 2][Slot & 3u] : 0u;
    float3 C = float3(float(Average & 255u), float((Average >> 8) & 255u), float((Average >> 16) & 255u)) / 255.0f;
    [branch] if (Tex != 0u)
        C = Skin[NonUniformResourceIndex(Tex - 1u)].Sample(Samp, In.Uv).rgb;
    // Its own light where the face carries one; otherwise the engine's own on the body by the way it faces, as the
    // mirror rays show an actor (SolidPass.hlsl), or white where the six ways know nothing yet.
    float3 L = float3(1.0f, 1.0f, 1.0f);
    if ((In.Face & 1u) == 0u && Engine != 0u)
    {
        const float3 N = normalize(In.Normal);
        L = (Light(CubeWay((N.x >= 0.0f) ? 0u : 1u)) * (N.x * N.x) +
             Light(CubeWay((N.y >= 0.0f) ? 2u : 3u)) * (N.y * N.y) +
             Light(CubeWay((N.z >= 0.0f) ? 4u : 5u)) * (N.z * N.z)) * Boost;
    }
    return float4(C.rgb * L, In.Depth);
}

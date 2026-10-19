// SecondRevision - the geometry pass's shaders (FSolidPass): the world's surfaces, the water, the screen tint and the
// water's splashes. One source, two builds:
//   - the renderer compiles it at run time with D3DCompile, from the text the build puts into the DLL (SolidPassText.h,
//     scripts/embed-text.ps1), with the macros FSolidPass::CreatePipelineState hands over;
//   - with RDX_RT=1 the build compiles the water's stages with dxc into DXIL (SolidPassRt*.h): the water whose mirror
//     image comes from rays traced against the scene (Reflections=RT, docs/terv-rt.md) - and the glass's mirror
//     layer the same way, with the pass's own vertex shader.
// Plain ASCII: the build refuses anything else (embed-text.ps1).

#if RDX_RT
// The run-time compile gets these as macros (FSolidPass::CreatePipelineState); dxc reads them from the C++ side's own
// header - the same numbers either way.
#include "RenderLimits.h"
#define RDX_LIGHT_SCALE   RDX_LIGHT_SCALE_VALUE
#define RDX_RIPPLE_WORLD  RDX_RIPPLE_WORLD_VALUE
#define RDX_RIPPLE_TEXELS RDX_RIPPLE_TEXELS_VALUE
#define RDX_WATER_EDGE    RDX_WATER_EDGE_VALUE
#endif

cbuffer Frame : register(b0)
{
    float2 InvHalfSize;   // 2/width, 2/height
    float2 Jitter;        // the sub-pixel shift a temporal upscaler asks for, in clip space; zero without one
    float  LightBoost;    // the lamps' strength, 1.0 untouched - see FBackend::SetLightBoost
    float  MipBias;       // the world's textures' mip bias under the temporal upscaler (FSolidPass::SetMipBias)
};
Texture2D    Tex: register(t0);
Texture2D    Light : register(t1);
SamplerState Samp: register(s0);
SamplerState SampPoint : register(s1);
// Per run: PF_NoSmooth, a screen tint's fog colour, and a water run's constants (FSolidPass::SetWaterConstants).
cbuffer RunConsts : register(b1)
{
    uint   NoSmooth;
    float3 FlashFog;
    float4 WaterTint;         // the water's hue, and the waves' strength
    float4 WaterView;         // the time, the engine's projection, the absorption, the shoreline's width
    float4 WaterCamera;       // the camera's world X, Y; the view's X axis in the world (X, Y)
    float4 WaterAxes;         // the view's Y axis and Z axis in the world (X, Y each)
    float4 WaterScreen;       // the screen's centre in the engine's pixels; the ripples' field's lower corner
    float4 WaterExtra;        // the camera's height above the surface (below it, negative); the Z of the view's axes
    float4 WaterWave;         // the texture's own waves: 0 none, else 1 + their slope spread (WaterSlopeSpread); their
                              // height; one repeat (U, V)
    float4 WaterDirs;         // the texture's U and V directions on the level
    float4 WaterSpare;        // 1 in x: the mirror image too (Reflections=High and up)
};

// t2..t5 are what a RUN binds besides its own texture, and what they hold depends on the run. A water run binds the
// picture behind it, the depth behind it, the ripples and the water texture's own wave field; a run with relief binds
// its height map. No surface is ever both, so the two share one table (FSolidPass::MakeRoot).
Texture2D SceneCopy : register(t2);     // with relief: the height map, 1 at the surface and 0 at its deepest
Texture2D DepthCopy : register(t3);     // not read by a relief run
Texture2D Ripples   : register(t4);     // x: the ripples' height in world units (FBackend::StepRipples)
Texture2D WaveField : register(t5);     // the water texture's own wave field, 0.5 the still surface (FBackend::SetWaterWaves)
// A water run's table has a fifth view, at t13: the caustics' net. No run reads it - it rides in the water's set for
// the pass that lays the caustics on the picture, which binds the set from its second view on (FBackend::LayCaustics).
// Geometry=Tessellation: the relief's covers - the depth of everything else the world draws, flat, taken just before
// the relief (FSolidPass::DrawReliefCovers): what may stand above a raised surface (ReliefClear). Read by DSRelief
// alone, through the root signature's own table, so it never meets a run's t2..t5. A view of nothing reads zero:
// nothing is in front of anything, and the relief is lifted as it always was.
Texture2D ReliefCover : register(t12);

// The water's own depth (FBackend's WaterSurface): the depth buffer's value of the nearest see-through water surface, for
// the motion vectors and the temporal upscaler, which would otherwise take whatever lies under the water - the floor, or
// the sky past the level - for the surface. The water writes no depth itself: what is drawn after it must still see
// what lies under it. Written where the run says it is bound for that (FTargetSwitch::WaterSurfaceView); the larger
// value is the nearer, and it is kept with an atomic - which is why it goes in as the float's bits.
// u3: in shader model 5.0 a pixel shader's UAVs share their numbers with its outputs, and the water writes three.
RWTexture2D<uint> WaterSurfaceDepth : register(u3);
#define WaterSurfaceOn WaterSpare.w
// 2 in the same number: the run was gone over first, and the depth holds the nearest of its points (PSWaterMark).
#define WaterSurfaceMarked (WaterSpare.w > 1.5f)
// With the shadows: what their apply pass needs of the water's pixel (FBackend::ApplyShadows), written beside its
// depth by the nearest of its points. x: the light the surface receives, as the light target has a surface's - the
// shadow factor of the surface's own place weighs a lamp's share against it; y: how much of what the shadows may take
// of the pixel is the bed seen through the water, the rest being the water's own light; zw: where in the picture that
// bed was seen, from the pixel itself - the bent view's place in the copy, whose shadow it takes. A view of nothing
// takes no write.
RWTexture2D<float4> WaterShade : register(u6);
static float4 GWaterShade = float4(0.0f, 0.0f, 0.0f, 0.0f);
// The RT mirror's measuring view 11: whether the place a ray met fell outside its surface's light map.
static bool GRtLightHeld = false;

// The temporal upscaler's jitter (Jitter, in clip space) as a shift in the picture's coordinates (0..1): the world is
// drawn this far from where the engine's projection puts it, and so it is found this far off in the copies of the picture.
float2 JitterUv()
{
    return float2(Jitter.x, -Jitter.y) * 0.5f;
}

// The relief's own four numbers ride in the last of the water's (FSolidPass's FRun::Pom): how deep it goes in the
// level's units, what is switched on, and one over the height map's size.
#define PomDepth   WaterSpare.x
#define PomFlags   WaterSpare.y
#define PomTexel   WaterSpare.zw
#define POM_ON          1.0f
// The texture is laid on this surface once, and the coordinates were moved into 0..1 by the CPU: the relief is held
// inside that, or the wrap brings the far side of the picture in at the edge.
#define POM_ONCE       32.0f
// How far the texture is ever allowed to slide, as a multiple of the relief's own depth. At the very edge the relief
// therefore looks this many times as deep as it does face on, and it gets there gradually - see PomWalk.
#define POM_SLIDE       4.0f
// Not the relief: a translucent run that fades out where it meets what is behind it. PomDepth is then how far in
// front of that the fade starts, in the level's units.
#define POM_SOFT        8.0f
// Geometry=Tessellation: the relief made real (VSRelief, HSRelief, DSRelief) - the surface is lifted out of the wall
// by the height map, and the pixel shader walks nothing.
#define POM_TESS       64.0f
// A relief run's own numbers, in the water's constants (FSolidPass::SetTessRun).
#define ReliefRate     WaterTint.xy     // texture repeats in one unit of the level, along U and along V
#define ReliefBevel    WaterTint.z      // the rim band's width in depths (FSolidPass::SetReliefBevel); 0: RELIEF_BEVEL
#define ReliefSmooth   WaterTint.w      // mip levels added to the height the patches are lifted by (SetReliefSmooth)
#define ReliefEdge     WaterView.x      // how long a cut edge may be, in the engine's pixels at its distance
#define ReliefFocal    WaterView.y      // the engine's projection, in its own pixels
#define ReliefCentre   WaterScreen.xy   // the screen's centre, in the engine's pixels
#define ReliefNormal   WaterDirs.xyz    // out of the surface, towards the eye, in view space
#define ReliefUvMap    WaterCamera      // the pixels' coordinate from the patches' own: U * x + z, V * y + w
#define ReliefAxisU    WaterAxes.xyz    // the texture's U and V directions along the surface, view space, unit
#define ReliefAxisV    WaterExtra.xyz
// The relief lit and shadowed by the lamps around it (ReliefLamps): its slopes turned towards them or away, and what
// of their light the relief itself stands in front of.
#define POM_LAMPS     128.0f
// A measurement view, marker only (SecondRevision-relief-keep.txt): the tessellated relief tinted red where DSRelief holds
// it down towards a held edge, the more the lower - a band along a seam between two surfaces shows as a red line - and
// blue where it goes round a bend, lifted along a direction the faces share.
#define POM_SHOWKEEP  256.0f
// How much of the baked light the lamps' directions reshape, 0..1: the rest of it is taken as coming from all around
// (the light the level bounces, which the relief shadows little), and stays as it was.
#define ReliefLampStrength WaterWave.x

// Soft particles. A smoke or steam sprite is a flat picture standing in the air, and where it cuts into the wall it
// ends in a straight line that has nothing to do with smoke. Fading it out as it nears whatever is behind it takes
// the line away - the same trick every engine uses, and it costs one depth read.
//
// The blend for these surfaces is src + dst * (1 - src): BLACK IS INVISIBLE. So the fade multiplies the colour
// rather than an alpha, which this pass does not carry.
float SoftFade(float4 Pos)
{
    // Reversed Z: the depth holds one over the view depth, and so does Pos.z here.
    const float Behind = 1.0f / max(DepthCopy.Load(int3(Pos.xy, 0)).r, 1e-6f);
    const float Here   = 1.0f / max(Pos.z, 1e-6f);
    // THE PLAYER'S OWN INTERFACE IS NOT A PARTICLE. The HUD, the menus and the inventory arrive at a view depth of
    // ONE, and this fade would then measure them against whatever the world left behind them - so standing close to
    // a wall, or inside steam, dimmed the interface itself. Nothing in the world is ever this near the eye.
    if (Here <= 2.0f)
        return 1.0f;
    // A floor. A soft particle should lose its HARD EDGE against the wall, not disappear into it: steam that
    // stands nearly parallel to a wall is within reach of it along its whole card, so without this the whole
    // sprite fades evenly and what is taken away is the steam rather than the line.
    const float Least = 0.35f;
    return Least + (1.0f - Least) * saturate((Behind - Here) / max(PomDepth, 1e-3f));
}

// The lamps a run is lit by, two rows a lamp: its place and its reach, then its colour. The water's (Reflections=High
// and up, FBackend::SetWaterLamps) have their place from the camera in world units; a relief run's (POM_LAMPS,
// FBackend::SetReliefLamps) have it in view space. The rows are as many as the relief's lamps can be
// (RDX_RELIEF_LAMP_MAX); the water fills eight of them at most. Geometry=Tessellation: the most held edges a relief run
// brings for its rim band (FSolidPass::SetReliefHeld).
#define RELIEF_HELD_MAX 64
cbuffer WaterLampConsts : register(b2)
{
    float4 WaterLampCount;
    float4 WaterLamp[2 * RDX_RELIEF_LAMP_MAX];
    // Geometry=Tessellation: the held edges near a relief run's polygons (FSolidPass::SetReliefHeld) - x is how many,
    // then three rows each: one end in view space with the share of the band it keeps (w), the other end, and its
    // shear (ReliefHeldAt). With none the rim band is the one the patches' corners carry.
    float4 ReliefHeldCount;
    float4 ReliefHeld[3 * RELIEF_HELD_MAX];
};

struct VSIn  { float3 Pos : POSITION; float2 Uv : TEXCOORD0; float2 Lv : TEXCOORD1; float4 Colour : COLOR;
               float4 Pom : TEXCOORD2; };
// View: the point's place in view space, for the relief's lamps (POM_LAMPS). Only a relief run's constants make it
// right, and only such a run reads it.
struct VSOut { float4 Pos : SV_Position; float2 Uv : TEXCOORD0; float2 Lv : TEXCOORD1;
               float4 Colour : COLOR; float4 Pom : TEXCOORD2; float3 View : TEXCOORD3; };

VSOut VSMain(VSIn In)
{
    VSOut Out;
    // The engine hands us vertices that are already projected to pixels, so there is
    // no matrix here - but the perspective divide still has to be undone and redone.
    // With w = 1 the rasteriser interpolates texture coordinates linearly in SCREEN
    // space, which is affine mapping: the texture warps on oblique surfaces and swims
    // as the camera moves. W must be the view-space depth, and Pos.z carries exactly
    // its reciprocal (docs/spec-03 section 4, point 2).
    float W = 1.0f / max(In.Pos.z, 1e-6f);
    Out.Pos.x = (In.Pos.x * InvHalfSize.x - 1.0f + Jitter.x) * W;
    Out.Pos.y = (1.0f - In.Pos.y * InvHalfSize.y + Jitter.y) * W;
    // Reversed-Z is unchanged: z/w = 1/W is the same depth value as before.
    Out.Pos.z = 1.0f;
    Out.Pos.w = W;
    // Uv, Lv and Colour need no change: the hardware interpolates every attribute as
    // attr/w and divides by 1/w per pixel, so a correct W makes all of them correct.
    Out.Uv     = In.Uv;
    Out.Lv     = In.Lv;
    Out.Colour = In.Colour;
    Out.Pom    = In.Pom;
    // Back into view space the way the engine projected it - on the engine's own pixels, which the render target's are
    // not when the picture is scaled up afterwards.
    Out.View   = float3((In.Pos.xy - ReliefCentre) * (W / max(ReliefFocal, 1.0f)), W);
    return Out;
}

// The flashlight (FlashlightBeam, FSolidPass::SetBeam): a cone of light from the eye along the view, in place of the
// ball of light the game sets down where the crosshair points. The run's block - its own, or one of zeros for a run
// that takes none of it: the sky, what is drawn after the world, what is lit by nothing.
cbuffer BeamConsts : register(b4)
{
    float4 BeamColour;   // rgb: its light at its strongest; a: how far it reaches, level units - 0: no beam
    float4 BeamCone;     // the hot spot's rim as a cosine, and one over its width; the spill's rim and width the same
    float4 BeamFall;     // where it is half as strong; where it begins, one over how soon it is full; the spill's share
    float4 BeamView;     // the view's centre and projection, the target's pixels; the least a surface turned away takes
    float4 BeamMirrors;  // x: how many of the level's mirrors give the cone back to this run (BeamOffMirrors)
#ifndef RDX_BEAM_MIRRORS
#define RDX_BEAM_MIRRORS 13     // RenderLimits.h's own number, for the run-time compile (FSolidPass checks the two)
#endif
#ifndef RDX_OWN_LAMPS
#define RDX_OWN_LAMPS     9     // RenderLimits.h's own numbers, as above
#define RDX_OWN_HEAD_ROWS 4
#define RDX_OWN_LAMP_ROWS 7
#endif
    // A mirror's two: its plane in the view's space, the normal away from the eye; then how much of the light it
    // gives back, and its number in the map.
    float4 BeamMirror[2 * RDX_BEAM_MIRRORS];
    float4 BeamSpare;
    // The own lamps (FSolidPass::SetBeam; RdxMath::OwnLampLight): where the run's view stands in the level - a place
    // rebuilt from a pixel as P lies at OwnView[0].xyz + P.x * OwnView[1].xyz + P.y * OwnView[2].xyz +
    // P.z * OwnView[3].xyz -, the share of their light a see-through surface takes (OwnView[0].w), the first lamp
    // the view's surfaces take (OwnView[1].w: in the world's view the flashlight's own is the cone itself), and
    // whether the view stands anywhere at all (OwnView[2].w); the view's centre and projection in the target's
    // pixels (OwnScreen); then the lamps' rows (OwnRow).
    float4 OwnView[4];
    float4 OwnScreen;
    float4 OwnRows[RDX_OWN_HEAD_ROWS + RDX_OWN_LAMP_ROWS * RDX_OWN_LAMPS];
};
// How much of it the pixel at Pos takes (RdxMath::BeamLight is the same arithmetic, tested). The pixel's place in view
// space comes from the pixel itself - Pos.z holds one over the view depth - so every run has it, whatever its vertices
// carry; and the surface's facing from how that place changes across the screen.
float3 BeamLight(float4 Pos)
{
    const float  Z    = 1.0f / max(Pos.z, 1e-6f);
    const float3 P    = float3((Pos.xy - BeamView.xy) * (Z / max(BeamView.z, 1.0f)), Z);
    const float  Dist = length(P);
    const float3 Nrm  = cross(ddx(P), ddy(P));
    const float  Face = abs(dot(Nrm, P)) / max(length(Nrm) * Dist, 1e-6f);
    const float  Cos  = Z / max(Dist, 1e-3f);
    const float  Hot   = smoothstep(0.0f, 1.0f, (Cos - BeamCone.x) * BeamCone.y);
    const float  Spill = smoothstep(0.0f, 1.0f, (Cos - BeamCone.z) * BeamCone.w);
    const float  Cone  = Hot + (1.0f - Hot) * BeamFall.w * Spill;
    const float  Near  = smoothstep(0.0f, 1.0f, (Dist - BeamFall.y) * BeamFall.z);
    const float  Far   = 1.0f - smoothstep(0.5f * BeamColour.a, BeamColour.a, Dist);
    const float  Fall  = 1.0f / (1.0f + (Dist / max(BeamFall.x, 1.0f)) * (Dist / max(BeamFall.x, 1.0f)));
    const float  Faces = BeamView.w + (1.0f - BeamView.w) * saturate(Face);
    return (BeamColour.a > 0.0f) ? BeamColour.rgb * (Cone * Near * Far * Fall * Faces) : float3(0.0f, 0.0f, 0.0f);
}
// The mirrors' views as the light off the mirrors finds them (FBeamBounce): x one over the depth a mirror's view left
// at the pixel, y that mirror's number - inside the mirror's own polygons as the world's view sees them, nothing
// elsewhere.
Texture2D<float2> BounceMap : register(t15);
// The flashlight's light off the level's mirrors, at the pixel at Pos. The cone that reaches a mirror goes on from it
// as from an eye behind the mirror, and that eye's view is the mirror's own view: the pixel's place is mirrored by
// the plane, and its image takes the cone as BeamLight has it - where the mirror is at the image's place on the
// screen (the map's number), and what the mirror's view shows there lies no nearer than the image (the map's depth):
// anything nearer stands in the light's way. The surface takes it by how it faces the lamp's image.
// The lamp is no point: it is BEAM_LAMP_RADIUS wide either way, so the mirror's edge - and whatever stands in the
// light's way - lies on the surface as a band, not a line, as wide as the way on from the mirror is long to the way
// up to it. The map is asked at sixteen places over a disc of that width about the image's place, turned from pixel
// to pixel; at each, what the mirror's view holds is held against the surface's own plane along that place's ray.
#define BEAM_LAMP_RADIUS 3.0f
static const float2 BeamDisc[16] = {
    float2(0.1768f, 0.0000f),   float2(-0.2258f, 0.2068f), float2(0.0346f, -0.3938f), float2(0.2846f, 0.3712f),
    float2(-0.5222f, -0.0924f), float2(0.4947f, -0.3147f), float2(-0.1655f, 0.6155f), float2(-0.3156f, -0.6076f),
    float2(0.6846f, 0.2500f),   float2(-0.7123f, 0.2940f), float2(0.3434f, -0.7337f), float2(0.2537f, 0.8089f),
    float2(-0.7647f, -0.4432f), float2(0.8971f, -0.1972f), float2(-0.5475f, 0.7788f), float2(-0.1265f, -0.9761f) };
float3 BeamOffMirrors(float4 Pos)
{
    const float  Z     = 1.0f / max(Pos.z, 1e-6f);
    const float3 P     = float3((Pos.xy - BeamView.xy) * (Z / max(BeamView.z, 1.0f)), Z);
    const float3 Nrm   = cross(ddx(P), ddy(P));
    const uint   Count = (uint)BeamMirrors.x;
    if (Count == 0u || BeamColour.a <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);
    // The surface's own side: the one the eye sees.
    const float3 Out = Nrm * (((dot(Nrm, P) > 0.0f) ? -1.0f : 1.0f) / max(length(Nrm), 1e-12f));
    uint MW, MH;
    BounceMap.GetDimensions(MW, MH);
    const float Turn = 6.2831853f * frac(52.9829189f * frac(dot(Pos.xy, float2(0.06711056f, 0.00583715f))));
    const float2 Rot = float2(cos(Turn), sin(Turn));
    float Sum = 0.0f;
    [loop] for (uint i = 0u; i < Count; i++)
    {
        const float4 Plane = BeamMirror[i * 2u];
        const float4 More  = BeamMirror[i * 2u + 1u];
        const float  Side  = dot(Plane.xyz, P) - Plane.w;
        if (Side > -1.0f)
            continue;                                   // on the mirror, or behind it
        const float3 Q = P - 2.0f * Side * Plane.xyz;
        if (Q.z < 1.0f)
            continue;
        const float Dist  = length(Q);
        const float Cos   = Q.z / Dist;
        const float Hot   = smoothstep(0.0f, 1.0f, (Cos - BeamCone.x) * BeamCone.y);
        const float Spill = smoothstep(0.0f, 1.0f, (Cos - BeamCone.z) * BeamCone.w);
        const float Cone  = Hot + (1.0f - Hot) * BeamFall.w * Spill;
        const float Far   = 1.0f - smoothstep(0.5f * BeamColour.a, BeamColour.a, Dist);
        // The light comes from the lamp's image, twice as far as the mirror along its normal.
        const float3 From = 2.0f * Plane.w * Plane.xyz - P;
        const float  Face = dot(Out, From) / max(length(From), 1e-3f);
        if (Cone * Far <= 0.0f || Face <= 0.0f)
            continue;
        // How far along the way to the image the mirror stands, and the disc's width in the map's pixels by it.
        const float  Part  = saturate(Plane.w / max(dot(Plane.xyz, Q), 1e-3f));
        const float  Wide  = clamp(BEAM_LAMP_RADIUS * (1.0f - Part) * BeamView.z / max(Q.z * Part, 1e-3f),
                                   0.75f, 48.0f);
        // The surface's own plane behind the mirror: its face mirrored, through the image.
        const float3 Lie   = Out - 2.0f * dot(Out, Plane.xyz) * Plane.xyz;
        const float  Lies  = dot(Lie, Q);
        const float2 Mid   = BeamView.xy + Q.xy * (BeamView.z / Q.z);
        const float  Tol   = max(1.5f, 0.006f * Q.z);
        float Open = 0.0f;
        [unroll] for (int k = 0; k < 16; k++)
        {
            const float2 D   = BeamDisc[k];
            const int2   T   = int2(floor(Mid + Wide * float2(D.x * Rot.x - D.y * Rot.y, D.x * Rot.y + D.y * Rot.x)));
            const bool   In  = T.x >= 0 && T.y >= 0 && T.x < (int)MW && T.y < (int)MH;
            const float2 K   = BounceMap.Load(int3(T, 0));
            const float3 Ray = float3((float2(T) + 0.5f - BeamView.xy) / max(BeamView.z, 1.0f), 1.0f);
            const float  Den = dot(Lie, Ray);
            const float  At  = (abs(Den) > 1e-5f) ? Lies / Den : Q.z;
            const float  Zs  = (At > 1.0f && At < 4.0f * Q.z) ? At : Q.z;
            const float  Zk  = (K.x > 1e-9f) ? 1.0f / K.x : 1e9f;
            Open += (In && abs(K.y - More.y) < 0.5f) ? 1.0f - smoothstep(Tol, 2.0f * Tol, Zs - Zk) : 0.0f;
        }
        if (Open <= 0.0f)
            continue;
        const float Fall = 1.0f / (1.0f + (Dist / max(BeamFall.x, 1.0f)) * (Dist / max(BeamFall.x, 1.0f)));
        // A mirror gives back what it is given; a pane of glass (More.z) by how the light meets it, as its own
        // mirror layer has it: little face on, nearly all along the pane.
        const float Meet  = saturate(dot(Q, Plane.xyz) / Dist);
        const float Gives = (More.z > 0.5f) ? More.x + (1.0f - More.x) * pow(1.0f - Meet, 5.0f) : More.x;
        Sum += Gives * Cone * Far * Fall * (BeamView.w + (1.0f - BeamView.w) * saturate(Face)) * (Open / 16.0f);
    }
    return BeamColour.rgb * Sum;
}
// What of the pixel's colour is the flashlight's doing (PSCore sets it), for the shadows' added target.
static float3 GBeamPart = float3(0.0f, 0.0f, 0.0f);

// The own lamps (RdxMath::OwnLampLight): the weapon lamps, and the flashlight's cone as a lamp in the level for the
// views that are not the eye's. Their rows: the head - how many lamps; a map's texel at unit distance, the maps' near
// distance -, the torch's rims, its fall and near fade, its reach, its facing floor and a see-through surface's share;
// then each lamp's seven - where it is, its axis and its map's slice, its colour, its map's four rows
// (RdxMath::SpotMatrix). The geometry pass reads them from its block; the mirror rays' passes, whose root signature
// has no room for the block, from the table the backend writes the same rows into (t18). The maps: t19.
Texture2D<float4>     OwnLampTable : register(t18);
Texture2DArray<float> OwnLampMaps  : register(t19);
float4 OwnRow(uint R)
{
#if RDX_RT
    return OwnLampTable.Load(int3(R, 0, 0));
#else
    return OwnRows[R];
#endif
}
// How much of lamp L's light the map lets reach W, 0..1: asked a little out from the face along N and a little nearer
// the lamp, so that a face does not shade itself; four two by two looks about the place, each weighed as the texels
// lie - a soft edge a texel or two wide.
float OwnShadow(uint L, float3 W, float3 N, float Dist)
{
    const uint   B     = RDX_OWN_HEAD_ROWS + L * RDX_OWN_LAMP_ROWS;
    const float4 Head  = OwnRow(0u);
    const float  Texel = Head.y * Dist;
    const float4 Q     = float4(W + N * (1.5f * Texel), 1.0f);
    const float4 C     = float4(dot(OwnRow(B + 3u), Q), dot(OwnRow(B + 4u), Q), dot(OwnRow(B + 5u), Q),
                                dot(OwnRow(B + 6u), Q));
    if (C.w <= Head.z)
        return 0.0f;
    const float2 Uv = float2(0.5f + 0.5f * C.x / C.w, 0.5f - 0.5f * C.y / C.w);
    if (any(Uv <= 0.0f) || any(Uv >= 1.0f))
        return 0.0f;
    const float Ref = Head.z / max(C.w - 2.0f * Texel, Head.z);
    uint MW, MH, MS;
    OwnLampMaps.GetDimensions(MW, MH, MS);
    const float2 Size = float2(MW, MH);
    const float2 T    = Uv * Size - 0.5f;
    const float2 F    = frac(T);
    const float2 G    = clamp((floor(T) + 1.0f) / Size, 2.0f / Size, 1.0f - 2.0f / Size);
    const float  Slice = OwnRow(B + 1u).w;
    static const float2 Ring[4] = { float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(-1.0f, 1.0f),
                                    float2(1.0f, 1.0f) };
    float Lit = 0.0f;
    [unroll] for (int k = 0; k < 4; k++)
    {
        // A gather's four: x the left below, y the right below, z the right above, w the left above.
        const float4 D  = OwnLampMaps.GatherRed(SampPoint, float3(G + Ring[k] / Size, Slice));
        const float4 In = float4(Ref >= D.x, Ref >= D.y, Ref >= D.z, Ref >= D.w);
        Lit += lerp(lerp(In.w, In.z, F.x), lerp(In.x, In.y, F.x), F.y);
    }
    return 0.25f * Lit;
}
// The own lamps' light at W, a place in the level whose face's normal N looks out towards whoever sees it: every lamp
// from First on that W lies in the cone of, as far as its map lets the light through (RdxMath::OwnLampLight is the
// same arithmetic).
float3 OwnLampsAt(float3 W, float3 N, uint First)
{
    const float4 Head  = OwnRow(0u);
    const float4 Rims  = OwnRow(1u);
    const float4 Fall  = OwnRow(2u);
    const float4 Reach = OwnRow(3u);
    const uint   Count = (uint)Head.x;
    float3 Sum = float3(0.0f, 0.0f, 0.0f);
    [loop] for (uint L = First; L < Count; L++)
    {
        const uint   B     = RDX_OWN_HEAD_ROWS + L * RDX_OWN_LAMP_ROWS;
        const float3 D     = W - OwnRow(B).xyz;
        const float  Dist2 = dot(D, D);
        if (Dist2 >= Reach.x * Reach.x || Dist2 < 1.0f)
            continue;
        const float  Dist = sqrt(Dist2);
        const float3 Dir  = D / Dist;
        const float  Cos  = dot(Dir, OwnRow(B + 1u).xyz);
        const float  Face = -dot(N, Dir);
        if (Cos <= Rims.z || Face <= 0.0f)
            continue;
        const float Hot    = smoothstep(0.0f, 1.0f, (Cos - Rims.x) * Rims.y);
        const float Spill  = smoothstep(0.0f, 1.0f, (Cos - Rims.z) * Rims.w);
        const float Cone   = Hot + (1.0f - Hot) * Fall.w * Spill;
        const float Near   = smoothstep(0.0f, 1.0f, (Dist - Fall.y) * Fall.z);
        const float Far    = 1.0f - smoothstep(0.5f * Reach.x, Reach.x, Dist);
        const float Q      = Dist / max(Fall.x, 1.0f);
        const float F      = min(Face, 1.0f);
        const float Light  = Cone * Near * Far * min(F * 10.0f, 1.0f) * (Reach.y + (1.0f - Reach.y) * F) /
                             (1.0f + Q * Q);
        if (Light <= 1e-4f)
            continue;
        Sum += OwnRow(B + 2u).rgb * (Light * OwnShadow(L, W, N, Dist));
    }
    return Sum;
}
// The own lamps at a pixel of a view of the level (the block's view): the pixel's place in the level from its depth,
// as BeamLight rebuilds it, and its face from how that place changes across the screen, turned towards the view's
// eye. Nothing where the block gives the view no place.
float3 OwnLampsHere(float4 Pos)
{
    const float  Z = 1.0f / max(Pos.z, 1e-6f);
    const float3 P = float3((Pos.xy - OwnScreen.xy) * (Z / max(OwnScreen.z, 1.0f)), Z);
    const float3 W = OwnView[0].xyz + P.x * OwnView[1].xyz + P.y * OwnView[2].xyz + P.z * OwnView[3].xyz;
    float3 N = cross(ddx(W), ddy(W));
    if (OwnView[2].w <= 0.5f)
        return float3(0.0f, 0.0f, 0.0f);
    N *= ((dot(N, OwnView[0].xyz - W) < 0.0f) ? -1.0f : 1.0f) / max(length(N), 1e-12f);
    return OwnView[0].w * OwnLampsAt(W, N, (uint)OwnView[1].w);
}

// Whether a flag is set in the run's own four numbers.
bool PomHas(float Bit) { return fmod(floor(PomFlags / Bit), 2.0f) >= 0.5f; }

// A number of no pattern for a place, a noise that goes smoothly between such places, and a cloud of it summed over
// four sizes: for the fountains' pools and sheets.
float FieldHash(float2 P)
{
    return frac(sin(dot(P, float2(127.1f, 311.7f))) * 43758.5453f);
}
float FieldNoise(float2 P)
{
    const float2 I = floor(P);
    const float2 F = frac(P);
    const float2 U = F * F * (3.0f - 2.0f * F);
    return lerp(lerp(FieldHash(I), FieldHash(I + float2(1.0f, 0.0f)), U.x),
                lerp(FieldHash(I + float2(0.0f, 1.0f)), FieldHash(I + float2(1.0f, 1.0f)), U.x), U.y);
}
float FieldFbm(float2 P)
{
    float Sum = 0.0f;
    float Amp = 0.5f;
    [unroll] for (int i = 0; i < 4; i++)
    {
        Sum += Amp * FieldNoise(P);
        P    = P * 2.03f + 17.7f;
        Amp *= 0.5f;
    }
    return Sum;
}
// A lean of no pattern for a grid point - how steeply a noise rises there, and which way. By whole-number
// arithmetic, so every stage of the pipeline has the same from the same point, however large the point's numbers
// are; the multipliers are odd numbers of no pattern (the golden section and three square roots, cut to 32 bits).
float2 FieldLean(int2 Q)
{
    uint H = (uint)Q.x * 0x9E3779B9u + (uint)Q.y * 0xBB67AE85u;
    H ^= H >> 15;
    H *= 0x6A09E667u;
    H ^= H >> 13;
    H *= 0x510E527Fu;
    H ^= H >> 16;
    return float2((float)(H & 0xFFFFu), (float)(H >> 16)) * (2.0f / 65535.0f) - 1.0f;
}
// A noise that is zero at every grid point and leans there as FieldLean says, smooth in between: z its height at a
// place, about zero, and xy how that changes along the grid's two ways - worked out together, so a surface lifted
// by the height is shaded by the slope it really has.
float3 FieldHumps(float2 P)
{
    const float2 I   = floor(P);
    const float2 F   = P - I;
    const float2 U   = F * F * F * (F * (F * 6.0f - 15.0f) + 10.0f);
    const float2 Du  = 30.0f * F * F * (F * (F - 2.0f) + 1.0f);
    const int2   Q   = (int2)I;
    const float2 G00 = FieldLean(Q);
    const float2 G10 = FieldLean(Q + int2(1, 0));
    const float2 G01 = FieldLean(Q + int2(0, 1));
    const float2 G11 = FieldLean(Q + int2(1, 1));
    const float  V00 = dot(G00, F);
    const float  V10 = dot(G10, F - float2(1.0f, 0.0f));
    const float  V01 = dot(G01, F - float2(0.0f, 1.0f));
    const float  V11 = dot(G11, F - float2(1.0f, 1.0f));
    const float  K   = V00 - V10 - V01 + V11;
    const float  H   = V00 + U.x * (V10 - V00) + U.y * (V01 - V00) + U.x * U.y * K;
    const float2 S   = G00 + U.x * (G10 - G00) + U.y * (G01 - G00) + U.x * U.y * (G00 - G10 - G01 + G11) +
                       Du * float2((V10 - V00) + U.y * K, (V01 - V00) + U.x * K);
    return float3(S, H);
}
// The relief is drawn at its full depth everywhere, never eased by angle or by distance: an ease has a band where it
// happens, and on a floor that band is a ring around the player that travels with them, so the carpet would swell and
// flatten under their feet. Distance needs no ease of its own - the height map's own shrunken copies flatten the relief
// as it recedes, exactly as they flatten the texture's detail, and that has no edge anywhere (see Level in PomWalk).

// Where to sample the texture, once the eye has been let into the surface. The ray walks down in equal steps until it
// is under the height map, then the crossing is found between the last two steps - which is what keeps a straight
// groove edge straight instead of stepped.
//
// More steps for a longer slide than for a short one: the ray covers more ground and would otherwise stride over
// whole grooves.
//
// RateX and RateY are the screen rates of the ORIGINAL coordinate, taken by the caller outside any branch - a
// derivative asked for inside one is not defined.
float2 PomWalk(float2 Uv, float4 Pom, float2 RateX, float2 RateY, out float OutHeight, out float OutLevel)
{
    // How far a point one unit deep slides: the sideways parts of the direction to the eye over its outward part,
    // which is the tangent of the angle the surface is seen at. The division is HERE and not at the vertex, because
    // a quotient cannot be interpolated across a polygon.
    //
    // Edge on that tangent runs away to infinity, so it is held - but by a CURVE, not by a cut. S*Tan/sqrt(S*S+Tan*Tan)
    // is the tangent itself while the tangent is small, never passes S, and rises the whole way without ever turning
    // back. A straight max() has a corner instead, and anything that takes the slide back DOWN past such a corner puts
    // the relief at its deepest on one ring around the player and gone by another, both travelling with them. Nothing
    // here may turn back, or a floor grows rings.
    //
    // This particular curve because it is FLAT where it matters: at forty-five degrees it takes back three hundredths
    // and at twenty-six a hundredth, so the relief a player looks straight at is the relief the height map says, and
    // only the grazing angles nobody can judge anyway are pulled in.
    //
    // The way to the eye comes as it is, not brought to length one (RdxMath::PomVertex): its outward part is the
    // eye's height over the surface, its sideways parts carry the texture's rates, and their quotient is the slide
    // itself, exact at every pixel whatever corners the polygon was cut to. Over the texture's own rate (Pom.w) the
    // slide is the tangent again, for the curve.
    const float  Out   = max(Pom.z, 1e-3f);
    const float2 Slide = Pom.xy / Out;
    const float  Tan   = length(Slide) / max(Pom.w, 1e-6f);
    const float  Held  = rsqrt(1.0f + (Tan / POM_SLIDE) * (Tan / POM_SLIDE));
    const float2 Whole = -Slide * Held * PomDepth;

    // How many steps. What decides it is the STRIDE, not the angle: a slide of six texels is walked safely in far
    // fewer steps than one of sixty, whatever the surface is turned to.
    const float  Texels = length(Whole / max(PomTexel, 1e-6f));
    const int    Steps  = (int)clamp(Texels + 12.0f, 12.0f, 48.0f);
    const float  Layer  = 1.0f / (float)Steps;
    const float2 Move   = Whole * Layer;

    // Which shrunken copy of the height to read - the coarser of what two things ask for.
    //
    // THE STEP, because reading finer detail than one step carries the eye across is not more truth, it is noise: at
    // a grazing angle every speck of dirt in the height then turns into a wave along the wall.
    //
    // THE SCREEN, because a map covering ten pixels has nothing finer than that to say either. This is also what
    // takes the relief away with distance: the copies grow smoother as the surface recedes and the relief flattens
    // with them, gradually, with no band anywhere for a ring to be drawn on.
    const float2 PixU   = RateX / max(PomTexel, 1e-6f);
    const float2 PixV   = RateY / max(PomTexel, 1e-6f);
    const float  Screen = 0.5f * log2(max(max(dot(PixU, PixU), dot(PixV, PixV)), 1.0f));
    const float  Level  = max(max(log2(max(Texels * Layer, 1.0f)), 0.0f), Screen);
    OutLevel = Level;

    // A texture laid on once (a sign) is held inside itself, half a texel in from each edge: sliding past it would
    // read the other side of the picture through the wrap. A tiling texture walks freely, across its seams.
    const bool   bOnce = PomHas(POM_ONCE);
    const float2 Inner = 0.5f * PomTexel;
    float2 At    = bOnce ? clamp(Uv, Inner, 1.0f - Inner) : Uv;
    float  Under = 1.0f - SceneCopy.SampleLevel(Samp, At, Level).r;  // how deep the surface is here, 0 at the top
    float  Walk  = 0.0f;                                             // how deep the ray has come
    float2 Prev  = At;                                               // where the ray was one step back
    [loop] for (int i = 0; i < Steps && Walk < Under; i++)
    {
        Prev  = At;
        At    = bOnce ? clamp(At + Move, Inner, 1.0f - Inner) : At + Move;
        Walk  += Layer;
        Under  = 1.0f - SceneCopy.SampleLevel(Samp, At, Level).r;
    }
    // The ray is now under the surface and was over it one step back, so the crossing is between the two. Halving
    // that gap a few times finds it: without this the crossing can only ever land on one of the steps, and those
    // steps are what showed as terracing on a wall seen from above.
    float2 Lo = Prev, Hi = At;           // over the surface, and under it - Prev, not At - Move, which the hold at a
                                         // texture's edge would put outside it
    float  LoD = Walk - Layer, HiD = Walk;
    [unroll] for (int k = 0; k < 6; k++)
    {
        const float2 Mid  = (Lo + Hi) * 0.5f;
        const float  MidD = (LoD + HiD) * 0.5f;
        if (MidD < 1.0f - SceneCopy.SampleLevel(Samp, Mid, Level).r)
        {
            Lo = Mid; LoD = MidD;        // still over the surface
        }
        else
        {
            Hi = Mid; HiD = MidD;
        }
    }
    OutHeight = 1.0f - saturate((LoD + HiD) * 0.5f);
    return (Lo + Hi) * 0.5f;
}

// How much of the room a point can still see, once the relief around it stands in the way. NOT a shadow cast by a
// lamp: these surfaces are lit by the level's own baked light, which has no direction to cast from. Four steps
// outwards, each further than the last; where the height map stands higher than a straight line from the point, that
// much of the view is blocked.
//
// It reads the same shrunken copy the march did. Reading the sharpest one here while the march walked a smooth one
// puts detail into the shading that is not in the shape, and far away that detail is a pixel wide and crawls.
float PomOcclusion(float2 At, float Height, float Level)
{
    const float2 Spread = PomTexel * 12.0f;
    float Blocked = 0.0f;
    [unroll] for (int i = 1; i <= 4; i++)
    {
        const float Step = (float)i * 0.25f;
        const float2 Dirs[4] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1) };
        [unroll] for (int d = 0; d < 4; d++)
        {
            const float There = SceneCopy.SampleLevel(Samp, At + Dirs[d] * Spread * Step, Level).r;
            Blocked += saturate((There - Height) * 4.0f) * (1.0f - Step);
        }
    }
    return saturate(Blocked * 0.12f);
}

// How much of a lamp's light gets to a point of the relief past the relief itself: a walk from the point towards the
// lamp through the height map, until the walk is above the highest the relief goes. Where the relief rises above the
// walk the light is cut - softly, by how far it rises above and how near to the point.
float ReliefLampShadow(float2 Uv, float Height, float Level, float3 L)
{
    // The lamp's direction along the surface's own axes; behind the wall it lights nothing.
    const float3 Lt = float3(dot(L, ReliefAxisU), dot(L, ReliefAxisV), dot(L, ReliefNormal));
    if (Lt.z <= 0.0f)
        return 0.0f;
    // From here up to the top of the relief: how far that is along the wall, in the level's units, then in the
    // texture's - held to eight times the depth, or a lamp nearly edge on walks across the whole texture.
    const float  Rise  = (1.0f - Height) * PomDepth;
    float2       Along = Lt.xy / max(Lt.z, 0.05f) * Rise;
    const float  Far   = length(Along);
    if (Far > 8.0f * PomDepth)
        Along *= 8.0f * PomDepth / Far;
    const float2 Move  = Along * ReliefRate;
    const bool   bOnce = PomHas(POM_ONCE);
    const float2 Inner = 0.5f * PomTexel;
    float Lit = 1.0f;
    [unroll] for (int k = 1; k <= 8; k++)
    {
        const float  t     = (float)k / 8.0f;
        const float2 At    = bOnce ? clamp(Uv + Move * t, Inner, 1.0f - Inner) : Uv + Move * t;
        // A fiftieth above the relief itself: near a top the walk rises very little a step, and the height map's own
        // grain would otherwise shade the tops in specks.
        const float  Ray   = Height + (1.0f - Height) * t + 0.02f;
        const float  There = SceneCopy.SampleLevel(Samp, At, Level).r;
        // A rise that covers the walk near the point cuts the most; the same rise further along, where the walk is
        // already high, less - which is what makes the shadow's edge soft.
        Lit = min(Lit, saturate(1.0f - (There - Ray) * 6.0f * (1.5f - t)));
    }
    // Never quite black: a hollow the lamp cannot see still sees the room around it.
    return 0.2f + 0.8f * Lit;
}

// Where a lamp of the relief's stands, in the view's space. In a mirror's view (the run's ReliefMirror: the mirror's
// plane in the view's space, no plane where its normal is nought) the lamps stand where the mirror shows them.
#define ReliefMirror float4(WaterWave.yzw, WaterView.z)
float3 ReliefLampPlace(int i)
{
    const float3 P = WaterLamp[2 * i].xyz;
    const float4 M = ReliefMirror;
    return (dot(M.xyz, M.xyz) > 0.25f) ? P - 2.0f * (dot(M.xyz, P) - M.w) * M.xyz : P;
}

// A lamp's share of the light the flat wall gets here (ReliefLamps): its colour's brightness, how near the point is
// within its reach, the angle it falls at. Zero out of its reach or behind the wall.
float ReliefLampShare(int i, float3 N, float3 View)
{
    const float3 To   = ReliefLampPlace(i) - View;
    const float  Dist = length(To);
    const float  Near = saturate(1.0f - Dist / max(WaterLamp[2 * i].w, 1.0f));
    const float  Flat = dot(N, To / max(Dist, 1e-3f));
    if (Near <= 0.0f || Flat <= 0.02f)
        return 0.0f;
    return Near * Flat * dot(WaterLamp[2 * i + 1].rgb, float3(0.299f, 0.587f, 0.114f));
}

// The lamps around the relief (POM_LAMPS): a factor for the baked light here, 1 on a flat wall. The light map says how
// much light reaches the wall, but not where it comes from - the lamps do. Each lamp's share is what it would give the
// flat wall here: its colour, how near it is within its reach, the angle it falls at. The relief's slope turns that
// share up or down (Lambert against the slope over Lambert against the wall), and ReliefLampShadow takes away what the
// relief itself shades. Only proportions: the light map's own level stays, and a wall no lamp of the list reaches is
// left as it was.
float ReliefLamps(float2 Uv, float Height, float Level, float3 View)
{
    // The slope here, from the height map's neighbours at the same copy: rise per unit of the level along U and V.
    const float2 Texel = PomTexel * exp2(Level);
    const float  HR    = SceneCopy.SampleLevel(Samp, Uv + float2(Texel.x, 0.0f), Level).r;
    const float  HL    = SceneCopy.SampleLevel(Samp, Uv - float2(Texel.x, 0.0f), Level).r;
    const float  HU    = SceneCopy.SampleLevel(Samp, Uv + float2(0.0f, Texel.y), Level).r;
    const float  HD    = SceneCopy.SampleLevel(Samp, Uv - float2(0.0f, Texel.y), Level).r;
    const float2 Slope = float2(HR - HL, HU - HD) / (2.0f * max(Texel, 1e-6f)) * ReliefRate * PomDepth;
    const float3 N     = ReliefNormal;
    const float3 Tilt  = normalize(N - Slope.x * ReliefAxisU - Slope.y * ReliefAxisV);
    // Each lamp's share first: the walk through the height map is the expensive part, and only the three lamps giving
    // the most here get one - the others' light is still turned by the slope, it only goes unshadowed. The first pass
    // finds the third largest share; the second takes every lamp and walks for those at or above it.
    const int Count = min((int)WaterLampCount.x, RDX_RELIEF_LAMP_MAX);
    float Top1 = 0.0f, Top2 = 0.0f, Top3 = 0.0f;
    for (int i = 0; i < Count; i++)
    {
        const float S = ReliefLampShare(i, N, View);
        if (S > Top1)      { Top3 = Top2; Top2 = Top1; Top1 = S; }
        else if (S > Top2) { Top3 = Top2; Top2 = S; }
        else if (S > Top3) { Top3 = S; }
    }
    float Sum = 0.0f, Weight = 0.0f;
    for (int k = 0; k < Count; k++)
    {
        const float S = ReliefLampShare(k, N, View);
        if (S <= 0.0f)
            continue;
        const float3 L    = normalize(ReliefLampPlace(k) - View);
        const float  Flat = dot(N, L);
        // A slope facing a lamp that only grazes the wall takes many times the wall's light; held to three, so a
        // single edge-on lamp cannot turn a grain into a highlight.
        const float  Turn = min(saturate(dot(Tilt, L)) / Flat, 3.0f);
        const float  Lit  = (S >= Top3) ? ReliefLampShadow(Uv, Height, Level, L) : 1.0f;
        Sum    += S * Turn * Lit;
        Weight += S;
    }
    return (Weight > 1e-5f) ? lerp(1.0f, Sum / Weight, saturate(ReliefLampStrength)) : 1.0f;
}

// TRIED AND TAKEN BACK (2026-09-23): the relief also writing its own depth, so that everything reading the depth
// buffer afterwards - the shadows, the occlusion, the reflections - would see it rather than the flat wall. The
// arithmetic worked, and the result was a door drawn entirely black: the shadow pass reads the depth back into a
// world position, and a position pushed INTO the wall is behind the wall, so every shadow ray met the wall itself
// at once and the surface shadowed itself away. Doing it properly means moving the shadow pass's own bias by the
// same amount, which is a change to that pass and not to this one. What is left here is the split into a core and
// two entry points, which that change will want anyway.
float4 PSCore(VSOut In)
{
    // The relief, where this surface has a height map. Everything below then works from the place the eye actually
    // reaches INTO the surface rather than the place the flat wall would have shown.
    float2 Uv    = In.Uv;
    float  Shade = 1.0f;
    const bool bPom = PomHas(POM_ON);
    // How fast the texture runs across the screen HERE, taken from the coordinate BEFORE the relief moves it. The
    // card picks which shrunken copy of the texture to read from exactly this, and the moved coordinate is not fit to
    // be asked: it jumps from pixel to pixel, so the card reads a copy far too fine and the wall breaks into
    // shimmering waves - which is what a corridor wall seen along its length looked like.
    const float2 RateX = ddx(In.Uv);
    const float2 RateY = ddy(In.Uv);
    // The light map's own rates, for the same reason: the relief moves the place the light is read from as well.
    const float2 LRateX = ddx(In.Lv);
    const float2 LRateY = ddy(In.Lv);
    float2 Lv = In.Lv;
    if (bPom)
    {
        float Height = 1.0f;
        float Level  = 0.0f;
        Uv = PomWalk(In.Uv, In.Pom, RateX, RateY, Height, Level);
        // The baked light moves with the relief, or the light and the shadows painted into it stay on the flat wall
        // while the texture slides over them. Both coordinates come from the same mapping axes, so along each one the
        // light map runs a fixed number of times as fast as the texture: that ratio is read off the two sets of rates,
        // fitted over both screen directions so neither being near zero upsets it.
        const float2 TexSq = RateX * RateX + RateY * RateY;
        const float2 Ratio = (LRateX * RateX + LRateY * RateY) / max(TexSq, 1e-12f);
        Lv = In.Lv + (Uv - In.Uv) * Ratio;
        // The relief darkens its own hollows, always, and the lamps shade it from the point the eye reached.
        Shade = 1.0f - PomOcclusion(Uv, Height, Level);
        if (PomHas(POM_LAMPS))
            Shade *= ReliefLamps(Uv, Height, Level, In.View);
    }
    // Geometry=Tessellation: the surface already stands out of the wall (DSRelief), so there is nothing to walk - only
    // the shading the relief gives it, from the height map at the texel's own place and at the screen's own copy.
    const bool bTess = PomHas(POM_TESS);
    if (bTess)
    {
        const float2 PixU   = RateX / max(PomTexel, 1e-6f);
        const float2 PixV   = RateY / max(PomTexel, 1e-6f);
        const float  Level  = 0.5f * log2(max(max(dot(PixU, PixU), dot(PixV, PixV)), 1.0f));
        const float  Height = SceneCopy.SampleLevel(Samp, Uv, Level).r;
        Shade = 1.0f - PomOcclusion(Uv, Height, Level);
        if (PomHas(POM_LAMPS))
            Shade *= ReliefLamps(Uv, Height, Level, In.View);
    }
    // PF_NoSmooth asks for the texels unfiltered. Measured: the stock renderer draws
    // the HUD icons with visible square texels, where ours smeared them.
    float4 T = NoSmooth ? Tex.Sample(SampPoint, Uv)
                        : (bPom ? Tex.SampleGrad(Samp, Uv, RateX, RateY)
                                : Tex.SampleBias(Samp, Uv, MipBias));
#if RDX_FOG
    // The engine's volumetric light (BLEND_Fog): the fog map as it is - its colour, and in alpha how much of the surface
    // it covers - times the vertex colour. A surface's fog map comes with white vertices; a mesh's fog comes in the
    // vertex colour itself, over the one white texel. Not lit, not clipped: the blend lays it over what is there.
    return T * In.Colour;
#endif
    // Masked textures arrive with alpha 0 on the transparent palette entry. Discard
    // rather than blend: that keeps the depth buffer correct, which blending would not.
#if !RDX_DEBUG_NOCLIP
    clip(T.a - 0.5f);
#endif

    // Baked lighting for BSP surfaces. Geometry without a light map gets a neutral
    // one bound instead, so there is no branch here.
    float3 L = Light.Sample(Samp, Lv).rgb * RDX_LIGHT_SCALE * LightBoost;

#if RDX_DEBUG_DIFFUSE
    // Diagnostic build: the diffuse texture alone, with no lighting at all. Comparing
    // this against the reference frame separates a wrong texture path from a wrong
    // lighting path. On screen the two look alike, and nothing else tells them apart.
    return float4(T.rgb, 1.0f);
#endif
#if RDX_DEBUG_LIGHTMAP
    // Diagnostic build: show the light map alone, to see whether a surface actually
    // receives one and what it contains.
    return float4(L, 1.0f);
#else
    // Soft particles, where this run asked for them. Nothing else about the surface changes.
    if (PomHas(POM_SOFT))
        T.rgb *= SoftFade(In.Pos);
#if RDX_MODULATED
    // Modulated blending in this engine is dst * src * 2: a texel of 128 means leave
    // what is behind it unchanged. Without the factor of two every modulated draw
    // halves the picture behind it - measured: a grey box around the mouse cursor,
    // square dark patches where blood decals are, and a sniper scope that came out at
    // exactly (0,0,0). The texture is a MULTIPLIER, so it is sampled linearly, not as
    // colour - see the bSrgb argument at the call site.
    // The vertex colour is NOT part of a modulated draw. Measured: the sniper scope
    // overlay arrives with colour (0,0,0), and multiplying by it turned the whole scope
    // black. The engine uses the colour only where it tints; a modulated draw multiplies
    // the destination by the texture and nothing else.
    // The doubling itself is done by the blend (DEST_COLOR, SRC_COLOR: src*dst + dst*src),
    // not here. A UNORM target clamps the shader's output to 1 BEFORE blending, so T * 2
    // could never exceed 1: a texel above 128 could not brighten anything. Measured: the
    // vision augmentation lays its SolidGreen (0,255,0) twice, each should double the
    // green, and our window came out exactly four times too dark before the output curve.
    return float4(T.rgb, 1.0f);
#else
    // The surface in its own light, and in the flashlight's where the run takes it: the cone itself, and what the
    // level's mirrors give back of it.
    const float3 BeamHere = BeamLight(In.Pos) + BeamOffMirrors(In.Pos) + OwnLampsHere(In.Pos);
    GBeamPart = T.rgb * BeamHere;
    float3 C = T.rgb * (In.Colour.rgb * L * Shade + BeamHere);
    if (bTess && PomHas(POM_SHOWKEEP))
    {
        C = lerp(C, float3(1.0f, 0.0f, 0.0f), 0.8f * saturate(1.0f - In.Pom.z));
        // And blue where it is lifted along a direction the faces of a bend share rather than its own normal.
        C = lerp(C, float3(0.0f, 0.3f, 1.0f), 0.8f * saturate(In.Pom.w * 200.0f));
        // And green where a cover holds it under itself, by how much of its lift that took.
        C = lerp(C, float3(0.0f, 1.0f, 0.0f), 0.8f * saturate(In.Pom.y * 2.0f));
    }
    return float4(C, 1.0f);
#endif
#endif
}

float4 PSMain(VSOut In) : SV_Target
{
    return PSCore(In);
}

// The relief's covers (FSolidPass::DrawReliefCovers): depth alone, with the holes a masked texture has - a grating's gaps
// let the relief through, as they let the eye through.
void PSCover(VSOut In)
{
#if !RDX_DEBUG_NOCLIP
    const float4 T = NoSmooth ? Tex.Sample(SampPoint, In.Uv) : Tex.SampleBias(Samp, In.Uv, MipBias);
    clip(T.a - 0.5f);
#endif
}
#if RDX_LIGHT_TARGET
// The shadows (Init with stencil kinds): the same colour, and in a second target the light the surface receives - the
// light map times the vertex colour, as luminance - for the shadow pass to weigh a light's share against. Only draws that
// write depth keep it (the pipelines' write masks), so it belongs to the depth buffer's pixel. A third target, the added
// one, holds what the shadows leave alone (FBackend::ApplyShadows): a blended draw's colour goes into it again, blended
// exactly as into the picture, and a draw that is the surface starts it anew (PSMainLitSurface) - so it ends as what
// the blended draws laid over the surface made of the picture, and of the surface itself the flashlight's light.
//
// The flashlight's light is NOT in the light target, and the shadows' factor does not touch it. It comes from the
// eye, and what the eye sees it reaches: no lamp's shadow takes any of it, nor the occlusion. The factor multiplies
// the picture whole, so the beam's part of the picture goes into the added target, which the factor leaves alone;
// and the lamps' shares are weighed against the lamps' light alone - a shadow is as deep with the flashlight on as
// without it, only the beam's light lies over it.

struct PSLitOut { float4 Colour : SV_Target0; float4 Lit : SV_Target1; float4 Added : SV_Target2; };
PSLitOut PSMainLit(VSOut In)
{
    PSLitOut Out;
    Out.Colour = PSCore(In);
    float3 Lit = In.Colour.rgb * Light.Sample(Samp, In.Lv).rgb * RDX_LIGHT_SCALE;
    Out.Lit = float4(dot(Lit, float3(0.299f, 0.587f, 0.114f)), 0.0f, 0.0f, 1.0f);
    Out.Added = Out.Colour;
    return Out;
}
// A draw that is the surface - opaque, or translucent claiming the depth: of its colour only the flashlight's part
// goes into the added target, replacing what was there. Without the flashlight that is nothing.
PSLitOut PSMainLitSurface(VSOut In)
{
    PSLitOut Out = PSMainLit(In);
    Out.Added = float4(GBeamPart, 1.0f);
    return Out;
}
#endif

// Water (BLEND_Water): a see-through water surface over copies of the picture and the depth taken at the frame's
// first water surface (FTargetSwitch::CopyForWater) - everything behind the water is in them by then (measured).
// The surface itself is what the translucent blend draws; what changes is what shows through it: it wavers with the
// waves, it takes on the water's colour with depth, and the surface's own texture fades out where the water is
// shallow, so the shoreline loses its hard edge.
// Four travelling waves as slopes, in the texture's own space: their size follows the water's texture (a pool's
// repeats more often than the sea's), and they stay on the surface as the camera moves. Per wave: its direction,
// waves per texture repeat, and repeats a second; the weights fall with the wavelength.
float2 WaterSlope(float2 Uv, float Time, float Strength)
{
    const float4 D[4] = { float4( 0.80f,  0.60f, 2.9f, 0.050f), float4(-0.60f,  0.80f, 4.3f, 0.045f),
                          float4( 0.20f, -0.98f, 6.1f, 0.040f), float4(-0.89f, -0.45f, 8.7f, 0.035f) };
    const float  A[4] = { 0.50f, 0.30f, 0.20f, 0.12f };
    float2 S = 0.0f;
    [unroll] for (int i = 0; i < 4; i++)
        S += D[i].xy * (A[i] * cos(6.2831853f * D[i].z * (dot(Uv, D[i].xy) - D[i].w * Time)));
    return S * Strength;
}

// The mirror image (Reflections=High and up): the view, mirrored by the surface's normal - so the waves and the ripples
// break it up - followed through the copy of the picture. From where the view meets the surface the ray goes on in
// steps that grow with the distance; each step goes back onto the screen, and where the ray has just gone behind what
// the picture holds there, that is what the water mirrors - a few halvings of the last step find the place more
// closely. Behind by more than the step itself, or by a wall's thickness, is something in front, not what the ray met.
// rgb: the image, a: how much of it to trust - nothing where the ray found nothing, and less towards the picture's
// edges, where the image would otherwise end in a hard line.
float4 WaterReflect(float3 Eye, float3 Sp, float3 Ray, float3 AX, float3 AY, float3 AZ, float Focal, float2 Size)
{
    const int   Steps = 24;
    const float Reach = 4096.0f;
    float  Prev = 0.0f, PrevZ = dot(Sp - Eye, AZ);
    [loop] for (int i = 1; i <= Steps; i++)
    {
        const float  T  = Reach * (float(i) / Steps) * (float(i) / Steps);
        const float3 Q  = Sp + Ray * T - Eye;
        const float  QZ = dot(Q, AZ);
        if (QZ < 1.0f)
            break;                                     // behind the camera: not in the picture
        const float2 Uv = (WaterScreen.xy + float2(dot(Q, AX), dot(Q, AY)) * (Focal / QZ)) * InvHalfSize * 0.5f +
                          JitterUv();
        if (any(Uv < 0.0f) || any(Uv > 1.0f))
            break;
        const float Scene = 1.0f / max(DepthCopy.Load(int3(min(Uv * Size, Size - 1.0f), 0)).r, 1e-6f);
        if (QZ > Scene && QZ - Scene < max(max(24.0f, QZ * 0.02f), (QZ - PrevZ) * 1.5f))
        {
            float  Lo = Prev, Hi = T;
            float2 At = Uv;
            [unroll] for (int k = 0; k < 5; k++)
            {
                const float  M   = 0.5f * (Lo + Hi);
                const float3 MQ  = Sp + Ray * M - Eye;
                const float  MZ  = max(dot(MQ, AZ), 1.0f);
                const float2 MUv = saturate((WaterScreen.xy + float2(dot(MQ, AX), dot(MQ, AY)) * (Focal / MZ)) *
                                            InvHalfSize * 0.5f + JitterUv());
                const float  MS  = 1.0f / max(DepthCopy.Load(int3(min(MUv * Size, Size - 1.0f), 0)).r, 1e-6f);
                if (MZ > MS) { Hi = M; At = MUv; } else Lo = M;
            }
            const float2 Edge = saturate(min(At, 1.0f - At) * 12.0f);
            return float4(SceneCopy.SampleLevel(Samp, At, 0.0f).rgb, Edge.x * Edge.y);
        }
        Prev  = T;
        PrevZ = QZ;
    }
    // Nothing met within reach: what lies along the ray's own direction far away - the sky, or land beyond the reach -
    // wherever the picture holds something further off than the ray has come.
    const float RZ = dot(Ray, AZ);
    if (RZ > 0.01f)
    {
        const float2 Far = (WaterScreen.xy + float2(dot(Ray, AX), dot(Ray, AY)) * (Focal / RZ)) * InvHalfSize * 0.5f +
                           JitterUv();
        if (all(Far >= 0.0f) && all(Far <= 1.0f) &&
            1.0f / max(DepthCopy.Load(int3(min(Far * Size, Size - 1.0f), 0)).r, 1e-6f) > PrevZ)
        {
            const float2 Edge = saturate(min(Far, 1.0f - Far) * 12.0f);
            return float4(SceneCopy.SampleLevel(Samp, Far, 0.0f).rgb, Edge.x * Edge.y);
        }
    }
    return 0.0f;
}

// The lamps' glints (Reflections=High and up): the lamps over the water as points, and their mirror image in the
// surface - sharp, so the waves and the ripples break it into sparkles that move with them. The copy of the picture
// cannot give these: a lamp over the water is mostly above or behind the view. A mirror image is as bright near as far,
// so the glint fades only well beyond the lamp's reach, where the water is too far off for its light to belong there.
float3 WaterGlints(float3 Eye, float3 Sp, float3 N, float H)
{
    const float3 ToEye = normalize(Eye - Sp);
    float3 Sum = 0.0f;
    const int Count = (int)WaterLampCount.x;
    [loop] for (int i = 0; i < Count; i++)
    {
        const float4 P    = WaterLamp[i * 2];
        const float3 At   = float3(P.xy, H + P.z);          // in the frame around the camera: z above the surface
        const float3 D    = At - Sp;
        const float  Dist = length(D);
        if (At.z <= 0.0f || Dist >= 2.0f * P.w)
            continue;
        const float3 Half = normalize(D / Dist + ToEye);
        const float  Fall = saturate(2.0f - Dist / P.w);
        const float  Back = 0.02f + 0.98f * pow(1.0f - saturate(dot(Half, ToEye)), 5.0f);
        Sum += WaterLamp[i * 2 + 1].rgb * (pow(saturate(dot(N, Half)), 800.0f) * 32.0f * Back * Fall);
    }
    return Sum;
}

#if RDX_RT
// Reflections=RT (docs/terv-rt.md, docs/terv-rt-host.md): the mirror image and the glints from rays traced against
// the rays' scene - the level's instance, InstanceID 0, the actors', InstanceID 1, and the sky walls', InstanceID 2
// (instance mask 4, which only these rays ask for), their triangles in world units.
//
// The tracing is the 64-bit host's, and everything else stays here - the textures a ray's answer is coloured with
// are this process's. Two builds of the same stages do it: RDX_RT_ASK only writes down the rays a pixel would
// trace (into the rays' list, for the host's RayList.hlsl), and RDX_RT_HOST reads the host's answers for them - in
// the order the rays were written down, which the same text running in both builds keeps the same. What follows
// from what a ray met - the lamps' light there (RtLampLight), the see-through surfaces on the ray's way
// (RtThinOver) - the host traces as well, with the ray itself (RDX_RAY_FLAG_LAMPS, RDX_RAY_FLAG_THIN): those rays
// start where another ray's answer says, and a pixel cannot write them down before it has that answer.
//
// A build that is not the asking kind is the reading kind: the stages before the pixel shader, which ask nothing,
// are built so. And RtLampLight.hlsli, which the host's own pass shares, is told that nothing is traced here.
#if !RDX_RT_ASK && !RDX_RT_HOST
#define RDX_RT_HOST 1
#endif
#define RDX_RT_NO_QUERY 1
// FRtPass::PrepareWater's table, as long as the frame's geometries ask: the camera in world units; where the level's
// surfaces' records start in LevelMaterial, and how many there are; then a row for each of the actors' geometries
// (RDX_RT_GEO_BYTES): its first vertex in the shadow maps' buffers (bit 31 set for the arena's), as the RT shadows
// have them, and what a ray shows of it - RtGeoRow.
ByteAddressBuffer RtWater : register(t14);
// The camera (RtEye), the level's numbers (RtLevel) and a geometry's row (RtGeoRow, RtGeoFirst) in it.
#include "RtTables.hlsli"
// The casters' vertices - three floats each (FShadowMaps::CasterUvw): u, v and the mask's slot, with the face's key,
// its own light and the vertex's normal in their fractions (RdxMath::PackCasterSlot, PackCasterPlace) - the frame's
// and the arena's, and the masks the shadow maps are drawn with.
ByteAddressBuffer FrameUvw : register(t7);
ByteAddressBuffer ArenaUvw : register(t8);
// The rays' list (RenderLimits.h, RDX_RAY_*; docs/terv-rt-host.md): every ray this text would trace is the 64-bit
// host's to trace. What one pixel of a surface asks lies together in the requests, a block, and the answers lie in
// the same slots of their own buffer.
#include "RayListSlots.hlsli"
// The host's answer for a ray, by a ray query's own names: what reads a traced ray's answer reads this one as it is.
struct FRtHit
{
    uint4 Rec;
    uint   CommittedStatus()               { return (Rec.x != 0xFFFFFFFFu && any(Rec != 0u)) ? COMMITTED_TRIANGLE_HIT
                                                                                              : COMMITTED_NOTHING; }
    uint   CommittedInstanceID()           { return Rec.y >> 30; }
    float  CommittedRayT()                 { return asfloat(Rec.x); }
    uint   CommittedPrimitiveIndex()       { return Rec.z; }
    uint   CommittedGeometryIndex()        { return Rec.y & 0x00FFFFFFu; }
    // The ray was asked for only where the eye sees its start (RDX_RAY_FLAG_SEEN), and the eye does not.
    bool   Unseen()                        { return Rec.x == 0xFFFFFFFFu && Rec.y == 1u; }
    float2 CommittedTriangleBarycentrics() { return float2((float)(Rec.w >> 16), (float)(Rec.w & 0xFFFFu)) / 65535.0f; }
};
// Flags the next ray asked goes with besides its own, and no ray after it: set before the ray is asked.
// RDX_RAY_FLAG_SEEN: the ray is the host's to trace only where the eye sees its start - where this text looks from
// the eye first and traces on only where that look met nothing (PSPaneKeep); the answer says where it does not
// (FRtHit::Unseen). RDX_RAY_FLAG_EYE: the host says with the ray's answer whether the lamp at the eye is in sight
// of the ray's start along the ray - where this text traces a ray of its own for it (GlassMirror).
static uint GRtAskMore = 0u;
// How faint a lamp's light on what a mirror ray meets may be and still be worked out, as the step the ray is asked
// with (RayListSlots.hlsli, RayLeastFor) - set by how much of the ray's image shows in the pixel (RtShows), and
// kept until that is said again.
static uint GRtAskLeast = 0u;
// Whether the lamp at the eye is in sight of the start of the ray last asked with RDX_RAY_FLAG_EYE - known where
// the answers are read, and no where the rays are only written down.
static bool GRtEyeSeen = false;
#if RDX_RT_ASK
// The pixels' blocks, the last written first: a pixel's cell holds the first slot of one (0: none), and a block's
// first request the first slot of the one written at that pixel before it.
RWTexture2D<uint>   RtHeads : register(u4);
RWByteAddressBuffer RtAsked : register(u5);
// What the pixel has asked so far - a request's first four uints and its last four - until RtAskFlush writes them
// out; and whether the pixel is thrown away, and asks nothing after all (the build has no target to keep it from).
static uint  GRtAskCount = 0u;
static uint4 GRtAskA[RDX_RAY_BLOCK_MOST];
static uint4 GRtAskB[RDX_RAY_BLOCK_MOST];
static bool  GRtAskDead  = false;
#else
// In the masks' place - the host tests them where it traces: the pixels' blocks. In the scene's place: the requests,
// for a block's key. And the answers, and the see-through surfaces the rays passed on their way.
Texture2D<uint>   RtHeads   : register(t9);
ByteAddressBuffer RtAsked   : register(t6);
ByteAddressBuffer RtAnswers : register(t16);
ByteAddressBuffer RtPassed  : register(t17);
// The pixel's own block - its first slot, and how many requests it has: none until RtFind found it - and how many of
// their answers the pixel has taken.
static uint GRtBlock      = 0u;
static uint GRtBlockCount = 0u;
static uint GRtTaken      = 0u;
// Whether the block is another pixel's of the two by two (Reflections=RTHalf, RtFind).
static bool GRtBorrowed   = false;
// What the host traced on to from the answer last taken (an answer's last four uints): the lamps' light where the
// ray met something, with what stood in that light's way looked for among GRtLitBy - RtLampLight gives it back
// where it is asked for the same -, and the see-through surfaces the ray passed: how many, and the first one's slot
// (RtPassedAt).
static float3 GRtLit         = float3(0.0f, 0.0f, 0.0f);
static uint   GRtLitBy       = 0u;
static uint   GRtPassedCount = 0u;
static uint   GRtPassedFirst = 0u;
#endif

// One ray of the pixel's: written down (RDX_RT_ASK) - what comes back is "met nothing", and the build draws no
// pixel by it - or its answer taken (RDX_RT_HOST), the next in the pixel's block.
// Kind and Flags: RDX_RAY_KIND_*, RDX_RAY_FLAG_*; Mask: the instances it meets.
FRtHit RtAsk(uint Kind, float3 Origin, float3 Way, float From, float To, uint Mask, uint Flags)
{
    FRtHit Q;
    Q.Rec = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
    const uint All = Flags | GRtAskMore;
    GRtAskMore = 0u;
#if RDX_RT_ASK
    if (GRtAskCount < RDX_RAY_BLOCK_MOST)
    {
        GRtAskA[GRtAskCount] = uint4(asuint(Origin), asuint(To));
        GRtAskB[GRtAskCount] = uint4(RayPackWay(Way), 0u, 0u, RayPackWord(Kind, Mask, All, From, GRtAskLeast));
        GRtAskCount++;
    }
#else
    GRtLit         = float3(0.0f, 0.0f, 0.0f);
    GRtLitBy       = 0u;
    GRtPassedCount = 0u;
    GRtPassedFirst = 0u;
    if (GRtTaken < GRtBlockCount)
    {
        const uint At = (GRtBlock + GRtTaken) * RDX_RAY_SLOT_BYTES;
        // Another pixel's block serves as long as its requests are the sort this pixel asks - their words, the
        // block's length aside; from the first that is not, nothing more.
        if (GRtBorrowed && ((RtAsked.Load(At + 28u) ^ RayPackWord(Kind, Mask, All, From, GRtAskLeast)) &
                            ~(RDX_RAY_COUNT_MASK << RDX_RAY_COUNT_SHIFT)) != 0u)
        {
            GRtBlockCount = 0u;
            GRtTaken++;
            return Q;
        }
        Q.Rec = RtAnswers.Load4(At);
        // What the host traced on to is there only where the request asked for it.
        if ((All & (RDX_RAY_FLAG_LAMPS | RDX_RAY_FLAG_THIN | RDX_RAY_FLAG_EYE)) != 0u)
        {
            const uint4 More = RtAnswers.Load4(At + 16u);
            GRtLit         = asfloat(More.xyz);
            GRtLitBy       = RayMoreLit(More.w);
            GRtPassedCount = RayMorePassed(More.w);
            GRtPassedFirst = RayMoreFirst(More.w);
            if ((All & RDX_RAY_FLAG_EYE) != 0u)
                GRtEyeSeen = RayMoreEye(More.w);
        }
    }
    GRtTaken++;
#endif
    return Q;
}

#if RDX_RT_ASK
// How alike a pixel's first ray must be to the first ray of the pixel that asks for it (RtAskLed): the cosine between
// their ways at least, and their starts no further apart than this share of their distance from the eye and a unit.
#define RT_HALF_ALIKE_COS 0.99f
#define RT_HALF_ALIKE_FAR 0.03f

// Reflections=RTHalf (RtEye.w): the pixels run in fours, two by two, and the first of the four that writes requests
// down - first in the four's order among those the wave counts as running: a helper lane, run for the derivatives
// alone, writes nothing, and is not - asks for the others whose requests are the same sort - the same ways of tracing,
// in the same order - and whose first ray starts at about the same place and goes about the same way. Those ask
// nothing: the build that reads the answers takes that one's for them (RtFind), with their own starts and ways and
// its distances. All four run this, before any of them leaves. Whether this pixel is one of the others.
bool RtAskLed(bool bWrites)
{
    uint Sig = GRtAskCount;
    [loop] for (uint i = 0u; i < GRtAskCount; i++)
        Sig = Sig * 0x9E3779B1u + GRtAskB[i].w;
    const float3 At   = asfloat(GRtAskA[0].xyz);
    const uint   Way  = GRtAskB[0].x;
    const uint4  Live = WaveActiveBallot(bWrites);
    uint   Lead    = 4u;
    uint   LeadIn  = 0u;
    uint   LeadSig = 0u;
    float3 LeadAt  = float3(0.0f, 0.0f, 0.0f);
    uint   LeadWay = 0u;
    [unroll] for (uint k = 0u; k < 4u; k++)
    {
        const uint   Lane = QuadReadLaneAt(WaveGetLaneIndex(), k);
        const uint   KSig = QuadReadLaneAt(Sig, k);
        const float3 KAt  = QuadReadLaneAt(At, k);
        const uint   KWay = QuadReadLaneAt(Way, k);
        if (Lead == 4u && ((Live[Lane >> 5u] >> (Lane & 31u)) & 1u) != 0u)
        {
            Lead    = k;
            LeadIn  = Lane;
            LeadSig = KSig;
            LeadAt  = KAt;
            LeadWay = KWay;
        }
    }
    if (!bWrites || Lead == 4u || LeadIn == WaveGetLaneIndex() || LeadSig != Sig)
        return false;
    const float  Reach = RT_HALF_ALIKE_FAR * length(At - RtEye.xyz) + 1.0f;
    const float3 Gap   = At - LeadAt;
    return dot(Gap, Gap) < Reach * Reach && dot(RayUnpackWay(Way), RayUnpackWay(LeadWay)) > RT_HALF_ALIKE_COS;
}

// The pixel's requests into the list, at the end of its shader - Cover: the pixel's coverage, none for a helper lane.
// Their slots are taken with an atomic; where the list has no room left for them nothing is written, and the count -
// which still says how many were wanted - tells the builds that read to give the whole frame up. The block goes first
// among the pixel's, with the pixel's depth for its key.
void RtAskFlush(float4 Pos, uint Cover)
{
    const bool bWrites = !GRtAskDead && GRtAskCount != 0u && Cover != 0u;
    if (RtEye.w > 0.5f && RtAskLed(bWrites))
        return;
    if (!bWrites)
        return;
    uint First;
    RtAsked.InterlockedAdd(0u, GRtAskCount, First);
    if (First + GRtAskCount > RtAsked.Load(4u))
        return;
    uint Before;
    InterlockedExchange(RtHeads[uint2(Pos.xy)], First, Before);
    GRtAskB[0].y  = asuint(Pos.z);
    GRtAskB[0].z  = Before;
    GRtAskB[0].w |= GRtAskCount << RDX_RAY_COUNT_SHIFT;
    [loop] for (uint i = 0u; i < GRtAskCount; i++)
    {
        RtAsked.Store4((First + i) * RDX_RAY_SLOT_BYTES, GRtAskA[i]);
        RtAsked.Store4((First + i) * RDX_RAY_SLOT_BYTES + 16u, GRtAskB[i]);
    }
}
#else
// The pixel's own block among those written at its place, before it takes an answer: the one whose key is the depth
// this build draws the pixel at - the stages before the pixel shader are the same in both builds, and so is the
// depth. With more asked in the frame than the list held - or more surfaces passed than their buffer holds -, no
// pixel has a block: answered for some pixels and not for others, a surface would be a patchwork.
void RtFind(float4 Pos)
{
    GRtBlock      = 0u;
    GRtBlockCount = 0u;
    GRtTaken      = 0u;
    GRtEyeSeen    = false;
    GRtBorrowed   = false;
    const uint2 Head = RtAsked.Load2(0u);
    const uint2 Past = RtPassed.Load2(0u);
    if (Head.x <= Head.y && Past.x <= Past.y)
    {
        const uint Key = asuint(Pos.z);
        uint At = RtHeads.Load(int3(Pos.xy, 0));
        [loop] for (uint n = 0u; n < RDX_RAY_CHAIN_MOST && At != 0u && At < Head.x; n++)
        {
            const uint4 B = RtAsked.Load4(At * RDX_RAY_SLOT_BYTES + 16u);
            if (B.y == Key)
            {
                GRtBlock      = At;
                GRtBlockCount = RayWordCount(B.w);
                break;
            }
            At = B.z;
        }
    }
    // Reflections=RTHalf (RtEye.w): a pixel that asked nothing of its own takes the block of the first of its two by
    // two that has one - the one that asked for it (RtAskLed). All four run this, before any of them is thrown away.
    if (RtEye.w > 0.5f)
    {
        const uint  OwnBlock = GRtBlock;
        const uint  OwnCount = GRtBlockCount;
        const uint4 Live     = WaveActiveBallot(OwnCount != 0u);
        [unroll] for (uint k = 0u; k < 4u; k++)
        {
            const uint Lane   = QuadReadLaneAt(WaveGetLaneIndex(), k);
            const uint KBlock = QuadReadLaneAt(OwnBlock, k);
            const uint KCount = QuadReadLaneAt(OwnCount, k);
            if (GRtBlockCount == 0u && ((Live[Lane >> 5u] >> (Lane & 31u)) & 1u) != 0u)
            {
                GRtBlock      = KBlock;
                GRtBlockCount = KCount;
                GRtBorrowed   = true;
            }
        }
    }
}

// A see-through surface the ray of the answer last taken passed on its way, the nearest first - as the host found
// them, where the other build traces for each (RtThinOver) - by a ray query's own names; and the lamps' light on it,
// for RtLampLight. Past the last of them: nothing.
FRtHit RtPassedAt(uint Layer)
{
    FRtHit Q;
    Q.Rec = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
    if (Layer < GRtPassedCount)
    {
        const uint  At = (GRtPassedFirst + Layer) * RDX_RAY_SLOT_BYTES;
        const uint4 A  = RtPassed.Load4(At);
        const uint2 B  = RtPassed.Load2(At + 16u);
        Q.Rec    = uint4(A.x, 3u << 30, A.y, 0u);
        GRtLit   = asfloat(uint3(A.z, A.w, B.x));
        GRtLitBy = B.y;
    }
    return Q;
}
#endif
// The level's colours (FRtPass::SetLevel): a uint for each triangle of the scene's level - its surface - then a record
// for each surface (RDX_RT_RECORD_BYTES): its texture's average colour (RdxMath::PackRtAlbedo); the light its light
// map gave it when last drawn (RdxMath::PackRtLight; all ones while none was seen); its texture's place among
// RtTextures plus one - 0 until the surface was drawn; and from the record's sixteenth byte the two rows that put a
// place of the level on that texture, then how many of its texels a unit of the level holds.
ByteAddressBuffer LevelMaterial : register(t10);
// The textures themselves: the texture heap from its first view on (FTextureCache), read by a texture's place in it.
Texture2D RtTextures[] : register(t0, space1);
// The far probe (FRtPass::GetFar): what the picture has shown further off than RDX_RT_FAR_MIN, by the world's directions
// from the camera; alpha 1 where it holds something.
TextureCube<float4> FarProbe : register(t11);

// A ray that meets a sky wall, or leaves the level, goes to the sky: what the picture holds along its direction stands
// in where it lies further off than the ray would have come through this many units - WaterReflect's own reach, so that
// the sky is what Medium shows.
#define RT_WATER_SKY_REACH 4096.0f
// Below this share of the mirror image in the surface's sheen no mirror ray is traced, and up to twice it the image fades
// in (WaterSurface).
#define RT_WATER_MIN_BACK 0.04f

// Whether a geometry of the actors' is the player's own body (FRtPass::BuildScene marks its row's colour): a pane
// of glass shows it as a mirror does, water does not - its rays pass it (WaterReflectRtSolid's bBody).
bool RtIsOwnBody(uint G)
{
    return (RtWater.Load(RtGeoRow(G) + 4u) & 0x8000000u) != 0u;
}

float3 RtAlbedo(uint V)
{
    return float3(float(V & 255u), float((V >> 8) & 255u), float((V >> 16) & 255u)) / 255.0f;
}

float3 RtLight(uint V)
{
    return float3(float(V & 1023u), float((V >> 10) & 1023u), float((V >> 20) & 1023u)) * (4.0f / 1023.0f);
}

// The light a caster's face gets, looking along N, of the engine's own light on the caster - what its drawn polygons
// carried, kept by the world's six ways (the row at Row): each way's by the square of the normal's part along it
// (RdxMath::RtCubeLight).
float3 RtCubeLight(uint Row, float3 N)
{
    const uint  At = Row + RDX_RT_GEO_CUBE;
    const uint3 W  = uint3((N.x >= 0.0f) ? 0u : 4u, (N.y >= 0.0f) ? 8u : 12u, (N.z >= 0.0f) ? 16u : 20u);
    return RtLight(RtWater.Load(At + W.x)) * (N.x * N.x) + RtLight(RtWater.Load(At + W.y)) * (N.y * N.y) +
           RtLight(RtWater.Load(At + W.z)) * (N.z * N.z);
}

// The level's lamps' light at a place a ray met (RtLampLight), a glint's ray's stop before its lamp (RT_GLINT_STOP)
// and a caster's vertex's normal (RtCasterNormal). Through the 64-bit host a lamp too faint to show in the pixel is
// left out (GRtLampLeast).
#define RDX_RT_LAMPS_LEAST 1
#include "RtLampLight.hlsli"
// How much of the mirror image along the rays asked from here on shows in the pixel, at the most: the lamps' light
// on what those rays meet is worked out only as far as it shows through that share - by the host, which is told
// with each ray (GRtAskLeast), and by this text beside the host's answers, which goes by the same.
void RtShows(float Share)
{
    GRtAskLeast  = RayLeastFor(Share);
    GRtLampLeast = RayLeast(GRtAskLeast);
}

// The light at a point the picture cannot show, where no light map of its own is known (an actor, a surface not drawn
// yet): the light the water gets here (Lit) or the lamps over the water (WaterLamp, their colour falling off to nothing
// at their reach), whichever is more, channel by channel - not the sum: the water's own light map holds those lamps
// already where it is near them. At: the point, in the frame around the camera (z above the surface); H the camera's
// height above it.
float3 RtNearLight(float3 At, float H, float3 Lit)
{
    float3 Sum = 0.0f;
    const int Count = (int)WaterLampCount.x;
    [loop] for (int i = 0; i < Count; i++)
    {
        const float4 P = WaterLamp[i * 2];
        const float  F = saturate(1.0f - distance(float3(P.xy, H + P.z), At) / max(P.w, 1.0f));
        Sum += WaterLamp[i * 2 + 1].rgb * (F * F * 2.0f);
    }
    return max(Lit, Sum * LightBoost);
}

// The mirror image (Reflections=RT): the ray WaterReflect follows through the picture, traced against the scene
// instead. Where it ends is exact - no step overshoots a thin post, no wall's thickness is guessed. What is there comes
// from the picture where the picture shows that very spot (its depth there is the hit's), and from the surface's own
// colour where it does not: off the picture's edge, behind the view, behind something nearer - the far shore behind the
// railing. That colour is the thing's own: a level surface's texture times its own light map as it was last drawn -
// and where none was seen yet, RtNearLight - and an actor's texture at the place the ray met, times the light that
// reaches the actor from where its face looks (RtCubeLight). What carries its own light
// (RdxMath::RtAlbedoOwnLight, a face marked so) is its colour alone. A ray that meets a sky wall or leaves the level
// goes to the sky.
// Lit: the light the mirroring surface itself receives, the stand-in RtNearLight starts from - or below zero where
// the caller has none to give (a pane of glass: its own light says nothing of what stands across the street): then
// what the ray met off the picture, with no light on record, is not shown at all. No light is made up for it.
// rgb: the image, a: how much of it to trust - all of it where the ray met something it can show.
// How far a ray goes before it meets something of the rays' scene, To where it meets nothing: the level and the
// casters, a masked one by its holes, as the mirror rays ask (WaterReflectRtSolid). bBody: whether the player's own
// body is in the way too.
float RtReach(float3 Origin, float3 Dir, float From, float To, bool bBody)
{
    // The host's to trace: a request of its own in the pixel's block - written down, or its answer taken.
    FRtHit Q = RtAsk(RDX_RAY_KIND_FIRST, Origin, Dir, From, To, 0x07u, bBody ? 0u : RDX_RAY_FLAG_NO_BODY);
    return (Q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && Q.CommittedInstanceID() != 2u) ? Q.CommittedRayT() : To;
}

float4 WaterReflectRtSolid(float3 Eye, float3 Sp, float3 Ray, float3 AX, float3 AY, float3 AZ, float Focal,
                           float2 Size, float3 Lit, bool bBody, out float Reach)
{
    // The host's to trace: where the ray starts in the level and how far it may go; its way, and the instances it
    // meets - the level, the bodies and the sky's walls. And what the host is to trace on to from what the ray
    // meets: the lamps' light there - not in a measuring view that shows no light (RtLevel.z 1, 3 and 6) -, and,
    // where the image is laid over with them (WaterReflectRt), the see-through surfaces on the ray's way.
    const uint MirrorFlags = (bBody ? 0u : RDX_RAY_FLAG_NO_BODY) |
                             ((RtLevel.z == 1u || RtLevel.z == 3u || RtLevel.z == 6u) ? 0u : RDX_RAY_FLAG_LAMPS) |
                             ((RtLevel.z == 0u || RtLevel.z == 8u || RtLevel.z == 10u) ? RDX_RAY_FLAG_THIN : 0u);
#if RDX_RT_ASK
    // Only written down: nothing to show yet.
    RtAsk(RDX_RAY_KIND_FIRST, RtEye.xyz + (Sp - Eye), Ray, 2.0f, 65536.0f, 0x07u, MirrorFlags);
    Reach = 65536.0f;
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
#else
    // What the host answered for the ray this pixel wrote down (the RDX_RT_ASK build of the same stage, drawn
    // before) - and nothing to show where the pixel has no block: the frame asked more than the list held.
    if (GRtBlockCount == 0u)
    {
        Reach = 65536.0f;
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    FRtHit Q = RtAsk(RDX_RAY_KIND_FIRST, RtEye.xyz + (Sp - Eye), Ray, 2.0f, 65536.0f, 0x07u, MirrorFlags);
    // How far off the ray met it - or that it met nothing: what it passes on its way is asked up to there.
    Reach = (Q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? Q.CommittedRayT() : 65536.0f;
    // The measuring view (RtLevel.z 1): blue the sky taken from the picture, light blue from the far probe, black none; the
    // level green where the picture shows the spot, yellow where its own colour stands in; an actor cyan and magenta the
    // same way. RtLevel.z 3: how far
    // the ray went to what it met - red within 500 units, yellow within 2000, green within 8000, white beyond; the sky blue.
    const bool bView = (RtLevel.z == 1u);
    // RtLevel.z 6: how high above the water's surface the ray met something - blue below it, green within 8 units of it,
    // yellow up to 64, red above; the level's darker than the actors'; black the sky.
    if (RtLevel.z == 6u)
    {
        if (Q.CommittedStatus() != COMMITTED_TRIANGLE_HIT || Q.CommittedInstanceID() == 2u)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        const float Z = (Sp + Ray * Q.CommittedRayT()).z;
        const float K = (Q.CommittedInstanceID() == 0u) ? 0.6f : 1.0f;
        return K * ((Z < -8.0f) ? float4(0.0f, 0.3f, 1.0f, 1.0f) : ((Z <= 8.0f) ? float4(0.0f, 1.0f, 0.0f, 1.0f)
                  : ((Z <= 64.0f) ? float4(1.0f, 1.0f, 0.0f, 1.0f) : float4(1.0f, 0.0f, 0.0f, 1.0f))));
    }
    if (RtLevel.z == 3u)
    {
        if (Q.CommittedStatus() != COMMITTED_TRIANGLE_HIT || Q.CommittedInstanceID() == 2u)
            return float4(0.0f, 0.0f, 0.8f, 1.0f);
        const float T = Q.CommittedRayT();
        return (T < 500.0f) ? float4(0.8f, 0.0f, 0.0f, 1.0f) : ((T < 2000.0f) ? float4(0.8f, 0.8f, 0.0f, 1.0f)
             : ((T < 8000.0f) ? float4(0.0f, 0.8f, 0.0f, 1.0f) : float4(0.9f, 0.9f, 0.9f, 1.0f)));
    }
    if (Q.CommittedStatus() != COMMITTED_TRIANGLE_HIT || Q.CommittedInstanceID() == 2u)
    {
        // The picture itself along the ray's direction, where it shows the sky there; towards its edges, and wherever it
        // shows something nearer in that direction, the far probe - what an earlier look saw there.
        const float4 Probe = FarProbe.SampleLevel(Samp, Ray, 0.0f);
        const float4 Kept  = (Probe.a > 0.25f) ? float4(Probe.rgb / Probe.a, 1.0f) : 0.0f;
        const float RZ = dot(Ray, AZ);
        if (RZ > 0.01f && RtLevel.z != 5u)
        {
            const float2 Far = (WaterScreen.xy + float2(dot(Ray, AX), dot(Ray, AY)) * (Focal / RZ)) * InvHalfSize * 0.5f +
                               JitterUv();
            const float  Min = dot(Sp + Ray * RT_WATER_SKY_REACH - Eye, AZ);
            if (all(Far >= 0.0f) && all(Far <= 1.0f) &&
                1.0f / max(DepthCopy.Load(int3(min(Far * Size, Size - 1.0f), 0)).r, 1e-6f) > Min)
            {
                const float2 Edge = saturate(min(Far, 1.0f - Far) * 12.0f);
                const float  Keep = Edge.x * Edge.y;
                if (bView)
                    return float4(0.0f, 0.0f, 0.8f, 1.0f);
                return float4(lerp(Kept.rgb, SceneCopy.SampleLevel(Samp, Far, 0.0f).rgb, Keep),
                              max(Keep, Kept.a));
            }
        }
        if (bView)
            return (Kept.a > 0.0f) ? float4(0.0f, 0.5f, 1.0f, 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
        return Kept;
    }
    float3 Own;
    // Whether the light at what the ray met is known - on record, its own, or the caller's stand-in.
    const bool bGuess = Lit.x >= 0.0f;
    bool       bLit   = true;
    // Whether the picture may show the very spot: not the player's own body, which first person never draws - its
    // places lie a hand's breadth from the camera, and the picture holds something else at every one of them.
    bool       bShown = true;
    const float3 Hit = Sp + Ray * Q.CommittedRayT();       // in the frame around the camera
    if (Q.CommittedInstanceID() == 0u)
    {
        const uint  Surf = LevelMaterial.Load(Q.CommittedPrimitiveIndex() * 4u);
        const uint  At   = RtLevel.x + Surf * RDX_RT_RECORD_BYTES;
        const uint4 Rec  = (Surf < RtLevel.y) ? LevelMaterial.Load4(At) : uint4(0x808080u, 0xFFFFFFFFu, 0u, 0u);
        const bool  bOwn = (Rec.x & 0x1000000u) != 0u;
        const float3 Spot = RtEye.xyz + (Hit - Eye);       // the place the ray met, in the level
        // The light of that very spot. Where the engine has made the surface's light map - it makes it for every
        // view it draws, a mirror's among them - that is the light the picture itself shows the surface by, its
        // baked shadows in it: read at the spot, and dimmed by what the actors take of the lamps' light there (a ray
        // to each lamp against the actors alone - the shadows the picture lays on the surface). Where there is no
        // light map, worked out whole: the light all round the zone, and every lamp of the level that reaches the
        // spot and that the spot can see.
        float3 L = 1.0f;
        if (!bOwn)
        {
            float3 N = asfloat(LevelMaterial.Load3(At + RDX_RT_RECORD_NORMAL));
            N = (dot(N, N) < 0.25f) ? -Ray : normalize(N);
            N = (dot(N, Ray) > 0.0f) ? -N : N;
            const bool bSpecial = (Rec.x & 0x2000000u) != 0u;
            if (Rec.w != 0u)
            {
                const float4 RowU = asfloat(LevelMaterial.Load4(At + RDX_RT_RECORD_LIGHT_ROWS));
                const float4 RowV = asfloat(LevelMaterial.Load4(At + RDX_RT_RECORD_LIGHT_ROWS + 16u));
                const float4 Edge = asfloat(LevelMaterial.Load4(At + RDX_RT_RECORD_LIGHT_EDGE));
                const float  Gain = asfloat(LevelMaterial.Load(At + RDX_RT_RECORD_GAIN));
                const float2 Lu   = float2(dot(RowU.xyz, Spot) + RowU.w, dot(RowV.xyz, Spot) + RowV.w);
                const float2 Lv   = clamp(Lu, Edge.xy, Edge.zw);
                GRtLightHeld      = any(abs(Lu - Lv) > 1e-4f);
                const float3 Open = RtLampLight(Spot, N, bSpecial, 0u);
                const float3 Left = RtLampLight(Spot, N, bSpecial, 0x02u);
                const float  All  = Open.r + Open.g + Open.b;
                // RtLevel.z 10, a measurement: the light map alone, not dimmed by the lamps' rays - the A/B of that
                // dimming, the picture otherwise as it is.
                L = RtTextures[NonUniformResourceIndex(Rec.w - 1u)].SampleLevel(Samp, Lv, 0.0f).rgb *
                    (Gain * RDX_LIGHT_SCALE * LightBoost) *
                    ((All > 0.012f && RtLevel.z != 10u) ? saturate((Left.r + Left.g + Left.b) / All) : 1.0f);
            }
            else
            {
                L = (RtLight(LevelMaterial.Load(At + RDX_RT_RECORD_AMBIENT)) + RtLampLight(Spot, N, bSpecial, 0x03u)) *
                    (asfloat(RtWater.Load(60u)) * LightBoost);
                // No light map on record, and the zone's light and the lamps the spot sees give it (nearly) none: what
                // the ray met is nothing the eye is shown - along a pool's wall at its waterline, met by the mirror
                // rays the waves tilt, it lay as black stripes over the water's edge. Nothing of it in the image, then:
                // the water's own look stands there, as where a ray meets nothing.
                if (dot(L, float3(0.299f, 0.587f, 0.114f)) < 0.004f)
                    return float4(0.0f, 0.0f, 0.0f, 0.0f);
            }
            // And the own lamps, which no light map holds: the weapon lamps and the flashlight, as the eye's own
            // view has them on the place itself.
            L += OwnLampsAt(Spot, N, 0u);
            // RtLevel.z 11, a measurement: which light the level's surface the ray met is given - red a light map that
            // gives (nearly) none, green one that gives light, magenta the place outside its light map and held at its
            // edge; blue no light map and (nearly) no light, cyan no light map and light.
            if (RtLevel.z == 11u)
            {
                const bool bDark = dot(L, float3(0.299f, 0.587f, 0.114f)) < 0.02f;
                if (Rec.w != 0u)
                    return GRtLightHeld ? float4(1.0f, 0.0f, 1.0f, 1.0f)
                                        : (bDark ? float4(1.0f, 0.0f, 0.0f, 1.0f) : float4(0.0f, 0.8f, 0.0f, 1.0f));
                return bDark ? float4(0.0f, 0.0f, 1.0f, 1.0f) : float4(0.0f, 0.8f, 0.8f, 1.0f);
            }
        }
        // The surface's own picture where it was drawn before - at the place the ray met, and as fine as the picture
        // would show it from as far as the view has come to it: to the mirror, and on along the ray. Its texture's
        // average colour until then. The measuring view 7: the average colour everywhere, as view 4 with no texture.
        float3 Colour = RtAlbedo(Rec.x);
        if (Rec.z != 0u && RtLevel.z != 7u)
        {
            const float4 RowU  = asfloat(LevelMaterial.Load4(At + 16u));
            const float4 RowV  = asfloat(LevelMaterial.Load4(At + 32u));
            const float  Dense = asfloat(LevelMaterial.Load(At + 48u));
            const float3 Place = Spot;
            const float  Foot  = (length(Sp - Eye) + Q.CommittedRayT()) / max(Focal, 1.0f);
            Colour = RtTextures[NonUniformResourceIndex(Rec.z - 1u)]
                         .SampleLevel(Samp, float2(dot(RowU.xyz, Place) + RowU.w, dot(RowV.xyz, Place) + RowV.w),
                                      log2(max(Foot * Dense, 1.0f))).rgb;
        }
        Own = Colour * L;
    }
    else
    {
        // An actor: the three corners of the triangle the ray met - their texture places, and in those the face's
        // key, its own light and the corners' normals.
        const uint Row  = RtGeoRow(Q.CommittedGeometryIndex());
        const uint Info = RtWater.Load(Row);
        const uint C    = RtWater.Load(Row + 4u);
        bShown = (C & 0x8000000u) == 0u;
        const uint At   = ((Info & 0x7FFFFFFFu) + Q.CommittedPrimitiveIndex() * 3u) * 12u;
        float3 VA, VB, VC;
        if ((Info >> 31) != 0u)
        {
            VA = asfloat(ArenaUvw.Load3(At));
            VB = asfloat(ArenaUvw.Load3(At + 12u));
            VC = asfloat(ArenaUvw.Load3(At + 24u));
        }
        else
        {
            VA = asfloat(FrameUvw.Load3(At));
            VB = asfloat(FrameUvw.Load3(At + 12u));
            VC = asfloat(FrameUvw.Load3(At + 24u));
        }
        const float2 Bary = Q.CommittedTriangleBarycentrics();
        const float  W0   = 1.0f - Bary.x - Bary.y;
        const uint   Face = (uint)(frac(VA.z) * 64.0f + 0.5f);     // twice the key plus one, and the own light's bit
        const bool   bOwn = (C & 0x1000000u) != 0u || (Face & 1u) != 0u;
        const uint   Key  = Face >> 1;
        const uint   Tex  = (Key != 0u) ? RtWater.Load(Row + RDX_RT_GEO_TEXTURES + (Key - 1u) * 4u) : 0u;
        // The face's own picture where the texture heap holds it, as fine as the picture would show it from as far
        // as the view has come to it; the caster's average colour otherwise, and in the measuring view 7.
        float3 Colour = RtAlbedo(C);
        if (Tex != 0u && RtLevel.z != 7u)
        {
            const float2 Uv = VA.xy * W0 + VB.xy * Bary.x + VC.xy * Bary.y;
            uint TW, TH;
            RtTextures[NonUniformResourceIndex(Tex - 1u)].GetDimensions(TW, TH);
            const float Foot  = (length(Sp - Eye) + Q.CommittedRayT()) / max(Focal, 1.0f);
            const float Dense = asfloat(RtWater.Load(Row + RDX_RT_GEO_DENSITY)) * (float)max(TW, TH);
            Colour = RtTextures[NonUniformResourceIndex(Tex - 1u)]
                         .SampleLevel(Samp, Uv, log2(max(Foot * Dense, 1.0f))).rgb;
        }
        float3 L = 1.0f;
        if (!bOwn)
        {
            // A thin thing met from behind - a flag, a leaf - is lit as its other side.
            float3 N = normalize(RtCasterNormal(VA) * W0 + RtCasterNormal(VB) * Bary.x + RtCasterNormal(VC) * Bary.y);
            N = (dot(N, Ray) > 0.3f) ? -N : N;
            // The light the picture itself shows it by: the engine's own on it, where the engine has shown the
            // caster (the row's mark). Where it never has, worked out where the ray met it: the light all round
            // it, and the level's lamps that reach the place and that the place can see.
            if ((C & 0x4000000u) != 0u)
                L = RtCubeLight(Row, N) * LightBoost;
            else
                L = (RtLight(RtWater.Load(Row + RDX_RT_GEO_AMBIENT)) +
                     RtLampLight(RtEye.xyz + (Hit - Eye), N, (C & 0x2000000u) != 0u, 0x03u)) *
                    (asfloat(RtWater.Load(Row + RDX_RT_GEO_SCALE)) * LightBoost);
            L += OwnLampsAt(RtEye.xyz + (Hit - Eye), N, 0u);
        }
        Own = Colour * L;
        // The measuring view 9: which light the caster is shown by - its own (white), the engine's (green), the
        // lamps' worked out where the ray met it (red), or those with next to none reaching it (blue).
        if (RtLevel.z == 9u)
            Own = bOwn ? float3(1.0f, 1.0f, 1.0f)
                       : ((C & 0x4000000u) != 0u) ? float3(0.0f, 1.0f, 0.0f)
                       : (max(L.r, max(L.g, L.b)) > 0.05f) ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
    }
    const float3 Qv = Hit - Eye;
    const float  QZ = dot(Qv, AZ);
    if (bShown && QZ > 1.0f && RtLevel.z != 4u && RtLevel.z != 7u && RtLevel.z != 9u)
    {
        const float2 Uv = (WaterScreen.xy + float2(dot(Qv, AX), dot(Qv, AY)) * (Focal / QZ)) * InvHalfSize * 0.5f;
        if (all(Uv >= 0.0f) && all(Uv <= 1.0f))
        {
            const float Seen = 1.0f / max(DepthCopy.Load(int3(min(Uv * Size, Size - 1.0f), 0)).r, 1e-6f);
            if (abs(Seen - QZ) < max(8.0f, QZ * 0.02f))
            {
                const float2 Edge = saturate(min(Uv, 1.0f - Uv) * 12.0f);
                if (bView)
                    return (Q.CommittedInstanceID() == 0u) ? float4(0.0f, 0.8f, 0.0f, 1.0f) : float4(0.0f, 0.8f, 0.8f, 1.0f);
                // Towards the picture's edges the spot's own colour takes over - or, where its light is not
                // known, nothing does: the image fades out there.
                const float3 Seen3 = SceneCopy.SampleLevel(Samp, Uv, 0.0f).rgb;
                return bLit ? float4(lerp(Own, Seen3, Edge.x * Edge.y), 1.0f) : float4(Seen3, Edge.x * Edge.y);
            }
        }
    }
    // Far off, and not in the picture now - a skyline behind the railing: what the far probe kept in that direction from
    // the camera, which is what the camera saw of that very spot when nothing stood in front of it (the probe keeps only
    // what lies further than RDX_RT_FAR_MIN, where the camera's own few steps shift nothing).
    if (length(Qv) > RDX_RT_FAR_MIN && RtLevel.z != 4u && RtLevel.z != 7u && RtLevel.z != 9u)
    {
        const float4 Probe = FarProbe.SampleLevel(Samp, normalize(Qv), 0.0f);
        if (Probe.a > 0.25f)
            return bView ? float4(0.0f, 0.5f, 1.0f, 1.0f) : float4(Probe.rgb / Probe.a, 1.0f);
    }
    if (bView)
        return (Q.CommittedInstanceID() == 0u) ? float4(0.8f, 0.8f, 0.0f, 1.0f) : float4(0.8f, 0.0f, 0.8f, 1.0f);
    return bLit ? float4(Own, 1.0f) : float4(0.0f, 0.0f, 0.0f, 0.0f);
#endif
}

// What a mirror ray passes on its way to what it meets (Reach units off): the level's see-through surfaces - a pane,
// a grating, a force field's sheet; the rays' own structure of them, which no other ray asks - the nearest first,
// each as the level draws it: a see-through one adds its colour and takes of what lies behind it as much as it
// covers, a multiplier scales it, and a masked one stands solid where its texture is not clear. Each lit where the
// ray passed it, like everything else (RtLampLight). Behind: what the ray met beyond them.
float4 RtThinOver(float3 Eye, float3 Sp, float3 Ray, float Focal, float Reach, float4 Behind)
{
#if RDX_RT_ASK
    // Asked with the ray itself (RDX_RAY_FLAG_THIN): nothing to lay over what the ray met yet.
    return Behind;
#else
    float3 Add  = 0.0f;
    float3 Keep = 1.0f;
    float  From = 2.0f;
    [loop] for (uint Layer = 0u; Layer < RDX_RT_THIN_LAYERS && From < Reach; Layer++)
    {
        // The host's to trace: the surfaces it found on the ray's way, in the order it found them.
        FRtHit Q = RtPassedAt(Layer);
        if (Q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            break;
        const float T = Q.CommittedRayT();
        From = T + 0.5f;
        const uint Surf = LevelMaterial.Load((RtLevel.w + Q.CommittedPrimitiveIndex()) * 4u);
        if (Surf >= RtLevel.y)
            continue;
        const uint  At  = RtLevel.x + Surf * RDX_RT_RECORD_BYTES;
        const uint4 Rec = LevelMaterial.Load4(At);
        if (Rec.z == 0u)
            continue;       // its texture is not in the heap yet
        const uint   Kind  = LevelMaterial.Load(At + RDX_RT_RECORD_KIND);
        const float3 Spot  = RtEye.xyz + (Sp - Eye) + Ray * T;
        const float4 RowU  = asfloat(LevelMaterial.Load4(At + 16u));
        const float4 RowV  = asfloat(LevelMaterial.Load4(At + 32u));
        const float  Dense = asfloat(LevelMaterial.Load(At + 48u));
        const float  Foot  = (length(Sp - Eye) + T) / max(Focal, 1.0f);
        const float4 Tex   = RtTextures[NonUniformResourceIndex(Rec.z - 1u)]
                                 .SampleLevel(Samp, float2(dot(RowU.xyz, Spot) + RowU.w, dot(RowV.xyz, Spot) + RowV.w),
                                              log2(max(Foot * Dense, 1.0f)));
        if ((Kind & RDX_RT_KIND_MASKED) != 0u && Tex.a < 0.5f)
            continue;
        // The measuring view 8: the first see-through surface a ray passes, by how the level draws it - green one
        // laid over what is behind, blue a multiplier, red one that stands solid there.
        if (RtLevel.z == 8u)
            return ((Kind & RDX_RT_KIND_TRANSLUCENT) != 0u) ? float4(0.0f, 0.8f, 0.0f, 1.0f)
                 : (((Kind & RDX_RT_KIND_MODULATED) != 0u) ? float4(0.0f, 0.0f, 0.8f, 1.0f)
                                                           : float4(0.8f, 0.0f, 0.0f, 1.0f));
        if ((Kind & RDX_RT_KIND_MODULATED) != 0u && (Kind & RDX_RT_KIND_TRANSLUCENT) == 0u)
        {
            Keep *= saturate(Tex.rgb * 2.0f);
            continue;
        }
        float3 L = 1.0f;
        if ((Rec.x & 0x1000000u) == 0u && (Kind & RDX_RT_KIND_UNLIT) == 0u)
        {
            float3 N = asfloat(LevelMaterial.Load3(At + RDX_RT_RECORD_NORMAL));
            N = (dot(N, N) < 0.25f) ? -Ray : normalize(N);
            N = (dot(N, Ray) > 0.0f) ? -N : N;
            L = (RtLight(LevelMaterial.Load(At + RDX_RT_RECORD_AMBIENT)) +
                 RtLampLight(Spot, N, (Rec.x & 0x2000000u) != 0u, 0x03u)) *
                (asfloat(RtWater.Load(60u)) * LightBoost) +
                OwnLampsAt(Spot, N, 0u) * OwnRow(3u).z;
        }
        const float3 Colour = Tex.rgb * L;
        Add += Keep * Colour;
        if ((Kind & RDX_RT_KIND_TRANSLUCENT) == 0u)
        {
            Keep = 0.0f;
            break;
        }
        Keep *= saturate(1.0f - Colour);
    }
    const float Through = (Keep.r + Keep.g + Keep.b) / 3.0f;
    return float4(Add + Keep * Behind.rgb, max(Behind.a, 1.0f - Through));
#endif
}

// The mirror image along a ray: what the ray meets (WaterReflectRtSolid), and over it what it passes on the way
// (RtThinOver). bBody: whether the image shows the player's own body - a pane of glass's does, water's does not.
float4 WaterReflectRt(float3 Eye, float3 Sp, float3 Ray, float3 AX, float3 AY, float3 AZ, float Focal, float2 Size,
                      float3 Lit, bool bBody)
{
    float Reach;
    const float4 Behind = WaterReflectRtSolid(Eye, Sp, Ray, AX, AY, AZ, Focal, Size, Lit, bBody, Reach);
    if (RtLevel.z != 0u && RtLevel.z != 8u && RtLevel.z != 10u)
        return Behind;
    return RtThinOver(Eye, Sp, Ray, Focal, Reach, Behind);
}

// The lamps' glints (Reflections=RT): WaterGlints' own, each only where its lamp is in sight of the water point - a ray
// towards it, stopped RT_GLINT_STOP short. The ray is asked only where the glint would show at all. The measuring view
// (RtLevel.z 2): the glints alone, green where they show, red where the level hides them, orange where an actor does.
float3 WaterGlintsRt(float3 Eye, float3 Sp, float3 N, float H)
{
    const float3 ToEye = normalize(Eye - Sp);
    float3 Sum = 0.0f;
    const int Count = (int)WaterLampCount.x;
    [loop] for (int i = 0; i < Count; i++)
    {
        const float4 P    = WaterLamp[i * 2];
        const float3 At   = float3(P.xy, H + P.z);          // in the frame around the camera: z above the surface
        const float3 D    = At - Sp;
        const float  Dist = length(D);
        if (At.z <= 0.0f || Dist >= 2.0f * P.w)
            continue;
        const float3 Half  = normalize(D / Dist + ToEye);
        const float  Fall  = saturate(2.0f - Dist / P.w);
        const float  Back  = 0.02f + 0.98f * pow(1.0f - saturate(dot(Half, ToEye)), 5.0f);
        const float3 Glint = WaterLamp[i * 2 + 1].rgb * (pow(saturate(dot(N, Half)), 800.0f) * 32.0f * Back * Fall);
        if (max(Glint.r, max(Glint.g, Glint.b)) < 0.002f)
            continue;
        if (Dist > RT_GLINT_STOP + 4.0f)
        {
            // The host's to trace: a request of its own in the pixel's block - written down, or its answer taken.
            FRtHit Q = RtAsk(RDX_RAY_KIND_ANY, RtEye.xyz + (Sp - Eye), D / Dist, 2.0f, Dist - RT_GLINT_STOP, 0x03u, 0u);
            if (Q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            {
                if (RtLevel.z == 2u)
                    Sum += ((Q.CommittedInstanceID() == 0u) ? float3(1.0f, 0.0f, 0.0f) : float3(1.0f, 0.5f, 0.0f)) *
                           max(Glint.r, max(Glint.g, Glint.b));
                continue;
            }
        }
        Sum += (RtLevel.z == 2u) ? float3(0.0f, 1.0f, 0.0f) * max(Glint.r, max(Glint.g, Glint.b)) : Glint;
    }
    return Sum;
}
#endif

// The slope of the water's surface at a point: the water texture's own waves where it has them - the very field that
// moves its picture, made real, so the view bends with the waves the texture shows - or gentle computed ones where it
// has none; and the ripples things make, at the point's place in the world. SeenH and SeenIn: the ripples' height
// there, and whether the point is in their field at all; RippleSlope the ripples' share of the slope alone.
//
// The waves' slope as the pixel sees it on average rather than at one point of it (WaterSlopeFilter: on wherever the
// water runs, off only with the marker SecondRevision-water-slope-filter-off.txt, for a measurement). Where a pixel spans
// many of the waves' cells - far off, and seen flat - one point of the slopes under it flickers from frame to frame:
// glints finer than a pixel, which a temporal upscaler smooths away with their light (a darker reflection), and in
// motion shows as grain; without one they flicker as they are. WaterSlopeFilter is how early the averaging starts: 1
// from one cell a pixel, 3 from a cell three pixels wide. The mean alone is a flatter mirror that loses the glints
// mirroring the lights; what it takes away the mirror image spreads over instead (WaterSlopeSpread).
// DUx, DUy: the texture coordinate's change from one pixel to the next, taken before the caller's clip.
#define WaterSlopeFilter WaterSpare.z
float WaterSlopeKeep(float2 DUx, float2 DUy, float2 Cells)
{
    const float Lx = length(DUx * Cells);
    const float Ly = length(DUy * Cells);
    return rsqrt(max(Lx, 1.0f) * max(Ly, 1.0f));
}

// What WaterSlopeKeep takes from the slope is not gone: the pixel still sees all of those slopes, only no longer one at a
// time. Its mirror image is theirs spread out - the lights drawn out into soft streaks, as on real water far off - and not
// the flat mirror the kept mean alone gives, which loses the glints that mirror the lights and comes out darker.
// SpreadU, SpreadV: one standard deviation of the lost slope along the texture's U and V directions, in the world's slope
// (as WaterSlopeAt's); zero where nothing is averaged, or where the run has no wave field of its own.
void WaterSlopeSpread(float2 DUx, float2 DUy, out float2 SpreadU, out float2 SpreadV)
{
    SpreadU = 0.0f;
    SpreadV = 0.0f;
    if (WaterSlopeFilter <= 0.0f || WaterWave.x < 1.0f)
        return;
    float2 FSize;
    WaveField.GetDimensions(FSize.x, FSize.y);
    const float Keep = WaterSlopeKeep(DUx * WaterSlopeFilter, DUy * WaterSlopeFilter, FSize);
    const float Lost = sqrt(saturate(1.0f - Keep * Keep)) * (WaterWave.x - 1.0f) * WaterWave.y;
    SpreadU = Lost * FSize.x / WaterWave.z * WaterDirs.xy;
    SpreadV = Lost * FSize.y / WaterWave.w * WaterDirs.zw;
}

// The rain's rings. Where the game's rain is falling into the water around the camera, a drop leaves a
// ring that runs out a hand or two and is gone within a second - far finer than the ripples' field can hold, so the
// rings are worked out here: the level cut into cells, in each cell a drop now and then, about as often as the game's
// rain puts one there, at a new place every time, its ring a crest running outwards and fading. Two such nets of
// different sizes and beats, so that no grid shows. Only as far as the game's rain reaches around the player
// (RAIN_RING_FAR), and only where a pixel is finer than a ring's crest: farther off a ring is a speck that would only
// flicker. WaterRainOn: how thick the rain falls into the water, 0 none to 1 (a quarter of it rides on the run's
// second spare number, whose whole part is another flag). Returned as a slope of the surface, as the ripples' is.
#define WaterRainOn        (frac(WaterSpare.y) * 4.0f)
#define RAIN_RING_REACH    12.0f      // how far a ring runs, in the level's units
#define RAIN_RING_WIDTH    2.5f       // its crest's width
#define RAIN_RING_SECONDS  0.7f       // how long it lasts
#define RAIN_RING_STEEP    0.4f       // the crest's slope, before the bell's own 0.86
#define RAIN_RING_FAR      640.0f
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
// One net's ring at a point: Cell the cells' size, Beat the seconds between two drops in a cell, Salt the net's own
// number. The drop lands in the cell's middle part, so its ring stays inside the cell.
float2 RainRing(float2 World, float Time, float Cell, float Beat, int Salt)
{
    const float2 P    = World / Cell;
    const int2   Id   = (int2)floor(P);
    const float  T    = Time / Beat + RainDice(Id, Salt).x;       // every cell keeps its own time
    const float  Age  = frac(T) * Beat / RAIN_RING_SECONDS;       // 0 as the drop goes in, 1 as its ring is gone
    const float2 Mid  = (float2)Id + 0.3f + 0.4f * RainDice(Id, Salt + 1 + (int)floor(T));
    const float2 D    = (P - Mid) * Cell;
    const float  R    = length(D);
    const float  X    = (R - Age * RAIN_RING_REACH) / RAIN_RING_WIDTH;
    // At its full height while it runs out, and gone over the last part of the way: a ring that faded from the start
    // would be seen only as the dot it begins as.
    const float  Live = saturate((1.0f - Age) * 2.5f);
    return D / max(R, 0.01f) * (-2.0f * X * exp(-X * X) * Live * RAIN_RING_STEEP);
}
float2 WaterRainRings(float2 World, float Time)
{
    // How much of the water a pixel covers there, along the view: the farther and the flatter it is seen, the more.
    const float2 Off  = World - WaterCamera.xy;
    const float  Flat = dot(Off, Off);
    const float  High = max(abs(WaterExtra.x), 8.0f);
    const float  Foot = (Flat + High * High) / (max(WaterView.y, 1.0f) * High);
    const float  Keep = saturate(1.5f - Foot / RAIN_RING_WIDTH) *
                        saturate((RAIN_RING_FAR * RAIN_RING_FAR - Flat) / (0.4f * RAIN_RING_FAR * RAIN_RING_FAR));
    if (Keep <= 0.0f)
        return 0.0f;
    return (RainRing(World, Time, 62.0f, 1.5f, 0) + RainRing(World, Time, 80.0f, 0.9f, 4096)) * (Keep * WaterRainOn);
}

// An ornamental fountain's pool (WaterWave.x at RdxMath::FountainPoolSheet: a fountain's water comes down into
// it). The foot of the fountain's curtain - its lowest edge, from WaterDirs.xy to WaterDirs.zw on the level - sends
// rings out over the pool, and the curtain's light, from its top WaterWave.y over the pool, lies on the water.
#define PoolKind        ((WaterWave.x < -0.5f && WaterWave.x > -1.5f) ? 1.0f : 0.0f)
#define PoolRise        max(WaterWave.y, 4.0f)
#define PoolSpread      WaterWave.z                     // how far either side of the foot's line the water comes down
#define POOL_RING_FINE  0.9f                            // the finest crest's width, in the level's units
#define POOL_LIGHT      float3(0.80f, 0.90f, 1.0f)
// One row of drops' rings at a point of the pool, S along the foot and D across it: a drop every Pitch units of the
// foot and every Beat seconds, landing up to Wide to either side of it; its ring runs out Reach units in Life seconds,
// Width its crest's. Long is the foot's length. Returned along and across the foot.
float2 PoolRings(float S, float D, float Long, float Time, float Pitch, float Beat, float Wide, float Reach,
                 float Width, float Life, int Salt)
{
    float2    Sum = 0.0f;
    const int Id  = (int)floor(S / Pitch);
    // A ring reaches into the cells beside its own and no further: Reach is never much more than Pitch.
    [unroll] for (int k = -1; k <= 1; k++)
    {
        const int    Cell = Id + k;
        const float  T    = Time / Beat + RainDice(int2(Cell, Salt), 0).x;      // every cell keeps its own time
        const float  Age  = frac(T) * Beat / Life;                              // 0 as the drop goes in, 1 as it ends
        const float2 Dice = RainDice(int2(Cell, Salt), 1 + (int)floor(T));
        const float2 Mid  = float2(((float)Cell + 0.15f + 0.7f * Dice.x) * Pitch, (Dice.y * 2.0f - 1.0f) * Wide);
        const float2 Off  = float2(S, D) - Mid;
        const float  R    = length(Off);
        const float  X    = (R - Age * Reach) / Width;
        // Only under the curtain: past the foot's ends no drop falls.
        const float  Live = saturate((1.0f - Age) * 2.5f) * step(0.0f, Mid.x) * step(Mid.x, Long);
        Sum += Off / max(R, 0.01f) * (-2.0f * X * exp(-X * X) * Live);
    }
    return Sum;
}
// The slope the fountain gives its pool's surface at a place of the level.
float2 PoolSlope(float2 World, float Time)
{
    // How much of the water a pixel covers there, along the view (as WaterRainRings): finer rings than that are left.
    const float2 Off  = World - WaterCamera.xy;
    const float  Flat = dot(Off, Off);
    const float  High = max(abs(WaterExtra.x), 8.0f);
    const float  Foot = (Flat + High * High) / (max(WaterView.y, 1.0f) * High);
    const float  Keep = saturate(1.5f - Foot / POOL_RING_FINE);
    if (Keep <= 0.0f)
        return 0.0f;
    const float2 Line = WaterDirs.zw - WaterDirs.xy;
    const float  Long = max(length(Line), 1.0f);
    const float2 Dir  = Line / Long;
    const float2 Nrm  = float2(-Dir.y, Dir.x);
    const float2 Rel  = World - WaterDirs.xy;
    const float  S    = dot(Rel, Dir);
    // Across the foot, from where the water comes down on that side: a slab's own footprint is no part of the pool.
    const float  Side = dot(Rel, Nrm);
    const float  D    = sign(Side) * max(abs(Side) - PoolSpread, 0.0f);
    // The water comes down all along the foot, here more of it and there less: waves run out from it to both sides
    // and round its ends, their crests uneven along it; right under it the water seethes with small quick rings,
    // and here and there a heavier fall sends out a wider one.
    const float2 Out   = float2(S - clamp(S, 0.0f, Long), D);
    const float  Far   = length(Out);
    const float  Bend  = FieldNoise(float2(S * 0.11f, Time * 0.35f)) * 5.0f;
    const float  Swell = 0.55f + 0.45f * FieldNoise(float2(S * 0.21f + 7.0f, Time * 0.5f));
    const float  Wave  = cos(Far * 1.25f - Time * 9.0f + Bend) * exp(-Far / 16.0f) * saturate(Far / 1.5f);
    float2 Slope = Out / max(Far, 0.01f) * (Wave * Swell * 0.30f);
    Slope += PoolRings(S, D, Long, Time, 4.5f, 0.42f, 1.5f, 5.0f, POOL_RING_FINE, 0.40f, 11) * 0.30f;
    Slope += PoolRings(S, D, Long, Time, 6.0f, 0.55f, 2.0f, 6.5f, 1.0f, 0.50f, 23) * 0.25f;
    Slope += PoolRings(S, D, Long, Time, 10.0f, 1.10f, 2.5f, 11.0f, 1.2f, 0.90f, 37) * 0.35f;
    return (Slope.x * Dir + Slope.y * Nrm) * Keep;
}
// The light of the fountain's curtain on its pool. The curtain is lit from its top (FountainSurface), and that light
// lies on the water under it: a glint on every slope that turns it to the eye, and a little of it on the water
// itself, strongest at the foot. N the surface's normal, ToEye the way to the camera, both in the level's axes.
float3 PoolLight(float2 World, float3 ToEye, float3 N)
{
    const float2 Line   = WaterDirs.zw - WaterDirs.xy;
    const float  Long   = max(length(Line), 1.0f);
    const float2 Dir    = Line / Long;
    // The lamp: over the nearest place of the foot, where the water leaves its slot.
    const float2 Rel    = World - WaterDirs.xy;
    const float2 Nrm    = float2(-Dir.y, Dir.x);
    const float  Side   = dot(Rel, Nrm);
    const float2 Near   = WaterDirs.xy + Dir * clamp(dot(Rel, Dir), 0.0f, Long) +
                          Nrm * (sign(Side) * min(abs(Side), PoolSpread));
    const float3 ToLamp = float3(Near - World, PoolRise);
    const float  Dist   = length(ToLamp);
    const float3 L      = ToLamp / Dist;
    const float  Glint  = pow(saturate(dot(N, normalize(L + ToEye))), 90.0f);
    const float  Fall   = 1.0f / (1.0f + Dist * Dist / (PoolRise * PoolRise));
    return POOL_LIGHT * ((Glint * 1.6f + 0.06f * L.z) * Fall);
}

// An ornamental fountain's chute (WaterWave.x at RdxMath::FountainChuteSheet): a lying piece of the fountain's own
// surface, sloping or level, that its water runs down to the slot the curtain falls from. Water as the level's
// other water is - its surface really lifted, point by point, see-through, mirroring and glinting as that is, with
// no light of its own - but running water: its surface is uneven as a sea's is, in humps and hollows of no
// pattern, and the water carries all of them down the chute. Nothing on it is a wave with a way of its own. What
// rides with the water goes by Tau, the seconds a place's water has left to the slot (at most zero) - one measure
// for all of a chute's pieces, so what the water carries passes from one to the next unbroken, drawn out where a
// slope speeds the water up.
#define ChuteOn         (WaterWave.x < -1.5f)
#define ChuteEndTime    WaterWave.y                     // Tau at the piece's lower end
#define ChuteRate       WaterWave.z                     // seconds the water takes over a unit of the level
#define ChuteWay        WaterDirs.xy                    // which way the water runs, on the level
#define ChuteDrop       WaterDirs.z                     // how much the piece drops for a unit along that
#define ChuteEndS       WaterDirs.w                     // the piece's lower end along that way
#define CHUTE_HIGH      0.5f                            // how far the running water's surface rises and sinks
// The lowest the surface may sink under the game's own sheet: the chutes' stone lies a little deeper than this
// under it, and a point under the stone is a hole in the water.
#define CHUTE_DEEP      0.75f
#define CHUTE_STEP      0.5f                            // how far apart the lifted surface's points may be there
// The unevenness, in four sizes laid over one another. Each: how many of its humps go to a unit across the chute,
// and how many to a second of Tau - fewer along the run than across even where the water is at its slowest
// (RdxMath::ChuteLevelSpeed), so a hump is longer the way the water goes than it is wide, never a ridge across
// the chute; how much of the water's way it is carried - the largest all of it, the smaller ones a little less,
// so the surface keeps changing as it goes down; and how much of the whole it is.
#define CHUTE_SIZES 4
static const float4 ChuteSize[CHUTE_SIZES] = {
    float4(0.14f, 0.66f, 1.00f, 0.34f), float4(0.19f, 0.89f, 0.93f, 0.28f),
    float4(0.33f, 1.55f, 0.87f, 0.24f), float4(0.65f, 3.06f, 0.80f, 0.14f) };
// The turn each size's grid of humps is laid at (its cosine and sine), to one side and the other by turns: no row
// of any grid lies across the chute or along it, and the humps lean to neither side on the whole.
static const float2 ChuteTurn[CHUTE_SIZES] = {
    float2(0.857f, 0.515f), float2(0.788f, -0.616f), float2(0.530f, 0.848f), float2(0.956f, -0.292f) };
// What the sizes' sum is divided by: with it the surface all but never leaves CHUTE_HIGH either way.
#define CHUTE_FULL      0.30f
// Where a place of the level is on the chute: x across it, in the level's units, y its Tau.
float2 ChutePlace(float2 World)
{
    return float2(dot(World, float2(-ChuteWay.y, ChuteWay.x)),
                  ChuteEndTime - (ChuteEndS - dot(World, ChuteWay)) * ChuteRate);
}
// The chute's surface at a place of it (ChutePlace): z how high it stands over its piece, in the level's units -
// what the surface's points are lifted by (DSWater) - and xy how that height changes over a unit across the chute
// and over a unit along it: one surface for both, so the shading is the surface's that is there. Foot: how much of
// the water a pixel covers along the view - a size finer than that is left out; 0 takes them all.
float3 ChuteSurface(float2 Place, float Foot)
{
    const float T = WaterView.x;
    float3 Sum = 0.0f;
    [unroll] for (int i = 0; i < CHUTE_SIZES; i++)
    {
        const float4 Z = ChuteSize[i];
        const float2 C = ChuteTurn[i];
        // The place in the water's own measure - it comes down with the water - counted in this size's humps.
        const float2 Own  = float2(Place.x * Z.x, (Place.y - Z.z * T) * Z.y) + 23.7f * i;
        const float3 N    = FieldHumps(float2(C.x * Own.x - C.y * Own.y, C.y * Own.x + C.x * Own.y));
        // The slope back from the turned grid's two ways to across the chute and along it.
        const float2 Back = float2(C.x * N.x + C.y * N.y, C.x * N.y - C.y * N.x);
        // The finer of the size's two ways on the level, against what a pixel covers.
        const float  Keep = Z.w * saturate(1.5f - 0.5f * Foot * 3.14f * max(Z.x, Z.y * ChuteRate));
        Sum += Keep * float3(Back.x * Z.x, Back.y * Z.y * ChuteRate, N.z);
    }
    Sum *= CHUTE_HIGH / CHUTE_FULL;
    // At the lowest it may sink, the surface lies flat.
    return (Sum.z > -CHUTE_DEEP) ? Sum : float3(0.0f, 0.0f, -CHUTE_DEEP);
}
float ChuteHeight(float2 Place)
{
    return ChuteSurface(Place, 0.0f).z;
}
// The slope of the chute's lifted surface at a place of the level, over the piece's own (ChuteTilt). Finer than a
// pixel its humps are left out, as the pool's rings are (PoolSlope).
float2 ChuteSlope(float2 World)
{
    const float2 Off  = World - WaterCamera.xy;
    const float  High = max(abs(WaterExtra.x), 8.0f);
    const float  Foot = (dot(Off, Off) + High * High) / (max(WaterView.y, 1.0f) * High);
    const float3 Surf = ChuteSurface(ChutePlace(World), Foot);
    return Surf.x * float2(-ChuteWay.y, ChuteWay.x) + Surf.y * ChuteWay;
}
// A piece's own slope on the level: it drops ChuteDrop for a unit along its way. Nothing for water that lies level.
float2 ChuteTilt()
{
    return ChuteOn ? -ChuteDrop * ChuteWay : float2(0.0f, 0.0f);
}

float2 WaterSlopeAt(float2 Uv, float2 World, float Time, float2 DUx, float2 DUy, out float SeenH, out float SeenIn,
                    out float2 RippleSlope)
{
    float2 Slope;
    float2 FSize = 64.0f;      // the computed waves' finest detail, in cells of one repeat
    if (WaterWave.x > 0.5f)
    {
        WaveField.GetDimensions(FSize.x, FSize.y);
        const float2 Du = 1.0f / FSize;
        const float  Hx = WaveField.SampleLevel(Samp, Uv + float2(Du.x, 0.0f), 0.0f).r -
                          WaveField.SampleLevel(Samp, Uv - float2(Du.x, 0.0f), 0.0f).r;
        const float  Hy = WaveField.SampleLevel(Samp, Uv + float2(0.0f, Du.y), 0.0f).r -
                          WaveField.SampleLevel(Samp, Uv - float2(0.0f, Du.y), 0.0f).r;
        // The field's swing (0.5 either way) is the waves' height; a step of one cell is 1/FSize of a repeat.
        const float  SU = Hx * 2.0f * WaterWave.y / (2.0f * Du.x * WaterWave.z);
        const float  SV = Hy * 2.0f * WaterWave.y / (2.0f * Du.y * WaterWave.w);
        Slope = SU * WaterDirs.xy + SV * WaterDirs.zw;
    }
    else
        Slope = WaterSlope(Uv, Time, WaterTint.a);
    if (WaterSlopeFilter > 0.0f)
        Slope *= WaterSlopeKeep(DUx * WaterSlopeFilter, DUy * WaterSlopeFilter, FSize);
    SeenH       = 0.0f;
    SeenIn      = 0.0f;
    RippleSlope = 0.0f;
    const float2 R = (World - WaterScreen.zw) / RDX_RIPPLE_WORLD;
    if (all(R > 0.0f) && all(R < 1.0f))
    {
        const float  Du = 1.0f / RDX_RIPPLE_TEXELS;
        SeenH  = Ripples.SampleLevel(Samp, R, 0.0f).x;
        SeenIn = 1.0f;
        const float2 G  = float2(Ripples.SampleLevel(Samp, R + float2(Du, 0.0f), 0.0f).x -
                                 Ripples.SampleLevel(Samp, R - float2(Du, 0.0f), 0.0f).x,
                                 Ripples.SampleLevel(Samp, R + float2(0.0f, Du), 0.0f).x -
                                 Ripples.SampleLevel(Samp, R - float2(0.0f, Du), 0.0f).x);
        // The field fades out towards its edges, where it ends.
        const float2 Edge = saturate(min(R, 1.0f - R) * 16.0f);
        RippleSlope = G / (2.0f * RDX_RIPPLE_WORLD / RDX_RIPPLE_TEXELS) * (Edge.x * Edge.y);
        Slope += RippleSlope;
    }
    // The rain's rings count as the ripples do, to whoever asks for the ripples' share.
    if (WaterRainOn > 0.0f)
    {
        const float2 Rain = WaterRainRings(World, Time);
        RippleSlope += Rain;
        Slope       += Rain;
    }
    // And so do the rings a fountain sends over its pool.
    if (PoolKind > 0.5f)
    {
        const float2 Rings = PoolSlope(World, Time);
        RippleSlope += Rings;
        Slope       += Rings;
    }
    return Slope;
}

// The mirror image (Reflections=High and up) at a point of a water surface - the see-through water's (WaterSurface) and
// the sludge's (SludgeMirror) alike: the view mirrored by the surface's normal, so the waves and the ripples break it
// up, and followed through the copy of the picture (WaterReflect) or traced (Reflections=RT, WaterReflectRt). A wave
// steep enough to send the mirrored view back into the water sends it along the surface instead. Eye, Sp, Into, AX, AY,
// AZ: the frame around the camera (WaterSurface); N the surface's normal on the camera's side (Up), Slope its slope;
// World the point's place in the level; DUx, DUy the texture coordinate's change from one pixel to the next; Lit the
// light the surface receives; Tir 1 where, from below, the surface is a full mirror. rgb: the image, a: how much of it
// to trust.
float4 WaterMirror(float3 Eye, float3 Sp, float3 Into, float3 N, float2 Slope, float Up, float2 World, float2 DUx,
                   float2 DUy, float3 AX, float3 AY, float3 AZ, float Focal, float2 Size, float3 Lit, float Tir)
{
    float4 Mirror = 0.0f;
    // Over the slopes the pixel spans (WaterSlopeSpread): four views along the slope towards and away from the camera, at
    // -1.5, -0.5, 0.5 and 1.5 of its standard deviation, and their mean - one view where nothing was averaged. That slope
    // tilts the mirrored view up and down, which draws a light out into a streak; a sideways one would put copies of it
    // beside each other, and far off, seen flat, it moves the mirrored view hardly at all. Always the same four, so the
    // picture holds still from frame to frame.
    float2 SpreadU, SpreadV;
    WaterSlopeSpread(DUx, DUy, SpreadU, SpreadV);
    float2 Ahead = World - WaterCamera.xy;
    Ahead = (dot(Ahead, Ahead) > 1e-6f) ? normalize(Ahead) : float2(1.0f, 0.0f);
    const float SpreadAhead = sqrt(dot(SpreadU, Ahead) * dot(SpreadU, Ahead) + dot(SpreadV, Ahead) * dot(SpreadV, Ahead));
    const uint  Views       = (SpreadAhead > 1e-3f) ? 4u : 1u;
#if RDX_RT
    // Seen from above, steeply, the surface gives back hardly anything of the mirror image (the sheen: 2% head-on,
    // RT_WATER_MIN_BACK at about sixty degrees): no ray is traced there - most of a pool seen from its edge - and the light
    // the surface receives stands in, as where a ray finds nothing. Faded in over a narrow band, so no edge shows where the
    // rays begin. From below the water, beyond the critical angle, the mirror is all there is: always traced.
    float Trace = 1.0f;
    if (Tir <= 0.0f && (RtLevel.z == 0u || RtLevel.z == 10u))
    {
        const float3 ToEyeW = ChuteOn ? normalize(Eye - Sp)
                                      : normalize(float3(WaterCamera.xy - World, WaterExtra.x));
        const float  Sheen  = 0.02f + 0.98f * pow(1.0f - saturate(dot(normalize(float3(-Slope, 1.0f)), ToEyeW)), 5.0f);
        Trace = saturate((Sheen - RT_WATER_MIN_BACK) / RT_WATER_MIN_BACK);
        // The mirror image shows by the sheen at the most.
        RtShows(Sheen);
    }
#endif
    [loop] for (uint v = 0u; v < Views; v++)
    {
        const float2 Tilt = Ahead * (SpreadAhead * ((float)v - 1.5f));
        const float3 NV   = (Views == 1u) ? N : normalize(float3(-(Slope + Tilt) * Up, Up));
        float3 Ray = reflect(Into, NV);
        if (ChuteOn)
        {
            // A chute's piece may slope: not into the piece - along it.
            const float3 Lies = normalize(float3(-ChuteTilt() * Up, Up));
            const float  Off  = dot(Ray, Lies);
            if (Off < 0.02f)
                Ray = normalize(Ray + Lies * (0.02f - Off));
        }
        else if (Ray.z * Up < 0.02f)
            Ray = normalize(float3(Ray.xy, 0.02f * Up));
#if RDX_RT
        if (Trace > 0.0f)
            Mirror += WaterReflectRt(Eye, Sp, Ray, AX, AY, AZ, Focal, Size, Lit, false);
#else
        Mirror += WaterReflect(Eye, Sp, Ray, AX, AY, AZ, Focal, Size);
#endif
    }
    Mirror /= (float)Views;
#if RDX_RT
    Mirror.a *= Trace;
#endif
    return Mirror;
}

float3 WaterSurface(VSOut In, out float3 Added)
{
    // Only the nearest of the water's own points at a pixel: the surface writes no depth, so where it stands in front
    // of itself - a crest before the trough behind it - whichever was drawn later would show. Each run is first gone
    // over to mark that nearest point in the water's own depth (PSWaterMark), and what lies behind it is dropped here.
#if RDX_RT_HOST
    // Before any pixel is thrown away: RtFind reads the two by two together.
    RtFind(In.Pos);
#endif
#if RDX_RT_ASK
    // No pixel to throw away in the build that only asks: what is dropped here asks nothing (RtAskFlush).
    if (WaterSurfaceMarked && asuint(In.Pos.z) < WaterSurfaceDepth[uint2(In.Pos.xy)])
        GRtAskDead = true;
#else
    if (WaterSurfaceMarked && asuint(In.Pos.z) < WaterSurfaceDepth[uint2(In.Pos.xy)])
        discard;
#endif
    float2 Size;
    SceneCopy.GetDimensions(Size.x, Size.y);
    const float2 ScreenUv = In.Pos.xy / Size;
    const float  SurfaceZ = 1.0f / max(In.Pos.z, 1e-6f);      // the vertex shader carries the view depth as 1/z
    const float  Time     = WaterView.x;
    const float  Focal    = WaterView.y;                        // the engine's projection, in its own pixels
    const float  Absorb   = WaterView.z;                        // per world unit; 0 when seen from under the water
    const float  Shore    = WaterView.w;                        // world units over which the texture comes in

    // The texture coordinate's change from pixel to pixel, for the waves' slope (WaterSlopeAt) - before the clip, after which
    // the neighbours it is taken from may be gone.
    const float2 DUx = ddx(In.Uv);
    const float2 DUy = ddy(In.Uv);
    // The surface, exactly as the translucent draw makes it (PSMain).
    float4 T = NoSmooth ? Tex.Sample(SampPoint, In.Uv) : Tex.SampleBias(Samp, In.Uv, MipBias);
#if RDX_RT_ASK
    GRtAskDead = GRtAskDead || T.a < 0.5f;
#else
    clip(T.a - 0.5f);
#endif
    // The light the surface receives - before the lamps' boost, as the light target holds a surface's (PSMainLit).
    const float3 Received = In.Colour.rgb * Light.Sample(Samp, In.Lv).rgb * RDX_LIGHT_SCALE;
    const float3 Lit = Received * LightBoost;
    const float3 Src = T.rgb * Lit;

    // Straight through: what lies behind, and how much water is in between.
    const float Behind0 = 1.0f / max(DepthCopy.Load(int3(In.Pos.xy, 0)).r, 1e-6f);
    const float Depth0  = max(Behind0 - SurfaceZ, 0.0f);

    // The pixel back in the world: its view-space place from the screen and its depth, then the view's axes. The screen's
    // place without the upscaler's jitter - the engine's projection has none.
    const float2 Eng   = (In.Pos.xy / Size - JitterUv()) * (2.0f / InvHalfSize);
    const float2 View  = (Eng - WaterScreen.xy) * (SurfaceZ / Focal);
    const float2 World = WaterCamera.xy + View.x * WaterCamera.zw + View.y * WaterAxes.xy + SurfaceZ * WaterAxes.zw;
    // The slope of the surface there, the texture's waves and the ripples (WaterSlopeAt).
    float  SeenH, SeenIn;
    float2 RippleSlope;
    // A fountain's chute has its running water's slope (ChuteSlope) and lies on a slope of its own (Tilt); all
    // other water lies level, under its waves and ripples.
    float2 Waves;
    [branch] if (ChuteOn)
    {
        Waves       = ChuteSlope(World);
        SeenH       = 0.0f;
        SeenIn      = 0.0f;
        RippleSlope = Waves;
    }
    else
        Waves = WaterSlopeAt(In.Uv, World, Time, DUx, DUy, SeenH, SeenIn, RippleSlope);
    const float2 Tilt  = ChuteTilt();
    const float2 Slope = Waves + Tilt;

    // The refraction, by Snell's law. In a frame around the camera - x and y the world's, measured from the camera, z the
    // height above the water - the view ray meets the surface at Sp, bends there by the surface's normal (the waves tilt
    // it), and goes on until it is as deep as whatever the straight ray found behind the surface: that is where what is
    // seen through the water lies. A floor looks higher and nearer than it is, and an edge that goes into the water
    // breaks at the waterline. Once more from where the first guess landed, for a floor that is not level.
    const float  H    = WaterExtra.x;                      // the camera's height above the surface; below it, negative
    const float3 AX   = float3(WaterCamera.zw, WaterExtra.y);
    const float3 AY   = float3(WaterAxes.xy,   WaterExtra.z);
    const float3 AZ   = float3(WaterAxes.zw,   WaterExtra.w);
    const float3 Eye  = float3(0.0f, 0.0f, H);
    const float3 Sp   = Eye + View.x * AX + View.y * AY + SurfaceZ * AZ;
    const float3 Into = normalize(Sp - Eye);
    const float  Up   = (H >= 0.0f) ? 1.0f : -1.0f;        // the normal faces the camera's side
    const float3 N    = normalize(float3(-Slope * Up, Up));
    const float3 Bent = refract(Into, N, (H >= 0.0f) ? (1.0f / 1.333f) : 1.333f);
    // The straight ray's own end, and how deep under the surface (above it, from below) that is: the shoreline's
    // measure - vertical, so a pool's upright wall gets no band along it, and a sloping shore a soft edge.
    const float3 B0     = Eye + View.x * (Behind0 / SurfaceZ) * AX + View.y * (Behind0 / SurfaceZ) * AY + Behind0 * AZ;
    const float  Shallow = (Behind0 < 1.0e5f) ? max((dot(Tilt, B0.xy) - B0.z) * Up, 0.0f) : 1.0e5f;
    float2 Uv     = ScreenUv;
    float  Behind = Behind0;
    float  Path   = Depth0;                                // how far the view goes through water
    float  Tir    = 0.0f;                              // 1: from below, beyond the critical angle - the surface is a mirror
    if (dot(Bent, Bent) > 0.0f && Behind0 < 1.0e5f)
    {
        float2 Guess  = ScreenUv;
        float  Far    = Behind0;
        float  Along0 = Depth0;                            // the bent ray, as deep as the straight view's own end
        [unroll] for (int k = 0; k < 2; k++)
        {
            // Where the straight view at Guess ends, and how far beyond the surface that is. Guess is a place in the copy,
            // jittered as the world was drawn; the engine's projection is not.
            const float2 GEng = (Guess - JitterUv()) * (2.0f / InvHalfSize);
            const float3 B    = Eye + (GEng.x - WaterScreen.x) * (Far / Focal) * AX +
                                (GEng.y - WaterScreen.y) * (Far / Focal) * AY + Far * AZ;
            const float  Beyond = max((dot(Tilt, B.xy) - B.z) * Up, 0.0f);
            // The second look, and what the first guess landed on is not under the water - a wall over it. A ray
            // followed that far would come back to the surface itself, and show the bed with no water over it.
            if (k > 0 && Beyond <= 0.0f)
            {
                Far = 0.0f;
                break;
            }
            // The bent ray, as far as that depth - and back onto the screen.
            const float  Along = Beyond / max((dot(Tilt, Bent.xy) - Bent.z) * Up, 0.05f);
            if (k == 0)
                Along0 = Along;
            const float3 E     = Sp + Bent * Along;
            const float3 Q     = E - Eye;
            const float  QZ    = max(dot(Q, AZ), 1.0f);
            const float2 QEng  = WaterScreen.xy + float2(dot(Q, AX), dot(Q, AY)) * (Focal / QZ);
            Guess = QEng * InvHalfSize * 0.5f + JitterUv();
            if (any(Guess < 0.0f) || any(Guess > 1.0f))
            {
                Far = 0.0f;     // off the picture: nothing there to show - the straight ray below
                break;
            }
            Far   = 1.0f / max(DepthCopy.Load(int3(min(Guess * Size, Size - 1.0f), 0)).r, 1e-6f);
            Path  = Along;
            // Something in front of the surface there - the pool's rim, hiding the bed the bent ray ends on: no
            // second look from that.
            if (Far < SurfaceZ)
                break;
        }
        // Where the bent view lands on something in FRONT of the surface - a post standing in the water, the shore - or
        // off the picture, that must not show under the water: there the view goes straight through. The water it is
        // seen through is the bent ray's all the same, down to that depth: by the straight ray's own long way through
        // it a band along the near shore would stand out darker than the bed beside it.
        if (Far >= SurfaceZ)
        {
            Uv     = Guess;
            Behind = Far;
        }
        else
            Path = min(Along0, Depth0);
    }
    else if (dot(Bent, Bent) <= 0.0f)
        Tir = 1.0f;
    // The mirror image (Reflections=High and up): from above always, as the sheen below; from under the water where the
    // surface is a full mirror (WaterMirror).
    float4 Mirror = 0.0f;
    if (WaterSpare.x > 0.5f && (H > 0.0f || Tir > 0.0f))
    {
        Mirror = WaterMirror(Eye, Sp, Into, N, Slope, Up, World, DUx, DUy, AX, AY, AZ, Focal, Size, Lit, Tir);
#if RDX_RT
        if (RtLevel.z == 1u || RtLevel.z == 3u || RtLevel.z == 6u || RtLevel.z == 11u)
        {
            Added = 0.0f;
            return Mirror.rgb;
        }
#endif
    }
    // What lies behind the surface, as the copy has it - with the caustics on it already, where the water runs with
    // them (FBackend::LayCaustics): they are light on the bed, and the surface shows them bent with it.
    const float3 Seen0 = SceneCopy.SampleLevel(Samp, Uv, 0.0f).rgb;
    // The deeper, the more of the water's own light instead: the light that reaches the surface (its light map), in
    // the water's hue - so the sea stays dark at night and a lit pool shows its colour. Red goes first, blue last.
    // Kept apart as they are added up, for the shadows (WaterShade): the bed seen through the water takes the shadow of
    // its own place, the water's own light the surface's, and what comes to the eye off the surface - the mirror image,
    // the fountain's light, the glints - none.
    const float3 Through = exp(-Path * Absorb * float3(1.35f, 1.0f, 0.75f));
    float3 Bed  = Seen0 * Through;
    float3 Own  = Lit * WaterTint.rgb * (1.0f - Through);
    // From below, beyond the critical angle nothing of the world above gets through: the surface mirrors the water, and
    // the water's own light stands in for that.
    Bed  *= 1.0f - Tir;
    Own   = Own * (1.0f - Tir) + Lit * WaterTint.rgb * ((1.0f - Mirror.a) * Tir);
    float3 Kept = Mirror.rgb * (Mirror.a * Tir);
    // The surface's sheen: water gives back more of the light around it the flatter it is seen, and a wave or a ripple
    // tilts it - which is what makes them visible over a floor with nothing on it to bend. What comes back is the
    // mirror image (Reflections=High and up); where there is none - the reflections off, or a ray that found nothing -
    // the light the surface itself receives stands in for the surroundings, and at most half of it comes back.
    if (H > 0.0f)
    {
        const float3 ToEye = ChuteOn ? normalize(Eye - Sp)
                                     : normalize(float3(WaterCamera.xy - World, WaterExtra.x));
        const float3 Up    = normalize(float3(-Slope, 1.0f));
        const float  Back  = 0.02f + 0.98f * pow(1.0f - saturate(dot(Up, ToEye)), 5.0f);
        Bed  *= 1.0f - Back;
        Own   = Own * (1.0f - Back) + Lit * (0.5f * (1.0f - Mirror.a) * Back);
        Kept  = Kept * (1.0f - Back) + Mirror.rgb * (Mirror.a * Back);
        if (PoolKind > 0.5f)
            Kept += PoolLight(World, ToEye, Up);
        if (WaterSpare.x > 0.5f)
        {
#if RDX_RT
            if (RtLevel.z == 2u)
            {
                Added = 0.0f;
                return WaterGlintsRt(Eye, Sp, N, H);
            }
            Kept += WaterGlintsRt(Eye, Sp, N, H);
#else
            Kept += WaterGlints(Eye, Sp, N, H);
#endif
        }
    }
    const float3 Seen = Bed + Own + Kept;
    // The surface's own depth (WaterSurfaceOn). Only where the surface is in front: the depth test comes before this
    // shader ([earlydepthstencil], PSWater). The drawing's to write, not the asking's before it.
#if !RDX_RT_ASK
    if (WaterSurfaceOn > 0.5f)
        InterlockedMax(WaterSurfaceDepth[uint2(In.Pos.xy)], asuint(In.Pos.z));
#endif
    // The shoreline: the surface's own texture comes in over the first few units of depth. Not a chute's: that
    // texture is the fountain's glowing picture, no water's - a chute's water is clear down to its bed.
    const float3 S = ChuteOn ? 0.0f : Src * saturate(Shallow / max(Shore, 1e-3f));
    // What the shadows leave alone (the added target): what comes off the surface. Of the rest, the bed's share and its
    // place in the copy, for the apply pass to take the shadow of that place; the surface's own texture is lit by the
    // light the surface receives, as the water's own light is.
    Added = Kept * (1.0f - S);
    const float3 Luma   = float3(0.299f, 0.587f, 0.114f);
    const float  Shaded = dot(S + (Bed + Own) * (1.0f - S), Luma);
    GWaterShade = float4(dot(Received, Luma), (Shaded > 1e-6f) ? saturate(dot(Bed * (1.0f - S), Luma) / Shaded) : 0.0f,
                         Uv - ScreenUv);
    // A measurement only (a negative shoreline, SecondRevision-ripple-view.txt): the ripples' field itself, red where it
    // holds a height, green where the pixel is inside it; blue the slope the ripples and the rain's rings give the
    // surface (0.2 is full).
    if (Shore < 0.0f)
        return float3(saturate(abs(SeenH)), 0.3f * SeenIn, saturate(length(RippleSlope) * 5.0f)) + Seen * 0.3f;
    return S + Seen * (1.0f - S);
}

// The water's surface made to move (BLEND_Water): the GPU cuts the engine's polygons - cut up beforehand where they are
// large (RdxMath::WaterCutPolygon) - into small triangles, as fine as the waves ask for (WaterEdgeCuts), and every
// vertex is lifted by the water texture's own waves and by the ripples, so the surface really rises and falls. The
// vertex shader only puts each corner back into view space; the domain shader does the lifting, then projects the
// way the engine does.
struct WaterPoint
{
    float3 View   : VIEWPOS;     // the corner in view space
    float2 Scr    : SCREEN;      // and on the screen, in the engine's pixels (the edges' lengths come from these)
    float2 Uv     : TEXCOORD0;
    float2 Lv     : TEXCOORD1;
    float4 Colour : COLOR;
};
struct WaterPatch
{
    float Edge[3] : SV_TessFactor;
    float Inner   : SV_InsideTessFactor;
};

WaterPoint VSWater(VSIn In)
{
    WaterPoint Out;
    const float Z = 1.0f / max(In.Pos.z, 1e-6f);
    Out.View   = float3((In.Pos.xy - WaterScreen.xy) * (Z / WaterView.y), Z);
    Out.Scr    = In.Pos.xy;
    Out.Uv     = In.Uv;
    Out.Lv     = In.Lv;
    Out.Colour = In.Colour;
    return Out;
}

// How far the lifting reaches: in full up to 800 units away, none beyond 1600 - further out a vertex is too far from the
// next one to follow the waves without shimmering (RdxMath::WaterLiftEnd: the water's polygons are cut that far).
float WaterLiftFade(float Z)
{
    return saturate((1600.0f - Z) / 800.0f);
}

// And none right around the camera: the engine does not lift the camera with the waves, so a crest passing it would
// put it under the water it thinks it is above. Flat within 48 units across, in full from 200 on.
float WaterLiftNear(float2 World)
{
    return saturate((length(World - WaterCamera.xy) - 48.0f) / 152.0f);
}

// How far apart, in the level's units, the lifted surface's points may be and still follow what lifts them: the
// texture's wave field is read for the lifting through taps three cells apart (WaterLift), and the ripples' field is
// a texel fine - nothing finer than those moves a point.
float WaterLiftStep()
{
    // A fountain's chute is lifted by its running water's own unevenness.
    if (ChuteOn)
        return CHUTE_STEP;
    float Step = RDX_RIPPLE_WORLD / RDX_RIPPLE_TEXELS;
    if (WaterWave.x > 0.5f)
    {
        float2 FSize;
        WaveField.GetDimensions(FSize.x, FSize.y);
        Step = min(Step, 3.0f * min(WaterWave.z / FSize.x, WaterWave.w / FSize.y));
    }
    return max(Step, 0.5f);
}

// How many pieces an edge is cut into: one for every RDX_WATER_EDGE engine pixels of it, or for every Step units of
// the level where that is more - seen flat, an edge that runs into the picture is a few pixels long and tens of
// units, and cut by its pixels alone its points would be further apart than the waves are long. Never finer than a
// piece every WATER_EDGE_FINEST pixels. By the edge's two ends alone: the patch on its other side cuts it the same.
//
// WATER_EDGE_FINEST is how close on the screen the surface's points come at the most, inside a patch as along its
// edges. Closer than that its triangles are a few pixels each - and the picture's pixels are worked out in fours, so
// a triangle that small has several times its own pixels worked out for it, each by the water's whole shader.
#define WATER_EDGE_FINEST 6.0f
float WaterEdgeCuts(WaterPoint A, WaterPoint B, float Step)
{
    const float Px    = length(A.Scr - B.Scr);
    const float Units = length(A.View - B.View);
    return clamp(max(Px / RDX_WATER_EDGE, min(Units / Step, Px / WATER_EDGE_FINEST)), 1.0f, 64.0f);
}

WaterPatch WaterPatchConstants(InputPatch<WaterPoint, 3> P)
{
    WaterPatch C;
    // Where nothing is lifted any more, an edge is not cut at all.
    const float Near = min(P[0].View.z, min(P[1].View.z, P[2].View.z));
    C.Edge[0] = C.Edge[1] = C.Edge[2] = 1.0f;
    if (WaterLiftFade(Near) > 0.0f)
    {
        const float Step = WaterLiftStep();
        C.Edge[0] = WaterEdgeCuts(P[1], P[2], Step);
        C.Edge[1] = WaterEdgeCuts(P[2], P[0], Step);
        C.Edge[2] = WaterEdgeCuts(P[0], P[1], Step);
    }
    // The inside as fine as the finest edge asks - its rows no closer than the edges' pieces may be: across the
    // patch's narrow way on the screen, its least height there, a row every WATER_EDGE_FINEST pixels at the most.
    const float2 E1     = P[1].Scr - P[0].Scr;
    const float2 E2     = P[2].Scr - P[0].Scr;
    const float  Long   = max(length(E1), max(length(E2), length(E2 - E1)));
    const float  Narrow = abs(E1.x * E2.y - E1.y * E2.x) / max(Long, 1.0f);
    C.Inner = min(max(C.Edge[0], max(C.Edge[1], C.Edge[2])), max(Narrow / WATER_EDGE_FINEST, 1.0f));
    return C;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("WaterPatchConstants")]
WaterPoint HSWater(InputPatch<WaterPoint, 3> P, uint I : SV_OutputControlPointID)
{
    return P[I];
}

// The height the surface is lifted by at a place: the water texture's own waves - smoothed over a few cells, as a
// vertex is not close enough to the next to follow a single drop - and the ripples things make.
// Uv is where the wave field is read for it (WaterLiftUv): the same for every surface at one place of the level.
float WaterLift(float2 Uv, float2 World)
{
    float Lift = 0.0f;
    if (WaterWave.x > 0.5f)
    {
        float2 FSize;
        WaveField.GetDimensions(FSize.x, FSize.y);
        const float2 D = 3.0f / FSize;
        float F = 0.0f;
        [unroll] for (int y = -1; y <= 1; y++)
            [unroll] for (int x = -1; x <= 1; x++)
                F += WaveField.SampleLevel(Samp, Uv + float2(x, y) * D, 0.0f).r * ((x == 0 && y == 0) ? 4.0f : ((x == 0 || y == 0) ? 2.0f : 1.0f));
        Lift = clamp((F / 16.0f - 0.50196f) * 2.0f, -0.6f, 0.6f) * WaterWave.y;
    }
    const float2 R = (World - WaterScreen.zw) / RDX_RIPPLE_WORLD;
    if (all(R > 0.0f) && all(R < 1.0f))
    {
        const float2 Edge = saturate(min(R, 1.0f - R) * 16.0f);
        Lift += Ripples.SampleLevel(Samp, R, 0.0f).x * (Edge.x * Edge.y);
    }
    return Lift;
}

// Where the wave field is read for the lifting: by the place in the world, at the surface's own size of a repeat -
// not by the surface's texture coordinates. A level's water is often several sheets side by side, each carrying the
// texture from an origin of its own: lifted by that, two sheets rise and fall apart along the edge they share, and a
// gap opens and closes between them. The picture of the waves on each sheet stays the texture's own.
float2 WaterLiftUv(float2 World)
{
    return World / max(WaterWave.zw, 1.0f);
}

[domain("tri")]
VSOut DSWater(WaterPatch C, float3 B : SV_DomainLocation, const OutputPatch<WaterPoint, 3> P)
{
    VSOut Out;
    float3 View = B.x * P[0].View + B.y * P[1].View + B.z * P[2].View;
    Out.Uv      = B.x * P[0].Uv + B.y * P[1].Uv + B.z * P[2].Uv;
    Out.Lv      = B.x * P[0].Lv + B.y * P[1].Lv + B.z * P[2].Lv;
    Out.Colour  = B.x * P[0].Colour + B.y * P[1].Colour + B.z * P[2].Colour;
    // Lifted along the world's up, which in view space is the Z of the view's three axes. The sludge (BLEND_Sludge,
    // WaterSpare.y) only ever up: the engine draws nothing under an opaque surface, so a trough would open a gap at
    // every wall it meets.
    const float2 World = WaterCamera.xy + View.x * WaterCamera.zw + View.y * WaterAxes.xy + View.z * WaterAxes.zw;
    const float2 Flat  = WaterScreen.xy + View.xy * (WaterView.y / max(View.z, 1.0f));
    float Lift = WaterLift(WaterLiftUv(World), World) * WaterLiftFade(View.z) * WaterLiftNear(World);
    // A fountain's chute is lifted by its running water's surface (ChuteHeight), right up to the eye: it is no
    // water to swim in.
    if (ChuteOn)
        Lift = ChuteHeight(ChutePlace(World)) * WaterLiftFade(View.z);
    if (WaterSpare.y > 0.5f)
        Lift = max(Lift, 0.0f);
    // A measurement only (SecondRevision-water-flat.txt): no lifting.
    if (WaterSpare.y < -0.5f)
        Lift = 0.0f;
    View += Lift * WaterExtra.yzw;
    View.z = max(View.z, 1.0f);
    // Onto the screen the way the engine projects, then into clip space the way VSMain does.
    float2 Scr = WaterScreen.xy + View.xy * (WaterView.y / View.z);
    // The engine hands the surface over cut at the view's edges, and nothing is drawn beyond a cut: a point on one
    // must not come in from the edge with the waves, or the picture shows a gap there for as long as the crest lasts.
    // It stays on the edge or goes out past it. The view's far edges are at twice its centre.
    const float2 Far = 2.0f * WaterScreen.xy;
    if (Flat.x <= 1.5f)
        Scr.x = min(Scr.x, Flat.x);
    if (Flat.x >= Far.x - 1.5f)
        Scr.x = max(Scr.x, Flat.x);
    if (Flat.y <= 1.5f)
        Scr.y = min(Scr.y, Flat.y);
    if (Flat.y >= Far.y - 1.5f)
        Scr.y = max(Scr.y, Flat.y);
    const float  W   = View.z;
    Out.View  = View;
    Out.Pos.x = (Scr.x * InvHalfSize.x - 1.0f + Jitter.x) * W;
    Out.Pos.y = (1.0f - Scr.y * InvHalfSize.y + Jitter.y) * W;
    Out.Pos.z = 1.0f;
    Out.Pos.w = W;
    return Out;
}

// Geometry=Tessellation: the relief made real. The same height map POM reads, but instead of sliding the texture the
// surface itself is lifted out of the wall - so its outline changes, it stands in front of what is beside it, and the
// depth everything after it reads (the occlusion, the shadows' receiving side, the fog) is the relief's own.
//
// OUTWARDS ONLY, by the relief's depth times the height: 0 (the height map's deepest) stays on the wall, 1 comes out
// by the whole depth. POM carves the same range INTO the wall, so one depth number means the same relief in both. Not
// inwards, because a point pushed behind the wall is behind what the shadows see of the wall, and shadows itself.
//
// The patches are the level's polygons cut into triangles around their centres (DrawComplexSurface). The vertices
// carry their place in view space, not on the screen, and their distance to the nearest HELD edge: an edge where the
// surface meets another surface - a corner of the room, the rim of a doorway - rather than another polygon of its
// own. A shared edge is lifted from both sides by the same height at the same place and needs no holding; at a held
// one the relief comes down to the wall, over a band RELIEF_BEVEL times as wide as it is deep, so its rim is a slope
// rather than a cliff - unless a wall stands at that edge, rising in front of the surface: then the relief goes up to
// it and its rim runs down along the wall, which hides it (ReliefHeldAt). The distance is measured at every point the
// GPU cuts, from the run's held edges themselves, so the band is as wide as it should be however the surface was split
// into patches. Where a run has none, the patches' corners carry it and it is interpolated - exact only where one
// straight edge is the nearest all across a patch: an edge between two corners that each lie on a held edge would come
// out held along its length.
#define RELIEF_BEVEL 2.0f
struct ReliefPoint
{
    float3 View   : VIEWPOS;
    float2 Uv     : TEXCOORD0;
    float2 Lv     : TEXCOORD1;
    float4 Colour : COLOR;
    float  Rim    : RIMDIST;      // level units to the nearest held edge
    float3 Dir    : RELIEFDIR;    // the direction to lift along, view space - unit at the corners, not between them
};
struct ReliefPatch
{
    float Edge[3] : SV_TessFactor;
    float Inner   : SV_InsideTessFactor;
};

ReliefPoint VSRelief(VSIn In)
{
    ReliefPoint Out;
    Out.View   = In.Pos;          // a relief run's vertex is in view space already
    Out.Uv     = In.Uv;
    Out.Lv     = In.Lv;
    Out.Colour = In.Colour;
    Out.Rim    = In.Pom.x;
    Out.Dir    = In.Pom.yzw;
    return Out;
}

// How far apart the cuts may be at a distance, as a share of the full density: all of it while the whole relief
// stands two pixels tall or more on the screen, less below that, down to a sixteenth. A relief under a pixel has no
// outline to show, and far away is where most of a large view's surface is. The CPU's split keeps the same rule
// (RenderDevice.cpp, ReliefDensity).
float ReliefDensity(float Z)
{
    return clamp(PomDepth * ReliefFocal / (max(Z, 1.0f) * 2.0f), 1.0f / 16.0f, 1.0f);
}

// How many cuts an edge gets: one every ReliefEdge pixels of the length it would have on the screen if it faced
// the eye, at the distance of its ends, thinned out where the relief is too small to see (ReliefDensity). Measured
// along the edge itself and not across the screen, because a wall seen edge on still needs its bumps cut finely -
// that is exactly where their outline shows. The DISTANCE from the eye, not the depth along the view: turning on the
// spot changes every depth and no distance, so a player looking round sees the cuts stay where they are. Only the
// edge's two ends go into it, in either order, so the patches on both sides of an edge cut it alike.
float ReliefCuts(float3 A, float3 B)
{
    const float Z = max(0.5f * (length(A) + length(B)), 1.0f);
    return clamp(length(A - B) * ReliefFocal / (Z * max(ReliefEdge, 1.0f)) * ReliefDensity(Z), 1.0f, 64.0f);
}

ReliefPatch ReliefPatchConstants(InputPatch<ReliefPoint, 3> P)
{
    ReliefPatch C;
    C.Edge[0] = ReliefCuts(P[1].View, P[2].View);
    C.Edge[1] = ReliefCuts(P[2].View, P[0].View);
    C.Edge[2] = ReliefCuts(P[0].View, P[1].View);
    C.Inner   = max(C.Edge[0], max(C.Edge[1], C.Edge[2]));
    return C;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ReliefPatchConstants")]
ReliefPoint HSRelief(InputPatch<ReliefPoint, 3> P, uint I : SV_OutputControlPointID)
{
    return P[I];
}

// Which shrunken copy of the height map a vertex reads: one whose texels are a little further apart than the vertices
// around it. A finer one and a speck of dirt between two vertices is caught by one cut and missed by the next, and as
// the cuts move with the camera the surface boils; a ridge running across the cuts turns into a zigzag. Half a level
// coarser than the gap itself, because a height read exactly at the gap still aliases on the diagonal. From the
// vertex's own place, never from its patch, so the patches on both sides of an edge read the same height at the same
// point.
float ReliefLevel(float3 View)
{
    const float  Far    = length(View);          // the distance, as ReliefCuts has it
    const float  Gap    = max(ReliefEdge, 1.0f) * max(Far, 1.0f) / (ReliefFocal * ReliefDensity(Far));
    const float2 Texels = Gap * ReliefRate / max(PomTexel, 1e-6f);
    return log2(max(max(Texels.x, Texels.y), 1.0f)) + 0.5f;
}

// What the run's held edges make of P (view space, on the flat surface): how much of the relief stands there, and how
// far its top moves across the surface per unit lifted. An edge keeps a share of the band (w, 0..1): all of it where
// nothing stands beyond - the relief comes down over the band's Width - none where a wall stands at the edge, and part
// of it where a gap opens before one. An edge up to a wall shears instead: the top moves by its shear there, less and
// less a band's width in, so the rim lies on a wall that leans (RenderMath.h, ReliefRimFromWall). A seam carried over
// (w -1) holds nothing down: a point on it reads its height where the other side reads it - Seam is what to add to
// the texture coordinate there, and zero elsewhere. The row gives it at both ends of the seam (A: the second and third
// rows' w; B: the third row's xy), in a straight line between them (ReliefSeamRow).
void ReliefHeldAt(float3 P, float Width, out float Keep, out float3 Shear, out float2 Seam)
{
    Keep  = 1.0f;
    Shear = float3(0.0f, 0.0f, 0.0f);
    Seam  = float2(0.0f, 0.0f);
    const int Count = min((int)ReliefHeldCount.x, RELIEF_HELD_MAX);
    for (int i = 0; i < Count; i++)
    {
        const float3 A  = ReliefHeld[3 * i].xyz;
        const float3 AB = ReliefHeld[3 * i + 1].xyz - A;
        const float  T  = saturate(dot(P - A, AB) / max(dot(AB, AB), 1e-8f));
        const float  D  = length(P - A - AB * T);
        const float  W  = ReliefHeld[3 * i].w;
        if (W < -0.5f)
        {
            if (D < 0.01f)
                Seam = lerp(float2(ReliefHeld[3 * i + 1].w, ReliefHeld[3 * i + 2].w), ReliefHeld[3 * i + 2].xy, T);
            continue;
        }
        if (W > 0.001f)
            Keep = min(Keep, smoothstep(0.0f, Width * W, D));
        Shear += ReliefHeld[3 * i + 2].xyz * (1.0f - smoothstep(0.0f, Width, D));
    }
}

// What stands above the relief: how far a point may be lifted before it reaches a poster, a mirror or a painting on a
// wall, a rug on a floor - so that it never pokes through what hangs or lies on it. RdxMath::ReliefClearance is the same
// walk, tested, and these are its numbers (ReliefClearSteps, ReliefClearMin, ReliefClearEps, ReliefClearGap).
//
// Walked up the point's own line a step at a time. What the eye sees on the ray through the lifted point, if it lies above
// the surface by more than a pixel's width and within a step of the point's height, may stand over the point - and it
// does if what the eye sees on the ray through the point's own line at that height is at that height too, give or take a
// pixel's width. The depth is read at a pixel's middle, not on the ray itself, so a surface seems up to half a pixel's
// width off its place - the surface itself too, just above itself where the view grazes it, and which pixel that is moves
// with the upscaler's jitter every frame. A cover lying along the surface is at the same height wherever the eye meets it;
// the surface itself, a wall beside it, or anything between the eye and it, is not.
#define RELIEF_CLEAR_STEPS 8
#define RELIEF_CLEAR_MIN   0.02f
#define RELIEF_CLEAR_EPS   0.02f
#define RELIEF_CLEAR_GAP   0.05f

// The covers' depth on the eye's ray through Q (view space) as a view depth: what the eye sees there. A huge number off
// the picture, where nothing was drawn, and where no covers are bound.
float ReliefCoverZ(float3 Q, float2 Size)
{
    if (Q.z < 1.0f)
        return 1.0e6f;
    // Onto the target as DSRelief puts a point there: the engine's pixels, then the target's own.
    const float2 Ndc = float2((ReliefFocal * Q.x / Q.z + ReliefCentre.x) * InvHalfSize.x - 1.0f + Jitter.x,
                              1.0f - (ReliefFocal * Q.y / Q.z + ReliefCentre.y) * InvHalfSize.y + Jitter.y);
    const float2 Px  = float2(Ndc.x + 1.0f, 1.0f - Ndc.y) * 0.5f * Size;
    if (any(Px < 0.0f) || any(Px >= Size))
        return 1.0e6f;
    return 1.0f / max(ReliefCover.Load(int3(Px, 0)).r, 1e-6f);
}

// The same on the ray itself, for the two heights that are held against each other: seen at a slant, each read at
// its own pixel's middle, they miss each other by more than the pixel's width they are allowed - a cover lying along
// the surface was then taken for none, a screen's column of points at a time. One over the depth goes evenly across
// a flat thing on the screen, so it is carried from the pixel's middle to the ray's own place by its slope to the
// pixel's neighbours - where the neighbours either way agree, as they do on one flat thing and not across an edge.
float ReliefCoverZOn(float3 Q, float2 Size)
{
    if (Q.z < 1.0f)
        return 1.0e6f;
    const float2 Ndc = float2((ReliefFocal * Q.x / Q.z + ReliefCentre.x) * InvHalfSize.x - 1.0f + Jitter.x,
                              1.0f - (ReliefFocal * Q.y / Q.z + ReliefCentre.y) * InvHalfSize.y + Jitter.y);
    const float2 Px  = float2(Ndc.x + 1.0f, 1.0f - Ndc.y) * 0.5f * Size;
    if (any(Px < 1.0f) || any(Px >= Size - 1.0f))
        return ReliefCoverZ(Q, Size);
    const int2   C   = int2(Px);
    const float  I   = ReliefCover.Load(int3(C, 0)).r;
    const float  L   = ReliefCover.Load(int3(C + int2(-1, 0), 0)).r;
    const float  R   = ReliefCover.Load(int3(C + int2(1, 0), 0)).r;
    const float  U   = ReliefCover.Load(int3(C + int2(0, -1), 0)).r;
    const float  B   = ReliefCover.Load(int3(C + int2(0, 1), 0)).r;
    const float2 Off = Px - (float2(C) + 0.5f);
    const float  Gx  = (abs(R - 2.0f * I + L) <= 0.25f * abs(R - L) + 4e-9f) ? 0.5f * (R - L) : 0.0f;
    const float  Gy  = (abs(B - 2.0f * I + U) <= 0.25f * abs(B - U) + 4e-9f) ? 0.5f * (B - U) : 0.0f;
    return 1.0f / max(I + Off.x * Gx + Off.y * Gy, 1e-6f);
}

// P: the point on the flat surface, D: the direction it is lifted along, Lift: how far - all in view space.
float ReliefClear(float3 P, float3 D, float Lift)
{
    float2 Size;
    ReliefCover.GetDimensions(Size.x, Size.y);
    if (Lift <= 0.0f || Size.x < 1.0f)
        return Lift;
    const float3 N    = ReliefNormal;
    const float  Up   = max(dot(D, N), 0.25f);
    const float  Step = Lift / RELIEF_CLEAR_STEPS;
    // What one pixel of the covers spans at a unit's distance: the engine's pixels over its projection, for each of the
    // target's.
    const float  Pixel = (2.0f / InvHalfSize.x) / Size.x / max(ReliefFocal, 1.0f);
    [loop]
    for (int k = 0; k < RELIEF_CLEAR_STEPS; k++)
    {
        const float  G = RELIEF_CLEAR_MIN + k * Step;
        const float3 Q = P + D * G;
        if (Q.z < 1.0f)
            break;
        // What the eye sees on Q's ray, and how far along D above the surface it is.
        const float  Z  = ReliefCoverZ(Q, Size);
        const float3 SP = Q * (Z / Q.z) - P;
        const float  H  = dot(SP, N) / Up;
        if (H <= RELIEF_CLEAR_MIN + Q.z * Pixel || abs(H - G) > Step)
            continue;
        // On P's own line at that height, is what the eye sees there at that height too? Both heights on their rays.
        const float  Hq = dot(Q * (ReliefCoverZOn(Q, Size) / Q.z) - P, N) / Up;
        const float3 Cp = P + D * Hq;
        const float  HC = dot(Cp * (ReliefCoverZOn(Cp, Size) / Cp.z) - P, N) / Up;
        // Under it by the gap, and by a pixel's width there besides: a point stopped nearer comes through the cover
        // in specks.
        if (abs(HC - Hq) <= RELIEF_CLEAR_EPS + Cp.z * Pixel)
            return clamp(Hq - RELIEF_CLEAR_GAP - Cp.z * Pixel, 0.0f, Lift);
    }
    // And the lifted point itself: what the eye sees on its own ray, where the point stands in front of it and it
    // lies along the surface at a height the point has passed, is a cover the steps went by - the point stops under
    // it as under one the steps found.
    const float3 Top = P + D * Lift;
    if (Top.z >= 1.0f)
    {
        const float Zt = ReliefCoverZOn(Top, Size);
        const float Ht = dot(Top * (Zt / Top.z) - P, N) / Up;
        if (Zt > Top.z && Ht > RELIEF_CLEAR_MIN + Top.z * Pixel && Ht < Lift)
        {
            const float3 Cp = P + D * Ht;
            const float  HC = dot(Cp * (ReliefCoverZOn(Cp, Size) / Cp.z) - P, N) / Up;
            if (abs(HC - Ht) <= RELIEF_CLEAR_EPS + Cp.z * Pixel)
                return max(Ht - RELIEF_CLEAR_GAP - Cp.z * Pixel, 0.0f);
        }
    }
    return Lift;
}

[domain("tri")]
VSOut DSRelief(ReliefPatch C, float3 B : SV_DomainLocation, const OutputPatch<ReliefPoint, 3> P)
{
    VSOut Out;
    float3 View = B.x * P[0].View + B.y * P[1].View + B.z * P[2].View;
    Out.Uv      = B.x * P[0].Uv + B.y * P[1].Uv + B.z * P[2].Uv;
    Out.Lv      = B.x * P[0].Lv + B.y * P[1].Lv + B.z * P[2].Lv;
    Out.Colour  = B.x * P[0].Colour + B.y * P[1].Colour + B.z * P[2].Colour;
    // How much of the relief stands here: none on a held edge, all of it a band's width in (RELIEF_BEVEL) - from the
    // run's held edges where it has them, else from the patches' corners.
    const float Bevel = (ReliefBevel > 0.0f) ? ReliefBevel : RELIEF_BEVEL;
    const float Width = Bevel * max(PomDepth, 1e-3f);
    float  Keep  = smoothstep(0.0f, Width, B.x * P[0].Rim + B.y * P[1].Rim + B.z * P[2].Rim);
    float3 Shear = float3(0.0f, 0.0f, 0.0f);
    float2 Seam  = float2(0.0f, 0.0f);
    if (ReliefHeldCount.x > 0.5f)
        ReliefHeldAt(View, Width, Keep, Shear, Seam);
    // A texture laid on once reads its height inside itself: the wrap would bring the far side in at the rim. On a seam
    // carried over, where the other side reads it.
    float2 At = Out.Uv + Seam;
    if (PomHas(POM_ONCE))
        At = clamp(At, 0.5f * PomTexel, 1.0f - 0.5f * PomTexel);
    const float Level  = ReliefLevel(View) + ReliefSmooth;
    const float Height = SceneCopy.SampleLevel(Samp, At, Level).r;
    // Lifted along the corners' direction: the surface's normal on a flat one, and at a bend (a round tower's faces)
    // the one both faces share there, so each lifts the edge between them alike.
    const float3 DirSum = B.x * P[0].Dir + B.y * P[1].Dir + B.z * P[2].Dir;
    const float3 Dir    = (dot(DirSum, DirSum) > 1e-8f) ? normalize(DirSum) : ReliefNormal;
    // Never through what stands above the point (ReliefClear).
    const float Wanted = PomDepth * Height * Keep;
    const float Lift   = ReliefClear(View, Dir + Shear, Wanted);
    View += (Dir + Shear) * Lift;
    // The detail texture's pass over the same patches reads its own coordinate; the height above was read with the
    // surface's, so both passes lift every point alike (FSolidPass::SetTessUvMap).
    Out.Uv = Out.Uv * ReliefUvMap.xy + ReliefUvMap.zw;
    // Onto the screen the way the engine projects and VSMain un-projects - written out so that no division by the
    // depth is left in it: a point lifted to within a unit of the eye is then clipped like any other.
    Out.Pos.x = View.x * (ReliefFocal * InvHalfSize.x) + (ReliefCentre.x * InvHalfSize.x - 1.0f + Jitter.x) * View.z;
    Out.Pos.y = -View.y * (ReliefFocal * InvHalfSize.y) + (1.0f - ReliefCentre.y * InvHalfSize.y + Jitter.y) * View.z;
    Out.Pos.z = 1.0f;
    Out.Pos.w = View.z;
    // w: how far the direction lifted along turns from the surface's normal - only the measurement view reads it.
    Out.Pom   = float4(Height, Level, Keep, 1.0f - dot(Dir, ReliefNormal));
    // The measurement view reads, in the level's place, how much of the lift the covers took (ReliefClear).
    if (PomHas(POM_SHOWKEEP))
        Out.Pom.y = (Wanted > 1e-4f) ? 1.0f - Lift / Wanted : 0.0f;
    Out.View  = View;
    return Out;
}

// The depth test before the shader, not after it: the shader writes the reactive mask, and a shader that writes a UAV is
// otherwise run - and writes it - where the surface is hidden too. The water writes no depth (its pipeline), so its clip
// loses nothing by the test coming first.
[earlydepthstencil]
float4 PSWater(VSOut In) : SV_Target
{
    float3 Added;
    return float4(WaterSurface(In, Added), 1.0f);
}
#if RDX_RT_ASK
// The surface's rays alone (docs/terv-rt-host.md), as the glass's layer asks its own (PSGlassMirrorAsk): the run
// drawn as it is drawn, after its nearest points are marked, and every ray it would trace - the mirror image's, the
// glints' - written into the rays' list for the host. No target.
[earlydepthstencil]
void PSWaterAsk(VSOut In, uint Cover : SV_Coverage)
{
    float3 Added;
    WaterSurface(In, Added);
    RtAskFlush(In.Pos, Cover);
}
#endif

// A see-through water run gone over before it is drawn (FSolidPass, the same triangles through the same stages):
// nothing reaches the picture - the pipeline masks every target - only the water's own depth takes the nearest of the
// surface's points at every pixel, for the drawing that follows to keep that one alone (WaterSurface). The texture's
// holes are no surface. An atomic keeps the nearer, whichever order the points come in.
void WaterMark(VSOut In)
{
    const float4 T = NoSmooth ? Tex.Sample(SampPoint, In.Uv) : Tex.SampleBias(Samp, In.Uv, MipBias);
    if (T.a >= 0.5f && WaterSurfaceOn > 0.5f)
        InterlockedMax(WaterSurfaceDepth[uint2(In.Pos.xy)], asuint(In.Pos.z));
}
[earlydepthstencil]
float4 PSWaterMark(VSOut In) : SV_Target
{
    WaterMark(In);
    return 0.0f;
}
#if RDX_RT_ASK
// The same go over a run before its rays are written down (PSWaterAsk), where there is no target to mask: the mark
// alone, through the very stages the asking and the drawing go through.
[earlydepthstencil]
void PSWaterMarkAsk(VSOut In)
{
    WaterMark(In);
}
#endif
#if RDX_LIGHT_TARGET
// With the shadows' three targets: the light target is left alone (the pipeline masks it, as for every blended
// draw), and the added target gets what the shadows leave alone; what their apply pass needs of the pixel besides goes
// beside the water's own depth (WaterShade), from the point that depth keeps.
[earlydepthstencil]
PSLitOut PSWaterLit(VSOut In)
{
    PSLitOut Out;
    float3 Added;
    Out.Colour = float4(WaterSurface(In, Added), 1.0f);
    Out.Lit    = float4(0.0f, 0.0f, 0.0f, 0.0f);
    Out.Added  = float4(Added, 1.0f);
    if (WaterSurfaceOn > 0.5f)
        WaterShade[uint2(In.Pos.xy)] = GWaterShade;
    return Out;
}
[earlydepthstencil]
PSLitOut PSWaterMarkLit(VSOut In)
{
    PSLitOut Out;
    WaterMark(In);
    Out.Colour = Out.Lit = Out.Added = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return Out;
}
#endif

// Water=Low and up: a water surface the level leaves opaque (BLEND_Sludge) - the sewer's sludge, water all the same
// (docs/meres-11). Lifted by its waves and its ripples as the see-through water is (DSWater), and drawn as the engine
// draws it - its own texture in its own light, as PSCore does - with what moves it on top: the texture swayed by the
// surface's slope, as a thin film of water over it would bend it; the sheen of the light around it, more where the
// surface tilts away from the eye (WaterSurface's, where it has no mirror image); and with the reflections on the
// lamps' glints. Nothing is seen through it, so the copies of the picture are not read. In.View is the lifted point in
// view space (DSWater).
//
// With the reflections on the mirror image goes over it as well, in a layer of its own drawn once the world is
// (SludgeMirror) - and the sheen and the glints go with it there.
//
// The rings things send out through it - a body wading, a fall beating down - would hardly show by that alone: on
// see-through water the bent view and the mirror image draw them, and this water has no bent view, and on Low no mirror
// image either. So the ripples' own
// slope is lit by the lamps over the water as a surface is lit: a ring's side turned towards them brighter, the side
// turned away darker. The texture's own waves keep the look they have - where nothing rings, nothing changes.
#define SLUDGE_FILM  8.0f      // how deep the film that sways the texture is, in the level's units
#define SLUDGE_RINGS 1.0f      // how much of that light and shade the rings get
// How much steeper than they are the rings are lit: they are low - a metre out from a fall's foot a slope of about 0.13,
// at two 0.06 - and lit as they are they come to a grey level or two, which the texture's own moving picture and the
// haze over it drown. Lit this much steeper, a ring a metre out takes half as much light again on its near side and a
// third of it on its far one, at two metres still a quarter more and less, and by three it is gone.
#define SLUDGE_RING_TILT 10.0f
float SludgeRingShade(float3 Sp, float H, float2 RippleSlope, float3 ToEye)
{
    float3 Toward = 0.0f;
    const int Count = (int)WaterLampCount.x;
    [loop] for (int i = 0; i < Count; i++)
    {
        const float4 P    = WaterLamp[i * 2];
        const float3 D    = float3(P.xy, H + P.z) - Sp;     // the lamp in the frame around the camera, z above the surface
        const float  Dist = length(D);
        if (D.z <= 0.0f || Dist >= 2.0f * P.w)
            continue;
        const float3 Hue = WaterLamp[i * 2 + 1].rgb;
        Toward += D / Dist * (dot(Hue, float3(0.299f, 0.587f, 0.114f)) * saturate(2.0f - Dist / P.w));
    }
    // Half of it always from the viewer's side, raised - as the light of a room mostly is where somebody stands to
    // look, and as what the water mirrors back to them is: lamps high over the water light a low ring nearly the same
    // on both its sides, and those rings would hardly show.
    float3 L = normalize(float3(normalize(ToEye.xy + 1e-5f), 1.2f));
    if (dot(Toward, Toward) >= 1e-8f)
        L = normalize(normalize(Toward) + L);
    const float3 N = normalize(float3(-RippleSlope * SLUDGE_RING_TILT, 1.0f));
    return lerp(1.0f, clamp(dot(N, L) / max(L.z, 0.25f), 0.3f, 2.5f), SLUDGE_RINGS);
}

// Whether the sludge's mirror layer (BLEND_SludgeMirror) follows: then the sheen, the mirror image and the glints are the
// layer's, and the surface leaves them out. Set by the renderer for the surface and its layer alike; on the layer the
// pass puts in its place whether the frame's copy of the picture was made.
#define SludgeMirrorOn WaterSpare.w

float3 SludgeSurface(VSOut In)
{
    const float  H     = WaterExtra.x;                     // the camera's height above the surface; below it, negative
    const float3 V     = In.View;
    const float2 World = WaterCamera.xy + V.x * WaterCamera.zw + V.y * WaterAxes.xy + V.z * WaterAxes.zw;
    float  SeenH, SeenIn;
    float2 RippleSlope;
    const float2 Slope = WaterSlopeAt(In.Uv, World, WaterView.x, ddx(In.Uv), ddy(In.Uv), SeenH, SeenIn, RippleSlope);
    // The film: the slope along the texture's own directions, over one repeat of it in the level's units. The computed
    // waves are in the texture's space already, and a repeat is taken as the game's usual 256 units.
    const float2 Sway = (WaterWave.x > 0.5f)
                      ? float2(dot(Slope, WaterDirs.xy) / WaterWave.z, dot(Slope, WaterDirs.zw) / WaterWave.w) * SLUDGE_FILM
                      : Slope * (SLUDGE_FILM / 256.0f);
    const float4 T   = NoSmooth ? Tex.Sample(SampPoint, In.Uv + Sway) : Tex.SampleBias(Samp, In.Uv + Sway, MipBias);
    const float3 Lit = In.Colour.rgb * Light.Sample(Samp, In.Lv).rgb * RDX_LIGHT_SCALE * LightBoost;
    float3 Seen  = T.rgb * Lit;
    float  Shade = 1.0f;
    if (H > 0.0f)
    {
        const float3 AX    = float3(WaterCamera.zw, WaterExtra.y);
        const float3 AY    = float3(WaterAxes.xy,   WaterExtra.z);
        const float3 AZ    = float3(WaterAxes.zw,   WaterExtra.w);
        const float3 Eye   = float3(0.0f, 0.0f, H);
        const float3 Sp    = Eye + V.x * AX + V.y * AY + V.z * AZ;
        const float3 ToEye = normalize(float3(WaterCamera.xy - World, H));
        Shade = SludgeRingShade(Sp, H, RippleSlope, ToEye);
        Seen *= Shade;
        if (SludgeMirrorOn < 0.5f)
        {
            const float3 N    = normalize(float3(-Slope, 1.0f));
            const float  Back = 0.02f + 0.98f * pow(1.0f - saturate(dot(N, ToEye)), 5.0f);
            Seen = Seen * (1.0f - Back) + Lit * 0.5f * Back;
            if (WaterSpare.x > 0.5f)
                Seen += WaterGlints(Eye, Sp, N, H);
        }
    }
    // A measurement only (a negative shoreline, SecondRevision-ripple-view.txt): the ripples themselves - red their slope
    // (0.2 is full), green the light and shade the rings get (0.5 is none, 0 and 1 the ends of it), blue where the point
    // is inside their field.
    if (WaterView.w < 0.0f)
        return float3(saturate(length(RippleSlope) * 5.0f), saturate((Shade - 0.4f) / 1.4f), 0.5f * SeenIn);
    return Seen;
}

float4 PSSludge(VSOut In) : SV_Target
{
    return float4(SludgeSurface(In), 1.0f);
}

// Reflections=High and up: the sludge's mirror layer (BLEND_SludgeMirror), over the surface's own patches once the
// world is drawn - the surface itself is drawn among the level's walls, while much of what it mirrors is not in the
// picture yet. What it gives back is what the see-through water gives back from its surface, the same way (WaterMirror,
// the glints), laid over the surface by its sheen and premultiplied - rgb what comes back, a how much of the surface's
// own colour that takes - so the surface and its layer make together what one draw would: the surface's colour times
// one less the sheen, and the mirror image's share. Where the frame has no copy of the picture the image is left out,
// and the layer is the sheen of the surface's own light, as the surface alone would have drawn it.
float4 SludgeMirror(VSOut In)
{
#if RDX_RT_HOST
    RtFind(In.Pos);
#endif
    const float H = WaterExtra.x;
    if (H <= 0.0f || WaterView.w < 0.0f)
        return 0.0f;            // from below nothing comes back; the ripples' measuring view is the surface's own
    const float3 V     = In.View;
    const float2 World = WaterCamera.xy + V.x * WaterCamera.zw + V.y * WaterAxes.xy + V.z * WaterAxes.zw;
    const float2 DUx   = ddx(In.Uv);
    const float2 DUy   = ddy(In.Uv);
    float  SeenH, SeenIn;
    float2 RippleSlope;
    const float2 Slope = WaterSlopeAt(In.Uv, World, WaterView.x, DUx, DUy, SeenH, SeenIn, RippleSlope);
    const float3 Lit   = In.Colour.rgb * Light.Sample(Samp, In.Lv).rgb * RDX_LIGHT_SCALE * LightBoost;
    const float3 AX    = float3(WaterCamera.zw, WaterExtra.y);
    const float3 AY    = float3(WaterAxes.xy,   WaterExtra.z);
    const float3 AZ    = float3(WaterAxes.zw,   WaterExtra.w);
    const float3 Eye   = float3(0.0f, 0.0f, H);
    const float3 Sp    = Eye + V.x * AX + V.y * AY + V.z * AZ;
    const float3 ToEye = normalize(float3(WaterCamera.xy - World, H));
    const float3 N     = normalize(float3(-Slope, 1.0f));
    const float  Back  = 0.02f + 0.98f * pow(1.0f - saturate(dot(N, ToEye)), 5.0f);
    float4 Mirror = 0.0f;
    if (SludgeMirrorOn > 0.5f)
    {
        float2 Size;
        SceneCopy.GetDimensions(Size.x, Size.y);
        Mirror = WaterMirror(Eye, Sp, normalize(Sp - Eye), N, Slope, 1.0f, World, DUx, DUy, AX, AY, AZ, WaterView.y,
                             Size, Lit, 0.0f);
    }
    float3 Given = lerp(Lit * 0.5f, Mirror.rgb, Mirror.a) * Back;
#if RDX_RT
    Given += WaterGlintsRt(Eye, Sp, N, H);
#else
    Given += WaterGlints(Eye, Sp, N, H);
#endif
    return float4(Given, Back);
}

float4 PSSludgeMirror(VSOut In) : SV_Target
{
    return SludgeMirror(In);
}
#if RDX_RT_ASK
// The layer's rays alone, as the glass's layer asks its own (PSGlassMirrorAsk). The depth tested before the shader,
// as the layer's own drawing has it tested: the layer writes nothing a later test could undo.
[earlydepthstencil]
void PSSludgeMirrorAsk(VSOut In, uint Cover : SV_Coverage)
{
    SludgeMirror(In);
    RtAskFlush(In.Pos, Cover);
}
#endif

// The engine's volumetric light over the sludge (BLEND_SludgeFog): its fog map on the same lifted surface - the same
// patches, cut and lifted by the same numbers, so the depth test passes it exactly where the sludge is. The fog map's
// coordinates ride in the light map's place (In.Lv), as the patches' own coordinate is what lifts them; its colour, and in
// alpha how much of the surface it covers, times the vertex colour - as PSCore gives a fog map under RDX_FOG.
//
// Its light is lit and shaded by the rings as the sludge is (SludgeRingShade): where the haze glows over the water, the
// water mirrors it back, and a ring tilts that mirror - and on a dark sludge under a bright haze that light is most of
// what is seen, so without it the rings would be lost under the haze.
float4 PSSludgeFog(VSOut In) : SV_Target
{
    float4 Fog = Tex.SampleBias(Samp, In.Lv, MipBias) * In.Colour;
    const float  H   = WaterExtra.x;
    const float2 DUx = ddx(In.Uv);
    const float2 DUy = ddy(In.Uv);
    if (H > 0.0f)
    {
        const float3 V     = In.View;
        const float2 World = WaterCamera.xy + V.x * WaterCamera.zw + V.y * WaterAxes.xy + V.z * WaterAxes.zw;
        float  SeenH, SeenIn;
        float2 RippleSlope;
        WaterSlopeAt(In.Uv, World, WaterView.x, DUx, DUy, SeenH, SeenIn, RippleSlope);
        const float3 Sp    = float3(0.0f, 0.0f, H) + V.x * float3(WaterCamera.zw, WaterExtra.y) +
                             V.y * float3(WaterAxes.xy, WaterExtra.z) + V.z * float3(WaterAxes.zw, WaterExtra.w);
        const float3 ToEye = normalize(float3(WaterCamera.xy - World, H));
        Fog.rgb *= SludgeRingShade(Sp, H, RippleSlope, ToEye);
    }
    return Fog;
}
#if RDX_LIGHT_TARGET
// With the shadows' three targets the sludge writes what an opaque surface writes (PSMainLit): the light it receives, as
// luminance, and the added target, which its pipeline clears. Its fog map writes what a fog map does.
PSLitOut PSSludgeLit(VSOut In)
{
    PSLitOut Out;
    Out.Colour = float4(SludgeSurface(In), 1.0f);
    const float3 Lit = In.Colour.rgb * Light.Sample(Samp, In.Lv).rgb * RDX_LIGHT_SCALE;
    Out.Lit    = float4(dot(Lit, float3(0.299f, 0.587f, 0.114f)), 0.0f, 0.0f, 1.0f);
    Out.Added  = Out.Colour;
    return Out;
}
PSLitOut PSSludgeFogLit(VSOut In)
{
    PSLitOut Out;
    Out.Colour = PSSludgeFog(In);
    Out.Lit    = float4(0.0f, 0.0f, 0.0f, 0.0f);
    Out.Added  = Out.Colour;
    return Out;
}
// The mirror layer leaves the light target alone (its pipeline masks it) and goes into the added target as into the
// picture: the shadows darken the surface's own colour under it, not what the surface gives back.
PSLitOut PSSludgeMirrorLit(VSOut In)
{
    PSLitOut Out;
    Out.Colour = SludgeMirror(In);
    Out.Lit    = float4(0.0f, 0.0f, 0.0f, 0.0f);
    Out.Added  = Out.Colour;
    return Out;
}
#endif

// The levels' glass (Reflections=RT, BLEND_GlassMirror): what a pane gives back, as a layer of its own laid
// over the pane - which is drawn before it as the level draws it, and stays so. The view is mirrored by the pane's
// own plane and traced (WaterReflectRt). A pane has the layer on rays alone: followed through the copy of the
// picture (WaterReflect) the image of what stands before a pane seen face on is not to be had - that way is left
// to a mirror of the level's own whose rays fail for a frame.
// How much comes back goes by the angle the pane is seen at, as glass has it: a few hundredths face on, nearly all
// along the pane. Premultiplied by that share - rgb + dst * (1 - a) - so the pane and what shows through it dim as
// the mirror image takes over; and only as far as an image was found: where there is none nothing is laid on, and
// nothing dimmed.
// The run's numbers ride in the water's constants: the engine's projection (WaterView.y), the view's axes in the
// world (WaterCamera.zw, WaterAxes, WaterExtra.yzw), the screen's centre (WaterScreen.xy), and the pane's normal in
// the world, on the camera's side (GlassNormal).
#define GlassNormal   WaterDirs.xyz
// What a pane gives back seen face on: four hundredths at each of its two sides.
#define GLASS_FACE_ON 0.08f
float4 GlassMirror(VSOut In)
{
    // A mirror of the level's own (WaterDirs.w 1): it gives all of the image back whatever the angle, over the
    // whole of its surface - the level's own drawing of the surface comes over it afterwards.
    const bool bWhole = WaterDirs.w > 0.5f;
#if RDX_RT_HOST
    RtFind(In.Pos);
#endif
    // The pane's own holes are no glass.
    const float4 T = NoSmooth ? Tex.Sample(SampPoint, In.Uv) : Tex.SampleBias(Samp, In.Uv, MipBias);
#if RDX_RT_ASK
    // No pixel to throw away in the build that only asks: a hole asks nothing.
    GRtAskDead = (bWhole ? 1.0f : T.a) < 0.5f;
#else
    clip((bWhole ? 1.0f : T.a) - 0.5f);
#endif
    float2 Size;
    SceneCopy.GetDimensions(Size.x, Size.y);
    // The pixel's place from the camera, along the world's axes: from the screen and its depth, then the view's axes -
    // as WaterSurface puts a pixel back into the world, with the camera for the origin.
    const float  Z     = 1.0f / max(In.Pos.z, 1e-6f);
    const float  Focal = WaterView.y;
    const float2 Eng   = (In.Pos.xy / Size - JitterUv()) * (2.0f / InvHalfSize);
    const float2 View  = (Eng - WaterScreen.xy) * (Z / Focal);
    const float3 AX    = float3(WaterCamera.zw, WaterExtra.y);
    const float3 AY    = float3(WaterAxes.xy,   WaterExtra.z);
    const float3 AZ    = float3(WaterAxes.zw,   WaterExtra.w);
    const float3 Eye   = float3(0.0f, 0.0f, 0.0f);
    const float3 Sp    = View.x * AX + View.y * AY + Z * AZ;
    const float3 Into  = normalize(Sp);
    const float3 N     = GlassNormal;
    const float  Face  = bWhole ? 1.0f : GLASS_FACE_ON;
    const float  Back  = Face + (1.0f - Face) * pow(1.0f - saturate(-dot(Into, N)), 5.0f);
    const float3 Ray   = reflect(Into, N);
#if RDX_RT
    // Through the 64-bit host the lamp's ray below is no request of its own: it goes the mirror ray's way from the
    // mirror ray's start, and the host says with the mirror ray's answer what it would meet (RDX_RAY_FLAG_EYE).
    GRtAskMore = (!bWhole && WaterTint.w > 0.0f) ? RDX_RAY_FLAG_EYE : 0u;
    // And the image shows by what the pane gives back at this angle, at the most.
    RtShows(Back);
    // No stand-in for a light that is not known (WaterReflectRt): the levels' glass is drawn with its own
    // brightness, which says nothing of the street it mirrors.
    const float4 Mirror = WaterReflectRt(Eye, Sp, Ray, AX, AY, AZ, Focal, Size, float3(-1.0f, -1.0f, -1.0f), true);
#else
    const float4 Mirror = WaterReflect(Eye, Sp, Ray, AX, AY, AZ, Focal, Size);
#endif
    // The mirror covers what was drawn there: where the image holds nothing, it is dark.
    if (bWhole)
        return float4(Mirror.rgb * Mirror.a, 1.0f);
    const float Share = Back * Mirror.a;
#if RDX_RT
    // The flashlight itself as the pane shows it (WaterTint.w: how strongly it shines this way, nought without it):
    // where the mirror ray passes the eye - the lamp is there - the lamp's disc and its glare, by what the pane gives
    // back; not where the ray meets something on its way there. The disc and the glare as wide as a mirror of the
    // level's shows them (FBeamBounce's LampRadius and GlareRadius).
    float Lamp = 0.0f;
    const float Near = -dot(Sp, Ray);
    if (WaterTint.w > 0.0f && Near > 1.0f)
    {
        const float Off = length(Sp + Ray * Near);
        if (Off < 96.0f && GRtEyeSeen)
        {
            const float Q = Off / 16.0f;
            Lamp = WaterTint.w * max(1.0f - smoothstep(1.5f, 2.0f, Off), 0.85f / ((1.0f + Q * Q) * (1.0f + Q * Q)));
        }
    }
    return float4(Mirror.rgb * Share + Lamp * Back, Share);
#else
    return float4(Mirror.rgb * Share, Share);
#endif
}

float4 PSGlassMirror(VSOut In) : SV_Target
{
    return GlassMirror(In);
}
#if RDX_RT_ASK
// The layer's rays alone (docs/terv-rt-host.md): the layer run as it is drawn, and every ray it would trace written
// into the rays' list for the host - nothing where the pane has a hole. No target: the pixel is tested against the
// world's depth before the shader runs, as the layer's own drawing tests it, so what the picture hides asks nothing.
[earlydepthstencil]
void PSGlassMirrorAsk(VSOut In, uint Cover : SV_Coverage)
{
    GlassMirror(In);
    RtAskFlush(In.Pos, Cover);
}
#endif
#if RDX_LIGHT_TARGET
// As the sludge's mirror layer: the light target left alone, the added target as the picture.
PSLitOut PSGlassMirrorLit(VSOut In)
{
    PSLitOut Out;
    Out.Colour = GlassMirror(In);
    Out.Lit    = float4(0.0f, 0.0f, 0.0f, 0.0f);
    Out.Added  = Out.Colour;
    return Out;
}
#endif
#if RDX_RT
// A pane's part of the mirrors' map (FBeamBounce; FSolidPass::Flush draws the panes with it before the world): the
// flashlight's light off a pane of glass goes where the pane's own mirror rays go. A pixel of a pane the eye can see -
// nothing of the rays' scene stands between; the lamp is at the eye, and what hides the pane from it takes its light -
// gets one over the depth at which the image of what its mirror ray meets lies behind the pane, as a mirror's view
// would have left it, and the pane's number (WaterTint.z). The target's size rides in WaterTint.xy.
float2 PSPaneKeep(VSOut In) : SV_Target
{
    const float4 T = NoSmooth ? Tex.Sample(SampPoint, In.Uv) : Tex.SampleBias(Samp, In.Uv, MipBias);
#if RDX_RT_HOST
    // Before any pixel is thrown away: RtFind reads the two by two together.
    RtFind(In.Pos);
#endif
#if RDX_RT_ASK
    // No pixel to throw away in the build that only asks: a hole asks nothing.
    GRtAskDead = T.a < 0.5f;
#else
    clip(T.a - 0.5f);
#endif
    const float  Z     = 1.0f / max(In.Pos.z, 1e-6f);
    const float2 Eng   = (In.Pos.xy / WaterTint.xy - JitterUv()) * (2.0f / InvHalfSize);
    const float2 View  = (Eng - WaterScreen.xy) * (Z / WaterView.y);
    const float3 Sp    = View.x * float3(WaterCamera.zw, WaterExtra.y) + View.y * float3(WaterAxes.xy, WaterExtra.z) +
                         Z * float3(WaterAxes.zw, WaterExtra.w);
    const float  Far   = max(length(Sp), 1e-3f);
    const float3 Into  = Sp / Far;
    // Through the 64-bit host every pixel kept here costs the asking of a ray, so none is kept that no surface
    // can read: the map is looked up within the flashlight's cone on the screen, and the widest disc a surface
    // looks it up over beyond that (BeamOffMirrors) - nowhere else. WaterTint.w: that reach as a cosine from the
    // view's axis (RdxMath::BeamMapRimCos); nought: not known, and every pixel is kept.
    if (WaterTint.w > 0.0f && Z < WaterTint.w * Far)
    {
#if RDX_RT_ASK
        GRtAskDead = true;
        return float2(0.0f, 0.0f);
#else
        discard;
#endif
    }
    // Through the 64-bit host the two rays below are one request: the ray on from the pane, traced only where the
    // eye sees the pane's point - the host takes the look from the eye itself, and its answer says where the point
    // is not seen (RDX_RAY_FLAG_SEEN).
    GRtAskMore = (Far > 6.0f) ? RDX_RAY_FLAG_SEEN : 0u;
    FRtHit On = RtAsk(RDX_RAY_KIND_FIRST, RtEye.xyz + Sp, reflect(Into, GlassNormal), 1.0f, 65536.0f, 0x07u, 0u);
#if RDX_RT_HOST
    if (On.Unseen())
        discard;
#endif
    const float  Met   = (On.CommittedStatus() == COMMITTED_TRIANGLE_HIT && On.CommittedInstanceID() != 2u)
                             ? On.CommittedRayT() : 65536.0f;
    return float2(1.0f / (Z * (1.0f + Met / Far)), WaterTint.z);
}
#if RDX_RT_ASK
// The panes' rays alone, before the panes go into the map through the 64-bit host's answers: the ray on from a pane,
// asked for where the eye sees the pane, written into the rays' list. No target, and no depth: the map has none.
void PSPaneKeepAsk(VSOut In, uint Cover : SV_Coverage)
{
    PSPaneKeep(In);
    RtAskFlush(In.Pos, Cover);
}
#endif
#endif

// An effect's body in a view the passes of the world's end are not made for (BLEND_Body): a force field's volume in a
// mirror of the level's, on the polygon the engine hands over for that view. What the walk draws - smoke, flames,
// the waterfalls' columns - the walk draws in a mirror's view too (FBackend::WalkMirrorView), and none of it is a
// body here. The run's constants: the clock, and the view's place in the world.
#define BodyTime   WaterView.x
#define BodyEye    float3(WaterWave.w, WaterDirs.w, WaterSpare.w)

float BodyHash(float3 P)
{
    P = frac(P * 0.3183099f + float3(0.1f, 0.2f, 0.3f));
    P *= 17.0f;
    return frac(P.x * P.y * P.z * (P.x + P.y + P.z));
}

float BodyNoise(float3 P)
{
    const float3 I = floor(P), F = P - I;
    const float3 W = F * F * (3.0f - 2.0f * F);
    const float  A = lerp(BodyHash(I + float3(0.0f, 0.0f, 0.0f)), BodyHash(I + float3(1.0f, 0.0f, 0.0f)), W.x);
    const float  B = lerp(BodyHash(I + float3(0.0f, 1.0f, 0.0f)), BodyHash(I + float3(1.0f, 1.0f, 0.0f)), W.x);
    const float  C = lerp(BodyHash(I + float3(0.0f, 0.0f, 1.0f)), BodyHash(I + float3(1.0f, 0.0f, 1.0f)), W.x);
    const float  D = lerp(BodyHash(I + float3(0.0f, 1.0f, 1.0f)), BodyHash(I + float3(1.0f, 1.0f, 1.0f)), W.x);
    return lerp(lerp(A, B, W.y), lerp(C, D, W.y), W.z);
}

// A force field's volume in such a view (ForceField=3D): the polygon the engine hands over for the
// view is the field's own surface, and each of its pixels walks its ray through the field's thickness about that
// surface - the field's pass's own reckoning (FFieldVolume) of arcs, bolts, sparks, glow and haze, by a grain worked
// out here where that pass reads its own: a field of the same make, not the same field. The run's constants: the
// surface's normal in the world and the half thickness, the clock, how bright the field is laid on, and its hue.
#define BodyFieldNormal WaterTint.xyz
#define BodyFieldHalf   WaterTint.w
#define BodyFieldBright WaterView.z
#define BodyFieldHue    WaterCamera.rgb

// A cloud of the level's space about a middle of 0.47, of two sizes or of three.
float BodyCloud2(float3 P)
{
    return (BodyNoise(P) * 0.5f + BodyNoise(P * 2.03f + float3(17.7f, 9.3f, 5.1f)) * 0.25f) * (0.94f / 0.75f);
}
float BodyCloud3(float3 P)
{
    return (BodyNoise(P) * 0.5f + BodyNoise(P * 2.03f + float3(17.7f, 9.3f, 5.1f)) * 0.25f +
            BodyNoise(P * 4.09f + float3(3.1f, 27.9f, 13.3f)) * 0.125f) * (0.94f / 0.875f);
}

// The arcs' pattern at a place of the level, as the field's pass has it (its PatternAt): x the wide sheet's number, y
// the fine sheet's, z the haze's cloud, w the slow cloud a patch of the field swells by. Where a sheet's number
// crosses 0.47 there is an arc.
float4 BodyFieldPattern(float3 World, float Time)
{
    const float3 W    = World / 12.8f;
    const float3 Pf   = W * 0.6f + float3(0.0f, Time * 0.21f, Time * 0.13f);
    const float4 Fold = float4(BodyCloud2(Pf), BodyCloud2(Pf + float3(31.7f, 17.3f, 47.1f)),
                               BodyCloud2(Pf + float3(71.9f, 53.3f, 11.7f)),
                               BodyCloud2(Pf + float3(13.1f, 91.7f, 67.3f)));
    const float3 Pw   = W + (Fold.xyz - 0.47f) * 2.4f + float3(Time * 0.07f, 0.0f, Time * 0.05f);
    const float  T2   = Time * 1.7f + 3.0f;
    const float3 Pn   = W * 2.3f + 9.1f + (Fold.yzx - 0.47f) * 2.4f + float3(T2 * 0.07f, 0.0f, T2 * 0.05f);
    return float4(BodyCloud3(Pw), BodyCloud2(Pn + float3(31.7f, 17.3f, 47.1f)),
                  BodyCloud2(Pw + float3(71.9f, 53.3f, 11.7f)), Fold.w);
}

// Four numbers of no pattern for a cell of the level's space, the same anywhere within it (the field's pass's Lot).
float4 BodyLot(float3 Cell)
{
    const float3 C = floor(Cell);
    return float4(BodyHash(C), BodyHash(C + float3(31.7f, 17.3f, 47.1f)), BodyHash(C + float3(71.9f, 53.3f, 11.7f)),
                  BodyHash(C + float3(13.1f, 91.7f, 67.3f)));
}

// What a stretch of a ray takes of a sheet's arcs, the sheet's number going from A at one end to B at the other and
// taken as even between: x the core, y the glow, each as its mean over the stretch (the field's pass's Across).
float2 BodyAcross(float A, float B, float Sharp)
{
    const float Kc = 2.2f * Sharp;
    const float Kg = 0.45f * Sharp;
    A -= 0.47f;
    B -= 0.47f;
    const float Ca = saturate(1.0f - Kc * abs(A)), Cb = saturate(1.0f - Kc * abs(B));
    const float Ga = saturate(1.0f - Kg * abs(A)), Gb = saturate(1.0f - Kg * abs(B));
    const float Ca4 = Ca * Ca * Ca * Ca, Cb4 = Cb * Cb * Cb * Cb;
    const float Ga3 = Ga * Ga * Ga,      Gb3 = Gb * Gb * Gb;
    const float SumCa = ((A < 0.0f) ? Ca4 : 2.0f - Ca4) / (4.0f * Kc);
    const float SumCb = ((B < 0.0f) ? Cb4 : 2.0f - Cb4) / (4.0f * Kc);
    const float SumGa = ((A < 0.0f) ? Ga3 : 2.0f - Ga3) / (3.0f * Kg);
    const float SumGb = ((B < 0.0f) ? Gb3 : 2.0f - Gb3) / (3.0f * Kg);
    const float Span  = B - A;
    const float Mc    = saturate(1.0f - Kc * abs(0.5f * (A + B)));
    const float Mg    = saturate(1.0f - Kg * abs(0.5f * (A + B)));
    const bool  bFlat = abs(Span) < 1e-4f;
    const float Safe  = bFlat ? 1.0f : Span;
    return float2(bFlat ? Mc * Mc * Mc : (SumCb - SumCa) / Safe, (bFlat ? Mg * Mg : (SumGb - SumGa) / Safe) * 0.3f);
}

float4 BodyField(VSOut In)
{
    // The pixel's ray in the world, and how far along it the field's surface lies.
    const float3 Ray   = In.View / max(In.View.z, 1e-3f);
    const float  Scale = length(Ray);
    const float3 Dir   = (WaterWave.xyz * Ray.x + WaterDirs.xyz * Ray.y + WaterSpare.xyz * Ray.z) / Scale;
    const float  Tf    = In.View.z * Scale;
    const float  Half  = max(BodyFieldHalf, 0.1f);
    const float  Dn    = dot(BodyFieldNormal, Dir);
    // Its stretch within the half thickness, either side of the surface; a ray that runs along the surface is walked
    // no further than a score of units each way.
    const float  Reach = min(Half / max(abs(Dn), 0.05f), 24.0f);
    const float  At    = max(Tf - Reach, 0.0f);
    const float  End   = Tf + Reach;
    const float  Gap   = max(0.5f, (End - At) / 8.0f);
    const float  Shift = frac(52.9829189f * frac(0.06711056f * In.Pos.x + 0.00583715f * In.Pos.y) + BodyTime * 7.31f);
    float  Core = 0.0f, Glow = 0.0f, Haze = 0.0f, Bolt = 0.0f, Spark = 0.0f;
    float  WasT = At, WasStrong = 0.0f, WasBolt = 0.0f, WasKey = -1.0f;
    float4 Was  = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float3 WasCell = float3(-1e9f, -1e9f, -1e9f);
    // A pixel's width at a unit of the ray, for the sparks: none is drawn under a pixel wide.
    const float Foot = 1.0f / max(ReliefFocal, 1.0f);
    [loop] for (int i = 0; i < 10; i++)
    {
        const float  T      = (i == 0) ? At : min(At + ((float)i - 1.0f + Shift) * Gap, End);
        // Full on the surface and nothing at the half thickness, along a quarter of a sine wave between.
        const float  Strong = cos(saturate(abs((T - Tf) * Dn) / Half) * 1.5707963f);
        const float3 World  = BodyEye + Dir * T;
        const float4 Now    = BodyFieldPattern(World, BodyTime);
        // The discharges: a part of the field strikes a bolt of its own now and then, each part at its own times - a
        // line of its own, sharper, gone within a ninth of a second.
        const float3 Part   = World / 12.8f * 0.6f;
        const float  Beat   = BodyTime * 9.0f + BodyLot(Part).r * 9.0f;
        const float  Struck = step(0.97f, BodyLot(Part + floor(Beat) * float3(13.0f, 7.0f, 5.0f)).g);
        const float  Key    = Struck * (1.0f + floor(Beat) + dot(floor(Part), float3(1.0f, 57.0f, 113.0f)));
        float NowBolt = 0.0f;
        [branch] if (Struck > 0.5f)
            NowBolt = BodyCloud3(World / 12.8f * 1.5f + floor(Beat) * 13.7f + float3(13.1f, 91.7f, 67.3f));
        if (i > 0 && T > WasT)
        {
            const float  Long = T - WasT;
            const float  Mid  = 0.5f * (Strong + WasStrong);
            const float  Rise = 0.5f * (Now.w + Was.w);
            const float  Live = 0.7f + 1.6f * Rise * Rise * Rise;
            const float2 Sw   = BodyAcross(Was.x, Now.x, 14.0f);
            const float2 Sf   = BodyAcross(Was.y, Now.y, 10.0f);
            Core += (Sw.x + 0.55f * Sf.x) * Live * Mid * Long;
            Glow += (Sw.y + 0.55f * Sf.y) * Live * Mid * Long;
            Haze += (0.09f + 0.09f * 0.5f * (Now.z + Was.z)) * Mid * Long;
            [branch] if (Key > 0.5f && Key == WasKey)
                Bolt += BodyAcross(WasBolt, NowBolt, 24.0f).x * (1.0f - frac(Beat)) * 2.2f * Mid * Long;
        }
        // The sparks: the level is cut into small cells, and now and then one lights a spark at a place of its own
        // within it, which flares and dies - each cell at its own times. How near the ray passes it is worked out.
        const float3 Cell = floor(World / 1.5f);
        [branch] if (any(Cell != WasCell))
        {
            const float  Tick = BodyTime * 1.3f + BodyLot(Cell + 0.5f).a * 10.0f;
            const float4 Lit  = BodyLot(Cell + 0.5f + floor(Tick) * float3(7.0f, 3.0f, 11.0f));
            [branch] if (Lit.a > 0.992f)
            {
                const float3 C   = (Cell + 0.25f + 0.5f * Lit.rgb) * 1.5f - BodyEye;
                const float  Tc  = dot(C, Dir);
                const float  Rad = max(0.14f, Foot * Tc);
                const float  D2  = max(dot(C, C) - Tc * Tc, 0.0f) / (Rad * Rad);
                const float  Off = abs(dot(BodyFieldNormal, C - Dir * Tf));
                const float  Age = frac(Tick);
                Spark += exp(-D2) * cos(saturate(Off / Half) * 1.5707963f) * smoothstep(0.0f, 0.2f, Age) *
                         (1.0f - smoothstep(0.3f, 1.0f, Age)) * ((Tc > 0.0f) ? 1.0f : 0.0f);
            }
        }
        WasCell   = Cell;
        WasBolt   = NowBolt;
        WasKey    = Key;
        Was       = Now;
        WasStrong = Strong;
        WasT      = T;
        if (T >= End)
            break;
    }
    // Held against what a ray straight through the field's middle sums the strength to; the glow and the haze fill
    // up towards a most.
    const float Full = Half * 1.2732395f;
    const float Arc  = min((Core + Bolt) / Full * 1.5f, 1.6f);
    const float Soft = 0.6f * (1.0f - exp(-(Glow / Full) / 0.6f));
    const float Mist = 0.45f * (1.0f - exp(-(Haze / Full) / 0.45f));
    return float4(((BodyFieldHue * (Mist + Soft + 0.5f * Arc) + 0.75f * Arc) +
                   (0.6f * BodyFieldHue + 0.8f) * min(Spark * 1.5f, 2.0f)) * BodyFieldBright, 0.0f);
}

float4 PSBody(VSOut In) : SV_Target
{
    return BodyField(In);
}
#if RDX_LIGHT_TARGET
// As the glass's mirror layer: the light target left alone, the added target as the picture.
PSLitOut PSBodyLit(VSOut In)
{
    PSLitOut Out;
    Out.Colour = BodyField(In);
    Out.Lit    = float4(0.0f, 0.0f, 0.0f, 0.0f);
    Out.Added  = Out.Colour;
    return Out;
}
#endif

// Water=Low and up: the engine's waterfall - its animated sheet, Effects.water.WtrFall, modulated, so drawn as it was
// it only darkens what is behind it - as falling water (BLEND_Fall, over the water's copy of the picture), in the
// colour of the water it comes down into. Where on its sheet a point lies comes with it (In.Lv: across, down -
// RenderDevice's FallSheet), so the water behaves as falling water does whatever piece of the sheet the engine hands
// over:
// - it runs down faster the further it has fallen, its lines stretching - read as two phases of a flow that start over
//   in turn, so the stretch never piles up;
// - it sways a little, more the further down;
// - it thickens into streams - where the texture is darker than its own average, which is all a modulated texture says
// -
//   and towards its foot breaks into ribbons with gaps between them, and drops;
// - its sides thin out to nothing, so a fall made of crossed sheets reads as a column and not as its planes.
// What is behind is seen through it, bent by the water and swallowed by its murk where it is thick - the colours its
// hue lacks the most; the water gives back its own colour - a quarter of its hue, as the water's surface takes it - lit
// as what is behind it is lit, so in a dark sewer it stays dark.
// The run's constants (FSolidPass::SetWaterConstants, RenderDevice's FallConstants):
#define FallHue     WaterTint.rgb     // the water's hue, the brightest channel 1
#define FallFoam    WaterTint.a       // how bright its white water is
#define FallTime    WaterView.x       // seconds
#define FallBend    WaterView.y       // how far the water bends the view, in the level's units at the sheet
#define FallSpeed   WaterView.z       // repeats of the texture a second down the sheet, where the fall starts
#define FallFocal   WaterView.w       // the engine's projection, in its own pixels
#define FallDensity WaterCamera.x     // how much of what is behind the water swallows where it is thickest

float FallRead(float2 Uv)
{
    return dot(Tex.SampleBias(Samp, Uv, MipBias).rgb, 1.0f / 3.0f);
}

// An ornamental fountain's curtain (RenderDevice's FallConstants): which way it is drawn - 0 a stream's fall, 1 a sheet
// of water, 2 drops - and the sheet's width and height in the level's units.
#define FallKind    WaterCamera.y
#define FallSize    WaterCamera.zw

// The film of water on a fountain's pane at a place of it (the level's units, from the sheet's top corner): streams
// that run down it, and finer, quicker trickles over them. 0 thin .. 1 thick.
float FountainFilm(float2 At)
{
    const float Streams = FieldFbm(float2(At.x * 0.55f, At.y * 0.045f - FallTime * 1.1f));
    const float Trickle = FieldFbm(float2(At.x * 1.7f + 5.3f, At.y * 0.13f - FallTime * 2.6f));
    return Streams * 0.65f + Trickle * 0.35f;
}

// An ornamental fountain's curtain: water let down a pane out of a lit slot at its top. The game's own picture of it is
// a small texture the engine paints as it paints its fires - one glowing colour, brightest at the top. Here it is
// water: what is behind is seen through it, and what it shows of itself is the light from its top - in that
// texture's hue, half way to white - on the film's streams or on the drops, fading down the sheet.
float3 FountainSurface(VSOut In, out float3 Added)
{
    float2 Size;
    SceneCopy.GetDimensions(Size.x, Size.y);
    const float2 ScreenUv = In.Pos.xy / Size;
    const float  SurfaceZ = 1.0f / max(In.Pos.z, 1e-6f);
    const float  Across   = In.Lv.x;
    const float  Down     = saturate(In.Lv.y);
    const float2 At       = float2(Across * FallSize.x, Down * FallSize.y);
    // The light's colour: the texture's own hue, read at sixteen places of it - the engine repaints it every frame,
    // and only its hue stays.
    float3 Mean = 0.0f;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++)
            Mean += Tex.SampleLevel(Samp, (float2(x, y) + 0.5f) * 0.25f, 0.0f).rgb;
    const float3 Glow = lerp(Mean / max(max(Mean.r, max(Mean.g, Mean.b)), 0.02f), 1.0f, 0.45f);
    // The slot's light on the water: strongest under the slot, a share of it all the way down; and the slot itself.
    const float  Lamp = 0.18f + 0.82f * exp(-2.4f * Down);
    const float  Slot = exp(-Down * FallSize.y * 0.9f);
    // The pane's sides and its foot: the water ends a little short of the frame, and at the foot it is the pool's.
    const float  Edge = smoothstep(0.0f, 0.04f, Across) * smoothstep(1.0f, 0.96f, Across) *
                        (1.0f - smoothstep(0.94f, 1.0f, Down));
    float  Water = 0.0f;        // how much water the view goes through here, 0..1
    float  Shine = 0.0f;        // how much of the slot's light it gives back
    float2 Bend  = 0.0f;        // which way it bends the view, of FallBend
    if (FallKind < 1.5f)
    {
        // A sheet: the film, its streams' flanks catching the light, the view bent by its slant across.
        const float Film  = FountainFilm(At);
        const float Slant = (FountainFilm(At + float2(0.6f, 0.0f)) - Film) / 0.6f;
        const float Flank = saturate(1.0f - abs(Slant * 4.0f - 0.45f) * 2.2f);
        Water = saturate(0.30f + Film * 0.85f);
        Shine = 0.16f * Water + 0.95f * Flank * Flank * Flank + 0.30f * Film * Film;
        Bend  = float2(Slant * 3.0f, (Film - 0.47f) * 0.6f);
    }
    else
    {
        // Drops: threads of them let go along the slot, four rows deep; each falls faster the further it has come, and
        // trails a streak the longer for it, its head the brightest. A thin wet film stays on the pane.
        float Sum = 0.0f;
        [unroll] for (int Row = 0; Row < 4; Row++)
        {
            const float Pitch  = 1.3f + 0.8f * Row;                              // units between two threads
            const float X      = At.x / Pitch + 17.31f * Row;
            const float Thread = floor(X);
            const float Seed   = FieldHash(float2(Thread, 7.0f * Row + 1.0f));
            const float Off    = (FieldHash(float2(Thread, 3.0f * Row + 5.0f)) - 0.5f) * 0.5f;
            const float Side   = (frac(X) - 0.5f - Off) * Pitch;                 // units from the drop's line
            const float Period = 0.45f + 0.45f * Seed;                           // seconds between two drops
            const float T      = frac(FallTime / Period + 7.0f * Seed);
            const float Along  = (T * T - Down) * FallSize.y;                    // units the drop's head is below
            const float Tail   = 0.6f + 5.0f * T;
            const float Thin   = saturate(1.0f - abs(Side) / 0.22f);
            Sum += Thin * (saturate(Along / 0.3f) * saturate(1.0f - Along / Tail) +
                           0.7f * saturate(1.0f - abs(Along - 0.25f) / 0.25f)) * (1.0f - 0.2f * Row);
        }
        const float Wet = FountainFilm(At);
        Water = saturate(0.10f + Sum);
        Shine = 0.03f + 0.05f * Wet + 1.1f * saturate(Sum);
        Bend  = float2(0.0f, saturate(Sum) * 0.8f);
    }
    Water *= Edge;
    Shine *= Edge;
    // What is behind, bent by the water - unless what is there lies in front of the sheet.
    const float  Px     = FallBend * FallFocal / max(SurfaceZ, 1.0f) * Water;
    const float2 BentUv = ScreenUv + Bend * Px * InvHalfSize * 0.5f;
    const float  BentZ  = 1.0f / max(DepthCopy.Load(int3(BentUv * Size, 0)).r, 1e-6f);
    const float3 Behind = SceneCopy.SampleLevel(Samp, (BentZ >= SurfaceZ) ? BentUv : ScreenUv, 0.0f).rgb;
    // The water takes a little of what is behind and gives back a little of its own colour, as a fall does; the slot's
    // light on it is its own.
    const float  Lum  = dot(Behind, float3(0.299f, 0.587f, 0.114f));
    const float3 Keep = exp(-(1.3f - 0.5f * FallHue) * FallDensity * 0.35f * Water);
    const float3 Own  = FallHue * 0.25f * (0.03f + Lum) * Water;
    const float3 Lit  = Glow * (Lamp * Shine + Slot * 0.6f * Edge);
    Added = Own + Lit;
    return Behind * Keep + Own + Lit;
}

float3 FallSurface(VSOut In, out float3 Added)
{
    // An ornamental fountain's curtain is drawn its own way.
    [branch] if (FallKind > 0.5f)
        return FountainSurface(In, Added);
    float2 Size;
    SceneCopy.GetDimensions(Size.x, Size.y);
    const float2 ScreenUv = In.Pos.xy / Size;
    const float  SurfaceZ = 1.0f / max(In.Pos.z, 1e-6f);      // the vertex shader carries the view depth as 1/z
    const float  Across   = In.Lv.x;
    const float  Down     = saturate(In.Lv.y);
    // The texture's own average - its smallest mip: the streams are what is darker than that.
    float2 TexSize;
    float  Levels;
    Tex.GetDimensions(0, TexSize.x, TexSize.y, Levels);
    const float Avg = max(dot(Tex.SampleLevel(Samp, In.Uv, Levels - 1.0f).rgb, 1.0f / 3.0f), 0.02f);
    // The flow: two phases half a period apart, each moving the picture down as far as the water has come in it - faster
    // the further down, as a free fall's speed grows with the root of the drop - and fading out before it starts over.
    const float  Rate  = FallSpeed * (1.0f + 2.0f * sqrt(Down));
    const float  Sway  = sin(FallTime * 1.3f + Down * 7.0f + In.Uv.x * 3.0f) * 0.015f * Down;
    const float  P0    = frac(FallTime * 0.5f);
    const float  P1    = frac(FallTime * 0.5f + 0.5f);
    const float2 Uv    = In.Uv * float2(1.0f, 1.0f - 0.45f * Down) + float2(Sway, 0.0f);      // longer lines lower down
    const float  S0    = FallRead(Uv - float2(0.0f, P0 * Rate * 2.0f));
    const float  S1    = FallRead(Uv - float2(0.0f, P1 * Rate * 2.0f) + float2(0.5f, 0.0f));
    const float  W0    = 1.0f - abs(2.0f * P0 - 1.0f);
    const float  Flow  = S0 * W0 + S1 * (1.0f - W0);
    const float  Now   = FallRead(In.Uv);                   // the engine's own frame
    const float  Mix   = Now * 0.4f + Flow * 0.6f;
    float        Streams = saturate((Avg - Mix) / Avg * 1.2f);
    // Towards the foot it breaks: fewer, thinner ribbons with gaps between them, and drops - the finest dark specks of the
    // texture, running fast.
    const float  Break = smoothstep(0.55f, 1.0f, Down);
    Streams = saturate((Streams - Break * 0.45f) / max(1.0f - Break * 0.45f, 0.2f));
    const float  Fine  = FallRead(In.Uv * float2(2.3f, 0.8f) + float2(0.13f, -FallTime * Rate * 1.5f));
    const float  Drops = Break * saturate((Avg * 0.7f - Fine) / (Avg * 0.7f) * 2.5f);
    // The sides thin out to nothing.
    const float  Edge  = smoothstep(0.0f, 0.22f, Across) * smoothstep(1.0f, 0.78f, Across);
    // And its last tenth thins out too, where it meets what it falls onto: no sheet's edge there, the splash is.
    const float  Foot  = 1.0f - smoothstep(0.9f, 1.0f, Down);
    const float  Water = saturate((Streams + Drops * 0.8f + 0.35f * (1.0f - 0.6f * Break)) * Edge * Foot);
    // What is behind, bent by the water - less in pixels the further off the sheet is - unless what is there lies in front
    // of the sheet: that is not seen through it.
    const float  Px     = FallBend * FallFocal / max(SurfaceZ, 1.0f) * Water;
    const float2 Bend   = float2(S0 - S1, Flow - Now) * Px * InvHalfSize * 0.5f;
    const float2 BentUv = ScreenUv + Bend;
    const float  BentZ  = 1.0f / max(DepthCopy.Load(int3(BentUv * Size, 0)).r, 1e-6f);
    const float3 Behind = SceneCopy.SampleLevel(Samp, (BentZ >= SurfaceZ) ? BentUv : ScreenUv, 0.0f).rgb;
    // The murk swallows what is behind where the water is thick, and the water gives back its own colour, lit as what is
    // behind it is lit; white water on top where the streams are thickest, and on the drops.
    const float  Lum    = dot(Behind, float3(0.299f, 0.587f, 0.114f));
    const float3 Keep   = exp(-(1.3f - 0.5f * FallHue) * FallDensity * Water);
    const float3 Own    = FallHue * 0.25f * (0.03f + 1.0f * Lum) * Water;
    const float3 White  = FallHue * (0.02f + 1.0f * Lum) * FallFoam * (Streams * Streams + Drops) * Edge;
    Added = Own + White;
    return Behind * Keep + Own + White;
}

float4 PSFall(VSOut In) : SV_Target
{
    float3 Added;
    return float4(FallSurface(In, Added), 1.0f);
}
#if RDX_LIGHT_TARGET
PSLitOut PSFallLit(VSOut In)
{
    PSLitOut Out;
    float3 Added;
    Out.Colour = float4(FallSurface(In, Added), 1.0f);
    Out.Lit    = float4(0.0f, 0.0f, 0.0f, 0.0f);
    Out.Added  = float4(Added, 1.0f);
    return Out;
}
#endif
// The water's splashes (FBackend::DrawSplashes), over the finished world: the drops' smoothed thickness (Light) - where
// there is enough of it is water, and its slant is the surface's - and the nearest drop's depth around (Tex), over a fresh
// copy of the picture (SceneCopy), laid on as water: what is behind bent through it, the lamps' light and glints on it,
// the surroundings given back at a flat angle, whiter where it is thick with air. For this run the water's constants hold
// the view: its axes in the world (WaterTint, WaterView, WaterCamera .xyz), the focal length (WaterTint.w) and the
// screen's centre in the engine's pixels (WaterView.w, WaterCamera.w).
float3 SplashView(int2 P, float Z, float2 EngPerPx)
{
    return float3(((float2(P) + 0.5f) * EngPerPx - float2(WaterView.w, WaterCamera.w)) * (Z / WaterTint.w), Z);
}

// An ornamental fountain's falling water among the splashes' (FBackend::DrawSplashes hands over the nearest three
// curtains in the run's numbers the view leaves free, six each: the slot's one end from the camera on the level's
// axes, its other end's X and Y, and the fall's height - zero where there is none; the screen's centre is in
// WaterScreen.xy, where the water's own mirror looks for it; and in WaterSpare.yzw how far back from the slot each
// one's chute goes - nought with none, below zero where it lies on the other side of the curtain's plane than Out).
// At a point of the water, Q from the camera on the level's axes: 0 where it is no fountain's; otherwise how much of
// the slot's light shows in it - the most right under the slot, less down the fall and out from it, and up the chute
// as down the fall: the chute's far end gets what the fall's foot gets - with To, the way from the point to the
// slot's light, Under, how high the camera is over the fountain's pool, Lie: which way the surface the water lies
// on faces there, on the level's axes, and (w) how much it lies at all - on the chute; falling, not at all - and
// Glow: how much of that light is in the water itself there, which no chute carries further than it goes.
// The light is the slot's own, a little out from the curtain on the camera's side: lit from straight above, falling
// water would show nothing of it to an eye in front.
float CurtainLightAt(float3 Q, out float3 To, out float Under, out float4 Lie, out float Glow)
{
    const float ChuteRun[3] = { WaterSpare.y, WaterSpare.z, WaterSpare.w };
    Lie  = float4(0.0f, 0.0f, 1.0f, 0.0f);
    Glow = 0.0f;
    const float C[18] = { WaterAxes.x,  WaterAxes.y,  WaterAxes.z,  WaterAxes.w,  WaterScreen.z, WaterScreen.w,
                          WaterExtra.x, WaterExtra.y, WaterExtra.z, WaterExtra.w, WaterWave.x,   WaterWave.y,
                          WaterWave.z,  WaterWave.w,  WaterDirs.x,  WaterDirs.y,  WaterDirs.z,   WaterDirs.w };
    To    = float3(0.0f, 0.0f, 1.0f);
    Under = 1.0f;
    [unroll] for (int i = 0; i < 3; i++)
    {
        const float3 A    = float3(C[i * 6], C[i * 6 + 1], C[i * 6 + 2]);
        const float2 B    = float2(C[i * 6 + 3], C[i * 6 + 4]);
        const float  Fall = C[i * 6 + 5];
        if (Fall <= 0.0f)
            continue;
        const float  Long = max(length(B - A.xy), 1.0f);
        const float2 U    = (B - A.xy) / Long;
        const float2 Out  = float2(U.y, -U.x);
        const float3 Rel  = Q - A;
        const float  S    = dot(Rel.xy, U);
        const float  Off  = dot(Rel.xy, Out);
        // The fountain's water: around the fall, and over the chute - on its side of the curtain's plane, as far
        // behind the slot as it goes.
        const float  Run   = abs(ChuteRun[i]);
        const float  Back  = (ChuteRun[i] < 0.0f) ? -Off : Off;
        const float  Reach = 0.6f * max(Fall, 4.0f);
        if (S < -12.0f || S > Long + 12.0f || Back < -16.0f || Back > 16.0f + Run ||
            Rel.z > 4.0f + ((Back > 0.0f) ? Run : 0.0f) || Rel.z < -(Fall + 4.0f))
            continue;
        const float  Side  = (dot(A.xy, Out) > 0.0f) ? -1.0f : 1.0f;        // the camera's side of the curtain
        const float3 Slot  = float3(A.xy + U * clamp(S, 0.0f, Long), A.z) - Q;
        To    = Slot + float3(Out * (Side * 0.35f * Fall), 0.0f);
        Under = max(Fall - A.z, 1.0f);
        // Water over the slot and behind it lies on the chute - as the line from the slot up to it lies. The water
        // that falls is under the slot, and none of this is its.
        const float  Lying = (Run > 0.0f) ? saturate((Back - 1.0f) * 0.25f) * saturate(Rel.z * 2.0f - 1.0f) : 0.0f;
        const float2 Fore  = Out * ((ChuteRun[i] < 0.0f) ? 1.0f : -1.0f);      // from the chute towards the slot
        Lie = float4(normalize(float3(Fore * max(Rel.z, 0.0f), max(Back, 1.0f))), Lying);
        const float  Near  = (Run > 0.0f) ? min(Fall / Run, 1.0f) : 1.0f;
        const float  Far2  = dot(Slot, Slot) - Lying * (1.0f - Near * Near) * (Back * Back + Rel.z * Rel.z);
        Glow = 1.0f / (1.0f + dot(Slot, Slot) / (Reach * Reach));
        return 1.0f / (1.0f + max(Far2, 0.0f) / (Reach * Reach));
    }
    return 0.0f;
}

float4 SplashColour(float4 Pos)
{
    const int2  P     = int2(Pos.xy);
    const float Thick = Light.Load(int3(P, 0)).r;
    const float Z     = Tex.Load(int3(P, 0)).r;
#if RDX_RT_HOST
    // Before any pixel is thrown away: RtFind reads the two by two together.
    RtFind(Pos);
#endif
#if RDX_RT_ASK
    // No pixel to throw away in the build that only asks: where there is no water nothing is asked.
    if (Thick < 0.05f || Z >= 1.0e5f)
    {
        GRtAskDead = true;
        return 0.0f;
    }
#else
    if (Thick < 0.05f || Z >= 1.0e5f)
        discard;
#endif
    float2 Size;
    SceneCopy.GetDimensions(Size.x, Size.y);
    const float2 EngPerPx = (2.0f / InvHalfSize) / Size;
    const float3 V = SplashView(P, Z, EngPerPx);
    // The surface's slant from the thickness around: half of it rises towards the camera, so its slope in world units -
    // two pixels either way, put into world units at the drop - tilts the normal.
    const float  PerUnitPx = WaterTint.w / Z / EngPerPx.x;
    const float2 Slope = 0.125f * PerUnitPx *
                         float2(Light.Load(int3(P + int2(2, 0), 0)).r - Light.Load(int3(P - int2(2, 0), 0)).r,
                                Light.Load(int3(P + int2(0, 2), 0)).r - Light.Load(int3(P - int2(0, 2), 0)).r);
    const float3 N     = normalize(float3(-Slope, -1.0f));
    const float3 ToEye = normalize(-V);
    const float  Back  = 0.02f + 0.98f * pow(1.0f - saturate(dot(N, ToEye)), 5.0f);
    const float2 Uv    = (float2(P) + 0.5f) / Size;
    // The shifts below are world units at the drop, put onto the screen: a small, far splash bends little, a near one more.
    const float  PerUnit = WaterTint.w / Z / EngPerPx.x / Size.x;
    // What shows through, bent by the surface's slant - the more, the thicker the water.
    const float3 Behind = SceneCopy.SampleLevel(Samp, saturate(Uv + N.xy * (0.5f * min(Thick, 12.0f) * PerUnit)), 0.0f).rgb;
    // What it gives back at a flat angle: the picture around the drop stands in for the room it is in.
    const float3 Around = SceneCopy.SampleLevel(Samp, saturate(Uv - N.xy * (6.0f * PerUnit)), 0.0f).rgb;
    // The lamps over the water (FBackend::SetWaterLamps): their light, wrapped round - a drop lets light through - and
    // their glints.
    const float3 AX = WaterTint.xyz, AY = WaterView.xyz, AZ = WaterCamera.xyz;
    float3 Lamps = 0.0f, Glints = 0.0f;
    const int Count = (int)WaterLampCount.x;
    [loop] for (int i = 0; i < Count; i++)
    {
        const float4 L    = WaterLamp[i * 2];
        const float3 To   = float3(dot(L.xyz, AX), dot(L.xyz, AY), dot(L.xyz, AZ)) - V;
        const float  Dist = length(To);
        if (Dist >= 2.0f * L.w)
            continue;
        const float3 Dir  = To / Dist;
        const float  Fall = saturate(1.0f - Dist / L.w);
        Lamps  += WaterLamp[i * 2 + 1].rgb * (saturate(dot(N, Dir) * 0.5f + 0.5f) * Fall * 2.0f);
        Glints += WaterLamp[i * 2 + 1].rgb *
                  (pow(saturate(dot(N, normalize(Dir + ToEye))), 200.0f) * 12.0f * Back * saturate(2.0f - Dist / L.w));
    }
    // An ornamental fountain's water is clear water, as the level's own is: what is behind it shows through, bent by
    // its slant; the room shows in it, mirrored by that slant - the water's own mirror, on rays where the water runs
    // on them, since the room a curtain facing the eye mirrors lies behind the eye, and is in no picture; and the
    // slot it leaves is lit, downwards - the pool's light too (PoolLight), in the same colour - which glints on it
    // and, a little, lights it through.
    float3 ToSlot;
    float  Under;
    float4 Lie;
    float  Glow;
    const float3 Place = V.x * AX + V.y * AY + V.z * AZ;
    const float  Slot  = CurtainLightAt(Place, ToSlot, Under, Lie, Glow);
    if (Slot > 0.0f)
    {
        // A film's surface is far gentler than a splash's drops': the same slant, a third as steep.
        const float3 Film  = normalize(float3(-Slope * 0.35f, -1.0f));
        // Falling, the film faces the eye, as its drops are drawn. On the chute it lies as the chute does (Lie), and
        // the room and the lights show in it as in a lying surface - never one that faces away from the eye -
        // slanted by the same slope, which counts for more there: running water is rougher than falling.
        float3 Lain = float3(dot(Lie.xyz, AX), dot(Lie.xyz, AY), dot(Lie.xyz, AZ));
        Lain.z = min(Lain.z, -0.05f);
        const float3 Face  = normalize(float3(-Slope * lerp(0.35f, 0.7f, Lie.w), 0.0f) +
                                       normalize(lerp(float3(0.0f, 0.0f, -1.0f), Lain, Lie.w)));
        // The lamps' glints on the lying film are the lying surface's: where its slant shows a lamp to the eye.
        float3 Glinted = Glints;
        if (Lie.w > 0.0f)
        {
            const float Graze = 0.02f + 0.98f * pow(1.0f - saturate(dot(Face, ToEye)), 5.0f);
            float3 Shown = 0.0f;
            [loop] for (int k = 0; k < Count; k++)
            {
                const float4 L    = WaterLamp[k * 2];
                const float3 To   = float3(dot(L.xyz, AX), dot(L.xyz, AY), dot(L.xyz, AZ)) - V;
                const float  Dist = length(To);
                if (Dist >= 2.0f * L.w)
                    continue;
                Shown += WaterLamp[k * 2 + 1].rgb * (pow(saturate(dot(Face, normalize(To / Dist + ToEye))), 200.0f) *
                                                     12.0f * Graze * saturate(2.0f - Dist / L.w));
            }
            Glinted = lerp(Glints, Shown, Lie.w);
        }
        // The mirror's frame: the level's axes, from the place under the camera at the pool's height.
        const float3 Eye   = float3(0.0f, 0.0f, Under);
        const float3 Sp    = Eye + Place;
        const float3 Up    = normalize(Face.x * AX + Face.y * AY + Face.z * AZ);
        const float3 Ray   = reflect(normalize(Place), Up);
        // The mirror image as the level's water has it (WaterSpare.x, as a water run is told): none on Low, the
        // picture's own with the reflections on, the rays' where the water runs on rays.
        float4 Mirror = 0.0f;
        if (WaterSpare.x > 0.5f)
        {
#if RDX_RT
            Mirror = WaterReflectRt(Eye, Sp, Ray, AX, AY, AZ, WaterTint.w, Size, 0.25f, false);
#else
            Mirror = WaterReflect(Eye, Sp, Ray, AX, AY, AZ, WaterTint.w, Size);
#endif
        }
        const float3 Room    = lerp(Around, Mirror.rgb, Mirror.a);
        // Water lets nearly all the light through: of what falls on it, a few hundredths come back off it where it
        // is looked straight at, and nearly all only at a flat angle. A little more than still water gives back,
        // the film is never still - but it is water, and what is behind it shows through it.
        const float  Shine   = (WaterSpare.x > 0.5f)
                             ? saturate(0.04f + 0.96f * pow(1.0f - saturate(dot(Face, ToEye)), 5.0f)) : 0.0f;
        const float  Body    = saturate(Thick * 1.5f);
        const float3 Dir     = normalize(float3(dot(ToSlot, AX), dot(ToSlot, AY), dot(ToSlot, AZ)));
        const float  Mirrors = (WaterSpare.x > 0.5f) ? 1.0f : 0.0f;
        // The slot's light on the water, as light is on water. Off its top comes back what water gives back at that
        // angle and no more - its glints, small and sharp as a lamp's are (Glints); on the chute, fainter, also where
        // its streaks would show it facing the eye as the falling water does. They are mirror images, and are not
        // there on Low.
        const float3 Half    = normalize(Dir + ToEye);
        const float  Given   = 0.04f + 0.96f * pow(1.0f - saturate(dot(ToEye, Half)), 5.0f);
        const float3 Spark   = POOL_LIGHT * (12.0f * Given * Slot *
                                             max(pow(saturate(dot(Face, Half)), 200.0f),
                                                 pow(saturate(dot(Film, Half)), 200.0f) * 0.25f * Lie.w));
        // The rest of that light goes on through the water. Under the film that lies on the chute it lights what
        // the film lies on, seen through it - whole water has nothing in it to hold light. Falling water breaks up
        // and takes air, and a little of the light shows in the water itself; where nothing mirrors (Water=Low)
        // more of it does, lying or falling, or the water would hardly be seen.
        const float3 Lights  = POOL_LIGHT * (Glow * Lie.w * Mirrors);
        const float3 Veil    = POOL_LIGHT * (Glow * lerp(0.20f, 0.07f * (1.0f - Lie.w), Mirrors) * Body);
        const float2 Bent    = saturate(Uv + Film.xy * (2.4f * PerUnit * saturate(0.3f + 2.0f * Thick)));
        const float3 Through = SceneCopy.SampleLevel(Samp, Bent, 0.0f).rgb * (1.0f + Lights);
        // A strand is thin, and is water all the same: it is there from very little of it on.
        return float4(Through * (1.0f - Shine) + Room * Shine + Veil + (Spark + Glinted) * Mirrors,
                      smoothstep(0.01f, 0.12f, Thick));
    }
    // Splashing water is full of air: whitish all through, whiter where it is thick - lit by the lamps, and never darker
    // than the room around it.
    const float3 Shine = max(Lamps, 0.75f * (Behind + Around));
    const float  Foam  = 0.2f + 0.4f * saturate(Thick / 16.0f);
    const float3 Water = lerp(Behind, Shine, Foam);
    return float4(Water * (1.0f - Back) + Around * Back + Glints, smoothstep(0.05f, 1.5f, Thick));
}

float4 PSSplash(float4 Pos : SV_Position) : SV_Target
{
    return SplashColour(Pos);
}
#if RDX_RT_ASK
// The splashes' rays alone: the one triangle over the target drawn as it is drawn, and the mirror ray of every pixel
// that holds falling water written into the rays' list for the host. No target.
void PSSplashAsk(float4 Pos : SV_Position, uint Cover : SV_Coverage)
{
    SplashColour(Pos);
    RtAskFlush(Pos, Cover);
}
#endif
#if RDX_LIGHT_TARGET
// With the shadows' three targets: the light target is left alone, and the added target gets the splash as the picture
// does - so the shadows, laid on after the world, leave it alone.
PSLitOut PSSplashLit(float4 Pos : SV_Position)
{
    PSLitOut Out;
    Out.Colour = SplashColour(Pos);
    Out.Lit    = float4(0.0f, 0.0f, 0.0f, 0.0f);
    Out.Added  = Out.Colour;
    return Out;
}
#endif

// The screen tint (EndFlash): one triangle over the whole target. The fog colour is the
// source; the blend multiplies what is already there by the flash scale.
float4 FlashVS(uint Id : SV_VertexID) : SV_Position
{
    float2 P = float2((Id << 1) & 2, Id & 2);
    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);
}
#if RDX_LIGHT_TARGET
// With the shadows' three targets: the tint into the picture, and blended the same way into the added target.
struct FlashOut { float4 Colour : SV_Target0; float4 Lit : SV_Target1; float4 Added : SV_Target2; };
FlashOut FlashPS(float4 Pos : SV_Position)
{
    FlashOut Out;
    Out.Colour = float4(FlashFog, 1.0f);
    Out.Lit    = float4(0.0f, 0.0f, 0.0f, 0.0f);
    Out.Added  = Out.Colour;
    return Out;
}
#else
float4 FlashPS(float4 Pos : SV_Position) : SV_Target
{
    return float4(FlashFog, 1.0f);
}
#endif

// SecondRevision - the geometry pass: textured, per-vertex coloured triangles.
//
// Vertices arrive from the engine already projected and clipped, in pixels
// (docs/spec-03 section 2), so this pass does no transform beyond turning pixels into
// clip space. Its real job is batching: measurement showed two very different load
// profiles - hundreds of tiny tiles in menus, thousands of gouraud polygons in play
// (docs/meres-01 section 3b.4c) - and both are dominated by draw-call count, so
// consecutive geometry sharing a texture is merged into one draw.
#pragma once

// For FRdxColours: the formats the HDR key chooses, which this pass's pipelines are built for.
#include "FinalPass.h"

class FTextureCache;

// Screen-space shadows' second render target (FSolidPass::Init with stencil kinds): the light each surface receives - the
// light map times the vertex colour, as luminance - for the shadow pass to weigh a light's share against. One float.
static const DXGI_FORMAT RDX_LIGHT_TARGET_FORMAT = DXGI_FORMAT_R16_FLOAT;

// How a run of geometry is combined with what is already on screen. Which mode a
// surface wants comes from its PolyFlags, and the flags mean different things on
// different call paths (docs/spec-02 section 1.3), so the choice is made by the
// engine-facing side, not here.
enum EBlendMode
{
    BLEND_Opaque = 0,   // ordinary geometry; masked pixels discarded, depth written
    BLEND_Translucent,  // src + dst*(1-src), measured (docs/meres-07 13): black adds nothing, so fades work
    BLEND_Modulated,    // multiplies what is behind it; used for shadows and decals
    // Same as opaque, but writes no depth. Every two-dimensional tile uses this: an
    // overlay must not become an occluder for whatever is drawn after it.
    BLEND_OpaqueNoZWrite,
    // Translucent, but an occluder all the same: the level marks it PF_Occlude, and it
    // writes depth. Measured on the reflective UNATCO floor (docs/meres-07 14).
    BLEND_TranslucentOccluding,
    // A see-through water surface (Water=Low and up, docs/meres-11): drawn as the translucent blend draws it, but over
    // a copy of the picture and the depth taken at the frame's first water surface, so what shows through can waver and
    // take on the water's colour with depth. Its own constants ride on the run (SetWaterConstants).
    BLEND_Water,
    // The engine's volumetric light laid over a surface (its fog map): fog + dst * (1 - fog's alpha), the fog map's own
    // colour and how much of the surface it covers. No depth, no stencil - it is over the surface, not a surface.
    BLEND_Fog,
    // The same over a masked surface: only where the surface itself wrote the depth (EQUAL) - through its holes shows
    // what lies behind, and that is not the surface's to fog. The same vertices and the same vertex shader give the same
    // depth, to the bit.
    BLEND_FogEqual,
    // A waterfall (Water=Low and up): a sheet the level lets water fall down, drawn as falling water over the same copy
    // of the picture the water reads (PSFall) - what is behind bent by its streams, the streams white. The shader makes
    // the whole result itself, so nothing is blended; no depth, no stencil. Its constants ride on the run as the
    // water's do.
    BLEND_Fall,
    // A water surface the level leaves opaque (Water=Low and up): the sewer's sludge (docs/meres-11). Cut and lifted by
    // its waves as BLEND_Water is (VSWater, HSWater, DSWater), but drawn opaque (PSSludge), writing the depth, the
    // stencil kind and the light target as an opaque surface does. Nothing is seen through it, so it reads no copy of
    // the picture; its mirror image is a layer of its own (BLEND_SludgeMirror). Its constants ride on the run as the
    // water's do.
    BLEND_Sludge,
    // Its fog map, over the same lifted patches (PSSludgeFog; the fog's blend, no depth, no stencil): drawn flat it would
    // lie behind the waves' crests and show only in their troughs. The fog map's coordinates ride in the light map's place.
    BLEND_SludgeFog,
    // Its mirror image (Reflections=High and up), over the same lifted patches (PSSludgeMirror): what the surface gives
    // back, premultiplied by its sheen - rgb + dst * (1 - a) - from the copy of the picture the see-through water
    // reads. The sludge is drawn among the level's walls, while much of what it mirrors is not drawn yet; this waits
    // with the blended runs, by when it is. No depth, no stencil; the added target as the picture, so the shadows
    // darken the surface's own colour under it and not what it gives back.
    BLEND_SludgeMirror,
    // The levels' glass (Reflections=High and up): what a pane gives back, laid over the pane - which is drawn before
    // it as the level draws it, and stays so (PSGlassMirror): premultiplied by its share, rgb + dst * (1 - a), from the
    // copy of the picture the water reads, or from rays. Ordinary triangles; it waits with the blended runs, after
    // its own pane. No depth, no stencil; the added target as the picture. Its constants ride on the run as the
    // water's do.
    BLEND_GlassMirror,
    // An effect's body in a view the passes at the world's end are not made for - a flame in a mirror of the level's
    // (PSBody): a square before the body, whose every pixel walks its own ray through the body and gives its light.
    // Laid on as the glass's layer is: rgb + dst * (1 - a). No depth, no stencil; its constants ride on the run.
    BLEND_Body,
    BLEND_MAX
};

// UVs are normalised here, not in texels: the engine hands over texel coordinates
// (docs/spec-02, spec-04) and dividing by the texture size on the CPU keeps the
// shader free of per-texture constants.
struct FSolidVertex
{
    FLOAT X, Y;        // pixels, origin top left
    FLOAT Depth;       // reversed-Z: 1 = closest, 0 = far plane
    FLOAT U, V;        // diffuse, normalised (may exceed 0..1: tiling is expected)
    FLOAT LU, LV;      // light map, normalised; exactly one map covers the surface
    FLOAT R, G, B, A;
    // Where the eye is, said in the surface's own axes, for the relief (POM). The engine hands surfaces over already
    // turned into the view's own space, where the eye sits at the origin, so the direction to it is simply the
    // vertex's own place negated. A DIRECTION is carried, not the sliding itself: the sliding is a quotient, and a
    // quotient cannot be interpolated across a polygon - that is worked out per pixel.
    // Zero unless a surface actually has a height map, and zero is what tells the shader there is no relief here,
    // so every other path that fills a vertex is left exactly as it was.
    FLOAT PX = 0.0f, PY = 0.0f;
    FLOAT PN = 0.0f;   // the eye's height over the surface: the way's outward part, the same at every corner
    FLOAT PD = 0.0f;   // the texture's own rate, the mean of its two (RdxMath::PomVertex)
};

// What a run on rays binds besides the water's own (FTargetSwitch::BeginMirrorAsk, SolidPass.hlsl's RT part): in the
// scene's place the rays' table until the host has answered, and its requests from then on; the rays' table; the
// casters' texture places - the frame's and the arena's; in the masks' place the rays' list's views in the texture
// heap; the level's colours; and the far probe.
struct FRtWaterBind
{
    D3D12_GPU_VIRTUAL_ADDRESS   Scene;
    D3D12_GPU_VIRTUAL_ADDRESS   Table;           // FRtPass::PrepareWater's
    D3D12_GPU_VIRTUAL_ADDRESS   FrameUvw;
    D3D12_GPU_VIRTUAL_ADDRESS   ArenaUvw;
    D3D12_GPU_DESCRIPTOR_HANDLE Masks;
    D3D12_GPU_VIRTUAL_ADDRESS   LevelMaterial;
    D3D12_GPU_DESCRIPTOR_HANDLE Far;             // the far probe's view in the texture heap (FRtPass::GetFar)
    // The table the builds that write their rays down bind as the water's UAVs - the water's own depth's view, and
    // by FSolidPass::SetRayListViews' offset from it the pixels' heads and the requests.
    D3D12_GPU_DESCRIPTOR_HANDLE Asked;
};

// Supersampling's one change of render target within a frame (docs/terv-latvany-roadmap.md 1.3). The HUD's
// first tile ends the scene: the scene is box-filtered into the output-sized target, and the rest of the
// frame is drawn there, at the output's own resolution. Which targets those are is the backend's business -
// Flush calls this at the marker (FSolidPass::AddTargetSwitch) and then sets its own state again.
class FTargetSwitch
{
public:
    // Records the switch on Cmd, leaves the output-sized target bound with the output's viewport, and
    // returns the depth view the rest of the frame clears (ClearZ).
    virtual D3D12_CPU_DESCRIPTOR_HANDLE SwitchToOutput(ID3D12GraphicsCommandList* Cmd) = 0;
    // The engine's ClearZ within a frame (a run marked bClearDepthFirst), given the depth view in use. Clears it and
    // returns it - or, when the owner keeps the depth drawn so far (the G-buffer's depth view), binds and clears
    // another one and returns that. The render target stays the one bound.
    virtual D3D12_CPU_DESCRIPTOR_HANDLE ClearDepthMidFrame(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv) = 0;
    // A mirror's view of the level has drawn its surfaces (a run AddMirrorEnd left, with the owner's number for the
    // view): what the owner lays over the whole of that view goes on here, while its depth is still there and
    // before the view's own bodies. The scene's targets are left bound as they were.
    virtual void MirrorViewEnd(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv, INT Index) = 0;
    // The frame's world starts here (FSolidPass::AddWorldStart): the depth buffer being drawn into holds the world from now
    // on, for the owner to keep through the frame's later clears.
    virtual void MarkWorldDepth() = 0;
    // The frame's first water run is next: copy the picture and the depth drawn so far where the water pipeline reads them
    // (the texture heap's reserved views), and leave the scene's targets bound again. False when there is nothing to copy
    // into - the water is then drawn as plain translucent geometry.
    virtual UBOOL CopyForWater(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv) = 0;
    // The same copies once more in the frame: a run that makes its whole pixel from the copy is next - a fall's sheet -
    // and the picture has been drawn on since the copies were made. What was laid on it would be lost under that run.
    // False when they could not be made again: the run then draws from the copies there are.
    virtual UBOOL CopyForWaterAgain(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv) = 0;
    // The water's splashes are made ready for this pass to lay on (FSolidPass::SetSplashViews, PSSplash) - a fresh copy of
    // the picture, the drops' smoothed depth and thickness - and the scene's targets are bound again. Asked twice a frame:
    // before the first water surface for the splashes the camera is under (bUnderWater - the surface then shows them bent,
    // as it shows everything above it), and where the world is finished for the rest. Frame: this pass's frame constants
    // (the engine's picture size and the jitter). Composite: the laying-on's constants, filled. False when there is
    // nothing to lay on.
    virtual UBOOL DrawSplashes(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv, const FLOAT* Frame,
                               FLOAT* Composite, UBOOL bUnderWater) = 0;
    // The same for a mirror's view of the level that has drawn its surfaces (the owner's number for the view, as
    // MirrorViewEnd has it): that view's splashes and fountains' drops made ready, for the surface to be laid on.
    virtual UBOOL DrawMirrorSplashes(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv,
                                     const FLOAT* Frame, INT Index, FLOAT* Composite) = 0;
    // A see-through water run is next: the water's own depth (FBackend's WaterSurface) made writable for its pixel shader
    // (u3), cleared on the frame's first water run. Returns the texture heap index of the view to bind there, a view of
    // nothing when there is none; bWrite: whether the water writes it this frame.
    virtual INT WaterSurfaceView(ID3D12GraphicsCommandList* Cmd, UBOOL& bWrite) = 0;
    // Between a water run's two goes: everything the first wrote into the water's own depth is in it before the second
    // reads it.
    virtual void WaterSurfaceSettle(ID3D12GraphicsCommandList* Cmd) = 0;
    // Geometry=Tessellation: the relief's first patches are next. The depth drawn so far is copied into the covers - the
    // depth the relief's domain shader finds what stands above a surface in (ReliefClear) - and the covers are left bound
    // as the only target, for this pass to draw the rest of the world's covers into (DrawReliefCovers). False, with
    // nothing changed, when there are no covers this frame: the relief then reads a view of nothing.
    virtual UBOOL BeginReliefCovers(ID3D12GraphicsCommandList* Cmd) = 0;
    // The covers are drawn: readable from here on, and the scene's targets bound again.
    virtual void  EndReliefCovers(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv) = 0;
    // The frame's panes of glass go into the mirrors' map before the world is drawn, for the flashlight's light off
    // them (FBeamBounce, PSPaneKeep): the map bound as the only target, what the rays bind, and the map's size - with,
    // third in Size, how far from the view's axis the map is ever looked up, as a cosine (0: not known). False, with
    // nothing changed, in a frame whose panes give no light back. Then the scene's targets again.
    virtual UBOOL BeginPaneKeep(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind, FLOAT* Size) = 0;
    virtual void  EndPaneKeep(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv) = 0;
    // Through the 64-bit host the panes' rays are asked first, a round of their own before the world: whether the
    // frame's panes give light back and the host can be asked; the list emptied; no target and no depth bound;
    // what the pipelines bind, and the map's size. EndMirrorAsk hands the rays over, and BeginPaneKeep then takes
    // Bind as that left it.
    virtual UBOOL BeginPaneAsk(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind, FLOAT* Size) = 0;
    // The mirror rays through the 64-bit host (FSolidPass::SetRtHosted, docs/terv-rt-host.md), right after the water's
    // copies: the glass's mirror layers of the flush first write down the rays each pixel would trace - into the rays'
    // list, a pixel's requests together - the host traces them, and the layers are then drawn reading its answers.
    // BeginMirrorAsk: whether the host can be asked this frame; the list emptied; no target bound, and the depth Dsv
    // as the layers' own drawing tests it; and what the pipelines bind (Bind). EndMirrorAsk: the rays handed to the
    // host and traced, and Bind as the layers that read the answers bind it - the requests in the scene's place. The
    // frame is split there: NOTHING set on Cmd survives it - the scene's targets and the viewport are bound again,
    // the rest is this pass's to set. False: no answers this frame, and the layers are left out.
    virtual UBOOL BeginMirrorAsk(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind,
                                 D3D12_CPU_DESCRIPTOR_HANDLE Dsv) = 0;
    virtual UBOOL EndMirrorAsk(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv,
                               FRtWaterBind& Bind) = 0;
protected:
    ~FTargetSwitch() {}
};

class FSolidPass
{
public:
    FSolidPass();
    ~FSolidPass();

    // bStencilKinds: the screen-space shadows' prototype (docs/terv-latvany-roadmap.md 3.3) - the depth buffer carries
    // a stencil, and every draw writes what kind of surface it is into it (see SetKind).
    // A measurement only, asked before Init (SecondRevision-shadow-added-off.txt): the translucent draws leave the added
    // target alone, so a frame shows what its blends cost where much is laid over the picture (Battery Park's steam).
    void  SetAddedTranslucentOff(UBOOL bOff) { bAddedTranslucentOff = bOff ? 1 : 0; }
    // A measurement only (SecondRevision-water-mark-off.txt): a see-through water run is not gone over first to mark the
    // nearest of its points - drawn as it comes, its later triangles over its earlier ones.
    void  SetWaterMarkOff(UBOOL bOff) { bWaterMarkOff = bOff ? 1 : 0; }
    // A measurement only (SecondRevision-draws-by-mode.txt): the draws by blend mode and surface kind - runs, triangles,
    // and the area their triangles cover in screens (Flush) - logged every 150 frames. Costs a pass over the vertices.
    void  SetModeStats(UBOOL bOn) { bModeStats = bOn ? 1 : 0; }
    // With DRED on (FBackend::RequestDred), every run's draw is preceded by a marker naming it, so a lost device's
    // breadcrumbs say which run the GPU stopped in.
    void  SetDredMarkers(UBOOL bOn) { bDredMarkers = bOn ? 1 : 0; }
    // The sub-pixel shift a temporal upscaler (FSR 3.1) asks for, in clip space: X = 2 * jitter in pixels / width,
    // Y = -2 * jitter / height, since the screen's y runs down and clip space's up. Zero without one, and then the
    // vertex shader adds nothing - the vanilla path is untouched (docs/szabaly-vanilla.md).
    void  SetJitter(FLOAT X, FLOAT Y) { JitterX = X; JitterY = Y; }
    // The lamps' strength: 1.0 is the engine's own, 2.0 twice as much. It multiplies the baked
    // lighting in the picture ONLY - not the light target the shadow factor reads as the pixel's total light. Were it to
    // scale that too, every light's share would shrink, the shadows would go lighter, the metric would improve, and the
    // compensation would start chasing its own tail. This way the metric keeps measuring what the shadows take, and the
    // boost brightens independently. Unlit places stay dark: the light map is zero there, and zero times anything is zero.
    void  SetLightBoost(FLOAT Scale) { LightBoost = (Scale > 0.0f) ? Scale : 0.0f; }
    FLOAT GetLightBoost() const      { return LightBoost; }
    // The world's textures' mip bias under the temporal upscaler (RdxMath::FsrMipBias): like the jitter it belongs to the
    // world alone - from its first geometry to the switch where the HUD begins.
    void  SetMipBias(FLOAT Bias) { MipBias = Bias; }
    FLOAT GetMipBias() const     { return MipBias; }
    // How many samples the WORLD is drawn with. Between frames only, and before Init: that is what builds the second
    // set of pipelines.
    void  SetMsaaSamples(INT N) { MsaaSamples = (N > 1) ? N : 1; }
    // The depth format and the targets the pipelines are made for: with the shadows' stencil kinds the depth buffer is
    // D32_FLOAT_S8X24_UINT and the pass writes three targets, without them D32_FLOAT and one. Takes effect with the
    // next RebuildPipelines.
    void  SetStencilKinds(UBOOL bOn) { bStencilKinds = bOn ? 1 : 0; }
    // Whether the WORLD is actually drawn multisampled in the frame about to start. Separate from SetMsaaSamples on
    // purpose: the pipelines may exist while MSAA is off for a while (the shadows were switched on and the card
    // cannot write stencil from a shader, say), and a pass that picks its pipelines by what exists rather than by
    // what is bound fails on every triangle.
    void  SetMsaaLive(UBOOL b) { bMsaaLive = b ? 1 : 0; }
    // Rebuild the pipelines for a new sample count, between frames (the renderer's menu).
    UBOOL RebuildPipelines(ID3D12Device* Device);
    // The rays are the 64-bit host's to trace, on every card (docs/terv-rt-host.md): this pass traces none. Before
    // Init, where the card takes the rays' shader model: the runs on rays - the water's, the sludge's mirror layer,
    // the glass's mirror layer, the splashes - get pipelines that trace nothing themselves, those that write down
    // each pixel's rays and those that read the host's answers for them, beside the rays' root signature, from the
    // DXIL the build made (SolidPassRt*.h). Refused by the driver, or not asked for: no run is drawn on rays, and
    // Reflections=RT runs as High. HasHostedGlass: the glass's two made, for every sample count this pass draws the
    // world with.
    void  SetRtHosted(UBOOL bOn) { bRtHosted = bOn ? 1 : 0; }
    // Where the rays' list's views lie in the texture heap, before Init - the rays' root signature names them by how
    // far each lies from a view it has a table at already: the answers (and after them what the rays passed) from
    // the masks' view, the pixels' heads (and after them the requests, as a pixel shader writes them) from the
    // water's own depth's view.
    void  SetRayListViews(INT AnswersAfterMasks, INT HeadsAfterSurface)
    {
        RayAnswersAfterMasks = AnswersAfterMasks;
        RayHeadsAfterSurface = HeadsAfterSurface;
    }
    // Where the view of what the shadows' apply pass needs of the water's pixels lies (u6), as far after the water's own
    // depth's view - before Init.
    void  SetWaterShadeView(INT AfterSurface) { WaterShadeAfterSurface = AfterSurface; }
    UBOOL HasHostedGlass() const
    {
        return bRtHosted && RtGlassMirrorPipelines[0].Get() != nullptr && RtGlassAskPipelines[0].Get() != nullptr &&
               (MsaaSamples <= 1 ||
                (RtGlassMirrorPipelines[1].Get() != nullptr && RtGlassAskPipelines[1].Get() != nullptr));
    }
    // The colour formats this pass writes (FRdxColours, from the HDR key). Set before Init and before any
    // RebuildPipelines: a pipeline carries its render target's format, so the two must not drift apart.
    void  SetColours(const FRdxColours& C) { Colours = C; }
    UBOOL Init(ID3D12Device* Device, UBOOL bStencilKinds);
    void  Shutdown();

    // FrameIndex: the frame slot this frame's vertices go to - see Dest.
    void Begin(INT Width, INT Height, INT FrameIndex);

    // SrvIndex < 0 means untextured; the pass substitutes a white texture so the
    // shader needs no branch.
    // LightSrv < 0 means "no light map": a 1x1 white texture is bound instead, so
    // the shader multiplies by one and needs no branch.
    // bNoSmooth: sample the texture unfiltered (PF_NoSmooth).
    void AddFan(const FSolidVertex* Verts, INT Count, INT SrvIndex, INT LightSrv, EBlendMode Mode,
                UBOOL bNoSmooth);

    // One triangle, written by the caller: reserves three vertices in this frame's buffer, opens
    // or extends the run, and returns where they go - the caller fills all three, in order, and
    // must not read them back (the upload heap is write-combined). Null if there is no room.
    // Meshes arrive one triangle a draw call - measured: 8500 a frame in Liberty - and this way
    // their vertices are built straight into the buffer instead of a local fan AddFan copies.
    FSolidVertex* BeginTriangle(INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth);
    // Count triangles at once, the same way: one run opened or extended, and room for 3 * Count vertices.
    FSolidVertex* BeginTriangles(INT Count, INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth);

    // Switch performs the target-switch marker; null (1x) draws the whole frame into the one target.
    void Flush(ID3D12GraphicsCommandList* Cmd, INT FrameIndex, FTextureCache& Textures,
               D3D12_CPU_DESCRIPTOR_HANDLE Dsv, FTargetSwitch* Switch);

    // The engine clears depth partway through a frame - after the sky box, before the
    // world. Measured: the sky is at view depths of 1250..2860 units while the world
    // behind it sits at 3300..10000, so without the clear the sky is NEARER than the
    // scene and rejects it. Recorded here so it happens in the right place.
    void AddDepthClear();
    // A mirror's view of the level has drawn its surfaces: the owner's FTargetSwitch::MirrorViewEnd is called here
    // with the number. A marker, as the clear is.
    void AddMirrorEnd(INT Index);

    // The engine's screen tint, at the point it asked for it: dst * Scale + Fog over the
    // whole target (EndFlash).
    void AddFlash(FLOAT ScaleR, FLOAT ScaleG, FLOAT ScaleB, FLOAT FogR, FLOAT FogG, FLOAT FogB);

    // Supersampling: the point where the frame moves to the output-sized target (FTargetSwitch) - the
    // HUD's first tile - recorded in draw order like the depth clear. False if the run list is full.
    UBOOL AddTargetSwitch();

    // With a readable depth (the shadows, the depth view): the frame's world begins with the next geometry - recorded in draw
    // order like the depth clear, so the owner learns which depth buffer holds the world (FTargetSwitch::MarkWorldDepth).
    UBOOL AddWorldStart();

    // Where the world ends when no target switch marks it (1x without the shadows): the HUD's first tile - recorded in
    // draw order like the depth clear, only for the water's splashes to be laid on there (FTargetSwitch::DrawSplashes).
    UBOOL AddWorldEnd();

    // With stencil kinds: what the next geometry is - 0 the level's BSP, 1 an actor (mesh, sprite, tile). Written
    // into the stencil where the draw passes the depth test, so the finished buffer tells, per pixel, what kind of
    // surface is nearest. Translucent and modulated draws write no stencil at all (their pipelines use a zero
    // stencil write mask), so what shows through them keeps the kind of the surface behind.
    void SetKind(INT Kind) { CurrentKind = Kind; }
    // Whether the next geometry is a sprite - a picture standing in the air, as the game's smoke, steam and sparks are
    // drawn - or a mesh's own polygons. Only a sprite fades as it nears what is behind it (the soft particles): a
    // see-through mesh lies where it is meant to - the laser sight's spot ON its wall, a ring ON the water.
    void SetSprite(UBOOL bOn) { CurrentSprite = bOn ? 1 : 0; }
    // Which of the frame's flashlight blocks the next geometry reads (SetBeam), and so how much of the flashlight's
    // light and of the own lamps' it takes: 0 none - the sky, what is drawn once the world is, what no light lights;
    // then two for each view, all of it - its own lit surfaces - and a see-through surface's share
    // (RdxMath::BeamSeeThrough): 1 and 2 the world's view's, 3 and 4 the frame's first mirror's view of the level, and
    // so on, two a view in the order the mirrors' views ended (FBackend::SetSurfaceBeam). A mirror's view takes the
    // cone alone, by the view's own numbers - the light off the mirrors is the world's view's. Runs that differ in
    // it are not merged.
    void SetBeamLit(INT How) { CurrentBeamLit = (How >= 1 && How <= BEAM_KINDS) ? How : 0; }
    // The frame slot's blocks: the flashlight's cone (RdxMath::BeamBlock's sixteen numbers) - Block for the runs that
    // take all of it, Thin for the see-through ones - or null for a frame without it. Mirrors: the level's mirrors
    // that give the cone back to the world's view's runs (FBeamBounce), eight numbers each - the mirror's plane in
    // the view's space, the normal away from the eye; how much of the light it gives back; its number in the map -
    // and how many, BEAM_MIRRORS at the most. Own: the own lamps' rows (OWN_FLOATS numbers, SolidPass.hlsl's
    // OwnRow) or null for a frame without them; Views: BEAM_VIEWS views' places in the level, BEAM_VIEW_FLOATS
    // numbers each - OwnView's four rows (the share in the first row's fourth is the block's own) and OwnScreen -,
    // a view whose third row's fourth is nought taking no lamp. With neither the cone nor a lamp every run reads the
    // block of zeros.
    void SetBeam(INT FrameIndex, const FLOAT* Block, const FLOAT* Thin, const FLOAT* Mirrors = nullptr,
                 INT MirrorCount = 0, const FLOAT* Own = nullptr, const FLOAT* Views = nullptr);
    // Where the own lamps' two views (their table and their maps, side by side) lie in the texture heap, before Init:
    // how far after the masks' view - the rays' root signature's table - and after the mirrors' map's view - the
    // pass's own - the first of them is.
    void  SetOwnLampViews(INT AfterMasks, INT AfterBounce)
    {
        OwnAfterMasks  = AfterMasks;
        OwnAfterBounce = AfterBounce;
    }
    // The mirrors' map's view in the texture heap (FBeamBounce's, or a view of nothing): every run of the pass's own
    // root signature binds it.
    void SetBounceView(INT Srv) { BounceView = Srv; }
    // Whether the panes of glass can be drawn into the mirrors' map: their pipeline on rays is there.
    UBOOL CanKeepPanes() const { return RtPaneKeepPipe() != nullptr; }
    enum { BEAM_MIRRORS = RDX_BEAM_MIRRORS };
    static_assert(BEAM_MIRRORS == 13, "SolidPass.hlsl declares the beam's mirrors by this number as well");
    // The views a frame's blocks are made for - the world's and as many mirrors' as give the cone back -, two blocks
    // each; and the own lamps' numbers in a block: a view's place (OwnView, OwnScreen) and the lamps' rows.
    enum { BEAM_VIEWS = 1 + BEAM_MIRRORS, BEAM_KINDS = 2 * BEAM_VIEWS, BEAM_VIEW_FLOATS = 20,
           OWN_FLOATS = 4 * (RDX_OWN_HEAD_ROWS + RDX_OWN_LAMP_ROWS * RDX_OWN_LAMPS) };
    static_assert(RDX_OWN_LAMPS == RdxMath::OwnLampMost && RDX_OWN_LAMPS == 9 && RDX_OWN_HEAD_ROWS == 4 &&
                  RDX_OWN_LAMP_ROWS == 7, "SolidPass.hlsl declares the own lamps' rows by these numbers as well");

    // The constants of the water runs that follow (BLEND_Water): the water's hue (lit by the surface's own light) and
    // the waves' strength, then the time in seconds, the engine's projection in its own pixels, the absorption per
    // world unit and the shoreline's width in world units; then what puts a pixel back into the world, for the ripples:
    // the camera's place and the view's three axes (each its world X and Y), the screen's centre in the engine's
    // pixels, and the ripples' field's lower corner (RdxMath::RippleOrigin); then the camera's height above the surface
    // (negative below it) and the Z of the view's three axes; then the texture's own waves: whether this run has them
    // (1) or the computed ones stand in (0), how tall they are, one texture repeat in world units (U, V), and the
    // texture's U and V directions on the level (X, Y each); then 1 for the mirror image too (Reflections=High and up),
    // 1 for the sludge (lifted by its waves, never lowered - DSWater), one spare, and the pass's own
    // (WATER_SURFACE_CONSTANT). Runs with different constants are not merged.
    enum { WATER_CONSTANTS = 36 };
    // The last of them is this pass's own, never RenderDevice's: 1 where the water's own depth is bound for the water to
    // write (FTargetSwitch::WaterSurfaceView), set as each water run is drawn.
    enum { WATER_SURFACE_CONSTANT = 35 };
    // The root signatures' parameter for that view (u3): after the pass's own seven, after the RT water's thirteen.
    enum { ROOT_WATER_UAV = 7, ROOT_WATER_UAV_RT = 13 };
    // And the pass's own root signature's parameter for the flashlight's block (b4). The RT water's has none, and no
    // room for one: the water's runs on rays read no beam.
    enum { ROOT_BEAM = 8 };
    // And for the mirrors' map (t15), a table as its view sits in the texture heap: the pass's own root signature's.
    enum { ROOT_BOUNCE = 9 };
    void SetWaterConstants(const FLOAT* C) { for (INT i = 0; i < WATER_CONSTANTS; i++) WaterNow[i] = C[i]; }
    // What follows is seen through a mirror whose image is traced: the engine's own drawing of the see-through things
    // in the mirror - a flame, a sprite, a pane - which the rays' scene does not hold. It comes before the world and
    // is drawn after the mirrors' traced images, over them (Flush's order).
    void SetInMirror(UBOOL bOn) { bInMirrorNow = bOn ? 1 : 0; }
    // The relief for the runs that follow, until it is cleared: where the height map sits in the texture heap, how
    // deep it goes in the level's units, what is switched on, and one over its size. Table below zero means none.
    void SetPomRun(INT Table, FLOAT Depth, FLOAT Flags, FLOAT InvW, FLOAT InvH)
    {
        PomTableNow = Table;
        PomNow[0] = Depth; PomNow[1] = Flags; PomNow[2] = InvW; PomNow[3] = InvH;
    }
    void ClearPomRun() { PomTableNow = -1; PomNow[0] = PomNow[1] = PomNow[2] = PomNow[3] = 0.0f; }
    // The relief's own numbers for the runs that follow, until cleared - both ways of drawing it. They ride in the
    // water's constants, as no surface is both: the normal out of the surface towards the eye, the texture's U and V
    // directions along it (all view space, unit), how many texture repeats one unit of the level holds along U and V,
    // how long a cut edge may be (engine pixels), and the engine's projection (its focal length and the screen's
    // centre, in its pixels). The shader's names: ReliefNormal, ReliefAxisU, ReliefAxisV, ReliefRate, ReliefEdge,
    // ReliefFocal, ReliefCentre; and how much of the baked light the lamps reshape (0..1, ReliefLampStrength). SetPomRun
    // still says which map, how deep and what else is on.
    //
    // bPatches - Geometry=Tessellation: the runs are the relief made real, patches of three whose vertices carry their
    // place in VIEW space instead of on the screen, cut on the GPU and lifted out of the wall (VSRelief, HSRelief,
    // DSRelief). Without it the numbers serve the POM's pixel shader (the lamps, POM_LAMPS).
    void SetReliefRun(UBOOL bPatches, const FLOAT N[3], const FLOAT AxisU[3], const FLOAT AxisV[3], FLOAT RateU,
                      FLOAT RateV, FLOAT Edge, FLOAT Focal, FLOAT CentreX, FLOAT CentreY, FLOAT LampStrength)
    {
        for (INT i = 0; i < WATER_CONSTANTS; i++)
            ReliefNow[i] = 0.0f;
        ReliefNow[24] = LampStrength;                                               // WaterWave.x
        ReliefNow[0]  = RateU;    ReliefNow[1]  = RateV;                            // WaterTint.xy
        ReliefNow[4]  = Edge;     ReliefNow[5]  = Focal;                            // WaterView.xy
        ReliefNow[8]  = 1.0f;     ReliefNow[9]  = 1.0f;                             // WaterCamera: see SetTessUvMap
        for (INT k = 0; k < 3; k++)
        {
            ReliefNow[12 + k] = AxisU[k];                                           // WaterAxes.xyz
            ReliefNow[20 + k] = AxisV[k];                                           // WaterExtra.xyz
            ReliefNow[28 + k] = N[k];                                               // WaterDirs.xyz
        }
        ReliefNow[16] = CentreX;  ReliefNow[17] = CentreY;                          // WaterScreen.xy
        bReliefNow = 1;
        bTessNow   = bPatches ? 1 : 0;
    }
    // The texture coordinate the patches' pixels get, from the one they carry (which the height map is read with):
    // U * ScaleU + OffsetU, and so for V. One to one unless set - the detail texture's pass over the same patches
    // sets its own (AddRepeatRun), and the height it lifts them by is still read where the surface's own texture is.
    void SetTessUvMap(FLOAT ScaleU, FLOAT ScaleV, FLOAT OffsetU, FLOAT OffsetV)
    {
        ReliefNow[8] = ScaleU; ReliefNow[9] = ScaleV; ReliefNow[10] = OffsetU; ReliefNow[11] = OffsetV;   // WaterCamera
    }
    // A relief run in a mirror's view: the mirror's plane in the view's space - a unit normal, and how far along it
    // the plane lies - by which the lamps are shown where the mirror shows them (the shader's ReliefLampPlace). For
    // the runs that follow until the next SetReliefRun.
    void SetReliefMirror(const FLOAT* Plane)
    {
        ReliefNow[25] = Plane[0]; ReliefNow[26] = Plane[1]; ReliefNow[27] = Plane[2];   // WaterWave.yzw
        ReliefNow[6]  = Plane[3];                                                        // WaterView.z
    }
    void ClearReliefRun() { bReliefNow = 0; bTessNow = 0; ReliefBlockNow = 0; }
    // Whether the world's tessellated runs are drawn together (Flush); on unless a measurement turns it off.
    void SetTessTogether(UBOOL bOn) { bTessTogether = bOn ? 1 : 0; }
    // Whether the world's blended runs go after the runs that write depth (Flush); on unless a measurement turns it off.
    void SetBlendedLast(UBOOL bOn) { bBlendedLast = bOn ? 1 : 0; }
    // How wide the rim band is, in the relief's depths (the shader's ReliefBevel), for the runs that follow until the
    // next SetReliefRun, which leaves it to the shader's own (RELIEF_BEVEL).
    void SetReliefBevel(FLOAT Depths) { ReliefNow[2] = Depths; }                  // WaterTint.z
    // How many mip levels coarser the height is read that the patches are lifted by (the shader's ReliefSmooth): the
    // height map's fine grain smoothed out of the shape, the shading keeping it. Zero until set, after each SetReliefRun.
    void SetReliefSmooth(FLOAT Levels) { ReliefNow[3] = Levels; }                 // WaterTint.w
    // Geometry=Tessellation: the held edges near the relief's polygons - Count of them, at most RELIEF_HELD_MAX, each
    // twelve floats: its two ends in view space with the share of the band it keeps after the first, and its shear (the
    // relief's top's move across it per unit lifted, where it goes up to a wall) - for the runs that follow until
    // ClearReliefRun: DSRelief measures its rim band from them at every point it cuts. They go in a block of this frame's together with the relief's lamps
    // (Lamps: LampFloats of them, laid out as FBackend::SetReliefLamps lays the frame's), as the two share the lamps'
    // register and the root signature has no room for another. Returns the block, for a run drawing the same patches
    // again (SetReliefBlock); zero when there are none, too many, or no room left this frame - the patches' corners then
    // carry the band.
    enum { RELIEF_HELD_MAX = 64 };
    D3D12_GPU_VIRTUAL_ADDRESS SetReliefHeld(const FLOAT* Lamps, INT LampFloats, const FLOAT* Ends, INT Count);
    void SetReliefBlock(D3D12_GPU_VIRTUAL_ADDRESS Block) { ReliefBlockNow = Block; }
    // The vertices written so far this frame, and a run that draws a stretch of them AGAIN with the state in force now -
    // the detail texture over a surface's relief patches, which must land on exactly the same lifted surface. Such a
    // run is never extended by the geometry after it. False if the draw list is full or the stretch is not there.
    INT   GetVertexCount() const { return VertexCount; }
    UBOOL AddRepeatRun(INT Start, INT Count, INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth);
    // Grass3D (GrassShader.h, docs/terv-novenyzet-szel.md): the frame's room for the grass's blocks - one a draw: the
    // constants and the tiles the blades' vertex shader reads, Rows float4s, at most RdxMath::GrassBlockRows - and the
    // draws themselves, each a run of its own right after its ground's: drawn where the ground is, in whatever view
    // that is. GrassBlock gives the block to fill and its GPU address, or null when the frame's room is used up.
    // AddGrassDraw takes the ground's texture, light map and filtering, and the stencil kind and the flashlight's share
    // now in force - the ground's, drawn just before.
    FLOAT* GrassBlock(INT Rows, D3D12_GPU_VIRTUAL_ADDRESS& Address);
    // bWorldView (here, in AddTreeDraw and AddLeafDraw): the world's own view, whose plants the motion vectors' pass
    // lays again (DrawPlantMotion) - not a mirror's.
    void   AddGrassDraw(D3D12_GPU_VIRTUAL_ADDRESS Block, INT Instances, INT SrvIndex, INT LightSrv, UBOOL bNoSmooth,
                        UBOOL bWorldView);
    UBOOL  CanGrass() const { return GrassPipelines[0].Get() && (MsaaSamples <= 1 || GrassPipelines[1].Get()); }
    INT    GrassDraws() const { return GrassRuns; }
    // Trees3D (TreeShader.h, TreeGen.h): a grown plant's bark or its leaves, a run of their own where the game's plant
    // is drawn - its vertices and indices (one of FBackend's tree meshes), the block of its place, size and light (in
    // GrassBlock's room), its picture, and the stencil kind and flashlight's share now in force.
    void   AddTreeDraw(const D3D12_VERTEX_BUFFER_VIEW& Vb, const D3D12_INDEX_BUFFER_VIEW& Ib, INT IndexStart,
                       INT IndexCount, D3D12_GPU_VIRTUAL_ADDRESS Block, INT SrvIndex, UBOOL bWorldView);
    UBOOL  CanTrees() const
    {
        return TreePipelines[0].Get() && LeafPipelines[0].Get() &&
               (MsaaSamples <= 1 || (TreePipelines[1].Get() && LeafPipelines[1].Get()));
    }
    // Trees3D: a grown plant's leaves - Count instances of the blade's frame (Ib: FrameIndices of its indices from
    // FrameFirst on - TreeGen.h's LeafFrame, the whole frame or the flat blade) from First on in the instance buffer Vb -
    // drawn by the leaves' vertex shader, its block in GrassBlock's room; and in the motion vectors' pass by the frame
    // from MotionFirst on, MotionIndices of it - the flat blade, its depth taken nearer (TreeShader.h's VSLeafMotion).
    void   AddLeafDraw(const D3D12_VERTEX_BUFFER_VIEW& Vb, const D3D12_INDEX_BUFFER_VIEW& Ib, INT FrameFirst,
                       INT FrameIndices, INT MotionFirst, INT MotionIndices, INT First, INT Count,
                       D3D12_GPU_VIRTUAL_ADDRESS Block, INT SrvIndex, UBOOL bWorldView);
    // The plants' own motion vectors (PlantMotion.h): the world view's grass, bark and leaves of this frame laid again
    // in the motion vectors' pass, now and the frame before, against the scene's depth (DsvFormat), with the two
    // cameras' 48 constants the bodies' pass is given (FFinalPass::DrawMotionObject). How many such draws the frame has;
    // DrawPlantMotion lays those of one kind - 0 the grass, 1 the bark, 2 the leaves - so each can be timed.
    INT    PlantMotionCount() const { return PlantMotionUsed; }
    void   DrawPlantMotion(ID3D12Device* Device, ID3D12GraphicsCommandList* Cmd, FTextureCache& Textures,
                           DXGI_FORMAT DsvFormat, const FLOAT* Constants, INT Kind);
    // Wind (DustShader.h): the motes of dust in the air, a see-through run of their own - Instances of them, their
    // block in GrassBlock's room, the mote's picture.
    void   AddDustDraw(D3D12_GPU_VIRTUAL_ADDRESS Block, INT Instances, INT SrvIndex);
    UBOOL  CanDust() const { return DustPipelines[0].Get() && (MsaaSamples <= 1 || DustPipelines[1].Get()); }
    INT    TreeDraws() const { return TreeDrawCount; }
    // Whether the tessellated runs can be drawn at all: their pipelines were made for both sample counts.
    UBOOL CanTessellate() const { return TessPipelines[0].Get() && (MsaaSamples <= 1 || TessPipelines[1].Get()); }
    // And the detail texture's pass over them (modulated).
    UBOOL CanTessellateDetail() const
    { return TessModPipelines[0].Get() && (MsaaSamples <= 1 || TessModPipelines[1].Get()); }
    // And the fog map's pass over them (BLEND_Fog).
    UBOOL CanTessellateFog() const
    { return TessFogPipelines[0].Get() && (MsaaSamples <= 1 || TessFogPipelines[1].Get()); }
    // Where the water runs' four views start in the texture heap (the picture, the depth, the ripples, the texture's own
    // wave field): the backend keeps two such sets and moves between them as the ripples' field flips (StepRipples).
    void SetWaterTable(INT FirstView) { WaterTable = FirstView; }
    // Whether those views have all been written this frame (FBackend::EnsureWaterTargets) - the heap's reserved slots hold
    // nothing until then. The sludge reads two of them without any copy being made, so it asks.
    void SetWaterTableReady(UBOOL bReady) { bWaterTableReady = bReady ? 1 : 0; }
    // Whether an opaque water surface can be drawn lifted this frame (BLEND_Sludge, with its fog map's BLEND_SludgeFog):
    // their pipelines for every sample count in use, the water's views and the lamps. Asked before the surface is handed
    // over; without it the sludge is drawn flat, as it always was.
    UBOOL CanSludge() const
    {
        for (INT Set = 0; Set < ((MsaaSamples > 1) ? 2 : 1); Set++)
            if (!Pipelines[Set][BLEND_Sludge].Get() || !Pipelines[Set][BLEND_SludgeFog].Get())
                return 0;
        return (bWaterTableReady && WaterLamps && WaterTable >= 0) ? 1 : 0;
    }
    // Whether the sludge's mirror layer (BLEND_SludgeMirror) can be drawn: its pipelines for every sample count in use, and
    // the sludge itself (CanSludge). Asked before the surface is handed over - the surface leaves its sheen to the layer
    // only where there will be one.
    UBOOL CanSludgeMirror() const
    {
        for (INT Set = 0; Set < ((MsaaSamples > 1) ? 2 : 1); Set++)
            if (!Pipelines[Set][BLEND_SludgeMirror].Get())
                return 0;
        return CanSludge();
    }
    // Whether a mirror's whole image can be traced - a BLEND_GlassMirror run that gives all of it back, on rays: the
    // glass layer's pipelines, the traced ones among them, for every sample count in use.
    UBOOL CanRtMirror() const
    {
        for (INT Set = 0; Set < ((MsaaSamples > 1) ? 2 : 1); Set++)
            if (!Pipelines[Set][BLEND_GlassMirror].Get() || !RtGlassMirrorPipe(Set))
                return 0;
        return 1;
    }
    // Whether the glass's mirror layer (BLEND_GlassMirror) can be drawn: its pipelines for every sample count in use.
    // Asked before a pane's layer is handed over; the copy of the picture it reads is made when the layer is drawn,
    // and without one the layer is left out there.
    UBOOL CanGlassMirror() const
    {
        for (INT Set = 0; Set < ((MsaaSamples > 1) ? 2 : 1); Set++)
            if (!Pipelines[Set][BLEND_GlassMirror].Get())
                return 0;
        return 1;
    }
    // Whether an effect's body (BLEND_Body) can be drawn: its pipelines for every sample count in use.
    UBOOL CanBody() const
    {
        for (INT Set = 0; Set < ((MsaaSamples > 1) ? 2 : 1); Set++)
            if (!Pipelines[Set][BLEND_Body].Get())
                return 0;
        return 1;
    }
    // Soft particles: how far in front of the wall a translucent surface fades out, in the level's units. Zero is
    // off, and then nothing about the frame changes.
    void SetSoftFade(FLOAT Units) { SoftFade = (Units > 0.0f) ? Units : 0.0f; }
    FLOAT GetSoftFade() const { return SoftFade; }
    // Whether smoke and gas are drawn as volumes (Volumetrics). Asked before the frame's draws, because it decides
    // whether the copy of the scene's depth is made at all - and without that copy a cloud has nothing to stop its
    // walk at a wall, so it falls back to the flat picture.
    void  SetCloudsOn(UBOOL bOn) { bClouds = bOn ? 1 : 0; }
    UBOOL GetCloudsOn() const { return bClouds; }
    // Where this frame's water lamps are (FBackend::SetWaterLamps), for the water's pixel shader - zero without them, and
    // then the water is drawn the plain translucent way.
    void SetWaterLamps(D3D12_GPU_VIRTUAL_ADDRESS Address) { WaterLamps = Address; }
    // And the lamps the relief is lit by (FBackend::SetReliefLamps), bound for the relief's runs - zero without them.
    void SetReliefLamps(D3D12_GPU_VIRTUAL_ADDRESS Address) { ReliefLamps = Address; }
    // The splashes' smoothed depth and thickness in the texture heap (FBackend::DrawSplashes), for PSSplash.
    void SetSplashViews(INT Smooth, INT Thick) { SplashSmoothView = Smooth; SplashThickView = Thick; }
    // The relief's covers in the texture heap (FBackend's ReliefCover): the covers' own view, and the view of nothing the
    // relief reads in a frame without them. The relief is not raised at all while there is no view of nothing to bind.
    void SetCoverViews(INT Made, INT None) { CoverView = Made; CoverNone = None; }
    // A measurement only (FBackend::SetReliefCoverModulated): the level's modulated surfaces left out of the covers.
    void SetCoverModulated(UBOOL bOn) { bCoverModulated = bOn ? 1 : 0; }

    INT GetTriangleCount() const { return VertexCount / 3; }
    INT GetDrawCount()     const { return RunCount; }
    UBOOL IsReady() const { return RootSignature.Get() != nullptr; }

private:
    // Extends the last run when the new geometry belongs to it, opens a new one otherwise.
    // False if the draw list is full (and says so, once).
    UBOOL EnsureRun(INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth);

    UBOOL CreateRootSignature(ID3D12Device* Device);
    // The pass's root signature into Out; with bRt the RT water's - the same parameters, and the rays' after them.
    UBOOL MakeRoot(ID3D12Device* Device, UBOOL bRt, TComRef<ID3D12RootSignature>& Out);
    UBOOL CreatePipelineState(ID3D12Device* Device);
    // The RT water's pipelines from the water's own description (Desc: its blend, targets and depth), with the DXIL stages
    // and the RT root signature. Refused: logged, and the RT water runs as High.
    void  MakeRtWater(ID3D12Device* Device, D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc);
    void  MakeWaterMark(ID3D12Device* Device, D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc, ID3DBlob* Ps);
    // The same for the sludge's mirror layer (BLEND_SludgeMirror), from the layer's own description. Refused: logged, and
    // the layer is drawn as on High.
    void  MakeRtSludgeMirror(ID3D12Device* Device, D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc);
    // And for the glass's mirror layer (BLEND_GlassMirror). Refused: logged, and the layer mirrors the picture, as on
    // High.
    void  MakeRtGlassMirror(ID3D12Device* Device, D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc);
    // A run's two pipelines for the 64-bit host's rays, from the run's own description on the rays' root signature
    // (Layer, its stages before the pixel shader set): the build that reads the host's answers, drawn as the run
    // is drawn, and the build that only writes the run's rays down - no target, nothing blended, the depth tested
    // as the drawing tests it and not written. A set each as the run's own. Both or neither: refused, the run
    // draws through the host as it does without rays.
    void  MakeHostPair(ID3D12Device* Device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Layer, const void* HostPs,
                       SIZE_T HostBytes, const void* AskPs, SIZE_T AskBytes, TComRef<ID3D12PipelineState>* Host,
                       TComRef<ID3D12PipelineState>* Ask, const TCHAR* What);
    UBOOL CreateVertexBuffers(ID3D12Device* Device);

    enum { MAX_VERTICES = 400000, MAX_RUNS = 8192 };

    // One draw call: a stretch of vertices sharing a texture.
    struct FRun
    {
        INT        Start;
        INT        Count;
        INT        SrvIndex;
        INT        LightSrv;
        EBlendMode Mode;
        // A depth clear is not a state change we can record when the engine asks for it:
        // every draw of the frame is batched and submitted at the end, so a clear recorded
        // mid-frame would land before all of them and do nothing. It has to travel WITH
        // the geometry, as a marker between runs.
        UBOOL      bClearDepthFirst;
        UBOOL      bNoSmooth;       // PF_NoSmooth: point sampling for this run
        UBOOL      bFlash;          // a screen-tint marker (EndFlash), not geometry
        UBOOL      bTargetSwitch;   // the move to the output-sized target (AddTargetSwitch), not geometry
        UBOOL      bWorldStart;     // the world's first geometry follows (AddWorldStart), not geometry
        UBOOL      bWorldEnd;       // the world's last geometry went before (AddWorldEnd), not geometry
        INT        Kind;            // SetKind's surface kind, for the stencil (geometry runs only)
        UBOOL      bSprite;         // SetSprite's: a sprite's picture, not a mesh's polygons (geometry runs only)
        INT        Beam;            // SetBeamLit's: which of the flashlight's blocks, 0 none (geometry runs only)
        FLOAT      Flash[6];        // its scale (RGB) and fog (RGB)
        FLOAT      Water[WATER_CONSTANTS];   // a water run's constants (SetWaterConstants)
        // The relief (POM). A run either has height maps or it has not; the four numbers ride in the constants the
        // water would otherwise use - no surface is both - so this costs the root signature nothing, and it has
        // nothing to spare (see MakeRootSignature).
        UBOOL      bPom;
        INT        PomTable;                 // where this texture's height map and colour sit in the texture heap
        FLOAT      Pom[4];                   // depth in the level's units, what is switched on, and one over the size
        // The relief's own numbers (SetReliefRun) are in Water; and with bTess the run is patches, drawn with
        // TessPipelines (TessModPipelines when modulated).
        UBOOL      bRelief;
        UBOOL      bTess;
        UBOOL      bRepeat;                  // draws vertices another run drew (AddRepeatRun): never extended
        UBOOL      bInMirror;                // SetInMirror's: seen through a traced mirror, drawn over its image
        D3D12_GPU_VIRTUAL_ADDRESS ReliefBlock;   // its held edges and lamps (SetReliefHeld); zero: the frame's lamps
        // A grass run (AddGrassDraw): no vertices of its own - this many blades, made by the grass's vertex shader
        // from the block at GrassBlock. Zero for every other run.
        INT        GrassInstances;
        D3D12_GPU_VIRTUAL_ADDRESS GrassBlock;
        // A plant's run (AddTreeDraw): its place in TreeDrawList and one more; zero for every other run.
        INT        Tree;
        // The dust's run (AddDustDraw): GrassInstances motes from the block at GrassBlock, see-through.
        UBOOL      bDust;
    };

    TComRef<ID3D12RootSignature> RootSignature;
    // One pipeline per blend mode. They differ only in blend and depth-write
    // state, so they share both shaders.
    // Two sets of the same pipelines: [0] one sample, [1] the MSAA count. A pipeline's sample count has to match the
    // targets bound under it, and this pass draws BOTH the multisampled world and, after the switch, the
    // single-sample HUD - so it needs both. With MSAA off, set [1] is never built.
    enum { PIPE_SETS = 2 };
    TComRef<ID3D12PipelineState> Pipelines[PIPE_SETS][BLEND_MAX];
    TComRef<ID3D12PipelineState> FlashPipeline[PIPE_SETS];   // the screen tint (EndFlash)
    TComRef<ID3D12PipelineState> SplashPipeline[PIPE_SETS];  // the water's splashes laid on (PSSplash)
    // Reflections=RT (SetRtHosted): the rays' root signature, and the glass's mirror layer as it reads the host's
    // answers (MakeRtGlassMirror), a set each as above.
    TComRef<ID3D12RootSignature> RtWaterRoot;
    TComRef<ID3D12PipelineState> RtGlassMirrorPipelines[PIPE_SETS];
    // The see-through water's first go over a run (MakeWaterMark, PSWaterMark): the water's own pipeline - and the RT
    // water's, each with the very stages its drawing has - that marks the nearest of the surface's points and draws
    // nothing. Without one the run is drawn as it comes: where its waves stand before one another, the later shows.
    TComRef<ID3D12PipelineState> WaterMarkPipelines[PIPE_SETS];
    TComRef<ID3D12PipelineState> RtWaterMarkPipelines[PIPE_SETS];
    // Geometry=Tessellation: the opaque pipeline with the relief's stages (VSRelief, HSRelief, DSRelief), a set each,
    // the modulated one for the detail texture over the same patches, and the fog map's.
    TComRef<ID3D12PipelineState> TessPipelines[PIPE_SETS];
    TComRef<ID3D12PipelineState> TessModPipelines[PIPE_SETS];
    TComRef<ID3D12PipelineState> TessFogPipelines[PIPE_SETS];
    // Grass3D: the opaque pipeline with the blades' vertex shader (MakeGrass) - the same pixel shader, targets, depth,
    // stencil and blend, so a blade is drawn exactly as the ground under it - a set each.
    TComRef<ID3D12PipelineState> GrassPipelines[PIPE_SETS];
    TComRef<ID3DBlob>            GrassVs;
    void MakeGrass(ID3D12Device* Device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc);
    // Trees3D: the opaque pipeline with the plants' vertex shader and their vertices' layout (MakeTree).
    TComRef<ID3D12PipelineState> TreePipelines[PIPE_SETS];
    TComRef<ID3DBlob>            TreeVs;
    // And the leaves': the same with their own vertex shader and a leaf's instance as the input (MakeTree).
    TComRef<ID3D12PipelineState> LeafPipelines[PIPE_SETS];
    TComRef<ID3DBlob>            LeafVs;
    void MakeTree(ID3D12Device* Device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc);
    // Wind: the see-through pipeline with the dust's vertex shader (MakeDust).
    TComRef<ID3D12PipelineState> DustPipelines[PIPE_SETS];
    TComRef<ID3DBlob>            DustVs;
    void MakeDust(ID3D12Device* Device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc);
    // And their covers (DrawReliefCovers): depth alone, single-sample - under MSAA there are none (FBackend refuses them).
    TComRef<ID3D12PipelineState> CoverPipeline;
    INT                          CoverView = -1;       // see SetCoverViews
    INT                          CoverNone = -1;
    UBOOL                        bCoverModulated = 1;  // see SetCoverModulated
    // Draws the covers - the world's runs in the order from From on that can stand in front of a raised surface, flat,
    // depth alone - into the target bound (FTargetSwitch::BeginReliefCovers). Returns how many it drew; Modulated: how
    // many of them were the level's modulated surfaces.
    INT DrawReliefCovers(ID3D12GraphicsCommandList* Cmd, FTextureCache& Textures, INT From, INT NumOrder,
                         INT& Modulated);
    FRdxColours Colours;      // see SetColours: the formats the pipelines are built for
    INT                          MsaaSamples;   // 1, or what the backend's MSAA settled on (SetMsaaSamples)
    UBOOL                        bMsaaLive;     // the world of THIS frame is multisampled (SetMsaaLive)

    FLOAT JitterX = 0.0f;                       // the temporal upscaler's sub-pixel shift, in clip space
    FLOAT JitterY = 0.0f;
    FLOAT LightBoost = 1.0f;                    // the lamps' strength, the compensation's first stage
    FLOAT MipBias = 0.0f;                       // the world's textures' mip bias (SetMipBias)
    UBOOL bModeStats = 0;                       // measurement only: the per-blend-mode breakdown
    UBOOL bDredMarkers = 0;                     // see SetDredMarkers
    UBOOL bAddedTranslucentOff = 0;             // measurement only: translucent draws skip the added target
    UBOOL bWaterMarkOff        = 0;             // measurement only: see SetWaterMarkOff

    ID3D12Resource* VertexBuffers[BACKEND_FRAME_COUNT];
    FSolidVertex*   Mapped[BACKEND_FRAME_COUNT];

    // Where AddFan writes: this frame slot's vertex buffer itself, from Begin to Flush. Straight into the upload
    // heap rather than into ordinary memory copied across at Flush - that copy measured 0.19 ms a frame at 35000-odd
    // vertices. The heap is write-combined, which suits AddFan exactly: written in order, never read back. Null
    // outside a frame, so nothing can write into a buffer the GPU is reading.
    FSolidVertex* Dest;
    INT           VertexCount;

    FRun Runs[MAX_RUNS];
    INT  RunCount;

    INT   Width;
    INT   Height;
    UBOOL bOverflowReported;
    UBOOL bStencilKinds;
    INT   CurrentKind;
    UBOOL CurrentSprite;
    INT   CurrentBeamLit = 0;
    // The flashlight's blocks (SetBeam): one upload buffer, BEAM_KINDS blocks a frame slot - each view's all of the
    // light and a see-through surface's share, the world's with the mirrors that give the cone back - and one more
    // that stays zeros. A block: the cone's sixteen numbers, how many mirrors (and three spare), the mirrors, eight
    // numbers each; from BEAM_OWN_FIRST on the view's place and the own lamps' rows.
    enum { BEAM_BLOCK_BYTES = 2048, BEAM_OWN_FIRST = 128 };
    static_assert((RdxMath::BeamBlockFloats + 4 + BEAM_MIRRORS * 8) <= BEAM_OWN_FIRST &&
                  (BEAM_OWN_FIRST + BEAM_VIEW_FLOATS + OWN_FLOATS) * sizeof(FLOAT) <= BEAM_BLOCK_BYTES,
                  "the beam's numbers fit their block");
    ID3D12Resource* BeamBuffer = nullptr;
    BYTE*           BeamMapped = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS BeamOn[BEAM_KINDS] = {};      // this frame's blocks, or zero for the block of zeros
    INT             BounceView = -1;
    UBOOL                        bRtHosted = 0;         // SetRtHosted: the mirror rays are the 64-bit host's to trace
    // PSGlassMirrorAsk: the layer's rays written down for the host, a set each as the layer's own - the pixels are
    // tested against the depth the world is drawn with.
    TComRef<ID3D12PipelineState> RtGlassAskPipelines[PIPE_SETS];
    INT                          RayAnswersAfterMasks = 0;      // SetRayListViews
    INT                          RayHeadsAfterSurface = 0;
    INT                          WaterShadeAfterSurface = 0;    // SetWaterShadeView
    INT                          OwnAfterMasks  = 0;            // SetOwnLampViews
    INT                          OwnAfterBounce = 0;
    // The water's runs, the sludge's mirror layer and the splashes for the host's rays (MakeHostPair): the builds
    // that read the host's answers, and the builds that write a run's rays down. And the water's first go over a
    // run for the asking, where there is no target.
    TComRef<ID3D12PipelineState> RtHostWaterPipelines[PIPE_SETS];
    TComRef<ID3D12PipelineState> RtWaterAskPipelines[PIPE_SETS];
    TComRef<ID3D12PipelineState> RtWaterAskMarkPipelines[PIPE_SETS];
    TComRef<ID3D12PipelineState> RtHostSludgeMirrorPipelines[PIPE_SETS];
    TComRef<ID3D12PipelineState> RtSludgeMirrorAskPipelines[PIPE_SETS];
    TComRef<ID3D12PipelineState> RtHostSplashPipeline[PIPE_SETS];
    TComRef<ID3D12PipelineState> RtSplashAskPipeline[PIPE_SETS];
    // And the panes' pass into the mirrors' map: reading the host's answers, and writing the panes' rays down.
    TComRef<ID3D12PipelineState> RtHostPaneKeepPipeline;
    TComRef<ID3D12PipelineState> RtPaneKeepAskPipeline;
    // The pipelines on rays as a frame draws with them: each run's build that reads the host's answers - made only
    // where the rays are the host's (SetRtHosted), so none of them is there without it.
    ID3D12PipelineState* RtWaterPipe(INT Set) const        { return RtHostWaterPipelines[Set].Get(); }
    ID3D12PipelineState* RtSludgeMirrorPipe(INT Set) const { return RtHostSludgeMirrorPipelines[Set].Get(); }
    ID3D12PipelineState* RtGlassMirrorPipe(INT Set) const  { return RtGlassMirrorPipelines[Set].Get(); }
    ID3D12PipelineState* RtSplashPipe(INT Set) const       { return RtHostSplashPipeline[Set].Get(); }
    ID3D12PipelineState* RtPaneKeepPipe() const
    {
        return RtPaneKeepAskPipeline ? RtHostPaneKeepPipeline.Get() : nullptr;
    }
    D3D12_GPU_VIRTUAL_ADDRESS BeamOff() const
    {
        return BeamBuffer ? BeamBuffer->GetGPUVirtualAddress() +
                                (UINT64)(BEAM_KINDS * BACKEND_FRAME_COUNT) * BEAM_BLOCK_BYTES
                          : 0;
    }
    FLOAT WaterNow[WATER_CONSTANTS];            // see SetWaterConstants
    INT   PomTableNow = -1;                     // see SetPomRun
    UBOOL bTessTogether = 1;                    // see SetTessTogether
    UBOOL bBlendedLast  = 1;                    // see SetBlendedLast
    INT   RunOrder[MAX_RUNS];                   // the order Flush draws the runs in
    INT   RunWaiting[MAX_RUNS];                 // and the tessellated runs waiting for their place in it
    INT   RunLater[MAX_RUNS];                   // and the blended runs waiting for theirs
    INT   RunInMirror[MAX_RUNS];                // and the runs seen through a traced mirror (SetInMirror)
    UBOOL bInMirrorNow = 0;
    FLOAT PomNow[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    UBOOL bReliefNow = 0;                       // see SetReliefRun
    UBOOL bTessNow   = 0;
    FLOAT ReliefNow[WATER_CONSTANTS];
    D3D12_GPU_VIRTUAL_ADDRESS ReliefLamps = 0;  // see SetReliefLamps
    D3D12_GPU_VIRTUAL_ADDRESS ReliefBlockNow = 0;   // see SetReliefHeld
    // The blocks SetReliefHeld writes: one upload buffer a frame slot, filled from HELD_ZERO on each frame. The first
    // HELD_ZERO bytes stay zero: a relief run with neither its own block nor the frame's lamps binds them, and reads no
    // lamps and no held edges rather than whatever the register held before - so they reach past the shader's lamp
    // rows and the held edges' count: (2 + 2 * RDX_RELIEF_LAMP_MAX) rows of 16 bytes.
    enum { HELD_BYTES = 2 * 1024 * 1024, HELD_ZERO = 1024 };
    static_assert((2 + 2 * RDX_RELIEF_LAMP_MAX) * 16 <= HELD_ZERO, "the zero block must cover the lamps and the count");
    ID3D12Resource* HeldBuffers[BACKEND_FRAME_COUNT] = {};
    BYTE*           HeldMapped[BACKEND_FRAME_COUNT]  = {};
    INT             HeldFrame   = -1;
    INT             HeldUsed    = 0;
    UBOOL           bHeldFullSaid = 0;
    // The grass's blocks (GrassBlock): an upload buffer a frame slot, with a constant buffer's reach of room past its
    // end - a root view reads a block's whole 64 KB, whatever of it was written.
    enum { GRASS_BYTES = 4 * 1024 * 1024, GRASS_TAIL = 64 * 1024 };
    ID3D12Resource* GrassBuffers[BACKEND_FRAME_COUNT] = {};
    BYTE*           GrassMapped[BACKEND_FRAME_COUNT]  = {};
    INT             GrassUsed       = 0;
    UBOOL           bGrassFullSaid  = 0;
    INT             GrassRuns       = 0;        // the frame's grass runs so far
    // The plants' draws this frame (AddTreeDraw), the runs pointing into it.
    enum { TREE_DRAWS = 2048 };
    struct FTreeDraw
    {
        D3D12_VERTEX_BUFFER_VIEW  Vb;
        D3D12_INDEX_BUFFER_VIEW   Ib;
        UINT                      IndexStart, IndexCount;
        UINT                      InstanceStart, InstanceCount;   // the leaves': their instances; the bark: 0, 1
        UBOOL                     bLeaves;
        D3D12_GPU_VIRTUAL_ADDRESS Block;
    };
    FTreeDraw       TreeDrawList[TREE_DRAWS];
    INT             TreeDrawCount   = 0;
    // DrawPlantMotion's count of what each kind lays (grass, bark, leaves), told every 600 frames.
    INT    PlantMotionFrames = 0;
    INT    PlantMotionDraws[3] = { 0, 0, 0 };
    UINT64 PlantMotionLaid[3] = { 0, 0, 0 };
    // The world view's plant draws this frame for the motion vectors' pass (DrawPlantMotion): the grass's (Kind 0), the
    // bark's (1) and the leaves' (2) - their blocks, buffers and counts, and the leaves' picture for their cut-out.
    struct FPlantMotion
    {
        INT                       Kind;
        D3D12_GPU_VIRTUAL_ADDRESS Block;
        D3D12_VERTEX_BUFFER_VIEW  Vb;
        D3D12_INDEX_BUFFER_VIEW   Ib;
        UINT                      Start, Count, InstanceStart, InstanceCount;
        INT                       Srv;
    };
    enum { PLANT_MOTIONS = 4096 };
    FPlantMotion                 PlantMotions[PLANT_MOTIONS];
    INT                          PlantMotionUsed = 0;
    TComRef<ID3DBlob>            PlantMotionVs[3], PlantMotionPs[2];
    TComRef<ID3D12RootSignature> PlantMotionRoot;
    TComRef<ID3D12PipelineState> PlantMotionPso[3];
    DXGI_FORMAT                  PlantMotionFormat = DXGI_FORMAT_UNKNOWN;
    INT                          PlantMotionState  = 0;     // for that format: 0 not tried, 1 made, -1 could not be
    UBOOL EnsurePlantMotion(ID3D12Device* Device, DXGI_FORMAT DsvFormat);
    void  NotePlantMotion(INT Kind, D3D12_GPU_VIRTUAL_ADDRESS Block, const D3D12_VERTEX_BUFFER_VIEW* Vb,
                          const D3D12_INDEX_BUFFER_VIEW* Ib, UINT Start, UINT Count, UINT InstanceStart,
                          UINT InstanceCount, INT Srv);
    INT   WaterTable;                           // see SetWaterTable
    UBOOL bWaterTableReady = 0;                 // see SetWaterTableReady
    FLOAT SoftFade = 0.0f;                      // see SetSoftFade
    UBOOL bClouds  = 0;                         // see SetCloudsOn
    D3D12_GPU_VIRTUAL_ADDRESS WaterLamps;       // see SetWaterLamps
    INT   SplashSmoothView;                     // see SetSplashViews
    INT   SplashThickView;
};

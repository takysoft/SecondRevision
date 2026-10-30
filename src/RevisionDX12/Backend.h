// SecondRevision - the Direct3D 12 side, kept apart from the engine interface.
//
// Decision D3 of the project dossier: the API-specific code lives in one thin,
// isolated layer so the backend choice stays cheap to revisit. Nothing in this file
// knows what a UE1 surface is; nothing in RenderDevice.cpp calls D3D12 directly.
#pragma once

// Three frame slots (command allocators, per-frame upload buffers) and three swap-chain buffers.
//
// Two is enough to keep the GPU fed only while nothing stalls. With two and shadows on, every second Present can
// end up waiting for its own frame to finish on the GPU: the GPU then sits idle while the CPU builds the next one,
// and throughput roughly halves. A third slot gives the CPU somewhere to work while the GPU drains, and the picture
// also reaches the screen sooner. It does not always trigger, and without shadows it makes no difference.
// RequestSwapBuffers can still ask for two.
enum { BACKEND_FRAME_COUNT = 3 };

#include "SolidPass.h"
#include "TextureCache.h"
#include "PomMaps.h"
#include "FinalPass.h"
#include "ShadowMaps.h"
#include "RtPass.h"
#include "Fsr3Pass.h"
#include "HostUpscale.h"
#include "HostRays.h"
#include "FramePacer.h"
#include "ReflexLink.h"
#include "Volumetrics.h"
#include "MotionBlur.h"
#include "DepthOfField.h"
#include "FilmLook.h"
#include "Bloom.h"
#include "FogRooms.h"
#include "BeamBounce.h"
#include "FieldVolume.h"
#include "HoloShell.h"
#include "MirrorProbe.h"
#include "MirrorBody.h"
#include "WetReflect.h"
#include "RaySparse.h"

class FBackend : private FTargetSwitch, private FCardWriter
{
public:
    FBackend();
    ~FBackend();

    // Brings up device, swap chain and per-frame command machinery. Returns false and
    // logs on failure - the engine falls back to another render device in that case.
    UBOOL Init(HWND Window, INT Width, INT Height);
    void  Shutdown();
    // Where Init's own shader compiles are waited for: the render device's, which answers the window's messages until
    // Done is signalled, without the engine - so Windows sees the game answering while the start compiles. Set before
    // Init; none, and the compiles run on the thread that makes the backend, the window answering nothing meanwhile.
    void (*AnswerWhile)(HWND Window, HANDLE Done) = nullptr;

    // Recreates the swap chain buffers for a new size. Safe to call with unchanged
    // dimensions; it will simply flush and rebuild.
    UBOOL Resize(INT Width, INT Height);

    // DXGI's own fullscreen state - an experiment, see Backend.cpp. QueryFullscreen asks
    // DXGI (it leaves that state on its own when the window loses focus): 1, 0, or -1.
    UBOOL SetFullscreen(UBOOL bOn);
    INT   QueryFullscreen() const;

    // Frame boundaries. BeginFrame acquires the back buffer and puts it in a state
    // that can be rendered to; EndFrame closes, submits and optionally presents.
    void BeginFrame(const FPlane& ClearColour);
    void EndFrame(UBOOL Present);

    // Geometry submission. The engine-facing side never touches D3D12 itself.
    void AddFan(const FSolidVertex* Verts, INT Count, INT SrvIndex, INT LightSrv, EBlendMode Mode,
                UBOOL bNoSmooth)
    { Solid.AddFan(Verts, Count, SrvIndex, LightSrv, Mode, bNoSmooth); }

    // One triangle the caller writes itself - see FSolidPass::BeginTriangle. Null outside a
    // frame or when there is no room.
    FSolidVertex* BeginTriangle(INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth)
    { return bInFrame ? Solid.BeginTriangle(SrvIndex, LightSrv, Mode, bNoSmooth) : nullptr; }
    FSolidVertex* BeginTriangles(INT Count, INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth)
    { return bInFrame ? Solid.BeginTriangles(Count, SrvIndex, LightSrv, Mode, bNoSmooth) : nullptr; }

    // Uploads on first sight and returns the descriptor index, or -1 if the
    // texture cannot be represented yet (the caller then skips the geometry).
    INT PrepareTexture(const FTextureInfo& Info, DWORD PolyFlags, UBOOL bSrgb, UBOOL bKeepAlpha = 0);
    // The level's geometry for the shadow maps' world maps let go at once (FShadowMaps::ForgetWorld) - only where the
    // GPU is surely done with it: after FlushTextures, at a level's change.
    void ForgetShadowWorld() { Shadow.ForgetWorld(); }
    // Whether that texture was written in this frame (FTextureCache::WrittenThisFrame).
    UBOOL TextureWrittenThisFrame(const FTextureInfo& Info, DWORD PolyFlags, UBOOL bSrgb) const
    { return Textures.WrittenThisFrame(Info, PolyFlags, bSrgb); }

    // The engine offers a level's textures OUTSIDE any Lock/Unlock pair - measured: 500
    // offers, all of them between frames. Uploads therefore need a command list of their
    // own, recorded here and submitted just before the next frame's work.
    INT PrepareTexturePrecache(const FTextureInfo& Info, DWORD PolyFlags, UBOOL bSrgb);
    void ClearDepth();
    // A mirror's view of the level has drawn all it has: its place and axes in the world, its projection and its
    // screen (seventeen numbers, as MirrorFlameView has them), the mirror's plane in that view's space, and the part
    // of the picture the mirror shows in - the engine's pixels, left, top, right and bottom; null for all of it. What
    // is laid over the whole of a view goes on here, in the order the world's view has it: its splashes and its
    // fountains' drops (DrawMirrorSplashes), the rooms' fog from the mirror on, then the walk of what the view
    // gathered - smoke, flames, columns of water (MirrorViewEnd). ViewPlane, Tris: the mirror as the world's view
    // sees it, for the flashlight's light off it (FBeamBounce) - its plane in that view's space, the normal away
    // from the eye, and its polygons there, three numbers a vertex and three vertices a triangle; null for a mirror
    // whose polygons are not known, which gives no light back.
    // Coords: where the view stands in the level for the own lamps (SetOwnLamps) - twelve numbers, as
    // SetOwnWorldView's (RdxMath::MirrorViewCoords); null where it is not known, and the view takes no lamp.
    void MirrorViewDone(const FLOAT* View, const FLOAT* Plane, const FLOAT* Rect, const FLOAT* ViewPlane = nullptr,
                        const FLOAT* Tris = nullptr, INT TriVerts = 0, const FLOAT* Coords = nullptr);
    // A pane of glass that gets its mirror layer this frame (Reflections=RT), for the flashlight's light off it
    // (FBeamBounce): its plane in the world's view's space, the normal away from the eye. Back comes the pane's
    // number in the mirrors' map - panes in one plane share it; 0 where the flashlight is off - and how strongly the
    // lamp shines towards the pane's nearest point, for the lamp's own image in it.
    INT  NoteGlassPane(const FLOAT* Plane, FLOAT& Lamp);
    // A measurement only (the marker SecondRevision-pane-all.txt): the panes give all of the light back, as a mirror
    // does - the light off them then shows by itself against a start without the marker.
    void SetPaneAll(UBOOL bOn) { bPaneAll = bOn ? 1 : 0; }

    // The engine's screen tint, recorded in draw order - see FSolidPass::AddFlash.
    void AddFlash(FLOAT SR, FLOAT SG, FLOAT SB, FLOAT FR, FLOAT FG, FLOAT FB)
    { if (bInFrame) Solid.AddFlash(SR, SG, SB, FR, FG, FB); }

    // The game's Brightness setting, applied to the finished frame by the final pass
    // (RdxMath::OutputExponent). Read every frame: the slider works mid-game.
    void SetBrightness(FLOAT Brightness);

    // The last finished frame, as the engine's FColor array (OutW x OutH, blue in the R
    // field and red in B - the order the engine's own screenshot writer reads), through
    // the same brightness curve the screen shows. For save-game snapshots and
    // screenshots. Only between frames; returns false otherwise.
    UBOOL ReadScene(FColor* Out, INT OutW, INT OutH);

    // The engine changed level. Waits for the GPU, then drops every cached texture -
    // otherwise a long session keeps filling the cache, and a full cache makes light maps
    // stop uploading, which renders surfaces with no lighting at all.
    void FlushTextures();
    INT  GetTriangleCount() const { return Solid.GetTriangleCount(); }
    INT  GetDrawCount()     const { return Solid.GetDrawCount(); }
    INT  GetTextureCount()  const { return Textures.GetCount(); }
    INT  GetRefreshCount()  const { return Textures.GetRefreshCount(); }
    const FTextureCache& GetTextureCache() const { return Textures; }   // per-frame upload diagnostics

    // See Backend.cpp: what this process uses of the card's memory, and what Windows says it may use.
    void VideoMemoryMB(DOUBLE& Used, DOUBLE& Budget) const;

    INT GetWidth()  const { return Width; }
    INT GetHeight() const { return Height; }

    // Diagnostic: how long the last frame change waited for the GPU to hand back the next
    // frame slot - see MoveToNextFrame.
    DOUBLE GetFrameWaitMs() const { return FrameWaitSec * 1000.0; }
    UBOOL IsReady() const { return Device.Get() != nullptr; }
    UBOOL HasPresented() const { return PresentN > 0; }   // a picture of the game's has gone out (not PresentBlack's)
    // The settings on rays (docs/terv-rt-host.md): their rays are traced by the 64-bit host, on every card - this
    // process traces none. Each runs where the host is up and has the pass for it (RtHostPasses), and as its
    // fallback where not.
    //
    // The RT ambient occlusion (docs/terv-rt.md): the SSAO's raw pass done with rays, the rest of it as it is - on a card
    // that traces rays and with the occlusion on; otherwise the SSAO. The marker SecondRevision-rt-ao.txt asks for one start,
    // the setting for good: AmbientOcclusion=RT (SetAmbientOcclusion). Mode: 0 not asked, 1 asked; 2 and 3 are measurements,
    // the rays against the level alone (2) or the actors alone (3) - which of the two a slow view's time goes to.
    void  RequestRtAo(INT Mode)
    {
        bRtAo         = (Mode > 0) ? 1 : 0;
        RtAoInstances = (Mode == 2) ? (INT)FRtPass::RT_MASK_LEVEL : ((Mode == 3) ? (INT)FRtPass::RT_MASK_ACTORS : 0);
    }
    UBOOL IsRtAoRequested() const { return (bRtAo || AoMode == RdxMath::Ao_Rt) && AoMode != RdxMath::Ao_Off; }
    // The passes the 64-bit host has for this card - Host64::ERayPass bits: the shadows' factor, the occlusion, the
    // rays' list - once the scene's lists are kept for it (FRtPass::InitHosted, from SyncHost); 0 until then.
    UINT  RtHostPasses() const { return Rt.IsHosted() ? Host.RayPasses() : 0; }
    UBOOL IsRtAoRunning() const
    {
        return IsRtAoRequested() && bShadows && (RtHostPasses() & Host64::RayPass_Ao) != 0;
    }
    // The RT shadows (docs/terv-rt.md): the shadow factor from rays traced against the scene, the maps not drawn - on a
    // card that traces rays, otherwise the maps as before. For now the marker SecondRevision-rt-shadow.txt asks for it:
    // The setting asks for it too: ShadowQuality=RT (RequestShadowQuality). The marker's modes:
    // Mode 0 off, 1 on; 3 on, with the rays each pixel traced shown in place of the shadows (ShadowFactor.hlsl, Flags 16);
    // 4 on, with the actors' non-opaque triangles - the masked ones, a light's own - never blocking (Flags 32); 5 on,
    // with the factor's two costs changed every few hundred frames through a table, each change logged, so that one run
    // measures them all; 6 on, only the level blocking (Flags 64); 7 on, only the actors (Flags 128). 3 to 7 are
    // measurements.
    void  RequestRtShadows(INT Mode)
    {
        bRtShadows       = (Mode > 0) ? 1 : 0;
        bRtShadowCounts  = (Mode == 3) ? 1 : 0;
        bRtShadowMaskOff = (Mode == 4) ? 1 : 0;
        bRtShadowBench   = (Mode == 5) ? 1 : 0;
        RtShadowOnly     = (Mode == 6) ? 1 : ((Mode == 7) ? 2 : 0);
    }
    // A measurement: the RT factor's two costs fixed for this start (the marker SecondRevision-rt-knobs.txt: lights x 10 +
    // edge samples, 62 for 6 and 2); 0 leaves RdxMath's.
    void  RequestRtKnobs(INT Lights, INT Edges) { RtKnobLights = Lights; RtKnobEdges = Edges; }
    UBOOL IsRtShadowsRequested() const { return (bRtShadows || bRtShadowSetting) && bShadows; }
    // ... and running: asked for, with the host's pass for them there. Otherwise the maps run.
    UBOOL IsRtShadowsRunning() const
    {
        return IsRtShadowsRequested() && (RtHostPasses() & Host64::RayPass_Shadow) != 0;
    }
    // The mirror images on rays (Reflections=RT, docs/terv-rt.md): the water's mirror image and its glints from rays
    // traced against the shadows' scene - with the shadows running, whose scene it is; otherwise as High, from the
    // picture. With the shadows asked for and not able to run nothing gathers their scene: the rays would meet the
    // level alone.
    UBOOL IsRtMirrorRequested() const { return RdxMath::ReflectionsTraced(ReflectionLevel) ? 1 : 0; }
    // ... with fewer rays than pixels (SolidPass.hlsl's RtEye.w): 1 a quarter of them asked (Reflections=RTHalf;
    // RtAskLed), 2 about a ninth of those asked traced (RTLow; RaySparse.hlsl - where its pass could not be made, all
    // are traced), 0 every one.
    INT   GetRtMirrorSparse() const
    {
        return (ReflectionLevel == RdxMath::Reflections_RtMedium) ? 1
             : (ReflectionLevel == RdxMath::Reflections_RtLow) ? 2 : 0;
    }
    // A measurement: how many lamps cast their shadows in the mirror image at RTLow, for this start (the marker
    // SecondRevision-rtlow-lamps.txt; RdxMath::RtLowShadowedLamps otherwise).
    void  RequestRtLowLamps(INT Lamps) { RtLowLamps = Lamps; }
    // ... and running: asked for, the host's list pass there, the shadows running, and the glass layer's two
    // pipelines for the host's rays made. The water's runs, the sludge's mirror layer and the splashes go with it
    // where their own pipelines for the host's rays were made (FSolidPass::MakeHostPair), and draw as without rays
    // where they were not.
    UBOOL IsRtMirrorRunning() const
    {
        return IsRtMirrorRequested() && (RtHostPasses() & Host64::RayPass_List) != 0 && WantsShadows() &&
               Solid.HasHostedGlass();
    }
    // A level surface's light map as it is drawn (RenderDevice.cpp, DrawComplexSurface), for the RT water's colours:
    // whether this light map is new for the surface - only then is its average worth taking - and the average itself.
    UBOOL WantsRtSurfaceLight(INT Surf, QWORD LightId) const
    {
        return IsRtMirrorRequested() && Rt.IsHosted() && Rt.WantsSurfaceLight(Surf, LightId);
    }
    void  SetRtSurfaceLight(INT Surf, QWORD LightId, DWORD Light) { Rt.SetSurfaceLight(Surf, LightId, Light); }
    // And its own picture, for the same rays: which texture, and the rows that put a place of the level on it
    // (FRtPass::SetSurfaceTexture) - so that a mirror ray shows the surface it met, not its texture's average colour.
    UBOOL WantsRtSurfaceTexture() const { return IsRtMirrorRequested() && Rt.IsHosted(); }
    // Whether something the mirror rays are traced for - a water, a pane, a mirror - was drawn in the last frames:
    // while it is, every caster within reach goes into the rays' scene (RenderDevice.cpp, PrepareShadowFrame), not
    // only those whose shadows show. A mirror shows what stands anywhere before it.
    UBOOL RtMirrorDrawnLately() const
    {
        return IsRtMirrorRunning() && RtMirrorIdle < RT_MIRROR_IDLE_FRAMES;
    }
    void  SetRtSurfaceTexture(INT Surf, INT SrvIndex, const FLOAT* Rows, FLOAT Density, UBOOL bKeepRows)
    { Rt.SetSurfaceTexture(Surf, SrvIndex, Rows, Density, bKeepRows); }
    void  ClearRtSurfaceTexture(INT Surf) { Rt.ClearSurfaceTexture(Surf); }
    // A surface's own face, once a level, and the level's lamps, a frame at a time: what the rays work the light
    // of a spot out from, where they meet it (FRtPass::SetSurfaceStatic, SetLamps).
    void  SetRtSurfaceStatic(INT Surf, const FLOAT* Normal, DWORD Ambient, UBOOL bSpecial, DWORD Kind)
    { Rt.SetSurfaceStatic(Surf, Normal, Ambient, bSpecial, Kind); }
    void  SetRtLamps(const BYTE* Blob, INT Bytes, const DWORD* Part, INT Count, INT Large, FLOAT Cell, FLOAT Gain)
    { Rt.SetLamps(Blob, Bytes, Part, Count, Large, Cell, Gain); }
    // And its light map as it is drawn (FRtPass::SetSurfaceLightMap): the rays read the light of the spot they met.
    void  SetRtSurfaceLightMap(INT Surf, INT SrvIndex, const FLOAT* Rows, const FLOAT* Edge, FLOAT Gain)
    { Rt.SetSurfaceLightMap(Surf, SrvIndex, Rows, Edge, Gain); }
    // A measurement only (the marker SecondRevision-rt-water-view.txt): the RT water shows, in place of the water, what its
    // mirror rays met (1), its glints alone (2), or how far its mirror rays went (3) - SolidPass.hlsl's colours for them;
    // or the water with the surfaces it mirrors in their own colours alone, never the picture's (4), or with the sky from
    // the far probe alone, never the picture (5), or how high above the water the mirror rays met something (6), or as
    // 4 with every texture's average colour in the texture's place (7). 0 the water.
    void  RequestRtWaterView(INT Mode) { RtWaterView = Mode; }

    // Diagnostic, off unless asked for: the D3D12 debug layer, its messages into our log.
    // Must be asked before Init - the layer has to be on before the device is created.
    void RequestValidation(UBOOL bOn) { bValidation = bOn ? 1 : 0; }

    // With the debug layer, for a measurement: its GPU-based validation as well. The layer alone checks what a draw
    // is handed; this checks, inside the shaders, what the draw goes on to read - a resource in a state it cannot be
    // read in, a descriptor that is not there - which is what one card puts up with and another stops at. Many
    // times slower. Asked before Init, like the layer, and nothing without it.
    void RequestGpuValidation(UBOOL bOn) { bGpuValidation = bOn ? 1 : 0; }

    // Big palettised textures converted on the GPU - FTextureCache::SetGpuConvert. Asked before
    // Init, like the debug layer.
    void RequestGpuConvert(UBOOL bOn) { bGpuConvert = bOn ? 1 : 0; }

    // The GPU key: which of the machine's cards to run on - Auto, a card's number in the log's list, or a piece of its
    // name (RdxMath::GpuAskedFor). Asked before Init: the card is chosen once, when the device is made.
    void RequestGpu(const TCHAR* Text);
    // A measurement: the display's HDR state asked of every card afresh, as on a machine whose screen hangs on another
    // card than the one the picture is drawn on - not of the device's own card first (ReadDisplayHdr).
    void RequestDisplayAskAll(UBOOL bOn) { bDisplayAskAll = bOn ? 1 : 0; }
    // SafeMode (SecondRevision.settings.ini, read once at the start): the 64-bit host is never started and never looked
    // for, and the display is not asked about HDR - the picture is eight-bit SDR. What needs the host runs as it does
    // wherever the host cannot: the built-in FSR 3.1, the maps' shadows, the screen's own reflections and occlusion.
    void  RequestSafeMode(UBOOL bOn) { bSafeMode = bOn ? 1 : 0; }
    UBOOL IsSafeMode() const { return bSafeMode; }

    // Supersampling (docs/terv-latvany-roadmap.md 1.2 and 2.1): the scene drawn at Scale times the output's size in
    // each direction, and box-filtered down by the final pass. 1 = off - the vanilla path, untouched. Asked before
    // Init, like the two above. The caller clamps (RdxMath::MaxSceneScale: the setting's ceiling of 4 and the D3D12
    // texture limit); this only refuses nonsense.
    void RequestSceneScale(INT Scale)
    { SceneScale = (Scale < 1) ? 1 : ((Scale > (INT)RdxMath::SuperSampleMax) ? (INT)RdxMath::SuperSampleMax : Scale); }
    // The averaging space of that box filter: approximately linear light (on - the default, the player's
    // choice) or the stored numbers (off).
    void RequestLinearMix(UBOOL bOn) { bLinearMix = bOn ? 1 : 0; }
    // Upscaling (docs/terv-latvany-roadmap.md 2.3; ): the scene drawn at the antialiased image's size
    // over the mode's ratio (RdxMath::UpscalingRatio: 1 NativeAA - 1.0, 2 Quality, 3 Balanced, 4 Performance, 0 off) and
    // scaled up to it before the HUD - AMD FSR 3.1, or a plain bilinear filter where it cannot run (and with bLinear,
    // for an A/B).
    // NativeAA upscales by nothing: it is the temporal antialiasing on its own, at full resolution.
    // Asked before Init. Supersampling is NOT cancelled by it: the two compose, see
    // RdxMath::SceneSideFor.
    void RequestUpscaling(INT Mode, UBOOL bLinear)
    {
        UpscaleMode    = (Mode >= 1 && Mode <= 4) ? Mode : 0;
        bUpscaleLinear = bLinear ? 1 : 0;
    }
    // A post-process antialiasing filter on the finished picture (AntiAliasing=FXAA*/SMAA*): 1 = FXAA, 2 = SMAA,
    // 0 = none. Span is how many pixels it may reach along an edge (2, 4, 8 or 16). Asked before Init.
    void RequestPostAa(INT Kind, FLOAT Span) { PostAaKind = (Kind >= 1 && Kind <= 2) ? Kind : 0; PostAaSpan = Span; }
    INT  GetPostAa() const { return PostAaKind; }

    // Hardware multisampling (AntiAliasing=MSAA*): how many samples per pixel. Asked before Init; the card may
    // refuse a count, and then MsaaSamples() says what it really got.
    void RequestMsaa(INT Samples) { MsaaWanted = (Samples == 2 || Samples == 4 || Samples == 8 || Samples == 16)
                                                     ? Samples : 1; }
    INT  MsaaSamples() const { return MsaaCount; }

    INT  GetUpscaling() const { return UpscaleMode; }
    // For the readout: whether FSR 3.1 is really doing the upscaling this frame, or it was asked for and failed, so
    // the bilinear pass took over. What is SET and what RUNS are two different things, and the screen should say which.
    UBOOL Fsr3Running() const { return bFsr3Wanted && Fsr3Ready > 0; }
    // NativeAA's mip bias: AMD's rule gives it -1, which is to be decided on the picture (RdxMath::FsrMipBias) - asked
    // by a measurement marker for now.
    void  RequestNativeMipBias(UBOOL bOn) { bNativeMipBias = bOn ? 1 : 0; }
    UBOOL Fsr3Failed()  const { return bFsr3Wanted && Fsr3Ready < 0; }
    // The motion vectors' target was refused at the scene's size as it is: no temporal upscaler can take the frame
    // until that size changes - for the readout, which would otherwise have one on its way.
    UBOOL MotionRefused() const
    {
        return bMotionWanted && bMotionFailed && MotionFailedW == SceneWidth && MotionFailedH == SceneHeight;
    }
    // The mode changed in play (Num Lock): the scene's size changes with it, so everything scene- or output-sized is made
    // again. Only between frames. Returns whether it went through; if not, the old mode stays.
    UBOOL SetUpscaling(INT Mode);

    // The factor pass at half resolution, changed in the running game (the renderer's menu). Takes effect at the
    // next SetShadowQuality, which is how the menu applies it: it is a field of the preset, not a pass of its own.
    void SetShadowHalfFactor(UBOOL bHalf) { OverrideFullRes = bHalf ? 0 : 1; }

    // The shadow preset changed in the running game - see Backend.cpp. Only between frames.
    UBOOL SetShadowQuality(INT Quality);

    // The whole AntiAliasing setting changed in the running game - see Backend.cpp. Only between frames.
    UBOOL SetAntiAliasing(INT Mode, INT Scale, INT PostKind, FLOAT PostSpan, INT Msaa);

    // HDR AND TEN-BIT OUTPUT (RdxMath::EHdrMode). Asked before Init, and changed in the running game between two
    // frames by SetHdrMode - which is a heavier change than the others here: every colour target, every pipeline
    // that writes one and the swap chain's own buffers carry the format, so all of them are made again.
    void  RequestHdrMode(INT Mode)
    { HdrMode = (Mode >= 0 && Mode < (INT)RdxMath::Hdr_Count) ? (RdxMath::EHdrMode)Mode : RdxMath::Hdr_Off; }
    UBOOL SetHdrMode(INT Mode);
    INT   GetHdrMode() const { return (INT)HdrMode; }
    // What the OUTPUT is really doing, which is not always what was asked for: the chain steps down on a display that
    // cannot take the mode (RdxMath::HdrModeForDisplay), and a mode whose pipeline could not be built is given up.
    // That is what the readout has to show - with the reason, GetHdrFallback (RdxMath::EHdrFallback).
    INT   GetHdrRunning() const { return (INT)HdrRunning; }
    INT   GetHdrFallback() const { return (INT)HdrWhy; }
    // Whether the display the window is on reports itself as HDR (IDXGIOutput6). Read at Init, at every SetHdrMode and
    // whenever the picture moves to another display (OnDisplayChanged).
    UBOOL GetDisplayHdr() const { return bDisplayHdr; }

    // Has the device been lost? Asked by the readout, and by anything that would otherwise go on believing what
    // it measures - see bDeviceLost.
    UBOOL GetDeviceLost() const { return bDeviceLost; }
    // A change of size or mode began to make the targets again and did not finish, and the ones before it were not put
    // back either: a frame begun now would draw into what is not there. Asked before every frame - the answer to it is
    // the lost device's, the renderer taken down and brought up again (URevisionRenderDevice::RecoverLostDevice).
    UBOOL TargetsUnmade() const { return bTargetsUnmade; }
#if RDX_TEST_DEVICE_LOSS
    // A test build only: the device taken away on purpose (EndFrame), where a marker arms it.
    void  RequestTestLoss(UBOOL bOn) { bTestLoss = bOn ? 1 : 0; }
#endif

    // The monitor the picture is on, by the name Windows' own display settings give it, in the caption font's
    // alphabet - see ReadDisplayHdr. Empty until the display has been asked.
    const char* GetDisplayName() const { return DisplayName; }
    // Bits a channel on the wire to that monitor - the SIGNAL, not the panel: a panel that makes ten bits out of
    // eight by flickering between neighbours (FRC) is invisible from here. 0 when it could not be asked.
    INT GetDisplayBits() const { return DisplayBits; }
    // Whether the monitor can do HDR at all, whatever Windows has it set to right now: 1 yes, 0 no, -1 unknown.
    INT GetDisplayHdrCapable() const { return DisplayHdrCapable; }

    // Ambient occlusion - see RdxMath::EAmbientOcclusion. It takes effect on the next frame and builds nothing on the
    // spot: its pipelines come up on first use, as the shadows' do. It runs only while the shadows run, because what it
    // darkens is their factor. bAlone puts the occlusion IN PLACE of the shadows - a view for judging what it does on its
    // own, never a setting.
    void  SetAmbientOcclusion(INT Mode, UBOOL bAlone);
    // The water (Water=Off/Low/High, docs/meres-11). 0 is the engine's own water; above it the geometry pass draws
    // see-through water over copies of the picture and the depth (CopyForWater), which needs a depth buffer that can
    // be read - made so by building the targets again, between frames. False if that failed.
    UBOOL SetWater(INT Level);
    INT   GetWater() const { return WaterLevel; }
    // What gives a mirror image back (Reflections=Default/High/RT, RdxMath::EReflections). 0: none of ours; above
    // it the water's surface and the levels' glass mirror the picture, or what rays find where they can run
    // (IsRtMirrorRunning). The glass's layer reads the water's copies of the picture and the depth (CopyForWater),
    // which needs a depth buffer that can be read - made so by building the targets again, between frames. False
    // if that failed.
    UBOOL SetReflections(INT Level);
    INT   GetReflections() const { return ReflectionLevel; }

    // The relief's height maps. The set is chosen by name - the folder under SecondRevision.pom - and changing it in
    // play is the point: the same wall, the same light, a different height map. GetPomCount is what the readout
    // reports, so a player can see at a glance whether the set is there at all.
    // Soft particles: how far in front of what is behind them the translucent surfaces fade out, in the level's
    // units. Zero is off. The depth copy the fade reads is the water's - see CopyForWater - so with the fade on the
    // depth buffer is one that can be copied (NeedsDepthRead): where it was not, the targets are made again. Only
    // between frames. False where they could not be made again.
    UBOOL SetSoftFade(FLOAT Units);
    // The depth buffers made again as ones that can be read, where something wishes to read them (NeedsDepthRead) and
    // they were not made so - a switch thrown after they were made, whichever it was. Only between frames. False where
    // the targets could not be made again; that is not tried again until they are made anew.
    UBOOL EnsureDepthReadable();
    // Smoke and gas as volumes (Volumetrics). Set before the frame's draws: it decides whether the copy of the
    // scene's depth is made, and a cloud with no depth behind it has nothing to stop its walk at a wall.
    // While the walk is out (VolumeOut) the GATHERING stops too - otherwise the cards would be taken out of the
    // frame and nothing put in their place, and the smoke would simply be gone. The same where the puffs' own lists
    // could not be made as large as a frame needed (bCloudListsRefused).
    void  SetVolumetrics(UBOOL bOn) { Solid.SetCloudsOn(bOn && !VolumeOut() && !bCloudListsRefused); }
    // The walk cannot run: its passes were refused, which is for good, or its target could not be made
    // (DrawVolumetrics), which lasts while the sizes do (TargetAsk).
    UBOOL VolumeOut() const { return (bVolumeRefused || Refused(VolumeRefusal)) ? 1 : 0; }
    // The walk asked for by a switch of its own - the smoke's, the fires'. It stops at a copy of the world's depth, and
    // that copy is the world's only in a frame whose depth is read (NeedsDepthRead). It is made where the world ends,
    // and the frame has to have that place whenever something may be gathered for it (WorldEndWanted): the smoke's
    // cards, the flames and the waterfalls' sheets are taken out of the frame, and only the walk puts anything in
    // their place.
    UBOOL WalkWanted() const { return (Solid.GetCloudsOn() || bFireWanted) ? 1 : 0; }
    // Whether the smoke asked for can be walked at all: what the menu and the readout say beside the switch.
    UBOOL VolumetricsCanRun() const { return (VolumeReady >= 0 && !VolumeOut() && !bCloudListsRefused) ? 1 : 0; }
    // A level change: the puffs followed are the level's that went.
    void  ForgetClouds()
    {
        CloudTrackCount = 0;
        CloudLastTime   = 0.0;
    }
    // What DRAWTILE asks: may a cloud card be gathered instead of drawn? Only if the walk will actually be able
    // to run - and it cannot without the depth copy (CopyForWater), which the solid pass only makes when a water
    // or a soft-particle run comes along. Under MSAA that run never comes, and without this the cards were
    // gathered, the walk was skipped, and the gas was GONE ALTOGETHER rather than merely flat.
    //
    // Last frame's answer, because this one's is not known when the first card arrives. That costs one frame of
    // flat sprites after a change and then settles - and it cannot latch off, because the copy is asked for by
    // WantsCloudDepth whether or not anything was gathered.
    UBOOL GetVolumetrics() const { return Solid.GetCloudsOn() && bCloudDepthLast; }
    // A CARD OF SMOKE WAS SEEN, whether it was gathered or drawn as it always was. This is what keeps the
    // depth copy wanted, and it has to be the card rather than the gathering - otherwise the three of them
    // latch: no copy, so no gathering; no gathering, so no puffs; no puffs, so the copy is not wanted, so no
    // copy. Once out, never back, and what the player saw was flat sprites for the rest of the session.
    // Grew: how much wider the card became once the screen's clip was put back (1 = nothing was cut). Kept for
    // the log, because a card that is nearly all cut away is where that reasoning is least sure of itself.
    void  NoteCloudCard(FLOAT Grew)
    {
        CloudCardsSeen++;
        if (Grew > CloudGrewMax)
            CloudGrewMax = Grew;
    }
    // Whether the depth copy is worth making for the smoke. Not the same question as whether the smoke is ON:
    // the GATHERING has to go on either way, or there would never be a puff to notice.
    // The shafts need the same copy of the depth, and they are wanted whether or not any smoke is on screen -
    // the air is everywhere. So do the caustics, laid on by it wherever a water is near (LayCaustics).
    UBOOL WantsCloudDepth() const
    {
        return (Solid.GetCloudsOn() && CloudsRecent > 0) || ShaftStrength > 0.0f || (bWaterWanted && FallsRecent > 0) ||
               FiresRecent > 0 || (FogRoomsWanted() && FogLightCount > 0) || CausticsNow() || FieldCount > 0 ||
               HoloVertCount > 0 || OwnBeamsNow();
    }
    // A level's waterfall (Water=Low and up), gathered instead of drawn: all the sheets of one column of water
    // (RdxMath::FFallSheetPiece). DrawVolumetrics makes columns of them (RdxMath::FallColumnsMake) - sheets that
    // cross, one round column; a sheet on its own, a flat ribbon - and walks them with the smoke, as falling water.
    // False where the fall is not taken - no room for its sheets, or for one more column: the caller then draws them
    // as sheets of falling water, so no fall is ever simply gone.
    UBOOL AddFall(const RdxMath::FFallSheetPiece* Pieces, INT Count);
    // A waterfall's sheet was seen, gathered or drawn: what keeps the depth copy wanted for the columns, as a cloud card
    // does for the smoke - so the two cannot latch off together (no copy, so no gathering, so no copy wanted).
    void NoteFallSheet() { FallSheetsSeen++; }
    // Whether a sheet may be gathered instead of drawn: the water on, the volumes' passes not refused, and last frame's
    // depth copy there - this frame's is not known when the first sheet arrives (as GetVolumetrics).
    UBOOL CanFallVolumes() const { return bWaterWanted && VolumeReady >= 0 && !VolumeOut() && bCloudDepthLast; }
    // A piece of a fire's flame (the Fire3D key), gathered instead of drawn - a flame card, a mesh's flame polygon
    // (RdxMath::FFlamePiece) - and a level's fire, all of its flame sheets together (AddFire). DrawVolumetrics makes
    // flame bodies of the pieces that stand together (RdxMath::FlameBodiesMake) and walks them with the smoke, as
    // light. NoteFireSheet and CanFireVolumes as for the waterfalls.
    // A frame takes every flame it is given: the lists are made as long as the frame needs (HoldFlames). False only
    // where the memory for that is refused - the caller then draws the game's own picture of the flame, and so does
    // every frame after it (bFireListsRefused): no flame is ever simply gone, and none goes back and forth between
    // the two pictures.
    UBOOL AddFirePiece(const RdxMath::FFlamePiece& P);
    UBOOL AddFire(const RdxMath::FFlamePiece* Pieces, INT Count);
    // Whether a flame Wide units across, Depth into the view, is wide enough on the walk's target to be walked
    // (RdxMath::FireMinPixels; bWalked: it was the last time, and is kept a little smaller than a new one is taken).
    UBOOL FireShows(FLOAT Wide, FLOAT Depth, UBOOL bWalked) const
    {
        const FLOAT Pixels = RdxMath::FlamePixels(Wide, Depth, CloudFocal, (FLOAT)Width,
                                                  FVolumetrics::TargetSide(SceneWidth));
        return (Pixels >= (bWalked ? RdxMath::FireKeepPixels : RdxMath::FireMinPixels)) ? 1 : 0;
    }
    void  NoteFireSheet() { FireSheetsSeen++; }
    UBOOL CanFireVolumes() const
    {
        return VolumeReady >= 0 && !VolumeOut() && !bFireListsRefused && bCloudDepthLast;
    }
    // Whether the fires asked for can be walked at all: what the menu and the readout say beside the switch.
    UBOOL FireCanRun() const { return (VolumeReady >= 0 && !VolumeOut() && !bFireListsRefused) ? 1 : 0; }
    // The Fire3D setting, said before the frame's draws like the smoke's (SetVolumetrics): the walk its flames are
    // gathered for stops at the same copy of the depth (WalkWanted).
    void  SetFireVolumes(UBOOL bOn) { bFireWanted = bOn ? 1 : 0; }
    // The sparks the walked fires throw (they come with Fire3D): drawn by the walk with the flames, nothing gathered
    // for them.
    void  SetFireSparks(UBOOL bOn) { bFireSparks = bOn ? 1 : 0; }
    // The air trembling over the walked fires (with Fire3D as well): a pass of the walk's own, after its result is
    // laid on the picture (FVolumetrics::RecordHaze).
    void  SetHeatHaze(UBOOL bOn) { bHeatHaze = bOn ? 1 : 0; }
    // How many times their first pace the walked fires' flames rise at (the FireSpeed key, the menu).
    void  SetFireSpeed(FLOAT Times) { FireSpeed = Clamp(Times, 0.5f, 4.0f); }
    // The upscaler's reactive mask where the walk lays smoke or a waterfall's column: the share of their cover of a
    // pixel that is told (RdxMath::CloudReactive, WaterfallReactive), 0 for nothing told. A marker's, for a
    // measurement.
    void  SetWalkReactive(FLOAT Smoke, FLOAT Falls)
    {
        WalkReactiveSmoke = Clamp(Smoke, 0.0f, 1.0f);
        WalkReactiveFalls = Clamp(Falls, 0.0f, 1.0f);
    }
    // The level's fog kept in its rooms (FFogRooms), said before the frame's draws like the fires: its pass stops at
    // the same copy of the world's depth as the walk, at the same place in the frame (WorldEndWanted).
    void  SetFogRooms(UBOOL bOn) { bFogRoomsWanted = bOn ? 1 : 0; }
    UBOOL FogRoomsWanted() const { return bFogRoomsWanted; }
    // Whether this frame's fog is the pass's - and so whether the engine's own fog maps and meshes' fog are left
    // undrawn. Asked at the world's first draw, after SetFogRooms.
    UBOOL FogRoomsCanRun() const
    {
        return (FogRoomsWanted() && FogRoomsReady >= 0 && bDepthReadable && FogRoom.NodeCount() > 0) ? 1 : 0;
    }
    // A level's BSP for the pass's zone search, at the level's first frame; and each frame its fog lights - every one
    // of them: there is no most lights a frame's fog may come from. A light left out for want of a place has no fog
    // at all - the engine's own is not drawn while the pass lays the frame's - and it would come as the eye walked
    // up to it.
    UBOOL SetFogNodes(const FFogNode* Nodes, INT Count);
    void  SetFogLights(const FFogLight* Lights, INT Count, UINT CameraZone);
    // A list of the fog's lights could not be made as long as a frame needed: the pass is out from here on, and the
    // engine draws its own fog (FogRoomsCanRun) - all of it, never the pass's with some lights missing.
    void  RefuseFogRooms(const TCHAR* Which, INT Wanted);
    // The force field as a volume of the level's space (ForceField=3D, FFieldVolume). FieldVolumeRuns: the pass is
    // there and the world's depth can be read for it - asked before a field's surface is left undrawn. A frame's
    // field is the polygons handed over in it, each once (Key tells them apart): the level's own, whole, in the
    // world, FieldCorners corners at most. Kind says which of the game's fields a polygon is of (its texture), Look
    // that field's hue and how bright it is laid on: a frame may hold fields of several kinds, each laid in its
    // own look. SetFieldClock: the level's clock.
    UBOOL FieldVolumeRuns();
    // The switch's wish (ForceField=3D): the world's depth readable, and a place in the frame where the world ends -
    // the pass is drawn there, and without one a field taken out of the world's drawing would be in no picture.
    void  SetFieldVolumeWanted(UBOOL bWanted) { bFieldWanted = bWanted ? 1 : 0; }
    // The pass could not be made, or the depth cannot be read: said in the menu and the readout.
    UBOOL FieldVolumeRefused() const { return (FieldReady < 0 || !bDepthReadable) ? 1 : 0; }
    void  AddFieldPolygon(INT Key, const FLOAT* Points, INT Count, INT Kind, const FLOAT Look[4]);
    void  SetFieldClock(FLOAT Time) { FieldTime = Time; }
    // HdtpHoloFix, On or Old: a hologram's triangles laid by a pass of their own, the nearest alone (FHoloShell).
    // SetHoloShellWanted: the switch's wish - the world's depth readable, and a place in the frame where the world
    // ends, as the field's (SetFieldVolumeWanted). HoloShellRuns: the pass is there and the depth can be read - asked
    // before a triangle is kept from the solid pass. AddHoloTriangles: room for Count triangles of one texture in
    // the frame's list; null where there is none, and the caller draws them as it always did. SetHoloShellLayers:
    // how many of the game's translucent layers the nearest surface is laid as (RdxMath::HoloShellLayers).
    void  SetHoloShellWanted(UBOOL bWanted) { bHoloShellWanted = bWanted ? 1 : 0; }
    // The pass could not be made, or went out after frames it could not lay: said in the menu and the readout.
    UBOOL HoloShellRefused() const { return ShellReady < 0; }
    void  SetHoloShellLayers(FLOAT Layers)  { HoloLayers = Clamp(Layers, 0.25f, 4.0f); }
    UBOOL HoloShellRuns();
    FHoloShell::FVertex* AddHoloTriangles(INT SrvIndex, UBOOL bNoSmooth, INT Count);
    void  DrawHoloShell(ID3D12GraphicsCommandList* Cmd);
    UBOOL FogRoomsRefused() const { return FogRoomsReady < 0; }
    // How much light a hundred units of air scatters towards the eye; 0 is off.
    void  SetGodRays(FLOAT Strength) { ShaftStrength = (Strength > 0.0f) ? Strength : 0.0f; }
    FLOAT GetGodRays() const { return ShaftStrength; }
    // The shafts ask the level's own shadow CUBE MAPS at every step. With the RT shadows the factor comes from
    // rays and THE MAPS ARE NOT DRAWN AT ALL - only the masks are uploaded. Reading them then is not an error
    // and not a blank picture: it is last frame's walls, or a level ago's, with nothing saying so.
    UBOOL ShaftsCanRun() const { return bCubesDrawn && Shadow.IsReady() && Shadow.GetLightCount() > 0; }
    // The same for the menu and the readout, which are written before the frame's lights are set and its shadows
    // drawn: by the frame before, whose maps are drawn only with lights to draw them for. Asked the way the walk asks,
    // the two would say "no shadow maps" of every frame, the ones the shafts run in too.
    UBOOL ShaftsRanLast() const { return bCubesDrawnLast; }
    // The lists of the shafts' lamps by tile could not be made as long as a frame needed: no shafts from there on -
    // from some of the lamps they would come and go with the frame - and the menu and the readout say so.
    UBOOL ShaftsRefused() const { return bShaftListsRefused; }
    // The smear needs motion vectors, and those are made FOR THE UPSCALER. With no upscaler there are none,
    // whatever the shadows are doing (DrawMotionBlur, and the gate in DrawMotionVectors).
    UBOOL BlurCanRun() const { return UpscaleMode != 0; }
    // The smear, the lens and film, and the halos switched on and not running: a pass that could not be made, or its
    // own pictures with the sizes as they are (RefuseTarget). The halos are added by the film's pass, so they are out
    // with it. What the readout says beside their switches.
    UBOOL BlurOut() const  { return (BlurReady < 0 || Refused(BlurRefusal)) ? 1 : 0; }
    UBOOL FilmOut() const  { return (FilmReady < 0 || Refused(FilmRefusal)) ? 1 : 0; }
    UBOOL BloomOut() const { return (FilmOut() || BloomReady < 0 || Refused(BloomRefusal)) ? 1 : 0; }

    // ONE puff of smoke or gas, gathered rather than drawn. The card the game sent is not drawn at all: it is only
    // a message saying where a puff is, how big it is and what colour. What draws them is a single walk a pixel,
    // over the WHOLE list - which is why nothing here is per puff on the screen, and why two puffs that overlap add
    // up instead of cutting into one another.
    //
    // Everything is in view space, where the walk happens.
    // Twelve floats, in the order the walk's own buffer wants them (FVolumetrics). No seed: the noise is read at the
    // sample's place in the WORLD, so two puffs standing apart differ by where they are and nothing has to be sown.
    struct FCloudPuff
    {
        FLOAT Centre[3];
        FLOAT Radius;        // across, the level's units
        FLOAT Tall;          // and up
        FLOAT Colour[3];     // the colour its own picture is painted in
        FLOAT Dense;         // how thick its middle is, by the unit: from its size and its fade (AddCloudPuff fills it)
        FLOAT Fade;          // the game's own fade of its card, 0..1
        FLOAT Spare[2];
    };

    // Gathered by the draw path, cleared at the frame's start. The list has no top: it holds what the frame brings.
    // False only where there is no memory for one more - the caller then draws the game's own picture of it, so that
    // nothing a card showed is ever simply gone.
    void  ClearCloudPuffs() { CloudPuffCount = 0; CloudPuffLost = 0; }
    UBOOL AddCloudPuff(const FCloudPuff& P);
    INT  GetCloudPuffCount() const { return CloudPuffCount; }
    INT  GetCloudPuffsLost() const { return CloudPuffsLost(); }
    const FCloudPuff* GetCloudPuffs() const { return CloudPuffs; }
    INT  CloudPuffsLost() const { return CloudPuffLost; }
    // The engine's projection and the centre of the world's view, in the engine's own pixels - what puts a pixel back
    // on a ray - and that view's rectangle on the screen: in a conversation it is the picture between the black bars,
    // its centre not the screen's. Taken from the draw path, because that is where the engine hands them over.
    void SetCloudView(FLOAT Focal, FLOAT Sx, FLOAT Sy, FLOAT X0, FLOAT Y0, FLOAT X1, FLOAT Y1)
    {
        CloudFocal     = Focal;
        CloudScreen[0] = Sx;
        CloudScreen[1] = Sy;
        CloudRect[0]   = X0;
        CloudRect[1]   = Y0;
        CloudRect[2]   = X1;
        CloudRect[3]   = Y1;
    }
    // How the smoke is shaped and lit. Swept in play from the draw path's markers, because a rebuild between two
    // values is a minute and a half and there are five of them to settle.
    struct FCloudTuning
    {
        FLOAT Depth, Grain, Ambient, Rise, Warp, SelfShade, StepWorld;
        INT   Steps;
    };
    void SetCloudTuning(const FCloudTuning& T) { CloudTune = T; }
    // The whole of the smoke and gas, walked once a pixel and laid over the finished picture. Called at the world's
    // end, AFTER the shadows: a shadow belongs to the wall behind the smoke, not to the smoke in front of it.
    void DrawVolumetrics(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    // The level's fog over the finished world, before the walk lays its smoke over both (FFogRooms).
    void DrawFogRooms(ID3D12GraphicsCommandList* Cmd);
    void DrawFieldVolume(ID3D12GraphicsCommandList* Cmd);
    // Motion blur, at the world's end and after everything that belongs to the world - the smoke included,
    // because smoke moves too and a shutter does not tell it apart from a wall. Zero is off, and then nothing
    // about the frame changes, not even the motion vectors being asked for.
    void  SetMotionBlur(FLOAT Strength) { BlurStrength = (Strength > 0.0f) ? Strength : 0.0f; }
    FLOAT GetMotionBlur() const { return BlurStrength; }
    void  DrawMotionBlur(ID3D12GraphicsCommandList* Cmd);
    // Depth of field, in a conversation's camera only (DepthOfField.h). Zero is off. The render device says every frame
    // whether a conversation's camera runs and where its speaker stands, in world units; the pass fades in and out and
    // follows the speaker by itself.
    void  SetDepthOfField(FLOAT Strength) { DofStrength = (Strength > 0.0f) ? Strength : 0.0f; }
    // Measurements only: the focus's lead in front of the speaker and the band that stays sharp, in place of
    // RenderMath's.
    void  SetDofTuning(FLOAT Lead, FLOAT Band) { DofLead = Lead; DofBand = Band; }
    // Whether it has what it needs - a depth to read, single-sampled, its pass and its pictures (DrawDepthOfField):
    // what the menu and the readout say beside the switch.
    UBOOL DofCanRun() const
    {
        return (NeedsDepthRead() && !MsaaOn() && DofReady >= 0 && !Refused(DofRefusal)) ? 1 : 0;
    }
    void  SetConversationFocus(UBOOL bActive, const FLOAT* WorldPos);
    void  DrawDepthOfField(ID3D12GraphicsCommandList* Cmd);
    // The lens and the film: the colours split towards the corners, the corners darkened, and grain. All three
    // on the FINISHED picture - after the antialiasing and the upscaler, before the HUD. Zero for each is off,
    // and with all three zero nothing runs at all.
    // The bloom: two halos added together, of the picture's bright parts at Picture and of the engine's coronas
    // (NoteCorona) at Coronas; 0 leaves one out.
    void  SetBloom(FLOAT Picture, FLOAT Coronas)
    {
        LookBloom        = (Picture > 0.0f) ? Picture : 0.0f;
        LookBloomCoronas = (Coronas > 0.0f) ? Coronas : 0.0f;
    }
    // One of the engine's coronas as DrawTile drew it - its corners in the engine's pixels, the part of its texture,
    // its colour, and its texture's view. Gathered a frame at a time and drawn again, small, for the coronas' halo and
    // the lens's ghosts (FBloom::DrawCoronas).
    void  NoteCorona(INT Srv, FLOAT X0, FLOAT Y0, FLOAT X1, FLOAT Y1, FLOAT U0, FLOAT V0, FLOAT U1, FLOAT V1,
                     FLOAT R, FLOAT G, FLOAT B);
    INT   GetCoronaCount() const { return BloomCoronaCount; }
    // The lens flare, its ghosts thrown by the engine's coronas (NoteCorona): Strength 0 is off.
    void  SetLensFlare(FLOAT Strength)
    {
        LookFlare = (Strength > 0.0f) ? Strength : 0.0f;
    }
    void  SetFilmLook(FLOAT Aberration, FLOAT Vignette, FLOAT Grain)
    {
        LookAberration = (Aberration > 0.0f) ? Aberration : 0.0f;
        LookVignette   = (Vignette > 0.0f) ? Vignette : 0.0f;
        LookGrain      = (Grain > 0.0f) ? Grain : 0.0f;
    }
    UBOOL FilmLookWanted() const
    {
        return (LookAberration > 0.0f || LookVignette > 0.0f || LookGrain > 0.0f || LookBloom > 0.0f ||
                LookBloomCoronas > 0.0f || LookFlare > 0.0f) ? 1 : 0;
    }
    // What is drawn where the world ends (SwitchToOutput, EndWorldInPlace) by a switch of its own: the lens and the
    // film, the depth of field, the room fog, and the walk - the smoke, the fires, and the waterfalls' columns that
    // come with the water. That place comes with an output target or with the shadows; where the frame has neither it
    // is made for these alone (BeginHud), or they would be switched on and do nothing - and what was gathered for the
    // walk would be gone from the frame. By the switches and not by what the frame holds: what is gathered is decided
    // while the world is drawn, before its end is.
    UBOOL WorldEndWanted() const
    {
        return (FilmLookWanted() || DofStrength > 0.0f || WalkWanted() || FogRoomsWanted() || bWaterWanted ||
                bFieldWanted || bHoloShellWanted) ? 1 : 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE EndWorldInPlace(ID3D12GraphicsCommandList* Cmd);
    void  DrawFilmLook(ID3D12GraphicsCommandList* Cmd);
    // This frame's lights, for what the shadow maps cannot light: a cloud of smoke standing in the air has no
    // surface for the shadow pass to darken, so it asks for the lights themselves and lights each step of the walk.
    INT   LightCount() const { return Shadow.GetLightCount(); }
    // Bounds checked here and not in the shadow maps: theirs is asked for inside their own loops, where the index
    // has already been counted out, and a test a frame's lights long is not free there.
    const FShadowLightFrame* LightAt(INT i) const
    {
        return (i >= 0 && i < Shadow.GetLightCount()) ? &Shadow.GetLight(i) : nullptr;
    }
    FLOAT GetSoftFade() const { return Solid.GetSoftFade(); }
    void  SetPomMethod(const char* Extension) { Pom.SetMethod(Extension); }
    INT   GetPomCount() const { return Pom.GetCount(); }
    INT   GetPomUploaded() const { return Pom.GetUploaded(); }
    // The entry for a texture, or -1; then its maps on the card, on first use.
    UBOOL PreparePom(INT Entry);
    // Where this entry's two views sit in the texture heap (the height first, the colour after it), its size, and
    // whether a colour was packed with it at all.
    INT   GetPomTable(INT Entry) const { return Pom.GetHeightSrv(Entry); }
    // Two steps, so a surface whose texture is already known costs neither the engine's path name nor a walk of the
    // list: ask what is remembered, and only look it up properly when nothing is.
    INT   PomRemembered(QWORD CacheId, INT USize, INT VSize) const { return Pom.Remembered(CacheId, USize, VSize); }
    INT   PomNotAsked() const { return FPomLibrary::PomNotAsked; }
    INT   FindPomCached(QWORD CacheId, const TCHAR* PathName, INT USize, INT VSize)
    { return Pom.FindCached(CacheId, PathName, USize, VSize); }
    // By the path alone, where no cache id is at hand (the spherifier's copy of the level, which asks once a texture).
    INT   FindPom(const TCHAR* PathName, INT USize, INT VSize) { return Pom.Find(PathName, USize, VSize); }
    void  GetPomSize(INT Entry, FLOAT& U, FLOAT& V) const { Pom.GetSize(Entry, U, V); }
    // A height map of the renderer's own - a grown plant's bark (FPomLibrary::AddOwn, MakeOwnBlob).
    INT   PomOwn(const char* Key, const BYTE* Blob, INT Bytes, INT W, INT H, FLOAT Depth)
    { return Pom.AddOwn(Key, Blob, Bytes, W, H, Depth); }
    static BYTE* PomOwnBlob(const BYTE* Heights, INT W, INT H, INT& Bytes)
    { return FPomLibrary::MakeOwnBlob(Heights, W, H, Bytes); }
    // The relief the runs that follow are drawn with, and the end of it.
    void  SetPomRun(INT Table, FLOAT Depth, FLOAT Flags, FLOAT InvW, FLOAT InvH)
    { Solid.SetPomRun(Table, Depth, Flags, InvW, InvH); }
    void  ClearPomRun() { Solid.ClearPomRun(); }
    // The relief's own numbers for the runs that follow, and with bPatches the relief made real (FSolidPass::SetReliefRun).
    void  SetReliefRun(UBOOL bPatches, const FLOAT N[3], const FLOAT AxisU[3], const FLOAT AxisV[3], FLOAT RateU,
                       FLOAT RateV, FLOAT Edge, FLOAT Focal, FLOAT CentreX, FLOAT CentreY, FLOAT LampStrength)
    {
        Solid.SetReliefRun(bPatches, N, AxisU, AxisV, RateU, RateV, Edge, Focal, CentreX, CentreY, LampStrength);
        if (bReliefMirror)
            Solid.SetReliefMirror(ReliefMirror);
    }
    // The relief runs that follow are a mirror's view's: its plane in that view's space (FSolidPass::SetReliefMirror) -
    // or null, and they are the world's view's again.
    void  SetReliefMirrorPlane(const FLOAT* Plane)
    {
        bReliefMirror = Plane ? 1 : 0;
        for (INT i = 0; i < 4 && Plane; i++)
            ReliefMirror[i] = Plane[i];
    }
    void  ClearReliefRun() { Solid.ClearReliefRun(); }
    // The models rounded: the mesh triangles that follow as patches (FSolidPass::SetMeshRun).
    void  SetMeshRun(FLOAT Sag, FLOAT Focal, FLOAT CentreX, FLOAT CentreY, FLOAT Most, FLOAT Shape)
    {
        Solid.SetMeshRun(Sag, Focal, CentreX, CentreY, Most, Shape);
    }
    void  ClearMeshRun() { Solid.ClearMeshRun(); }
    // And this frame's room for their edges' terms (FSolidPass::MeshEdgeRoom).
    FLOAT* MeshEdgeRoom(INT Count, INT& First) { return Solid.MeshEdgeRoom(Count, First); }
    UBOOL CanRoundMeshes(INT Mode) const { return Solid.CanRoundMeshes(Mode); }
    // LampShadows: the lamps the relief is lit by this frame - Count of eight floats each, as the shader reads them: the
    // place in view space and the reach, then the colour and a spare (RdxMath::ReliefLampMax at most).
    void  SetReliefLamps(const FLOAT* Lamps, INT Count);
    // Geometry=Tessellation: the held edges near the relief's polygons, for its rim band - Count of them, each its two
    // ends in view space - and the spherifier's rows where the patches are rounded (FSolidPass::SetReliefHeld). The
    // block they went into, for the detail texture's run over the same patches (SetReliefBlock); zero if they did not.
    UINT64 SetReliefHeld(const FLOAT* Ends, INT Count, const FLOAT* Sphere = nullptr)
    { return (UINT64)Solid.SetReliefHeld(ReliefLampCopy, 4 * (1 + 2 * RdxMath::ReliefLampMax), Sphere, Ends, Count); }
    // The spherifier's field for the relief's patches (FSolidPass::SetSphereField), and the number it was given with.
    void SetSphereField(const DWORD* Words, INT Count, INT Gen) { Solid.SetSphereField(Words, Count, Gen); }
    void TakeSphereField(DWORD* Words, INT Count, INT Gen) { Solid.TakeSphereField(Words, Count, Gen); }
    INT  SphereFieldGen() const { return Solid.SphereFieldGen(); }
    void  SetReliefBlock(UINT64 Block) { Solid.SetReliefBlock((D3D12_GPU_VIRTUAL_ADDRESS)Block); }
    void  SetReliefBevel(FLOAT Depths) { Solid.SetReliefBevel(Depths); }
    void  SetReliefSmooth(FLOAT Levels) { Solid.SetReliefSmooth(Levels); }
    UBOOL CanTessellate() const { return Solid.CanTessellate(); }
    // The relief's covers (ReliefCover), wanted while the relief is drawn raised. They are copied from the depth buffer,
    // which has to be one that can be copied - made so by building the targets again, between frames, as for the water's
    // copies. False if that failed.
    UBOOL SetReliefCoverWanted(UBOOL bWant);
    // A measurement only (SecondRevision-relief-nocover.txt): the relief lifted through everything, as before the covers.
    void  SetReliefCoverOff(UBOOL bOff) { bReliefCoverOff = bOff ? 1 : 0; }
    // A measurement only (SecondRevision-relief-cover-no-modulated.txt): the level's modulated surfaces left out of the
    // covers.
    void  SetReliefCoverModulated(UBOOL bOn) { Solid.SetCoverModulated(bOn); }
    // The detail texture over the patches: its coordinate from theirs, its pipelines, and the run that draws the
    // patches' stretch of vertices again (FSolidPass::SetTessUvMap, AddRepeatRun).
    void  SetTessUvMap(FLOAT ScaleU, FLOAT ScaleV, FLOAT OffsetU, FLOAT OffsetV)
    { Solid.SetTessUvMap(ScaleU, ScaleV, OffsetU, OffsetV); }
    UBOOL CanTessellateDetail() const { return Solid.CanTessellateDetail(); }
    UBOOL CanTessellateFog() const { return Solid.CanTessellateFog(); }
    // Whether an opaque water surface can be drawn lifted this frame (FSolidPass::CanSludge).
    UBOOL CanSludge() const { return Solid.CanSludge(); }
    // And whether its mirror layer can go over it (Reflections=High and up; FSolidPass::CanSludgeMirror).
    UBOOL CanSludgeMirror() const { return Solid.CanSludgeMirror(); }
    // Whether a pane of the levels' glass can get its mirror layer (FSolidPass::CanGlassMirror).
    UBOOL CanGlassMirror() const { return Solid.CanGlassMirror(); }
    // The probe gathered from the picture (FMirrorProbe): whether the levels' glass mirrors what it holds this frame -
    // a mirror image asked for, no rays running for it, and what the probe gathers by there: the shadows' stencil of
    // surface kinds and their added target, a world drawn with one sample (with MSAA the added target is the world's
    // only once it is over). Not refused by a failure to make it.
    UBOOL IsProbeMirrorRunning() const
    {
        return RdxMath::ReflectionsHasMirror(ReflectionLevel) && !IsRtMirrorRunning() && bShadows && LightTarget &&
               AddedTarget && !MsaaOn() && !bProbeRefused;
    }
    // A pane the world's view draws, for where the probe stands (FMirrorProbe::NotePane); and where it stands, for the
    // pane's mirror layer - false while it holds nothing yet.
    void  NoteProbePane(const FLOAT* Middle, const FLOAT* Normal, FLOAT Area) { MirrorProbe.NotePane(Middle, Normal, Area); }
    UBOOL GetProbePlace(FLOAT* Centre, FLOAT& Reach) const { return MirrorProbe.GetPlace(Centre, Reach); }
    // And the pane it stands before, whose plane the own body is mirrored by (FMirrorBody): its normal and middle.
    UBOOL GetProbePane(FLOAT* Normal, FLOAT* Middle) const { return MirrorProbe.GetPane(Normal, Middle); }
    // A new level: the probe empty, standing nowhere.
    void  ForgetMirrorProbe() { MirrorProbe.Forget(); }
    // Whether the level's own mirrors can be traced in the engine's place (FSolidPass::CanRtMirror): the mirror rays
    // run, and the layer that carries their image is there.
    UBOOL CanRtMirror() const { return IsRtMirrorRunning() && Solid.CanRtMirror(); }
    // What is drawn next is seen through a traced mirror (FSolidPass::SetInMirror).
    void  SetInMirror(UBOOL bOn) { Solid.SetInMirror(bOn); }
    // A measurement only (the marker SecondRevision-volume-depth-view.txt): the volumes' walk lays the depth it stops at
    // over the picture instead of the smoke (FVolumetrics).
    void SetVolumeDepthView(UBOOL bOn) { bVolumeDepthView = bOn ? 1 : 0; }
    // The one white texel (FTextureCache::EnsureWhite, made at every frame's start): what a pass that draws only its
    // vertex colour samples - the meshes' fog (BLEND_Fog).
    INT   GetWhiteSrv() const { return Textures.GetWhiteIndex(); }
    INT   GetSolidVertexCount() const { return Solid.GetVertexCount(); }
    void  SetTessTogether(UBOOL bOn) { Solid.SetTessTogether(bOn); }
    void  SetBlendedLast(UBOOL bOn)  { Solid.SetBlendedLast(bOn); }
    UBOOL AddRepeatRun(INT Start, INT Count, INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth)
    { return bInFrame ? Solid.AddRepeatRun(Start, Count, SrvIndex, LightSrv, Mode, bNoSmooth) : 0; }
    // One texture's own choice: whether its relief is on, and how deep it goes. Changing it writes the list back,
    // because gathering those choices is the whole point of being able to change one.
    UBOOL PomIsOn(INT Entry) const { return Pom.GetOn(Entry); }
    FLOAT PomDepthOf(INT Entry) const { return Pom.GetDepth(Entry); }
    void  PomSetOn(INT Entry, UBOOL bOn) { Pom.SetOn(Entry, bOn); Pom.WriteList(); }
    void  PomSetDepth(INT Entry, FLOAT Depth) { Pom.SetDepth(Entry, Depth); Pom.WriteList(); }
    // A mirror of the level's shows what the walk draws as the world's view shows it - the smoke and the gas, the
    // fires' flames, the waterfalls' columns - by the walk itself. What a mirror's view gathers of them is kept apart
    // from the world's (AddMirrorCloudPuff, AddMirrorFire, AddMirrorFall), and where that view ends (MirrorViewDone)
    // the same pass walks it, from the mirror's plane on and to that view's own depth (WalkMirrorView).
    // CanMirrorWalk: whether a mirror's view can be walked at all - the walk's passes not refused, the depth buffers
    // made to be read (bDepthReadable), and the world not drawn multisampled: that depth is not read where such a
    // view ends. Where it cannot be, the callers leave the game's own drawing of the flames and the falls in the
    // mirror.
    UBOOL CanMirrorWalk() const { return VolumeReady >= 0 && !VolumeOut() && bDepthReadable && !MsaaOn(); }
    UBOOL CanMirrorFlames() const { return CanMirrorWalk() && !bFireListsRefused; }
    UBOOL CanMirrorFalls() const { return CanMirrorWalk() && bWaterWanted; }
    UBOOL CanMirrorSmoke() const { return CanMirrorWalk() && Solid.GetCloudsOn(); }
    // And whether a body can be drawn into a mirror's view by the solid pass: a force field's volume is one.
    UBOOL CanMirrorBodies() const { return Solid.CanBody(); }
    // A puff of smoke for the mirror's view being drawn, in that view's space, as the walk is given the world's.
    // False where there is no memory for one more.
    UBOOL AddMirrorCloudPuff(const FCloudPuff& P);
    UBOOL AddMirrorFirePiece(const RdxMath::FFlamePiece& P);
    UBOOL AddMirrorFire(const RdxMath::FFlamePiece* Pieces, INT Count);
    INT   MirrorFlamesHeld() const { return MirrorFlameCount; }
    // A waterfall's column for the mirror's view being drawn (RdxMath::FallColumnFloats numbers, as the walk takes
    // them). False where there is no memory to keep it: the caller draws the fall's sheets then.
    UBOOL AddMirrorFall(const FLOAT* Column);
    // What was gathered for a view that did not end as a mirror's (MirrorViewDone) is not the next view's.
    void  DropMirrorGathered();
    // The constants of the water runs that follow (FSolidPass::SetWaterConstants).
    void  SetWaterRun(const FLOAT* C) { Solid.SetWaterConstants(C); }
    // The caustics (Water=High): the net of light the waves throw, laid on whatever the world shows in a
    // see-through liquid - the bed, a sunk crate, a wall - before the water itself is drawn, so the water shows it bent
    // as it shows the bed, and a diver sees it on the bed itself. CanRun: its passes and its mask were not refused.
    void  SetCaustics(UBOOL bOn) { bCausticsWanted = bOn ? 1 : 0; }
    UBOOL CausticsCanRun() const { return (bCausticsFailed || Refused(CausticRefusal)) ? 0 : 1; }
    // A measurement only: the net from waves that keep to two directions, which fold the light into a lattice.
    void  SetCausticOldNet(UBOOL bOn) { bCausticOldNet = bOn ? 1 : 0; }
    // The view the frame's world is drawn in - the camera's place and its three axes on the level, the engine's
    // projection, the screen's centre in the engine's pixels (as SetSplashView) - the level's see-through water
    // surfaces near it (Count of RdxMath::CausticBoxFloats numbers), and what tells which pixels lie in a liquid: the
    // liquids' borders as triangles (Corners of RdxMath::CausticPortalFloats numbers) and the camera's own water
    // plus one (WaterProbe.h, RdxWaterPortals). Said once a frame, from the world's view; a frame that does not say it
    // has no caustics laid on.
    void  SetCausticView(const FLOAT* Origin, const FLOAT* Axes, FLOAT Focal, FLOAT CentreX, FLOAT CentreY,
                         const FLOAT* Boxes, INT Count, const FLOAT* Borders, INT Corners, INT CamWet);
    // Whether this frame's picture gets them: asked for, the view said, a water near enough - and the camera in a
    // liquid, or a liquid's border before it.
    UBOOL CausticsNow() const
    {
        return (bCausticsWanted && bWaterWanted && bCausticView && CausticBoxCount > 0 &&
                (CausticCamWet > 0 || CausticCorners > 0)) ? 1 : 0;
    }
    // The ripples (StepRipples): where the camera is, so the field stays around it, and what pushes the water this
    // frame - Count pulses of four: world X, Y, radius in world units, push in world units of height. Pulses beyond
    // RdxMath::RippleMaxPulses wait for no one: they are dropped.
    void  SetRipples(FLOAT CameraX, FLOAT CameraY, const FLOAT* Pulses, INT Count);
    // The field's lower corner the water reads now, in the world - far away while there is no field, so no pixel finds it.
    void  GetRippleOrigin(FLOAT& X, FLOAT& Y) const;
    // The water texture's own wave field (WaterProbe.h, RdxWaterSheet): W x H heights, 128 the still surface. Uploaded at
    // the next frame's start; the water reads it through the fourth view of its set.
    void  SetWaterWaves(const BYTE* Heights, INT W, INT H);
    // The lamps that glint on the water this frame (Reflections=High and up, RdxWaterLamps): Count of eight numbers -
    // the lamp's place from the camera, its reach, its colour and a zero. Written straight into this frame slot's
    // constants, which the water's pixel shader reads (FSolidPass::SetWaterLamps) - every frame with water, so no slot
    // keeps an older frame's.
    void  SetWaterLamps(const FLOAT* Lamps, INT Count);
    // The water's splashes: things falling into the water this frame (RdxWaterSplashes, five numbers each - where they went
    // in, their size, how fast they fell), each starting a splash that lasts RdxMath::SplashLife; at most
    // RdxMath::SplashMax at a time, the oldest giving way. Column scales how fast its column rises from the middle - its
    // height by the square of it - and leaves the crown and the spray as they are (a waterfall's foot: a lower column). And
    // the view they are drawn in, once a frame: the camera's place and its three axes in the world, the engine's
    // projection, and the screen's centre in the engine's pixels.
    void  AddSplashes(const FLOAT* Falls, INT Count, FLOAT Column = 1.0f);
    void  SetSplashView(const FLOAT* Origin, const FLOAT* Axes, FLOAT Focal, FLOAT CentreX, FLOAT CentreY);
    // The ornamental fountains whose water falls as drops this frame (RdxWaterFountains: RdxMath::CurtainFloats numbers
    // each, at most RdxMath::CurtainMax). The splashes' own passes draw them, with the splashes (DrawSplashes).
    void  SetCurtains(const FLOAT* List, INT Count);
    // Whether those passes drew the fountains' water a moment ago - or would have, had one been in view: then the
    // level's own sheet is left out where a fountain's drops fall. Not yet, or no longer, and the sheet is drawn.
    UBOOL CurtainsDrawn() const;
    INT   GetAmbientOcclusion() const { return (INT)AoMode; }
    UBOOL GetAoAlone() const { return bAoAlone; }
    // The occlusion's passes could not be built on this device, or its targets at these sizes (DrawAmbientOcclusion):
    // it is switched on and not running.
    UBOOL GetAoFailed() const { return AoReady < 0 || Refused(AoRefusal); }
    // A measurement only (the markers SecondRevision-ao-radius.txt and -ao-strength.txt): the occlusion's radius and
    // strength for this run, to compare values on one scene without a rebuild. Zero keeps RdxMath's.
    void  RequestAoTuning(FLOAT Radius, FLOAT Strength) { AoRadiusSet = Radius; AoStrengthSet = Strength; }
    // The window has been moved to another monitor: the display is asked again what it is. Nothing is rebuilt -
    // the output mode does not depend on the display, and the HDR transform reads the answer every frame.
    void OnDisplayChanged(HWND Window);
    // The peak brightness the display claims, in nits, and 0 when it says nothing. It sets how far the highlights
    // are allowed to go on a real HDR display (DrawFinal), but only within a clamp: panels routinely claim a
    // number they cannot reach, and a tone curve built on a lie is worse than one built on a fixed assumption.
    FLOAT GetDisplayNits() const { return DisplayNits; }
    // The false-colour view of where the picture sits against paper white (RdxMath::EHdrBand): blue below it,
    // green to twice, yellow to four times, red beyond. The one debug tool that works on a display with no HDR
    // at all - and the answer to "are the walls staying put and only the lamps going up".
    void  SetHdrFalseColour(UBOOL bOn) { bHdrFalseColour = bOn ? 1 : 0; }
    UBOOL GetHdrFalseColour() const { return bHdrFalseColour; }
    // Whether the scene has a target apart from the output's - supersampling or upscaling - with the HUD going on its own.
    // The output target is what the HUD is drawn onto once the world is finished. A post filter needs it too: it
    // reads the finished world and writes somewhere else, and that somewhere is this.
    UBOOL HasOutputTarget() const { return SceneScale > 1 || UpscaleMode != 0 || PostAaKind != 0; }

    // G-buffer, first step (docs/terv-latvany-roadmap.md 1.4): a diagnostic view instead of the picture - 1 the scene's
    // depth, 2 the surface normals computed from it, 0 off (the normal path, untouched). Asked before Init, like the
    // debug layer: only then is the depth buffer created readable.
    // 3: the shadows' factor (RequestShadowQuality), white = unshadowed. 4: the light each surface receives (the
    // geometry pass's second target). 5: the shadows' share map - the factor as if every light were blocked everywhere.
    void RequestDepthView(INT Mode) { DepthViewMode = (Mode >= 1 && Mode <= 7) ? Mode : 0; }
    // The shadows' factor view (3) switched while the game runs - the renderer menu's SHADOW VIEW. With the shadows on the
    // depth is readable anyway (NeedsDepthRead), so nothing has to be made again. Off leaves another view a marker asked
    // for as it is.
    void  SetShadowView(UBOOL bOn) { DepthViewMode = bOn ? 3 : ((DepthViewMode == 3) ? 0 : DepthViewMode); }
    UBOOL IsShadowView() const     { return DepthViewMode == 3; }
    // The view's half-angle tangents, every frame - the normal view rebuilds positions with them.
    void SetViewTangents(FLOAT TanX, FLOAT TanY) { ViewTanX = TanX; ViewTanY = TanY; }
    // The view the world is drawn with, every frame (RenderDevice.cpp, at the first BSP surface of a node without a
    // parent): Eye - the camera's place in the world, three floats; Axes - its three axes, nine floats, the engine's
    // FCoords (X right, Y down, Z forward). The tangents come from SetViewTangents. The backend keeps the previous
    // frame's as well: a temporal upscaler (FSR 3.1) needs motion vectors, and for everything that does not move on its
    // own those follow from the depth and these two cameras (docs/terv-latvany-roadmap.md 1.6 and 2.3).
    void SetViewCamera(const FLOAT* Eye, const FLOAT* Axes);
    // Motion vectors on (a marker for now, later whatever needs them): one pass a frame from the world's depth and the
    // two cameras, into a target of the scene's size. Asked before Init - the world's depth has to survive the frame.
    // The diagnostic view's own request (the marker): it draws the vectors whatever the upscaler is doing.
    void RequestMotionVectors(UBOOL bOn) { bMotionWanted = bOn ? 1 : 0; bMotionForced = bOn ? 1 : 0; }
    // Whether the motion vectors are drawn at all. The engine side asks before it does the bookkeeping the
    // per-object vectors need: without upscaling nobody reads those vectors, and the work would be thrown away.
    UBOOL MotionWanted() const { return bMotionWanted; }
    // A self-test for the motion vectors (markers, measurement only), because a picture taken while the camera stands
    // still is all zeroes by rights. Mode 1: the pass counts with a previous camera turned one degree, the depth read and
    // everything else as usual - what it draws must then be a turn's picture, and the sky (no depth) must stay zero.
    // Mode 2: the shader writes a fixed ten pixels without reading anything - that checks the writing and the view alone.
    // Mode 3: the shader writes the depth it read, a thousand times over - that tells a bad depth read from a camera
    // that simply did not move.
    void RequestMotionProbe(INT Mode) { MotionProbeMode = (Mode >= 1 && Mode <= 3) ? Mode : 0; }
    // The jitter a temporal upscaler needs (a marker for now, later the FSR 3.1 path itself): the geometry pass shifts
    // the picture by a fraction of a pixel each frame, along the Halton sequence (RdxMath::FsrJitterOffset), so the
    // upscaler gets a new sample position every frame. Off, nothing is shifted.
    void RequestJitter(UBOOL bOn) { bJitterWanted = bOn ? 1 : 0; }
    // FSR 3.1 itself (docs/terv-latvany-roadmap.md 2.3): with an upscaling mode on, the temporal upscaler fills
    // the output target. It cannot work without the jitter and the motion vectors, so Init
    // turns those on for it whatever the markers said - and the vectors decide the depth buffer's format, which
    // is why that happens there and not here. If the context cannot be built the frame falls back to the bilinear pass.
    void RequestFsr3(UBOOL bOn) { bFsr3Wanted = bOn ? 1 : 0; }

    // The next frame throws away what the upscaler has collected. The camera jumping is not movement: after a level
    // load the previous picture belongs to another world, and moved by this frame's motion vectors it would smear
    // across a frame or two. The engine's Flush (a level change) asks for it - see URevisionRenderDevice::Flush.
    void RequestFsr3Reset() { bFsr3Reset = 1; }

    // A body that moves BY ITSELF, for the motion vectors. RenderDevice.cpp fills these in before EndFrame; without
    // them the upscaler sees only the camera's movement. The list is the frame's own: a frame nobody fills it in has
    // none.
    //
    // Its triangles are the shadow maps' caster of this frame (Caster) where it is one, or its own in the motion buffer
    // (MotionVertices: First, Count) - a body no chosen light reaches, and every body while the shadows are off. Where
    // it was: each vertex's own place the frame before, in the motion buffer (PrevFirst) - or, without those, the
    // body's movement as a whole: the affine 3x4 that takes a point from where the body is now to where it was, row by
    // row. MAX_MOTION_BODIES a frame at the most: one past them goes with the camera's vector alone, and the frame's
    // log line counts it.
    enum { MAX_MOTION_BODIES = 128 };
    struct FMotionBody
    {
        INT   Caster;       // -1: its own triangles
        INT   First;
        INT   Count;
        INT   PrevFirst;    // -1: Prev says where it was
        FLOAT Prev[12];
    };
    void   SetMotionBodies(const FMotionBody* List, INT Count);
    // Room in this frame's motion buffer for VertexCount places - x y z each, world units - and its first vertex there.
    // Null when the buffer is full or could not be made: the body then goes without (the camera's vector alone).
    FLOAT* MotionVertices(INT VertexCount, INT& First);
    // Whether the bodies' own vectors are drawn at all this frame: only a temporal upscaler reads them. What the
    // readout says of it, and how many bodies the last frame drew with a vector of their own. The bodies' pass against
    // the scene's depth: drawn nearer by this share of each point's depth, so that a shape which is the scene's own
    // body by another route is not left behind it - and, for a measurement, with no depth test at all. Before the first
    // frame.
    void   RequestMotionDepth(FLOAT Nearer, UBOOL bNoTest) { MotionNearer = Nearer; bMotionNoDepth = bNoTest ? 1 : 0; }
    UBOOL  MotionBodiesWanted() const { return ((bMotionWanted && UpscaleMode) || bMotionForced) ? 1 : 0; }
    INT    GetMotionBodiesDrawn() const { return MotionBodiesLast; }

    // FSR 3.1's own sharpening (the ini key Fsr3Sharpness), 0..1. Asked for at Init and kept: the dispatch reads it.
    void SetFsr3Sharpness(FLOAT S) { Fsr3Sharpness = (S < 0.0f) ? 0.0f : ((S > 1.0f) ? 1.0f : S); }

    // A run where the upscaler's context is NOT allowed to come up (the marker SecondRevision-fsr3-fail.txt): the frame must
    // fall back to the bilinear pass and the picture must stay. A safety net nobody has tried is not a safety net.
    void RequestFsr3ForceFail(UBOOL bOn) { bFsr3ForceFail = bOn ? 1 : 0; }

    // Whose temporal upscaler the FSR modes run with (the Upscaler key, RdxMath::EUpscaler; docs/terv-fsr4.md): Auto -
    // AMD's FSR DLLs through the 64-bit host (HostUpscale.h), FSR 4 where the card runs it, with the built-in FSR 3.1
    // while the host starts and wherever it cannot; Builtin - the built-in one alone; DLSS - NVIDIA's DLSS through the
    // same host, the built-in FSR 3.1 where it cannot run; XeSS - Intel's XeSS through the same host. The values are
    // RdxMath::EUpscaler's. Asked before Init, or changed in the running game between frames - a host that went down is
    // tried once more then.
    void  RequestUpscaler(INT Which) { UpscalerChoice = (Which >= 0 && Which < (INT)RdxMath::Upscaler_Count) ? Which : 0; }
    UBOOL SetUpscaler(INT Which);
    INT   GetUpscaler() const { return UpscalerChoice; }
    // For the readout: whether the host is wanted at all (an FSR mode, Upscaler=Auto, DLSS or XeSS), whether the last
    // frame's upscale went through it, whether it is still starting, and what it says - what runs, or why nothing does.
    UBOOL       HostWanted() const   { return !bSafeMode && UpscaleMode && bFsr3Wanted && UpscalerChoice != 1; }
    // ... and wanted for the rays alone (docs/terv-rt-host.md): a setting on rays asked for, whatever upscales - the
    // host is the one that traces. Not once a host that came up has said that its card traces none.
    UBOOL       HostRaysWanted() const
    {
        return !bSafeMode && !bRtHostUseless && (IsRtShadowsRequested() || IsRtAoRequested() || IsRtMirrorRequested());
    }
    // For the readout: no rays on this machine as it stands - the host that came up says its card traces none, or
    // the scene's lists could not be kept for it.
    UBOOL       RtHostCannot() const
    {
        return bSafeMode || bRtHostUseless || bRtHostRefused || (Host.IsUp() && Host.RayPasses() == 0);
    }
    UBOOL       HostUpscaled() const { return bHostUpscaled; }
    UBOOL       HostStarting() const { return Host.IsStarting(); }
    // Up now - not starting, not stopped for a change of upscaler, not down. HostUpscaled is the LAST frame's, and right
    // after such a change it still speaks of the host that was stopped.
    UBOOL       HostUp() const       { return Host.IsUp(); }
    UBOOL       HostDown() const     { return Host.IsDown(); }
    UBOOL       HostOn() const       { return Host.IsOn(); }
    UBOOL       HostPreparing() const { return Host.IsPreparing(); }
    const char* HostMessage() const  { return Host.Message(); }

    // Frame generation (the FrameGeneration key; docs/terv-fsr4.md, M4): every frame the engine draws goes out as two
    // pictures - the one the host generates between the last frame and this, then this one, half a frame later
    // (FFramePacer). It needs the host's upscale (FSR4 or DLSS) and that library's frame generation there; a frame
    // without them goes out as one picture, and FrameGenWhy says why. On, the swap chain lives on the pacer's queue
    // whether a frame generates or not; off, everything presents as it always has. Asked before Init, or changed
    // between frames - the swap chain is then made again on the other queue.
    void  RequestFrameGen(UBOOL bOn) { bFrameGenWanted = bOn ? 1 : 0; }
    UBOOL SetFrameGen(UBOOL bOn);
    UBOOL GetFrameGen() const     { return bFrameGenWanted; }
    UBOOL FrameGenRunning() const { return bFgLast; }       // the last frame went out as two pictures
    // Why the last frame did not generate: 0 it did, 1 it is off, 2 no presenting queue, 3 it needs the host's upscale
    // (FSR4 or DLSS), 4 the host is not up (starting, making its upscaler, down), 5 the host has none for this card.
    // The LAST frame's answer: the readout is written before the frame it is shown in decides its own.
    INT   FrameGenWhy() const     { return bFrameGenWanted ? FgWhyLast : 1; }
    // The pacer's measure of the frames' interval, in seconds (0: not yet) - the readout's generated frame rate.
    DOUBLE FrameGenInterval() const { return Pacer.Interval(); }

    // The latency's driver help (the AntiLag key): AMD Radeon Anti-Lag 2 (AntiLag2.h) where AMD's driver has it,
    // NVIDIA Reflex (ReflexLink.h) where NVIDIA's does - each asked for once, with the device. Where it is wanted the
    // game's thread is held back at each frame's end (AntiLagFrameEnd, just before the engine reads its input) until
    // the GPU is nearly through. The frame's stages are told to the driver as each asks: Anti-Lag 2 the end of the
    // frame's work and each picture's kind; Reflex the frame's start (its Sleep, every frame, on or off), the drawing's
    // start (FrameDrawStarts), the work's end and the Present.
    enum EAntiLag { AntiLag_None = 0, AntiLag_Amd = 1, AntiLag_Reflex = 2 };
    void    SetAntiLag(UBOOL bOn)
    {
        bAntiLagWanted = bOn ? 1 : 0;
        Pacer.SetAntiLag((AmdAntiLagUp() && bAntiLagWanted) ? &AntiLag : nullptr);
        RdxReflexSetOn(bAntiLagWanted);
    }
    INT     AntiLagKind() const  { return AmdAntiLagUp() ? AntiLag_Amd : (RdxReflexUp() ? AntiLag_Reflex : AntiLag_None); }
    UBOOL   AntiLagUp() const    { return AntiLagKind() != AntiLag_None; }
    HRESULT AntiLagStart() const { return AntiLagHr; }
    void    AntiLagFrameEnd()
    {
        if (AmdAntiLagUp())
            RdxAntiLag2::Update(&AntiLag, bAntiLagWanted != 0, 0);
        RdxReflexSleep();
        if (bAntiLagWanted)
            PaceThroughHost();
        PaceWake       = appSeconds();
        PaceFirst      = 0.0;
        bPaceHostFrame = 0;
    }
    // The frames through the 64-bit host. AMD's and NVIDIA's help sees the game's queue idle while it waits for the
    // host, and wakes the game a whole GPU frame before its work can start: the input is read that much too early. So
    // with the AntiLag on, a frame whose work went through the host is paced here too (PaceThroughHost): at its end the
    // game waits for the frame before's GPU end, and then until the frame just submitted is as near its own end as the
    // next frame's CPU time to its first submission - the engine reads its input then, and its work reaches the GPU
    // as the GPU comes free. The times are the recent frames': their least GPU time and their most CPU time, so the
    // wait ends early rather than late. A burst of the game's own work longer than any of theirs leaves the GPU
    // waiting for that frame - the price of the input's age; a window long enough to hold the rare bursts would wake
    // the game early on every frame. A stall (a level's load) is not a CPU time. A measurement turns it off (the
    // marker SecondRevision-pace-host-off.txt).
    void    RequestHostPacing(UBOOL bOn) { bPaceHostWanted = bOn ? 1 : 0; }
    // Before every submission of a frame's work; bHost: a hand-over to the host.
    void    NoteFrameSubmit(UBOOL bHost)
    {
        if (PaceFirst <= 0.0)
            PaceFirst = appSeconds();
        if (bHost)
            bPaceHostFrame = 1;
    }
    void    FrameDrawStarts()
    {
        RdxReflexMark(RdxReflexMark_SimulationEnd);
        RdxReflexMark(RdxReflexMark_SubmitStart);
    }
    // The input latency measurement's (the marker SecondRevision-input-latency.txt): the last Present's moment
    // (QueryPerformanceCounter as it returned - with frame generation, as the pacer took the frame), the pacer's number
    // for that frame (0 without frame generation), and the pacer's own moments for its two pictures.
    LONGLONG LastPresentQpc() const                    { return LatencyPresentAt; }
    UINT64   LastPacerJob() const                      { return LatencyJob; }
    void     SetLatencyStamps(UBOOL bOn)               { Pacer.SetStamping(bOn); bLatencyStamps = bOn ? 1 : 0; }
    UBOOL    TakePacerStamp(FFramePacer::FStamp& Out)  { return Pacer.TakeStamp(Out); }

    // Shadow maps (docs/terv-latvany-roadmap.md 3.2): the preset (RdxMath::EShadowQuality), asked before Init - Off is the
    // vanilla path, no maps and no pass. They need the world's depth like the depth view does, and go onto the world at
    // the HUD's first tile (BeginHud) - at 1x as well as at 2x. If the maps cannot be made, Init goes on without them.
    void  RequestShadowQuality(INT Quality);
    // An A/B of the presets' fields (the RenderDevice's markers): after RequestShadowQuality and before Init, one field
    // replaced - a value below 0 leaves it as the preset has it.
    void  OverrideShadowPreset(INT ActorMapSize, INT FilterTaps, INT FullResolution)
    {
        // Remembered, because a preset changed in the running game (SetShadowQuality) starts from the plain preset
        // again and would drop these - and then the player's ShadowHalfFactor would quietly stop counting.
        OverrideActorMap = ActorMapSize;
        OverrideTaps     = FilterTaps;
        OverrideFullRes  = FullResolution;
        if (!bShadows)
            return;
        if (ActorMapSize > 0)
            ShadowPreset.ActorMapSize = ActorMapSize;
        if (FilterTaps > 0)
            ShadowPreset.FilterTaps = FilterTaps;
        if (FullResolution >= 0)
            ShadowPreset.bFullResolution = FullResolution != 0;
    }
    // False as well once the shadow passes could not be made (ShadowReady -1), and while their factor has no target
    // (ShadowRefusal). Both halves matter: the engine's own blob shadow comes back, and the frames stop doing shadow
    // work - without the first the level would have no shadow of either kind. The passes' targets and pipelines stay
    // as Init made them.
    UBOOL WantsShadows() const { return bShadows && ShadowReady >= 0 && !Refused(ShadowRefusal); }
    // Asked for, and not able to run - what the menu's line and the readout mark.
    UBOOL ShadowsRefused() const { return bShadows && (ShadowReady < 0 || Refused(ShadowRefusal)); }
    // No memory for a list the shadows' casters are kept in (What): the shadows are left out as a whole, as where
    // their factor has no target - the engine's own blob shadow comes back, and they are asked for again when a size
    // or the colours change.
    void  RefuseShadows(const TCHAR* What);
    // The preset in effect: all zero with the shadows off.
    const RdxMath::FShadowPreset& GetShadowPreset() const { return ShadowPreset; }
    // A light's share absolute (brightness * falloff), instead of its share of the pixel's own light (the geometry pass's
    // light target) - to compare the two, with a marker. Asked before Init.
    void  RequestShadowAbsolute(UBOOL bOn) { bShadowAbsolute = bOn ? 1 : 0; }
    // Diagnostic, marker only: how many (tile, light) pairs would a tiled light list keep - see FillShadowConstants.
    // It clears the "already ran" flag too, and the RenderDevice calls it on EVERY Init, with 0 as well: that way the
    // state is deterministic, and an uninitialised member can never start the probe in a real game.
    void  RequestTileProbe(UBOOL bOn) { bTileProbe = bOn ? 1 : 0; bTileProbeDone = 0; TileProbeFrames = 0; }
    // The level's own geometry casts shadows too - a wall, a built-in desk, a bench. Only actors and movers cast without it, because the level's shadows are in its
    // light map already; with it the two add up where the map has one, which is accepted knowingly - the strength is
    // turned down globally at the end instead. On by default; a marker turns it off for the A/B. Any time.
    void  RequestShadowWorldCast(UBOOL bOn) { bShadowWorldCast = bOn ? 1 : 0; }
    // How much of the picture's light the shadows took, 0..1 - 1.0 means they took nothing, 0.75 that a quarter is gone.
    // It is the average of the shadow factor target, copied back every couple of seconds and averaged on the CPU: the
    // measurement is of what the SHADOWS took, not of how dark the picture is - a room that is dark because nothing lights
    // it reads 1.0, so no compensation starts there. -1 until the first sample.
    FLOAT GetShadowMetric() const { return ShadowMetric; }
    // The same, but weighted by how much light each pixel receives: what share of the scene's LIGHT the shadows took,
    // rather than what share of its SURFACE. Measured: on the same scenes the unweighted number said 15% and
    // 8.5% while the finished picture had lost 24.5% and 4.3% - a shadow on an already dark floor counts fully in the
    // unweighted number and hardly at all in the picture. The compensation is to be driven by this one; both are on
    // screen while it is being checked. -1 before the first sample.
    FLOAT GetShadowMetricLit() const { return ShadowMetricLit; }
    // The light-weighted number costs eight times what the area one does: the light target is scene-sized (four times the
    // pixels with 2x supersampling) and R16_FLOAT, so its copy is 66 MB against the factor's 8.3 MB, and it takes that
    // much more address space in this 32-bit process. Measured on two scenes the two agree within 1-2.5
    // percentage points (40.6 vs 39.5, 8.4 vs 6.0), so the area number drives the compensation and this one is kept for
    // checking, behind a marker. Asked any time.
    void  RequestShadowMetricLit(UBOOL bOn) { bMetricLit = bOn ? 1 : 0; }
    // Whether a caption is on screen now (the readout gives way to any other).
    UBOOL HasCaption() const { return CaptionText[0][0] != 0 && appSeconds() < CaptionUntil; }
    // The renderer's menu, its own block on the LEFT: the caption's is on the right, so the two are on screen together
    // and the menu no longer has to carry the readout in its own lines.
    void  ShowMenuPanel(const char* Text, FLOAT Seconds);
    UBOOL HasMenuPanel() const { return MenuText[0][0] != 0 && appSeconds() < MenuUntil; }
    // A notice of its own, above the caption's place: what the player has to be told whether or not the readout is on
    // - and the caption cannot carry it, because the menu and the readout rewrite that every frame. It goes away by
    // itself, or at the first key the player presses (HideNotice).
    // The frame rate limit and vsync (FPSLimit and VSync in our own settings file). The SETTING is what was asked for
    // (RdxMath::FpsLimit_Off, _Auto, or a number); the TARGET is what runs, once the screen has had its say. The
    // refresh rate comes from the monitor the window is on, and is taken again whenever the window changes screens.
    void  SetFrameLimit(INT Setting, UBOOL bVsyncOn);
    void  SetDisplayRefresh(INT Hz);
    void  SetVrrState(INT State);          // RdxMath::EVrr, from the graphics driver (RdxAskVrr)
    INT   GetVrrState() const { return VrrState; }
    INT   GetFpsSetting() const { return FpsSetting; }
    INT   GetFpsTarget() const { return FpsTarget; }
    INT   GetRefreshHz() const { return RefreshHz; }
    UBOOL GetVsync() const { return bVsync; }
    // Whether tearing presents are allowed at all - an OS and driver matter, true on every modern machine, and NOT a
    // statement about the screen. The screen's variable refresh is GetVrrState.
    UBOOL GetTearingAllowed() const { return bAllowTearing; }
    FLOAT GetFps() const { return (FrameSeconds > 0.0) ? (FLOAT)(1.0 / FrameSeconds) : 0.0f; }

    void  ShowNotice(const char* Text, FLOAT Seconds);
    void  HideNotice() { NoticeText[0][0] = 0; }
    UBOOL HasNotice() const { return NoticeText[0][0] != 0 && appSeconds() < NoticeUntil; }
    // A measurement only, before Init: see FSolidPass::SetAddedTranslucentOff.
    void  RequestAddedTranslucentOff(UBOOL bOff) { Solid.SetAddedTranslucentOff(bOff); }
    // A measurement only: see FSolidPass::SetWaterMarkOff.
    void  RequestWaterMarkOff(UBOOL bOff) { Solid.SetWaterMarkOff(bOff); }
    // A measurement only: see FSolidPass::SetModeStats.
    void  RequestModeStats(UBOOL bOn) { Solid.SetModeStats(bOn); }
    // A diagnostic, marker only: a lost device told in full (Init turns DRED on, NoteDeviceLost reads it) - which
    // operation the GPU was on, the solid pass's run markers around it, a page fault's address. It costs every command
    // list its breadcrumbs, so never by default. Before Init.
    void  RequestDred(UBOOL bOn) { bDredWanted = bOn ? 1 : 0; Solid.SetDredMarkers(bDredWanted); }
    // A measurement only, before Init: the swap chain's buffers, two or BACKEND_FRAME_COUNT (see there).
    void  RequestSwapBuffers(UINT Count) { SwapBuffers = (Count >= (UINT)BACKEND_FRAME_COUNT) ? (UINT)BACKEND_FRAME_COUNT : 2u; }
    // How much of the shadows' darkening is kept, 0..1: it scales the most one light may take (the factor shader's
    // MaxShare). The compensation owns it now (RdxMath::CompensateShadows) - 1.0 full, and 0 means nothing is shadowed at
    // all, which is what the menu's shadows-off state is. Any time - the next frame's constants have it.
    void  SetShadowStrength(FLOAT Strength) { ShadowStrength = Clamp(Strength, 0.0f, 1.0f); }
    FLOAT GetShadowStrength() const { return ShadowStrength; }
    // The strength that was in force when the last sample was MEASURED, not the one running now. The compensation divides
    // the measured loss by it to recover the loss at full strength - the one number easing the shadows does not move.
    // Without it the controller would be reading its own output and would walk away from the target.
    FLOAT GetShadowMetricStrength() const { return MetricSampleStrength; }
    // The lamps' strength for the compensation - see FSolidPass::SetLightBoost. 1.0 is the engine's own.
    void  SetLightBoost(FLOAT Scale) { Solid.SetLightBoost(Scale); }
    FLOAT GetLightBoost() const      { return Solid.GetLightBoost(); }

    // A short caption near the top of the screen for Seconds (upper case, digits, a few signs - RdxMath::CaptionGlyph; at
    // most RdxMath::CaptionMaxChars, 30 characters). Onto the back buffer, after the brightness curve: no brightness setting dims it, and it is in
    // no snapshot the engine takes (ReadScene).
    // bMenu: the renderer's own menu, which is drawn LOWER and to the LEFT so it does not sit on top of the
    // readout.
    void  ShowCaption(const char* Text, FLOAT Seconds);
    // With the shadows on: what kind of surface the next geometry is (FSolidPass::SetKind) - 0 BSP, 1 an actor. And
    // whether it is a sprite's picture (FSolidPass::SetSprite), which the soft particles fade near what is behind it.
    void  SetSurfaceKind(INT Kind, UBOOL bSprite = 0)
    {
        Solid.SetKind(Kind);
        Solid.SetSprite(bSprite);
    }
    // FlashlightBeam: how much of the flashlight's cone and of the own lamps' light the next geometry takes: How 0
    // none, 1 all of it and 2 a see-through surface's share in the world's view, 3 and 4 the same in a mirror's view
    // of the level - the block of that very view (FSolidPass::SetBeamLit), the one now drawn being the next to end
    // (MirrorViewDone); a mirror's view past the blocks' count takes none. And the frame's cone - its light at its
    // strongest, three numbers, or null for a frame without it. The cone is the world view's (SetCloudView): from
    // that view's eye, along its axis. Within a frame; a frame begins without one.
    // Whether the next geometry is a wet floor, and of what kind (RdxMath::EWetKind; FSolidPass::SetWet): 0 not. Within
    // a frame; whoever sets it sets it back.
    void  SetSurfaceWet(INT Kind) { Solid.SetWet(Kind); }
    // The wet floors' water on rays this frame (FSolidPass::AddWetLayer): the layer's numbers, a pane's (RenderDevice's
    // GlassRunConstants). Once a frame, where the rays run; the late reflection pass then leaves the water to it.
    void  AddWetLayer(const FLOAT* Consts)
    {
        Solid.AddWetLayer(Consts, Textures.GetWhiteIndex());
        bWetLayerNow = 1;
    }
    // The wet floors' measuring view (SecondRevision-wet-view.txt; SolidPass.hlsl's WetScreen.w): 0 none.
    void  RequestWetView(INT View) { WetShowView = (View > 0 && View < 16) ? View : 0; }
    // The lamps whose streaks the wet floors give back this frame (WetReflect.hlsl's WetLamp; RdxMath::WetLampMax at
    // most): two rows of four a lamp - its place in the world view's space and its glass's radius, its glass's colour by
    // its brightness. Within a frame; a frame begins without any.
    void  SetWetLamps(const FLOAT* Lamps, INT Count);
    // How thick the rain falls on the floors this frame (RdxFloorRainThick) and the level's clock its rings run by: the
    // puddles' rings, the late reflection pass's Rays.yz.
    void  SetWetRain(FLOAT Thick, FLOAT Clock) { WetRainNow[0] = Thick; WetRainNow[1] = Clock; }
    // The rain's cover over a level's wet floors (docs/terv-nedves-feluletek.md 8.4): W * H cells, four numbers each -
    // the open share 0..1, the height of the highest wet floor there, the lower floors' open share 0..1 and the highest
    // floor near it -, the grid's corner (X, Y) and its cell, in the level's units. Copied on the frame's own list,
    // within a frame. No cells: no cover, everywhere open.
    void  SetWetCover(const FLOAT* Cells, INT W, INT H, FLOAT X, FLOAT Y, FLOAT Cell);
    void  SetSurfaceBeam(INT How)
    {
        INT Block = 0;
        if (How == 1 || How == 2)
            Block = How;
        else if ((How == 3 || How == 4) && MirrorEndCount < (INT)FSolidPass::BEAM_MIRRORS)
            Block = 3 + 2 * MirrorEndCount + (How - 3);
        Solid.SetBeamLit(Block);
    }
    // The own lamps (Flashlight=Everybody, and the flashlight where a view is not the eye's own - RdxMath::OwnLampLight):
    // this frame's, set before EndFrame - the flashlight's first where it is one of them, then the weapon lamps
    // nearest the eye, OwnLampMost in all. Each gets a map drawn before the world (FShadowMaps::RenderSpots), and the
    // geometry pass lights by them (SolidPass.hlsl, OwnLampsAt); with no maps to draw (OwnLampsRefused) none lights.
    struct FOwnLamp
    {
        FLOAT At[3];
        FLOAT Aim[3];       // a unit vector
        FLOAT Colour[3];    // its light at its strongest, as the cone's own (RdxMath::BeamGain)
        UBOOL bEye;         // the flashlight's: the world's view takes the cone itself instead
        UBOOL bSeen;        // the eye sees it, nothing of the level between: its glare, and its ghosts with LensFlare
        FLOAT WetFloor;     // the wet floor below it, for its light off the water (RenderDevice.cpp's OwnLampWetFloor):
                            // its height's whole part and how wet it is as the fraction; NoWetFloor where there is none
    };
    static constexpr FLOAT NoWetFloor = -1.0e30f;
    void  SetOwnLamps(const FOwnLamp* Lamps, INT Count);
    // The world's view in the level for the own lamps, this frame's: its eye, then its axes - right, down, forward -
    // twelve numbers, as the engine's view coordinates have them.
    void  SetOwnWorldView(const FLOAT* Coords);
    // Whether the own lamps cannot light anything: no shadow maps to draw theirs with, or the card refused them.
    UBOOL OwnLampsRefused() const { return (!bShadows || !Shadow.IsReady() || Shadow.SpotsRefused()) ? 1 : 0; }
    // Whether a view this frame other than the eye's can show the flashlight's light - a mirror's view of the level,
    // or the mirror rays: then its own lamp is one of the own lamps. At the frame's end, once its views are drawn.
    UBOOL EyeLampWanted() const { return (MirrorEndCount > 0 || IsRtMirrorRunning()) ? 1 : 0; }
    // The flashlight's own image in a mirror or a pane of glass the world's view draws - at the place of the eye's
    // image - for the lens's ghosts (LensFlare): by how strongly the cone shines along the plane's normal and how much
    // the surface gives back (Share). Plane: in the world's view's space, the normal away from the eye. A plane once.
    void  NoteLampImage(const FLOAT* Plane, FLOAT Share);
    // Grass3D (FSolidPass::GrassBlock, AddGrassDraw): the blades' blocks and their draws, which the device fills from the
    // level's grass as its ground is drawn.
    FLOAT* GrassBlock(INT Rows, D3D12_GPU_VIRTUAL_ADDRESS& Address) { return Solid.GrassBlock(Rows, Address); }
    void   AddGrassDraw(D3D12_GPU_VIRTUAL_ADDRESS Block, INT Instances, INT Srv, INT LightSrv, UBOOL bNoSmooth,
                        UBOOL bWorldView)
    {
        Solid.AddGrassDraw(Block, Instances, Srv, LightSrv, bNoSmooth, bWorldView);
    }
    UBOOL  CanGrass() const { return Solid.CanGrass(); }
    INT    GrassDraws() const { return Solid.GrassDraws(); }
    // Trees3D (FSolidPass::AddTreeDraw): a grown plant's bark, its block in GrassBlock's room.
    void   AddTreeDraw(const D3D12_VERTEX_BUFFER_VIEW& Vb, const D3D12_INDEX_BUFFER_VIEW& Ib, INT Start, INT Count,
                       D3D12_GPU_VIRTUAL_ADDRESS Block, INT Srv, UBOOL bWorldView)
    {
        Solid.AddTreeDraw(Vb, Ib, Start, Count, Block, Srv, bWorldView);
    }
    // Trees3D (FSolidPass::AddLeafDraw): a grown plant's leaves, Count instances of the blade from First on, the
    // blade's frame or the flat blade (FrameFirst, FrameIndices); in the motion vectors' pass the frame from MotionFirst.
    void   AddLeafDraw(const D3D12_VERTEX_BUFFER_VIEW& Vb, const D3D12_INDEX_BUFFER_VIEW& Ib, INT FrameFirst,
                       INT FrameIndices, INT MotionFirst, INT MotionIndices, INT First, INT Count,
                       D3D12_GPU_VIRTUAL_ADDRESS Block, INT Srv, UBOOL bWorldView)
    {
        Solid.AddLeafDraw(Vb, Ib, FrameFirst, FrameIndices, MotionFirst, MotionIndices, First, Count, Block, Srv,
                          bWorldView);
    }
    UBOOL  CanTrees() const { return Solid.CanTrees(); }
    // Wind (FSolidPass::AddDustDraw): the motes of dust in the air, their block in GrassBlock's room. Only where the
    // world's start is marked (MarkWorldStart): the see-through runs go after the solid ones from there on.
    void   AddDustDraw(D3D12_GPU_VIRTUAL_ADDRESS Block, INT Instances, INT Srv) { Solid.AddDustDraw(Block, Instances, Srv); }
    UBOOL  CanDust() const { return bInFrame && NeedsDepthRead() && Solid.CanDust(); }
    INT    TreeDraws() const { return Solid.TreeDraws(); }
    // The plants' meshes: one buffer a level, in memory the CPU writes and the card reads, every plant's vertices and
    // indices in it. TreeMeshBuffer makes it - where to write, and its address on the card - when there is none; the
    // texture flush between levels lets it go, the card being idle then, and so does a device made again. The
    // generation counts those goings, so the device side knows to grow its plants again.
    BYTE*  TreeMeshBuffer(INT Bytes, D3D12_GPU_VIRTUAL_ADDRESS& Address);
    void   TreeMeshWritten();    // all of it written: copied onto the card's memory, where it is drawn from
    void   ReleaseTreeMesh();
    void   ReleaseTreeMeshStaging();
    void   DropTreeMesh();      // ReleaseTreeMesh while the game runs: the card done with it first
    INT    GetTreeMeshGeneration() const { return TreeMeshGeneration; }
    void  SetBeam(const FLOAT* Colour)
    {
        bBeamOn = Colour ? 1 : 0;
        for (INT i = 0; i < 3; i++)
            BeamColour[i] = Colour ? Colour[i] : 0.0f;
    }
    // Shadow maps, a frame's input (RenderDevice.cpp). The level's occluding triangles, x y z a vertex in world units -
    // for a new level, and again whenever GetShadowGeneration changes (the device made again, or the maps made again for
    // a quality that needs other maps - SetShadowQuality). Within a frame, before
    // EndFrame: the casters one by one (FShadowMaps::AddCaster), then the lights, with the camera's axes in world space
    // (X right, Y down, Z forward: nine values, axis by axis). Nothing of a frame carries over to the next one.
    INT    GetShadowGeneration() const { return ShadowGeneration; }
    // The level's triangles for the shadow maps (SetShadowMapsWorld), and then the ray tracing scene's own - the see-through
    // surfaces left out - with each of its triangles' surface (RtSurf) and each surface's colour (SurfAlbedo, SurfCount of
    // them), for the RT water, and the sky walls, for its rays alone (FRtPass::SetLevel). ThinXyz, ThinSurf: the level's
    // see-through surfaces' triangles and each one's surface, for the mirror rays alone (FRtPass::SetThin). Two calls, so
    // that the caller can make the second list of the first one's memory once the maps have it.
    UBOOL  SetShadowMapsWorld(const FLOAT* Xyz, INT VertexCount);
    void   SetShadowRtWorld(const FLOAT* RtXyz, INT RtVertexCount, const DWORD* RtSurf, const DWORD* SurfAlbedo,
                            INT SurfCount, const FLOAT* SkyXyz, INT SkyVertexCount, const FLOAT* ThinXyz,
                            INT ThinVertexCount, const DWORD* ThinSurf);
    FLOAT* AddShadowCaster(INT VertexCount, FLOAT X, FLOAT Y, FLOAT Z, FLOAT Radius, INT& Index);
    void   SetShadowLights(const FShadowLightFrame* Lights, INT Count, const FLOAT* CameraAxes, const FLOAT* CameraEye);
    // Whether a caster AddShadowCaster or AddShadowArenaCaster placed uses a mask somewhere (FShadowMaps::SetCasterMasked).
    void   SetShadowCasterMasked(INT Index, UBOOL bMasked) { Shadow.SetCasterMasked(Index, bMasked); }
    // The player's own body, not drawn in first person (FShadowMaps::SetCasterOwnBody).
    void   SetShadowCasterOwnBody(INT Index) { Shadow.SetCasterOwnBody(Index); }
    // What the mirror rays show of a caster (FShadowMaps::SetCasterRt), and a texture's place in the texture heap
    // for it, where the heap already holds the texture (FTextureCache::FindColour; -1: it does not).
    void   SetShadowCasterRt(INT Index, const FShadowMaps::FCasterRt& Rt) { Shadow.SetCasterRt(Index, Rt); }
    INT    FindTextureColour(QWORD CacheId, QWORD PaletteId, UBOOL bMasked = 0, UBOOL bSrgb = 1) const
    { return Textures.FindColour(CacheId, PaletteId, bMasked, bSrgb); }
    const FShadowMaps& GetShadowMaps() const { return Shadow; }
    // Within a frame (RenderDevice.cpp, at the world's first BSP surface): the world view's part of the output - x, y,
    // width, height, 0..1 - and its half-angle tangents (RdxMath::FrameShadowView); until then a frame's is the whole
    // output with SetViewTangents'. A view drawn later in the frame onto another part of the screen - the spy drone's
    // window, a security camera's - is left out of the shadows: SkipShadowRect, at most SHADOW_SKIP_RECTS a frame, the
    // same part once. GetShadowSkipCount: this frame's so far.
    void  SetShadowView(const FLOAT* Rect, FLOAT TanX, FLOAT TanY);
    void  SkipShadowRect(const FLOAT* Rect);
    INT   GetShadowSkipCount() const { return ShadowSkipCount; }
    // Masked casters (FShadowMaps::FindMask, AddMask, CasterUvw): -2 and null with the shadows off.
    INT    FindShadowMask(void* Key) const { return (bShadows && Shadow.IsReady()) ? Shadow.FindMask(Key) : -2; }
    INT    AddShadowMask(void* Key, const BYTE* Coverage)
    { return (bShadows && Shadow.IsReady()) ? Shadow.AddMask(Key, Coverage) : -2; }
    FLOAT* ShadowCasterUvw(INT Index) { return (bShadows && bInFrame && Shadow.IsReady()) ? Shadow.CasterUvw(Index) : nullptr; }
    // The arena of unchanging shapes (FShadowMaps::ArenaAlloc, AddArenaCaster): false with the shadows off.
    UBOOL  ShadowArenaAlloc(INT VertexCount, INT& Offset, FLOAT*& Xyz, FLOAT*& Uvw)
    {
        Offset = -1;
        Xyz    = nullptr;
        Uvw    = nullptr;
        return (bShadows && Shadow.IsReady()) ? Shadow.ArenaAlloc(VertexCount, Offset, Xyz, Uvw) : 0;
    }
    UBOOL  AddShadowArenaCaster(INT Offset, INT VertexCount, FLOAT X, FLOAT Y, FLOAT Z, FLOAT Radius, INT& Index)
    {
        Index = -1;
        return (bShadows && bInFrame && Shadow.IsReady()) ? Shadow.AddArenaCaster(Offset, VertexCount, X, Y, Z, Radius, Index)
                                                          : 0;
    }

    // Supersampling: the HUD's first tile of the frame (RdxMath::IsHudTile, docs/terv-latvany-roadmap.md 1.3).
    // From there on the frame is drawn into the output-sized target, onto the finished, box-filtered scene, at
    // the output's own resolution - the HUD exactly as at 1x, in the engine's order, blend by blend. Queued in
    // draw order (FSolidPass::AddTargetSwitch). True if this call queued it; at 1x only with the shadows on or where a
    // pass of the world's end is asked for (WorldEndWanted) - they go onto the world there, and the frame goes on in
    // the same target (EndWorldInPlace).
    UBOOL BeginHud();
    INT   GetSceneScale() const { return SceneScale; }

    // The frame's world begins with the next geometry (RenderDevice.cpp: the main view's first BSP surface). With a readable
    // depth, the depth buffer drawn into from there on is kept through the frame's later clears - see ClearDepthMidFrame.
    void  MarkWorldStart() { if (bInFrame && NeedsDepthRead()) Solid.AddWorldStart(); }

    // Diagnostic: the last EndFrame's steps, in seconds - the solid pass's flush, the final pass
    // with its barriers, Close + ExecuteCommandLists, Present, and the wait for the next frame
    // slot (or, without Present, for the GPU).
    const DOUBLE* GetEndFrameParts() const { return EndParts; }

    // Diagnostic, off unless asked for (the marker SecondRevision-frames.txt): the time of every
    // Present to SecondRevision-frames.csv - frame times by our own clock, in both presentation
    // modes alike. PresentMon misattributes frames in the composed one (docs/meres-08 7.).
    void StartFrameCapture();
    // Present answered DEVICE_REMOVED or _RESET - see bDeviceLost. Says it once and closes the capture. Where: the call
    // that met it - a resize meets it before any Present does.
    void NoteDeviceLost(HRESULT Hr, const TCHAR* Where = TEXT("Present"));
    void LogDred();
    // DRED on (RequestDred): the big resources named, so a page fault's report can tell which stood at its address.
    void NameResources();
    // WHAT IS RUNNING, written into every row of the capture from now on. It is there so that ONE run can measure
    // several settings: the rows say for themselves which one they belong to, instead of being matched up
    // afterwards against a list of times. A measurement that has to be aligned by the clock is a measurement that
    // can silently line up wrong, and this one already has been.
    void SetFrameCaptureLabel(const char* Text);

    const TCHAR* GetAdapterName() const { return AdapterName; }
    // The card's name in the caption font's alphabet, for the readout; and what came of the GPU key - 0 it asked for
    // none, 1 the card it asked for runs, 2 it named no card that can run, and the first that can does.
    const char* GetAdapterLabel() const { return AdapterLabel; }
    INT GetGpuKeyState() const { return GpuKeyState; }
    const char* GetGpuKeyLabel() const { return GpuKeyLabel; }
    // The adapter's PCI vendor id (0x1002 AMD, 0x10DE NVIDIA, 0x8086 Intel), 0 before Init: what the antialiasing's Auto
    // picks by, and what FSR4 and DLSS need (RdxMath::GpuVendorOf).
    UINT GetAdapterVendor() const { return AdapterVendor; }

private:
    UBOOL CreateDeviceAndQueue();
    // Logs what the adapter offers (feature level, shader model, DXR) - see Backend.cpp.
    void  LogCapabilities();
    UBOOL                          bRtShadows;       // see RequestRtShadows
    UBOOL                          bRtShadowSetting; // ShadowQuality=RT (RequestShadowQuality)
    UBOOL                          bRtShadowCounts;  // ... its Mode 3
    UBOOL                          bRtShadowMaskOff; // ... its Mode 4
    UBOOL                          bRtShadowBench;   // ... its Mode 5
    INT                            RtShadowOnly;     // ... its Modes 6 (1: the level only) and 7 (2: the actors only)
    INT                            RtKnobLights, RtKnobEdges;   // see RequestRtKnobs
    INT                            RtBenchFrames;    // the RT factor's frames so far, for the bench's table
    UBOOL                          bRtAo;            // see RequestRtAo
    INT                            RtAoInstances;    // ... the instances its rays meet, 0 for the level and the actors
    // The frame's rays' scene: whether its bodies were listed for the host yet (at the mirror rays' first asking or
    // with the shadows, whichever comes first - once a frame), and whether that came off. Cleared at BeginFrame.
    UBOOL                          bRtSceneTried;
    UBOOL                          bRtSceneBuilt;
    // Frames since mirror rays were last asked for a draw (UpdateRtFar), up to RT_MIRROR_IDLE_FRAMES - see
    // RtMirrorDrawnLately.
    enum { RT_MIRROR_IDLE_FRAMES = 120 };
    INT                            RtMirrorIdle;
    UBOOL                          bRtFarViews;      // the far probe's two views are in the texture heap
    INT                            RtWaterView;      // see RequestRtWaterView
    TComRef<ID3D12Device5>         Device5;          // the device as FRtPass::InitHosted takes it; null until asked
    FRtPass                        Rt;               // the rays' scene, kept for the 64-bit host (RtPass.h)
    UBOOL CreateSwapChain(HWND Window);
    UBOOL CreateFrameResources();
    // One black picture as soon as the chain can show one: until the first real picture the window is white.
    void  PresentBlack();
    // The start's caption: while the backend's own start compiles shaders the window says so - COMPILING SHADERS, how
    // many of about how many, and a bar - on its lower part, clear of the game's own logo. Shown before each shader
    // that goes through the compiler (RdxCompileWatch), from the thread that makes the backend, until its start is
    // done; a start with every shader in the cache's file never shows it.
    void  PresentCompiling();
    // How many shaders the backend's start compiles where the cache's file gave none (no file, or the cache off): every
    // one the renderer has, made by Init and WarmShaders whatever the settings - the caption's total from its first
    // moment. Where a shader comes or goes, the start's end says so in the log and this number follows it.
    enum { START_SHADERS = 170 };
    static void CompilingWatch(void* Who);
    // The wait while a shader compiles on its own thread (RdxCompileWatch's Wait): AnswerWhile on the thread that makes
    // the backend, a plain wait on any other.
    static void CompilingWait(void* Who, HANDLE Compiling);
    // Every shader the renderer may ask for, compiled while it starts (Init): see WarmShaders in Backend.cpp.
    void WarmShaders();
    // The wet floors' late pass made for the picture's and the depth's formats and the samples now - again where they
    // moved (DrawWetReflections, WarmShaders). False where it could not be.
    UBOOL EnsureWetReflect();
    DWORD CompilingThread = 0;
    UBOOL CreateDepthBuffer();
    UBOOL CreateSceneTarget();
    // Supersampling only: the output's own viewport and scissor (Width x Height) - BeginFrame's
    // cover the larger scene target.
    void  SetOutputViewport(ID3D12GraphicsCommandList* Cmd) const;
    // Supersampling only: the output-sized target the scene is box-filtered into, the HUD is drawn onto, and
    // the final pass and ReadScene read - with a depth buffer of its own for 3D that comes after the HUD's
    // first tile.
    UBOOL CreateOutputTarget();
    // The finished scene into the output target, box-filtered, without the brightness curve. Leaves that
    // target bound, with the output's viewport.
    void  ResolveScene(ID3D12GraphicsCommandList* Cmd);
    // FTargetSwitch: ResolveScene, then the output target with its depth buffer, cleared.
    virtual D3D12_CPU_DESCRIPTOR_HANDLE SwitchToOutput(ID3D12GraphicsCommandList* Cmd);
    void  ReleaseBackBuffers();
    void  WaitForGpu();          // full stall; used on resize and shutdown only
    // The frame fence's event, waited for at most TotalMs - in slices while the host is on, asking between them whether
    // it still answers: a frame of ours in the hands of a host that died would otherwise hold the queue the whole time.
    DWORD WaitFenceEvent(DWORD TotalMs);
    // The host started or stopped as the frames want it (HostWanted), and its state moved on. Once a frame.
    void  SyncHost();
    void  MoveToNextFrame();     // per-frame fence, no stall in the common case
    void  NotePresent(LONGLONG Q0, LONGLONG Q1);   // diagnostic, see EndFrame

    TComRef<IDXGIFactory4>            Factory;
    TComRef<ID3D12Device>             Device;
    ID3D12Resource*                   TreeMesh           = nullptr;   // TreeMeshBuffer's, on the card
    ID3D12Resource*                   TreeMeshStaging    = nullptr;   // the memory it is written into, then copied
    BYTE*                             TreeMeshMapped     = nullptr;
    INT                               TreeMeshBytes      = 0;         // to copy (TreeMeshWritten); 0 once done
    INT                               TreeMeshStagingFrames = 0;      // frames until the staging may go
    INT                               TreeMeshGeneration = 0;
    TComRef<ID3D12CommandQueue>       Queue;
    TComRef<IDXGISwapChain3>          SwapChain;
    TComRef<ID3D12DescriptorHeap>     RtvHeap;
    TComRef<ID3D12GraphicsCommandList> CommandList;
    TComRef<ID3D12Fence>              Fence;
    TComRef<ID3D12DescriptorHeap>     DsvHeap;
    TComRef<ID3D12Resource>           DepthBuffer;

    // See RequestDepthView. The depth buffer's float view lives in a shader-visible heap of its own; DepthViewReady
    // is 0 before the view's pipeline was tried, 1 when built, -1 when it failed (the picture is drawn then).
    INT                               DepthViewMode;
    INT                               DepthViewReady;
    FLOAT                             ViewTanX, ViewTanY;
    // See SetViewCamera: this frame's camera, and the previous frame's with its tangents. bCamSet: this frame's arrived
    // (no world drawn in menus and while loading); bPrevCamValid: the previous frame's may be used for motion vectors.
    FLOAT                             CamEye[3], CamAxes[9];
    FLOAT                             PrevEye[3], PrevAxes[9], PrevTanX, PrevTanY;
    UBOOL                             bCamSet, bPrevCamValid;
    TComRef<ID3D12DescriptorHeap>     DepthSrvHeap;     // two views: DepthBuffer, DepthBuffer2
    // With the depth view on, a second depth buffer: the engine's ClearZ within a frame moves the drawing over to the
    // other buffer instead of clearing, so the world's depth survives to the frame's end. ActiveDepth is the one drawn
    // into (0 or 1), RetiredDepth the one given up at the frame's last such ClearZ (-1: none this frame).
    TComRef<ID3D12Resource>           DepthBuffer2;
    TComRef<ID3D12DescriptorHeap>     DsvHeap2;
    INT                               ActiveDepth;
    INT                               RetiredDepth;
    // The buffer the frame's world was drawn into (MarkWorldDepth; -1 before it), and the one that holds the world's depth
    // for the frame's end: that one, or without the mark the last given up, or the one drawn into.
    INT                               WorldDepth;
    INT WorldDepthIndex() const { return (WorldDepth >= 0) ? WorldDepth : ((RetiredDepth >= 0) ? RetiredDepth : ActiveDepth); }
    // What the depth buffers were made as (CreateDepthBuffer): whether they can be read - typeless, the second one
    // beside the first, their shader views. bDepthReadRefused: making them readable failed (EnsureDepthReadable).
    UBOOL                             bDepthReadable    = 0;
    UBOOL                             bDepthReadRefused = 0;
    UBOOL DrawDepthView(ID3D12GraphicsCommandList* Cmd);
    // The world's depth has to survive the frame (typeless depth buffers, the second one, the ClearZ switch) - or be copied
    // (the water's copies, the relief's covers, the depth the volumes' walk stops at). This is the WISH: what the
    // buffers were made as is bDepthReadable, and a wish that comes after they were made gets them made again between
    // frames (EnsureDepthReadable).
    UBOOL NeedsDepthRead() const
    {
        return DepthViewMode || bShadows || bMotionWanted || bWaterWanted || ReflectionLevel > 0 ||
               bReliefCoverWanted || Solid.GetSoftFade() > 0.0f || WalkWanted() || FogRoomsWanted() || bFieldWanted ||
               bHoloShellWanted;
    }
    // With the shadows on the depth buffers carry the surface kinds' stencil (FSolidPass::SetKind): cleared with it.
    D3D12_CLEAR_FLAGS DepthClearFlags() const
    { return bShadows ? (D3D12_CLEAR_FLAGS)(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL) : D3D12_CLEAR_FLAG_DEPTH; }

    // See RequestShadowQuality. ShadowSrvHeap: two tables of six, the world's depth first - 0 depth 1, 1 depth 2,
    // 2 stencil 1, 3 light, 4 the level's cubes, 5 the actors' cubes | 6 depth 2, 7 depth 1, 8 stencil 2, 9 light, 10 and
    // 11 the cubes - 12 the factor target, 13 the added target (CreateLightTarget), 14 the shadow maps' masks
    // (WriteShadowCubeViews). ShadowReady like DepthViewReady.
    // Then two runs of ten for the see-through water (WriteWaterShadowViews), the world's depth first in each as in the
    // tables: the factor, the added target, the water's own depth, the other depth, a stencil of nothing, the water's
    // shade (WaterShade), the two cube arrays, the water surface's factor (WaterFactor), the world's depth - the apply
    // pass reads a run whole. Then the water surface factor's own table of the factor's six: the water's pack
    // (WaterPack) for the world's depth, no depth after the ClearZ, no surface kind but the level's, the pack's light
    // for the light target's, the two cube arrays. Then the pack's own two: the water's own depth and its shade.
    enum { SHADOW_TABLE = 6, SHADOW_FACTOR_SRV = 12, SHADOW_ADDED_SRV = 13, SHADOW_MASK_SRV = 14, SHADOW_WATER_RUN = 15,
           SHADOW_WATER_RUN_VIEWS = 10, SHADOW_WATER_OWN_AT = 8,
           SHADOW_WATER_FACTOR_TABLE = SHADOW_WATER_RUN + 2 * SHADOW_WATER_RUN_VIEWS,
           SHADOW_WATER_PACK_IN = SHADOW_WATER_FACTOR_TABLE + 6, SHADOW_SRVS = SHADOW_WATER_PACK_IN + 2,
           SHADOW_SKIP_RECTS = 4 };
    UBOOL                             bShadows;
    RdxMath::FShadowPreset            ShadowPreset;      // the preset asked for; all zero with the shadows off
    FShadowMaps                       Shadow;
    INT                               ShadowGeneration;  // counts Inits - see GetShadowGeneration
    FLOAT                             ShadowAxes[9];     // see SetShadowLights
    FLOAT                             ShadowEye[3];      // ... and the camera in world space, for the RT shadows' rays
    // See SetShadowView: this frame's, reset by BeginFrame.
    UBOOL                             bShadowViewSet;
    FLOAT                             ShadowViewRect[4];
    FLOAT                             ShadowTanX, ShadowTanY;
    FLOAT                             ShadowSkip[SHADOW_SKIP_RECTS * 4];
    INT                               ShadowSkipCount;
    void  WriteShadowCubeViews();
    INT   ShadowFactorDivisor() const { return ShadowPreset.bFullResolution ? 1 : 2; }
    // bRt: for the RT factor, which takes every light of the pick (no maps to wait for). Order, Ordered: which light each
    // slot of the constants' arrays is - FRtPass::FillShadowExclusion writes its own in the same order.
    void  FillShadowConstants(BYTE* Out, UBOOL bRt, INT* Order, INT& Ordered) const;
    UBOOL                             bShadowAbsolute;   // see RequestShadowAbsolute
    mutable UBOOL                     bTileProbe;        // see RequestTileProbe
    mutable UBOOL                     bTileProbeDone;    // the probe runs ONCE, not every frame
    mutable INT                       TileProbeFrames;   // and not on the FIRST frame - see FillShadowConstants
    UBOOL                             bShadowWorldCast;  // see RequestShadowWorldCast
    // The shadow metric - see GetShadowMetric. The factor target is copied to a readback buffer every couple of seconds
    // and read a few frames later, so the GPU is never waited on: the copy is recorded into the frame's own list, and the
    // buffer is only mapped once that frame's fence has passed.
    TComRef<ID3D12Resource>           MetricReadback;
    UINT64                            MetricBytes;
    UINT                              MetricW, MetricH, MetricPitch;
    INT                               MetricPending;     // the frame slot whose copy is in flight, -1 none
    UINT64                            MetricFence;       // the fence value that slot's frame was submitted with
    FLOAT                             ShadowMetric;      // the last average, -1 before the first sample
    DOUBLE                            MetricNextAt;      // when the next sample may be taken
    // The light target's copy, the weight for GetShadowMetricLit. R16_FLOAT and scene-sized, so with supersampling it is
    // larger than the factor target - the coordinates are scaled when the two are read together.
    TComRef<ID3D12Resource>           MetricLight;
    UINT64                            MetricLightBytes;
    UINT                              MetricLightW, MetricLightH, MetricLightPitch;
    FLOAT                             ShadowMetricLit;
    UBOOL                             bMetricLit;        // see RequestShadowMetricLit - off by default, it is the costly one
    // Rest: the state the factor target sits in around the copy, because the copy happens where the factor has just
    // been drawn (a render target) rather than where everything reads it.
    void   SampleShadowMetric(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES Rest);
    void   CollectShadowMetric();
    FLOAT                             ShadowStrength;    // see SetShadowStrength
    FLOAT                             MetricPendingStrength;  // the strength the copy in flight was drawn at
    FLOAT                             MetricSampleStrength;   // and the one belonging to the sample already collected
    // The geometry pass's second target (FSolidPass, PSMainLit): the light each surface receives, scene-sized; and its third,
    // the added target: what the blended draws laid over the surfaces made of the picture (ApplyShadows). Their RTV heap: 0
    // the light target, 1 an output-sized one of the same format (2x only) - bound beside the output target after the scene,
    // written by the HUD's draws and never read - 2 the added target, 3 its output-sized one (2x only, the same way).
    TComRef<ID3D12Resource>           LightTarget;
    TComRef<ID3D12Resource>           OutputLightTarget;
    TComRef<ID3D12Resource>           AddedTarget;
    TComRef<ID3D12Resource>           OutputAddedTarget;
    TComRef<ID3D12DescriptorHeap>     LightRtvHeap;
    UBOOL CreateLightTarget();
    // The scene target with the depth view Dsv - and with the shadows on, the light target beside it.
    void  BindSceneTargets(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    INT                               ShadowReady;
    INT                               ShadowW, ShadowH;
    TComRef<ID3D12Resource>           ShadowColor;
    TComRef<ID3D12DescriptorHeap>     ShadowRtvHeap;
    TComRef<ID3D12DescriptorHeap>     ShadowSrvHeap;
    UBOOL EnsureShadowTarget();
    void  ApplyShadows(ID3D12GraphicsCommandList* Cmd);

    // Ambient occlusion - see RdxMath::EAmbientOcclusion. It keeps everything of its own: two targets at the shadow
    // factor's size - the world's depth copied down to it (R32, what the samples read) and the raw occlusion (R8) - their
    // RTVs, and a heap of two tables of five, the world's depth first: depth 1, depth 2, small depth, raw, RT factor |
    // depth 2, depth 1, small depth, raw, RT factor. Each pass binds as much of a table as it reads, so no target is ever
    // in the table of the pass that draws into it. Nothing is added to the shadow heap, so the shadows' descriptor layout
    // stays exactly as it was; all the occlusion touches of theirs is the factor target, and only through a blend.
    // The fifth, the RT shadows' own: the factor the rays gave, before it is smoothed into the factor target
    // (FFinalPass::DrawRtFactorBlur) - it lives here because the smoothing reads what the occlusion's passes read.
    enum { AO_TABLE = 6, AO_SRVS = 12, AO_SMALL_DEPTH_SRV = 2, AO_RAW_SRV = 3, AO_RT_FACTOR_SRV = 4, AO_STENCIL_SRV = 5 };
    RdxMath::EAmbientOcclusion        AoMode;
    UBOOL                             bAoAlone;
    FLOAT                             AoRadiusSet, AoStrengthSet;    // RequestAoTuning; zero = RdxMath's
    INT                               AoReady;     // as ShadowReady: 0 not tried yet, 1 up, -1 failed for this device
    INT                               AoW, AoH;
    TComRef<ID3D12Resource>           AoRaw;
    TComRef<ID3D12Resource>           AoDepth;     // the world's depth at the factor's size (GFinalAoDepthShader)
    TComRef<ID3D12Resource>           RtFactorRaw; // the RT factor before its smoothing (R8, the factor's size)
    INT                               RtFactorBlurReady;   // its pipeline: 0 not tried yet, 1 up, -1 failed
    TComRef<ID3D12DescriptorHeap>     AoRtvHeap;   // three RTVs: the raw target, the small depth, the RT factor's
    TComRef<ID3D12DescriptorHeap>     AoSrvHeap;
    UBOOL EnsureAoTarget();
    void  WriteAoDepthViews();
    void  FillAoConstants(UINT* Out) const;
    // bFromHost: the host traced this frame's occlusion (ApplyShadows) - the raw pass is copied from what it wrote,
    // not drawn.
    void  DrawAmbientOcclusion(ID3D12GraphicsCommandList* Cmd, INT World, UBOOL bFromHost);
    // GPU time per pass, from timestamp queries. Points:
    // 0 the frame's start, 1 the shadows' start, 2 the maps drawn, 3 the factor drawn, 4 the shadows applied, 5 the frame's
    // end - a set of six for each of the BACKEND_FRAME_COUNT command allocators. A set is read back when its allocator is
    // reset (the GPU is done with that frame by then), summed, and logged every GPU_TIME_LOG_FRAMES frames. The ticks'
    // unit comes from each log window: the GPU's frame-to-frame time against the CPU's, to the nearest power of ten -
    // a tenth of the performance counter's tick on this card.
    // 6-7: the motion vector pass, 8-9: the upscaler's own pass. Both are timed directly, so "what does FSR cost" is
    // answered by a measurement rather than by subtracting two totals.
    // 10-11: the ambient occlusion, between the factor and the apply. Both are placed in every shadowed frame, occlusion
    // or not, and the apply is timed from 11 - so with the occlusion off the two stand side by side, it costs nothing,
    // and the apply's time means exactly what it meant before there was an occlusion to leave out of it.
    //
    // WHAT THESE NUMBERS CAN AND CANNOT SETTLE. The frame time on this machine moves between two states - long steady
    // stretches near 9 ms and blocks near 14 - with the SAME work in both: identical draw counts, casters, lights and
    // world-map redraws. That is the machine, not the renderer, and a line every 30 frames is dense enough to see it.
    //
    // Averaging harder does not help, which is the part worth knowing before anyone tunes this. Cut into windows, the
    // median's spread is 1.85 ms at 5 samples a window, 1.86 at 30 and 1.64 at 100 - flat, where sampling noise alone
    // would fall as 1/sqrt(N). A state lasts longer than even a 100-sample window, so more points inside a block do
    // not bring in more blocks; only a longer RUN does. Keep 30 for the detail, and do not read a 1-2 ms difference
    // between two modes out of these lines.
    // 26: through the 64-bit host (HostUpscale.h), between 8 and 9 - the copies into its textures are done.
    // 27-28: the depth of field. 29-30: the fog in its rooms.
    // 31-34: the plants' motion vectors, inside 6-7 (FSolidPass::DrawPlantMotion, a kind at a time): their start, the
    // grass done, the bark done, the leaves done.
    // From GPU_TIME_ROUND_FIRST on: four points for each of a frame's hand-overs of the rays' list to the 64-bit
    // host (GPU_TIME_ROUNDS of them; the ones past them go untimed) - the asking draws begin, they are done, the
    // copies for the host are done, its answers are there (RoundPoint).
    enum { GPU_TIME_ROUNDS = 7, GPU_TIME_ROUND_FIRST = 35, GPU_TIME_ROUND_PARTS = 3,
           GPU_TIME_POINTS = GPU_TIME_ROUND_FIRST + 4 * GPU_TIME_ROUNDS, GPU_TIME_LOG_FRAMES = 30 };
    TComRef<ID3D12QueryHeap>          TimeHeap;
    TComRef<ID3D12Resource>           TimeReadback;
    UINT64*                           TimeMapped;
    UINT64                            TimePlaced[BACKEND_FRAME_COUNT];   // bit p: point p was ended in its place
    UBOOL                             TimeLive[BACKEND_FRAME_COUNT];     // the set holds a resolved frame not read yet
    // maps, factor, apply, shadows, frame, motion vectors, upscaler, ambient occlusion, the water's copies, the ripples,
    // ..., and through the host: its copies, and its side (the crossing, its work, the draw back); the depth of field;
    // the fog in its rooms
    enum { GPU_TIME_PARTS = 22 };
    DOUBLE                            TimeSum[GPU_TIME_PARTS];
    DOUBLE                            TimeMax[GPU_TIME_PARTS];
    INT                               TimeCount[GPU_TIME_PARTS];
    // The rays' list: which of the frame's hand-overs is being recorded; and, a frame's hand-overs together, the
    // asking draws, the copies for the host, and the wait for its answers - the crossing both ways and its tracing.
    INT                               TimeRound;
    DOUBLE                            RoundSum[GPU_TIME_ROUND_PARTS];
    DOUBLE                            RoundMax[GPU_TIME_ROUND_PARTS];
    INT                               RoundFrames;
    INT                               RoundCount;
    UINT                              ListTimedLast;                     // FHost64Link::WorkListTimed at the last line
    UINT64                            TimeLastStart;
    DOUBLE                            TimeGapSum;
    INT                               TimeGaps;
    DOUBLE                            TimeCpuLast;
    DOUBLE                            TimeCpuGapSum;
    INT                               TimeCpuGaps;
    INT                               TimeFrames;
    DOUBLE                            TimeTick;                          // seconds a tick
    // The CPU's waits for the GPU within the same lines (Battery Park: a still view ran serially): the
    // upload list's full stall (SubmitUploadList) - how many and how long - and the frame slot's wait (MoveToNextFrame).
    INT                               WaitUploads   = 0;
    DOUBLE                            WaitUploadSum = 0.0;
    DOUBLE                            WaitUploadMax = 0.0;
    DOUBLE                            WaitSlotSum   = 0.0;
    DOUBLE                            WaitSlotMax   = 0.0;
    void  CreateGpuTimer();
    void  GpuTime(ID3D12GraphicsCommandList* Cmd, INT Point);
    void  ReadGpuTime(UINT Slot);
    // The n-th point of the hand-over of the rays' list being recorded; -1, which GpuTime takes for none, past the
    // hand-overs that have points.
    INT   RoundPoint(INT n) const
    {
        return (TimeRound < GPU_TIME_ROUNDS) ? GPU_TIME_ROUND_FIRST + 4 * TimeRound + n : -1;
    }
    // See ShowCaption. CaptionReady like ShadowReady; CaptionText is empty when no caption is due. One row a line, all
    // padded to the longest so the boxes stack into a block instead of a ragged column.
    char                              CaptionText[RdxMath::CaptionLines][RdxMath::CaptionLineChars + 1];
    DOUBLE                            CaptionUntil;
    char                              MenuText[RdxMath::CaptionLines][RdxMath::CaptionLineChars + 1];
    DOUBLE                            MenuUntil;
    INT                               FpsSetting;    // what the settings file asked for
    INT                               FpsTarget;     // what is enforced now, 0 = nothing
    INT                               RefreshHz;     // the monitor's refresh rate, 0 when it is not known
    INT                               VrrState;      // RdxMath::EVrr for the screen the window is on
    UBOOL                             bVsync;
    DOUBLE                            NextFrameAt;   // when the next present may leave, 0 before the first
    DOUBLE                            LastPresentAt;
    DOUBLE                            FrameSeconds;  // smoothed frame to frame time, for the readout
    // The notice (ShowNotice): three lines and a spare, in the same letters as the caption.
    enum { NoticeLines = 4 };
    char                              NoticeText[NoticeLines][RdxMath::CaptionLineChars + 1];
    DOUBLE                            NoticeUntil;
    INT                               CaptionReady;
    void  DrawCaption(ID3D12GraphicsCommandList* Cmd);
    // RightGap: 0 centres the lines; otherwise their right edge stands this far from the screen's.
    void  DrawTextLines(ID3D12GraphicsCommandList* Cmd, const char (*Block)[RdxMath::CaptionLineChars + 1],
                        INT Lines, INT TopY, INT Scale, INT RightGap);
    // FTargetSwitch: the engine's ClearZ within a frame - see DepthBuffer2.
    virtual D3D12_CPU_DESCRIPTOR_HANDLE ClearDepthMidFrame(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    // FTargetSwitch: the world begins in the buffer drawn into now - see WorldDepth.
    virtual void MarkWorldDepth();
    // FTargetSwitch: a mirror's view has drawn all it has (MirrorViewDone) - the rooms' fog over it from the mirror
    // on, then the walk of what the view gathered (WalkMirrorView).
    virtual void MirrorViewEnd(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv, INT Index);
    // FTargetSwitch: the copies the water reads, at the frame's first water run - see WaterColor.
    virtual UBOOL CopyForWater(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    // FTargetSwitch: the copies made again before a fall's sheet, where the picture was drawn on since.
    virtual UBOOL CopyForWaterAgain(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    // FTargetSwitch: the water's splashes made ready where the world ends - see FluidDepth.
    virtual UBOOL DrawSplashes(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv, const FLOAT* Frame,
                               FLOAT* Composite, UBOOL bUnderWater);
    // FTargetSwitch: the same for a mirror's view of the level (MirrorViewDone's number) - DrawSplashes with
    // that view in the world's place.
    virtual UBOOL DrawMirrorSplashes(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv,
                                     const FLOAT* Frame, INT Index, FLOAT* Composite);
    // FTargetSwitch: the water's own depth made writable for a see-through water run - see WaterSurface.
    virtual INT WaterSurfaceView(ID3D12GraphicsCommandList* Cmd, UBOOL& bWrite);
    virtual void WaterSurfaceSettle(ID3D12GraphicsCommandList* Cmd);
    // FTargetSwitch: the wet floors' mark made writable for a wet floor's run - see WetMark.
    virtual INT WetMarkView(ID3D12GraphicsCommandList* Cmd);
    UBOOL EnsureWetMark();
    // How thick the rain falls on the floors this frame and the clock its rings run by (SetWetRain).
    FLOAT WetRainNow[2] = { 0.0f, 0.0f };
    void  WetMarkTo(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES State);
    // The water's own depth made (EnsureWaterSurface), taken to a state (WaterSurfaceTo), and stamped into the world's
    // depth for the upscaler (StampWaterSurface: Dsv is the world's depth, in DEPTH_WRITE).
    UBOOL EnsureWaterSurface();
    void  WaterSurfaceTo(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES State);
    // With the shadows: the water's shade (WaterShade) and the water surface's factor (WaterFactor) made, taken to a
    // state, and their views - the runs of ShadowSrvHeap - written again once something they show was made again
    // (bWaterShadowViewsDirty).
    UBOOL EnsureWaterShade();
    void  WaterShadeTo(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES State);
    void  WaterShadeViewNone();
    UBOOL EnsureWaterFactor();
    void  WriteWaterShadowViews();
public:
    // A measurement only (the marker SecondRevision-water-shadow-off.txt): the see-through water takes the shadow of
    // what lies under it, as before it had its own - the frame time's A/B within one start.
    void  SetWaterShadowMarkOff(UBOOL bOff) { bWaterShadowMarkOff = bOff ? 1 : 0; }
private:
    void  StampWaterSurface(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    // The reactive mask made (EnsureReactive), taken to a state (ReactiveTo), and made ready for a writer (ReactiveBegin):
    // cleared on the frame's first, whoever that is, and left in State.
    UBOOL EnsureReactive();
    void  ReactiveTo(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES State);
    UBOOL ReactiveBegin(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES State);
    // FTargetSwitch: the relief's covers, before its first patches - see ReliefCover.
    virtual UBOOL BeginReliefCovers(ID3D12GraphicsCommandList* Cmd);
    virtual void  EndReliefCovers(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    virtual UBOOL BeginPaneKeep(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind, FLOAT* Size);
    virtual void  EndPaneKeep(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    virtual UBOOL BeginMirrorAsk(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    virtual UBOOL BeginPaneAsk(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind, FLOAT* Size);
    // What the two have in common. Dsv: the depth the asking draws are tested against - null: none, the panes'
    // pass has no depth. bWorld: the world's runs ask - the far probe, which their shading reads, is brought up to
    // the frame first, and without it there is no asking.
    UBOOL BeginRayAsk(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind, const D3D12_CPU_DESCRIPTOR_HANDLE* Dsv,
                      UBOOL bWorld);
    virtual UBOOL EndMirrorAsk(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv, FRtWaterBind& Bind);
    // The far probe, which the mirror layers on rays read: its views in the texture heap, and this frame's far
    // picture into it.
    UBOOL UpdateRtFar(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind);
    // What the host's rays read of the frame's own, named for FHostRayFeed: the same from wherever in the frame it is
    // asked - the share is made for it - with the states the three textures stand in there.
    void  HostRayViews(FHostRayFeed::FViews& Views, D3D12_RESOURCE_STATES DepthState, D3D12_RESOURCE_STATES LightState);
    UBOOL EnsureReliefCover();
    UBOOL EnsureSplashes();
    UBOOL EnsureWaterTargets();
    UBOOL EnsureRipples();
    void  StepRipples(ID3D12GraphicsCommandList* Cmd);

    FSolidPass    Solid;
    FTextureCache Textures;
    FPomLibrary   Pom;
    FFinalPass    Final;

    // The frame is drawn here, then copied to the back buffer through the brightness
    // curve by the final pass. Its render target view is the RTV heap's last slot.
    // This frame's puffs of smoke and gas - see AddCloudPuff. A frame's worth, not a level's: they are gathered as
    // the cards arrive and thrown away at the next frame's start.
    FCloudPuff*                   CloudPuffs     = nullptr;
    INT                           CloudPuffHeld  = 0;   // puffs the list has memory for
    INT                           CloudPuffCount = 0;
    INT                           CloudPuffLost  = 0;   // this frame, puffs there was no memory for (AddCloudPuff)
    // Which puffs a pixel's ray can meet, by tile of the walk's target (BuildCloudTiles): two numbers a tile, then the
    // lists; and each puff's rectangle of tiles while they are made.
    UINT*                         CloudTileData  = nullptr;
    INT                           CloudTileHeld  = 0;
    INT*                          CloudPuffRects = nullptr;
    INT                           CloudRectHeld  = 0;
    INT                           CloudTileMost  = 0;   // the longest list of a tile, since the log's last line
    UBOOL                         bCloudListsRefused = 0;   // see SetVolumetrics
    INT  BuildCloudTiles(FCloudPuff* Puffs, INT Count, FLOAT Focal, const FLOAT* Screen, INT TilesX, INT TilesY,
                         const FLOAT* EngPerPixel);
    // The walk's passes and targets, made at the first need of them (DrawVolumetrics, WalkMirrorView).
    UBOOL WalkReady();
    void  WalkMirrorView(ID3D12GraphicsCommandList* Cmd, INT Index, ID3D12Resource* Depth);
    UBOOL                         bDepthCopyDone = 0;   // CopyForWater ran this frame and its copy is good
    FLOAT                         CloudFocal     = 0.0f;
    FLOAT                         CloudScreen[2] = { 0.0f, 0.0f };
    FLOAT                         CloudRect[4]   = { 0.0f, 0.0f, 0.0f, 0.0f };     // see SetCloudView
    UBOOL                         bBeamOn        = 0;                              // see SetBeam
    FLOAT                         BeamColour[3]  = { 0.0f, 0.0f, 0.0f };
    FCloudTuning                  CloudTune = { RdxMath::CloudPuffDepth, RdxMath::CloudGrainWorld,
                                                RdxMath::CloudAmbient, RdxMath::CloudRiseWorld,
                                                RdxMath::CloudWarpWorld, RdxMath::CloudSelfShade,
                                                RdxMath::CloudStepWorld, (INT)RdxMath::CloudSteps };
    FVolumetrics                  Volume;
    INT                           VolumeReady    = 0;   // 0 not tried, 1 up, -1 refused and never tried again
    UBOOL                         bVolumeRefused = 0;   // see VolumeOut
    // The passes' own targets that could not be made (RefuseTarget): each left out while the sizes and the colours it
    // failed at stand (TargetAsk), and forgotten once they are others (BeginFrame) - so asked again then, and on the
    // way back to those sizes as well. The Ready numbers beside them are the passes themselves, and a pass refused
    // stays refused.
    RdxMath::FTargetRefusal       VolumeRefusal;
    RdxMath::FTargetRefusal       BlurRefusal;
    RdxMath::FTargetRefusal       DofRefusal;
    RdxMath::FTargetRefusal       FilmRefusal;
    RdxMath::FTargetRefusal       BloomRefusal;
    RdxMath::FTargetRefusal       ShadowRefusal;
    RdxMath::FTargetRefusal       AoRefusal;
    RdxMath::FTargetAsk TargetAsk() const
    {
        RdxMath::FTargetAsk Ask;
        Ask.SceneW    = SceneWidth;
        Ask.SceneH    = SceneHeight;
        Ask.OutW      = Width;
        Ask.OutH      = Height;
        Ask.Colours   = (INT)Colours.Storage;
        Ask.FactorDiv = ShadowFactorDivisor();
        return Ask;
    }
    // Whether a refusal stands now. The ask is put together only for one that was made: this is asked for every
    // surface of a frame (WantsShadows).
    UBOOL Refused(const RdxMath::FTargetRefusal& Refusal) const
    {
        return (Refusal.bRefused && Refusal.Stands(TargetAsk())) ? 1 : 0;
    }
    void  RefuseTarget(RdxMath::FTargetRefusal& Refusal, const TCHAR* Pass);
    UBOOL                         bCloudDepthLast = 0;  // the depth copy was there last frame - see GetVolumetrics
    UBOOL                         bCubesDrawn = 0;      // this frame's cube maps are this frame's - see ShaftsCanRun
    UBOOL                         bCubesDrawnLast = 0;  // and the frame before's were its own - see ShaftsRanLast
    // Frames left of "there has been smoke lately". The walk needs a copy of the scene's depth, and that copy
    // costs MORE than the walk does - so it is asked for only while there is something to walk through. Held
    // for a while after the last puff, because a cloud that thins to nothing for a frame and comes back would
    // otherwise make and drop the copy over and over.
    INT                           CloudsRecent   = 0;
    INT                           CloudCardsSeen = 0;   // this frame, gathered or not - see NoteCloudCard
    // This frame's waterfall sheets, gathered instead of drawn (AddFall), and the same "lately" for them as for the
    // smoke - the columns want the same depth copy (WantsCloudDepth).
    RdxMath::FFallGather          Fall;
    INT                           FallSheetsSeen = 0;   // this frame, gathered or drawn - see NoteFallSheet
    INT                           FallsRecent    = 0;
    // The same for the fires' flames (AddFirePiece): this frame's pieces, in a list that holds what a frame asks for
    // (HoldFlames) - there is no most flames a frame may have -, and the bodies made of them with the numbers that
    // takes (DrawVolumetrics), as long as the pieces are many.
    RdxMath::FFlameGather         Fire;
    RdxMath::FFlamePiece*         FirePieces     = nullptr;
    INT                           FirePieceHeld  = 0;
    FLOAT*                        FireBodies     = nullptr;
    INT                           FireBodyHeld   = 0;   // in numbers, FIRE_FLOATS a body
    UBOOL                         bReliefMirror    = 0;         // SetReliefMirrorPlane
    FLOAT                         ReliefMirror[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };
    RdxMath::FFlamePiece*         MirrorFlames     = nullptr;   // a mirror's view's pieces of flame, until its end
    INT                           MirrorFlameCount = 0;
    INT                           MirrorFlameHeld  = 0;
    // A mirror's view of the level as its end needs it (MirrorViewDone, MirrorViewEnd): the view's seventeen
    // numbers, the mirror's plane in the view's space, the part of the picture the mirror shows in (the engine's
    // pixels; none where the right is not past the left: all of it), and what the view gathered for its walk - its
    // puffs among MirrorClouds, its flame bodies among MirrorBodies, its columns among MirrorFalls.
    struct FMirrorEnd
    {
        FLOAT View[17];
        FLOAT Plane[4];
        FLOAT Rect[4];
        INT   PuffFirst, PuffCount;
        INT   FireFirst, FireCount;
        INT   FallFirst, FallCount;
        FLOAT ViewPlane[4];             // the mirror's plane in the world's view's space
        INT   TriFirst, TriCount;       // its polygons' vertices among MirrorTris
        FLOAT OwnCoords[12];            // where the view stands in the level, for the own lamps
        UBOOL bOwnCoords;
    };
    FMirrorEnd*                   MirrorEnds       = nullptr;   // the frame's mirrors' views, in the order they ended
    INT                           MirrorEndCount   = 0;
    INT                           MirrorEndHeld    = 0;
    // The frame's mirrors' views' puffs of smoke, in each view's own space; from the mark on they are of the view
    // being drawn.
    FCloudPuff*                   MirrorClouds     = nullptr;
    INT                           MirrorCloudCount = 0;
    INT                           MirrorCloudHeld  = 0;
    INT                           MirrorCloudMark  = 0;
    FLOAT*                        MirrorFalls      = nullptr;   // and their columns of water, the same way
    INT                           MirrorFallCount  = 0;
    INT                           MirrorFallHeld   = 0;
    INT                           MirrorFallMark   = 0;
    // The frame's mirrors' own polygons in the world's view's space (MirrorViewDone), three numbers a vertex; and the
    // flashlight's light off the mirrors (FBeamBounce): whether it is up, whether this frame's flush keeps the
    // mirrors' views' depth for it, which making of the map the texture heap's view is of, and the frame's cone as
    // the geometry pass was given it.
    FLOAT*                        MirrorTris       = nullptr;
    INT                           MirrorTriCount   = 0;         // vertices
    INT                           MirrorTriHeld    = 0;         // numbers
    FBeamBounce                   Bounce;
    // The frame's panes of glass by their planes (NoteGlassPane), four numbers each, and whether this frame's flush
    // draws them into the map.
    enum { PANE_FIRST_NUMBER = 1000 };
    FLOAT*                        PanePlanes       = nullptr;
    INT                           PaneCount        = 0;
    INT                           PaneHeld         = 0;
    UBOOL                         bPaneNow         = 0;
    FLOAT                         PaneRimCos       = 0.0f;      // RdxMath::BeamMapRimCos for the frame's view
    UBOOL                         bPaneAll         = 0;         // a measurement: glass gives all the light back
    INT                           BounceReady      = 0;
    UBOOL                         bBounceNow       = 0;
    INT                           BounceMade       = -1;
    UBOOL                         bBounceNone      = 0;         // the view of nothing is written
    FLOAT                         BeamBlockNow[RdxMath::BeamBlockFloats];
    FLOAT*                        MirrorBodies     = nullptr;   // and the flame bodies made of each view's pieces
    INT                           MirrorBodyCount  = 0;         // in bodies
    INT                           MirrorBodyHeld   = 0;         // in numbers, FIRE_FLOATS a body
    INT*                          MirrorGroups     = nullptr;
    INT                           MirrorGroupHeld  = 0;
    // Whether any of the frame's mirrors' views has something to tell the upscaler's reactive mask, worked out at
    // the first of them to be walked: all of them are handed the mask alike (FVolumetrics::Record).
    UBOOL                         bMirrorTellsKnown = 0;
    UBOOL                         bMirrorTells      = 0;
    INT*                          FireGroups     = nullptr;
    INT                           FireGroupHeld  = 0;
    // Which bodies a pixel's ray can meet, by tile of the walk's target (BuildFireTiles), as the puffs' lists are;
    // and each body's rectangle of tiles while they are made.
    UINT*                         FireTileData   = nullptr;
    INT                           FireTileHeld   = 0;
    INT*                          FireRects      = nullptr;
    INT                           FireRectHeld   = 0;
    INT                           FireTileMost   = 0;   // the longest list of a tile, since the log's last line
    UBOOL                         bFireListsRefused = 0;    // see AddFirePiece
    UBOOL HoldFlames(INT Wanted);
    void  RefuseFireLists(const TCHAR* Which, INT Wanted);
    INT   BuildFireTiles(const FLOAT* Bodies, INT Count, const FVolumetrics::FWalk& W);
    INT                           FireSheetsSeen = 0;
    INT                           FiresRecent    = 0;
    UBOOL                         bFireWanted    = 0;   // see SetFireVolumes
    FLOAT                         WalkReactiveSmoke = RdxMath::CloudReactive;      // see SetWalkReactive
    FLOAT                         WalkReactiveFalls = RdxMath::WaterfallReactive;
    UBOOL                         bVolumeDepthView = 0; // see SetVolumeDepthView
    // How often the depth copy was ASKED FOR this frame and how often it said no. Counters rather than log
    // lines: a line that is capped spends itself on the first frames of a session and is silent at the one
    // frame that mattered, which has happened three times over now.
    INT                           CopyAsked      = 0;
    INT                           CopyRefused    = 0;
    // Diagnostics for a cloud that will not hold still: the widest a clipped card had to be put back, the most
    // the list changed in ONE frame, and how often the camera stood inside a puff. A cloud that shivers is
    // either a list that churns or a card whose recovered size jumps, and these three tell which.
    FLOAT                         CloudGrewMax   = 1.0f;
    INT                           CloudChurnMax  = 0;
    INT                           CloudInside    = 0;
    INT                           CloudMadeNow   = 0;   // this frame: puffs the tracker did not recognise
    INT                           CloudLostNow   = 0;   // and ones it let go
    FMotionBlur                   Smear;
    FLOAT                         BlurStrength   = 0.0f;
    INT                           BlurReady      = 0;   // 0 not tried, 1 up, -1 refused and never tried again
    UBOOL                         bBlurToldNoVectors = 0;
    FDepthOfField                 Dof;
    FLOAT                         DofStrength    = 0.0f;
    FLOAT                         DofLead        = RdxMath::DofFaceLead;    // SetDofTuning
    FLOAT                         DofBand        = RdxMath::DofSharpBand;
    FLOAT                         DofAlong       = 0.0f;   // the speaker's eyes' place along the view, for the log
    INT                           DofReady       = 0;   // 0 not tried, 1 up, -1 refused and never tried again
    UBOOL                         bDofToldNoDepth = 0;
    UBOOL                         bConvFocus     = 0;   // a conversation's camera runs (SetConversationFocus)
    FLOAT                         ConvFocus[3]   = { 0.0f, 0.0f, 0.0f };   // its speaker, world units
    FLOAT                         DofAmount      = 0.0f;   // the fade in and out, 0..1
    FLOAT                         DofFocus       = 0.0f;   // the followed focus, world units along the view axis
    UBOOL                         bDofFocusValid = 0;
    INT                           DofFrames      = 0;   // frames drawn with it, for the log's line now and then
    FFilmLook                     Film;
    FLOAT                         LookAberration = 0.0f;
    FLOAT                         LookVignette   = 0.0f;
    FLOAT                         LookGrain      = 0.0f;
    INT                           FilmReady      = 0;
    INT                           FilmFrame      = 0;
    FBloom                        Halo;
    FLOAT                         LookBloom      = 0.0f;
    FLOAT                         LookBloomCoronas = 0.0f;
    FLOAT                         LookFlare      = 0.0f;
    enum { MAX_BLOOM_CORONAS = 128 };
    FBloomCorona                  BloomCoronas[MAX_BLOOM_CORONAS];   // this frame's (NoteCorona)
    INT                           BloomCoronaCount = 0;
    // The own lamps (SetOwnLamps) and the world's view in the level (SetOwnWorldView), a frame's; their rows as the
    // passes read them (PrepareOwnLamps: OwnRowsLamps lamps, none this frame at 0); the table the mirror rays' passes
    // read the rows from, and its upload buffer, a frame slot's OWN_TABLE_SLOT bytes each; the maps' making whose
    // view the texture heap holds (FShadowMaps::SpotsMade), -1 the view of nothing.
    FOwnLamp                      OwnLamps[RdxMath::OwnLampMost];
    INT                           OwnLampCount     = 0;
    FLOAT                         OwnWorld[12]     = {};
    UBOOL                         bOwnWorld        = 0;
    FLOAT                         OwnRowsNow[FSolidPass::OWN_FLOATS] = {};
    INT                           OwnRowsLamps     = 0;
    enum { OWN_TABLE_SLOT = 2048 };
    TComRef<ID3D12Resource>       OwnTable;
    TComRef<ID3D12Resource>       OwnTableUpload;
    BYTE*                         OwnTableMapped   = nullptr;
    D3D12_RESOURCE_STATES         OwnTableState    = D3D12_RESOURCE_STATE_COPY_DEST;
    UBOOL                         bOwnTableRefused = 0;
    UBOOL                         bOwnTableLit     = 0;     // the table holds lamps, not the head of none
    INT                           OwnMapsMade      = -1;
    UBOOL                         bOwnViewsNone    = 0;
    // The flashlight's images in the frame's mirrors and panes (NoteLampImage: a plane and a share each), and what the
    // frame's lamps give the lens's ghosts and the eye (MakeLampFlares; DrawLampGlares).
    enum { MAX_LAMP_IMAGES = 16, MAX_BLOOM_LAMPS = 32 };
    FLOAT                         LampImages[MAX_LAMP_IMAGES * 5] = {};
    INT                           LampImageCount   = 0;
    FBloomLamp                    BloomLamps[MAX_BLOOM_LAMPS];
    INT                           BloomLampCount   = 0;
    FBeamBounce::FLamp            LampGlares[RdxMath::OwnLampMost];
    INT                           LampGlareCount   = 0;
    // Since the log's last line: frames with own lamps, the most in one, those without maps for want of them (the
    // shadows off or refused), the mirrors' views that took them, the glares, the ghosts, the flashlight's images,
    // and the walks that drew the beams in the air - the world's view's and the mirrors' views'.
    INT OwnStatFrames = 0, OwnStatMost = 0, OwnStatUnmapped = 0, OwnStatMirrors = 0, OwnStatGlares = 0,
        OwnStatGhosts = 0, OwnStatImages = 0, OwnStatBeams = 0, OwnStatBeamMirrors = 0;
    // Whether this frame's own lamps hold a weapon lamp with its map (PrepareOwnLamps): its beam in the air, which the
    // walk draws (DrawVolumetrics, WalkMirrorView).
    UBOOL OwnBeamsNow() const
    {
        for (INT i = 0; i < OwnRowsLamps; i++)
            if (!OwnLamps[i].bEye)
                return 1;
        return 0;
    }
    // The beams into a walk's numbers, View the view of the maps it points at - nothing where there are none.
    void OwnBeamsInto(FVolumetrics::FWalk& W, D3D12_SHADER_RESOURCE_VIEW_DESC& View);
    void PrepareOwnLamps();
    void UploadOwnTable();
    void MakeLampFlares(const FLOAT* Cone);
    void DrawLampGlares(ID3D12GraphicsCommandList* Cmd, INT World);
    FLOAT                         ShaftStrength  = 0.0f;
    INT                           ShaftLamps     = 0;     // gathered last frame, for the readout
    // Which lamps a pixel's ray can meet, by tile of the walk's target (BuildShaftTiles), as the puffs' lists are;
    // and each lamp's rectangle of tiles while they are made.
    UINT*                         ShaftTileData  = nullptr;
    INT                           ShaftTileHeld  = 0;
    INT*                          ShaftRects     = nullptr;
    INT                           ShaftRectHeld  = 0;
    INT                           ShaftLampsMost = 0;     // the most lamps of a frame, since the log's last line
    INT                           ShaftLeftOut   = 0;     // and the most that could show and were not picked
    INT                           ShaftTileMost  = 0;     // and the longest list of a tile
    INT                           ShaftFrames    = 0;     // frames with shafts, since that line
    UBOOL                         bShaftListsRefused = 0; // see ShaftsRefused
    INT   BuildShaftTiles(const FLOAT* Lamps, INT Count, const FVolumetrics::FWalk& W);
    INT                           BloomReady     = 0;
    enum { CLOUDS_RECENT_FRAMES = 120 };
    // The puffs FOLLOWED between frames, so that one the game stops sending can be kept: whole while it is only out
    // of the view, thinning for a moment where it has gone.
    //
    // The card carries no name, so which puff is which has to be worked out from WHERE it is - in the world, not on
    // the screen, or every one of them would look new the moment the head turns. One that is gone has nothing near
    // it in this frame's list. Where a puff stood is looked up in a grid of the level (CloudCellHead by a cell's
    // hash, CloudCellNext the chain of the puffs in it): a level places smoke by the hundred puffs.
    struct FCloudTrack
    {
        FLOAT World[3];
        FLOAT Radius, Tall;
        FLOAT Colour[3];
        FLOAT Fade;       // the game's fade of its card when it was last seen
        FLOAT Gone;       // seconds since it was last seen AND could have been seen; 0 while it is still there
        FLOAT Away;       // seconds since it was last seen while OUT OF THE VIEW - not the same thing
        UBOOL bUsed;
    };
    FCloudTrack*                  CloudTracks     = nullptr;
    INT                           CloudTrackHeld  = 0;  // puffs followed the list has memory for
    INT                           CloudTrackCount = 0;
    INT*                          CloudCellHead   = nullptr;
    INT                           CloudCellHeld   = 0;
    INT*                          CloudCellNext   = nullptr;
    INT                           CloudNextHeld   = 0;
    DOUBLE                        CloudLastTime   = 0.0;

    TComRef<ID3D12Resource>       SceneColor;
    TComRef<ID3D12DescriptorHeap> SceneSrvHeap;
    // The water's copies (CopyForWater): the picture and the depth as they were at the frame's first water surface, made
    // alike to the scene target and the depth buffer so a plain copy (or the MSAA resolve) fills them. Their views sit in
    // the texture heap's reserved slots 0 and 1, where the geometry pass can see them. Made on first use, and dropped
    // with the targets they copy.
    TComRef<ID3D12Resource>       WaterColor;
    TComRef<ID3D12Resource>       WaterDepth;
    D3D12_RESOURCE_STATES         WaterDepthState;
    // A depth view of that copy: what the engine draws after the world goes into it before the volumes' walk.
    TComRef<ID3D12DescriptorHeap> WaterDepthDsvHeap;
    INT                           DepthNearerReady = 0;     // 0 not tried, 1 up, -1 refused
    UBOOL                         bWeaponDepthPut  = 0;     // PutAfterWorldDepth ran this frame
    void PutAfterWorldDepth(ID3D12GraphicsCommandList* Cmd);
    FFogRooms                     FogRoom;
    INT                           FogRoomsReady    = 0;     // 0 not tried, 1 up, -1 refused
    FFieldVolume                  Field;
    INT                           FieldReady       = 0;     // 0 not tried, 1 up, -1 could not be made
    UBOOL                         bFieldWanted     = 0;     // see SetFieldVolumeWanted
    FHoloShell                    Shell;
    INT                           ShellReady       = 0;     // 0 not tried, 1 up, -1 could not be made
    UBOOL                         bHoloShellWanted = 0;     // see SetHoloShellWanted
    // This frame's hologram triangles: their corners, the runs of one texture they come in, and each run's texture
    // as the texture cache numbers it.
    FHoloShell::FVertex*          HoloVerts        = nullptr;
    FHoloShell::FRun*             HoloRuns         = nullptr;
    INT*                          HoloRunSrvs      = nullptr;
    INT                           HoloVertCount    = 0, HoloVertHeld = 0;
    INT                           HoloRunCount     = 0, HoloRunHeld = 0, HoloRunSrvHeld = 0;
    INT                           HoloFrames       = 0;     // frames the shell was laid in, for the log's line
    INT                           HoloMisses       = 0;     // frames in a row whose holograms could not be laid
    FLOAT                         HoloLayers       = RdxMath::HoloShellLayers;     // see SetHoloShellLayers
    // This frame's field: each polygon's key, how many corners it has, which of the frame's kinds it is of, and its
    // corners in the world (FieldCorners places of three numbers).
    INT*                          FieldKeys        = nullptr;
    INT*                          FieldSizes       = nullptr;
    INT*                          FieldKinds       = nullptr;
    FLOAT*                        FieldPoints      = nullptr;
    FLOAT*                        FieldRowsList    = nullptr;   // the pass's rows, made of them at the draw
    INT                           FieldKeysHeld    = 0, FieldSizesHeld = 0, FieldPointsHeld = 0, FieldRowsHeld = 0;
    INT                           FieldKindsHeld   = 0;
    INT                           FieldCount       = 0;
    // The kinds of field the frame holds: what each was handed over as, and its look (its hue and how bright it is
    // laid on - four numbers).
    INT*                          FieldKindNames   = nullptr;
    FLOAT*                        FieldKindLooks   = nullptr;
    INT                           FieldKindNamesHeld = 0, FieldKindLooksHeld = 0;
    INT                           FieldKindCount   = 0;
    // The frame's polygons as the draw makes them fewer (RdxMath::FieldMergeAll): copies, the frame's own stay.
    INT*                          FieldFewSizes    = nullptr;
    INT*                          FieldFewKinds    = nullptr;
    FLOAT*                        FieldFewPoints   = nullptr;
    INT                           FieldFewSizesHeld = 0, FieldFewKindsHeld = 0, FieldFewPointsHeld = 0;
    FLOAT                         FieldTime        = 0.0f;
    INT                           FieldFrames      = 0;     // frames the field was laid in, for the log's line
    UBOOL                         bFogRoomsWanted  = 0;
    FFogLight*                    FogLights        = nullptr;   // this frame's, as many as there are (SetFogLights)
    INT                           FogLightHeld     = 0;
    INT                           FogLightCount    = 0;
    INT                           FogLightMost     = 0;     // the most in a frame so far, for the log
    UINT                          FogCameraZone    = 0;
    UBOOL                         bFogRoomsSaid    = 0;     // the pass's first frame named in the log
    INT                           WaterLevel;
    UBOOL                         bWaterWanted;
    INT                           ReflectionLevel;          // see SetReflections
    // Geometry=Tessellation: the relief's covers (BeginReliefCovers) - the depth drawn before the relief's first patches,
    // and then everything else of the world that can stand in front of a surface, drawn into it flat: where a raised
    // surface meets a poster, a mirror or a rug, DSRelief reads it (ReliefClear). Made alike to the depth buffer on first
    // use, and dropped with it. Its view sits in the texture heap's reserved slots, beside a view of nothing that the
    // relief reads in a frame without covers (ReliefCoverView, ReliefCoverNone). bReliefCoverOff: a measurement only.
    TComRef<ID3D12Resource>       ReliefCover;
    TComRef<ID3D12DescriptorHeap> ReliefCoverDsvHeap;
    D3D12_RESOURCE_STATES         ReliefCoverState   = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    UBOOL                         bReliefCoverWanted = 0;
    UBOOL                         bReliefCoverOff    = 0;
    UBOOL                         bReliefCoverNone   = 0;   // the view of nothing is written
    // The ripples (StepRipples): two height fields taking turns - the one the last step wrote is the one the water reads,
    // through the second of the reserved view sets when it is field 1. Their own small pass: a root signature, a pipeline,
    // a view heap for reading the other field, and render target views.
    // The water's views in the texture heap's reserved slots: two sets, one per ripple field (StepRipples picks the one the
    // last step wrote), each the picture, the depth, that ripple field, the texture's own wave field, the caustics' net,
    // the mirror probe's pyramid (FMirrorProbe) and the own body's picture (FMirrorBody), for the glass's layer - where
    // the geometry pass's water table has them (FSolidPass::MakeRoot).
    enum { WaterSetViews = 7, WaterViewPicture = 0, WaterViewDepth = 1, WaterViewRipples = 2, WaterViewWaves = 3,
           WaterViewNet = 4, WaterViewProbe = 5, WaterViewBody = 6 };
    static_assert(WaterViewNet == FSolidPass::WATER_VIEW_NET && WaterViewProbe == FSolidPass::WATER_VIEW_PROBE &&
                  WaterViewBody == FSolidPass::WATER_VIEW_PROBE + 1,
                  "the water's views must stand where the geometry pass's water table reads them");
    INT   WaterSlot(INT Set, INT View) const { return Textures.GetReservedIndex(Set * WaterSetViews + View); }
    // The texture's own wave field (SetWaterWaves): its GPU copy, an upload buffer per frame slot, and the heights waiting
    // for the next frame's start.
    TComRef<ID3D12Resource>       WaveField;
    TComRef<ID3D12Resource>       WaveUpload[BACKEND_FRAME_COUNT];
    BYTE*                         WaveUploadMapped[BACKEND_FRAME_COUNT];
    BYTE                          WaveHeights[128 * 128];
    INT                           WaveW, WaveH;
    UBOOL                         bWaveNew;
    UBOOL                         bWaveFailed;
    UBOOL EnsureWaveField();
    // The water's lamps (SetWaterLamps): one upload buffer, 1024 bytes for each frame slot - a constant buffer's place is
    // a multiple of 256, and a slot covers the shader's whole block up to the held edges' count and the spherifier's rows
    // after it (zeros: off), which a relief run bound to the relief's slot reads: the lamps' count,
    // RDX_RELIEF_LAMP_MAX lamps of two rows, that count, and those rows.
    enum { WaterLampBytes = 1024 };
    static_assert((2 + FSolidPass::SPHERE_ROWS + 2 * RDX_RELIEF_LAMP_MAX) * 16 <= WaterLampBytes,
                  "a lamp slot must hold the shader's lamp rows, the count and the spherifier's rows");
    TComRef<ID3D12Resource>       WaterLampBuffer;
    BYTE*                         WaterLampMapped;
    UBOOL                         bWaterLampFailed;
    UBOOL EnsureWaterLamps();
    // The relief's lamps (SetReliefLamps): the same layout and size, a buffer of their own. The slot is emptied at the
    // frame's start, so a frame nobody gave lamps to lights the relief with none.
    TComRef<ID3D12Resource>       ReliefLampBuffer;
    BYTE*                         ReliefLampMapped = nullptr;
    UBOOL                         bReliefLampFailed = 0;
    // And what the frame's slot was given, kept here as well: the relief's held-edge blocks carry the lamps too
    // (SetReliefHeld), and the upload heap is not to be read back.
    FLOAT                         ReliefLampCopy[4 * (1 + 2 * RdxMath::ReliefLampMax)] = {};
    UBOOL EnsureReliefLamps();
    // The water's splashes (AddSplashes, DrawSplashes). Every drop is worked out where it is from its splash's start
    // alone, so a splash is its place, size, strength, a seed for its drops, and when it began. What draws them: the
    // drops' nearest depth and their thickness, that depth smoothed so neighbouring drops run together into one surface,
    // and the upscaler's reactive mask - all at the scene's size, made again with it; the first three have views in the
    // texture heap's reserved slots after the water's, in the order the smoothing's two passes read them in pairs (the
    // first pass's depth and thickness, then the drops' depth and thickness). The smoothed surface ends up back in the
    // drops' targets. A root signature and three pipelines of their own, and a constant buffer's place for each frame
    // slot. bExtraCopy: a copy made besides the frame's own - the splashes', a fall's sheet's (CopyForWaterAgain) - is
    // not the water's, for the timing.
    struct FSplash
    {
        FLOAT  X, Y, Z, Radius, Strength, Seed, Column;
        DOUBLE Start;
    };
    enum { SplashViewSmooth = 2 * WaterSetViews, SplashViewDepth = SplashViewSmooth + 1, SplashViewThick = SplashViewSmooth + 2 };
    // The RT water's views, beside the splashes' (FTextureCache::RESERVED_VIEWS): the shadow masks, the far probe, and the
    // probe's UAV for its update.
    enum { RtMaskView = SplashViewThick + 1, RtFarView = RtMaskView + 1, RtFarUav = RtMaskView + 2 };
    // The relief's covers' view, and the view of nothing read in their place (ReliefCover).
    enum { ReliefCoverView = RtFarUav + 1, ReliefCoverNone = RtFarUav + 2 };
    // The water's own depth as the see-through water writes it (WaterSurfaceView) - a view of nothing while there is none.
    enum { WaterSurfaceUavView = ReliefCoverNone + 1 };
    // Its view in DepthSrvHeap, after the two depth buffers': the motion vectors and the stamp read it from there.
    enum { WaterSurfaceDepthSrv = 2 };
    enum { SplashConstBytes = 2048 };
    FSplash                       Splashes[RdxMath::SplashMax];
    INT                           SplashNext;
    INT                           SplashSeed;
    FLOAT                         SplashOrigin[3];
    FLOAT                         SplashAxes[9];
    FLOAT                         SplashFocal, SplashCentreX, SplashCentreY;
    UBOOL                         bSplashView;
    // Which view DrawSplashes draws for: the world's (0) or a mirror's (1, DrawMirrorSplashes) - each with constants
    // of its own, as both are drawn in one frame.
    INT                           SplashRun = 0;
    // The frame's fountains (SetCurtains), when their clock started, and when their drops were last drawn.
    FLOAT                         Curtains[RdxMath::CurtainMax * RdxMath::CurtainFloats];
    INT                           CurtainCount;
    DOUBLE                        CurtainEpoch;
    DOUBLE                        CurtainAble;
    TComRef<ID3D12Resource>       FluidDepth;
    TComRef<ID3D12Resource>       FluidThick;
    TComRef<ID3D12Resource>       FluidSmooth;
    TComRef<ID3D12Resource>       Reactive;
    TComRef<ID3D12DescriptorHeap> SplashRtvHeap;
    TComRef<ID3D12RootSignature>  SplashRoot;
    TComRef<ID3D12PipelineState>  SplashDropPipeline;
    TComRef<ID3D12PipelineState>  SplashCurtainPipeline;        // a fountain's drops, into the same targets
    TComRef<ID3D12PipelineState>  SplashSmoothPipeline[2];      // across, then down with the reactive mask
    TComRef<ID3D12Resource>       SplashConstBuffer;
    BYTE*                         SplashConstMapped;
    UBOOL                         bSplashFailed;
    UBOOL                         bExtraCopy;
    UBOOL                         bReactiveLive;         // this frame's splashes or walk wrote the reactive mask
    // The temporal upscaler's reactive mask (Reactive, at the scene's size): how much less of the frames before it
    // keeps at a pixel - where the water's splashes are (DrawSplashes), and where the walk lays fires and waterfalls'
    // columns (DrawVolumetrics; SetWalkReactive). Written as a render target by the splashes and through a UAV by the
    // walk; the state it is in follows it (ReactiveState), and the frame's first writer clears it (bReactiveInit).
    // The see-through water does not write it: there it switched the upscaler's accumulation off over the whole
    // surface, and the waves shimmered even standing still.
    TComRef<ID3D12DescriptorHeap> ReactiveRtvHeap;
    D3D12_RESOURCE_STATES         ReactiveState;
    UBOOL                         bReactiveInit;
    // The see-through water's own depth (WaterSurface, R32 at the scene's size: the depth buffer's value of the nearest
    // water surface, 0 where there is none). The water writes no depth - a run drawn after it must still see what lies
    // under it - so the motion vectors and the temporal upscaler would take the floor under the water, or the sky past
    // the level, for the surface: walking, the waves then move with the wrong thing, and the upscaler sees an edge at the
    // shore that is not there. Written by the water through a UAV (WaterSurfaceView), cleared on the frame's first water
    // run (bWaterSurfaceCleared); read by the motion vectors (DrawMotionVectors) and stamped into the world's depth
    // just before the upscaler reads it (StampWaterSurface) - after the shadows, the occlusion and the smoke, which
    // want what lies under the water. Those read it under a temporal upscaler alone (bWaterSurfaceLive). The water
    // itself reads it in every mode: lifted by its waves the surface stands in front of itself, and with no depth
    // written the nearest of its points at a pixel is known only from here (the solid pass's two goes over a water
    // run).
    TComRef<ID3D12Resource>       WaterSurface;
    TComRef<ID3D12DescriptorHeap> WaterSurfaceRtvHeap;
    D3D12_RESOURCE_STATES         WaterSurfaceState;
    UBOOL                         bWaterSurfaceLive;      // this frame's water wrote it, for what reads it afterwards
    UBOOL                         bWaterSurfaceCleared;   // this frame's first water run has cleared it
    UBOOL                         bWaterSurfaceViewReal;  // WaterSurfaceUavView is its own view, not a view of nothing
    // The wet floors' mark (docs/terv-nedves-feluletek.md 8.5; RdxMath::WetMarkPack): R32 twice the scene's width, the
    // nearest wet floor's depth at each pixel and what the late reflection pass needs of it at the pixel's even column,
    // its relief's normal at the odd one (RdxMath::WetTiltPack) - written by the wet floors'
    // runs through a UAV with an atomic maximum (WetMarkView), cleared through a render target view on the frame's
    // first wet run (bWetMarkCleared), read by the late reflection pass.
    TComRef<ID3D12Resource>       WetMark;
    TComRef<ID3D12DescriptorHeap> WetMarkRtvHeap;
    D3D12_RESOURCE_STATES         WetMarkState;
    UBOOL                         bWetMarkCleared;
    UBOOL                         bWetMarkViewReal;       // WetMarkUav is its own view, not a view of nothing
    UBOOL                         bWetMarkNone = 0;       // both its views are views of nothing (BeginFrame)
    // The wet floors' measuring view asked for (RequestWetView): 0 none.
    INT                           WetShowView = 0;
    // The wet floors' late reflection pass (FWetReflect), made the first frame it is wanted (WetReflectReady: 0 not yet,
    // 1 made, -1 refused), for the picture's format, the depth's and the sample count it was made with (WetReflectFor:
    // made again where they moved); the frame's wet block (SetWetBlocks' rows, bWetBlockNow: there is one), which it
    // reads too; and its drawing, from FBackend::CopyForWater - true where it drew.
    FWetReflect                   WetReflect;
    INT                           WetReflectReady = 0;
    DXGI_FORMAT                   WetReflectFor[2] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
    INT                           WetReflectSamples = 0;
    FLOAT                         WetBlockNow[RdxMath::WetBlockFloats] = {};
    UBOOL                         bWetBlockNow    = 0;
    // The wet floors' lamps (SetWetLamps): the water's lamps' layout of a slot - the count, then two rows a lamp -, a
    // buffer of their own, emptied at the frame's start.
    TComRef<ID3D12Resource>       WetLampBuffer;
    BYTE*                         WetLampMapped   = nullptr;
    UBOOL                         bWetLampFailed  = 0;
    UBOOL EnsureWetLamps();
    // The rain's cover's place for the wet block (SetWetCover): its grid's corner in X and Y, one over its span in each;
    // all 0 - no cover - until a level's is made. Its texture and the buffer it was copied from, two of each: a level's
    // takes the other pair, and the pair let go is two levels old, long done with by the card.
    FLOAT                         WetCoverNow[4]  = {};
    TComRef<ID3D12Resource>       WetCoverTex[2];
    TComRef<ID3D12Resource>       WetCoverUp[2];
    INT                           WetCoverPair    = 0;
    UBOOL                         bWetCoverViewSet = 0;   // its view written once at least, a real one or of nothing
    UBOOL                         bWetLayerNow    = 0;    // AddWetLayer's: the water is the rays' this frame
    UBOOL DrawWetReflections(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    // With the shadows, what their apply pass needs of the see-through water's pixels (ApplyShadows): the surface's
    // light, the bed's share and where the bed was seen (SolidPass.hlsl's WaterShade, RGBA16 at the scene's size),
    // written by the water beside its own depth; the water's own depth and light at the factor's samples (WaterPack,
    // two floats, coarser than the factor by WaterPackCoarse); and the shadow factor of the surface's own place
    // (WaterFactor, R8), drawn from the pack as the factor is from the world's depth, by the same sampling rule over it -
    // by the maps here, by the rays in the 64-bit host. bWaterShadeLive: this frame's water wrote the shade.
    TComRef<ID3D12Resource>       WaterShade;
    D3D12_RESOURCE_STATES         WaterShadeState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    UBOOL                         bWaterShadeLive = 0;
    UBOOL                         bWaterShadeViewReal = 0;
    UBOOL                         bWaterShadeFailed = 0;  // it would not be made: not again until the sizes change
    TComRef<ID3D12Resource>       WaterFactor;
    TComRef<ID3D12Resource>       WaterPack;
    TComRef<ID3D12DescriptorHeap> WaterFactorRtvHeap;     // the factor's view, then the pack's
    INT                           WaterFactorW = 0, WaterFactorH = 0, WaterPackW = 0, WaterPackH = 0;
    INT                           WaterPackReady = 0;     // its pipeline: 0 not tried, 1 up, -1 failed
    // How much coarser than the world factor's samples the pack takes its own, each way: 2, the surface's factor then
    // an eighth of the scene's size where the factor is half of it - the surface's own shadow is soft, and it moves.
    enum { WaterPackCoarse = 2 };
    UBOOL                         bWaterShadowViewsDirty = 1;
    UBOOL                         bWaterShadowMarkOff = 0;    // SetWaterShadowMarkOff
    // The caustics' net (DrawCausticNet): the light carried down through a square of smooth waves and counted where
    // it lands, a repeating picture laid on what lies under the water (LayCaustics), with smaller levels of itself for
    // where a pixel covers more of it. CausticPoints a side: the places of the surface the light is carried from;
    // CausticTile: the level's units a repeat covers; CausticCarry: how far, in those units, a slope of one moves the
    // light sideways - how deep the bed lies, as the net sees it.
    enum { CausticNetSize = 512, CausticNetLevels = 4, CausticPoints = 2048 };
    static constexpr FLOAT CausticTile  = 256.0f;
    static constexpr FLOAT CausticCarry = 34.0f;
    TComRef<ID3D12Resource>       CausticNet;
    TComRef<ID3D12DescriptorHeap> CausticSrvHeap;
    TComRef<ID3D12DescriptorHeap> CausticRtvHeap;
    TComRef<ID3D12RootSignature>  CausticRoot;
    TComRef<ID3D12PipelineState>  CausticSplat;
    TComRef<ID3D12PipelineState>  CausticHalve;
    UBOOL                         bCausticsWanted = 0;    // SetCaustics
    UBOOL                         bCausticsFailed = 0;    // a pass could not be made: no caustics, said once
    UBOOL                         bCausticsDrawn  = 0;    // the net holds a picture
    DOUBLE                        CausticStart    = 0.0;
    UBOOL EnsureCaustics();
    void  DrawCausticNet(ID3D12GraphicsCommandList* Cmd);
    UBOOL                         bCausticOldNet  = 0;    // SetCausticOldNet
    // The laying-on (LayCaustics). Its mask: a picture of the scene's size that says of every pixel which liquid it
    // lies in (RdxMath::CausticMaskValue), drawn from the liquids' borders - their triangles in an upload buffer, a
    // place for each frame slot - with its view in the texture heap's reserved slots; made on first use, gone with the
    // scene's target, and where it cannot be made the caustics are left out until the sizes change (CausticRefusal).
    // Its pipelines: the mask's, and for laying on one for the picture at one sample and one for the world's
    // multisampled picture, each with the format and the samples it was made for (EnsureCausticLay). This frame's view,
    // waters and borders (SetCausticView); and whether this frame's picture has the caustics already - the water's
    // copies may be made more than once a frame (CopyForWaterAgain), the caustics are laid on once.
    enum { CAUSTIC_LAY_CONSTANTS = 48, CausticPortalBytes = RdxMath::CausticPortalVertsMost *
                                                            RdxMath::CausticPortalFloats * sizeof(FLOAT) };
    enum { CausticMaskView = WaterSurfaceUavView + 1 };
    // The map of the mirrors' views' depth, for the flashlight's light off the mirrors (FBeamBounce).
    enum { BounceMapView = CausticMaskView + 1 };
    // The rays' list's views (FHostRayFeed, docs/terv-rt-host.md). For the builds that write their rays down: the
    // pixels' heads and the requests as a pixel shader writes them, side by side - a range of the water's UAV table,
    // which begins at WaterSurfaceUavView. For the builds that read the answers: the answers and what the rays
    // passed, side by side - a range of the masks' table, whose first view (RtMaskView) is the pixels' heads there.
    enum { RayHeadsUav = BounceMapView + 1, RayAskedUav = BounceMapView + 2, RayAnswersView = BounceMapView + 3,
           RayPassedView = BounceMapView + 4 };
    // Reflections=RTLow's pass over the list (FRaySparse), on the heads' view above: made the first round it runs
    // (bSparseTried). The round being asked: 2 where it runs as RTLow - the pass and the own list made - else what
    // GetRtMirrorSparse says, RT where RTLow cannot run. And the own list's view as last written: for which share, and
    // whether there was a list.
    FRaySparse                    RaySparse;
    UBOOL                         bSparseTried    = 0;
    INT                           RtSparseNow     = 0;
    INT                           RtLowLamps      = RdxMath::RtLowShadowedLamps;
    INT                           RtLowViewSerial = -1;
    UBOOL                         RtLowViewHas    = 0;
    // The own lamps' table and maps (PrepareOwnLamps), side by side: a range of the mirrors' map's table in the
    // geometry pass's own root signature, and of the masks' table in the rays' (FSolidPass::SetOwnLampViews).
    enum { OwnLampTableView = RayPassedView + 1, OwnLampMapsView = RayPassedView + 2 };
    // What the shadows' apply pass needs of the see-through water's pixels, as the water writes it (WaterShade) - a
    // view of nothing while there is none.
    enum { WaterShadeUav = OwnLampMapsView + 1 };
    // The mirror probe's passes' table (FMirrorProbe::VIEWS long, in its order): the copies of the picture, the depth and
    // its stencil, the added target, then the probe's own five buffers.
    enum { ProbeTableView = WaterShadeUav + 1 };
    // Reflections=RTLow's own list (FHostRayFeed::GetLow) as its pixels write it - u7 of the water's UAV table, a view
    // of nothing until the list is made.
    enum { RayAskedLowUav = ProbeTableView + FMirrorProbe::VIEWS };
    // ... and the corners the rays are traced against, as the host has them (FHostRayFeed::GetCorners), side by side:
    // the level's, the frame's bodies', the arena's - a range of the masks' table in the rays' root signature
    // (t23..t25), for the build that reads the answers. Views of nothing where the share has none.
    enum { RayCornersView = RayAskedLowUav + 1 };
    // The wet floors' mark (WetMark) as their runs write it - u4 of the water's UAV table, a view of nothing until it is
    // made - and as the late reflection pass reads it.
    enum { WetMarkUav = RayCornersView + 3, WetMarkSrv = RayCornersView + 4 };
    // The rain's cover over the wet floors (SetWetCover) as the wet floors' runs and the late reflection pass read it -
    // t27 of the wet runs' UAV table, as far after the water's own depth's view as this is; a view of nothing until a
    // level's is made.
    enum { WetCoverSrv = RayCornersView + 5 };
    static_assert(WetCoverSrv + 1 <= FTextureCache::ReservedViews(), "the texture heap reserves too few views");
    // The volumetric walk's picture as the late wet pass's second draw reads it (the fires' flames in the wet floors'
    // mirror, DrawVolumetrics): a view of nothing until the first frame it is drawn, made again where the picture is.
    enum { WetFlameSrv = WetCoverSrv + 1 };
    static_assert(WetFlameSrv + 1 <= FTextureCache::ReservedViews(), "the texture heap reserves too few views");
    // The late wet pass's numbers this frame and whether it drew multisampled, kept for that second draw, and the
    // walk's picture its view was last made of (compared, never followed); and the own lamps as the pass reads them
    // (WetReflect.hlsl's WetSpots), this frame's block.
    FLOAT           WetConstNow[FWetReflect::CONSTANTS];
    D3D12_GPU_VIRTUAL_ADDRESS WetSpotsNow = 0;
    UBOOL           bWetDrawnNow = 0, bWetMsaaNow = 0;
    ID3D12Resource* WetFlameOf   = nullptr;
    // The probe (UpdateMirrorProbe): made the first frame it runs (bProbeTried; bProbeRefused where that failed);
    // its table to be written again where what it views was made again; run this frame.
    FMirrorProbe                  MirrorProbe;
    FMirrorBody                   MirrorBody;
    UBOOL                         bProbeTried      = 0;
    UBOOL                         bProbeRefused    = 0;
    UBOOL                         bProbeViewsDirty = 1;
    UBOOL                         bProbeDone       = 0;
    // The probe made where it is wanted and not yet tried, its pyramid's view put in the water's sets; then its frame's
    // passes on the copies just made (CopyForWater) - the copies and the added target readable by them for the while.
    UBOOL EnsureMirrorProbe();
    UBOOL UpdateMirrorProbe(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    // The own body's picture for the frame (FMirrorBody), after the probe's passes: drawn where the probe stands before
    // a pane and the shadows hold the body, cleared otherwise; the scene's targets bound again with Dsv.
    void  DrawMirrorBody(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv);
    void  WriteProbeViews();
    TComRef<ID3D12RootSignature>  CausticLayRoot;
    TComRef<ID3D12PipelineState>  CausticLays[2];
    TComRef<ID3D12PipelineState>  CausticMaskPipeline;
    TComRef<ID3D12Resource>       CausticMask;
    TComRef<ID3D12DescriptorHeap> CausticMaskRtvHeap;
    TComRef<ID3D12Resource>       CausticPortalBuffer;
    BYTE*                         CausticPortalMapped  = nullptr;
    RdxMath::FTargetRefusal       CausticRefusal;
    DXGI_FORMAT                   CausticLayFormat[2]  = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
    UINT                          CausticLaySamples[2] = { 0, 0 };
    FLOAT                         CausticOrigin[3]  = { 0.0f, 0.0f, 0.0f };
    FLOAT                         CausticAxes[9]    = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    FLOAT                         CausticFocal      = 0.0f;
    FLOAT                         CausticCentreX    = 0.0f;
    FLOAT                         CausticCentreY    = 0.0f;
    FLOAT                         CausticBoxes[RdxMath::CausticBoxesMost * RdxMath::CausticBoxFloats];
    INT                           CausticBoxCount   = 0;
    INT                           CausticCorners    = 0;  // this frame's borders' corners, in this frame slot's place
    INT                           CausticCamWet     = 0;  // the liquid zone the camera is in, plus one
    UBOOL                         bCausticView      = 0;
    UBOOL                         bCausticsLaid     = 0;
    // For the log, with the GPU timing's lines: the frames of its window that drew the net, and those that laid it on.
    INT                           CausticNetFrames  = 0;
    INT                           CausticLaidFrames = 0;
    UBOOL EnsureCausticLay(DXGI_FORMAT Format, UINT Samples);
    UBOOL EnsureCausticBorders();
    UBOOL EnsureCausticMask();
    UBOOL LayCaustics(ID3D12GraphicsCommandList* Cmd, UBOOL bMsaa);
    TComRef<ID3D12Resource>       RippleField[2];
    TComRef<ID3D12DescriptorHeap> RippleSrvHeap;
    TComRef<ID3D12DescriptorHeap> RippleRtvHeap;
    TComRef<ID3D12RootSignature>  RippleRoot;
    TComRef<ID3D12PipelineState>  RipplePipeline;
    INT                           RippleCurrent;          // the field the last step wrote
    UBOOL                         bRippleCleared;         // both fields cleared once, on the first frame with water
    UBOOL                         bRippleFailed;          // the pass could not be made: no ripples, said once
    UBOOL                         bRippleWanted;          // SetRipples was called: there is water to ripple
    FLOAT                         RippleCarry;            // steps owed, in fractions (RdxMath::RippleSteps)
    DOUBLE                        RippleLastTime;
    FLOAT                         RippleOriginX, RippleOriginY;   // the field's lower corner at the last step
    UBOOL                         bRippleOriginValid;
    FLOAT                         RippleCameraX, RippleCameraY;   // where the field should be centred next
    FLOAT                         RipplePulses[RdxMath::RippleMaxPulses * 4];
    INT                           RipplePulseCount;
    FLOAT                         OutputExponent;

    // CPU-readable copy of the scene target, made on demand by ReadScene.
    TComRef<ID3D12Resource>       Readback;
    UINT64                        ReadbackSize;

    // Supersampling only - see CreateOutputTarget. Two render target views of the one storage: slot 0 in
    // the scene's drawing format (the geometry pass's pipelines), slot 1 in the back buffer's (the box
    // filter's). ReadScene reads it too: output-sized, where the scene target at 2x in 4K would put 133 MB
    // into this 32-bit process for every snapshot.
    TComRef<ID3D12Resource>       OutputColor;
    TComRef<ID3D12Resource>       OutputDepth;
    TComRef<ID3D12DescriptorHeap> OutputRtvHeap;
    TComRef<ID3D12DescriptorHeap> OutputDsvHeap;
    TComRef<ID3D12DescriptorHeap> OutputSrvHeap;

    ID3D12Resource*         BackBuffers[BACKEND_FRAME_COUNT];
    ID3D12CommandAllocator* Allocators[BACKEND_FRAME_COUNT];
    UINT64                  FenceValues[BACKEND_FRAME_COUNT];

    D3D12_CPU_DESCRIPTOR_HANDLE RtvStart;
    UINT   RtvStride;
    UINT   FrameIndex;
    UINT   SwapBuffers;    // the swap chain's buffers (RequestSwapBuffers); FrameIndex stays below it
    HANDLE FenceEvent;
    DOUBLE FrameWaitSec;   // the last MoveToNextFrame's wait

    // Diagnostic: our own clock on every Present - see NotePresent.
    LONGLONG PrevPresentQpc;
    DOUBLE   QpcMs;                       // milliseconds per performance-counter tick
    INT      PresentN, PresentLong, PresentShort, PresentLogged;
    DOUBLE   PresentMaxGap, PresentMaxCall;
    INT      PresentsAfterFlush;          // every one of the first N after a level change is logged

    INT    Width;
    INT    Height;
    // HDR and ten-bit output. HdrMode is what was ASKED for, HdrRunning what the output path actually does, HdrWhy
    // why the two differ. Colours is the format table both the targets here and FFinalPass's pipelines are built from,
    // so there is one answer to "what format is this".
    RdxMath::EHdrMode HdrMode;
    RdxMath::EHdrMode HdrRunning;
    RdxMath::EHdrFallback HdrWhy;
    FRdxColours       Colours;
    UBOOL             bDisplayHdr;
    FLOAT             DisplayNits;
    char              DisplayName[32];
    INT               DisplayBits;
    INT               DisplayHdrCapable;
    UBOOL             bHdrFalseColour;
    // The window the swap chain presents into, kept from Init so the display can be asked again later - the
    // monitor under the window is what decides whether HDR is available, and it can change during a run.
    HWND              OutputWindow;
    // Reads the output the window sits on and fills the two above. Needs a window, so not before Init.
    void   ReadDisplayHdr(HWND Window);
    // HdrMode once the display has had its say, and why it came out lower (RdxMath::HdrModeForDisplay).
    RdxMath::EHdrMode ResolveHdrMode(RdxMath::EHdrFallback& Why) const;
    // The running mode worked out again for the display as it stands, and every colour target, pipeline and the swap
    // chain's buffers made again if it moved. Between frames only. SetHdrMode and OnDisplayChanged both end here.
    UBOOL  ApplyHdrRunning();
    // The frame's last pass onto the back buffer: the plain one, or the HDR transform.
    void   DrawFinal(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE Srv);
    // Every pipeline that carries a colour format, built again for whatever Colours now says. Both halves of
    // SetHdrMode go through it, so a change and the undo of a failed change cannot build different sets.
    UBOOL  RebuildForColours();
    UBOOL  RebuildForDepthFormat();
    // Tells DXGI what the numbers in the back buffer mean. Without it a half-float swap chain is taken for plain
    // sRGB and the picture comes out washed out and wrong, which is the failure that looks like a bug in the
    // shader. Returns whether the colour space was accepted.
    UBOOL  ApplyColourSpace();
    // See RequestSceneScale. SceneWidth x SceneHeight is the size of the scene target and the depth
    // buffer: Width x Height times SceneScale - the very same numbers with supersampling off.
    INT    SceneScale;
    INT    SceneWidth;
    INT    SceneHeight;
    // The antialiased image: the output's size times SceneScale. This is what the upscaler writes and what the box
    // filter averages down to the output; equal to the output's size when supersampling is off.
    INT    AaWidth;
    INT    AaHeight;
    // SMAA's own picture of where the edges are, and the two views its blending pass reads (colour, then edges).
    TComRef<ID3D12Resource>       SmaaEdges;
    TComRef<ID3D12DescriptorHeap> SmaaRtvHeap;
    TComRef<ID3D12DescriptorHeap> SmaaSrvHeap;    // 2 descriptors: [0] the colour, [1] the edges
    UBOOL  CreateSmaaTargets();
    void   DrawSmaa(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* ColourHeap,
                    D3D12_GPU_DESCRIPTOR_HANDLE ColourSrv, D3D12_CPU_DESCRIPTOR_HANDLE OutRtv);

    INT    OverrideActorMap;   // see OverrideShadowPreset - kept so a live preset change can apply them again
    INT    OverrideTaps;
    INT    OverrideFullRes;
    INT    MsaaWanted;      // see RequestMsaa
    INT    MsaaCount;       // what the card actually gives, 1 when there is no multisampling
    // MSAA. The WORLD draws into these four instead of the single-sample targets, and the moment the world is done
    // they are RESOLVED into those - so the shadows, the motion vectors, the upscaler, the post filters and the HUD
    // all see exactly the same single-sample pictures they saw before, and not one of them had to change.
    //
    // Colour, light and the added target resolve in hardware. The depth cannot: D3D12 resolves colour formats only,
    // so ours goes through a small pass of its own that writes sample 0 into SV_Depth - and, with the shadows on,
    // the surface kind into SV_StencilRef, which is the one thing that needs a capability (bStencilRef).
    TComRef<ID3D12Resource>       MsaaColor, MsaaLight, MsaaAdded, MsaaDepth;
    TComRef<ID3D12DescriptorHeap> MsaaRtvHeap;    // 3: colour, light, added
    TComRef<ID3D12DescriptorHeap> MsaaDsvHeap;    // 1
    TComRef<ID3D12DescriptorHeap> MsaaSrvHeap;    // 2: the multisampled depth, and its stencil plane
    UBOOL  bStencilRef;        // PSSpecifiedStencilRef; without it the MSAA goes, not the shadows
    UBOOL  bMsaaResolved;      // this frame's world has already been resolved (SwitchToOutput can be reached twice)
    INT    MsaaSampleCount() const { return (MsaaCount > 1 && MsaaColor) ? MsaaCount : 1; }
    UBOOL  MsaaOn() const { return MsaaSampleCount() > 1; }
    // True while the world is being drawn into the multisampled targets - after the HUD's switch it is the
    // single-sample ones again, and the solid pass has to pick its pipelines accordingly.
    UBOOL  MsaaWorldLive() const { return MsaaOn() && !bMsaaResolved; }
    INT    PickMsaaCount(INT Wanted);          // what the card gives for Wanted, with the shadow rule applied
    // ... and everything that has to follow it. Between frames only. bMakeTargets is 0 when a Resize is coming
    // anyway: it builds them at the right size, and building them twice means holding two sets for a moment.
    UBOOL  ApplyMsaaCount(INT Count, UBOOL bMakeTargets);
    UBOOL  CreateMsaaTargets();
    void   ReleaseMsaaTargets();
    void   ResolveMsaaWorld(ID3D12GraphicsCommandList* Cmd);
    void   ResolveMsaaDepth(ID3D12GraphicsCommandList* Cmd, INT Index);
    INT    PostAaKind;      // see RequestPostAa
    FLOAT  PostAaSpan;
    INT    FxaaReady;       // 0 not tried, 1 up, -1 the pipeline did not build
    TComRef<IDXGIAdapter3> MemoryAdapter;   // see VideoMemoryMB
    UBOOL  bLinearMix;
    INT    UpscaleMode;        // see RequestUpscaling: 0 off, 1 Quality, 2 Balanced, 3 Performance
    UBOOL  bUpscaleLinear;     // ... the bilinear pass instead of the temporal upscaler, for an A/B
    TComRef<ID3D12Resource>       UpscaleTemp;      // the middle picture at the output's size, for a post filter to read
    TComRef<ID3D12DescriptorHeap> UpscaleRtvHeap;
    TComRef<ID3D12DescriptorHeap> UpscaleSrvHeap;
    // Motion vectors (SetViewCamera): two numbers a pixel at the scene's size - where the pixel was on the previous
    // frame, in pixels. FFinalPass::DrawMotion makes them from the world's depth and the two cameras; a temporal
    // upscaler (FSR 3.1) reads them.
    TComRef<ID3D12Resource>       MotionTarget;
    // A refused allocation stays refused until something changes, and the something here is the size. Without this
    // the renderer asked again on every frame - measured with the create forced to fail: 4133 refusals in one short
    // run, each with its own line in the log, on top of the work the asking itself costs.
    UBOOL                         bMotionFailed = 0;
    INT                           MotionFailedW = 0;
    INT                           MotionFailedH = 0;
    UBOOL                         bFsr3Failed   = 0;
    INT                           Fsr3FailedW   = 0;
    INT                           Fsr3FailedH   = 0;
    TComRef<ID3D12DescriptorHeap> MotionRtvHeap;
    TComRef<ID3D12DescriptorHeap> MotionSrvHeap;
    INT    MotionW, MotionH;   // the target's size (the scene's, remade when that changes)
    INT    MotionReady;        // 0 before the pipeline was tried, 1 built, -1 failed (then the pass is left out)
    INT    MotionViewReady;    // the same for the diagnostic view of them (depth view 6)
    UBOOL  bMotionWanted;      // see RequestMotionVectors
    UBOOL  bMotionForced;      // the diagnostic view asked for them - then they are drawn with the upscaler off too
    UBOOL  bMotionDone;        // the vectors were made this frame (the world's end, or the frame's end as a fallback)
    UBOOL  bJitterWanted;      // see RequestJitter
    INT    JitterFrame;        // counts frames for the jitter's sequence
    INT    MotionProbeMode;    // see RequestMotionProbe - the self-test, a measurement only
    // FSR 3.1 (RequestFsr3). The upscaler writes through an unordered access view, which the output target
    // cannot give it - that one is typeless storage with two render target views - so it gets a target of its
    // own and the last step of the upscale is a one-to-one copy from it onto the output target.
    FFsr3Pass Fsr3;
    TComRef<ID3D12Resource>       Fsr3Output;
    TComRef<ID3D12DescriptorHeap> Fsr3SrvHeap;
    INT    Fsr3W, Fsr3H;       // the size that target was made for (the output's, remade when it changes)
    INT    Fsr3Ready;          // 0 before the context was tried, 1 up, -1 failed (then the bilinear pass runs)
    UBOOL  bNativeMipBias;     // see RequestNativeMipBias
    FLOAT  MipBiasLogged;      // the bias last written to the log, so a change is said once
    UBOOL  bFsr3Wanted;        // see RequestFsr3
    UBOOL  bFsr3Reset;         // the next dispatch throws the collected history away (the first frame, a level change)
    FLOAT  Fsr3Sharpness;      // see SetFsr3Sharpness
    UBOOL  bFsr3ForceFail;     // see RequestFsr3ForceFail
    // The upscale through the 64-bit host (RequestUpscaler).
    FHostUpscale Host;
    // The rays through the host (RtHostPasses): what feeds them; the frame's bodies as FRtPass listed them (-1: not
    // listed); whether the host traced this frame's rays; whether keeping the lists for it was refused once (not tried
    // again every frame); and the host stopped together with what the rays shared with it.
    FHostRayFeed RayFeed;
    INT    RtHitsViewSerial;   // the share whose list's views are in the texture heap (the heads' in the masks' place)
    INT    RtHostBodies;
    UBOOL  bRtHostTraced;
    UBOOL  bRtHostRefused;
    // The host as SyncHost started it: with which backend (Host64::EBackend) - one started for the rays alone has
    // no upscaler - and whether a host that came up for the rays alone found that its card traces none.
    UINT   HostBackend          = 0;
    UBOOL  bRtHostUseless       = 0;
    void   StopHost() { Host.Stop(); RayFeed.Abandon(); }
    INT    UpscalerChoice;     // see RequestUpscaler: 0 Auto, 1 Builtin, 2 DLSS, 3 XeSS
    UBOOL  bHostUpscaled;      // the last frame's upscale went through the host
    // Frame generation (RequestFrameGen).
    FFramePacer Pacer;
    UBOOL  bFrameGenWanted;    // the setting
    UBOOL  bFgMode;            // the swap chain is on the pacer's queue: the frame's picture goes into a texture
    UBOOL  bFgFrame;           // this frame generates: prepared with its upscale, generated at its end
    UBOOL  bFgLast;            // the last frame did (the readout's)
    UBOOL  bHudLessDone;       // this frame's HUD-less picture is drawn (SwitchToOutput)
    UBOOL  bFgReset;           // this frame's upscale threw its history away: so does its generated picture
    INT    FgWhy;              // see FrameGenWhy - this frame's, as it is decided
    INT    FgWhyLast;          // ... and the last finished frame's, which the readout says
    // The frame's finished picture in frame generation's mode, where it does not go to the host (a frame that does not
    // generate): the pacer copies it into a back buffer. Made when first needed, let go with the sizes and formats.
    TComRef<ID3D12Resource>       FgFinal;
    TComRef<ID3D12DescriptorHeap> FgFinalRtvHeap;
    UBOOL  EnsureFgFinal();
    UBOOL  RecreateSwapChain();
    void   CaptureHudLess(ID3D12GraphicsCommandList* Cmd);
    // The bodies that move by themselves - see SetMotionBodies - and the pass that draws them (0 not tried, 1 up, -1
    // failed).
    FMotionBody   MotionBodies[MAX_MOTION_BODIES];
    INT           MotionBodyCount;
    INT           MotionObjReady;
    DXGI_FORMAT   MotionObjFormat;  // the depth format its pipelines were made for
    FLOAT         MotionNearer;     // see RequestMotionDepth
    UBOOL         bMotionNoDepth;
    INT           MotionObjDrawn;   // bodies drawn with their own vector since the log line
    INT           MotionBodiesLast; // ... and in the last frame that drew the vectors
    INT           MotionVertsDrawn; // of those since the log line, the ones with a place for every vertex
    // The motion buffer: a frame slot's own, written while the frame is made and read by its draws (MotionVertices).
    enum { MOTION_VERTEX_MAX = 262144 };
    TComRef<ID3D12Resource> MotionVb[BACKEND_FRAME_COUNT];
    FLOAT*        MotionVbMapped[BACKEND_FRAME_COUNT];
    INT           MotionVbUsed;     // vertices of this frame's
    INT           MotionVbState;    // 0 not tried, 1 there, -1 could not be made
    INT           MotionVbRefused;  // bodies turned away since the log line: the buffer was full
    FLOAT         JitterClip[2];    // the jitter the geometry pass used, in clip units
    FLOAT  JitterPx[2];        // the offset the geometry pass used this frame, in pixels - the upscaler is told it
    FLOAT  FrameDeltaMs;       // the time since the previous frame: how much the blended picture covers
    UBOOL   AmdAntiLagUp() const { return AntiLag.m_pAntiLagAPI ? 1 : 0; }
    void    PaceThroughHost();
    // The host's path's pacing (AntiLagFrameEnd): when the engine went on to read its input, this frame's first
    // submission, whether a hand-over to the host was among them; the fence values of the last two frames; the recent
    // frames' CPU times (input to first submission) and GPU times (point 0 to point 5), seconds; and the counts for its
    // log line.
    UBOOL    bPaceHostWanted = 1;
    UBOOL    bPaceHostFrame  = 0;
    DOUBLE   PaceWake        = 0.0;
    DOUBLE   PaceFirst       = 0.0;
    UINT64   PaceFenceLast   = 0;
    UINT64   PaceFencePrev   = 0;
    INT      PaceSlotLast    = -1;     // the frame slots of the same two frames: their timestamps
    INT      PaceSlotPrev    = -1;
    HANDLE   PaceEvent       = nullptr;
    enum { PACE_KEPT = 16 };
    DOUBLE   PaceCpu[PACE_KEPT] = {};
    DOUBLE   PaceGpu[PACE_KEPT] = {};
    INT      PaceCpuCount = 0, PaceGpuCount = 0;
    INT      PaceFrames   = 0, PaceWaits = 0;
    DOUBLE   PaceWaited   = 0.0;
    RdxAntiLag2::Context AntiLag;              // SetAntiLag
    HRESULT              AntiLagHr      = E_FAIL;
    UBOOL                bAntiLagWanted = 0;
    LONGLONG LatencyPresentAt = 0;  // LastPresentQpc
    // The measurement's too: the Present's moment of the frame drawn with each slot, until its GPU times are read
    // (ReadGpuTime logs the GPU's end of that frame beside it - with the driver's latency help on, the Present returns
    // before the GPU is through).
    UBOOL    bLatencyStamps = 0;
    LONGLONG LatencySlotPresent[BACKEND_FRAME_COUNT] = {};
    UINT64   LatencyJob       = 0;  // LastPacerJob
    LONGLONG PrevFrameQpc;
    UBOOL  EnsureFsr3Target();
    UBOOL  MakeMotionTarget();         // the targets themselves; EnsureMotionTarget asks it once a size
    UBOOL  EnsureMotionTarget();
    UBOOL  MakeFsr3Target();           // the same pair for the upscaler's own output
    void   DrawMotionVectors(ID3D12GraphicsCommandList* Cmd);
    UBOOL  bHudQueued;     // this frame's switch to the output target is queued (BeginHud)
    UBOOL  bWorldEndQueued;   // or, where there is no switch, the world's end for the splashes (BeginHud)
    UBOOL  bOutputLive;    // this frame's scene is in the output target already (ResolveScene)
    UBOOL  bFireSparks = 0;    // SetFireSparks
    UBOOL  bHeatHaze   = 0;    // SetHeatHaze
    FLOAT  FireSpeed   = 1.0f; // SetFireSpeed
    UBOOL  bInFrame;
    // Frames whose world ended at the frame's end, no HUD tile having come, of the frames counted (EndFrame's line).
    INT    LateWorldEnds       = 0;
    INT    LateWorldEndFrames  = 0;

    // Out-of-frame uploads: opened lazily on the first precache offer, submitted at the
    // start of the next frame so the frame's own fence covers its staging buffers too.
    ID3D12CommandAllocator*            UploadAllocator;
    TComRef<ID3D12GraphicsCommandList> UploadList;
    UBOOL                              bUploadOpen;
    UBOOL OpenUploadList();
    void  SubmitUploadList();
    // FCardWriter: a buffer on the card's own memory, and what goes into it - through CardStaging, mapped for good, a
    // piece of CARD_STAGING_BYTES at a time, on a list of its own (not the upload list: sending that lets the texture
    // cache's staging go, which a frame's list may still be copying from), each piece waited for (WaitForGpu).
    virtual ID3D12Resource* MakeCardBuffer(UINT64 Bytes, const TCHAR* What);
    virtual UBOOL WriteCardBuffer(ID3D12Resource* Dest, UINT64 At, const void* Data, UINT64 Bytes);
    void  ReleaseCardWriter();
    enum { CARD_STAGING_BYTES = 4 << 20 };
    TComRef<ID3D12Resource>            CardStaging;
    BYTE*                              CardStagingMapped = nullptr;
    TComRef<ID3D12CommandAllocator>    CardAllocator;
    TComRef<ID3D12GraphicsCommandList> CardList;
    UBOOL  bAllowTearing;
    UBOOL  bExclusive;     // we put the swap chain in DXGI's fullscreen state
    // Alt+Tab hides the window: Present then returns DXGI_STATUS_OCCLUDED, and a tearing-enabled present is not
    // valid in that state. While occluded we only test whether presenting is possible again (DXGI_PRESENT_TEST)
    // and put nothing on the screen.
    UBOOL  bOccluded;

    // The device is gone: Present answered DXGI_ERROR_DEVICE_REMOVED or _RESET. Nothing we own is usable any
    // more - every resource belonged to that device - and no amount of trying again brings it back.
    //
    // This is remembered rather than merely logged because a lost device is SILENT. The game does not stop, the
    // message loop keeps turning, and Present fails INSTANTLY - so the frame times get shorter and everything
    // that reads them reports a very fast machine. A measurement once ran sixty more modes past this point and
    // finished without an error, with a table full of invention. What the flag is for, in order of importance:
    // to close the frame capture so no invented row is ever written, to say it once instead of twice a frame
    // (13818 lines of it, the first time), and to put it on the readout where it cannot be missed.
    UBOOL  bDeviceLost;
    // A Resize has let the targets go and has not made them all again: set where it begins, cleared where it ends
    // well. Whoever asked for the change may put the sizes before it back with another Resize; where that fails too,
    // or nobody tries, there is nothing whole to draw into (see TargetsUnmade).
    UBOOL  bTargetsUnmade;
#if RDX_TEST_DEVICE_LOSS
    UBOOL  bTestLoss = 0;      // see RequestTestLoss
    UBOOL  bTestLossTaken = 0; // this backend's device has been taken away
#endif
    UBOOL  bDredWanted;                 // see RequestDred

    // See RequestGpuConvert.
    UBOOL  bGpuConvert;

    // See GetEndFrameParts.
    DOUBLE EndParts[5];

    // The debug layer's message queue, when RequestValidation asked for it - see
    // DrainDebugMessages.
    UBOOL                    bValidation;
    UBOOL                    bGpuValidation;
    TComRef<ID3D12InfoQueue> InfoQueue;
    enum { MAX_DEBUG_IDS = 128 };
    INT                      DebugIds[MAX_DEBUG_IDS];     // message ids seen so far ...
    INT                      DebugCounts[MAX_DEBUG_IDS];  // ... and how often each came
    INT                      DebugIdCount;
    void DrainDebugMessages(UBOOL bFinal);

    // The frame-time capture - see StartFrameCapture. Lines gather here and go to the file a
    // buffer at a time (about a second of frames), so a game closed by force loses at most that.
    enum { FRAMECAP_BYTES = 8192, FRAMECAP_LABEL = 128 };
    HANDLE   FrameFile;
    char     FrameCapBuf[FRAMECAP_BYTES];
    INT      FrameCapLen;
    LONGLONG FrameCapPrevQpc;
    // What is running, written into every row - see SetFrameCaptureLabel.
    char     FrameCapLabel[FRAMECAP_LABEL];
    void     FlushFrameCapture();

    TCHAR  AdapterName[128];
    UINT   AdapterVendor;
    char   AdapterLabel[40];    // AdapterName for the readout
    TCHAR  GpuWanted[64];       // the GPU key's text, as RequestGpu gave it
    char   GpuKeyLabel[24];     // ... for the readout
    INT    GpuKeyState;         // see GetGpuKeyState
    UBOOL  bDisplayAskAll;      // see RequestDisplayAskAll
    UBOOL  bSafeMode;           // see RequestSafeMode
};

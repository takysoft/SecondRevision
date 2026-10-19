// SecondRevisionHost64 - the DLSS backend: NVIDIA's upscaler through the NGX SDK (docs/terv-fsr4.md, M5).
//
// NGX itself is part of NVIDIA's display driver; the DLSS model is nvngx_dlss.dll, found beside the host. Where there is
// no NVIDIA card or driver, NGX does not start, and Load says so - the renderer then runs its own upscaler.
//
// The protocol's conventions (Host64Protocol.h) are what DLSS asks for as they are: the motion in render pixels, where a
// pixel was minus where it is; the jitter in render pixels; the depth 1 nearest, 0 far (DLSS's "inverted" depth).
//
// Frame generation is NGX's own feature too (nvngx_dlssg.dll beside the host), on the cards that have it. It only makes
// the picture between two frames, into a texture: when the pictures go out is the caller's business, as with FSR's
// frame generation here - the renderer's pacer presents both.
//
// The NGX headers stay in Host64Dlss.cpp: nothing else needs them.
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "Host64Protocol.h"

// What a measurement can turn on a card that has DLSS: the conventions that cannot be tried on any other card. Each is
// a marker the host reads as it starts (Host64Main.cpp).
enum EDlssProbe : uint32_t
{
    DlssProbe_JitterFlipX   = 1u << 0,   // the upscaler's jitter with its sign turned on that axis
    DlssProbe_JitterFlipY   = 1u << 1,
    DlssProbe_MotionFlipX   = 1u << 2,   // the motion vectors taken the other way on that axis, by both features
    DlssProbe_MotionFlipY   = 1u << 3,
    DlssProbe_FgTranspose   = 1u << 4,   // frame generation's matrices laid out for column vectors
    DlssProbe_FgNoJitter    = 1u << 5,   // ... its jitter left out
    DlssProbe_FgJitterFlip  = 1u << 6,   // ... or with its sign turned
    DlssProbe_FgNoHudLess   = 1u << 7,   // ... the HUD-less picture not given
    DlssProbe_FgInvalidZero = 1u << 8,   // ... a motion of exactly zero said to mean "no motion written here"
};

class FHostDlss
{
public:
    typedef void (*FLogFunc)(const char* Line);

    ~FHostDlss() { Shutdown(); }

    // Before Load: EDlssProbe bits.
    void SetProbe(uint32_t Flags) { Probe = Flags; }
    // Before Load, a measurement too: what frame generation's linear depth (1 / depth: the distance in the level's
    // units, the near plane being one unit out) is multiplied by before NGX's heuristics see it. 0: NGX's own.
    void SetFgDepthScale(float Scale) { FgDepthScale = Scale; }
    // Starts NGX on this device and asks whether DLSS can run. False: it cannot - Why says why.
    bool Load(ID3D12Device* Device, FLogFunc Log, char* Why, size_t WhyChars);
    // The upscaler for these sizes (the most the render can be, the output), made on the next Upscale and made again when
    // they change. bHdr: the colour is linear light above 1.0. Whether the one there now has to go first: NeedsRebuild -
    // the GPU must be done with it before it goes (the host waits).
    void Prepare(uint32_t RenderW, uint32_t RenderH, uint32_t OutputW, uint32_t OutputH, bool bHdr);
    bool NeedsRebuild() const { return Feature && bStale; }
    // What the last Prepare said of the colour - not HDR before the first.
    bool Hdr() const { return bHdrNow; }
    // DLSS - and its frame generation, where PrepareFg has its sizes - made now, into List, where they are not there:
    // the making can take seconds the first time on a machine, so the host does it as it takes the textures, which the
    // renderer waits for, rather than in a frame. What could not be made is tried again by the frame that needs it.
    void Make(ID3D12GraphicsCommandList* List);
    // The one there now let go - once the GPU is done with it.
    void Release();
    // One frame's upscale into List - DLSS made first where it is not there (or not for these sizes). The textures are in
    // the states the caller put them in: the inputs readable by a compute shader, the output in unordered access.
    bool Upscale(ID3D12GraphicsCommandList* List, ID3D12Resource* const Resources[Host64::Resource_Count],
                 const Host64::FFrame& F);
    void Shutdown();

    // What runs, e.g. "DLSS 310.9.1", after Load; empty before.
    const char* Running() const { return Version; }

    // Frame generation: whether this card and driver have it, known after Load.
    bool HasFrameGen() const { return bFgAvailable; }
    // Its sizes - the most the render can be, the display - and the back buffer's format (a DXGI_FORMAT). It is made on
    // the next Generate. Called with the GPU done with everything: what was made for other sizes is let go here.
    void PrepareFg(uint32_t RenderW, uint32_t RenderH, uint32_t DisplayW, uint32_t DisplayH, uint32_t Format);
    // A frame's generation in the two halves the host gives every backend. PrepareFrame keeps the frame's depth and
    // motion - copies of its own, because DLSS takes them together with the finished picture, and by then the shared
    // ones may hold the next frame's - and its camera; the shared two are copy sources while it runs. Generate makes
    // the picture between the last frame's Present and this one's, into Generated - with the HUD kept where the HudLess
    // picture is given; the inputs are readable by a compute shader, Generated is in unordered access. A Generate
    // without its PrepareFrame first is refused.
    bool PrepareFrame(ID3D12GraphicsCommandList* List, ID3D12Resource* const Resources[Host64::Resource_Count],
                      const Host64::FFrame& F);
    bool Generate(ID3D12GraphicsCommandList* List, ID3D12Resource* const Resources[Host64::Resource_Count],
                  const Host64::FFrame& F);
    // What generates, e.g. "DLSS FG 310.9.1", after Load where the card has it; empty otherwise.
    const char* RunningFg() const { return FgVersion; }

private:
    bool MakeUpscaler(ID3D12GraphicsCommandList* List);
    bool MakeFg(ID3D12GraphicsCommandList* List);
    void ReleaseFg();

    ID3D12Device* Device    = nullptr;
    FLogFunc      Log       = nullptr;
    uint32_t      Probe     = 0;         // EDlssProbe bits
    float         FgDepthScale = 0.0f;   // a measurement's scale on frame generation's linear depth; 0: NGX's own
    bool          bStarted  = false;     // NGX is up on Device
    void*         Params    = nullptr;   // NVSDK_NGX_Parameter*: the map every call takes
    void*         Feature   = nullptr;   // NVSDK_NGX_Handle*: DLSS for the sizes below
    uint32_t      RenderW = 0, RenderH = 0, OutW = 0, OutH = 0;
    bool          bHdrNow   = false;
    bool          bStale    = false;     // Feature was made for other sizes
    char          Version[64] = {};

    bool            bFgAvailable = false;
    void*           FgParams     = nullptr;    // NVSDK_NGX_Parameter*: frame generation's own map
    void*           FgFeature    = nullptr;    // NVSDK_NGX_Handle*: frame generation for the sizes below
    uint32_t        FgRenderW = 0, FgRenderH = 0, FgW = 0, FgH = 0, FgFormat = 0;
    ID3D12Resource* FgKept[2]    = {};         // this frame's depth and motion, kept from PrepareFrame to Generate
    Host64::FFrame  FgFrame      = {};         // the frame PrepareFrame took: its camera, its sizes, its jitter
    Host64::FFrame  FgBefore     = {};         // ... and the one before it, for the way the camera moved
    bool            bFgPrepared  = false;      // PrepareFrame ran and Generate has not yet
    bool            bFgBefore    = false;      // FgBefore holds a frame
    unsigned        FgLastCode   = 0;          // the last evaluate's result: said when it changes
    char            FgVersion[64] = {};
};

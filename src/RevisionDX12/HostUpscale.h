// SecondRevision - the temporal upscale through the 64-bit host (docs/terv-fsr4.md; src/Host64).
//
// FSR 4 comes only as 64-bit DLLs, and the game is a 32-bit process. SecondRevisionHost64.exe runs them for us; this is
// the backend's side of it: it starts the host, keeps the textures the two share, copies the frame's inputs into them
// and splits the frame where the upscale happens.
//
// The frame's own targets are not shared: the inputs are COPIED into textures made for the host. A shared texture has
// to be in COMMON whenever it changes hands, and the scene target, the depth buffer and the motion target are drawn
// into all frame long - put through COMMON every frame they would lose whatever the driver keeps compressed about them.
// The copies cost a few tenths of a millisecond at 4K; the upscaled picture itself is written straight into a shared
// texture and read from there.
//
// The split: the frame's first half - the world, and the copies - is closed and executed, the frame is handed to the
// host (its queue waits for ours, ours for its), and the SAME command list is opened again on the same allocator for
// the second half. Nothing that was set on the list survives that: whatever the second half draws sets its own state.
//
// Knows the backend's D3D12 objects, not the backend: FBackend decides when, this does the how.
#pragma once

#include "../Host64/Host64Link.h"

class FHostUpscale
{
public:
    FHostUpscale();
    ~FHostUpscale();

    // The host started for Device, not waited for (Poll says when it is up), asked for Backend's library (a
    // Host64::EBackend: Fsr, Dlss). It is looked for in the folder SecondRevisionHost64 beside our DLL. bDebugLayer: the
    // D3D12 debug layer in the host too (a validation run).
    void  Start(ID3D12Device* Device, UBOOL bDebugLayer, UINT Backend);
    // The host asked to leave (ended if it does not) and the shared textures let go. Whatever the GPU still has to do
    // must not read them: the caller waits for it first.
    void  Stop();
    // Once a frame, and while a fence is waited for: a starting host that answered is up; one that failed, died or
    // hung is down, and every GPU wait for it lets go. Cheap, never blocks.
    void  Poll();

    UBOOL IsOn() const       { return bOn; }        // started and not stopped: starting, up or down
    UBOOL IsStarting() const;
    UBOOL IsUp() const;
    UBOOL IsDown() const;
    // Up, with textures handed over that it is still making its upscaler for.
    UBOOL IsPreparing() const;
    // What runs ("FSR 4.1.1"), or why nothing does - the host's last word, plain ASCII, as of the last Poll.
    const char* Message() const { return Said; }
    // The host's own GPU time for a frame's upscale, in microseconds (a running average; 0 before it is known).
    UINT  WorkUs() const;

    // Whether this frame can go through the host: its textures for these sizes and formats handed over AND taken. New
    // ones are made and handed over here, and the host makes its upscaler for them - which can take seconds the first
    // time on a machine - while the built-in upscaler draws the frames (False meanwhile; Poll moves it on). False too
    // where the link is down. Called where the frame's upscale is decided: the textures it replaces were last used
    // before the resize that changed the sizes, and that resize waited for the GPU.
    // With bFrameGen the textures frame generation needs come too, at the display's size and in the back buffer's
    // format (BackFormat): the finished picture, the same without the HUD, and the generated one. A card that cannot
    // write that format through an unordered access view gets none of them - the upscale goes on without.
    UBOOL Prepare(ID3D12Device* Device, INT RenderW, INT RenderH, INT OutW, INT OutH, DXGI_FORMAT SceneFormat,
                  DXGI_FORMAT OutputFormat, UBOOL bFrameGen = 0, DXGI_FORMAT BackFormat = DXGI_FORMAT_UNKNOWN,
                  INT DisplayW = 0, INT DisplayH = 0);
    // The shared textures let go - after a wait for the GPU, where the sizes or the formats change.
    void  ReleaseTextures();

    // The frame's inputs into the shared textures: Sources are the colour, the depth (its depth plane is taken), the
    // motion vectors and the reactive mask (may be null), in the States given; they are back in them after. One barrier
    // batch before the copies and one after, for the frame's textures and the host's alike.
    void  CopyInputs(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* const Sources[4],
                     const D3D12_RESOURCE_STATES States[4]);
    // The split: Cmd closed and executed on Queue, the frame handed to the host, Cmd opened again on Allocator - with
    // nothing set on it. Frame.Flags gets FrameFlag_Reset added when the textures are new. False: Cmd could not be
    // opened again, and the frame cannot go on.
    UBOOL Handover(ID3D12GraphicsCommandList* Cmd, ID3D12CommandAllocator* Allocator, ID3D12CommandQueue* Queue,
                   Host64::FFrame& Frame);

    // The upscaled picture, in COMMON when the second half starts; a view of it for the pixel shaders in OutputHeap.
    ID3D12Resource*       GetOutput() const     { return Textures[Host64::Resource_Output].Get(); }
    ID3D12DescriptorHeap* GetOutputHeap() const { return OutputHeap.Get(); }

    // Frame generation: whether it can run with the textures handed over (the host has it, and took them), and those
    // textures - the finished picture and the HUD-less one with a render target view each, in COMMON between uses; the
    // generated picture, which the host writes.
    UBOOL FrameGenReady() const;
    ID3D12Resource*             GetPresent() const   { return Textures[Host64::Resource_Present].Get(); }
    ID3D12Resource*             GetHudLess() const   { return Textures[Host64::Resource_HudLess].Get(); }
    ID3D12Resource*             GetGenerated() const { return Textures[Host64::Resource_Generated].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE PresentRtv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE HudLessRtv() const;
    // The frame's generated picture asked for: Queue signals the host that the finished pictures are drawn. Nobody waits
    // for the result here - whoever copies it out waits for DoneFence to reach DoneValue, in its own order (FFramePacer).
    // False: the link is down.
    UBOOL SubmitFrameGen(ID3D12CommandQueue* Queue, Host64::FFrame& Frame);
    ID3D12Fence* DoneFence() const { return Link.HostFence(); }
    UINT64       DoneValue() const { return (UINT64)Link.LastSubmitted(); }
    // The host's own GPU time for a generated frame, in microseconds.
    UINT  WorkFgUs() const;

    // The rays (docs/terv-rt-host.md, HostRays.h): the passes the host has for this card - Host64::ERayPass bits; 0
    // while it is not up, or where it traces none - and the link itself, for what feeds them (FHostRayFeed).
    UINT         RayPasses() const { return IsUp() ? (UINT)Link.RayPasses() : 0; }
    FHost64Link& GetLink() { return Link; }

private:
    // Start, and what it was last called with - for starting the host once more when it died on its way up
    // (Poll, Restarts).
    void  Launch();
    FHost64Link                   Link;
    ID3D12Device*                 StartDevice;
    UBOOL                         bStartDebugLayer;
    UINT                          StartBackend;
    INT                           Restarts;      // since the caller last started it
    UBOOL                         bOn;
    UBOOL                         bFresh;        // the textures were just handed over: the next frame has no history
    TComRef<ID3D12Resource>       Textures[Host64::Resource_Count];
    TComRef<ID3D12DescriptorHeap> OutputHeap;
    TComRef<ID3D12DescriptorHeap> RtvHeap;          // the finished picture's and the HUD-less one's render target views
    UINT                          RtvStep;
    INT                           RenderW, RenderH, OutW, OutH;
    UBOOL                         bFrameGenAsked;   // the textures were made with frame generation asked for ...
    DXGI_FORMAT                   BackFormat;       // ... in this format, at this size
    INT                           DisplayW, DisplayH;
    INT                           FailedW, FailedH;   // the sizes the textures could not be made at: not tried again
    DXGI_FORMAT                   SceneFormat, OutputFormat;
    char                          Said[Host64::MessageChars];
};

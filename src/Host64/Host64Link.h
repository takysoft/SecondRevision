// SecondRevision - the renderer's end of the 64-bit host (docs/terv-fsr4.md, Host64Protocol.h, Host64Main.cpp).
//
// Starts SecondRevisionHost64.exe for the renderer's device, hands it the shared textures and fences, and passes it the
// frames. Knows nothing of the engine - Windows and Direct3D 12 only - so the probe (tests/Host64Probe) runs the very
// same code without the game.
//
// A frame, from the renderer's side:
//   1. the frame's first half is drawn and executed on the queue, and every shared texture it hands over is in COMMON;
//   2. Submit: the queue signals the host's fence with the frame's number, the frame's numbers go into the block, the
//      host is woken, and the queue is told to wait for the host's fence with the same number;
//   3. the frame's second half is executed on the same queue - the GPU starts it when the host's work is done. The CPU
//      never waits for the host.
// If the host dies or hangs, the renderer's queue would wait for ever: Poll notices, signals the host's fence from the
// CPU so every wait lets go, and from then on the link is down and the renderer does the work itself.
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "Host64Protocol.h"

class FHost64Link
{
public:
    typedef void (*FLogFunc)(const char* Line);

    enum EState
    {
        State_Off,        // never started, or stopped
        State_Starting,   // the host runs and has not answered yet
        State_Up,         // frames may be submitted
        State_Down,       // it failed, died or hung; Message() says why. Start again to retry.
    };

    FHost64Link() {}
    ~FHost64Link() { Stop(); }

    void SetLog(FLogFunc Func) { LogFunc = Func; }
    // The D3D12 debug layer in the host too, from its next start - a validation run's, never a player's.
    void SetDebugLayer(bool bOn) { bDebugLayer = bOn; }

    // Starts the host (HostExe: its full path) for Device, with the backend Want (a Host64::EBackend). TimeoutMs 0: the
    // call returns as soon as the process runs, and Poll says when it has answered; otherwise it waits for the answer at
    // most that long. False: it could not be started, or (waiting) did not come up - Message() says why.
    bool Start(ID3D12Device* Device, const wchar_t* HostExe, uint32_t Want, DWORD TimeoutMs);
    void Stop();

    // Once a frame or so, never blocking: a starting host that answered is up, one that failed, died or took longer
    // than StartMs is down; a share the host took is done, one it did not take in its time is down; an up host that
    // died, gave up, refused a long run of frames, or left a frame of ours waiting for HangMs is down too.
    EState Poll(DWORD StartMs, DWORD HangMs);

    EState      State() const { return Now; }
    // Down because the host's process ended while it was starting - not a failure it reported, nor a start that took
    // too long: what a crash on the way up looks like.
    bool        EndedWhileStarting() const { return bEndedStarting; }
    bool        IsUp() const { return Now == State_Up; }
    // The host's last word: what runs ("FSR 4.1.1"), or why it stopped. Read afresh each call.
    const char* Message();
    // Frames whose work the host's backend refused so far: their output is not their picture.
    uint32_t    FramesFailed() const { return Block ? *(volatile const uint32_t*)&Block->FramesFailed : 0; }
    uint32_t    Backends() const { return Block ? Block->Backends : 0; }
    uint32_t    Services() const { return Block ? Block->Services : 0; }
    // The host's own GPU time for an upscale's submission and for a generated frame's, in microseconds (running
    // averages; 0 before they are known).
    uint32_t    WorkUs() const { return Block ? *(volatile const uint32_t*)&Block->WorkUs : 0; }
    uint32_t    WorkFgUs() const { return Block ? *(volatile const uint32_t*)&Block->WorkFgUs : 0; }

    // The shared textures, by Host64::EResource (null: none). Each must have been made shared (MakeShared). The host
    // lets go of the old ones, opens these and makes its backend's context for them, which can take seconds the first
    // time; BeginShare only hands them over, and Poll says when the host has taken them (Shared) or failed to within
    // TimeoutMs (down). The caller keeps the textures alive, and no frame is submitted meanwhile.
    bool BeginShare(ID3D12Resource* const Resources[Host64::Resource_Count], DWORD TimeoutMs);
    bool Sharing() const { return Now == State_Up && bSharing; }
    bool Shared() const  { return Now == State_Up && !bSharing && ShareGeneration != 0; }
    // BeginShare, and the answer waited for here (the probe's way).
    bool Share(ID3D12Resource* const Resources[Host64::Resource_Count], DWORD TimeoutMs);

    // One frame, as above: Queue signals the host that the frame's inputs are ready; WaitQueue (Queue where null) waits
    // for the host's result - a queue of its own where only presenting needs it, so that the rendering queue goes on.
    // bWait false: no queue waits here - the caller has one wait for HostFence at LastSubmitted, where its own order puts
    // it. Frame.Frame is ignored and numbered here. False: the link is down, or a share is not taken yet.
    bool Submit(ID3D12CommandQueue* Queue, Host64::FFrame& Frame, ID3D12CommandQueue* WaitQueue = nullptr,
                bool bWait = true);
    // The host's fence and the number the last Submit gave its frame: what a queue waits for to see that frame's work
    // done (the link down, the fence is past every frame).
    ID3D12Fence* HostFence() const { return ToRenderer; }
    uint64_t     LastSubmitted() const { return NextFrame - 1; }

    // The rays (Host64Protocol.h, FRayShare): the resources by Host64::ERayResource (null: none), each made shared; the
    // corners the level's three buffers hold; and how many bodies a frame's list has room for. Handed over as the
    // textures are, with a generation of their own - a new level shares these anew and leaves the upscaler's alone.
    // BeginShareRays only hands them over; Poll says when the host has taken them (RaysShared) or the link is down, and
    // no frame that asks for rays is submitted meanwhile. False: the host traces no rays, something could not be
    // shared (the link stays up, nothing was handed over), or the link is down.
    bool BeginShareRays(ID3D12Resource* const Resources[Host64::Ray_Count], uint32_t LevelVertices, uint32_t SkyVertices,
                        uint32_t ThinVertices, uint32_t BodyRoom, DWORD TimeoutMs);
    bool SharingRays() const { return Now == State_Up && bRaySharing; }
    // The host answered the last share and could take it.
    bool RaysShared() const
    {
        return Now == State_Up && !bRaySharing && RayGeneration != 0 &&
               *(volatile const uint32_t*)&Block->RayTaken == RayGeneration;
    }
    // ... answered it and could NOT take it (its log says why): asking again with the same things gets the same answer.
    bool RaysRefused() const
    {
        return Now == State_Up && !bRaySharing && RayGeneration != 0 &&
               *(volatile const uint32_t*)&Block->RayTaken != RayGeneration;
    }
    // BeginShareRays, and the answer waited for here (the probe's way).
    bool ShareRays(ID3D12Resource* const Resources[Host64::Ray_Count], uint32_t LevelVertices, uint32_t SkyVertices,
                   uint32_t ThinVertices, uint32_t BodyRoom, DWORD TimeoutMs);
    // The list of the frame the next Submit sends: the caller fills it - the passes, where their tables are, and after
    // it the bodies, RayBodyRoom() of them at the most - and then submits that frame with Service_Rays. Null: no rays'
    // share is taken, or the link is down.
    Host64::FRayFrame* RayFrame();
    uint32_t RayBodyRoom() const { return RayRoom; }
    // The passes the host has pipelines for on this card (Host64::ERayPass bits; 0: it traces no rays), its GPU time
    // for a frame's rays in microseconds (a running average), and - with the debug layer on in it - how many errors
    // its layer has had.
    uint32_t RayPasses() const
    {
        return (Block && (Block->Services & Host64::Service_Rays)) ? *(volatile const uint32_t*)&Block->RayPasses : 0;
    }
    uint32_t WorkRayUs() const { return Block ? *(volatile const uint32_t*)&Block->WorkRayUs : 0; }
    uint32_t WorkListUs() const { return Block ? *(volatile const uint32_t*)&Block->WorkListUs : 0; }
    uint32_t WorkListTimed() const { return Block ? *(volatile const uint32_t*)&Block->WorkListTimed : 0; }
    uint32_t HostDebugErrors() const { return Block ? *(volatile const uint32_t*)&Block->DebugErrors : 0; }

    // The up half of Poll: is the host still there, and still keeping up? False (and the link is down) otherwise.
    bool CheckAlive(DWORD HangMs);

    // A texture the host can open: committed, on a shared heap.
    static HRESULT MakeShared(ID3D12Device* Device, const D3D12_RESOURCE_DESC& Desc, D3D12_RESOURCE_STATES Initial,
                              const D3D12_CLEAR_VALUE* Clear, ID3D12Resource** Out);

private:
    void     Log(const char* Format, ...);
    void     Down(const char* Why);
    void     Release();
    void     PollShare();
    void     PollRayShare();
    bool     SlotFree(uint64_t Frame);
    bool     GiveHandle(HANDLE Mine, uint64_t& Theirs);

    FLogFunc        LogFunc    = nullptr;
    ID3D12Device*   Device     = nullptr;
    ID3D12Fence*    ToHost     = nullptr;
    ID3D12Fence*    ToRenderer = nullptr;
    HANDLE          Mapping    = nullptr;
    Host64::FBlock* Block      = nullptr;
    HANDLE          Wake       = nullptr;
    HANDLE          Job        = nullptr;
    HANDLE          Process    = nullptr;
    uint64_t        NextFrame  = 1;
    uint64_t        StartedAt  = 0;       // GetTickCount64 when the process was started
    uint64_t        LagSince   = 0;       // GetTickCount64 since when a frame of ours has waited on the host with no
                                          // frame of the host's finishing; 0: none waits
    uint64_t        LagDone    = 0;       // the host's last finished frame at LagSince
    uint32_t        ShareGeneration = 0;  // the textures' generation last handed over
    bool            bSharing   = false;   // ... and not yet taken
    uint64_t        ShareSince = 0;       // GetTickCount64 when it was handed over
    DWORD           ShareMs    = 0;       // how long the host may take to take it
    HANDLE          RayMapping = nullptr; // the rays' frames' lists: a mapping of the last share's own
    void*           RayFrames  = nullptr;
    uint32_t        RayRoom    = 0;       // the bodies a list has room for
    uint32_t        RayGeneration = 0;    // the rays' share last handed over
    bool            bRaySharing = false;  // ... and not yet answered
    uint64_t        RayShareSince = 0;
    DWORD           RayShareMs = 0;
    EState          Now        = State_Off;
    bool            bDebugLayer = false;
    bool            bEndedStarting = false;
    char            Text[Host64::MessageChars] = {};
};

// SecondRevisionHost64 - the XeSS backend: Intel's upscaler (XeSS-SR) through the XeSS SDK's libxess.dll beside the
// host, loaded at run time - a host without it still serves FSR and DLSS.
//
// XeSS-SR runs on Intel's Arc cards on their matrix units, and on any other card with Shader Model 6.4 - AMD's from the
// RX 5000s, NVIDIA's from the GTX 10s - on the DP4a path. The protocol's conventions (Host64Protocol.h) are XeSS's as
// they are: the motion in render pixels, where a pixel was minus where it is (XeSS's "from the current frame to the
// previous"), at the render size; the jitter in render pixels, taken as FSR takes it; the depth 1 nearest (XeSS's
// inverted depth). The modes' sizes are the renderer's - 1.0, 1.5, 1.7 and 2.0 - which are XeSS's own older factors:
// it is told to keep them.
//
// XeSS's frame generation is not here. It takes the game's swap chain over and presents every picture itself, with
// Intel's own latency reduction in place of any other; the swap chain is the renderer's, in the game's 32-bit process.
//
// The XeSS headers stay in Host64Xess.cpp: nothing else needs them.
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "Host64Protocol.h"

class FHostXess
{
public:
    typedef void (*FLogFunc)(const char* Line);

    ~FHostXess() { Shutdown(); }

    // Loads libxess.dll and makes its context on this device. False: XeSS cannot run here - Why says why.
    bool Load(ID3D12Device* Device, FLogFunc Log, char* Why, size_t WhyChars);
    // The upscaler for these sizes (the most the render can be, the output), made on the next Make or Upscale and made
    // again when they change. bHdr: the colour is linear light above 1.0. Whether the one there now has to be made
    // again first: NeedsRebuild - the GPU must be done with everything that used it (the host waits).
    void Prepare(uint32_t RenderW, uint32_t RenderH, uint32_t OutputW, uint32_t OutputH, bool bHdr);
    bool NeedsRebuild() const { return bMade && bStale; }
    // What the last Prepare said of the colour - not HDR before the first.
    bool Hdr() const { return bHdrNow; }
    // XeSS made now for the sizes Prepare said: its making is blocking and builds its pipelines, which can take a while
    // the first time, so the host does it as it takes the textures rather than in a frame.
    bool Make();
    // One frame's upscale into List - XeSS made first where it is not there (or not for these sizes). The textures are
    // in the states the caller put them in: the inputs readable by a compute shader, the output in unordered access.
    // XeSS sets descriptor heaps of its own on List.
    bool Upscale(ID3D12GraphicsCommandList* List, ID3D12Resource* const Resources[Host64::Resource_Count],
                 const Host64::FFrame& F);
    void Shutdown();

    // What runs, e.g. "XeSS 3.0.2", after Load; empty before.
    const char* Running() const { return Version; }

private:
    ID3D12Device* Device   = nullptr;
    HMODULE       Library  = nullptr;
    void*         Context  = nullptr;   // xess_context_handle_t
    uint32_t      RenderW = 0, RenderH = 0, OutW = 0, OutH = 0;
    bool          bHdrNow  = false;
    bool          bMade    = false;     // xessD3D12Init ran for some sizes
    bool          bStale   = false;     // ... for other sizes than the ones Prepare said
    char          Version[64] = {};
};

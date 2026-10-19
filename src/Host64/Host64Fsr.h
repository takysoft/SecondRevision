// SecondRevisionHost64 - the FSR backend: AMD's upscaler through the FSR API's signed DLLs (docs/terv-fsr4.md).
//
// The loader (amd_fidelityfx_loader_dx12.dll) is loaded at run time from beside the host, and it finds the effect DLLs
// there too. Without a version override the API picks the best upscaler this card has - FSR 4 where the card can run it,
// FSR 3.1 elsewhere - and the host reports which one it got.
//
// The FidelityFX headers stay in Host64Fsr.cpp: nothing else needs them.
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "Host64Protocol.h"

class FHostFsr
{
public:
    typedef void (*FLogFunc)(const char* Line);

    ~FHostFsr() { Shutdown(); }

    // Loads the DLLs and asks which upscalers this device can run. False: none - Why says what is missing.
    bool Load(ID3D12Device* Device, FLogFunc Log, char* Why, size_t WhyChars);
    // The upscaler for these sizes (the most the render can be, the output), made again when they change. bHdr: the
    // colour is linear light above 1.0.
    bool Prepare(uint32_t RenderW, uint32_t RenderH, uint32_t OutputW, uint32_t OutputH, bool bHdr);
    // One frame's upscale into List. The textures are in the states RecordUpscale's caller put them in: the inputs
    // readable by a compute shader, the output in unordered access.
    bool Upscale(ID3D12GraphicsCommandList* List, ID3D12Resource* const Resources[Host64::Resource_Count],
                 const Host64::FFrame& F);
    void Shutdown();

    // What runs, e.g. "FSR 4.1.1", after Prepare; empty before.
    const char* Running() const { return Version; }
    // Frame generation, from the same loader (amd_fidelityfx_framegeneration_dx12.dll beside the host). Whether this
    // device has it at all, known after Load.
    bool HasFrameGen() const { return bFgAvailable; }
    // Its context for these sizes (the most the render can be, the display) and the back buffer's format (a
    // DXGI_FORMAT), made again when they change. HudLessFormat: the HUD-less picture's, where it has one of its own (0:
    // none, or the back buffer's). bHdr: the colours are linear light above 1.0.
    bool PrepareFg(uint32_t RenderW, uint32_t RenderH, uint32_t DisplayW, uint32_t DisplayH, uint32_t Format,
                   uint32_t HudLessFormat, bool bHdr);
    // A frame's generation, in the two halves FSR's frame generation requires in this order (the manual dispatch - the
    // host has no swap chain, the renderer presents): PrepareFrame configures it and takes the frame's depth, motion and
    // camera; Generate makes the frame between the Present picture of the frame before and this one's, into Generated -
    // with the HUD kept where the HudLess picture is given. The textures are in the states the caller put them in: the
    // inputs readable by a compute shader, Generated in unordered access. A Generate without its PrepareFrame first is
    // refused.
    bool PrepareFrame(ID3D12GraphicsCommandList* List, ID3D12Resource* const Resources[Host64::Resource_Count],
                      const Host64::FFrame& F);
    bool Generate(ID3D12GraphicsCommandList* List, ID3D12Resource* const Resources[Host64::Resource_Count],
                  const Host64::FFrame& F);
    // What generates, e.g. "FSR FG 4.0.1", after PrepareFg; empty before.
    const char* RunningFg() const { return FgVersion; }

private:
    ID3D12Device* Device  = nullptr;
    HMODULE       Loader  = nullptr;
    void*         Fns     = nullptr;   // ffxFunctions
    void*         Context = nullptr;   // ffxContext
    uint32_t      MaxRenderW = 0, MaxRenderH = 0, OutW = 0, OutH = 0;
    bool          bHdrNow = false;
    char          Version[64] = {};
    void*         FgContext = nullptr;   // ffxContext
    uint32_t      FgRenderW = 0, FgRenderH = 0, FgW = 0, FgH = 0, FgFormat = 0, FgHudLessFormat = 0;
    bool          bFgHdr = false;
    bool          bFgAvailable = false;
    uint64_t      FgFrame = 0;           // the frame generation's own count: it must go up by exactly one a frame
    bool          bFgPrepared = false;   // PrepareFrame ran for FgFrame and Generate has not yet
    char          FgVersion[64] = {};
};

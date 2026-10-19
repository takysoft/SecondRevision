// SecondRevision - FSR 3.1, the SDK side. See Fsr3Pass.h for what this is and why the
// FidelityFX headers appear in this file and nowhere else.
#include "Pch.h"
#include "Log.h"
#include "Fsr3Pass.h"

// The engine's Core declares clock() and unclock() as macros - its own cycle counters. The
// FidelityFX headers reach the C runtime's <ctime> through their own includes, and there the
// macro turns the declaration of clock() into a syntax error. Neither macro is used in this
// file, so they go away before the SDK is parsed.
#undef clock
#undef unclock

// After Pch.h, so these are parsed at the default structure packing - the engine's 4-byte
// region is closed by then. The SDK's own headers require it, exactly as the D3D12 ones do.
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>

// The backend keeps its bookkeeping for a fixed number of effect contexts, and the scratch
// memory is sized for the same number. We run one effect; the spare is headroom for the
// internal contexts the SDK's documentation warns some effects create.
static const size_t GFsr3MaxContexts = 2;

// The SDK talks back through this - with DEBUG_CHECKING on it reports anything it finds
// wrong with what we hand it, which is the fastest way to catch a bad frame's parameters.
static void Fsr3Message(FfxMsgType Type, const wchar_t* Message)
{
    if (Message)
        RdxLog(TEXT("FSR3 (%i): %s"), (INT)Type, (const TCHAR*)Message);
}

FFsr3Pass::FFsr3Pass()
    : Context(nullptr), Interface(nullptr), Scratch(nullptr),
      SceneW(0), SceneH(0), OutputW(0), OutputH(0), bReady(0), Frames(0)
{
}

FFsr3Pass::~FFsr3Pass()
{
    Shutdown();
}

void FFsr3Pass::Shutdown()
{
    if (bReady && Frames > 0)
        RdxLog(TEXT("FSR3: shut down - %i frames went through the upscaler in total"), Frames);
    Frames = 0;
    if (Context)
    {
        if (bReady)
            ffxFsr3UpscalerContextDestroy((FfxFsr3UpscalerContext*)Context);
        appFree(Context);
        Context = nullptr;
    }
    if (Interface)
    {
        appFree(Interface);
        Interface = nullptr;
    }
    if (Scratch)
    {
        appFree(Scratch);
        Scratch = nullptr;
    }
    PrevDepth.Release();
    DilatedDepth.Release();
    DilatedMotion.Release();
    SceneW = SceneH = OutputW = OutputH = 0;
    bReady = 0;
}

// The three shared targets, made exactly as the SDK describes them: it hands back a
// description apiece (format, size, how it will be used) and we create the storage.
UBOOL FFsr3Pass::CreateShared(ID3D12Device* Device)
{
    FfxFsr3UpscalerSharedResourceDescriptions Shared;
    appMemzero(&Shared, sizeof(Shared));
    if (ffxFsr3UpscalerGetSharedResourceDescriptions((FfxFsr3UpscalerContext*)Context, &Shared) != FFX_OK)
    {
        RdxLog(TEXT("FSR3: the shared resources cannot be described"));
        return 0;
    }

    const FfxCreateResourceDescription* Want[3] =
        { &Shared.reconstructedPrevNearestDepth, &Shared.dilatedDepth, &Shared.dilatedMotionVectors };
    TComRef<ID3D12Resource>* Into[3] = { &PrevDepth, &DilatedDepth, &DilatedMotion };

    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (INT i = 0; i < 3; i++)
    {
        const FfxResourceDescription& R = Want[i]->resourceDescription;
        D3D12_RESOURCE_DESC Desc;
        appMemzero(&Desc, sizeof(Desc));
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = R.width;
        Desc.Height           = R.height;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = (R.mipCount > 0) ? (UINT16)R.mipCount : 1;
        Desc.Format           = ffxGetDX12FormatFromSurfaceFormat(R.format);
        Desc.SampleDesc.Count = 1;
        if (R.usage & FFX_RESOURCE_USAGE_UAV)
            Desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (R.usage & FFX_RESOURCE_USAGE_RENDERTARGET)
            Desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        // They start where the SDK says they start, and every dispatch leaves them there.
        if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   __uuidof(ID3D12Resource), Into[i]->VoidSlot())))
        {
            RdxLog(TEXT("FSR3: shared resource %i (%ix%i) could not be created"), i, (INT)R.width, (INT)R.height);
            return 0;
        }
    }
    return 1;
}

UBOOL FFsr3Pass::Init(ID3D12Device* Device, INT RenderW, INT RenderH, INT OutW, INT OutH, UBOOL bDebugChecking)
{
    Shutdown();
    if (!Device || RenderW <= 0 || RenderH <= 0 || OutW <= 0 || OutH <= 0)
        return 0;

    const size_t ScratchSize = ffxGetScratchMemorySizeDX12(GFsr3MaxContexts);
    Scratch   = appMalloc((INT)ScratchSize, TEXT("Fsr3Scratch"));
    Interface = appMalloc((INT)sizeof(FfxInterface), TEXT("Fsr3Interface"));
    Context   = appMalloc((INT)sizeof(FfxFsr3UpscalerContext), TEXT("Fsr3Context"));
    if (!Scratch || !Interface || !Context)
    {
        RdxLog(TEXT("FSR3: out of memory (%i bytes were needed)"), (INT)ScratchSize);
        Shutdown();
        return 0;
    }
    appMemzero(Scratch, (INT)ScratchSize);
    appMemzero(Interface, sizeof(FfxInterface));
    appMemzero(Context, sizeof(FfxFsr3UpscalerContext));

    FfxErrorCode Err = ffxGetInterfaceDX12((FfxInterface*)Interface, ffxGetDeviceDX12(Device),
                                           Scratch, ScratchSize, GFsr3MaxContexts);
    if (Err != FFX_OK)
    {
        RdxLog(TEXT("FSR3: the DX12 backend did not come up (error %i)"), (INT)Err);
        Shutdown();
        return 0;
    }

    FfxFsr3UpscalerContextDescription Desc;
    appMemzero(&Desc, sizeof(Desc));
    // Our geometry pass writes reversed depth: z/w = 1/W, so the near plane is 1, the far plane is 0, and there is
    // no far clip at all (FSolidPass VSMain). The first two flags say exactly that.
    // Auto exposure is needed because no exposure texture is handed over.
    Desc.flags = FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED |
                 FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE |
                 FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    // Only for a validation run: the SDK checks every parameter of every dispatch and reports through our log. It costs
    // time on every frame, so a measurement must not have it (docs/fsr3-atadas.md 5.).
    if (bDebugChecking)
        Desc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    Desc.maxRenderSize.width   = (uint32_t)RenderW;
    Desc.maxRenderSize.height  = (uint32_t)RenderH;
    Desc.maxUpscaleSize.width  = (uint32_t)OutW;
    Desc.maxUpscaleSize.height = (uint32_t)OutH;
    Desc.fpMessage             = &Fsr3Message;
    Desc.backendInterface      = *(FfxInterface*)Interface;

    Err = ffxFsr3UpscalerContextCreate((FfxFsr3UpscalerContext*)Context, &Desc);
    if (Err != FFX_OK)
    {
        RdxLog(TEXT("FSR3: the context could not be created (error %i)"), (INT)Err);
        appFree(Context);
        Context = nullptr;
        Shutdown();
        return 0;
    }
    // From here on the context exists, so Shutdown must destroy it.
    bReady = 1;

    if (!CreateShared(Device))
    {
        Shutdown();
        return 0;
    }

    SceneW  = RenderW;
    SceneH  = RenderH;
    OutputW = OutW;
    OutputH = OutH;
    Frames = 0;
    RdxLog(TEXT("FSR3: up - %ix%i -> %ix%i (%i kB scratch)%s"),
           SceneW, SceneH, OutputW, OutputH, (INT)(ScratchSize / 1024),
           bDebugChecking ? TEXT("; the SDK's own checking is ON (validation, slower)") : TEXT(""));
    return 1;
}

UBOOL FFsr3Pass::Dispatch(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* Colour, ID3D12Resource* Depth,
                          ID3D12Resource* Motion, ID3D12Resource* Output,
                          FLOAT JitterX, FLOAT JitterY, FLOAT FrameMs, FLOAT FovY, UBOOL bReset, FLOAT Sharpness,
                          ID3D12Resource* Reactive)
{
    if (!bReady || !Cmd || !Colour || !Depth || !Motion || !Output)
        return 0;

    FfxFsr3UpscalerDispatchDescription Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.commandList = ffxGetCommandListDX12(Cmd);

    // The descriptions come from the resources themselves, so nothing here has to be told what format anything
    // is in - which matters because the scene's format follows the HDR key now. Where storage is still typeless
    // (the depth, and the colour in the eight-bit mode) the backend maps it to the format a shader may actually
    // read it through; above eight bits the targets are named outright, because the SDK's own typeless mapping
    // knows the formats it was written against and R10G10B10A2_TYPELESS was not one of them.
    Desc.color = ffxGetResourceDX12(Colour, ffxGetResourceDescriptionDX12(Colour), L"RDX_Colour",
                                    FFX_RESOURCE_STATE_COMPUTE_READ);
    Desc.depth = ffxGetResourceDX12(Depth, ffxGetResourceDescriptionDX12(Depth), L"RDX_Depth",
                                    FFX_RESOURCE_STATE_COMPUTE_READ);
    Desc.motionVectors = ffxGetResourceDX12(Motion, ffxGetResourceDescriptionDX12(Motion), L"RDX_Motion",
                                            FFX_RESOURCE_STATE_COMPUTE_READ);
    Desc.output = ffxGetResourceDX12(Output, ffxGetResourceDescriptionDX12(Output, FFX_RESOURCE_USAGE_UAV),
                                     L"RDX_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    if (Reactive)
        Desc.reactive = ffxGetResourceDX12(Reactive, ffxGetResourceDescriptionDX12(Reactive), L"RDX_Reactive",
                                           FFX_RESOURCE_STATE_COMPUTE_READ);

    Desc.reconstructedPrevNearestDepth =
        ffxGetResourceDX12(PrevDepth.Get(), ffxGetResourceDescriptionDX12(PrevDepth.Get(), FFX_RESOURCE_USAGE_UAV),
                           L"RDX_PrevDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    Desc.dilatedDepth =
        ffxGetResourceDX12(DilatedDepth.Get(), ffxGetResourceDescriptionDX12(DilatedDepth.Get(), FFX_RESOURCE_USAGE_UAV),
                           L"RDX_DilatedDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    Desc.dilatedMotionVectors =
        ffxGetResourceDX12(DilatedMotion.Get(), ffxGetResourceDescriptionDX12(DilatedMotion.Get(), FFX_RESOURCE_USAGE_UAV),
                           L"RDX_DilatedMotion", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    Desc.jitterOffset.x = JitterX;
    Desc.jitterOffset.y = JitterY;
    // Ours are already the movement in pixels (FFinalPass::DrawMotion writes was - here),
    // which is what the SDK works in - so nothing to convert.
    Desc.motionVectorScale.x = 1.0f;
    Desc.motionVectorScale.y = 1.0f;
    Desc.renderSize.width    = (uint32_t)SceneW;
    Desc.renderSize.height   = (uint32_t)SceneH;
    Desc.upscaleSize.width   = (uint32_t)OutputW;
    Desc.upscaleSize.height  = (uint32_t)OutputH;
    Desc.enableSharpening    = true;
    Desc.sharpness           = Sharpness;     // the ini key Fsr3Sharpness; 0.5 is the SDK's middle
    Desc.frameTimeDelta      = FrameMs;
    Desc.preExposure         = 1.0f;
    Desc.reset               = bReset ? true : false;
    Desc.cameraNear          = 1.0f;          // z/w = 1/W: the near plane is one unit out
    Desc.cameraFar           = FLT_MAX;       // no far plane at all - hence DEPTH_INFINITE
    Desc.cameraFovAngleVertical = FovY;
    Desc.viewSpaceToMetersFactor = 0.0f;      // 0: the SDK's own default

    const FfxErrorCode Err = ffxFsr3UpscalerContextDispatch((FfxFsr3UpscalerContext*)Context, &Desc);
    if (Err != FFX_OK)
    {
        static INT Told = 0;
        if (Told++ < 3)
            RdxLog(TEXT("FSR3: the dispatch failed (error %i)"), (INT)Err);
        return 0;
    }
    // That the upscaler really ran is a measurement, not an assumption: the first frame says so, and every 600th after it.
    // Before this line the only sign was the absence of errors, and a silent fallback to the bilinear pass would look exactly the same.
    if (++Frames == 1 || (Frames % 600) == 0)
        RdxLog(TEXT("FSR3: frame %i went through the upscaler (jitter %.2f, %.2f px; frame time %.2f ms)"), Frames,
               JitterX, JitterY, FrameMs);
    return 1;
}

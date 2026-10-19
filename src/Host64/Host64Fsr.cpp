// SecondRevisionHost64 - the FSR backend. See Host64Fsr.h.

#include "Host64Fsr.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"
#include "ffx_framegeneration.h"

static FHostFsr::FLogFunc GFsrLog = nullptr;

static void FsrLog(const char* Format, const char* A = "", unsigned long long B = 0)
{
    if (!GFsrLog)
        return;
    char Line[512];
    sprintf_s(Line, Format, A, B);
    GFsrLog(Line);
}

// The API's own messages - with debug checking on, everything it finds wrong with a frame's parameters.
static void FsrMessage(uint32_t Type, const wchar_t* Message)
{
    if (!GFsrLog || !Message)
        return;
    char Text[400];
    ::WideCharToMultiByte(CP_ACP, 0, Message, -1, Text, sizeof(Text) - 1, nullptr, nullptr);
    Text[sizeof(Text) - 1] = 0;
    char Line[480];
    sprintf_s(Line, "fsr (%s): %s", Type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", Text);
    GFsrLog(Line);
}

static ffxFunctions& Functions(void* P) { return *(ffxFunctions*)P; }

bool FHostFsr::Load(ID3D12Device* Dev, FLogFunc Log, char* Why, size_t WhyChars)
{
    Shutdown();
    GFsrLog = Log;
    Device  = Dev;
    // From beside the host, and nowhere else: a copy of the loader somewhere on the path is not ours to trust.
    wchar_t Path[MAX_PATH];
    const DWORD N = ::GetModuleFileNameW(nullptr, Path, MAX_PATH);
    wchar_t* Slash = (N > 0 && N < MAX_PATH) ? wcsrchr(Path, L'\\') : nullptr;
    if (!Slash || wcscpy_s(Slash + 1, MAX_PATH - (Slash + 1 - Path), L"amd_fidelityfx_loader_dx12.dll") != 0)
    {
        strncpy_s(Why, WhyChars, "fsr: the host's own folder is unknown", _TRUNCATE);
        return false;
    }
    Loader = ::LoadLibraryExW(Path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!Loader)
    {
        sprintf_s(Why, WhyChars, "fsr: amd_fidelityfx_loader_dx12.dll is not beside the host (error %lu)",
                  ::GetLastError());
        return false;
    }
    ffxFunctions* F = new ffxFunctions;
    memset(F, 0, sizeof(*F));
    ffxLoadFunctions(F, Loader);
    Fns = F;
    if (!F->CreateContext || !F->DestroyContext || !F->Query || !F->Dispatch || !F->Configure)
    {
        strncpy_s(Why, WhyChars, "fsr: the loader does not have the FSR API's five functions", _TRUNCATE);
        return false;
    }

    // The upscalers this device can run, as the API names them: the log says, whatever is chosen.
    uint64_t Count = 0;
    ffxQueryDescGetVersions Q = {};
    Q.header.type    = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    Q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    Q.device         = Device;
    Q.outputCount    = &Count;
    if (F->Query(nullptr, &Q.header) != FFX_API_RETURN_OK || Count == 0)
    {
        strncpy_s(Why, WhyChars, "fsr: no upscaler for this device", _TRUNCATE);
        return false;
    }
    uint64_t    Ids[16]   = {};
    const char* Names[16] = {};
    Count          = (Count < 16) ? Count : 16;
    Q.versionIds   = Ids;
    Q.versionNames = Names;
    if (F->Query(nullptr, &Q.header) == FFX_API_RETURN_OK)
        for (uint64_t i = 0; i < Count; i++)
            FsrLog("fsr: this device can run %s (id %llu)", Names[i] ? Names[i] : "?", Ids[i]);

    // Frame generation, the same way. Not having it is no failure: the upscaler works without.
    uint64_t FgCount = 0;
    ffxQueryDescGetVersions G = {};
    G.header.type    = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    G.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    G.device         = Device;
    G.outputCount    = &FgCount;
    bFgAvailable = F->Query(nullptr, &G.header) == FFX_API_RETURN_OK && FgCount > 0;
    if (bFgAvailable)
    {
        uint64_t    FgIds[16]   = {};
        const char* FgNames[16] = {};
        FgCount        = (FgCount < 16) ? FgCount : 16;
        G.versionIds   = FgIds;
        G.versionNames = FgNames;
        if (F->Query(nullptr, &G.header) == FFX_API_RETURN_OK)
            for (uint64_t i = 0; i < FgCount; i++)
                FsrLog("fsr: this device can generate frames with %s (id %llu)", FgNames[i] ? FgNames[i] : "?", FgIds[i]);
    }
    else
        FsrLog("fsr: no frame generation for this device%s (count %llu)", "", FgCount);
    return true;
}

bool FHostFsr::PrepareFg(uint32_t RenderW, uint32_t RenderH, uint32_t DisplayW, uint32_t DisplayH, uint32_t Format,
                         uint32_t HudLessFormat, bool bHdr)
{
    if (!Fns || !bFgAvailable)
        return false;
    if (FgContext && RenderW == FgRenderW && RenderH == FgRenderH && DisplayW == FgW && DisplayH == FgH &&
        Format == FgFormat && HudLessFormat == FgHudLessFormat && bHdr == bFgHdr)
        return true;
    ffxFunctions& F = Functions(Fns);
    if (FgContext)
    {
        F.DestroyContext(&FgContext, nullptr);
        FgContext = nullptr;
    }
    FgVersion[0] = 0;

    // The renderer's conventions, as for the upscaler: depth 1 at the near plane down to 0 at an infinite far one.
    ffxCreateContextDescFrameGeneration Desc = {};
    Desc.header.type          = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    Desc.flags                = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE |
                                (bHdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0u);
    Desc.displaySize.width    = DisplayW;
    Desc.displaySize.height   = DisplayH;
    Desc.maxRenderSize.width  = RenderW;
    Desc.maxRenderSize.height = RenderH;
    Desc.backBufferFormat     = ffxApiGetSurfaceFormatDX12((DXGI_FORMAT)Format);
    ffxCreateBackendDX12Desc Backend = {};
    Backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    Backend.device      = Device;
    ffxCreateContextDescFrameGenerationVersion Built = {};
    Built.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    Built.version     = FFX_FRAMEGENERATION_VERSION;
    Desc.header.pNext    = &Backend.header;
    Backend.header.pNext = &Built.header;
    // A HUD-less picture in a format of its own has to be named at creation; one in the back buffer's needs nothing.
    ffxCreateContextDescFrameGenerationHudless HudLess = {};
    HudLess.header.type             = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    HudLess.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12((DXGI_FORMAT)HudLessFormat);
    if (HudLessFormat && HudLessFormat != Format)
        Built.header.pNext = &HudLess.header;
    const ffxReturnCode_t Made = F.CreateContext(&FgContext, &Desc.header, nullptr);
    if (Made != FFX_API_RETURN_OK)
    {
        FgContext = nullptr;
        FsrLog("fsr: frame generation would not come up%s (code %llu)", "", Made);
        return false;
    }
    ffxQueryGetProviderVersion Which = {};
    Which.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (F.Query(&FgContext, &Which.header) == FFX_API_RETURN_OK && Which.versionName)
        sprintf_s(FgVersion, "FSR FG %s", Which.versionName);
    else
        strcpy_s(FgVersion, "FSR FG");
    FgRenderW = RenderW;
    FgRenderH = RenderH;
    FgW       = DisplayW;
    FgH       = DisplayH;
    FgFormat  = Format;
    FgHudLessFormat = HudLessFormat;
    bFgHdr    = bHdr;
    FgFrame   = 0;
    char Line[200];
    sprintf_s(Line, "fsr: %s up - render up to %ux%u, display %ux%u, back buffer format %u%s", FgVersion, RenderW, RenderH,
              DisplayW, DisplayH, Format, bHdr ? ", HDR" : "");
    if (GFsrLog)
        GFsrLog(Line);
    return true;
}

bool FHostFsr::PrepareFrame(ID3D12GraphicsCommandList* List, ID3D12Resource* const Resources[Host64::Resource_Count],
                            const Host64::FFrame& Fr)
{
    ID3D12Resource* const Depth  = Resources[Host64::Resource_Depth];
    ID3D12Resource* const Motion = Resources[Host64::Resource_Motion];
    bFgPrepared = false;
    if (!FgContext || !Depth || !Motion)
        return false;
    ffxFunctions& F      = Functions(Fns);
    const bool    bReset = (Fr.Flags & Host64::FrameFlag_Reset) != 0;
    const uint64_t Id    = ++FgFrame;
    static int Told = 0;

    // 1. Configure: on, no swap chain to tell anything - the renderer presents both frames itself. The HUD-less picture,
    // where there is one, is this frame's: the work runs on this queue, not an async one, so the next frame may
    // overwrite it.
    ID3D12Resource* const HudLess = Resources[Host64::Resource_HudLess];
    ffxConfigureDescFrameGeneration C = {};
    C.header.type                   = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    C.swapChain                     = nullptr;
    C.frameGenerationEnabled        = true;
    C.allowAsyncWorkloads           = false;
    C.flags                         = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    if (HudLess)
        C.HUDLessColor = ffxApiGetResourceDX12(HudLess, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    C.generationRect.left           = 0;
    C.generationRect.top            = 0;
    C.generationRect.width          = (int32_t)FgW;
    C.generationRect.height         = (int32_t)FgH;
    C.frameID                       = Id;
    ffxReturnCode_t Rc = F.Configure(&FgContext, &C.header);
    if (Rc != FFX_API_RETURN_OK)
    {
        if (Told++ < 3)
            FsrLog("fsr: frame generation's configure failed%s (code %llu)", "", Rc);
        return false;
    }

    // 2. Prepare: this frame's depth, motion and camera.
    ffxDispatchDescFrameGenerationPrepareV2 P = {};
    P.header.type              = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    P.frameID                  = Id;
    P.flags                    = 0;
    P.commandList              = List;
    P.renderSize.width         = Fr.RenderWidth;
    P.renderSize.height        = Fr.RenderHeight;
    P.jitterOffset.x           = Fr.JitterX;
    P.jitterOffset.y           = Fr.JitterY;
    P.motionVectorScale.x      = 1.0f;
    P.motionVectorScale.y      = 1.0f;
    P.frameTimeDelta           = Fr.FrameTimeMs;
    P.reset                    = bReset;
    P.cameraNear               = (Fr.NearZ > 0.0f) ? Fr.NearZ : 1.0f;
    P.cameraFar                = FLT_MAX;
    P.cameraFovAngleVertical   = Fr.FovY;
    P.viewSpaceToMetersFactor  = Fr.UnitsToMeters;
    P.depth                    = ffxApiGetResourceDX12(Depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    P.motionVectors            = ffxApiGetResourceDX12(Motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    for (int k = 0; k < 3; k++)
    {
        P.cameraPosition[k] = Fr.CameraPosition[k];
        P.cameraUp[k]       = Fr.CameraUp[k];
        P.cameraRight[k]    = Fr.CameraRight[k];
        P.cameraForward[k]  = Fr.CameraForward[k];
    }
    Rc = F.Dispatch(&FgContext, &P.header);
    if (Rc != FFX_API_RETURN_OK)
    {
        if (Told++ < 3)
            FsrLog("fsr: frame generation's prepare failed%s (code %llu)", "", Rc);
        return false;
    }
    bFgPrepared = true;
    return true;
}

bool FHostFsr::Generate(ID3D12GraphicsCommandList* List, ID3D12Resource* const Resources[Host64::Resource_Count],
                        const Host64::FFrame& Fr)
{
    ID3D12Resource* const Present   = Resources[Host64::Resource_Present];
    ID3D12Resource* const Generated = Resources[Host64::Resource_Generated];
    // The order is the SDK's requirement: this frame's configure and prepare first, then this - once.
    if (!FgContext || !Present || !Generated || !bFgPrepared)
        return false;
    bFgPrepared = false;
    ffxFunctions& F      = Functions(Fns);
    const bool    bReset = (Fr.Flags & Host64::FrameFlag_Reset) != 0;
    const uint64_t Id    = FgFrame;
    static int Told = 0;

    // 3. The generated frame: between the last frame's Present and this one's.
    ffxDispatchDescFrameGeneration D = {};
    D.header.type                = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    D.commandList                = List;
    D.presentColor               = ffxApiGetResourceDX12(Present, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    D.outputs[0]                 = ffxApiGetResourceDX12(Generated, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    D.numGeneratedFrames         = 1;
    D.reset                      = bReset;
    D.backbufferTransferFunction = (Fr.Flags & Host64::FrameFlag_ScRgb) ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB
                                                                        : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    D.minMaxLuminance[0]         = 0.0f;
    D.minMaxLuminance[1]         = 1000.0f;
    D.generationRect.left        = 0;
    D.generationRect.top         = 0;
    D.generationRect.width       = (int32_t)FgW;
    D.generationRect.height      = (int32_t)FgH;
    D.frameID                    = Id;
    const ffxReturnCode_t Rc = F.Dispatch(&FgContext, &D.header);
    if (Rc != FFX_API_RETURN_OK)
    {
        if (Told++ < 3)
            FsrLog("fsr: frame generation's dispatch failed%s (code %llu)", "", Rc);
        return false;
    }
    return true;
}

bool FHostFsr::Prepare(uint32_t RenderW, uint32_t RenderH, uint32_t OutputW, uint32_t OutputH, bool bHdr)
{
    if (!Fns)
        return false;
    if (Context && RenderW == MaxRenderW && RenderH == MaxRenderH && OutputW == OutW && OutputH == OutH &&
        bHdr == bHdrNow)
        return true;
    ffxFunctions& F = Functions(Fns);
    if (Context)
    {
        F.DestroyContext(&Context, nullptr);
        Context = nullptr;
    }
    Version[0] = 0;

    // The renderer's conventions (Host64Protocol.h): depth 1 at the eye's near plane down to 0 at an infinite far one,
    // no exposure texture.
    ffxCreateContextDescUpscale Desc = {};
    Desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    Desc.flags       = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                       (bHdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0u);
    Desc.maxRenderSize.width   = RenderW;
    Desc.maxRenderSize.height  = RenderH;
    Desc.maxUpscaleSize.width  = OutputW;
    Desc.maxUpscaleSize.height = OutputH;
    Desc.fpMessage             = &FsrMessage;
    ffxCreateBackendDX12Desc Backend = {};
    Backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    Backend.device      = Device;
    ffxCreateContextDescUpscaleVersion Built = {};
    Built.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    Built.version     = FFX_UPSCALER_VERSION;
    Desc.header.pNext    = &Backend.header;
    Backend.header.pNext = &Built.header;
    const ffxReturnCode_t Made = F.CreateContext(&Context, &Desc.header, nullptr);
    if (Made != FFX_API_RETURN_OK)
    {
        Context = nullptr;
        FsrLog("fsr: the upscaler would not come up%s (code %llu)", "", Made);
        return false;
    }
    ffxQueryGetProviderVersion Which = {};
    Which.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (F.Query(&Context, &Which.header) == FFX_API_RETURN_OK && Which.versionName)
        sprintf_s(Version, "FSR %s", Which.versionName);
    else
        strcpy_s(Version, "FSR");
    MaxRenderW = RenderW;
    MaxRenderH = RenderH;
    OutW       = OutputW;
    OutH       = OutputH;
    bHdrNow    = bHdr;
    char Line[320];
    sprintf_s(Line, "fsr: %s up - %ux%u -> %ux%u%s", Version, RenderW, RenderH, OutputW, OutputH, bHdr ? ", HDR" : "");
    if (GFsrLog)
        GFsrLog(Line);
    // Which of the frame's inputs this upscaler needs and which it takes when given - the reactive mask among them: an
    // upscaler that does not take it keeps the history the renderer asks it to let go of.
    ffxQueryDescUpscaleGetResourceRequirements Needs = {};
    Needs.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
    if (F.Query(&Context, &Needs.header) == FFX_API_RETURN_OK)
    {
        static const char* const Names[6] = { "colour", "depth", "motion", "exposure", "reactive mask",
                                              "transparency and composition mask" };
        char Req[160] = {}, Opt[160] = {};
        for (int i = 0; i < 6; i++)
        {
            char* const To = (Needs.required_resources & (1ull << i)) ? Req
                           : ((Needs.optional_resources & (1ull << i)) ? Opt : nullptr);
            if (To)
            {
                if (To[0])
                    strcat_s(To, 160, ", ");
                strcat_s(To, 160, Names[i]);
            }
        }
        sprintf_s(Line, "fsr: %s needs %s; takes when given: %s", Version, Req[0] ? Req : "nothing listed",
                  Opt[0] ? Opt : "nothing more");
        if (GFsrLog)
            GFsrLog(Line);
    }
    else if (GFsrLog)
        GFsrLog("fsr: this upscaler does not say which inputs it takes");
    return true;
}

bool FHostFsr::Upscale(ID3D12GraphicsCommandList* List, ID3D12Resource* const Resources[Host64::Resource_Count],
                       const Host64::FFrame& Fr)
{
    ID3D12Resource* const Color  = Resources[Host64::Resource_Color];
    ID3D12Resource* const Depth  = Resources[Host64::Resource_Depth];
    ID3D12Resource* const Motion = Resources[Host64::Resource_Motion];
    ID3D12Resource* const Output = Resources[Host64::Resource_Output];
    if (!Context || !Color || !Depth || !Motion || !Output)
        return false;
    ffxDispatchDescUpscale D = {};
    D.header.type   = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    D.commandList   = List;
    D.color         = ffxApiGetResourceDX12(Color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    D.depth         = ffxApiGetResourceDX12(Depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    D.motionVectors = ffxApiGetResourceDX12(Motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (Resources[Host64::Resource_Reactive] && (Fr.Flags & Host64::FrameFlag_Reactive))
        D.reactive = ffxApiGetResourceDX12(Resources[Host64::Resource_Reactive], FFX_API_RESOURCE_STATE_COMPUTE_READ);
    D.output = ffxApiGetResourceDX12(Output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // The motion vectors are already the movement in pixels, where the pixel was minus where it is - the upscaler's own
    // measure, so nothing to scale.
    D.jitterOffset.x        = Fr.JitterX;
    D.jitterOffset.y        = Fr.JitterY;
    D.motionVectorScale.x   = 1.0f;
    D.motionVectorScale.y   = 1.0f;
    D.renderSize.width      = Fr.RenderWidth;
    D.renderSize.height     = Fr.RenderHeight;
    D.upscaleSize.width     = Fr.OutputWidth;
    D.upscaleSize.height    = Fr.OutputHeight;
    // As the renderer's own FSR 3.1 does it: the sharpening pass always on, the key only saying how much.
    D.enableSharpening      = true;
    D.sharpness             = Fr.Sharpness;
    D.frameTimeDelta        = Fr.FrameTimeMs;
    D.preExposure           = 1.0f;
    D.reset                 = (Fr.Flags & Host64::FrameFlag_Reset) != 0;
    D.cameraNear            = (Fr.NearZ > 0.0f) ? Fr.NearZ : 1.0f;
    D.cameraFar             = FLT_MAX;
    D.cameraFovAngleVertical = Fr.FovY;
    D.viewSpaceToMetersFactor = Fr.UnitsToMeters;
    const ffxReturnCode_t Done = Functions(Fns).Dispatch(&Context, &D.header);
    if (Done != FFX_API_RETURN_OK)
    {
        static int Told = 0;
        if (Told++ < 3)
            FsrLog("fsr: a frame's upscale failed%s (code %llu)", "", Done);
        return false;
    }
    return true;
}

void FHostFsr::Shutdown()
{
    if (Fns && FgContext)
        Functions(Fns).DestroyContext(&FgContext, nullptr);
    FgContext    = nullptr;
    FgVersion[0] = 0;
    bFgAvailable = false;
    if (Fns && Context)
        Functions(Fns).DestroyContext(&Context, nullptr);
    Context = nullptr;
    delete (ffxFunctions*)Fns;
    Fns = nullptr;
    if (Loader)
        ::FreeLibrary(Loader);
    Loader     = nullptr;
    Device     = nullptr;
    Version[0] = 0;
    MaxRenderW = MaxRenderH = OutW = OutH = 0;
}

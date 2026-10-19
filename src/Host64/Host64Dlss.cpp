// SecondRevisionHost64 - the DLSS backend. See Host64Dlss.h.
#include "Host64Dlss.h"
#include "Host64Camera.h"

#include <float.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_dlssg_d3d.h"

// The project's own identifier for NGX: a custom engine names itself with a GUID of its own (DLSS Programming Guide,
// NVSDK_NGX_Init_with_ProjectID).
static const char* const GProjectId     = "d5afb2e7-54e3-4b79-9353-1f040b92bdd3";
static const char* const GEngineVersion = "1.0";

// The directory this executable is in, with a backslash at its end - where nvngx_dlss.dll is looked for.
static bool HostDirectory(wchar_t* Out, size_t Chars)
{
    const DWORD N = ::GetModuleFileNameW(nullptr, Out, (DWORD)Chars);
    if (N == 0 || N >= Chars)
        return false;
    wchar_t* Slash = wcsrchr(Out, L'\\');
    if (!Slash)
        return false;
    Slash[1] = 0;
    return true;
}

// What one of NGX's DLLs beside the host says its version is, e.g. "310.9.1" (empty where it has none to say).
static void DllVersion(const wchar_t* Name, char* Out, size_t Chars)
{
    Out[0] = 0;
    wchar_t Path[MAX_PATH];
    if (!HostDirectory(Path, MAX_PATH) || wcslen(Path) + wcslen(Name) + 1 >= MAX_PATH)
        return;
    wcscat_s(Path, Name);
    DWORD      Ignored = 0;
    const DWORD Size   = ::GetFileVersionInfoSizeW(Path, &Ignored);
    if (!Size)
        return;
    void* const Data = malloc(Size);
    if (!Data)
        return;
    VS_FIXEDFILEINFO* Info = nullptr;
    UINT              Len  = 0;
    if (::GetFileVersionInfoW(Path, 0, Size, Data) && ::VerQueryValueW(Data, L"\\", (void**)&Info, &Len) && Info)
        sprintf_s(Out, Chars, "%u.%u.%u", HIWORD(Info->dwFileVersionMS), LOWORD(Info->dwFileVersionMS),
                  HIWORD(Info->dwFileVersionLS));
    free(Data);
}

bool FHostDlss::Load(ID3D12Device* InDevice, FLogFunc InLog, char* Why, size_t WhyChars)
{
    Shutdown();
    Device = InDevice;
    Log    = InLog;

    // NGX's own files (its logs) go where the host may write whatever the game's folder allows: the temporary folder.
    wchar_t Data[MAX_PATH], Dir[MAX_PATH];
    const DWORD T = ::GetTempPathW(MAX_PATH, Data);
    if (T == 0 || T + 24 >= MAX_PATH || !HostDirectory(Dir, MAX_PATH))
    {
        sprintf_s(Why, WhyChars, "dlss: no folder for its files");
        return false;
    }
    wcscat_s(Data, L"SecondRevisionHost64\\");
    ::CreateDirectoryW(Data, nullptr);

    const wchar_t* Paths[1] = { Dir };
    NVSDK_NGX_FeatureCommonInfo Info = {};
    Info.PathListInfo.Path   = Paths;
    Info.PathListInfo.Length = 1;
    const NVSDK_NGX_Result Init = NVSDK_NGX_D3D12_Init_with_ProjectID(GProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                                      GEngineVersion, Data, Device, &Info);
    if (NVSDK_NGX_FAILED(Init))
    {
        // No NVIDIA card or driver ends here - the usual answer on any other card. The reason first: the readout's line
        // has room for a few words only.
        sprintf_s(Why, WhyChars, "dlss: needs an NVIDIA RTX card and its driver (NGX would not start: 0x%08x)",
                  (unsigned)Init);
        return false;
    }
    bStarted = true;

    // Whether DLSS itself can run - the driver may be too old for it, or the card may not have it.
    NVSDK_NGX_Parameter* Caps = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&Caps)) || !Caps)
    {
        sprintf_s(Why, WhyChars, "dlss: NGX gave no capabilities");
        Shutdown();
        return false;
    }
    int Available = 0, NeedsDriver = 0;
    unsigned int Major = 0, Minor = 0;
    int InitResult = 0;
    NVSDK_NGX_Parameter_GetI(Caps, NVSDK_NGX_Parameter_SuperSampling_Available, &Available);
    NVSDK_NGX_Parameter_GetI(Caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &NeedsDriver);
    NVSDK_NGX_Parameter_GetUI(Caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &Major);
    NVSDK_NGX_Parameter_GetUI(Caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &Minor);
    NVSDK_NGX_Parameter_GetI(Caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &InitResult);
    // Frame generation is asked in the same answer: a feature of its own, which a card can lack while it has the
    // upscaler. How many pictures it could make between two frames is the card's too; one is all this host asks for.
    int FgAvailable = 0, FgNeedsDriver = 0, FgInitResult = 0;
    unsigned int FgMajor = 0, FgMinor = 0, FgMost = 0;
    NVSDK_NGX_Parameter_GetI(Caps, NVSDK_NGX_Parameter_FrameGeneration_Available, &FgAvailable);
    NVSDK_NGX_Parameter_GetI(Caps, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &FgNeedsDriver);
    NVSDK_NGX_Parameter_GetUI(Caps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &FgMajor);
    NVSDK_NGX_Parameter_GetUI(Caps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &FgMinor);
    NVSDK_NGX_Parameter_GetI(Caps, NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &FgInitResult);
    NVSDK_NGX_Parameter_GetUI(Caps, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &FgMost);
    NVSDK_NGX_D3D12_DestroyParameters(Caps);
    if (!Available)
    {
        if (NeedsDriver)
            sprintf_s(Why, WhyChars, "dlss: the driver is too old for it - it needs %u.%u or newer", Major, Minor);
        else
            sprintf_s(Why, WhyChars, "dlss: not available on this card (0x%08x)", (unsigned)InitResult);
        Shutdown();
        return false;
    }

    // The map every call of this backend fills in.
    NVSDK_NGX_Parameter* P = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&P)) || !P)
    {
        sprintf_s(Why, WhyChars, "dlss: NGX gave no parameter map");
        Shutdown();
        return false;
    }
    Params = P;

    char V[32];
    DllVersion(L"nvngx_dlss.dll", V, sizeof(V));
    sprintf_s(Version, "DLSS%s%s", V[0] ? " " : "", V);
    if (Log)
    {
        char Line[128];
        sprintf_s(Line, "dlss: NGX is up - %s", Version);
        Log(Line);
    }

    // Frame generation, where the card has it: a map of its own, so that neither feature's calls leave the other's
    // values behind. Without it nothing fails - the host does not offer the service, and the log says why.
    NVSDK_NGX_Parameter* G = nullptr;
    if (FgAvailable && NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_AllocateParameters(&G)) && G)
    {
        FgParams     = G;
        bFgAvailable = true;
        DllVersion(L"nvngx_dlssg.dll", V, sizeof(V));
        sprintf_s(FgVersion, "DLSS FG%s%s", V[0] ? " " : "", V);
    }
    if (Log)
    {
        char Line[160];
        if (bFgAvailable)
            sprintf_s(Line, "dlss: frame generation is there - %s, up to %u generated picture(s) a frame on this card",
                      FgVersion, FgMost > 1 ? FgMost : 1u);
        else if (FgNeedsDriver)
            sprintf_s(Line, "dlss: no frame generation - the driver is too old for it, it needs %u.%u or newer",
                      FgMajor, FgMinor);
        else
            sprintf_s(Line, "dlss: no frame generation on this card (0x%08x)", (unsigned)FgInitResult);
        Log(Line);
    }
    return true;
}

void FHostDlss::Prepare(uint32_t InRenderW, uint32_t InRenderH, uint32_t InOutW, uint32_t InOutH, bool bHdr)
{
    if (InRenderW == RenderW && InRenderH == RenderH && InOutW == OutW && InOutH == OutH && bHdr == bHdrNow)
        return;
    RenderW = InRenderW;
    RenderH = InRenderH;
    OutW    = InOutW;
    OutH    = InOutH;
    bHdrNow = bHdr;
    bStale  = true;
}

// DLSS's quality setting from how much smaller the render is than the output - the ratios the renderer's modes use.
static NVSDK_NGX_PerfQuality_Value QualityFor(uint32_t RenderW, uint32_t OutW)
{
    const float Ratio = RenderW ? (float)OutW / (float)RenderW : 1.0f;
    if (Ratio < 1.25f)
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (Ratio < 1.6f)
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (Ratio < 1.85f)
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (Ratio < 2.5f)
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

// The upscaler for the sizes Prepare last said, made into List where it is not there. The one made for other sizes has
// to be gone by now: the host waits for the GPU and calls Release first (NeedsRebuild).
bool FHostDlss::MakeUpscaler(ID3D12GraphicsCommandList* List)
{
    if (Feature && !bStale)
        return true;
    if (Feature || !Params || !RenderW || !OutW)
        return false;
    NVSDK_NGX_DLSS_Create_Params C = {};
    C.Feature.InWidth            = RenderW;
    C.Feature.InHeight           = RenderH;
    C.Feature.InTargetWidth      = OutW;
    C.Feature.InTargetHeight     = OutH;
    C.Feature.InPerfQualityValue = QualityFor(RenderW, OutW);
    C.InFeatureCreateFlags       = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                   NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                                   (bHdrNow ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0);
    NVSDK_NGX_Handle* H = nullptr;
    const NVSDK_NGX_Result Made = NGX_D3D12_CREATE_DLSS_EXT(List, 1, 1, &H, (NVSDK_NGX_Parameter*)Params, &C);
    char Line[128];
    if (NVSDK_NGX_FAILED(Made) || !H)
    {
        if (Log)
        {
            sprintf_s(Line, "dlss: it would not be made for %ux%u -> %ux%u (0x%08x)", RenderW, RenderH, OutW, OutH,
                      (unsigned)Made);
            Log(Line);
        }
        return false;
    }
    Feature = H;
    bStale  = false;
    if (Log)
    {
        sprintf_s(Line, "dlss: made for %ux%u -> %ux%u%s", RenderW, RenderH, OutW, OutH, bHdrNow ? ", HDR" : "");
        Log(Line);
    }
    return true;
}

void FHostDlss::Make(ID3D12GraphicsCommandList* List)
{
    MakeUpscaler(List);
    if (bFgAvailable && FgW)
        MakeFg(List);
}

bool FHostDlss::Upscale(ID3D12GraphicsCommandList* List, ID3D12Resource* const R[Host64::Resource_Count],
                        const Host64::FFrame& F)
{
    if (!MakeUpscaler(List))
        return false;
    NVSDK_NGX_Parameter* const P = (NVSDK_NGX_Parameter*)Params;

    NVSDK_NGX_D3D12_DLSS_Eval_Params E = {};
    E.Feature.pInColor                   = R[Host64::Resource_Color];
    E.Feature.pInOutput                  = R[Host64::Resource_Output];
    E.pInDepth                           = R[Host64::Resource_Depth];
    E.pInMotionVectors                   = R[Host64::Resource_Motion];
    E.InJitterOffsetX                    = (Probe & DlssProbe_JitterFlipX) ? -F.JitterX : F.JitterX;
    E.InJitterOffsetY                    = (Probe & DlssProbe_JitterFlipY) ? -F.JitterY : F.JitterY;
    E.InRenderSubrectDimensions.Width    = F.RenderWidth;
    E.InRenderSubrectDimensions.Height   = F.RenderHeight;
    E.InReset                            = (F.Flags & Host64::FrameFlag_Reset) ? 1 : 0;
    E.InMVScaleX                         = (Probe & DlssProbe_MotionFlipX) ? -1.0f : 1.0f;
    E.InMVScaleY                         = (Probe & DlssProbe_MotionFlipY) ? -1.0f : 1.0f;
    E.InFrameTimeDeltaInMsec             = F.FrameTimeMs;
    const NVSDK_NGX_Result Done = NGX_D3D12_EVALUATE_DLSS_EXT(List, (NVSDK_NGX_Handle*)Feature, P, &E);
    return NVSDK_NGX_SUCCEED(Done);
}

void FHostDlss::Release()
{
    if (Feature)
    {
        NVSDK_NGX_D3D12_ReleaseFeature((NVSDK_NGX_Handle*)Feature);
        Feature = nullptr;
    }
    bStale = true;
}

// ---------------------------------------------------------------------------
// Frame generation.
// ---------------------------------------------------------------------------

static void Turn(ID3D12GraphicsCommandList* List, ID3D12Resource* R, D3D12_RESOURCE_STATES From,
                 D3D12_RESOURCE_STATES To)
{
    D3D12_RESOURCE_BARRIER B = {};
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = R;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = From;
    B.Transition.StateAfter  = To;
    List->ResourceBarrier(1, &B);
}

void FHostDlss::PrepareFg(uint32_t InRenderW, uint32_t InRenderH, uint32_t DisplayW, uint32_t DisplayH, uint32_t Format)
{
    if (InRenderW == FgRenderW && InRenderH == FgRenderH && DisplayW == FgW && DisplayH == FgH && Format == FgFormat)
        return;
    ReleaseFg();
    FgRenderW = InRenderW;
    FgRenderH = InRenderH;
    FgW       = DisplayW;
    FgH       = DisplayH;
    FgFormat  = Format;
}

void FHostDlss::ReleaseFg()
{
    if (FgFeature)
    {
        NVSDK_NGX_D3D12_ReleaseFeature((NVSDK_NGX_Handle*)FgFeature);
        FgFeature = nullptr;
    }
    for (int i = 0; i < 2; i++)
        if (FgKept[i])
        {
            FgKept[i]->Release();
            FgKept[i] = nullptr;
        }
    FgFrame     = {};
    bFgPrepared = false;
    bFgBefore   = false;
    FgLastCode  = 0;
}

bool FHostDlss::PrepareFrame(ID3D12GraphicsCommandList* List, ID3D12Resource* const R[Host64::Resource_Count],
                             const Host64::FFrame& F)
{
    bFgPrepared = false;
    ID3D12Resource* const Shared[2] = { R[Host64::Resource_Depth], R[Host64::Resource_Motion] };
    if (!bFgAvailable || !FgW || !Shared[0] || !Shared[1])
        return false;
    for (int i = 0; i < 2; i++)
    {
        D3D12_RESOURCE_DESC Want = Shared[i]->GetDesc();
        if (FgKept[i])
        {
            const D3D12_RESOURCE_DESC Have = FgKept[i]->GetDesc();
            if (Have.Width == Want.Width && Have.Height == Want.Height && Have.Format == Want.Format)
                continue;
            // The shared textures change with a generation, and PrepareFg lets these go then: one of another size
            // here is a frame out of its order, and the GPU may still be reading the kept one.
            return false;
        }
        Want.Flags     = D3D12_RESOURCE_FLAG_NONE;
        Want.MipLevels = 1;
        D3D12_HEAP_PROPERTIES Heap = {};
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Want,
                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&FgKept[i]))))
        {
            FgKept[i] = nullptr;
            if (Log)
                Log("dlss: frame generation has no memory for its copy of the depth and the motion");
            return false;
        }
    }
    for (int i = 0; i < 2; i++)
    {
        Turn(List, FgKept[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        List->CopyResource(FgKept[i], Shared[i]);
        Turn(List, FgKept[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    // The generation's own frame carries no camera: this one's is kept for it, and the one before for how it moved.
    bFgBefore   = FgFrame.Frame != 0;
    FgBefore    = FgFrame;
    FgFrame     = F;
    bFgPrepared = true;
    return true;
}

// Frame generation for the sizes PrepareFg last said, made into List where it is not there.
bool FHostDlss::MakeFg(ID3D12GraphicsCommandList* List)
{
    if (FgFeature)
        return true;
    if (!bFgAvailable || !FgParams || !FgW)
        return false;
    NVSDK_NGX_DLSSG_Create_Params C = {};
    C.Width                    = FgW;
    C.Height                   = FgH;
    C.NativeBackbufferFormat   = FgFormat;
    C.RenderWidth              = FgRenderW;
    C.RenderHeight             = FgRenderH;
    C.DynamicResolutionScaling = false;
    NVSDK_NGX_Handle* H = nullptr;
    const NVSDK_NGX_Result Made = NGX_D3D12_CREATE_DLSSG(List, 1, 1, &H, (NVSDK_NGX_Parameter*)FgParams, &C);
    char Line[160];
    if (NVSDK_NGX_FAILED(Made) || !H)
    {
        // Said when the answer changes, not every frame that tries again.
        if (Log && (unsigned)Made != FgLastCode)
        {
            sprintf_s(Line, "dlss: frame generation would not be made for %ux%u, render %ux%u, format %u (0x%08x)", FgW,
                      FgH, FgRenderW, FgRenderH, FgFormat, (unsigned)Made);
            Log(Line);
        }
        FgLastCode = (unsigned)Made;
        return false;
    }
    FgFeature = H;
    if (Log)
    {
        sprintf_s(Line, "dlss: frame generation made for %ux%u, render %ux%u, back buffer format %u", FgW, FgH,
                  FgRenderW, FgRenderH, FgFormat);
        Log(Line);
    }
    return true;
}

bool FHostDlss::Generate(ID3D12GraphicsCommandList* List, ID3D12Resource* const R[Host64::Resource_Count],
                         const Host64::FFrame& F)
{
    ID3D12Resource* const Present   = R[Host64::Resource_Present];
    ID3D12Resource* const Generated = R[Host64::Resource_Generated];
    if (!Present || !Generated || !bFgPrepared || !FgKept[0] || !FgKept[1])
        return false;
    bFgPrepared = false;
    if (!MakeFg(List))
        return false;
    NVSDK_NGX_Parameter* const P = (NVSDK_NGX_Parameter*)FgParams;

    // The frame PrepareFrame took, and the one before it. With none before - the first frame, or the history thrown
    // away - the camera has not moved, and the generator is told to start over.
    const Host64::FFrame& Now    = FgFrame;
    const bool            bReset = (F.Flags & Host64::FrameFlag_Reset) != 0 || !bFgBefore;
    const Host64::FFrame& Before = bFgBefore ? FgBefore : FgFrame;
    // The camera as matrices (Host64Camera.h): rows first, for row vectors, as the guide asks ("row-major order,
    // assuming post-multiplication") - the other layout is one of the measurements.
    Host64::FMat ToClip     = Host64::ViewToClip(Now);
    Host64::FMat FromClip   = Host64::ClipToView(Now);
    Host64::FMat ClipToPrev = Host64::ClipToClip(Now, Before);
    Host64::FMat PrevToClip = Host64::ClipToClip(Before, Now);
    if (Probe & DlssProbe_FgTranspose)
    {
        ToClip     = Host64::MatTransposed(ToClip);
        FromClip   = Host64::MatTransposed(FromClip);
        ClipToPrev = Host64::MatTransposed(ClipToPrev);
        PrevToClip = Host64::MatTransposed(PrevToClip);
    }
    const Host64::FMat Identity = Host64::MatIdentity();

    NVSDK_NGX_D3D12_DLSSG_Eval_Params E = {};
    E.pBackbuffer        = Present;
    E.pDepth             = FgKept[0];
    E.pMVecs             = FgKept[1];
    E.pHudless           = (Probe & DlssProbe_FgNoHudLess) ? nullptr : R[Host64::Resource_HudLess];
    E.pOutputInterpFrame = Generated;

    NVSDK_NGX_DLSSG_Opt_Eval_Params O = {};
    O.multiFrameCount = 1;
    O.multiFrameIndex = 1;
    memcpy(O.cameraViewToClip, ToClip.M, sizeof(O.cameraViewToClip));
    memcpy(O.clipToCameraView, FromClip.M, sizeof(O.clipToCameraView));
    memcpy(O.clipToLensClip, Identity.M, sizeof(O.clipToLensClip));
    memcpy(O.clipToPrevClip, ClipToPrev.M, sizeof(O.clipToPrevClip));
    memcpy(O.prevClipToClip, PrevToClip.M, sizeof(O.prevClipToClip));
    // The jitter in clip space: the protocol's is in render pixels, y down.
    const float JitterSign = (Probe & DlssProbe_FgJitterFlip) ? -1.0f : 1.0f;
    if (!(Probe & DlssProbe_FgNoJitter) && Now.RenderWidth && Now.RenderHeight)
    {
        O.jitterOffset[0] = JitterSign * 2.0f * Now.JitterX / (float)Now.RenderWidth;
        O.jitterOffset[1] = JitterSign * -2.0f * Now.JitterY / (float)Now.RenderHeight;
    }
    // The motion as the protocol has it is what the generator asks for: where a pixel was minus where it is, in pixels
    // of the motion's own texture.
    O.mvecScale[0] = (Probe & DlssProbe_MotionFlipX) ? -1.0f : 1.0f;
    O.mvecScale[1] = (Probe & DlssProbe_MotionFlipY) ? -1.0f : 1.0f;
    for (int k = 0; k < 3; k++)
    {
        O.cameraPos[k]   = Now.CameraPosition[k];
        O.cameraUp[k]    = Now.CameraUp[k];
        O.cameraRight[k] = Now.CameraRight[k];
        O.cameraFwd[k]   = Now.CameraForward[k];
    }
    O.cameraNear             = Host64::CameraNear(Now);
    // The far plane is infinite; the generator is given a distance no level reaches, not an infinity to divide by.
    O.cameraFar              = 1.0e7f;
    O.cameraFOV              = Now.FovY;
    O.cameraAspectRatio      = Host64::CameraAspect(Now);
    O.colorBuffersHDR        = (F.Flags & Host64::FrameFlag_ScRgb) != 0;
    O.depthInverted          = true;
    O.cameraMotionIncluded   = true;
    O.reset                  = bReset;
    O.notRenderingGameFrames = false;
    O.orthoProjection        = false;
    O.motionVectorsDilated   = false;
    O.menuDetectionEnabled   = false;
    // Every motion vector here is a written one, a still pixel's zero too - the weapon's, which does not move on
    // the screen while the camera turns: the value that would mean "nothing written" is one no vector has.
    O.motionVectorsInvalidValue = (Probe & DlssProbe_FgInvalidZero) ? 0.0f : FLT_MAX;
    // The part of the depth and the motion this frame drew, where it is less than the whole texture.
    const D3D12_RESOURCE_DESC Kept = FgKept[0]->GetDesc();
    if (Now.RenderWidth && Now.RenderHeight && (Now.RenderWidth < Kept.Width || Now.RenderHeight < Kept.Height))
    {
        O.mvecsSubrectSize.Width  = Now.RenderWidth;
        O.mvecsSubrectSize.Height = Now.RenderHeight;
        O.depthSubrectSize.Width  = Now.RenderWidth;
        O.depthSubrectSize.Height = Now.RenderHeight;
    }
    // NGX's depth heuristics count in linear depth, tuned for a near plane further out than this engine's one unit;
    // a measurement can bring the two together. The helper below does not touch this parameter.
    if (FgDepthScale > 0.0f)
        NVSDK_NGX_Parameter_SetF(P, NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale, FgDepthScale);

    const NVSDK_NGX_Result Done = NGX_D3D12_EVALUATE_DLSSG(List, (NVSDK_NGX_Handle*)FgFeature, P, &E, &O);
    // Said when the answer changes - the first picture, the first refusal, the first after a refusal - not every frame.
    if (Log && (unsigned)Done != FgLastCode)
    {
        char Line[160];
        sprintf_s(Line, "dlss: frame generation's evaluate answers 0x%08x (%s)%s", (unsigned)Done,
                  NVSDK_NGX_SUCCEED(Done) ? "a picture" : "refused", bReset ? ", with a reset" : "");
        Log(Line);
    }
    FgLastCode = (unsigned)Done;
    return NVSDK_NGX_SUCCEED(Done);
}

void FHostDlss::Shutdown()
{
    Release();
    ReleaseFg();
    FgRenderW = FgRenderH = FgW = FgH = FgFormat = 0;
    if (FgParams)
    {
        NVSDK_NGX_D3D12_DestroyParameters((NVSDK_NGX_Parameter*)FgParams);
        FgParams = nullptr;
    }
    bFgAvailable = false;
    FgVersion[0] = 0;
    if (Params)
    {
        NVSDK_NGX_D3D12_DestroyParameters((NVSDK_NGX_Parameter*)Params);
        Params = nullptr;
    }
    if (bStarted && Device)
        NVSDK_NGX_D3D12_Shutdown1(Device);
    bStarted = false;
    Device   = nullptr;
    Version[0] = 0;
    RenderW = RenderH = OutW = OutH = 0;
}

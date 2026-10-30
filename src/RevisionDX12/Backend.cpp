#include "Pch.h"
#include "Backend.h"
#include "TextureCache.h"
#include "Log.h"
#include "ViewGeneration.h"
#include "PresentWatch.h"
#include "RenderMath.h"     // RDX_CLASSIC
#include <math.h>
#include <stdio.h>
#include <d3dcompiler.h>     // the ripples' own small pass (StepRipples)
#include "ShaderCache.h"

// A transition barrier for a whole resource (the splashes' targets; defined with them, further down).
static void SplashBarrier(D3D12_RESOURCE_BARRIER& B, ID3D12Resource* R, D3D12_RESOURCE_STATES From, D3D12_RESOURCE_STATES To);

// Every D3D12 entry point returns an HRESULT and every one of them can fail on a
// driver we have never seen. Checking each call at the call site keeps the failure
// message close to the cause, which matters far more here than brevity.
static UBOOL CheckResult(HRESULT Result, const TCHAR* What)
{
    if (SUCCEEDED(Result))
        return 1;
    RdxLog(TEXT("%s failed, hr=0x%08X"), What, (DWORD)Result);
    return 0;
}
// A measurement only: the out-of-memory paths, walked on a card that has memory (Log.h, RdxForcedFailure). Where a
// failure is forced the call is not made at all: a call that is refused leaves nothing behind, and a forced refusal
// has to leave the same - an object made after all would be taken for a whole one by whoever looks at its pointer
// next, and a path that cannot do without the object would never be seen to fall over.
#define Check(Call, What) (RdxForcedFailure(What) ? 0 : CheckResult((Call), (What)))

FBackend::FBackend()
{
    appMemzero(BackBuffers, sizeof(BackBuffers));
    appMemzero(Allocators, sizeof(Allocators));
    appMemzero(FenceValues, sizeof(FenceValues));
    appMemzero(&RtvStart, sizeof(RtvStart));
    RtvStride    = 0;
    FrameIndex   = 0;
    FenceEvent   = nullptr;
    FrameWaitSec = 0.0;
    PrevPresentQpc = 0;
    QpcMs          = 0.0;
    PresentN = PresentLong = PresentShort = PresentLogged = 0;
    PresentMaxGap = PresentMaxCall = 0.0;
    PresentsAfterFlush = 0;
    Width        = 0;
    Height       = 0;
    HdrMode      = RdxMath::Hdr_Off;
    HdrRunning   = RdxMath::Hdr_Off;
    HdrWhy       = RdxMath::HdrFallback_None;
    Colours      = RdxColoursFor(RdxMath::Hdr_Off);
    bDisplayHdr  = 0;
    DisplayNits  = 0.0f;
    DisplayName[0] = 0;
    DisplayBits    = 0;
    DisplayHdrCapable = -1;
    bHdrFalseColour = 0;
    OutputWindow = nullptr;
    SceneScale   = 1;
    UpscaleMode  = 0;
    bUpscaleLinear = 0;
    SceneWidth   = 0;
    SceneHeight  = 0;
    bLinearMix   = 0;
    bHudQueued   = 0;
    bWorldEndQueued = 0;
    bOutputLive  = 0;
    bInFrame     = 0;
    bAllowTearing = 0;
    bExclusive    = 0;
    bOccluded     = 0;
    bDeviceLost   = 0;
    bTargetsUnmade = 0;
    bDredWanted   = 0;
    bValidation   = 0;
    bGpuValidation = 0;
    bGpuConvert   = 0;
    DepthViewMode  = 0;
    DepthViewReady = 0;
    ActiveDepth    = 0;
    RetiredDepth   = -1;
    bShadows       = 0;
    bShadowAbsolute = 0;
    bShadowWorldCast = 1;      // the level's geometry casts too - see RequestShadowWorldCast
    MetricBytes    = 0;
    MetricW = MetricH = MetricPitch = 0;
    MetricPending  = -1;
    MetricFence    = 0;
    ShadowMetric   = -1.0f;
    MetricNextAt   = 0.0;
    MetricLightBytes = 0;
    MetricLightW = MetricLightH = MetricLightPitch = 0;
    ShadowMetricLit  = -1.0f;
    bMetricLit       = 0;
    ShadowStrength  = 1.0f;
    MetricPendingStrength = 1.0f;
    MetricSampleStrength  = 1.0f;
    ShadowPreset   = RdxMath::ShadowPresetFor(RdxMath::ShadowQuality_Off);
    ShadowGeneration = 0;
    WorldDepth     = -1;
    ShadowReady    = 0;
    ShadowW        = 0;
    ShadowH        = 0;
    AoMode         = RdxMath::Ao_Off;
    bAoAlone       = 0;
    AoRadiusSet    = 0.0f;
    AoStrengthSet  = 0.0f;
    AoReady        = 0;
    RtFactorBlurReady = 0;
    AoW            = 0;
    AoH            = 0;
    CaptionText[0][0] = 0;
    CaptionUntil   = 0.0;
    NoticeText[0][0] = 0;
    NoticeUntil    = 0.0;
    FpsSetting     = RdxMath::FpsLimit_Off;
    FpsTarget      = 0;
    RefreshHz      = 0;
    VrrState       = RdxMath::Vrr_NoData;
    bVsync         = 0;
    NextFrameAt    = 0.0;
    LastPresentAt  = 0.0;
    FrameSeconds   = 0.0;
    CaptionReady   = 0;
    TimeMapped     = nullptr;
    SwapBuffers    = BACKEND_FRAME_COUNT;
    for (INT s = 0; s < BACKEND_FRAME_COUNT; s++)
    {
        TimePlaced[s] = 0;
        TimeLive[s]   = 0;
    }
    for (INT k = 0; k < GPU_TIME_PARTS; k++)
    {
        TimeSum[k]   = 0.0;
        TimeMax[k]   = 0.0;
        TimeCount[k] = 0;
    }
    for (INT k = 0; k < GPU_TIME_ROUND_PARTS; k++)
        RoundSum[k] = RoundMax[k] = 0.0;
    TimeRound     = 0;
    RoundFrames   = 0;
    RoundCount    = 0;
    ListTimedLast = 0;
    TimeLastStart = 0;
    TimeGapSum    = 0.0;
    TimeGaps      = 0;
    TimeCpuLast   = 0.0;
    TimeCpuGapSum = 0.0;
    TimeCpuGaps   = 0;
    TimeFrames    = 0;
    TimeTick      = 0.0;
    appMemzero(ShadowAxes, sizeof(ShadowAxes));
    appMemzero(ShadowEye, sizeof(ShadowEye));
    appMemzero(CamEye, sizeof(CamEye));
    appMemzero(CamAxes, sizeof(CamAxes));
    appMemzero(PrevEye, sizeof(PrevEye));
    appMemzero(PrevAxes, sizeof(PrevAxes));
    PrevTanX      = 1.0f;
    PrevTanY      = 0.5625f;
    bCamSet       = 0;
    bPrevCamValid = 0;
    MotionW       = 0;
    MotionH       = 0;
    MotionReady     = 0;
    MotionViewReady = 0;
    bMotionWanted   = 0;
    bMotionForced   = 0;
    WaterLevel      = 0;
    bWaterWanted    = 0;
    ReflectionLevel = 0;
    bNativeMipBias  = 0;
    MipBiasLogged   = 0.0f;
    WaterDepthState = D3D12_RESOURCE_STATE_COMMON;
    WaveW = WaveH   = 0;
    bWaveNew        = 0;
    bWaveFailed     = 0;
    appMemzero(WaveUploadMapped, sizeof(WaveUploadMapped));
    WaterLampMapped  = nullptr;
    bWaterLampFailed = 0;
    bRtShadows       = 0;
    bRtShadowSetting = 0;
    bRtShadowCounts  = 0;
    bRtShadowMaskOff = 0;
    bRtShadowBench   = 0;
    RtShadowOnly     = 0;
    RtKnobLights     = 0;
    RtKnobEdges      = 0;
    RtBenchFrames    = 0;
    bRtAo            = 0;
    RtAoInstances    = 0;
    bRtSceneTried    = 0;
    bRtSceneBuilt    = 0;
    RtHostBodies     = -1;
    RtHitsViewSerial = -1;
    bRtHostTraced    = 0;
    bRtHostRefused   = 0;
    RtMirrorIdle     = RT_MIRROR_IDLE_FRAMES;
    bRtFarViews      = 0;
    RtWaterView      = 0;
    for (INT i = 0; i < RdxMath::SplashMax; i++)
    {
        appMemzero(&Splashes[i], sizeof(Splashes[i]));
        Splashes[i].Start = -1000.0;      // long over
    }
    SplashNext        = 0;
    SplashSeed        = 1;
    appMemzero(SplashOrigin, sizeof(SplashOrigin));
    appMemzero(SplashAxes, sizeof(SplashAxes));
    SplashFocal       = 1.0f;
    SplashCentreX     = SplashCentreY = 0.0f;
    bSplashView       = 0;
    appMemzero(Curtains, sizeof(Curtains));
    CurtainCount      = 0;
    CurtainEpoch      = 0.0;
    CurtainAble       = -1000.0;
    SplashConstMapped = nullptr;
    bSplashFailed     = 0;
    bExtraCopy        = 0;
    bReactiveLive     = 0;
    ReactiveState     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bReactiveInit     = 0;
    WaterSurfaceState     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bWaterSurfaceLive     = 0;
    bWaterSurfaceCleared  = 0;
    bWaterSurfaceViewReal = 0;
    WetMarkState          = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bWetMarkCleared       = 0;
    bWetMarkViewReal      = 0;
    RippleCurrent   = 0;
    bRippleCleared  = 0;
    bRippleFailed   = 0;
    bRippleWanted   = 0;
    RippleCarry     = 0.0f;
    RippleLastTime  = 0.0;
    RippleOriginX   = RippleOriginY = 0.0f;
    bRippleOriginValid = 0;
    RippleCameraX   = RippleCameraY = 0.0f;
    RipplePulseCount = 0;
    OverrideActorMap = -1;
    OverrideTaps     = -1;
    OverrideFullRes  = -1;
    MsaaWanted      = 1;
    MsaaCount       = 1;
    bStencilRef     = 0;
    bMsaaResolved   = 0;
    PostAaKind      = 0;
    PostAaSpan      = 4.0f;
    FxaaReady       = 0;
    MotionProbeMode = 0;
    bJitterWanted   = 0;
    JitterFrame     = 0;
    Fsr3W           = 0;
    Fsr3H           = 0;
    Fsr3Ready       = 0;
    bFsr3Wanted     = 0;
    bFsr3Reset      = 1;       // the very first frame has no history to keep
    Fsr3Sharpness   = 0.5f;    // the SDK's middle; the ini key sets it at Init
    bFsr3ForceFail  = 0;
    UpscalerChoice  = 0;
    bHostUpscaled   = 0;
    bFrameGenWanted = 0;
    bFgMode         = 0;
    bFgFrame        = 0;
    bFgLast         = 0;
    bHudLessDone    = 0;
    bFgReset        = 0;
    FgWhy           = 1;
    FgWhyLast       = 1;
    MotionBodyCount   = 0;
    MotionObjReady    = 0;
    MotionObjFormat   = DXGI_FORMAT_UNKNOWN;
    MotionNearer      = 0.0f;
    bMotionNoDepth    = 0;
    MotionObjDrawn    = 0;
    MotionBodiesLast  = 0;
    MotionVertsDrawn  = 0;
    for (INT s = 0; s < BACKEND_FRAME_COUNT; s++)
        MotionVbMapped[s] = nullptr;
    MotionVbUsed      = 0;
    MotionVbState     = 0;
    MotionVbRefused   = 0;
    JitterClip[0]     = 0.0f;
    JitterClip[1]     = 0.0f;
    FrameDeltaMs    = 16.6f;
    PrevFrameQpc    = 0;
    appMemzero(JitterPx, sizeof(JitterPx));
    bShadowViewSet  = 0;
    appMemzero(ShadowViewRect, sizeof(ShadowViewRect));
    ShadowTanX      = 1.0f;
    ShadowTanY      = 0.5625f;
    appMemzero(ShadowSkip, sizeof(ShadowSkip));
    ShadowSkipCount = 0;
    ViewTanX       = 1.0f;
    ViewTanY       = 0.5625f;
    appMemzero(EndParts, sizeof(EndParts));
    DebugIdCount  = 0;
    FrameFile       = nullptr;
    FrameCapLen     = 0;
    FrameCapPrevQpc = 0;
    FrameCapLabel[0] = 0;
    appMemzero(DebugIds, sizeof(DebugIds));
    appMemzero(DebugCounts, sizeof(DebugCounts));
    UploadAllocator = nullptr;
    bUploadOpen     = 0;
    AdapterName[0] = 0;
    AdapterVendor  = 0;
    AdapterLabel[0] = 0;
    GpuWanted[0]    = 0;
    GpuKeyLabel[0]  = 0;
    GpuKeyState     = 0;
    bDisplayAskAll  = 0;
    bSafeMode       = 0;
}

FBackend::~FBackend()
{
    Shutdown();
}

// ---------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------

// What this adapter offers - logged once per start. The roadmap's two biggest items hang on it
// (docs/terv-latvany-roadmap.md 2.3 and 3.6): FSR 3.1's shaders need shader model 6.x, where our
// own compute path is cs_5_0, and raytracing needs DXR. Whether a 32-bit process gets either from
// the driver is a question to measure, not to assume - and since this runs on every machine, a
// log sent in from an NVIDIA or Intel card answers it for the vendors we do not have here.
void FBackend::LogCapabilities()
{
    // The highest level the hardware reaches; the device itself is created at 11_0.
    static const D3D_FEATURE_LEVEL Levels[] =
    {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS Fl;
    appMemzero(&Fl, sizeof(Fl));
    Fl.NumFeatureLevels        = (UINT)ARRAY_COUNT(Levels);
    Fl.pFeatureLevelsRequested = Levels;
    const UBOOL bFl = SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &Fl, sizeof(Fl)));

    // The runtime refuses a shader model newer than it knows (E_INVALIDARG), so ask from the
    // newest down; the answer is the lower of what the runtime and the driver support.
    static const D3D_SHADER_MODEL Models[] =
    {
        D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
        D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1
    };
    D3D12_FEATURE_DATA_SHADER_MODEL Sm;
    appMemzero(&Sm, sizeof(Sm));
    UBOOL bSm = 0;
    for (INT i = 0; i < (INT)ARRAY_COUNT(Models) && !bSm; i++)
    {
        Sm.HighestShaderModel = Models[i];
        bSm = SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &Sm, sizeof(Sm)));
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 O5;
    appMemzero(&O5, sizeof(O5));
    const UBOOL bO5 = SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &O5, sizeof(O5)));

    D3D12_FEATURE_DATA_D3D12_OPTIONS4 O4;
    appMemzero(&O4, sizeof(O4));
    const UBOOL bO4 = SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &O4, sizeof(O4)));

    TCHAR FlText[16];
    if (bFl)
        appSprintf(FlText, TEXT("%i_%i"), ((INT)Fl.MaxSupportedFeatureLevel >> 12) & 0xF,
                   ((INT)Fl.MaxSupportedFeatureLevel >> 8) & 0xF);
    else
        appStrcpy(FlText, TEXT("?"));

    TCHAR SmText[16];
    if (bSm)
        appSprintf(SmText, TEXT("%i.%i"), ((INT)Sm.HighestShaderModel >> 4) & 0xF,
                   (INT)Sm.HighestShaderModel & 0xF);
    else
        appStrcpy(SmText, TEXT("?"));

    TCHAR RtText[16];
    if (!bO5)
        appStrcpy(RtText, TEXT("?"));
    else if (O5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        appStrcpy(RtText, TEXT("none"));
    else
        appSprintf(RtText, TEXT("%i.%i"), (INT)O5.RaytracingTier / 10, (INT)O5.RaytracingTier % 10);

    RDX_LOGF("capabilities (%i-bit process): feature level %s, shader model %s, raytracing (DXR) %s, "
             "16-bit shader operations %s",
             (INT)(sizeof(void*) * 8), FlText, SmText, RtText,
             !bO4 ? TEXT("?") : (O4.Native16BitShaderOpsSupported ? TEXT("yes") : TEXT("no")));
}

// A name in the caption font's alphabet - capitals, digits and hyphens, anything else a single space - because a
// model name can carry any character and the font draws very few. bNoBrackets leaves out what stands in brackets:
// a card's "(R)" and "(TM)".
static void CaptionName(const WCHAR* From, char* Out, INT Max, UBOOL bNoBrackets)
{
    INT   k      = 0;
    INT   Depth  = 0;
    UBOOL bSpace = 1;   // so that leading spaces, and every run of them, come out as at most one
    for (INT j = 0; From && From[j] && k < Max - 1; j++)
    {
        WCHAR C = From[j];
        if (bNoBrackets && (C == L'(' || C == L')'))
        {
            Depth = (C == L'(') ? Depth + 1 : ((Depth > 0) ? Depth - 1 : 0);
            C     = L' ';
        }
        else if (Depth > 0)
            C = L' ';
        if (C >= L'a' && C <= L'z')
            C = (WCHAR)(C - L'a' + L'A');
        if ((C >= L'A' && C <= L'Z') || (C >= L'0' && C <= L'9') || C == L'-')
        {
            Out[k++] = (char)C;
            bSpace   = 0;
        }
        else if (!bSpace)
        {
            Out[k++] = ' ';
            bSpace   = 1;
        }
    }
    while (k > 0 && Out[k - 1] == ' ')
        k--;
    Out[k] = 0;
}

void FBackend::RequestGpu(const TCHAR* Text)
{
    INT i = 0;
    for (; Text && Text[i] && i < (INT)(sizeof(GpuWanted) / sizeof(GpuWanted[0])) - 1; i++)
        GpuWanted[i] = Text[i];
    GpuWanted[i] = 0;
}

UBOOL FBackend::CreateDeviceAndQueue()
{
    UINT FactoryFlags = 0;

    // The debug layer - see RequestValidation. On before the device exists, or not at all. Not
    // DXGI's debug factory: nothing here reads its messages, and without its DLL the factory
    // itself would fail, and the renderer with it.
#if defined(_DEBUG) || defined(RDX_DEBUG_LAYER)
    bValidation = 1;
#endif
    if (bValidation)
    {
        TComRef<ID3D12Debug> Debug;
        if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), Debug.VoidSlot())))
        {
            Debug->EnableDebugLayer();
            RDX_LOG("debug layer enabled");
            // Its GPU-based validation with it, where asked (RequestGpuValidation): before the device, as the layer.
            TComRef<ID3D12Debug1> Deeper;
            if (bGpuValidation &&
                SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug1), Deeper.VoidSlot())))
            {
                Deeper->SetEnableGPUBasedValidation(TRUE);
                RDX_LOG("debug layer: GPU-based validation on (the marker SecondRevision-gbv.txt) - many times slower");
            }
        }
        else
        {
            RDX_LOG("debug layer: not available (Graphics Tools?)");
            bValidation = 0;
        }
    }

    if (!Check(CreateDXGIFactory2(FactoryFlags, __uuidof(IDXGIFactory4), Factory.VoidSlot()),
               TEXT("CreateDXGIFactory2")))
        return 0;

    // DRED (RequestDred): switched on before the device is made, or it records nothing.
    if (bDredWanted)
    {
        TComRef<ID3D12DeviceRemovedExtendedDataSettings1> Dred;
        if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings1), Dred.VoidSlot())))
        {
            Dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            Dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            Dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            RDX_LOG("DRED (marker): breadcrumbs, their markers and page faults are recorded");
        }
        else
        {
            RDX_LOG("DRED (marker): not available on this system");
            bDredWanted = 0;
            Solid.SetDredMarkers(0);
        }
    }

    // The machine's cards, in the order the system gives them - "high performance first" where it has such an order
    // (IDXGIFactory6). On a laptop with two cards the plain order begins with the one built into the processor: the
    // game would run on that, and DLSS and FSR 4 would be looked for on it. With one card the two orders are the same.
    // The software adapter is not among them, deliberately: falling back to WARP silently would look like a mysterious
    // performance collapse.
    TComRef<IDXGIFactory6> Factory6;
    const UBOOL bByPerformance =
        SUCCEEDED(Factory->QueryInterface(__uuidof(IDXGIFactory6), Factory6.VoidSlot())) ? 1 : 0;
    enum { MaxCards = 8 };
    TComRef<IDXGIAdapter1> Cards[MaxCards];
    DXGI_ADAPTER_DESC1     Descs[MaxCards];
    const WCHAR*           Names[MaxCards];
    bool                   Usable[MaxCards];
    INT                    CardCount = 0;
    for (UINT Index = 0; CardCount < MaxCards; Index++)
    {
        TComRef<IDXGIAdapter1>& Adapter = Cards[CardCount];
        const HRESULT Next = bByPerformance
            ? Factory6->EnumAdapterByGpuPreference(Index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                   __uuidof(IDXGIAdapter1), Adapter.VoidSlot())
            : Factory->EnumAdapters1(Index, Adapter.Slot());
        if (FAILED(Next))
            break;
        DXGI_ADAPTER_DESC1& Desc = Descs[CardCount];
        if (FAILED(Adapter->GetDesc1(&Desc)) || (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            Adapter.Release();
            continue;
        }
        Desc.Description[127] = 0;
        Names[CardCount]      = Desc.Description;
        // Whether it can run the renderer at all, asked without making the device: that is what no device pointer
        // means to this call.
        Usable[CardCount]     = SUCCEEDED(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                                            __uuidof(ID3D12Device), nullptr));
        CardCount++;
    }

    // The one to run on: the card the GPU key names or - where it names none, or none that can run - the first that
    // can. A card that said it could and then makes no device is passed over for the next.
    bool bAsked = false;
    INT  Pick   = RdxMath::GpuAskedFor(GpuWanted, Names, Usable, CardCount, bAsked);
    GpuKeyState = !bAsked ? 0 : ((Pick >= 0) ? 1 : 2);
    CaptionName((const WCHAR*)GpuWanted, GpuKeyLabel, (INT)sizeof(GpuKeyLabel), 0);
    for (INT Tries = 0; !Device && Tries <= CardCount; Tries++)
    {
        for (INT c = 0; Pick < 0 && c < CardCount; c++)
            if (Usable[c])
                Pick = c;
        if (Pick < 0)
            break;
        if (SUCCEEDED(D3D12CreateDevice(Cards[Pick].Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                        Device.VoidSlot())) && Device)
            break;
        RDX_LOGF("adapter: %s would make no device after all", (const TCHAR*)Names[Pick]);
        Device.Release();
        Usable[Pick] = false;
        GpuKeyState  = bAsked ? 2 : 0;
        Pick         = -1;
    }

    if (Device)
    {
        IDXGIAdapter1* const      Adapter = Cards[Pick].Get();
        const DXGI_ADAPTER_DESC1& Desc    = Descs[Pick];
        // DXGI hands us a UTF-16 description; TCHAR is the same width here.
        for (INT i = 0; i < 127; i++)
        {
            AdapterName[i] = (TCHAR)Desc.Description[i];
            if (!Desc.Description[i])
                break;
        }
        AdapterName[127] = 0;
        AdapterVendor    = Desc.VendorId;
        CaptionName(Desc.Description, AdapterLabel, (INT)sizeof(AdapterLabel), 1);
        // Kept for VideoMemoryMB below: the budget and the usage are DXGI's, and only IDXGIAdapter3 has them.
        Adapter->QueryInterface(__uuidof(IDXGIAdapter3), MemoryAdapter.VoidSlot());
        // The adapter description's dedicated size is a SIZE_T, and a 32-bit process gets a capped figure there
        // (3072 MB on a 16 GB card). The budget is DXGI's 64-bit number: what this process may really use.
        DOUBLE Used = 0.0, Budget = 0.0;
        VideoMemoryMB(Used, Budget);
        RDX_LOGF("adapter: %s (vendor 0x%04X), video memory this process may use %.0f MB (the adapter description says "
                 "%.0f MB, capped for a 32-bit process)", AdapterName, Desc.VendorId, Budget,
                 (DOUBLE)Desc.DedicatedVideoMemory / (1024.0 * 1024.0));
        RDX_LOGF("adapter: number %i in the system's order, %s", Pick + 1,
                 bByPerformance ? TEXT("the high performance card first") : TEXT("as it lists them"));
        if (GpuKeyState == 1)
            RDX_LOGF("adapter: the GPU key asks for '%s' - this card", GpuWanted);
        else if (GpuKeyState == 2)
            RDX_LOGF("adapter: the GPU key asks for '%s' - no card of that number or name can run the renderer, so the "
                     "first that can was taken, as with Auto", GpuWanted);
        // The display driver's version, as the system's device list names it. A run's log then says which driver it
        // ran on: a picture, a time or a lost device from one driver is not held against another's unknowingly.
        LARGE_INTEGER Driver;
        Driver.QuadPart = 0;
        if (SUCCEEDED(Adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &Driver)))
            RDX_LOGF("display driver: %i.%i.%i.%i", (INT)HIWORD(Driver.HighPart), (INT)LOWORD(Driver.HighPart),
                     (INT)HIWORD(Driver.LowPart), (INT)LOWORD(Driver.LowPart));
        else
            RDX_LOG("display driver: its version could not be asked");
    }
    // The others by name and number: a log from a machine with two cards says which one the game did NOT run on, and
    // what the GPU key would have to say to run on it.
    for (INT c = 0; c < CardCount; c++)
        if (c != Pick || !Device)
            RDX_LOGF("adapter not taken: number %i, %s (vendor 0x%04X)%s", c + 1, (const TCHAR*)Names[c],
                     Descs[c].VendorId, Usable[c] ? TEXT("") : TEXT(" - it cannot run Direct3D 12"));

    if (!Device)
    {
        RDX_LOG("no Direct3D 12 capable adapter found");
        return 0;
    }

    LogCapabilities();
    RDX_LOG("ray tracing: the 64-bit host's - this process traces none, whatever its device offers");

    if (bValidation &&
        SUCCEEDED(Device->QueryInterface(__uuidof(ID3D12InfoQueue), InfoQueue.VoidSlot())))
    {
        // Kept, not acted on: no breaking into a debugger that is not there.
        InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
        InfoQueue->SetMessageCountLimit(4096);
        RDX_LOG("debug layer: errors and warnings go to the log");
    }

    D3D12_COMMAND_QUEUE_DESC QueueDesc;
    appMemzero(&QueueDesc, sizeof(QueueDesc));
    QueueDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    QueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    return Check(Device->CreateCommandQueue(&QueueDesc, __uuidof(ID3D12CommandQueue), Queue.VoidSlot()),
                 TEXT("CreateCommandQueue"));
}

// Why the output runs below what was asked for, as the tail of a log line.
static const TCHAR* HdrFallbackText(RdxMath::EHdrFallback Why)
{
    switch (Why)
    {
    case RdxMath::HdrFallback_NeedsHdrDisplay:   return TEXT(" (stepped down: the display cannot show HDR)");
    case RdxMath::HdrFallback_HdrOffInWindows:   return TEXT(" (stepped down: HDR is off for this display in Windows)");
    case RdxMath::HdrFallback_Needs10BitDisplay: return TEXT(" (stepped down: the connection carries eight bits)");
    case RdxMath::HdrFallback_Failed:            return TEXT(" (stepped down: the mode could not be built)");
    case RdxMath::HdrFallback_SafeMode:          return TEXT(" (stepped down: safe mode)");
    default:                                     return TEXT("");
    }
}

UBOOL FBackend::CreateSwapChain(HWND Window)
{
    // Tearing support is what makes FreeSync/VRR work in a borderless window, so ask
    // for it up front rather than bolting it on later.
    {
        TComRef<IDXGIFactory5> Factory5;
        if (SUCCEEDED(Factory->QueryInterface(__uuidof(IDXGIFactory5), Factory5.VoidSlot())))
        {
            BOOL Allow = FALSE;
            if (SUCCEEDED(Factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &Allow, sizeof(Allow))))
                bAllowTearing = Allow ? 1 : 0;
        }
    }
    RDX_LOGF("tearing (VRR) support: %s", bAllowTearing ? TEXT("yes") : TEXT("no"));

    // The display has to be asked BEFORE the swap chain is described: a format that the display cannot show is
    // not a format worth creating. HdrRunning is what comes of that, and every target and pipeline from here on
    // is built from it - so it is settled in exactly one place, and once.
    OutputWindow = Window;
    ReadDisplayHdr(Window);
    HdrRunning = ResolveHdrMode(HdrWhy);
    Colours    = RdxColoursFor(HdrRunning);
    Final.SetColours(Colours);
    Solid.SetColours(Colours);
    // The output transform's pipeline is built HERE, before the swap chain is described, and not with the rest
    // of the passes later. The swap chain's format follows the mode, so a mode that cannot be built has to be
    // given up while that is still only a decision - afterwards the back buffer would already be half floats
    // with nothing able to write them, and a machine that could not compile one shader would show no picture at
    // all instead of the picture it had yesterday.
    if (HdrRunning == RdxMath::Hdr_On && !Final.InitHdr(Device.Get()))
    {
        RDX_LOG("HDR: the output transform's pipeline could NOT be built - falling back to the ten-bit output");
        HdrRunning = RdxMath::Hdr_Ten;
        HdrWhy     = RdxMath::HdrFallback_Failed;
        Colours    = RdxColoursFor(HdrRunning);
        Final.SetColours(Colours);
        Solid.SetColours(Colours);
    }
    {
        TCHAR Asked[16], Got[16];
        RdxMath::HdrName(HdrMode, Asked, 16);
        RdxMath::HdrName(HdrRunning, Got, 16);
        RDX_LOGF("HDR: %s asked for, %s running%s - %i bits a channel, the display is in %s mode", Asked, Got,
                 HdrFallbackText(HdrWhy), RdxMath::HdrSceneBits(HdrRunning), bDisplayHdr ? TEXT("HDR") : TEXT("SDR"));
        // The one case worth pointing out rather than leaving to be noticed: the display CAN do it and the key
        // says no. Somebody who moved the game to an HDR television would otherwise get the ordinary picture with
        // nothing to say why.
        if (bDisplayHdr && HdrMode != RdxMath::Hdr_On)
            RDX_LOG("HDR: this display is in HDR mode and the renderer is not - set HDR=On in "
                    "SecondRevision.settings.ini, or step it in the renderer's menu (its key is DebugKey, Scroll Lock by default)");
    }

    DXGI_SWAP_CHAIN_DESC1 Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Width            = Width;
    Desc.Height           = Height;
    // Not why our frames are composed (PresentMon: "Composed: Flip", the desktop compositor
    // copying each frame, where the stock D3D10 renderer gets "Hardware: Independent Flip"):
    // B8G8R8A8, the desktop's own format, makes no difference to it.
    Desc.Format           = Colours.BackBuffer;
    Desc.SampleDesc.Count = 1;
    Desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    Desc.BufferCount      = SwapBuffers;           // two, or three for a measurement (see BACKEND_FRAME_COUNT)
    Desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Desc.Flags            = bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    RDX_LOGF("swap chain: %u back buffer", SwapBuffers);

    RDX_LOGF("swap chain: format %u (%s)", (UINT)Colours.BackBuffer,
             (Colours.BackBuffer == DXGI_FORMAT_R16G16B16A16_FLOAT) ? TEXT("half float, HDR")
             : ((Colours.BackBuffer == DXGI_FORMAT_R10G10B10A2_UNORM) ? TEXT("ten bits a channel")
                                                                      : TEXT("eight bits a channel")));

    // Frame generation's mode presents from the pacer's queue (FFramePacer): the chain belongs to the queue it is made on.
    TComRef<IDXGISwapChain1> Chain1;
    ID3D12CommandQueue* const ChainQueue = (bFgMode && Pacer.IsUp()) ? Pacer.GetQueue() : Queue.Get();
    if (!Check(Factory->CreateSwapChainForHwnd(ChainQueue, Window, &Desc, nullptr, nullptr, Chain1.Slot()),
               TEXT("CreateSwapChainForHwnd")))
        return 0;
    if (ChainQueue != Queue.Get())
        RDX_LOG("swap chain: on frame generation's presenting queue");

    // The engine owns the window and handles Alt+Enter itself, so DXGI must not.
    Factory->MakeWindowAssociation(Window, DXGI_MWA_NO_ALT_ENTER);

    if (!Check(Chain1->QueryInterface(__uuidof(IDXGISwapChain3), SwapChain.VoidSlot()),
               TEXT("QueryInterface(IDXGISwapChain3)")))
        return 0;

    FrameIndex = SwapChain->GetCurrentBackBufferIndex();

    // The colour space is what says what the numbers in the back buffer MEAN. A half-float chain whose scRGB
    // was refused would be read as ordinary sRGB, and the picture would come out pale and wrong in a way that
    // looks like a shader fault - so a refusal is not something to log and carry on from.
    //
    // It can only be asked of a chain that exists, which is why this is a step back rather than a different
    // first choice. The chain is not thrown away: a flip-model chain's format can be changed by ResizeBuffers,
    // and nothing has taken a reference to a back buffer yet - CreateFrameResources has not run.
    if (!ApplyColourSpace() && HdrRunning == RdxMath::Hdr_On)
    {
        RDX_LOG("HDR: this swap chain will not take scRGB - stepping back to the ten-bit output, which needs none");
        HdrRunning = RdxMath::Hdr_Ten;
        HdrWhy     = RdxMath::HdrFallback_Failed;
        Colours    = RdxColoursFor(HdrRunning);
        Final.SetColours(Colours);
        Solid.SetColours(Colours);
        const UINT Flags = bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        if (!Check(SwapChain->ResizeBuffers(SwapBuffers, Width, Height, Colours.BackBuffer, Flags),
                   TEXT("ResizeBuffers(stepping back from scRGB)")))
            return 0;
        FrameIndex = SwapChain->GetCurrentBackBufferIndex();
        ApplyColourSpace();
    }
    return 1;
}

// DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 (wingdi.h, Windows 11 24H2 and later), written out here because the SDK
// declares it only for builds that target Windows 11, and this one runs on far older systems. The layout is the
// documented one; the flags are bit fields allocated from the lowest bit, so HDR support is bit 4.
struct FRdxAdvancedColorInfo2
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32                           Flags;
    DISPLAYCONFIG_COLOR_ENCODING     colorEncoding;
    UINT32                           bitsPerColorChannel;
    UINT32                           activeColorMode;
};
static const UINT32 RdxAdvancedColorInfo2Type = 15;        // DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2
static const UINT32 RdxHdrSupportedBit        = 1u << 4;   // highDynamicRangeSupported

// What a monitor is, from Windows' own display configuration: its name, whether it can do HDR, and how many bits a
// channel go down the wire to it.
//
// The NAME as Windows' display settings show it ("LG TV SSCR2") rather than the GDI device name ("\\.\DISPLAY2"),
// which says which socket a screen hangs on, not which screen it is. Written in the caption font's alphabet -
// capitals, digits and hyphens, anything else a single space - because a model name can carry any character and the
// font draws very few. With no name to be had (a projector, a capture card, a panel whose EDID carries none) the GDI
// name stands in, without its "\\.\" prefix: it still tells two screens apart, which is the whole of the job.
//
// HDR CAPABILITY is asked the newer way first, because the older answer means something else on current Windows:
// its "advanced colour supported" is true for a wide-gamut SDR monitor too, since colour management runs through the
// same pipeline - asked that way, an ordinary monitor would be reported as able to do HDR. The newer question has an
// answer of its own for HDR. Only a Windows too old to understand it (before 11 24H2) gets the older one, where
// advanced colour and HDR were still the same thing.
static void DescribeMonitor(HMONITOR Monitor, char* Out, INT Max, INT& HdrCapable, INT& Bits)
{
    Out[0]     = 0;
    HdrCapable = -1;
    Bits       = 0;
    MONITORINFOEXW Mi;
    appMemzero(&Mi, sizeof(Mi));
    Mi.cbSize = sizeof(Mi);
    if (!Monitor || Max <= 0 || !::GetMonitorInfoW(Monitor, &Mi))
        return;

    WCHAR Name[64];
    Name[0] = 0;
    UINT32 NPaths = 0, NModes = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &NPaths, &NModes) == ERROR_SUCCESS && NPaths && NModes)
    {
        DISPLAYCONFIG_PATH_INFO* Paths =
            (DISPLAYCONFIG_PATH_INFO*)appMalloc((INT)(NPaths * sizeof(DISPLAYCONFIG_PATH_INFO)), TEXT("RdxDisplayPaths"));
        DISPLAYCONFIG_MODE_INFO* Modes =
            (DISPLAYCONFIG_MODE_INFO*)appMalloc((INT)(NModes * sizeof(DISPLAYCONFIG_MODE_INFO)), TEXT("RdxDisplayModes"));
        if (Paths && Modes &&
            ::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &NPaths, Paths, &NModes, Modes, nullptr) == ERROR_SUCCESS)
        {
            // The path whose source is this monitor's GDI device leads to the target - the screen itself - and the
            // target is what carries the name and the colour answers.
            for (UINT32 i = 0; i < NPaths; i++)
            {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME Src;
                appMemzero(&Src, sizeof(Src));
                Src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                Src.header.size      = sizeof(Src);
                Src.header.adapterId = Paths[i].sourceInfo.adapterId;
                Src.header.id        = Paths[i].sourceInfo.id;
                if (::DisplayConfigGetDeviceInfo(&Src.header) != ERROR_SUCCESS ||
                    ::lstrcmpiW(Src.viewGdiDeviceName, Mi.szDevice) != 0)
                    continue;
                DISPLAYCONFIG_TARGET_DEVICE_NAME Target;
                appMemzero(&Target, sizeof(Target));
                Target.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                Target.header.size      = sizeof(Target);
                Target.header.adapterId = Paths[i].targetInfo.adapterId;
                Target.header.id        = Paths[i].targetInfo.id;
                if (::DisplayConfigGetDeviceInfo(&Target.header) == ERROR_SUCCESS)
                    ::lstrcpynW(Name, Target.monitorFriendlyDeviceName, 64);

                FRdxAdvancedColorInfo2 Colour2;
                appMemzero(&Colour2, sizeof(Colour2));
                Colour2.header.type      = (DISPLAYCONFIG_DEVICE_INFO_TYPE)RdxAdvancedColorInfo2Type;
                Colour2.header.size      = sizeof(Colour2);
                Colour2.header.adapterId = Paths[i].targetInfo.adapterId;
                Colour2.header.id        = Paths[i].targetInfo.id;
                if (::DisplayConfigGetDeviceInfo(&Colour2.header) == ERROR_SUCCESS)
                {
                    HdrCapable = (Colour2.Flags & RdxHdrSupportedBit) ? 1 : 0;
                    Bits       = (INT)Colour2.bitsPerColorChannel;
                }
                else
                {
                    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO Colour;
                    appMemzero(&Colour, sizeof(Colour));
                    Colour.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
                    Colour.header.size      = sizeof(Colour);
                    Colour.header.adapterId = Paths[i].targetInfo.adapterId;
                    Colour.header.id        = Paths[i].targetInfo.id;
                    if (::DisplayConfigGetDeviceInfo(&Colour.header) == ERROR_SUCCESS)
                    {
                        HdrCapable = (Colour.value & 1u) ? 1 : 0;   // advancedColorSupported
                        Bits       = (INT)Colour.bitsPerColorChannel;
                    }
                }
                break;
            }
        }
        if (Paths)
            appFree(Paths);
        if (Modes)
            appFree(Modes);
    }

    const WCHAR* From = Name[0] ? Name : Mi.szDevice;
    if (!Name[0] && From[0] == L'\\' && From[1] == L'\\' && From[2] == L'.' && From[3] == L'\\')
        From += 4;
    CaptionName(From, Out, Max, 0);
}

// What one card's outputs say of a monitor: 0 it is not among them, 1 described (Out filled in), 2 it is there but this
// Windows cannot be asked (no IDXGIOutput6), 3 it is there and would not describe itself.
static INT DescribeOutput(IDXGIAdapter* Adapter, HMONITOR Monitor, DXGI_OUTPUT_DESC1& Out)
{
    for (UINT i = 0; Adapter; i++)
    {
        TComRef<IDXGIOutput> Output;
        if (Adapter->EnumOutputs(i, Output.Slot()) == DXGI_ERROR_NOT_FOUND || !Output)
            break;
        DXGI_OUTPUT_DESC Desc;
        appMemzero(&Desc, sizeof(Desc));
        if (FAILED(Output->GetDesc(&Desc)) || Desc.Monitor != Monitor)
            continue;
        // GetDesc1, and so the whole question, arrived with IDXGIOutput6 (Windows 10 1703). An older system
        // simply has no HDR to report.
        TComRef<IDXGIOutput6> Output6;
        if (FAILED(Output->QueryInterface(__uuidof(IDXGIOutput6), Output6.VoidSlot())) || !Output6)
            return 2;
        appMemzero(&Out, sizeof(Out));
        return SUCCEEDED(Output6->GetDesc1(&Out)) ? 1 : 3;
    }
    return 0;
}

// What the display under the window says about itself. The window matters: on a machine with an ordinary
// monitor and an HDR television the answer is different on each, and the one that counts is the one the
// picture is going to.
//
// Whether the display is IN HDR MODE is the operating system's business, not ours - the player switches it on
// in Windows' own display settings, and this only asks what came of that. There is no EDID to read and no
// driver to ask.
// It runs BEFORE the swap chain exists, which is why it walks the cards' outputs and matches monitor
// handles rather than asking the swap chain which output it is on: the swap chain's FORMAT depends on the
// answer, so the answer has to come first.
void FBackend::ReadDisplayHdr(HWND Window)
{
    bDisplayHdr = 0;
    DisplayNits = 0.0f;
    if (!MemoryAdapter || !Window)
        return;
    if (bSafeMode)
    {
        RDX_LOG("display: safe mode - the display is not asked about HDR or its bits; the picture is eight-bit SDR");
        return;
    }

    const HMONITOR Monitor = ::MonitorFromWindow(Window, MONITOR_DEFAULTTONEAREST);
    // What the monitor is first, and before the cards are asked: every way out below is an early return, and a
    // screen whose HDR state could not be read still has a name and a capability worth showing.
    DescribeMonitor(Monitor, DisplayName, (INT)sizeof(DisplayName), DisplayHdrCapable, DisplayBits);
    RDX_LOGF("display: the window is on %s - %i bits a channel on the wire, HDR %s",
             DisplayName[0] ? appFromAnsi(DisplayName) : TEXT("an unnamed monitor"), DisplayBits,
             (DisplayHdrCapable > 0) ? TEXT("possible") : ((DisplayHdrCapable == 0) ? TEXT("not possible") : TEXT("unknown")));

    // The device's own card first. Where the screen is not among its outputs it hangs on another card - a laptop's own
    // panel on the one built into its processor, while the picture is drawn on the other - and what a screen says of
    // itself is that card's to be asked. Through a factory of its own: the device's knows the cards and screens as
    // they were when the game started.
    DXGI_OUTPUT_DESC1 Desc1;
    appMemzero(&Desc1, sizeof(Desc1));
    INT Found = bDisplayAskAll ? 0 : DescribeOutput(MemoryAdapter.Get(), Monitor, Desc1);
    if (bDisplayAskAll)
        RDX_LOG("HDR (marker): the device's own card is not asked first - every card is, afresh");
    if (!Found)
    {
        TComRef<IDXGIFactory1> All;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), All.VoidSlot())) && All)
            for (UINT a = 0; !Found; a++)
            {
                TComRef<IDXGIAdapter1> Other;
                if (All->EnumAdapters1(a, Other.Slot()) == DXGI_ERROR_NOT_FOUND || !Other)
                    break;
                Found = DescribeOutput(Other.Get(), Monitor, Desc1);
                DXGI_ADAPTER_DESC1 Whose;
                if (!Found || FAILED(Other->GetDesc1(&Whose)))
                    continue;
                const LUID Ours = Device->GetAdapterLuid();
                if (Whose.AdapterLuid.LowPart != Ours.LowPart || Whose.AdapterLuid.HighPart != Ours.HighPart)
                {
                    Whose.Description[127] = 0;
                    RDX_LOGF("display: this screen hangs on another card (%s) - the picture crosses over to it",
                             (const TCHAR*)Whose.Description);
                }
            }
    }
    if (Found == 2)
    {
        RDX_LOG("HDR: this Windows cannot be asked about the display (no IDXGIOutput6) - treating it as SDR");
        return;
    }
    if (Found == 3)
    {
        RDX_LOG("HDR: the display refused to describe itself - treating it as SDR");
        return;
    }
    if (!Found)
    {
        RDX_LOG("HDR: the window is on no display any card drives - treating it as SDR");
        return;
    }
    // Whether the display is IN HDR MODE is the operating system's business, not ours: the player turns it
    // on in Windows' own display settings, and this only asks what came of that. No EDID to read, no driver
    // to interrogate.
    bDisplayHdr = (Desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ? 1 : 0;
    DisplayNits = Desc1.MaxLuminance;
    // The same wire format, as DXGI sees it - for a Windows whose display configuration would not say.
    if (DisplayBits <= 0)
        DisplayBits = (INT)Desc1.BitsPerColor;
    // The peak brightness is logged, not acted on: panels routinely claim a number they cannot reach, and a
    // tone map built on a lie is worse than one built on a fixed, honest assumption.
    RDX_LOGF("HDR: the display is in %s mode, claiming %.0f nits at its brightest and %.4f at its darkest",
             bDisplayHdr ? TEXT("HDR") : TEXT("SDR"), (FLOAT)Desc1.MaxLuminance, (FLOAT)Desc1.MinLuminance);
}

// The picture has been moved to another monitor. The display is asked again, and the output with it: what can RUN
// depends on the display (RdxMath::HdrModeForDisplay) - HDR only on a display in HDR mode, ten bits only where the
// connection carries ten - so a move from the television to an eight-bit monitor takes the output down two steps,
// and a move back takes it up again. ApplyHdrRunning rebuilds only when the running mode really changes; otherwise
// nothing is made again, and what does change - whether the screen is in HDR, how bright it claims to be - DrawFinal
// reads every frame. The caller calls this between frames, which a rebuild needs.
void FBackend::OnDisplayChanged(HWND Window)
{
    const UBOOL             bWasHdr    = bDisplayHdr;
    const RdxMath::EHdrMode WasRunning = HdrRunning;
    if (Window)
        OutputWindow = Window;
    ApplyHdrRunning();
    TCHAR Name[16];
    RdxMath::HdrName(HdrRunning, Name, 16);
    RDX_LOGF("display: the picture moved - the screen is in %s mode now%s; the output runs %s%s%s",
             bDisplayHdr ? TEXT("HDR") : TEXT("SDR"), (bDisplayHdr != bWasHdr) ? TEXT(" (it was not, before)") : TEXT(""),
             Name, HdrFallbackText(HdrWhy), (HdrRunning != WasRunning) ? TEXT(" - changed, rebuilt") : TEXT(""));
}

// What the output path will really do, once the display has had its say - see RdxMath::HdrModeForDisplay for the chain
// and for why HDR steps down on a display that is not in HDR mode rather than running there folded back under white.
RdxMath::EHdrMode FBackend::ResolveHdrMode(RdxMath::EHdrFallback& Why) const
{
    if (bSafeMode)
    {
        Why = (HdrMode != RdxMath::Hdr_Off) ? RdxMath::HdrFallback_SafeMode : RdxMath::HdrFallback_None;
        return RdxMath::Hdr_Off;
    }
    return RdxMath::HdrModeForDisplay(HdrMode, bDisplayHdr != 0, DisplayHdrCapable, DisplayBits, Why);
}

// The HDR key changed in the running game. It is the heaviest of the switches here: the format is baked into
// every colour target, into every pipeline that writes one, and into the swap chain's own buffers - so all of
// them go and are made again. Only between frames.
//
// Every pipeline FFinalPass holds is released rather than picked over. Which of them exist depends on the
// antialiasing, the shadows, the depth view and what has been reached in play so far, and a list of that kept
// here would be a second copy of the truth, one release behind. The "ready" flags go back to "not tried", and
// each pass builds itself again on its next use, in the new format.
// Every pipeline that carries a colour format, built again for whatever Colours now says. It is one function and
// not two because the failure path below needs exactly the same work: a restore that rebuilt a DIFFERENT set from
// the one the change had built would leave whatever it forgot missing - and the first thing it forgot was the HDR
// transform itself, which would have left a mode that failed to change showing nothing at all.
//
// Which passes exist depends on the antialiasing, the shadows and what has been reached in play, so the "ready"
// flags go back to "not tried" and each of those builds itself again on its next use, in the new format.
UBOOL FBackend::RebuildForColours()
{
    Final.Shutdown();
    Final.SetColours(Colours);
    // The geometry pass writes the SCENE target, whose format moved as well. A pipeline built for eight bits
    // aimed at a ten-bit target writes eight-bit numbers into ten-bit texels, and every surface comes back with
    // the bits of its neighbours mixed in.
    Solid.SetColours(Colours);

    ShadowReady     = 0;
    AoReady         = 0;
    RtFactorBlurReady = 0;
    FxaaReady       = 0;
    CaptionReady    = 0;
    DepthViewReady  = 0;
    MotionReady     = 0;
    MotionViewReady = 0;
    MotionObjReady  = 0;
    DepthNearerReady = 0;

    if (!Solid.RebuildPipelines(Device.Get()))
        return 0;
    if (HdrRunning == RdxMath::Hdr_On && !Final.InitHdr(Device.Get()))
    {
        RDX_LOG("HDR: the output transform's pipeline could not be built");
        return 0;
    }
    if (!Final.Init(Device.Get()))
        return 0;
    if (MsaaCount > 1 && !Final.InitMsaaDepth(Device.Get(), MsaaCount, bShadows))
        return 0;
    if (SceneScale > 1 && !Final.InitBox(Device.Get()))
        return 0;
    if (UpscaleMode && !Final.InitUpscale(Device.Get()))
        return 0;
    if (PostAaKind == 1)
        FxaaReady = Final.InitFxaa(Device.Get()) ? 1 : -1;
    // SMAA's pipelines alone: its edge picture and its view of the colour belong to the scene targets, which are still
    // of the old format here - the Resize that follows every change of the colours makes them (CreateSmaaTargets).
    else if (PostAaKind == 2)
        FxaaReady = Final.InitSmaa(Device.Get()) ? 1 : -1;

    // FSR 3.1's context holds its own resources in the old format, so it is thrown away and the next frame builds
    // one that matches. Its target goes with it - EnsureFsr3Target makes a new one at the Compute format. The host's
    // textures are in the old formats too: the next frame hands it new ones.
    if (Fsr3Ready > 0)
        Fsr3.Shutdown();
    Fsr3Ready = 0;
    Fsr3Output.Release();
    Fsr3W = Fsr3H = 0;
    Host.ReleaseTextures();
    FgFinal.Release();
    return 1;
}

// The HDR key changed in the running game - and, through ApplyHdrRunning, everything it takes. Only between frames.
UBOOL FBackend::SetHdrMode(INT Mode)
{
    if (!Device || !SwapChain || bInFrame)
        return 0;
    const RdxMath::EHdrMode Want = (Mode >= 0 && Mode < (INT)RdxMath::Hdr_Count) ? (RdxMath::EHdrMode)Mode
                                                                                 : RdxMath::Hdr_Off;
    if (Want == HdrMode)
        return 1;
    const RdxMath::EHdrMode OldMode = HdrMode;
    HdrMode = Want;
    if (ApplyHdrRunning())
        return 1;
    HdrMode = OldMode;          // ApplyHdrRunning has already put back what was running
    return 0;
}

// The running mode worked out again, and made real if it moved. It is the heaviest change here: the format is baked
// into every colour target, into every pipeline that writes one, and into the swap chain's own buffers - so all of
// them go and are made again. A running mode that did not move rebuilds nothing, which is what keeps an ordinary move
// between two SDR monitors, or a change of the key that the display turns into the same mode, free.
UBOOL FBackend::ApplyHdrRunning()
{
    if (!Device || !SwapChain || bInFrame)
        return 0;
    const RdxMath::EHdrMode     OldRunning = HdrRunning;
    const RdxMath::EHdrFallback OldWhy     = HdrWhy;
    const FRdxColours           OldColours = Colours;
    // The display is asked again: between the start and now the window may have been taken to another monitor, and
    // turning HDR on in Windows is exactly the sort of thing done just before trying this.
    if (OutputWindow)
        ReadDisplayHdr(OutputWindow);
    const RdxMath::EHdrMode Want = ResolveHdrMode(HdrWhy);
    if (Want == OldRunning)
    {
        ApplyColourSpace();     // it belongs to the swap chain, but is accepted per output - and the output may be new
        return 1;
    }
    WaitForGpu();
    HdrRunning = Want;
    Colours    = RdxColoursFor(HdrRunning);
    // Resize is what re-creates the swap chain's buffers in the new format and, with them, every scene target.
    if (RebuildForColours() && Resize(Width, Height))
    {
        // The same step back as at start-up: a half-float chain whose scRGB is refused would be read as sRGB, and the
        // picture would be pale and wrong. Ten bits needs no colour space of its own, so it always takes.
        if (!ApplyColourSpace() && HdrRunning == RdxMath::Hdr_On)
        {
            RDX_LOG("HDR: this swap chain will not take scRGB - stepping back to the ten-bit output");
            HdrRunning = RdxMath::Hdr_Ten;
            HdrWhy     = RdxMath::HdrFallback_Failed;
            Colours    = RdxColoursFor(HdrRunning);
            if (!RebuildForColours() || !Resize(Width, Height))
                return 0;
            ApplyColourSpace();
        }
        TCHAR Name[16];
        RdxMath::HdrName(HdrRunning, Name, 16);
        RDX_LOGF("HDR: now %s%s - %i bits a channel, the display is in %s mode", Name, HdrFallbackText(HdrWhy),
                 RdxMath::HdrSceneBits(HdrRunning), bDisplayHdr ? TEXT("HDR") : TEXT("SDR"));
        return 1;
    }

    RDX_LOG("HDR: the change FAILED - going back to what was running");
    HdrRunning = OldRunning;
    HdrWhy     = OldWhy;
    Colours    = OldColours;
    RebuildForColours();
    Resize(Width, Height);
    ApplyColourSpace();
    return 0;
}

// The frame's last pass: the picture onto the back buffer. Two shaders behind one call, because the choice is
// the output's business and not the caller's - Draw for the two SDR modes, exactly as it always was, and
// DrawHdr for the transform.
void FBackend::DrawFinal(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                         D3D12_GPU_DESCRIPTOR_HANDLE Srv)
{
    if (HdrRunning != RdxMath::Hdr_On)
    {
        Final.Draw(Cmd, Heap, Srv, OutputExponent);
        return;
    }
    FFinalPass::FHdrOutput Out;
    Out.Exponent     = OutputExponent;
    Out.DecodeGamma  = RdxMath::HdrDecodeGamma;
    Out.bFalseColour = bHdrFalseColour;
    if (bDisplayHdr)
    {
        // A real HDR display. Paper white is where the frame's own white goes; above it is the room the
        // display has left, and that is where the lamps are allowed to live. The panel's claim about its peak
        // is not trusted far: it is taken as an upper bound, clamped into a range that stays sensible when the
        // number is a fiction, and the knee sits AT paper white so nothing below it is touched at all.
        const FLOAT Peak    = (DisplayNits > 1.0f) ? DisplayNits : 0.0f;
        FLOAT       Ceiling = (Peak > 0.0f) ? (Peak / RdxMath::HdrPaperWhiteNits) : 2.0f;
        if (Ceiling < 1.25f) Ceiling = 1.25f;
        if (Ceiling > 8.0f)  Ceiling = 8.0f;
        Out.Knee       = 1.0f;
        Out.Ceiling    = Ceiling;
        Out.PaperWhite = RdxMath::PaperWhiteScale(RdxMath::HdrPaperWhiteNits);
    }
    else
    {
        // An SDR display, where the compositor reads scRGB 1.0 as white: there is no headroom to give the
        // lamps, so the top quarter of the range is where they are folded in. The picture below the knee is
        // still untouched, which is what makes this a fair look at what the mode does.
        Out.Knee       = RdxMath::HdrShoulderKnee;
        Out.Ceiling    = 1.0f;
        Out.PaperWhite = 1.0f;
    }
    Final.DrawHdr(Cmd, Heap, Srv, Out);
}

// The numbers in a back buffer mean nothing on their own - the colour space says what they are. Half floats
// with no SetColorSpace1 are taken for ordinary sRGB, and the picture comes out pale and wrong in a way that
// looks like a shader fault, so this is worth a log line of its own.
UBOOL FBackend::ApplyColourSpace()
{
    if (!SwapChain)
        return 0;
    const UBOOL bScRgb = (Colours.BackBuffer == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 1 : 0;
    // scRGB: linear light, Rec.709 primaries, 1.0 = 80 nits. The alternative, HDR10, would mean encoding PQ
    // ourselves and converting to Rec.2020 primaries; scRGB leaves both to the compositor, which is the right
    // trade while the question is whether any of this works at all.
    const DXGI_COLOR_SPACE_TYPE Want = bScRgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                              : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT Support = 0;
    if (FAILED(SwapChain->CheckColorSpaceSupport(Want, &Support)) ||
        !(Support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
    {
        RDX_LOGF("HDR: the swap chain will not take the %s colour space", bScRgb ? TEXT("scRGB") : TEXT("sRGB"));
        return 0;
    }
    if (FAILED(SwapChain->SetColorSpace1(Want)))
    {
        RDX_LOGF("HDR: SetColorSpace1(%s) FAILED", bScRgb ? TEXT("scRGB") : TEXT("sRGB"));
        return 0;
    }
    RDX_LOGF("HDR: colour space %s", bScRgb ? TEXT("scRGB (linear, 1.0 = 80 nits)") : TEXT("sRGB"));
    return 1;
}

UBOOL FBackend::CreateFrameResources()
{
    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
    appMemzero(&HeapDesc, sizeof(HeapDesc));
    HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HeapDesc.NumDescriptors = BACKEND_FRAME_COUNT + 1;     // + the scene target
    HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), RtvHeap.VoidSlot()),
               TEXT("CreateDescriptorHeap(RTV)")))
        return 0;

    RtvStart  = RtvHeap->GetCPUDescriptorHandleForHeapStart();
    RtvStride = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < SwapBuffers; i++)
    {
        if (!Check(SwapChain->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&BackBuffers[i]),
                   TEXT("SwapChain::GetBuffer")))
            return 0;

        D3D12_CPU_DESCRIPTOR_HANDLE Handle = RtvStart;
        Handle.ptr += i * RtvStride;
        // The back buffer only ever receives the final pass, which writes the finished,
        // already encoded numbers - so no conversion on write, in either colour mode.
        D3D12_RENDER_TARGET_VIEW_DESC RtvDesc;
        appMemzero(&RtvDesc, sizeof(RtvDesc));
        RtvDesc.Format        = Colours.BackBuffer;
        RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        Device->CreateRenderTargetView(BackBuffers[i], &RtvDesc, Handle);

        if (!Allocators[i] &&
            !Check(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  __uuidof(ID3D12CommandAllocator), (void**)&Allocators[i]),
                   TEXT("CreateCommandAllocator")))
            return 0;
    }

    if (!CommandList)
    {
        if (!Check(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocators[0], nullptr,
                                             __uuidof(ID3D12GraphicsCommandList), CommandList.VoidSlot()),
                   TEXT("CreateCommandList")))
            return 0;
        CommandList->Close();   // command lists are created in the recording state
    }

    if (!Fence)
    {
        if (!Check(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), Fence.VoidSlot()),
                   TEXT("CreateFence")))
            return 0;
        for (UINT i = 0; i < BACKEND_FRAME_COUNT; i++)
            FenceValues[i] = 0;
        FenceValues[FrameIndex] = 1;

        FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!FenceEvent)
        {
            RDX_LOG("CreateEvent failed");
            return 0;
        }
    }

    return 1;
}

// A window nothing has drawn into yet is white, and the game's first picture comes a good while after the chain exists:
// the backend's pipelines, then the engine's own start and its first frame. A black picture goes out now, so the wait
// shows black. Afterwards every frame slot starts from the same fence value, as after a resize.
//
// Not in frame generation's mode: there the chain belongs to the pacer's queue, and a back buffer written from this
// queue took the device away (measured: DXGI_ERROR_DEVICE_REMOVED at this very present, and the renderer could not
// start). The window's GDI black (the render device's Init) holds there until the first picture.
void FBackend::PresentBlack()
{
    if (bFgMode || !SwapChain || !CommandList || !Allocators[FrameIndex] || !RtvHeap)
        return;
    ID3D12CommandAllocator* const Allocator = Allocators[FrameIndex];
    if (FAILED(Allocator->Reset()) || FAILED(CommandList->Reset(Allocator, nullptr)))
        return;
    const UINT Back = SwapChain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER Barrier;
    appMemzero(&Barrier, sizeof(Barrier));
    Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource   = BackBuffers[Back];
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    CommandList->ResourceBarrier(1, &Barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
    Rtv.ptr += Back * RtvStride;
    const FLOAT Black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    CommandList->ClearRenderTargetView(Rtv, Black, 0, nullptr);
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    CommandList->ResourceBarrier(1, &Barrier);
    if (FAILED(CommandList->Close()))
        return;
    ID3D12CommandList* Lists[1] = { CommandList.Get() };
    Queue->ExecuteCommandLists(1, Lists);
    WaitForGpu();
    const HRESULT Hr = SwapChain->Present(0, bAllowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(Hr))
        RdxLog(TEXT("the window: the black picture before the first one was refused (0x%08X) - it stays white until "
                    "then"), (DWORD)Hr);
    else
        RdxLog(TEXT("the window: a black picture until the first one"));
    FrameIndex = SwapChain->GetCurrentBackBufferIndex();
    UINT64 Highest = 0;
    for (UINT i = 0; i < BACKEND_FRAME_COUNT; i++)
        if (FenceValues[i] > Highest)
            Highest = FenceValues[i];
    for (UINT i = 0; i < BACKEND_FRAME_COUNT; i++)
        FenceValues[i] = Highest;
}

// The caption's letters: five dots wide and seven high, a row a number, the left dot its sixteen.
struct FCaptionGlyph
{
    char Letter;
    BYTE Rows[7];
};
static const FCaptionGlyph GCaptionGlyphs[] =
{
    { 'A', { 14, 17, 17, 31, 17, 17, 17 } }, { 'C', { 14, 17, 16, 16, 16, 17, 14 } },
    { 'D', { 30, 17, 17, 17, 17, 17, 30 } }, { 'E', { 31, 16, 16, 30, 16, 16, 31 } },
    { 'G', { 14, 17, 16, 23, 17, 17, 15 } }, { 'H', { 17, 17, 17, 31, 17, 17, 17 } },
    { 'I', { 14, 4, 4, 4, 4, 4, 14 } },      { 'L', { 16, 16, 16, 16, 16, 16, 31 } },
    { 'M', { 17, 27, 21, 21, 17, 17, 17 } }, { 'N', { 17, 25, 21, 19, 17, 17, 17 } },
    { 'O', { 14, 17, 17, 17, 17, 17, 14 } }, { 'P', { 30, 17, 17, 30, 16, 16, 16 } },
    { 'R', { 30, 17, 17, 30, 20, 18, 17 } }, { 'S', { 15, 16, 16, 14, 1, 1, 30 } },
    { '0', { 14, 17, 19, 21, 25, 17, 14 } }, { '1', { 4, 12, 4, 4, 4, 4, 14 } },
    { '2', { 14, 17, 1, 2, 4, 8, 31 } },     { '3', { 30, 1, 1, 14, 1, 1, 30 } },
    { '4', { 2, 6, 10, 18, 31, 2, 2 } },     { '5', { 31, 16, 30, 1, 1, 17, 14 } },
    { '6', { 6, 8, 16, 30, 17, 17, 14 } },   { '7', { 31, 1, 2, 4, 8, 8, 8 } },
    { '8', { 14, 17, 17, 14, 17, 17, 14 } }, { '9', { 14, 17, 17, 15, 1, 2, 12 } },
    { '/', { 1, 1, 2, 4, 8, 16, 16 } }
};

void FBackend::CompilingWatch(void* Who)
{
    if (Who)
        ((FBackend*)Who)->PresentCompiling();
}

void FBackend::CompilingWait(void* Who, HANDLE Compiling)
{
    FBackend* const B = (FBackend*)Who;
    if (B && B->AnswerWhile && B->OutputWindow && ::GetCurrentThreadId() == B->CompilingThread)
        B->AnswerWhile(B->OutputWindow, Compiling);
    ::WaitForSingleObject(Compiling, INFINITE);
}

// Drawn with nothing but clears of rectangles of the back buffer: at the start there is no pipeline to draw with yet -
// making them is what the wait is for. Not in frame generation's mode, as the black picture is not (PresentBlack).
void FBackend::PresentCompiling()
{
    if (bFgMode || !SwapChain || !CommandList || !Allocators[FrameIndex] || !RtvHeap ||
        ::GetCurrentThreadId() != CompilingThread || Width < 320 || Height < 200)
        return;
    // RECTS_A_CLEAR: how many rectangles one clear is handed. The call itself takes any number, but a driver keeps
    // its own copy of them in a fixed array on its stack - NVIDIA's 32-bit one has room for 32 - and writes past its
    // end for a longer list: the process then ends inside the driver (a fail-fast no handler sees), with no line in
    // any log. Half of what that driver holds.
    enum { RECTS_MAX = 800, RECTS_A_CLEAR = 16 };
    // The text names the shader about to compile (Now). Where the cache's file gave nothing the start compiles every
    // shader the renderer has: their number is known (START_SHADERS), and the bar shows the ones done. Where it gave
    // some - a new renderer over the file of an older one - only the shaders that changed compile, and how many is not
    // known before: the count alone, with no bar.
    const INT   Done   = RdxStartupShaders();
    const INT   Now    = Done + 1;
    const UBOOL bWhole = RdxShaderCacheRead() == 0;
    const INT   Total  = Max(Now, (INT)START_SHADERS);
    char Text[64];
    if (bWhole)
        _snprintf_s(Text, sizeof(Text), _TRUNCATE, "COMPILING SHADERS  %i / %i", Now, Total);
    else
        _snprintf_s(Text, sizeof(Text), _TRUNCATE, "COMPILING SHADERS  %i", Now);
    INT Chars = 0;
    while (Text[Chars])
        Chars++;
    static D3D12_RECT Rects[RECTS_MAX];
    INT       Count = 0;
    const INT Dot   = Max(2, Height / 360);
    const INT Left  = (Width - Chars * 6 * Dot) / 2;
    const INT Top   = Height * 7 / 8;
    for (INT c = 0; c < Chars; c++)
        for (INT g = 0; g < (INT)(sizeof(GCaptionGlyphs) / sizeof(GCaptionGlyphs[0])); g++)
        {
            if (GCaptionGlyphs[g].Letter != Text[c])
                continue;
            // A row's dots that touch go in as one rectangle.
            for (INT r = 0; r < 7; r++)
                for (INT x = 0; x < 5; x++)
                {
                    if (!(GCaptionGlyphs[g].Rows[r] & (16 >> x)))
                        continue;
                    INT End = x;
                    while (End + 1 < 5 && (GCaptionGlyphs[g].Rows[r] & (16 >> (End + 1))))
                        End++;
                    if (Count < RECTS_MAX)
                    {
                        Rects[Count].left   = Left + (c * 6 + x) * Dot;
                        Rects[Count].right  = Left + (c * 6 + End + 1) * Dot;
                        Rects[Count].top    = Top + r * Dot;
                        Rects[Count].bottom = Top + (r + 1) * Dot;
                        Count++;
                    }
                    x = End;
                }
            break;
        }
    // The bar under it: its rim, then as much of its inside as is done.
    const INT BarWide = Width * 2 / 5, BarHigh = 3 * Dot;
    const INT BarLeft = (Width - BarWide) / 2, BarTop = Top + 11 * Dot;
    D3D12_RECT Rim[4];
    Rim[0].left = BarLeft;                 Rim[0].right = BarLeft + BarWide; Rim[0].top = BarTop;
    Rim[0].bottom = BarTop + 1;
    Rim[1] = Rim[0];                       Rim[1].top = BarTop + BarHigh - 1; Rim[1].bottom = BarTop + BarHigh;
    Rim[2].left = BarLeft;                 Rim[2].right = BarLeft + 1;       Rim[2].top = BarTop;
    Rim[2].bottom = BarTop + BarHigh;
    Rim[3] = Rim[2];                       Rim[3].left = BarLeft + BarWide - 1; Rim[3].right = BarLeft + BarWide;
    D3D12_RECT Fill;
    Fill.left   = BarLeft + 2;
    Fill.right  = BarLeft + 2 + (INT)((FLOAT)(BarWide - 4) * Min(1.0f, (FLOAT)Done / (FLOAT)Total));
    Fill.top    = BarTop + 2;
    Fill.bottom = BarTop + BarHigh - 2;

    ID3D12CommandAllocator* const Allocator = Allocators[FrameIndex];
    if (FAILED(Allocator->Reset()) || FAILED(CommandList->Reset(Allocator, nullptr)))
        return;
    const UINT Back = SwapChain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER Barrier;
    appMemzero(&Barrier, sizeof(Barrier));
    Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource   = BackBuffers[Back];
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    CommandList->ResourceBarrier(1, &Barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
    Rtv.ptr += Back * RtvStride;
    const FLOAT Black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const FLOAT Pale[4]  = { 0.92f, 0.92f, 0.92f, 1.0f };
    const FLOAT Dim[4]   = { 0.32f, 0.32f, 0.32f, 1.0f };
    CommandList->ClearRenderTargetView(Rtv, Black, 0, nullptr);
    // The caption's rectangles a few to a clear, never the whole list in one (RECTS_A_CLEAR).
    for (INT First = 0; First < Count; First += RECTS_A_CLEAR)
        CommandList->ClearRenderTargetView(Rtv, Pale, (UINT)Min(Count - First, (INT)RECTS_A_CLEAR), Rects + First);
    if (bWhole)
    {
        CommandList->ClearRenderTargetView(Rtv, Dim, 4, Rim);
        if (Fill.right > Fill.left && Fill.bottom > Fill.top)
            CommandList->ClearRenderTargetView(Rtv, Pale, 1, &Fill);
    }
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    CommandList->ResourceBarrier(1, &Barrier);
    if (FAILED(CommandList->Close()))
        return;
    ID3D12CommandList* Lists[1] = { CommandList.Get() };
    Queue->ExecuteCommandLists(1, Lists);
    WaitForGpu();
    SwapChain->Present(0, bAllowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    FrameIndex = SwapChain->GetCurrentBackBufferIndex();
    UINT64 Highest = 0;
    for (UINT i = 0; i < BACKEND_FRAME_COUNT; i++)
        if (FenceValues[i] > Highest)
            Highest = FenceValues[i];
    for (UINT i = 0; i < BACKEND_FRAME_COUNT; i++)
        FenceValues[i] = Highest;
    static INT Said = 0;
    if (Said++ == 0)
        RdxLog(TEXT("the window: shaders are being compiled - the caption says so until the backend is up"));
}

UBOOL FBackend::CreateDepthBuffer()
{
    DepthBuffer.Release();
    DepthBuffer2.Release();
    bDepthReadable = 0;
    WaterDepth.Release();       // a copy of this buffer, made again on first use (EnsureWaterTargets)
    ReliefCover.Release();      // and another (EnsureReliefCover)
    RdxViewsChanged();          // the passes' views of them go with them (ViewGeneration.h)

    if (!DsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        HeapDesc.NumDescriptors = 1;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), DsvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(DSV)")))
            return 0;
    }

    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = SceneWidth;
    Desc.Height           = SceneHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    // Typeless where something reads the same storage as a float texture (NeedsDepthRead). Asked once: the shadows
    // may still go further down, and what is made here stays what it was made as (bDepthReadable).
    // With the shadows on, a stencil too: the surface kinds (FSolidPass::SetKind).
    const UBOOL bReadable = NeedsDepthRead() ? 1 : 0;
    Desc.Format           = bShadows ? DXGI_FORMAT_R32G8X24_TYPELESS :
                            (bReadable ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT);
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // Reversed-Z: the far plane is 0, so that is what a cleared buffer must contain.
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format               = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    Clear.DepthStencil.Depth   = 0.0f;

    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &Clear,
                                               __uuidof(ID3D12Resource), DepthBuffer.VoidSlot()),
               TEXT("CreateCommittedResource(depth)")))
        return 0;

    D3D12_DEPTH_STENCIL_VIEW_DESC ViewDesc;
    appMemzero(&ViewDesc, sizeof(ViewDesc));
    ViewDesc.Format        = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    ViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    Device->CreateDepthStencilView(DepthBuffer.Get(), &ViewDesc, DsvHeap->GetCPUDescriptorHandleForHeapStart());

    if (bReadable)
    {
        if (!DepthSrvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
            appMemzero(&HeapDesc, sizeof(HeapDesc));
            HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            HeapDesc.NumDescriptors = 3;        // the two depth buffers, and the water's own depth (WaterSurfaceDepthSrv)
            HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), DepthSrvHeap.VoidSlot()),
                       TEXT("CreateDescriptorHeap(depth SRV)")))
                return 0;
        }
        if (!DsvHeap2)
        {
            D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
            appMemzero(&HeapDesc, sizeof(HeapDesc));
            HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            HeapDesc.NumDescriptors = 1;
            if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), DsvHeap2.VoidSlot()),
                       TEXT("CreateDescriptorHeap(DSV 2)")))
                return 0;
        }
        // The second depth buffer, the same description - see ClearDepthMidFrame.
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE, &Clear,
                                                   __uuidof(ID3D12Resource), DepthBuffer2.VoidSlot()),
                   TEXT("CreateCommittedResource(depth 2)")))
            return 0;
        Device->CreateDepthStencilView(DepthBuffer2.Get(), &ViewDesc, DsvHeap2->GetCPUDescriptorHandleForHeapStart());

        D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc;
        appMemzero(&SrvDesc, sizeof(SrvDesc));
        SrvDesc.Format                  = bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT;
        SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SrvDesc.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE Srv = DepthSrvHeap->GetCPUDescriptorHandleForHeapStart();
        Device->CreateShaderResourceView(DepthBuffer.Get(), &SrvDesc, Srv);
        Srv.ptr += Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Device->CreateShaderResourceView(DepthBuffer2.Get(), &SrvDesc, Srv);
        // The water's own depth (WaterSurfaceDepthSrv): a view of nothing until it is made (EnsureWaterSurface).
        Srv.ptr += Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC WaterDesc = SrvDesc;
        WaterDesc.Format = DXGI_FORMAT_R32_FLOAT;
        Device->CreateShaderResourceView(WaterSurface.Get(), &WaterDesc, Srv);

        if (bShadows)
        {
            // The shadows' own heap and target. Where these cannot be made, the SHADOWS go and the renderer stays:
            // with the light target's create forced to fail (the marker SecondRevision-fail-alloc.txt) the whole of
            // Init once failed on it, the renderer never came up, and the game sat on the title screen for ever -
            // over a target that only the shadows use.
            UBOOL bShadowTargets = 1;
            if (!ShadowSrvHeap)
            {
                D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
                appMemzero(&HeapDesc, sizeof(HeapDesc));
                HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                HeapDesc.NumDescriptors = SHADOW_SRVS;
                HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), ShadowSrvHeap.VoidSlot()),
                           TEXT("CreateDescriptorHeap(shadow SRV)")))
                    bShadowTargets = 0;
            }
            if (bShadowTargets && !CreateLightTarget())
                bShadowTargets = 0;
            if (!bShadowTargets)
            {
                RdxLog(TEXT("shadows: their own heap or target could not be made - the renderer goes on WITHOUT "
                            "shadows, and the rest of the picture is as it was"));
                bShadows = 0;
                ShadowSrvHeap.Release();
                LightTarget.Release();
                AddedTarget.Release();
                // The buffers made above carry the stencil plane the shadows' surface kinds were for, and from here
                // on everything that asks the depth's format is told the plain one (bShadows): made again as that,
                // so that no pipeline is built for another format than the buffer it draws into has. Whoever had
                // pipelines standing for the shadows' format makes them again too (Resize).
                return CreateDepthBuffer();
            }
            else
            {
            // Two tables of six, the world's buffer first: depth 1, depth 2, stencil 1, light, the two cube arrays | depth 2,
            // depth 1, stencil 2, light, the cube arrays. The stencil is the depth resource's second plane, the light the
            // geometry pass's second target (CreateLightTarget), the cubes FShadowMaps' (WriteShadowCubeViews). Slot 12:
            // the factor target (EnsureShadowTarget).
            D3D12_SHADER_RESOURCE_VIEW_DESC StencilDesc = SrvDesc;
            StencilDesc.Format               = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
            StencilDesc.Texture2D.PlaneSlice = 1;
            D3D12_SHADER_RESOURCE_VIEW_DESC LightDesc = SrvDesc;
            LightDesc.Format = RDX_LIGHT_TARGET_FORMAT;
            const UINT Inc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            for (INT t = 0; t < 2; t++)
            {
                ID3D12Resource* const Own   = (t == 0) ? DepthBuffer.Get() : DepthBuffer2.Get();
                ID3D12Resource* const Other = (t == 0) ? DepthBuffer2.Get() : DepthBuffer.Get();
                ID3D12Resource* const Order[4] = { Own, Other, Own, LightTarget.Get() };
                D3D12_CPU_DESCRIPTOR_HANDLE S = ShadowSrvHeap->GetCPUDescriptorHandleForHeapStart();
                S.ptr += (SIZE_T)(t * SHADOW_TABLE) * Inc;
                for (INT i = 0; i < 4; i++)
                {
                    const D3D12_SHADER_RESOURCE_VIEW_DESC* View = (i == 2) ? &StencilDesc : ((i == 3) ? &LightDesc : &SrvDesc);
                    Device->CreateShaderResourceView(Order[i], View, S);
                    S.ptr += Inc;
                }
            }
            // Slot 13: the added target (CreateLightTarget), which the apply pass reads beside the factor at 12.
            D3D12_SHADER_RESOURCE_VIEW_DESC AddedDesc = SrvDesc;
            AddedDesc.Format = Colours.Draw;
            D3D12_CPU_DESCRIPTOR_HANDLE AddedSrv = ShadowSrvHeap->GetCPUDescriptorHandleForHeapStart();
            AddedSrv.ptr += (SIZE_T)SHADOW_ADDED_SRV * Inc;
            Device->CreateShaderResourceView(AddedTarget.Get(), &AddedDesc, AddedSrv);
            WriteShadowCubeViews();
            WriteAoDepthViews();
            bWaterShadowViewsDirty = 1;
            }
        }
    }
    bDepthReadable    = bReadable;
    bDepthReadRefused = 0;
    return 1;
}

// The cube arrays' views in both tables of ShadowSrvHeap (slots 4 and 5, 10 and 11). Without shadow maps nothing draws
// with those slots.
void FBackend::WriteShadowCubeViews()
{
    if (!ShadowSrvHeap || !Shadow.IsReady())
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC Cube;
    Shadow.DescribeCubeView(Cube);
    const UINT Inc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (INT t = 0; t < 2; t++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE S = ShadowSrvHeap->GetCPUDescriptorHandleForHeapStart();
        S.ptr += (SIZE_T)(t * SHADOW_TABLE + 4) * Inc;
        Device->CreateShaderResourceView(Shadow.GetWorldCubes(), &Cube, S);
        S.ptr += Inc;
        Device->CreateShaderResourceView(Shadow.GetActorCubes(), &Cube, S);
    }
    bWaterShadowViewsDirty = 1;     // the water's runs show them as well
    // Slot 14: the masks of masked casters, read by the actors' maps (FShadowMaps::Render).
    D3D12_SHADER_RESOURCE_VIEW_DESC Masks;
    Shadow.DescribeMaskView(Masks);
    D3D12_CPU_DESCRIPTOR_HANDLE M = ShadowSrvHeap->GetCPUDescriptorHandleForHeapStart();
    M.ptr += (SIZE_T)SHADOW_MASK_SRV * Inc;
    Device->CreateShaderResourceView(Shadow.GetMaskArray(), &Masks, M);
    // The far probe's two views in the texture heap - the geometry pass sees that heap alone - may not be there
    // yet: UpdateRtFar writes them.
    bRtFarViews = 0;
}

// Shadow maps (RequestShadowQuality): the factor target, R8 - the output's size, or half of it (the preset's
// bFullResolution). Upscaling: the scene's size instead, the smaller one - so a factor pixel stays a whole number of
// depth pixels (the factor shader's Scale).
// SMAA's edge picture and the pair of views its second pass reads. The colour it reads is whatever the resolve
// leaves behind: the scene itself when nothing else resolves, the middle picture otherwise - the same choice
// ResolveScene makes, made once here.
UBOOL FBackend::CreateSmaaTargets()
{
    SmaaEdges.Release();
    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
    appMemzero(&HeapDesc, sizeof(HeapDesc));
    HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HeapDesc.NumDescriptors = 1;
    if (!SmaaRtvHeap &&
        !Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), SmaaRtvHeap.VoidSlot()),
               TEXT("CreateDescriptorHeap(SMAA RTV)")))
        return 0;
    appMemzero(&HeapDesc, sizeof(HeapDesc));
    HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.NumDescriptors = 2;
    HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!SmaaSrvHeap &&
        !Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), SmaaSrvHeap.VoidSlot()),
               TEXT("CreateDescriptorHeap(SMAA SRV)")))
        return 0;

    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = Width;
    Desc.Height           = Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R8G8_UNORM;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    // Cleared to no edge every frame (DrawSmaa): said here, so the clear is the driver's quick one.
    D3D12_CLEAR_VALUE NoEdge;
    appMemzero(&NoEdge, sizeof(NoEdge));
    NoEdge.Format = Desc.Format;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &NoEdge,
                                               __uuidof(ID3D12Resource), SmaaEdges.VoidSlot()),
               TEXT("CreateCommittedResource(SMAA edges)")))
        return 0;
    Device->CreateRenderTargetView(SmaaEdges.Get(), nullptr, SmaaRtvHeap->GetCPUDescriptorHandleForHeapStart());

    ID3D12Resource* const Source = (SceneScale > 1 || UpscaleMode) ? UpscaleTemp.Get() : SceneColor.Get();
    if (!Source)
        return 0;
    // A FULLY QUALIFIED description, not a null one: the scene target is created TYPELESS (so the depth-read and the
    // sRGB paths can each see it their own way), and a typeless resource has no default view. A null desc there is an
    // illegal call - D3D12 removes the device for it, and then everything created afterwards fails with no clue as to
    // why. Measured the hard way: the next allocation to fail was the texture cache's heap, which had
    // nothing to do with it.
    const UINT Inc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc;
    appMemzero(&SrvDesc, sizeof(SrvDesc));
    SrvDesc.Format                  = Colours.Draw;
    SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SrvDesc.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE H = SmaaSrvHeap->GetCPUDescriptorHandleForHeapStart();
    Device->CreateShaderResourceView(Source, &SrvDesc, H);
    H.ptr += Inc;
    SrvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    Device->CreateShaderResourceView(SmaaEdges.Get(), &SrvDesc, H);
    RDX_LOGF("antialiasing (SMAA): edge image %ix%i, colour comes from the %s", Width, Height,
             (SceneScale > 1 || UpscaleMode) ? TEXT("intermediate image") : TEXT("scene"));
    return 1;
}

UBOOL FBackend::EnsureShadowTarget()
{
    const INT Div   = ShadowFactorDivisor();
    const INT BaseW = UpscaleMode ? SceneWidth : Width, BaseH = UpscaleMode ? SceneHeight : Height;
    const INT W = (BaseW + Div - 1) / Div, H = (BaseH + Div - 1) / Div;
    if (ShadowColor && ShadowW == W && ShadowH == H)
        return 1;
    ShadowColor.Release();
    if (!ShadowRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 1;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), ShadowRtvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(shadow RTV)")))
            return 0;
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = W;
    Desc.Height           = H;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R8_UNORM;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                               __uuidof(ID3D12Resource), ShadowColor.VoidSlot()),
               TEXT("CreateCommittedResource(shadow factor)")))
        return 0;
    Device->CreateRenderTargetView(ShadowColor.Get(), nullptr, ShadowRtvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc;
    appMemzero(&SrvDesc, sizeof(SrvDesc));
    SrvDesc.Format                  = DXGI_FORMAT_R8_UNORM;
    SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SrvDesc.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE S = ShadowSrvHeap->GetCPUDescriptorHandleForHeapStart();
    S.ptr += SHADOW_FACTOR_SRV * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Device->CreateShaderResourceView(ShadowColor.Get(), &SrvDesc, S);
    ShadowW = W;
    ShadowH = H;
    bWaterShadowViewsDirty = 1;
    RdxLog(TEXT("shadows: factor target %ix%i"), W, H);
    return 1;
}

static void PutFloat(UINT* Values, INT Index, FLOAT F)
{
    appMemcpy(&Values[Index], &F, sizeof(FLOAT));
}

// How strong a shadow is allowed to be. A light's estimated share of a pixel counts GShadowCalib times over, and one
// light may take at most GShadowMaxShare of that pixel's light away. The multiplier exists because the estimate is
// conservative: at 1.0 a shadow darkens the picture by only a fifth or so, which is invisible on a dark street.
// Neither number is calibrated against a reference picture yet.
static const FLOAT GShadowCalib    = 8.0f;
static const FLOAT GShadowMaxShare = 0.95f;
// The soft edge's radius, in texels of an actors' map face.
static const FLOAT GShadowSoftTexels = 2.5f;

void FBackend::RequestShadowQuality(INT Quality)
{
    const RdxMath::EShadowQuality Q = (Quality >= RdxMath::ShadowQuality_Low && Quality < RdxMath::ShadowQuality_Count)
                                      ? (RdxMath::EShadowQuality)Quality : RdxMath::ShadowQuality_Off;
    ShadowPreset     = RdxMath::ShadowPresetFor(Q);      // an RT one: its maps' quality's
    bShadows         = (Q != RdxMath::ShadowQuality_Off) ? 1 : 0;
    bRtShadowSetting = RdxMath::IsRtShadowQuality(Q) ? 1 : 0;
}

UBOOL FBackend::SetShadowMapsWorld(const FLOAT* Xyz, INT VertexCount)
{
    if (!bShadows || !Shadow.IsReady())
        return 0;
    // Where the rays go through the host, the GPU is done with the frames under way before either list is let go, as
    // it is when the rays' list follows (SetShadowRtWorld).
    if (Rt.IsHosted() && Rt.IsReady())
        WaitForGpu();
    return Shadow.SetWorld(Device.Get(), Xyz, VertexCount, this);
}

void FBackend::SetShadowRtWorld(const FLOAT* RtXyz, INT RtVertexCount, const DWORD* RtSurf, const DWORD* SurfAlbedo,
                                INT SurfCount, const FLOAT* SkyXyz, INT SkyVertexCount, const FLOAT* ThinXyz,
                                INT ThinVertexCount, const DWORD* ThinSurf)
{
    if (!bShadows || !Shadow.IsReady())
        return;
    // The rays' own copy, split for them (FRtPass::SetLevel), kept for the host's structures. The GPU finishes what
    // it was given first: a frame still under way may be copying the level's buffers that are let go here.
    if (Rt.IsHosted() && Rt.IsReady())
    {
        WaitForGpu();
        Rt.SetThin(ThinXyz, ThinVertexCount, ThinSurf, this);
        Rt.SetLevel(RtXyz, RtVertexCount, RtSurf, SurfAlbedo, SurfCount, SkyXyz, SkyVertexCount, this);
    }
}

FLOAT* FBackend::AddShadowCaster(INT VertexCount, FLOAT X, FLOAT Y, FLOAT Z, FLOAT Radius, INT& Index)
{
    Index = -1;
    if (!bShadows || !bInFrame || !Shadow.IsReady())
        return nullptr;
    return Shadow.AddCaster(VertexCount, X, Y, Z, Radius, Index);
}

void FBackend::SetMotionBodies(const FMotionBody* List, INT Count)
{
    MotionBodyCount = 0;
    if (!List || Count <= 0)
        return;
    const INT Max = (INT)MAX_MOTION_BODIES;
    for (INT i = 0; i < Count && MotionBodyCount < Max; i++)
        MotionBodies[MotionBodyCount++] = List[i];
}

// The buffers are made the first time a body asks for room: a game played without a temporal upscaler never has them.
// One a frame slot, mapped for good - what a frame writes is read by that frame's own draws, and by nobody after.
FLOAT* FBackend::MotionVertices(INT VertexCount, INT& First)
{
    First = -1;
    if (!bInFrame || !Device || VertexCount <= 0 || MotionVbState < 0)
        return nullptr;
    if (MotionVbState == 0)
    {
        MotionVbState = 1;
        D3D12_HEAP_PROPERTIES Heap;
        appMemzero(&Heap, sizeof(Heap));
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc;
        appMemzero(&Desc, sizeof(Desc));
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = (UINT64)MOTION_VERTEX_MAX * 3 * sizeof(FLOAT);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.SampleDesc.Count = 1;
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (INT s = 0; s < BACKEND_FRAME_COUNT && MotionVbState > 0; s++)
        {
            void* Mapped = nullptr;
            if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       __uuidof(ID3D12Resource), MotionVb[s].VoidSlot()),
                       TEXT("CreateCommittedResource(motion bodies)")) ||
                FAILED(MotionVb[s]->Map(0, nullptr, &Mapped)) || !Mapped)
                MotionVbState = -1;
            MotionVbMapped[s] = (FLOAT*)Mapped;
        }
        RdxLog(TEXT("motion vectors (objects): the bodies' own buffer %s - %i vertices a frame"),
               (MotionVbState > 0) ? TEXT("made") : TEXT("could NOT be made: only the shadows' casters get a vector"),
               (INT)MOTION_VERTEX_MAX);
        if (MotionVbState < 0)
            return nullptr;
    }
    if (MotionVbUsed + VertexCount > (INT)MOTION_VERTEX_MAX)
    {
        MotionVbRefused++;
        return nullptr;
    }
    First         = MotionVbUsed;
    MotionVbUsed += VertexCount;
    return MotionVbMapped[FrameIndex] + (SIZE_T)First * 3;
}

void FBackend::SetShadowLights(const FShadowLightFrame* Lights, INT Count, const FLOAT* CameraAxes, const FLOAT* CameraEye)
{
    if (!bShadows || !bInFrame || !Shadow.IsReady())
        return;
    for (INT i = 0; i < 9; i++)
        ShadowAxes[i] = CameraAxes ? CameraAxes[i] : 0.0f;
    for (INT i = 0; i < 3; i++)
        ShadowEye[i] = CameraEye ? CameraEye[i] : 0.0f;
    Shadow.SetLights(Lights, Count);
    if (Shadow.ListsRefused())
        RefuseShadows(TEXT("the maps' lists of casters"));
}

void FBackend::RefuseShadows(const TCHAR* What)
{
    if (Refused(ShadowRefusal))
        return;
    ShadowRefusal.Note(TargetAsk());
    RdxLog(TEXT("shadows: no memory for %s - left out, and asked again when a size or the colours change"), What);
}

void FBackend::SetViewCamera(const FLOAT* Eye, const FLOAT* Axes)
{
    if (!Eye || !Axes)
        return;
    for (INT i = 0; i < 3; i++)
        CamEye[i] = Eye[i];
    for (INT i = 0; i < 9; i++)
        CamAxes[i] = Axes[i];
    bCamSet = 1;
}

void FBackend::SetShadowView(const FLOAT* Rect, FLOAT TanX, FLOAT TanY)
{
    if (!Rect || !(Rect[2] > 0.0f) || !(Rect[3] > 0.0f))
        return;
    for (INT k = 0; k < 4; k++)
        ShadowViewRect[k] = Rect[k];
    ShadowTanX     = TanX;
    ShadowTanY     = TanY;
    bShadowViewSet = 1;
}

void FBackend::SkipShadowRect(const FLOAT* Rect)
{
    if (!Rect || !(Rect[2] > 0.0f) || !(Rect[3] > 0.0f) || ShadowSkipCount >= SHADOW_SKIP_RECTS)
        return;
    for (INT r = 0; r < ShadowSkipCount; r++)
        if (ShadowSkip[r * 4] == Rect[0] && ShadowSkip[r * 4 + 1] == Rect[1] && ShadowSkip[r * 4 + 2] == Rect[2] &&
            ShadowSkip[r * 4 + 3] == Rect[3])
            return;
    for (INT k = 0; k < 4; k++)
        ShadowSkip[ShadowSkipCount * 4 + k] = Rect[k];
    ShadowSkipCount++;
}

// The factor shader's constant buffer - ShadowFactor.hlsl's cbuffer. The layout is written down ONCE, in RenderMath.h's
// ShadowCb* enum, and both sides read it from there; the light arrays are last so that their length can change without
// shifting anything.
void FBackend::FillShadowConstants(BYTE* Out, UBOOL bRt, INT* Order, INT& Ordered) const
{
    UINT V[RdxMath::ShadowCbCount];
    appMemzero(V, sizeof(V));
    const RdxMath::FShadowPreset& P = Shadow.GetPreset();
    // The world view's own tangents and place (SetShadowView) - else the viewport's, over the whole output.
    PutFloat(V, 0, bShadowViewSet ? ShadowTanX : ViewTanX);
    PutFloat(V, 1, bShadowViewSet ? ShadowTanY : ViewTanY);
    for (INT k = 0; k < 4; k++)
        PutFloat(V, RdxMath::ShadowCbViewRect + k, bShadowViewSet ? ShadowViewRect[k] : ((k < 2) ? 0.0f : 1.0f));
    for (INT r = 0; r < ShadowSkipCount; r++)
        for (INT k = 0; k < 4; k++)
            PutFloat(V, RdxMath::ShadowCbSkipRect + 4 * r + k, ShadowSkip[r * 4 + k]);
    V[RdxMath::ShadowCbSkipCount] = (UINT)ShadowSkipCount;
    // Scene pixels to one factor texel (the shader multiplies its own pixel by this to read the depth and the light,
    // both scene-sized). The factor target follows the OUTPUT when nothing is upscaled and the SCENE when something is
    // - EnsureShadowTarget - so the ratio differs between the two, and with supersampling the scene is bigger than the
    // output on top of that. Written out rather than derived from the two sizes: an integer division would fall to 1
    // on an odd scene side (Balanced at 4320 gives 2541), and a wrong ratio puts every shadow in the wrong place.
    V[2] = (UINT)(UpscaleMode ? ShadowFactorDivisor() : (SceneScale * ShadowFactorDivisor()));
    // What was drawn after the world is in the other buffer only if the drawing moved there (ClearDepthMidFrame).
    V[4] = (WorldDepthIndex() != ActiveDepth) ? 0 : 1;
    // The share of the pixel's own light (Flags 1) - absolute with the marker; Flags 2: the share map (depth view 5).
    V[5] = (bShadowAbsolute ? 0u : 1u) | ((DepthViewMode == 5) ? 2u : 0u) | (bShadowWorldCast ? 4u : 0u) |
           ((bRt && bRtShadowCounts) ? 16u : 0u) | ((bRt && bRtShadowMaskOff) ? 32u : 0u) |
           ((bRt && RtShadowOnly == 1) ? 64u : 0u) | ((bRt && RtShadowOnly == 2) ? 128u : 0u);
    PutFloat(V, 6, GShadowCalib);
    PutFloat(V, 7, GShadowMaxShare * ShadowStrength);
    for (INT k = 0; k < 3; k++)
    {
        PutFloat(V, 8 + k,  ShadowAxes[k]);
        PutFloat(V, 12 + k, ShadowAxes[3 + k]);
        PutFloat(V, 16 + k, ShadowAxes[6 + k]);
    }
    PutFloat(V, 11, Shadow.GetNear());
    PutFloat(V, 15, 2.0f / (FLOAT)((P.ActorMapSize > 0) ? P.ActorMapSize : 1));
    PutFloat(V, 19, 2.0f / (FLOAT)((P.WorldMapSize > 0) ? P.WorldMapSize : 1));
    V[20] = (UINT)P.FilterTaps;
    V[21] = (DepthViewMode == 3 || DepthViewMode == 5) ? 1 : 0;   // the lights' projections as dots, in the diagnostic views
    PutFloat(V, 22, GShadowSoftTexels * 2.0f / (FLOAT)((P.ActorMapSize > 0) ? P.ActorMapSize : 1));
    // The RT shadows' rays (ShadowFactor.hlsl with RDX_RT): their lights' size, and the camera the surface points and the
    // lights are placed from - the same axes, the same origin the lights' camera-space places were made with.
    PutFloat(V, RdxMath::ShadowCbLightSize, RdxMath::RtShadowLightRadius);
    for (INT k = 0; k < 3; k++)
        PutFloat(V, RdxMath::ShadowCbEye + k, ShadowEye[k]);
    // The lights whose maps are drawn this frame (FShadowMaps::IsLightReady): one whose world map waits is left out. The
    // RT factor needs no map, and takes every light of the pick.
    INT Count = 0;
    for (INT i = 0; i < Shadow.GetLightCount() && Count < RdxMath::ShadowMaxLights; i++)
    {
        if (!bRt && !Shadow.IsLightReady(i))
            continue;
        if (Order)
            Order[Count] = i;
        const FShadowLightFrame& L = Shadow.GetLight(i);
        for (INT k = 0; k < 3; k++)
            PutFloat(V, RdxMath::ShadowCbLightCam + 4 * Count + k, L.Cam[k]);
        PutFloat(V, RdxMath::ShadowCbLightCam + 4 * Count + 3, L.Radius);
        PutFloat(V, RdxMath::ShadowCbLightInfo + 4 * Count, (FLOAT)Shadow.GetSlot(i));
        PutFloat(V, RdxMath::ShadowCbLightInfo + 4 * Count + 1, L.Bright);
        PutFloat(V, RdxMath::ShadowCbLightInfo + 4 * Count + 2, L.Weight);   // the graduated pick's, 0..1
        for (INT k = 0; k < 4; k++)
            PutFloat(V, RdxMath::ShadowCbLightSpot + 4 * Count + k, L.Spot[k]);
        Count++;
    }
    V[3] = (UINT)Count;
    Ordered = Count;
    appMemcpy(Out, V, sizeof(V));

    // Diagnostic, marker only (SecondRevision-tile-probe.txt): would a TILED light list be worth building? The factor
    // pass walks every light for every pixel, twice, with no screen-area culling at all, and it is roughly two
    // thirds of the shadow cost. A tiled list would only pay if most (tile, light) pairs can be dropped - this
    // counts how many survive, so the answer is known BEFORE the compute pass, the root signature and the shader are
    // rewritten. If the ratio is poor, the whole item can be closed without building anything.
    // Runs ONCE, not per frame: ~100k sphere-vs-tile tests every frame would distort exactly what it measures.
    // But NOT on the first frame either: the lights' maps are built a couple per frame, so Count starts at a handful
    // and climbs to the preset's maximum. Fired at once it measures two lights of fifty, which says nothing about the
    // real load. So it waits for a full set, with a fallback after 300 frames for a scene that never has that many
    // lights - otherwise it would never run there at all.
    if (bTileProbe && !bTileProbeDone)
        TileProbeFrames++;
    if (bTileProbe && !bTileProbeDone && Count > 0 &&
        (Count >= P.MaxLights || TileProbeFrames > 300))
    {
        bTileProbeDone = 1;
        const FLOAT TanX = bShadowViewSet ? ShadowTanX : ViewTanX;
        const FLOAT TanY = bShadowViewSet ? ShadowTanY : ViewTanY;
        enum { TILES_X = 60, TILES_Y = 34 };       // ~64x64 pixels at 3840x2160 - coarse on purpose, an ratio is wanted
        INT Pairs = 0, EmptyTiles = 0, MostHere = 0;
        for (INT ty = 0; ty < TILES_Y; ty++)
            for (INT tx = 0; tx < TILES_X; tx++)
            {
                const FLOAT TileL = TanX * (2.0f * (FLOAT)tx       / (FLOAT)TILES_X - 1.0f);
                const FLOAT TileR = TanX * (2.0f * (FLOAT)(tx + 1) / (FLOAT)TILES_X - 1.0f);
                const FLOAT TileT = TanY * (2.0f * (FLOAT)ty       / (FLOAT)TILES_Y - 1.0f);
                const FLOAT TileB = TanY * (2.0f * (FLOAT)(ty + 1) / (FLOAT)TILES_Y - 1.0f);
                INT Here = 0, Seen = 0;
                for (INT i = 0; i < Shadow.GetLightCount() && Seen < Count; i++)
                {
                    if (!Shadow.IsLightReady(i))
                        continue;
                    const FShadowLightFrame& Lf = Shadow.GetLight(i);
                    Seen++;
                    if (RdxMath::SphereTouchesTile(Lf.Cam[0], Lf.Cam[1], Lf.Cam[2], Lf.Radius,
                                                   TileL, TileR, TileT, TileB))
                        Here++;
                }
                Pairs += Here;
                if (Here == 0)
                    EmptyTiles++;
                if (Here > MostHere)
                    MostHere = Here;
            }
        const INT AllPairs = TILES_X * TILES_Y * Count;
        RdxLog(TEXT("tile measurement: %ix%i tiles, %i lights -> %i pairs out of %i (%.1f%%); %.1f lights per tile on average, ")
               TEXT("most %i, %i empty tiles out of %i"),
               (INT)TILES_X, (INT)TILES_Y, Count, Pairs, AllPairs,
               (AllPairs > 0) ? (100.0f * (FLOAT)Pairs / (FLOAT)AllPairs) : 0.0f,
               (FLOAT)Pairs / (FLOAT)(TILES_X * TILES_Y), MostHere, EmptyTiles, (INT)(TILES_X * TILES_Y));
    }
}

// Shadow maps (docs/terv-latvany-roadmap.md 3.2), onto the finished scene before it is box-filtered and the HUD comes: the
// maps drawn (FShadowMaps::Render), the factor from them (ShadowFactor.hlsl), the factor multiplied in. The scene
// target is bound and in the render target state.
// One multisampled surface down to one sample, into the target everything else already reads.
static void ResolveOne(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* Src, ID3D12Resource* Dst, DXGI_FORMAT Fmt)
{
    if (!Src || !Dst)
        return;
    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 2; i++)
    {
        B[i].Type                    = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource  = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B[i].Transition.StateBefore  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    B[0].Transition.pResource  = Src;
    B[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    B[1].Transition.pResource  = Dst;
    B[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    Cmd->ResourceBarrier(2, B);

    Cmd->ResolveSubresource(Dst, 0, Src, 0, Fmt);

    for (INT i = 0; i < 2; i++)
    {
        const D3D12_RESOURCE_STATES Was = B[i].Transition.StateBefore;
        B[i].Transition.StateBefore = B[i].Transition.StateAfter;
        B[i].Transition.StateAfter  = Was;
    }
    Cmd->ResourceBarrier(2, B);
}

// The world's multisampled depth into one of the two ordinary depth buffers - the one that, from here on, IS the
// world's depth as far as the shadows, the motion vectors and the upscaler are concerned.
void FBackend::ResolveMsaaDepth(ID3D12GraphicsCommandList* Cmd, INT Index)
{
    if (!MsaaOn() || !MsaaDepth || !MsaaSrvHeap || !Final.HasMsaaDepth())
        return;
    ID3D12Resource* const Dst = (Index == 1 && DepthBuffer2) ? DepthBuffer2.Get() : DepthBuffer.Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = (Index == 1 && DsvHeap2) ? DsvHeap2->GetCPUDescriptorHandleForHeapStart()
                                                                     : DsvHeap->GetCPUDescriptorHandleForHeapStart();
    if (!Dst)
        return;

    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = MsaaDepth.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &B);

    // Depth only: no colour target, and the destination's own size and full rectangle.
    Cmd->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
    D3D12_VIEWPORT Viewport;
    Viewport.TopLeftX = 0.0f;
    Viewport.TopLeftY = 0.0f;
    Viewport.Width    = (FLOAT)SceneWidth;
    Viewport.Height   = (FLOAT)SceneHeight;
    Viewport.MinDepth = 0.0f;
    Viewport.MaxDepth = 1.0f;
    Cmd->RSSetViewports(1, &Viewport);
    D3D12_RECT Scissor;
    Scissor.left   = 0;
    Scissor.top    = 0;
    Scissor.right  = SceneWidth;
    Scissor.bottom = SceneHeight;
    Cmd->RSSetScissorRects(1, &Scissor);
    Final.DrawMsaaDepth(Cmd, MsaaSrvHeap.Get(), MsaaSrvHeap->GetGPUDescriptorHandleForHeapStart());

    B.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    Cmd->ResourceBarrier(1, &B);
}

// The world is finished: bring the whole multisampled set down to one sample. From this line on the frame is exactly
// what it was before MSAA existed - the shadows, the motion vectors, the upscaler, the post filters and the HUD all
// work on the single-sample targets, and none of them knows anything happened.
void FBackend::ResolveMsaaWorld(ID3D12GraphicsCommandList* Cmd)
{
    if (!MsaaOn() || bMsaaResolved)
        return;

    ResolveMsaaDepth(Cmd, ActiveDepth);

    const DXGI_FORMAT DrawFmt = Colours.Draw;
    // The sRGB format is deliberate and not decoration: a resolve with it averages in LINEAR light and stores back
    // encoded, which is the only correct way to average a picture that was drawn through an sRGB view. With the plain
    // UNORM format the edge pixels would come out too dark.
    ResolveOne(Cmd, MsaaColor.Get(), SceneColor.Get(), DrawFmt);
    if (bShadows)
    {
        ResolveOne(Cmd, MsaaLight.Get(), LightTarget.Get(), RDX_LIGHT_TARGET_FORMAT);
        ResolveOne(Cmd, MsaaAdded.Get(), AddedTarget.Get(), DrawFmt);
    }

    bMsaaResolved = 1;    // and with it MsaaWorldLive() goes false: everything binds single-sample from here
}

// Ambient occlusion's heap (Backend.h): made on the first call, and its depth views written again every time the depth
// buffers are made again - which is why this is called from the very place their own views are written, and not from
// wherever the occlusion is switched on. Written whether the occlusion is on or not: four descriptors, and the menu can
// switch it on at any frame. The two targets' slots are EnsureAoTarget's, which makes the targets themselves.
void FBackend::WriteAoDepthViews()
{
    if (!DepthBuffer || !DepthBuffer2)
        return;
    if (!AoSrvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        HeapDesc.NumDescriptors = AO_SRVS;
        HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), AoSrvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(ambient occlusion SRV)")))
            return;
    }
    // The depth plane of the depth buffers, read as the shadow heap reads them.
    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc;
    appMemzero(&SrvDesc, sizeof(SrvDesc));
    SrvDesc.Format                  = bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT;
    SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SrvDesc.Texture2D.MipLevels     = 1;
    const UINT Inc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (INT t = 0; t < 2; t++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE S = AoSrvHeap->GetCPUDescriptorHandleForHeapStart();
        S.ptr += (SIZE_T)(t * AO_TABLE) * Inc;
        Device->CreateShaderResourceView((t == 0) ? DepthBuffer.Get() : DepthBuffer2.Get(), &SrvDesc, S);
        S.ptr += Inc;
        Device->CreateShaderResourceView((t == 0) ? DepthBuffer2.Get() : DepthBuffer.Get(), &SrvDesc, S);
        // The world's surface kinds (AO_STENCIL_SRV): the small depth leaves out what casts none of our shadows.
        D3D12_SHADER_RESOURCE_VIEW_DESC Kinds = SrvDesc;
        Kinds.Format               = bShadows ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R8G8_UINT;
        Kinds.Texture2D.PlaneSlice = bShadows ? 1 : 0;
        S = AoSrvHeap->GetCPUDescriptorHandleForHeapStart();
        S.ptr += (SIZE_T)(t * AO_TABLE + AO_STENCIL_SRV) * Inc;
        Device->CreateShaderResourceView(bShadows ? ((t == 0) ? DepthBuffer.Get() : DepthBuffer2.Get()) : nullptr,
                                         &Kinds, S);
    }
}

// The occlusion's two targets at the shadow factor's size, made again whenever that size moves (EnsureShadowTarget has
// run by the time this is called, the factor being drawn first): the raw occlusion (R8), and the world's depth copied
// down to that size (R32). And a third for the RT shadows: their factor before its smoothing (R8).
//
// The small depth is what makes the samples cheap. Each texel takes twelve reads scattered over up to a hundred and
// twenty-eight scene pixels, and read from the scene-sized depth buffer - eight bytes a texel, the stencil beside the
// depth - most of them missed every cache on the way: near walls, where the scatter is widest, the pass cost twice what
// it cost in the open. At the factor's size and four bytes a texel the same reads cover a sixteenth of the memory.
UBOOL FBackend::EnsureAoTarget()
{
    if (!AoSrvHeap || ShadowW <= 0 || ShadowH <= 0)
        return 0;
    if (AoRaw && AoDepth && RtFactorRaw && AoW == ShadowW && AoH == ShadowH)
        return 1;
    AoRaw.Release();
    AoDepth.Release();
    RtFactorRaw.Release();
    if (!AoRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 3;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), AoRtvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(ambient occlusion RTV)")))
            return 0;
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = ShadowW;
    Desc.Height           = ShadowH;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const DXGI_FORMAT Formats[3] = { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8_UNORM };
    TComRef<ID3D12Resource>* const Targets[3] = { &AoRaw, &AoDepth, &RtFactorRaw };
    const INT Slots[3] = { AO_RAW_SRV, AO_SMALL_DEPTH_SRV, AO_RT_FACTOR_SRV };
    static const TCHAR* const What[3] = { TEXT("CreateCommittedResource(ambient occlusion)"),
                                          TEXT("CreateCommittedResource(ambient occlusion, small depth)"),
                                          TEXT("CreateCommittedResource(RT shadow factor, before its smoothing)") };
    const UINT Inc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT RtvInc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (INT k = 0; k < 3; k++)
    {
        Desc.Format = Formats[k];
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   __uuidof(ID3D12Resource), Targets[k]->VoidSlot()), What[k]))
        {
            AoRaw.Release();
            AoDepth.Release();
            RtFactorRaw.Release();
            return 0;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE Rtv = AoRtvHeap->GetCPUDescriptorHandleForHeapStart();
        Rtv.ptr += (SIZE_T)k * RtvInc;
        Device->CreateRenderTargetView(Targets[k]->Get(), nullptr, Rtv);
        D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc;
        appMemzero(&SrvDesc, sizeof(SrvDesc));
        SrvDesc.Format                  = Formats[k];
        SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SrvDesc.Texture2D.MipLevels     = 1;
        for (INT t = 0; t < 2; t++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE S = AoSrvHeap->GetCPUDescriptorHandleForHeapStart();
            S.ptr += (SIZE_T)(t * AO_TABLE + Slots[k]) * Inc;
            Device->CreateShaderResourceView(Targets[k]->Get(), &SrvDesc, S);
        }
    }
    AoW = ShadowW;
    AoH = ShadowH;
    RdxLog(TEXT("ambient occlusion: its targets %ix%i"), AoW, AoH);
    return 1;
}

// The two AO passes' root constants (RdxMath::AoCb*). The view, the rectangles to leave out, the scale and the on-top rule
// are the shadow factor's own, worked out here exactly as FillShadowConstants works them out: the occlusion must leave
// out what the shadows leave out, pixel for pixel.
void FBackend::FillAoConstants(UINT* V) const
{
    appMemzero(V, RdxMath::AoCbCount * sizeof(UINT));
    PutFloat(V, RdxMath::AoCbTanX, bShadowViewSet ? ShadowTanX : ViewTanX);
    PutFloat(V, RdxMath::AoCbTanY, bShadowViewSet ? ShadowTanY : ViewTanY);
    V[RdxMath::AoCbScale]   = (UINT)(UpscaleMode ? ShadowFactorDivisor() : (SceneScale * ShadowFactorDivisor()));
    V[RdxMath::AoCbNoOnTop] = (WorldDepthIndex() != ActiveDepth) ? 0 : 1;
    for (INT k = 0; k < 4; k++)
        PutFloat(V, RdxMath::AoCbViewRect + k, bShadowViewSet ? ShadowViewRect[k] : ((k < 2) ? 0.0f : 1.0f));
    const INT Skips = (ShadowSkipCount < RdxMath::AoSkipRects) ? ShadowSkipCount : RdxMath::AoSkipRects;
    for (INT r = 0; r < Skips; r++)
        for (INT k = 0; k < 4; k++)
            PutFloat(V, RdxMath::AoCbSkipRect + 4 * r + k, ShadowSkip[r * 4 + k]);
    V[RdxMath::AoCbSkipCount] = (UINT)Skips;
    PutFloat(V, RdxMath::AoCbRadius,   (AoRadiusSet > 0.0f) ? AoRadiusSet : RdxMath::AoRadius);
    PutFloat(V, RdxMath::AoCbStrength, (AoStrengthSet > 0.0f) ? AoStrengthSet : RdxMath::AoStrength);
    PutFloat(V, RdxMath::AoCbCosBias,  RdxMath::AoCosBias);
}

// A pass's own target could not be made: written down with what the targets are made by now (TargetAsk), and said
// once - the pass's own line beside it says which of its pictures it was.
void FBackend::RefuseTarget(RdxMath::FTargetRefusal& Refusal, const TCHAR* Pass)
{
    Refusal.Note(TargetAsk());
    RdxLog(TEXT("%s: no target for it with the picture %ix%i and the scene %ix%i - left out, and asked again when a "
                "size or the colours change"), Pass, Width, Height, SceneWidth, SceneHeight);
}

// The occlusion, between the shadow factor and its apply (ApplyShadows): the depth buffers are readable there and the
// factor target is still a render target, which is all it needs. The raw pass goes into its own target; the blur then
// goes INTO the factor, by multiplication - so the compensation, which reads the factor, sees the occlusion exactly as
// it sees the shadows, and stays the last step of the light.
void FBackend::DrawAmbientOcclusion(ID3D12GraphicsCommandList* Cmd, INT World, UBOOL bFromHost)
{
    if (AoMode == RdxMath::Ao_Off || !AoSrvHeap || !ShadowRtvHeap)
        return;
    if (AoReady == 0)
    {
        AoReady = Final.InitAo(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("ambient occlusion: the pipelines %s"),
               (AoReady > 0) ? TEXT("up") : TEXT("FAILED - the occlusion is skipped, the shadows run on without it"));
    }
    if (AoReady < 0 || Refused(AoRefusal))
        return;
    if (!EnsureAoTarget())
    {
        RefuseTarget(AoRefusal, TEXT("ambient occlusion and the RT factor's smoothing"));
        return;
    }
    UINT Constants[RdxMath::AoCbCount];
    FillAoConstants(Constants);
    const UINT Inc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE Table = AoSrvHeap->GetGPUDescriptorHandleForHeapStart();
    if (World == 1)
        Table.ptr += AO_TABLE * Inc;
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)ShadowW, (FLOAT)ShadowH, 0.0f, 1.0f };
    D3D12_RECT     Sc = { 0, 0, ShadowW, ShadowH };
    const UINT RtvInc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // The world's depth, copied down to this size (EnsureAoTarget says why).
    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 2; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    B[0].Transition.pResource   = AoDepth.Get();
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Cmd->ResourceBarrier(1, &B[0]);
    D3D12_CPU_DESCRIPTOR_HANDLE DepthRtv = AoRtvHeap->GetCPUDescriptorHandleForHeapStart();
    DepthRtv.ptr += RtvInc;
    Cmd->OMSetRenderTargets(1, &DepthRtv, FALSE, nullptr);
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    Final.DrawAoDepth(Cmd, AoSrvHeap.Get(), Table, Constants);

    // The raw occlusion, reading the small depth - readable now - into its own target.
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[1].Transition.pResource   = AoRaw.Get();
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Cmd->ResourceBarrier(2, B);
    D3D12_CPU_DESCRIPTOR_HANDLE RawRtv = AoRtvHeap->GetCPUDescriptorHandleForHeapStart();
    Cmd->OMSetRenderTargets(1, &RawRtv, FALSE, nullptr);
    // The host's occlusion is there already, in what it shares: copied into the target the raw pass draws into.
    if (bFromHost)
        RayFeed.CopyOcclusion(Cmd, AoRaw.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    else
        Final.DrawAo(Cmd, AoSrvHeap.Get(), Table, Constants);

    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &B[1]);
    D3D12_CPU_DESCRIPTOR_HANDLE FactorRtv = ShadowRtvHeap->GetCPUDescriptorHandleForHeapStart();
    Cmd->OMSetRenderTargets(1, &FactorRtv, FALSE, nullptr);
    Final.DrawAoBlur(Cmd, AoSrvHeap.Get(), Table, Constants, bAoAlone);
}

// Smoke and gas as a volume, walked once a pixel over the whole of this frame's puffs and laid over the finished
// picture (FVolumetrics). Called at the WORLD'S END and after the shadows, which is not a detail: a shadow belongs to
// the wall behind the smoke, and a wall's shadow drawn onto the smoke in front of it is the oldest tell there is.
// After the multisampled resolve too, so what is laid over is one sample like the pass that lays it.
// A level's fall, all of its sheets together (RdxMath::FFallGather).
UBOOL FBackend::AddFall(const RdxMath::FFallSheetPiece* Pieces, INT Count)
{
    if (Fall.AddTogether(Pieces, Count, (INT)FVolumetrics::MAX_FALLS))
        return 1;
    static INT Told = 0;
    if (Told++ < 4)
        RdxLog(TEXT("waterfalls as volumes: no room for a fall of %i sheets (%i sheets in %i columns taken) - it is "
                    "drawn as sheets of falling water"), Count, Fall.Count, Fall.Columns);
    return 0;
}

// The smoke's lists hold what a frame asks for: there is no most puffs a room may have. Made larger by doubling, so
// a cloud that grows a puff a frame does not move its list a frame. False where the memory is refused.
template <class T> static UBOOL HoldMore(T*& List, INT& Held, INT Wanted, const TCHAR* Tag)
{
    if (Wanted <= Held)
        return 1;
    const INT Room = Max(Wanted, Max(Held * 2, 256));
    T* const  More = (T*)appRealloc(List, (DWORD)Room * sizeof(T), Tag);
    if (!More)
        return 0;
    List = More;
    Held = Room;
    return 1;
}

UBOOL FBackend::AddCloudPuff(const FCloudPuff& P)
{
    if (!HoldMore(CloudPuffs, CloudPuffHeld, CloudPuffCount + 1, TEXT("RdxCloudPuffs")))
    {
        CloudPuffLost++;
        return 0;
    }
    FCloudPuff& Q = CloudPuffs[CloudPuffCount++];
    Q          = P;
    Q.Dense    = RdxMath::CloudPuffDensity(Min(P.Radius, P.Tall), P.Fade, CloudTune.Depth);
    Q.Spare[0] = Q.Spare[1] = 0.0f;
    return 1;
}

// The flames' list holds what a frame asks for, as the smoke's does: there is no most flames a frame may have. A
// burning thing wears a hundred of the game's licks and a room may hold several; a list that turns the last of them
// away leaves those to the game's own flat cards, and which ones they are changes with every step the player takes.
UBOOL FBackend::HoldFlames(INT Wanted)
{
    if (Wanted <= Fire.Room)
        return 1;
    if (bFireListsRefused)
        return 0;
    const INT Held = FirePieceHeld;
    if (RdxForcedFailure(TEXT("fire lists")) ||
        !HoldMore(FirePieces, FirePieceHeld, Wanted, TEXT("RdxFirePieces")))
    {
        RefuseFireLists(TEXT("the pieces' list"), Wanted);
        return 0;
    }
    Fire.Hold(FirePieces, FirePieceHeld);
    RdxLog(TEXT("fires as volumes: the pieces' list now holds %i (it held %i)"), FirePieceHeld, Held);
    return 1;
}

// A list of the flames could not be made as long as a frame needed: from here on the game draws its own flames
// (CanFireVolumes), all of them and in every frame - never some of them, which would change from frame to frame.
void FBackend::RefuseFireLists(const TCHAR* Which, INT Wanted)
{
    if (bFireListsRefused)
        return;
    bFireListsRefused = 1;
    RdxLog(TEXT("fires as volumes: %s could not be made for %i - the game's own flames are drawn instead from here "
                "on"), Which, Wanted);
}

UBOOL FBackend::AddFirePiece(const RdxMath::FFlamePiece& P)
{
    return (HoldFlames(Fire.Count + 1) && Fire.Add(P)) ? 1 : 0;
}

UBOOL FBackend::AddFire(const RdxMath::FFlamePiece* Pieces, INT Count)
{
    return (Count > 0 && HoldFlames(Fire.Count + Count) && Fire.AddTogether(Pieces, Count)) ? 1 : 0;
}

UBOOL FBackend::AddMirrorFirePiece(const RdxMath::FFlamePiece& P)
{
    return AddMirrorFire(&P, 1);
}

UBOOL FBackend::AddMirrorFire(const RdxMath::FFlamePiece* Pieces, INT Count)
{
    if (Count <= 0 || !HoldMore(MirrorFlames, MirrorFlameHeld, MirrorFlameCount + Count, TEXT("RdxMirrorFlames")))
        return 0;
    for (INT i = 0; i < Count; i++)
        MirrorFlames[MirrorFlameCount++] = Pieces[i];
    return 1;
}

// The clock the fires' grain rises by: the walk's flames' and a mirror's alike.
static FLOAT FireClock()
{
    static const DOUBLE Start = appSeconds();
    return (FLOAT)(appSeconds() - Start);
}

UBOOL FBackend::AddMirrorCloudPuff(const FCloudPuff& P)
{
    if (!HoldMore(MirrorClouds, MirrorCloudHeld, MirrorCloudCount + 1, TEXT("RdxMirrorClouds")))
        return 0;
    FCloudPuff& Q = MirrorClouds[MirrorCloudCount++];
    Q          = P;
    Q.Dense    = RdxMath::CloudPuffDensity(Min(P.Radius, P.Tall), P.Fade, CloudTune.Depth);
    Q.Spare[0] = Q.Spare[1] = 0.0f;
    return 1;
}

UBOOL FBackend::AddMirrorFall(const FLOAT* Column)
{
    const INT Each = (INT)RdxMath::FallColumnFloats;
    if (!Column || !HoldMore(MirrorFalls, MirrorFallHeld, (MirrorFallCount + 1) * Each, TEXT("RdxMirrorFalls")))
        return 0;
    appMemcpy(MirrorFalls + MirrorFallCount * Each, Column, Each * sizeof(FLOAT));
    MirrorFallCount++;
    return 1;
}

void FBackend::DropMirrorGathered()
{
    MirrorFlameCount = 0;
    MirrorCloudCount = MirrorCloudMark;
    MirrorFallCount  = MirrorFallMark;
}

INT FBackend::NoteGlassPane(const FLOAT* Plane, FLOAT& Lamp)
{
    Lamp = 0.0f;
    if (!bInFrame || !bBeamOn || !Plane || Plane[3] <= 1.0f)
        return 0;
    // How strongly the lamp shines towards the pane's nearest point: the cone's own rims, as the frame before had
    // them.
    const FLOAT* const Cone = BeamBlockNow;
    const FLOAT Hot   = Clamp((Plane[2] - Cone[4]) * Cone[5], 0.0f, 1.0f);
    const FLOAT Spill = Clamp((Plane[2] - Cone[6]) * Cone[7], 0.0f, 1.0f);
    const FLOAT HotS  = Hot * Hot * (3.0f - 2.0f * Hot);
    Lamp = HotS + (1.0f - HotS) * Cone[11] * Spill * Spill * (3.0f - 2.0f * Spill);
    for (INT i = 0; i < PaneCount; i++)
    {
        const FLOAT* const P = PanePlanes + i * 4;
        if (P[0] * Plane[0] + P[1] * Plane[1] + P[2] * Plane[2] > 0.9995f && Abs(P[3] - Plane[3]) < 1.0f)
            return PANE_FIRST_NUMBER + i;
    }
    if (!HoldMore(PanePlanes, PaneHeld, (PaneCount + 1) * 4, TEXT("RdxPanePlanes")))
        return 0;
    appMemcpy(PanePlanes + PaneCount * 4, Plane, 4 * sizeof(FLOAT));
    return PANE_FIRST_NUMBER + PaneCount++;
}

void FBackend::SetOwnLamps(const FOwnLamp* Lamps, INT Count)
{
    OwnLampCount = 0;
    if (!bInFrame || !Lamps)
        return;
    for (INT i = 0; i < Count && OwnLampCount < (INT)RdxMath::OwnLampMost; i++)
        OwnLamps[OwnLampCount++] = Lamps[i];
}

void FBackend::SetOwnWorldView(const FLOAT* Coords)
{
    bOwnWorld = Coords ? 1 : 0;
    if (Coords)
        appMemcpy(OwnWorld, Coords, sizeof(OwnWorld));
}

void FBackend::NoteLampImage(const FLOAT* Plane, FLOAT Share)
{
    if (!bInFrame || !Plane || Plane[3] <= 1.0f || Plane[2] <= 0.05f || Share <= 0.0f)
        return;
    for (INT i = 0; i < LampImageCount; i++)
    {
        const FLOAT* const P = LampImages + i * 5;
        if (P[0] * Plane[0] + P[1] * Plane[1] + P[2] * Plane[2] > 0.9995f && Abs(P[3] - Plane[3]) < 1.0f)
            return;
    }
    if (LampImageCount >= (INT)MAX_LAMP_IMAGES)
        return;
    FLOAT* const P = LampImages + LampImageCount++ * 5;
    for (INT k = 0; k < 4; k++)
        P[k] = Plane[k];
    P[4] = Share;
}

void FBackend::PrepareOwnLamps()
{
    OwnRowsLamps = 0;
    if (OwnLampCount > 0)
    {
        OwnStatFrames++;
        OwnStatMost = Max(OwnStatMost, OwnLampCount);
    }
    if (OwnLampCount <= 0 || OwnLampsRefused() || !ShadowSrvHeap || !Shadow.EnsureSpots(Device.Get()))
    {
        OwnStatUnmapped += (OwnLampCount > 0) ? 1 : 0;
        // The table keeps what it was last given: a frame with no lamp gives it a head of none.
        if (bOwnTableLit)
        {
            appMemzero(OwnRowsNow, sizeof(OwnRowsNow));
            UploadOwnTable();
            bOwnTableLit = 0;
        }
        return;
    }
    FShadowMaps::FSpot Spots[RdxMath::OwnLampMost];
    for (INT i = 0; i < OwnLampCount; i++)
    {
        const FOwnLamp&     L = OwnLamps[i];
        FShadowMaps::FSpot& S = Spots[i];
        for (INT k = 0; k < 3; k++)
        {
            S.At[k]  = L.At[k];
            S.Aim[k] = L.Aim[k];
        }
        const RdxMath::FVec3 At  = { L.At[0], L.At[1], L.At[2] };
        const RdxMath::FVec3 Aim = { L.Aim[0], L.Aim[1], L.Aim[2] };
        RdxMath::SpotMatrix(At, Aim, RdxMath::OwnLampMapTan, RdxMath::OwnLampNear, S.Matrix);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE MaskSrv = ShadowSrvHeap->GetGPUDescriptorHandleForHeapStart();
    MaskSrv.ptr += SHADOW_MASK_SRV * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Shadow.RenderSpots(CommandList.Get(), ShadowSrvHeap.Get(), MaskSrv, Spots, OwnLampCount);
    // The world's targets, viewport and scissor again, as the frame's start bound them.
    BindSceneTargets(CommandList.Get(), DsvHeap->GetCPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const D3D12_RECT     Sc = { 0, 0, SceneWidth, SceneHeight };
    CommandList->RSSetViewports(1, &Vp);
    CommandList->RSSetScissorRects(1, &Sc);
    // The maps' view, written again whenever they were made again.
    if (OwnMapsMade != Shadow.SpotsMade() && Shadow.GetSpotMaps() && Textures.GetHeap())
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
        Shadow.DescribeSpotView(Srv);
        Device->CreateShaderResourceView(Shadow.GetSpotMaps(), &Srv,
                                         Textures.GetCpuHandle(Textures.GetReservedIndex(OwnLampMapsView)));
        OwnMapsMade = Shadow.SpotsMade();
    }
    // The rows (SolidPass.hlsl, OwnRow): the head, then each lamp's seven.
    appMemzero(OwnRowsNow, sizeof(OwnRowsNow));
    RdxMath::OwnLampRowsHead(OwnRowsNow, OwnLampCount);
    for (INT i = 0; i < OwnLampCount; i++)
    {
        const RdxMath::FVec3 At  = { OwnLamps[i].At[0], OwnLamps[i].At[1], OwnLamps[i].At[2] };
        const RdxMath::FVec3 Aim = { OwnLamps[i].Aim[0], OwnLamps[i].Aim[1], OwnLamps[i].Aim[2] };
        RdxMath::OwnLampRowsLamp(OwnRowsNow, i, At, Aim, OwnLamps[i].Colour, OwnLamps[i].bEye != 0, Spots[i].Matrix);
        // Its place's spare: the wet floor below it, for its light off the water (SolidPass.hlsl's OwnBounce).
        OwnRowsNow[4 * (RDX_OWN_HEAD_ROWS + i * RDX_OWN_LAMP_ROWS) + 3] = OwnLamps[i].WetFloor;
    }
    OwnRowsLamps = OwnLampCount;
    UploadOwnTable();
    bOwnTableLit = 1;
}

void FBackend::OwnBeamsInto(FVolumetrics::FWalk& W, D3D12_SHADER_RESOURCE_VIEW_DESC& View)
{
    static_assert((INT)FVolumetrics::OWN_FLOATS == (INT)FSolidPass::OWN_FLOATS, "the own lamps' rows differ");
    appMemzero(&View, sizeof(View));
    if (!OwnBeamsNow() || !Shadow.GetSpotMaps())
        return;
    Shadow.DescribeSpotView(View);
    W.Own         = OwnRowsNow;
    W.OwnCount    = OwnRowsLamps;
    W.OwnMaps     = Shadow.GetSpotMaps();
    W.OwnMapsView = &View;
    W.OwnMapsMade = Shadow.SpotsMade();
    W.OwnDust     = RdxMath::OwnBeamDust;
    W.OwnForward  = RdxMath::OwnBeamForward;
}

void FBackend::UploadOwnTable()
{
    if (bOwnTableRefused || !CommandList || !Textures.GetHeap())
        return;
    const UINT Width = (UINT)(FSolidPass::OWN_FLOATS / 4);
    const UINT Pitch = (Width * 16u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    static_assert((FSolidPass::OWN_FLOATS / 4) * 16 <= OWN_TABLE_SLOT &&
                  OWN_TABLE_SLOT % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0, "a frame slot's rows fit their part");
    if (!OwnTable)
    {
        D3D12_HEAP_PROPERTIES Heap;
        appMemzero(&Heap, sizeof(Heap));
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC Desc;
        appMemzero(&Desc, sizeof(Desc));
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = Width;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
        Desc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES Up;
        appMemzero(&Up, sizeof(Up));
        Up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC UpDesc;
        appMemzero(&UpDesc, sizeof(UpDesc));
        UpDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        UpDesc.Width            = (UINT64)OWN_TABLE_SLOT * BACKEND_FRAME_COUNT;
        UpDesc.Height           = 1;
        UpDesc.DepthOrArraySize = 1;
        UpDesc.MipLevels        = 1;
        UpDesc.SampleDesc.Count = 1;
        UpDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE NoRead = { 0, 0 };
        if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, __uuidof(ID3D12Resource), OwnTable.VoidSlot())) ||
            FAILED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &UpDesc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   __uuidof(ID3D12Resource), OwnTableUpload.VoidSlot())) ||
            FAILED(OwnTableUpload->Map(0, &NoRead, (void**)&OwnTableMapped)) || !OwnTableMapped)
        {
            OwnTable.Release();
            OwnTableUpload.Release();
            OwnTableMapped   = nullptr;
            bOwnTableRefused = 1;
            RdxLog(TEXT("own lamps: their table for the mirror rays could not be made - the rays' images go without "
                        "them"));
            return;
        }
        OwnTableState = D3D12_RESOURCE_STATE_COPY_DEST;
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
        appMemzero(&Srv, sizeof(Srv));
        Srv.Format                  = DXGI_FORMAT_R32G32B32A32_FLOAT;
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels     = 1;
        Device->CreateShaderResourceView(OwnTable.Get(), &Srv,
                                         Textures.GetCpuHandle(Textures.GetReservedIndex(OwnLampTableView)));
    }
    const UINT64 At = (UINT64)OWN_TABLE_SLOT * FrameIndex;
    appMemcpy(OwnTableMapped + At, OwnRowsNow, sizeof(OwnRowsNow));
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = OwnTable.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (OwnTableState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        B.Transition.StateBefore = OwnTableState;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        CommandList->ResourceBarrier(1, &B);
    }
    D3D12_TEXTURE_COPY_LOCATION Dst;
    appMemzero(&Dst, sizeof(Dst));
    Dst.pResource        = OwnTable.Get();
    Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION Src;
    appMemzero(&Src, sizeof(Src));
    Src.pResource                          = OwnTableUpload.Get();
    Src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    Src.PlacedFootprint.Offset             = At;
    Src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R32G32B32A32_FLOAT;
    Src.PlacedFootprint.Footprint.Width    = Width;
    Src.PlacedFootprint.Footprint.Height   = 1;
    Src.PlacedFootprint.Footprint.Depth    = 1;
    Src.PlacedFootprint.Footprint.RowPitch = Pitch;
    CommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    CommandList->ResourceBarrier(1, &B);
    OwnTableState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void FBackend::MakeLampFlares(const FLOAT* Cone)
{
    BloomLampCount = 0;
    LampGlareCount = 0;
    if (!bOwnWorld || CloudFocal <= 1.0f || Width <= 0 || Height <= 0)
        return;
    const FLOAT Sx = (FLOAT)Max(SceneWidth, 1) / (FLOAT)Max(Width, 1);
    const FLOAT Sy = (FLOAT)Max(SceneHeight, 1) / (FLOAT)Max(Height, 1);
    // A lamp for the lens's ghosts, at its place in the engine's pixels, as wide as its glare reaches.
    auto Ghost = [&](FLOAT X, FLOAT Y, FLOAT Halo, const FLOAT* Colour, FLOAT Strength)
    {
        if (LookFlare <= 0.0f || BloomLampCount >= (INT)MAX_BLOOM_LAMPS || Strength <= 0.01f)
            return;
        const FLOAT  R = Max(3.0f * Halo, 6.0f);
        FBloomLamp&  K = BloomLamps[BloomLampCount++];
        K.X0 = (X - R) * 2.0f / (FLOAT)Width - 1.0f;
        K.Y0 = 1.0f - (Y - R) * 2.0f / (FLOAT)Height;
        K.X1 = (X + R) * 2.0f / (FLOAT)Width - 1.0f;
        K.Y1 = 1.0f - (Y + R) * 2.0f / (FLOAT)Height;
        K.R     = Colour[0];
        K.G     = Colour[1];
        K.B     = Colour[2];
        K.Shine = Min(Strength, 1.0f);
    };
    const FLOAT* const O = OwnWorld;
    const FLOAT* const X = OwnWorld + 3;
    const FLOAT* const Y = OwnWorld + 6;
    const FLOAT* const Z = OwnWorld + 9;
    // The weapon lamps that shine at the eye: by how near the eye lies to the axis, their disc and glare as wide as
    // a mirror's lamp's (FBeamBounce), where the eye sees them.
    for (INT i = 0; i < OwnLampCount; i++)
    {
        const FOwnLamp& L = OwnLamps[i];
        if (L.bEye || !L.bSeen)
            continue;
        const FLOAT D[3] = { L.At[0] - O[0], L.At[1] - O[1], L.At[2] - O[2] };
        const FLOAT Vz   = D[0] * Z[0] + D[1] * Z[1] + D[2] * Z[2];
        const FLOAT Len  = appSqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
        if (Vz < 16.0f || Len < 1.0f)
            continue;
        const FLOAT Toward = RdxMath::OwnLampToward(-(L.Aim[0] * D[0] + L.Aim[1] * D[1] + L.Aim[2] * D[2]) / Len);
        if (Toward <= 0.01f)
            continue;
        const FLOAT Vx   = D[0] * X[0] + D[1] * X[1] + D[2] * X[2];
        const FLOAT Vy   = D[0] * Y[0] + D[1] * Y[1] + D[2] * Y[2];
        const FLOAT Ex   = CloudScreen[0] + CloudFocal * Vx / Vz;
        const FLOAT Ey   = CloudScreen[1] + CloudFocal * Vy / Vz;
        const FLOAT Most = Max(Max(L.Colour[0], L.Colour[1]), Max(L.Colour[2], 1e-4f));
        const FLOAT Hue[3] = { L.Colour[0] / Most, L.Colour[1] / Most, L.Colour[2] / Most };
        FBeamBounce::FLamp& G = LampGlares[LampGlareCount++];
        G.Centre[0] = Ex * Sx;
        G.Centre[1] = Ey * Sy;
        G.Depth     = Vz;
        G.Core      = Max(CloudFocal * Sx * FBeamBounce::LampRadius / Vz, 1.5f);
        G.Halo      = CloudFocal * Sx * FBeamBounce::GlareRadius / Vz;
        G.Strength  = Toward;
        for (INT k = 0; k < 3; k++)
            G.Colour[k] = Hue[k];
        Ghost(Ex, Ey, CloudFocal * FBeamBounce::GlareRadius / Vz, Hue, Toward);
    }
    // The flashlight's own image in each mirror and pane it lit this frame: at the place of the eye's image, twice as
    // far as the plane along its normal, by how strongly the cone shines that way and what the surface gives back.
    if (Cone && Cone[3] > 0.0f)
    {
        const FLOAT Most   = Max(Max(Cone[0], Cone[1]), Max(Cone[2], 1e-4f));
        const FLOAT Hue[3] = { Cone[0] / Most, Cone[1] / Most, Cone[2] / Most };
        for (INT i = 0; i < LampImageCount; i++)
        {
            const FLOAT* const N = LampImages + i * 5;
            const FLOAT Hot   = Clamp((N[2] - Cone[4]) * Cone[5], 0.0f, 1.0f);
            const FLOAT Spill = Clamp((N[2] - Cone[6]) * Cone[7], 0.0f, 1.0f);
            const FLOAT HotS  = Hot * Hot * (3.0f - 2.0f * Hot);
            const FLOAT Lamp  = HotS + (1.0f - HotS) * Cone[11] * Spill * Spill * (3.0f - 2.0f * Spill);
            const FLOAT Depth = 2.0f * N[3] * N[2];
            if (Depth < 1.0f)
                continue;
            Ghost(CloudScreen[0] + CloudFocal * N[0] / N[2], CloudScreen[1] + CloudFocal * N[1] / N[2],
                  CloudFocal * FBeamBounce::GlareRadius / Depth, Hue, Lamp * N[4]);
        }
    }
}

void FBackend::DrawLampGlares(ID3D12GraphicsCommandList* Cmd, INT World)
{
    if (!Cmd || LampGlareCount <= 0)
        return;
    if (BounceReady == 0)
        BounceReady = Bounce.Init(Device.Get()) ? 1 : -1;
    ID3D12Resource* const Depth = (World == 1 && DepthBuffer2) ? DepthBuffer2.Get() : DepthBuffer.Get();
    if (BounceReady < 0 || !Depth)
        return;
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Depth;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &B);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
    Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;       // the scene target's view (BindSceneTargets)
    const FLOAT Sx = (FLOAT)Max(SceneWidth, 1) / (FLOAT)Max(Width, 1);
    const FLOAT Sy = (FLOAT)Max(SceneHeight, 1) / (FLOAT)Max(Height, 1);
    FBeamBounce::FView V;
    V.Screen[0] = CloudScreen[0] * Sx;
    V.Screen[1] = CloudScreen[1] * Sy;
    V.Focal     = CloudFocal * Sx;
    V.W         = SceneWidth;
    V.H         = SceneHeight;
    for (INT i = 0; i < LampGlareCount; i++)
        Bounce.LampGlare(Cmd, Device.Get(), Colours.Draw, Rtv, Depth,
                         bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT, LampGlares[i], V);
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    Cmd->ResourceBarrier(1, &B);
    // As the shadows' apply leaves it: the scene target alone, the whole of it.
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const D3D12_RECT     Sc = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
}

void FBackend::MirrorViewDone(const FLOAT* View, const FLOAT* Plane, const FLOAT* Rect, const FLOAT* ViewPlane,
                              const FLOAT* Tris, INT TriVerts, const FLOAT* Coords)
{
    if (!bInFrame || !View || !Plane ||
        !HoldMore(MirrorEnds, MirrorEndHeld, MirrorEndCount + 1, TEXT("RdxMirrorEnds")))
    {
        DropMirrorGathered();
        return;
    }
    FMirrorEnd& E = MirrorEnds[MirrorEndCount];
    appMemcpy(E.View, View, sizeof(E.View));
    appMemcpy(E.Plane, Plane, sizeof(E.Plane));
    for (INT k = 0; k < 4; k++)
        E.Rect[k] = Rect ? Rect[k] : 0.0f;
    // The mirror as the world's view sees it, for the flashlight's light off it.
    E.TriFirst = MirrorTriCount;
    E.TriCount = 0;
    for (INT k = 0; k < 4; k++)
        E.ViewPlane[k] = ViewPlane ? ViewPlane[k] : 0.0f;
    E.bOwnCoords = Coords ? 1 : 0;
    for (INT k = 0; k < 12; k++)
        E.OwnCoords[k] = Coords ? Coords[k] : 0.0f;
    if (ViewPlane && Tris && TriVerts >= 3 &&
        HoldMore(MirrorTris, MirrorTriHeld, (MirrorTriCount + TriVerts) * 3, TEXT("RdxMirrorTris")))
    {
        appMemcpy(MirrorTris + MirrorTriCount * 3, Tris, (SIZE_T)TriVerts * 3 * sizeof(FLOAT));
        E.TriCount      = TriVerts;
        MirrorTriCount += TriVerts;
    }
    // The view's puffs and columns: what was gathered since the view before it ended.
    E.PuffFirst     = MirrorCloudMark;
    E.PuffCount     = MirrorCloudCount - MirrorCloudMark;
    MirrorCloudMark = MirrorCloudCount;
    E.FallFirst     = MirrorFallMark;
    E.FallCount     = MirrorFallCount - MirrorFallMark;
    MirrorFallMark  = MirrorFallCount;
    // Its flame bodies, made of its pieces as the walk makes the world's view's of that view's (DrawVolumetrics): a
    // body a piece at the most, by how much of the view a pixel of the walk's target covers.
    E.FireFirst = MirrorBodyCount;
    E.FireCount = 0;
    const INT Pieces = MirrorFlameCount;
    MirrorFlameCount = 0;
    if (Pieces > 0 && View[12] > 1.0f &&
        HoldMore(MirrorBodies, MirrorBodyHeld, (MirrorBodyCount + Pieces) * (INT)RdxMath::FlameBodyFloats,
                 TEXT("RdxMirrorBodies")) &&
        HoldMore(MirrorGroups, MirrorGroupHeld, Pieces, TEXT("RdxMirrorGroups")))
    {
        const FLOAT EngPerPixel = (FLOAT)Max(Width, 1) / (FLOAT)Max(FVolumetrics::TargetSide(SceneWidth), 1);
        E.FireCount = RdxMath::FlameBodiesMake(MirrorFlames, Pieces,
                                               MirrorBodies + MirrorBodyCount * RdxMath::FlameBodyFloats, Pieces,
                                               EngPerPixel / View[12], MirrorGroups);
        MirrorBodyCount += E.FireCount;
    }
    Solid.AddMirrorEnd(MirrorEndCount++);
}

// The frame's flame bodies by tile of the walk's target, as the puffs are listed (BuildCloudTiles): for every tile
// where its list starts and how many it holds, then the lists, each a body's place among the bodies
// (FVolumetrics::FWalk::FireTiles). A body is listed wherever the ball that holds all of its light can show
// (RdxMath::FlameBodyTiles) - with the sparks on, a standing fire's as far as they fly. The walk asks a pixel only
// its tile's bodies: without the lists every pixel of the target asks every flame of the frame, and a room of
// burning things costs by all its flames at every pixel. How many numbers there are; 0 where there is no memory
// for them.
// Bodies: the view's own - the world's (FireBodies), or a mirror's (WalkMirrorView).
INT FBackend::BuildFireTiles(const FLOAT* Bodies, INT Count, const FVolumetrics::FWalk& W)
{
    const INT TilesX = RdxMath::CloudTilesAlong(Volume.TargetW());
    const INT TilesY = RdxMath::CloudTilesAlong(Volume.TargetH());
    if (TilesX * TilesY <= 0 || !Bodies || Count <= 0 ||
        !HoldMore(FireRects, FireRectHeld, Count * 4, TEXT("RdxFireRects")))
        return 0;
    for (INT i = 0; i < Count; i++)
    {
        INT* const R = FireRects + i * 4;
        if (!RdxMath::FlameBodyTiles(Bodies + i * FVolumetrics::FIRE_FLOATS, bFireSparks != 0, W.EyeWorld, W.AxisX,
                                     W.AxisY, W.AxisZ, W.Focal, W.Screen, W.EngPerPixel, TilesX, TilesY, R[0], R[1],
                                     R[2], R[3]))
        {
            R[0] = R[1] = 0;
            R[2] = R[3] = -1;       // in no tile
        }
    }
    const INT Numbers = RdxMath::CloudTileListsSize(FireRects, Count, TilesX, TilesY);
    if (!HoldMore(FireTileData, FireTileHeld, Numbers, TEXT("RdxFireTiles")))
        return 0;
    FireTileMost = Max(FireTileMost, RdxMath::CloudTileListsFill(FireRects, Count, TilesX, TilesY, FireTileData));
    return Numbers;
}

// The shafts' lamps by tile of the walk's target, as the puffs are listed (BuildCloudTiles): for every tile where its
// list starts and how many it holds, then the lists, each a lamp's place among the lamps
// (FVolumetrics::FWalk::LampTiles). A lamp - two rows of four numbers, its place in view space and its reach in the
// first - is listed wherever the ball of its reach can show: outside it the lamp lights no air. How many numbers
// there are; 0 where there is no memory for them.
INT FBackend::BuildShaftTiles(const FLOAT* Lamps, INT Count, const FVolumetrics::FWalk& W)
{
    const INT TilesX = RdxMath::CloudTilesAlong(Volume.TargetW());
    const INT TilesY = RdxMath::CloudTilesAlong(Volume.TargetH());
    if (TilesX * TilesY <= 0 || Count <= 0 || RdxForcedFailure(TEXT("shaft lists")) ||
        !HoldMore(ShaftRects, ShaftRectHeld, Count * 4, TEXT("RdxShaftRects")))
        return 0;
    for (INT i = 0; i < Count; i++)
    {
        const FLOAT* const L = Lamps + i * FVolumetrics::LAMP_FLOATS;
        INT* const         R = ShaftRects + i * 4;
        if (!RdxMath::CloudPuffTiles(L, L[3], W.Focal, W.Screen, W.EngPerPixel, TilesX, TilesY, R[0], R[1], R[2], R[3]))
        {
            R[0] = R[1] = 0;
            R[2] = R[3] = -1;       // in no tile
        }
    }
    const INT Numbers = RdxMath::CloudTileListsSize(ShaftRects, Count, TilesX, TilesY);
    if (!HoldMore(ShaftTileData, ShaftTileHeld, Numbers, TEXT("RdxShaftTiles")))
        return 0;
    ShaftTileMost = Max(ShaftTileMost, RdxMath::CloudTileListsFill(ShaftRects, Count, TilesX, TilesY, ShaftTileData));
    return Numbers;
}

// The frame's puffs as the walk takes them - none smaller on its target than a few pixels - and their lists by tile
// of that target: for every tile where its list starts and how many it holds, then the lists, each a puff's place
// among the puffs (FVolumetrics::Record). A puff is listed wherever its ball can show, taken as wide as its own reach
// and the self-shadow's step beside the ray - the walk asks its tile's list for both. How many numbers there are; 0
// where there is no memory for them.
// Puffs: the view's own - the world's (CloudPuffs), or a mirror's (WalkMirrorView) - by that view's projection and
// its screen's centre.
INT FBackend::BuildCloudTiles(FCloudPuff* Puffs, INT Count, FLOAT Focal, const FLOAT* Screen, INT TilesX, INT TilesY,
                              const FLOAT* EngPerPixel)
{
    const INT Tiles = TilesX * TilesY;
    if (Tiles <= 0 || !Puffs || Count <= 0 ||
        !HoldMore(CloudPuffRects, CloudRectHeld, Count * 4, TEXT("RdxCloudRects")))
        return 0;
    // The level's units a pixel of the target covers at a depth of one: a puff smaller there than a few pixels is
    // widened to that, and thinned by as much (RdxMath::CloudPuffMinPixels).
    const FLOAT UnitsAPixel = EngPerPixel[0] / Max(Focal, 1.0f);
    for (INT i = 0; i < Count; i++)
    {
        FCloudPuff& P = Puffs[i];
        INT* const  R = CloudPuffRects + i * 4;
        P.Dense *= RdxMath::VolumeWiden(P.Radius, P.Tall,
                                        0.5f * RdxMath::CloudPuffMinPixels * UnitsAPixel * Max(P.Centre[2], 1.0f));
        if (!RdxMath::CloudPuffTiles(P.Centre, Max(P.Radius, P.Tall) + CloudTune.SelfShade, Focal, Screen,
                                     EngPerPixel, TilesX, TilesY, R[0], R[1], R[2], R[3]))
        {
            R[0] = R[1] = 0;
            R[2] = R[3] = -1;       // in no tile
        }
    }
    const INT Numbers = RdxMath::CloudTileListsSize(CloudPuffRects, Count, TilesX, TilesY);
    if (!HoldMore(CloudTileData, CloudTileHeld, Numbers, TEXT("RdxCloudTiles")))
        return 0;
    CloudTileMost = Max(CloudTileMost,
                        RdxMath::CloudTileListsFill(CloudPuffRects, Count, TilesX, TilesY, CloudTileData));
    return Numbers;
}

// What the engine draws after the world - the weapon in hand - stands in the smoke too. Where the drawing moved to the
// other depth buffer at the engine's ClearZ (ClearDepthMidFrame), that buffer holds it and nothing else, and its depth
// goes into the walk's copy of the world's depth wherever it is the nearer: the walk then ends at the weapon, so the
// smoke between the eye and the weapon lies over it, and the smoke behind it does not.
void FBackend::PutAfterWorldDepth(ID3D12GraphicsCommandList* Cmd)
{
    if (bWeaponDepthPut || !WaterDepth || !WaterDepthDsvHeap || !DepthSrvHeap || !DepthBuffer2 ||
        WorldDepthIndex() == ActiveDepth)
        return;
    if (DepthNearerReady == 0)
    {
        DepthNearerReady = Final.InitDepthNearer(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("volumetrics: the walk %s"), (DepthNearerReady > 0)
               ? TEXT("ends at the weapon in hand - only the smoke in front of it lies over it")
               : TEXT("goes on behind the weapon in hand: its pass could NOT be made"));
    }
    if (DepthNearerReady < 0 || !Final.HasDepthNearer(bShadows))
        return;
    ID3D12Resource* const After = (ActiveDepth == 1) ? DepthBuffer2.Get() : DepthBuffer.Get();
    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 2; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    B[0].Transition.pResource   = WaterDepth.Get();
    B[0].Transition.StateBefore = WaterDepthState;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B[1].Transition.pResource   = After;
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(2, B);

    const D3D12_CPU_DESCRIPTOR_HANDLE CopyDsv = WaterDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
    Cmd->OMSetRenderTargets(0, nullptr, FALSE, &CopyDsv);
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    D3D12_RECT     Sc = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    D3D12_GPU_DESCRIPTOR_HANDLE Srv = DepthSrvHeap->GetGPUDescriptorHandleForHeapStart();
    Srv.ptr += (UINT64)ActiveDepth * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Final.DrawDepthNearer(Cmd, DepthSrvHeap.Get(), Srv, bShadows);
    bWeaponDepthPut = 1;        // once a frame: the fog and the walk both read the copy after it

    for (INT i = 0; i < 2; i++)
    {
        const D3D12_RESOURCE_STATES Was = B[i].Transition.StateBefore;
        B[i].Transition.StateBefore = B[i].Transition.StateAfter;
        B[i].Transition.StateAfter  = Was;
    }
    Cmd->ResourceBarrier(2, B);
}

UBOOL FBackend::SetFogNodes(const FFogNode* Nodes, INT Count)
{
    if (!Device)
        return 0;
    if (FogRoomsReady == 0)
    {
        FogRoomsReady = FogRoom.Init(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("fog rooms: the pass %s"), (FogRoomsReady > 0)
               ? TEXT("up - the level's fog stays in its rooms")
               : TEXT("could NOT be made - the engine's own fog is drawn instead"));
    }
    if (FogRoomsReady < 0)
        return 0;
    const UBOOL bOk = FogRoom.SetNodes(Device.Get(), Nodes, Count);
    RdxLog(TEXT("fog rooms: the level's %i BSP nodes %s"), Count,
           bOk ? TEXT("handed to the pass") : TEXT("could NOT be handed to it - the engine's own fog is drawn"));
    return bOk;
}

void FBackend::RefuseFogRooms(const TCHAR* Which, INT Wanted)
{
    if (FogRoomsReady < 0)
        return;
    FogRoomsReady = -1;
    FogLightCount = 0;
    RdxLog(TEXT("fog rooms: %s could not be made for %i - the engine's own fog is drawn instead from here on"), Which,
           Wanted);
}

void FBackend::SetFogLights(const FFogLight* Lights, INT Count, UINT CameraZone)
{
    FogLightCount = 0;
    FogCameraZone = CameraZone;
    if (!Lights || Count <= 0 || FogRoomsReady < 0)
        return;
    if (!HoldMore(FogLights, FogLightHeld, Count, TEXT("RdxFogLights")))
    {
        RefuseFogRooms(TEXT("the frame's list of lights"), Count);
        return;
    }
    FogLightCount = Count;
    appMemcpy(FogLights, Lights, sizeof(FFogLight) * FogLightCount);
}

void FBackend::DrawFogRooms(ID3D12GraphicsCommandList* Cmd)
{
    if (!Cmd || !Device || bOutputLive || FogLightCount <= 0 || CloudFocal <= 1.0f || !FogRoomsCanRun())
        return;
    // The copy of the world's depth the frame made (CopyForWater, asked for by WantsCloudDepth): without it there is
    // no knowing where a pixel is, and this frame has no fog - said once.
    if (!bDepthCopyDone || !WaterDepth)
    {
        static INT Said = 0;
        if (Said++ < 3)
            RdxLog(TEXT("fog rooms: no copy of the world's depth this frame - no fog in it"));
        return;
    }
    GpuTime(Cmd, 29);
    // The weapon in hand into the copy, where nearer: its own fog, not the wall's behind it.
    PutAfterWorldDepth(Cmd);
    D3D12_RESOURCE_BARRIER D;
    appMemzero(&D, sizeof(D));
    D.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    D.Transition.pResource   = WaterDepth.Get();
    D.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    const D3D12_RESOURCE_STATES DepthWas = WaterDepthState;
    const UBOOL bMove = (DepthWas & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) ? 0 : 1;
    if (bMove)
    {
        D.Transition.StateBefore = DepthWas;
        D.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &D);
    }

    // Over the scene target, within the world's view only - in a conversation not over the black bars.
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
    Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;      // the scene target's view (BindSceneTargets)
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    D3D12_RECT Sc = { 0, 0, SceneWidth, SceneHeight };
    if (CloudRect[2] > CloudRect[0] && CloudRect[3] > CloudRect[1] && Width > 0 && Height > 0)
    {
        const FLOAT Kx = (FLOAT)SceneWidth / (FLOAT)Width, Ky = (FLOAT)SceneHeight / (FLOAT)Height;
        Sc.left   = Clamp(appFloor(CloudRect[0] * Kx), 0, SceneWidth);
        Sc.top    = Clamp(appFloor(CloudRect[1] * Ky), 0, SceneHeight);
        Sc.right  = Clamp(appCeil(CloudRect[2] * Kx), (INT)Sc.left, SceneWidth);
        Sc.bottom = Clamp(appCeil(CloudRect[3] * Ky), (INT)Sc.top, SceneHeight);
    }
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);

    FFogRooms::FView V;
    appMemzero(&V, sizeof(V));
    V.Screen[0]      = CloudScreen[0];
    V.Screen[1]      = CloudScreen[1];
    V.Focal          = CloudFocal;
    V.EngPerPixel[0] = (FLOAT)Max(Width, 1) / (FLOAT)Max(SceneWidth, 1);
    V.EngPerPixel[1] = (FLOAT)Max(Height, 1) / (FLOAT)Max(SceneHeight, 1);
    for (INT i = 0; i < 3; i++)
    {
        V.Eye[i]   = CamEye[i];
        V.AxisX[i] = CamAxes[i];
        V.AxisY[i] = CamAxes[3 + i];
        V.AxisZ[i] = CamAxes[6 + i];
    }
    V.CameraZone = FogCameraZone;
    // The see-through water's own depth, where this frame's water wrote it (a temporal upscaler asks for it).
    ID3D12Resource* const Water = (bWaterSurfaceLive && WaterSurface) ? WaterSurface.Get() : nullptr;
    if (Water)
        WaterSurfaceTo(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const UBOOL bDrawn = FogRoom.Record(Cmd, Device.Get(), Colours.Draw, WaterDepth.Get(),
                                        bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT, Water,
                                        V, FogLights, FogLightCount, FrameIndex);
    if (!bFogRoomsSaid)
    {
        bFogRoomsSaid = 1;
        RdxLog(TEXT("fog rooms: the first frame's fog %s - %i lights, the eye in zone %u"),
               bDrawn ? TEXT("laid over the world") : TEXT("could NOT be laid"), FogLightCount, FogCameraZone);
    }
    // The most lights a frame's fog has come from, said when it is passed: how many a level's rooms really hold.
    if (bDrawn && FogLightCount > FogLightMost)
    {
        FogLightMost = FogLightCount;
        RdxLog(TEXT("fog rooms: the fog of %i lights in one frame - the most so far"), FogLightMost);
    }
    // This frame's fog is not there; from the next one the engine draws its own, and goes on drawing it.
    if (!bDrawn && FogRoom.LightsRefused())
        RefuseFogRooms(TEXT("the pass's list of lights"), FogLightCount);
    if (bMove)
    {
        D.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        D.Transition.StateAfter  = DepthWas;
        Cmd->ResourceBarrier(1, &D);
    }
    // The pass bound a view heap of its own; everything after this reads through the texture heap.
    ID3D12DescriptorHeap* Back[1] = { Textures.GetHeap() };
    if (Back[0])
        Cmd->SetDescriptorHeaps(1, Back);
    GpuTime(Cmd, 30);
}

UBOOL FBackend::FieldVolumeRuns()
{
    if (!Device || !bDepthReadable)
        return 0;
    if (FieldReady == 0)
    {
        FieldReady = Field.Init(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("field volume: the pass %s"), (FieldReady > 0)
               ? TEXT("up - the force field is a volume of the level's space")
               : TEXT("could NOT be made - the field is drawn as arcs on its surface instead"));
    }
    return FieldReady > 0;
}

void FBackend::AddFieldPolygon(INT Key, const FLOAT* Points, INT Count, INT Kind, const FLOAT Look[4])
{
    if (!Points || !Look || Count < 3 || Count > RdxMath::FieldCorners)
        return;
    for (INT i = 0; i < FieldCount; i++)
        if (FieldKeys[i] == Key)
            return;
    // Which of the frame's kinds: one it has already, or a new one - as many as the frame hands over.
    INT At = 0;
    while (At < FieldKindCount && FieldKindNames[At] != Kind)
        At++;
    const INT Wanted = FieldCount + 1;
    if (!HoldMore(FieldKindNames, FieldKindNamesHeld, At + 1, TEXT("RdxFieldKindNames")) ||
        !HoldMore(FieldKindLooks, FieldKindLooksHeld, (At + 1) * 4, TEXT("RdxFieldKindLooks")) ||
        !HoldMore(FieldKeys, FieldKeysHeld, Wanted, TEXT("RdxFieldKeys")) ||
        !HoldMore(FieldSizes, FieldSizesHeld, Wanted, TEXT("RdxFieldSizes")) ||
        !HoldMore(FieldKinds, FieldKindsHeld, Wanted, TEXT("RdxFieldKinds")) ||
        !HoldMore(FieldPoints, FieldPointsHeld, Wanted * RdxMath::FieldCorners * 3, TEXT("RdxFieldPoints")))
        return;
    FieldKindNames[At] = Kind;
    for (INT i = 0; i < 4; i++)
        FieldKindLooks[At * 4 + i] = Look[i];
    FieldKindCount = Max(FieldKindCount, At + 1);
    FieldKeys[FieldCount]  = Key;
    FieldSizes[FieldCount] = Count;
    FieldKinds[FieldCount] = At;
    appMemcpy(FieldPoints + FieldCount * RdxMath::FieldCorners * 3, Points, sizeof(FLOAT) * 3 * Count);
    FieldCount++;
}

UBOOL FBackend::HoloShellRuns()
{
    if (!Device || !bDepthReadable || !NeedsDepthRead())
        return 0;
    if (ShellReady == 0)
    {
        ShellReady = Shell.Init(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("hologram shell: the pass %s"), (ShellReady > 0)
               ? TEXT("up - a hologram shows the nearest of its surfaces alone")
               : TEXT("could NOT be made - a hologram is drawn as any translucent mesh"));
    }
    return ShellReady > 0;
}

FHoloShell::FVertex* FBackend::AddHoloTriangles(INT SrvIndex, UBOOL bNoSmooth, INT Count)
{
    if (!bInFrame || Count <= 0 || SrvIndex < 0)
        return nullptr;
    const INT   Corners = Count * 3;
    const UBOOL bSame   = (HoloRunCount > 0 && HoloRunSrvs[HoloRunCount - 1] == SrvIndex &&
                           HoloRuns[HoloRunCount - 1].bNoSmooth == (bNoSmooth ? 1 : 0)) ? 1 : 0;
    if (!HoldMore(HoloVerts, HoloVertHeld, HoloVertCount + Corners, TEXT("RdxHoloVerts")) ||
        (!bSame && (!HoldMore(HoloRuns, HoloRunHeld, HoloRunCount + 1, TEXT("RdxHoloRuns")) ||
                    !HoldMore(HoloRunSrvs, HoloRunSrvHeld, HoloRunCount + 1, TEXT("RdxHoloRunSrvs")))))
        return nullptr;
    if (!bSame)
    {
        FHoloShell::FRun& R = HoloRuns[HoloRunCount];
        R.First       = HoloVertCount;
        R.Count       = 0;
        R.Texture.ptr = 0;
        R.bNoSmooth   = bNoSmooth ? 1 : 0;
        HoloRunSrvs[HoloRunCount] = SrvIndex;
        HoloRunCount++;
    }
    HoloRuns[HoloRunCount - 1].Count += Corners;
    FHoloShell::FVertex* const At = HoloVerts + HoloVertCount;
    HoloVertCount += Corners;
    return At;
}

// The frame's hologram triangles laid over the finished world (FHoloShell): over the scene target, held against the
// copy of the world's depth - with the weapon in hand in it, which a hologram behind it stays behind.
void FBackend::DrawHoloShell(ID3D12GraphicsCommandList* Cmd)
{
    if (!Cmd || !Device || bOutputLive || HoloVertCount < 3 || HoloRunCount <= 0 || ShellReady <= 0)
        return;
    // A frame whose holograms cannot be laid has none in it: three of those one after the other, and the pass is out
    // from there on - the holograms are then drawn as any translucent mesh, by the solid pass, as before.
    const auto Missed = [&](const TCHAR* Why)
    {
        RdxLog(TEXT("hologram shell: %s - this frame's holograms are not drawn (%i such frames in a row)"), Why,
               HoloMisses + 1);
        if (++HoloMisses >= 3)
        {
            ShellReady = -1;
            RdxLog(TEXT("hologram shell: out from here on - a hologram is drawn as any translucent mesh"));
        }
    };
    if (!bDepthCopyDone || !WaterDepth || !Textures.GetHeap())
    {
        Missed(TEXT("no copy of the world's depth"));
        return;
    }
    PutAfterWorldDepth(Cmd);
    for (INT r = 0; r < HoloRunCount; r++)
        HoloRuns[r].Texture = Textures.GetGpuHandle(HoloRunSrvs[r]);

    D3D12_RESOURCE_BARRIER D;
    appMemzero(&D, sizeof(D));
    D.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    D.Transition.pResource   = WaterDepth.Get();
    D.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    const D3D12_RESOURCE_STATES DepthWas = WaterDepthState;
    const UBOOL bMove = (DepthWas == D3D12_RESOURCE_STATE_COPY_SOURCE) ? 0 : 1;
    if (bMove)
    {
        D.Transition.StateBefore = DepthWas;
        D.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        Cmd->ResourceBarrier(1, &D);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
    Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;      // the scene target's view (BindSceneTargets)
    const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const D3D12_RECT     Sc = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    FHoloShell::FFrame F;
    F.InvHalf[0] = 2.0f / (FLOAT)Max(Width, 1);
    F.InvHalf[1] = 2.0f / (FLOAT)Max(Height, 1);
    F.Jitter[0]  = JitterClip[0];
    F.Jitter[1]  = JitterClip[1];
    F.LightBoost = Solid.GetLightBoost();
    F.MipBias    = Solid.GetMipBias();
    F.Layers     = HoloLayers;
    const UBOOL bDrawn = Shell.Record(Cmd, Device.Get(), Colours.Draw, Rtv, WaterDepth.Get(),
                                      bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT,
                                      Textures.GetHeap(), F, HoloVerts, HoloVertCount, HoloRuns, HoloRunCount,
                                      FrameIndex);
    if (bDrawn)
        HoloMisses = 0;
    else
        Missed(TEXT("the pass could not record them"));
    // Said at the first frame and at every six-hundredth after: that the holograms are laid, and of how much.
    if (bDrawn && HoloFrames++ % 600 == 0)
        RdxLog(TEXT("hologram shell: laid over the world - %i triangles in %i runs (frame %i of it)"),
               HoloVertCount / 3, HoloRunCount, HoloFrames);
    if (bMove)
    {
        D.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        D.Transition.StateAfter  = DepthWas;
        Cmd->ResourceBarrier(1, &D);
    }
}

// The force field laid over the finished world (FFieldVolume), where the frame handed any of it over: over the scene
// target, within the world's view, within the part of the screen the field's polygons can show in.
void FBackend::DrawFieldVolume(ID3D12GraphicsCommandList* Cmd)
{
    if (!Cmd || !Device || bOutputLive || FieldCount <= 0 || CloudFocal <= 1.0f || FieldReady <= 0)
        return;
    if (!bDepthCopyDone || !WaterDepth)
    {
        static INT Said = 0;
        if (Said++ < 3)
            RdxLog(TEXT("field volume: no copy of the world's depth this frame - no field in it"));
        return;
    }
    // The frame's polygons made as few as they will go, on copies of them: the level hands a flat piece of a field
    // over as the many polygons it is cut into, and the pass asks every polygon it is given at every place of a ray.
    if (!HoldMore(FieldRowsList, FieldRowsHeld, FieldCount * RdxMath::FieldRows * 4, TEXT("RdxFieldRows")) ||
        !HoldMore(FieldFewSizes, FieldFewSizesHeld, FieldCount, TEXT("RdxFieldFewSizes")) ||
        !HoldMore(FieldFewKinds, FieldFewKindsHeld, FieldCount, TEXT("RdxFieldFewKinds")) ||
        !HoldMore(FieldFewPoints, FieldFewPointsHeld, FieldCount * RdxMath::FieldCorners * 3,
                  TEXT("RdxFieldFewPoints")))
        return;
    appMemcpy(FieldFewSizes, FieldSizes, sizeof(INT) * FieldCount);
    appMemcpy(FieldFewKinds, FieldKinds, sizeof(INT) * FieldCount);
    appMemcpy(FieldFewPoints, FieldPoints, sizeof(FLOAT) * 3 * RdxMath::FieldCorners * FieldCount);
    const INT Few = RdxMath::FieldMergeAll((RdxMath::FVec3*)FieldFewPoints, FieldFewSizes, FieldFewKinds, FieldCount);
    // The pass's rows, from the eye; and the part of the screen the polygons' balls can show in, in the engine's
    // pixels - the whole view where the eye is in one, or all but.
    const RdxMath::FVec3 Eye = { CamEye[0], CamEye[1], CamEye[2] };
    FLOAT Box[4] = { 1.0e9f, 1.0e9f, -1.0e9f, -1.0e9f };
    INT   Made   = 0;
    for (INT i = 0; i < Few; i++)
    {
        FLOAT* const Rows = FieldRowsList + Made * RdxMath::FieldRows * 4;
        if (!RdxMath::FieldPolygonRows((const RdxMath::FVec3*)(FieldFewPoints + i * RdxMath::FieldCorners * 3),
                                       FieldFewSizes[i], Eye, RdxMath::FieldHalf,
                                       FieldKindLooks + FieldFewKinds[i] * 4, Rows))
            continue;
        Made++;
        const FLOAT Vx = Rows[4] * CamAxes[0] + Rows[5] * CamAxes[1] + Rows[6] * CamAxes[2];
        const FLOAT Vy = Rows[4] * CamAxes[3] + Rows[5] * CamAxes[4] + Rows[6] * CamAxes[5];
        const FLOAT Vz = Rows[4] * CamAxes[6] + Rows[5] * CamAxes[7] + Rows[6] * CamAxes[8];
        const FLOAT R  = Rows[7];
        if (Vz + R <= 1.0f)
            continue;                                   // wholly behind the eye
        if (Vz - R <= 4.0f)
        {
            Box[0] = Box[1] = -1.0e9f;
            Box[2] = Box[3] = 1.0e9f;
            continue;
        }
        const FLOAT K = CloudFocal / (Vz - R);
        Box[0] = Min(Box[0], CloudScreen[0] + (Vx - R) * K);
        Box[1] = Min(Box[1], CloudScreen[1] + (Vy - R) * K);
        Box[2] = Max(Box[2], CloudScreen[0] + (Vx + R) * K);
        Box[3] = Max(Box[3], CloudScreen[1] + (Vy + R) * K);
    }
    if (Made <= 0 || Box[2] <= Box[0] || Box[3] <= Box[1])
        return;

    // The weapon in hand into the copy, where nearer: the field behind it stays behind it.
    PutAfterWorldDepth(Cmd);
    D3D12_RESOURCE_BARRIER D;
    appMemzero(&D, sizeof(D));
    D.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    D.Transition.pResource   = WaterDepth.Get();
    D.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    const D3D12_RESOURCE_STATES DepthWas = WaterDepthState;
    const UBOOL bMove = (DepthWas & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) ? 0 : 1;
    if (bMove)
    {
        D.Transition.StateBefore = DepthWas;
        D.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &D);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
    Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;      // the scene target's view (BindSceneTargets)
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const FLOAT Kx = (FLOAT)SceneWidth / (FLOAT)Max(Width, 1), Ky = (FLOAT)SceneHeight / (FLOAT)Max(Height, 1);
    FLOAT View[4] = { 0.0f, 0.0f, (FLOAT)Width, (FLOAT)Height };
    if (CloudRect[2] > CloudRect[0] && CloudRect[3] > CloudRect[1])
        for (INT i = 0; i < 4; i++)
            View[i] = CloudRect[i];
    D3D12_RECT Sc;
    Sc.left   = Clamp(appFloor(Max(Box[0], View[0]) * Kx), 0, SceneWidth);
    Sc.top    = Clamp(appFloor(Max(Box[1], View[1]) * Ky), 0, SceneHeight);
    Sc.right  = Clamp(appCeil(Min(Box[2], View[2]) * Kx), (INT)Sc.left, SceneWidth);
    Sc.bottom = Clamp(appCeil(Min(Box[3], View[3]) * Ky), (INT)Sc.top, SceneHeight);
    UBOOL bDrawn = 0;
    if (Sc.right > Sc.left && Sc.bottom > Sc.top)
    {
        Cmd->RSSetViewports(1, &Vp);
        Cmd->RSSetScissorRects(1, &Sc);
        FFieldVolume::FView V;
        appMemzero(&V, sizeof(V));
        V.Screen[0]      = CloudScreen[0];
        V.Screen[1]      = CloudScreen[1];
        V.Focal          = CloudFocal;
        V.EngPerPixel[0] = (FLOAT)Max(Width, 1) / (FLOAT)Max(SceneWidth, 1);
        V.EngPerPixel[1] = (FLOAT)Max(Height, 1) / (FLOAT)Max(SceneHeight, 1);
        for (INT i = 0; i < 3; i++)
        {
            V.Eye[i]   = CamEye[i];
            V.AxisX[i] = CamAxes[i];
            V.AxisY[i] = CamAxes[3 + i];
            V.AxisZ[i] = CamAxes[6 + i];
        }
        bDrawn = Field.Record(Cmd, Device.Get(), Colours.Draw, WaterDepth.Get(),
                              bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT, V,
                              FieldRowsList, Made, RdxMath::FieldHalf, FieldTime, FrameIndex);
    }
    // Said at the first frame and at every six-hundredth after: that the field is laid, of how many polygons - and
    // how many the frame handed over for them -, of how many kinds, and where.
    if (FieldFrames++ % 600 == 0)
        RdxLog(TEXT("field volume: %s - %i polygons (of the %i handed over), %i kinds, the screen's part ")
               TEXT("(%i, %i)..(%i, %i) of %ix%i (frame %i of it)"),
               bDrawn ? TEXT("laid over the world") : TEXT("NOT laid"), Made, FieldCount, FieldKindCount, (INT)Sc.left,
               (INT)Sc.top, (INT)Sc.right, (INT)Sc.bottom, SceneWidth, SceneHeight, FieldFrames);
    if (bMove)
    {
        D.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        D.Transition.StateAfter  = DepthWas;
        Cmd->ResourceBarrier(1, &D);
    }
    // The pass bound a view heap of its own; everything after this reads through the texture heap.
    ID3D12DescriptorHeap* Heaps[1] = { Textures.GetHeap() };
    if (Heaps[0])
        Cmd->SetDescriptorHeaps(1, Heaps);
}

// The walk's passes and its targets, made at the first need of them - the world's view's walk (DrawVolumetrics) or a
// mirror's (WalkMirrorView). False where they are not to be had: for good where a pass was refused, while the sizes
// stand where a target was.
UBOOL FBackend::WalkReady()
{
    if (VolumeReady == 0)
    {
        VolumeReady = (Volume.Init(Device.Get()) && Volume.InitComposite(Device.Get(), Colours.Draw)) ? 1 : -1;
        if (VolumeReady < 0)
            bVolumeRefused = 1;      // and the cards are drawn again from the next frame
        RdxLog(TEXT("volumetrics: the passes %s"),
               (VolumeReady > 0) ? TEXT("up - smoke and gas are walked as volumes")
                                 : TEXT("FAILED - the game's own flat sprites are drawn instead, as before"));
    }
    if (VolumeReady < 0)
        return 0;
    // The laying-on's pipeline carries the scene's format: made again when that has changed (InitComposite does nothing
    // while it has not).
    if (!Volume.InitComposite(Device.Get(), Colours.Draw))
    {
        VolumeReady    = -1;
        bVolumeRefused = 1;
        RdxLog(TEXT("volumetrics: the laying-on could NOT be made for the new colours - the game's own flat sprites "
                    "are drawn instead"));
        return 0;
    }
    if (VolumeOut())
        return 0;
    if (!Volume.EnsureTargets(Device.Get(), SceneWidth, SceneHeight))
    {
        // The gathering stops with it (SetVolumetrics), so the game's own flat sprites come back rather than the smoke
        // simply going missing.
        RefuseTarget(VolumeRefusal, TEXT("volumetrics"));
        return 0;
    }
    return 1;
}

// A mirror's view of the level walked as the world's own view is (DrawVolumetrics), by the same pass: the smoke and
// the gas that view holds, its fires' flames and its waterfalls' columns - from the mirror's plane on, to that view's
// own depth where it stands drawn (Depth, readable by a compute shader) - laid over the picture within the part of
// it the mirror shows in. The view's lists are its own in the frame (FVolumetrics::Record's View). Not the lamps'
// shafts: the shadow maps they ask are drawn where the world ends, after this. The weapon lamps' beams in the air are
// walked: their maps are drawn before the world (PrepareOwnLamps).
void FBackend::WalkMirrorView(ID3D12GraphicsCommandList* Cmd, INT Index, ID3D12Resource* Depth)
{
    const FMirrorEnd& E      = MirrorEnds[Index];
    const INT         NPuffs = Solid.GetCloudsOn() ? E.PuffCount : 0;
    if ((NPuffs <= 0 && E.FireCount <= 0 && E.FallCount <= 0 && !OwnBeamsNow()) || !WalkReady())
        return;
    const FLOAT* const Eye = E.View;
    const FLOAT* const AX  = E.View + 3;
    const FLOAT* const AY  = E.View + 6;
    const FLOAT* const AZ  = E.View + 9;
    // The part of the picture the mirror shows in, in the scene's pixels: what is laid on goes there alone.
    D3D12_RECT Sc = { 0, 0, SceneWidth, SceneHeight };
    if (E.Rect[2] > E.Rect[0] && E.Rect[3] > E.Rect[1] && Width > 0 && Height > 0)
    {
        const FLOAT Kx = (FLOAT)SceneWidth / (FLOAT)Width, Ky = (FLOAT)SceneHeight / (FLOAT)Height;
        Sc.left   = Clamp(appFloor(E.Rect[0] * Kx), 0, SceneWidth);
        Sc.top    = Clamp(appFloor(E.Rect[1] * Ky), 0, SceneHeight);
        Sc.right  = Clamp(appCeil(E.Rect[2] * Kx), (INT)Sc.left, SceneWidth);
        Sc.bottom = Clamp(appCeil(E.Rect[3] * Ky), (INT)Sc.top, SceneHeight);
    }
    if (Sc.right <= Sc.left || Sc.bottom <= Sc.top)
        return;

    FVolumetrics::FWalk W;
    appMemzero(&W, sizeof(W));
    W.Screen[0]      = E.View[13];
    W.Screen[1]      = E.View[14];
    W.Focal          = E.View[12];
    W.EngPerPixel[0] = (FLOAT)Max(Width, 1) / (FLOAT)Max(Volume.TargetW(), 1);
    W.EngPerPixel[1] = (FLOAT)Max(Height, 1) / (FLOAT)Max(Volume.TargetH(), 1);
    W.Time           = FireClock();
    for (INT k = 0; k < 3; k++)
    {
        W.EyeWorld[k] = Eye[k];
        W.AxisX[k]    = AX[k];
        W.AxisY[k]    = AY[k];
        W.AxisZ[k]    = AZ[k];
    }
    for (INT k = 0; k < 4; k++)
        W.NearPlane[k] = E.Plane[k];
    // The walk's target is half the scene's size: the same part of it, and two of its pixels more each way - the
    // laying-on reads between the target's pixels, and at the part's edge it must not read an unwalked one.
    W.Window[0] = Max((INT)Sc.left / 2 - 2, 0);
    W.Window[1] = Max((INT)Sc.top / 2 - 2, 0);
    W.Window[2] = Min(((INT)Sc.right + 1) / 2 + 2, Volume.TargetW());
    W.Window[3] = Min(((INT)Sc.bottom + 1) / 2 + 2, Volume.TargetH());

    // The lamp the smoke and the columns are lit by: the one the world's view's walk takes in this frame - the nearest
    // to its first puff - so that a cloud and its image in the mirror are lit alike; with no smoke in the world's
    // view, the nearest to this view's first puff, or to its first column.
    FLOAT At[3] = { 0.0f, 0.0f, 0.0f };
    UBOOL bAt   = 0;
    if (Solid.GetCloudsOn() && CloudPuffCount > 0)
    {
        const FLOAT* const V = CloudPuffs[0].Centre;
        for (INT k = 0; k < 3; k++)
            At[k] = CamEye[k] + CamAxes[k] * V[0] + CamAxes[3 + k] * V[1] + CamAxes[6 + k] * V[2];
        bAt = 1;
    }
    else if (NPuffs > 0)
    {
        const FLOAT* const V = MirrorClouds[E.PuffFirst].Centre;
        for (INT k = 0; k < 3; k++)
            At[k] = Eye[k] + AX[k] * V[0] + AY[k] * V[1] + AZ[k] * V[2];
        bAt = 1;
    }
    else if (E.FallCount > 0)
    {
        const FLOAT* const F = MirrorFalls + E.FallFirst * RdxMath::FallColumnFloats;
        At[0] = F[0];
        At[1] = F[1];
        At[2] = 0.5f * (F[2] + F[3]);
        bAt   = 1;
    }
    const FShadowLightFrame* Lamp  = nullptr;
    FLOAT                    LampD = 0.0f;
    for (INT i = 0; bAt && i < Shadow.GetLightCount(); i++)
    {
        const FShadowLightFrame* const L = LightAt(i);
        if (!L)
            continue;
        const FLOAT dx = L->World[0] - At[0], dy = L->World[1] - At[1], dz = L->World[2] - At[2];
        const FLOAT d  = dx * dx + dy * dy + dz * dz;
        if (!Lamp || d < LampD)
        {
            Lamp  = L;
            LampD = d;
        }
    }
    if (Lamp)
    {
        const FLOAT D[3] = { Lamp->World[0] - Eye[0], Lamp->World[1] - Eye[1], Lamp->World[2] - Eye[2] };
        W.LampView[0] = D[0] * AX[0] + D[1] * AX[1] + D[2] * AX[2];
        W.LampView[1] = D[0] * AY[0] + D[1] * AY[1] + D[2] * AY[2];
        W.LampView[2] = D[0] * AZ[0] + D[1] * AZ[1] + D[2] * AZ[2];
        W.LampReach   = (Lamp->Radius > 1.0f) ? Lamp->Radius : 1.0f;
        W.LampHue[0]  = Lamp->Colour[0];
        W.LampHue[1]  = Lamp->Colour[1];
        W.LampHue[2]  = Lamp->Colour[2];
        W.LampLit     = Lamp->Bright;
    }
    W.Grain         = CloudTune.Grain;
    W.Ambient       = CloudTune.Ambient;
    W.Rise          = CloudTune.Rise;
    W.Warp          = CloudTune.Warp;
    W.SelfShade     = CloudTune.SelfShade;
    W.StepWorld     = CloudTune.StepWorld;
    W.Steps         = CloudTune.Steps;
    W.Falls         = (E.FallCount > 0) ? MirrorFalls + E.FallFirst * RdxMath::FallColumnFloats : nullptr;
    W.FallCount     = E.FallCount;
    W.FallDensity   = RdxMath::WaterfallVolumeDensity;
    W.FallAmbient   = RdxMath::WaterfallAmbient;
    W.FallRate      = RdxMath::WaterfallStreakRate;
    W.FallReactive  = WalkReactiveFalls;
    W.SmokeReactive = WalkReactiveSmoke;
    // The view's flame bodies and their lists by tile of the walk's target; without the lists none of them is walked.
    const FLOAT* const Bodies = (E.FireCount > 0) ? MirrorBodies + E.FireFirst * RdxMath::FlameBodyFloats : nullptr;
    INT       NFires        = E.FireCount;
    const INT FireTileUints = (NFires > 0) ? BuildFireTiles(Bodies, NFires, W) : 0;
    if (FireTileUints <= 0)
        NFires = 0;
    W.Fires         = Bodies;
    W.FireCount     = NFires;
    W.FireTiles     = FireTileData;
    W.FireTileUints = FireTileUints;
    W.FireGlow      = RdxMath::FireGlow;
    W.FireRate      = RdxMath::FireRise * FireSpeed;
    W.bSparks       = bFireSparks;
    // The view's puffs by tile, by its own projection.
    FCloudPuff* const Puffs = (NPuffs > 0) ? MirrorClouds + E.PuffFirst : nullptr;
    const INT TilesX    = RdxMath::CloudTilesAlong(Volume.TargetW());
    const INT TilesY    = RdxMath::CloudTilesAlong(Volume.TargetH());
    const INT TileUints = (NPuffs > 0)
                          ? BuildCloudTiles(Puffs, NPuffs, W.Focal, W.Screen, TilesX, TilesY, W.EngPerPixel) : 0;
    // The upscaler's reactive mask, as the world's view's walk writes it: every mirror's view of the frame is handed
    // it alike where any of them has something to tell (FVolumetrics::Record says why).
    if (!bMirrorTellsKnown)
    {
        bMirrorTellsKnown = 1;
        bMirrorTells      = 0;
        for (INT i = 0; i < MirrorEndCount && Volume.WritesReactive() && UpscaleMode && bFsr3Wanted; i++)
        {
            const FMirrorEnd& V = MirrorEnds[i];
            if (V.FireCount > 0 || (V.PuffCount > 0 && Solid.GetCloudsOn() && WalkReactiveSmoke > 0.0f) ||
                (V.FallCount > 0 && WalkReactiveFalls > 0.0f))
                bMirrorTells = 1;
        }
    }
    ID3D12Resource* WalkMask = nullptr;
    if (bMirrorTells && ReactiveBegin(Cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        WalkMask = Reactive.Get();

    D3D12_SHADER_RESOURCE_VIEW_DESC OwnView;
    OwnBeamsInto(W, OwnView);
    const UBOOL bLists = Volume.Record(Cmd, Device.Get(), Depth,
                                       bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT, Puffs,
                                       (TileUints > 0) ? NPuffs : 0, CloudTileData, TileUints, TilesX, W, FrameIndex,
                                       nullptr, nullptr, WalkMask, 1 + Index);
    OwnStatBeamMirrors += (W.OwnCount > 0 && Volume.Wrote()) ? 1 : 0;
    // The result laid over the scene target alone, as the world's view's is, within the mirror's part of it.
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
    Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;      // the scene target's view (BindSceneTargets)
    if (Volume.Wrote())
    {
        D3D12_RESOURCE_BARRIER B;
        appMemzero(&B, sizeof(B));
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Volume.Target();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &B);

        Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
        const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
        Cmd->RSSetViewports(1, &Vp);
        Cmd->RSSetScissorRects(1, &Sc);
        Volume.RecordComposite(Cmd);

        B.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Cmd->ResourceBarrier(1, &B);
    }
    // The air over the view's fires, once their flames are on the picture, as the world's view has it
    // (DrawVolumetrics): what the mirror shows through that air is bent, read by the view's own depth.
    if (bHeatHaze && NFires > 0 && Volume.FiresHeld() && SceneColor)
        Volume.RecordHaze(Cmd, Device.Get(), SceneColor.Get(), Colours.Draw, Rtv, Sc, W, 1);
    // Now and then, what the mirrors' views' walks took: that they run at all, and with what.
    static INT Asked = 0, Wrote = 0;
    Wrote += Volume.Wrote() ? 1 : 0;
    if ((Asked++ % 600) == 0)
        RdxLog(TEXT("mirrors' views: the walk has run over %i of %i so far - this one with %i puffs of smoke, %i flame "
                    "bodies and %i columns of water, within %i,%i - %i,%i of the scene%s"),
               Wrote, Asked, (TileUints > 0) ? NPuffs : 0, NFires, E.FallCount, (INT)Sc.left, (INT)Sc.top,
               (INT)Sc.right, (INT)Sc.bottom, bLists ? TEXT("") : TEXT("; ITS SMOKE'S LISTS WERE REFUSED"));
}

void FBackend::DrawVolumetrics(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    // Five things can want this pass now, and any one on its own is enough: the smoke, the light shafts, the waterfalls
    // gathered instead of drawn (Water=Low and up), the fires' flame sheets gathered the same way, and the weapon
    // lamps' beams in the air (Flashlight=Everybody).
    // The smoke: this frame's puffs, or the ones followed from before - a plume whose cards the engine stopped sending
    // because its foot left the view is still there to walk.
    const UBOOL bSmoke  = Solid.GetCloudsOn() && (CloudPuffCount > 0 || CloudTrackCount > 0);
    const UBOOL bShafts = ShaftStrength > 0.0f && ShaftsCanRun();
    const UBOOL bFalls  = Fall.Count > 0;
    const UBOOL bFires  = Fire.Count > 0;
    const UBOOL bBeams  = OwnBeamsNow();
    if (!Cmd || !Device || bOutputLive || CloudFocal <= 1.0f || (!bSmoke && !bShafts && !bFalls && !bFires && !bBeams))
        return;
    if (!WalkReady())
        return;
    // The copy of the scene's depth the frame already made (CopyForWater, asked for by GetCloudsOn among others).
    // It is taken at the first SEE-THROUGH surface of the draw list, and the engine draws every solid surface before
    // any of those - so the whole of the world that a cloud can hide behind is in it.
    //
    // It is not made again here. Making it twice in a frame means transitioning resources out of states they are no
    // longer in, and that is not a wrong picture: it is the device removed, which is what the first attempt at this
    // did (hr 0x887A0005).
    if (!bDepthCopyDone || !WaterDepth)
        return;

    // FOLLOWING THE PUFFS between frames, so that one the game stops sending can be kept a while instead of blinking
    // out.
    //
    // A card carries no name, so which puff is which is worked out from WHERE it stands - in the WORLD, because in
    // view space every puff looks new the moment the head turns. A puff with nothing near it in this frame's list is
    // gone, and is kept a moment longer, thinning - or whole, while it is only out of the view. A new one is not eased
    // in: the game brings its own puffs in from nothing.
    if (!Solid.GetCloudsOn())
        CloudTrackCount = 0;        // switched off: nothing is gathered, and what was followed is not drawn on
    else
    {
        const DOUBLE Now  = appSeconds();
        const FLOAT  Step = (CloudLastTime > 0.0) ? (FLOAT)Min(Now - CloudLastTime, 0.25) : 0.0f;
        CloudLastTime = Now;
        auto ToWorld = [&](const FLOAT* V, FLOAT* Out)
        {
            for (INT k = 0; k < 3; k++)
                Out[k] = CamEye[k] + CamAxes[k] * V[0] + CamAxes[3 + k] * V[1] + CamAxes[6 + k] * V[2];
        };
        // The puffs followed, by the cell of the level each stood in: a chain a hash. Two cells may share a chain;
        // the distance below tells their puffs apart.
        INT Cells = 256;
        while (Cells < CloudTrackCount * 2)
            Cells *= 2;
        const UBOOL bGrid = HoldMore(CloudCellHead, CloudCellHeld, Cells, TEXT("RdxCloudCells")) &&
                            HoldMore(CloudCellNext, CloudNextHeld, Max(CloudTrackCount, 1), TEXT("RdxCloudChain"));
        auto CellOf = [](FLOAT V) -> INT { return appFloor(V / RdxMath::CloudTrackCell); };
        auto ChainOf = [Cells](INT X, INT Y, INT Z) -> INT
        {
            const DWORD Mixed = ((DWORD)X * 73856093u) ^ ((DWORD)Y * 19349663u) ^ ((DWORD)Z * 83492791u);
            return (INT)(Mixed & (DWORD)(Cells - 1));
        };
        for (INT c = 0; c < Cells && bGrid; c++)
            CloudCellHead[c] = -1;
        for (INT t = 0; t < CloudTrackCount; t++)
        {
            CloudTracks[t].bUsed = 0;
            if (!bGrid)
                continue;
            const INT H = ChainOf(CellOf(CloudTracks[t].World[0]), CellOf(CloudTracks[t].World[1]),
                                  CellOf(CloudTracks[t].World[2]));
            CloudCellNext[t] = CloudCellHead[H];
            CloudCellHead[H] = t;
        }
        // Every puff of this frame, matched to the nearest one followed - within three quarters of its own size, or
        // as far as a puff goes in a frame, so two puffs a whole cloud apart are never taken for one another and a
        // fast one is still found. Never further than a cell: the lookup reaches the cells beside its own.
        for (INT i = 0; i < CloudPuffCount; i++)
        {
            FLOAT Wp[3];
            ToWorld(CloudPuffs[i].Centre, Wp);
            const FLOAT Reach = Min(Max(CloudPuffs[i].Radius * 0.75f, RdxMath::CloudTrackSpeed * Step),
                                    RdxMath::CloudTrackCell);
            INT   Best  = -1;
            FLOAT BestD = Reach * Reach;
            for (INT z = CellOf(Wp[2] - Reach); z <= CellOf(Wp[2] + Reach) && bGrid; z++)
                for (INT y = CellOf(Wp[1] - Reach); y <= CellOf(Wp[1] + Reach); y++)
                    for (INT x = CellOf(Wp[0] - Reach); x <= CellOf(Wp[0] + Reach); x++)
                        for (INT t = CloudCellHead[ChainOf(x, y, z)]; t >= 0; t = CloudCellNext[t])
                        {
                            if (CloudTracks[t].bUsed)
                                continue;
                            const FLOAT dx = CloudTracks[t].World[0] - Wp[0];
                            const FLOAT dy = CloudTracks[t].World[1] - Wp[1];
                            const FLOAT dz = CloudTracks[t].World[2] - Wp[2];
                            const FLOAT d  = dx * dx + dy * dy + dz * dz;
                            if (d <= BestD)
                            {
                                Best  = t;
                                BestD = d;
                            }
                        }
            if (Best >= 0)
            {
                CloudTracks[Best].bUsed = 1;
                CloudTracks[Best].Gone  = 0.0f;
                CloudTracks[Best].Away  = 0.0f;
            }
            else if (HoldMore(CloudTracks, CloudTrackHeld, CloudTrackCount + 1, TEXT("RdxCloudTracks")))
            {
                // Not in the grid, which was made of the puffs followed before this frame: no later puff takes it.
                Best = CloudTrackCount++;
                appMemzero(&CloudTracks[Best], sizeof(FCloudTrack));
                CloudTracks[Best].bUsed = 1;
                CloudMadeNow++;          // a puff the tracker did NOT recognise: a real event, not a state
            }
            if (Best >= 0)
            {
                for (INT k = 0; k < 3; k++)
                {
                    CloudTracks[Best].World[k]  = Wp[k];
                    CloudTracks[Best].Colour[k] = CloudPuffs[i].Colour[k];
                }
                CloudTracks[Best].Radius = CloudPuffs[i].Radius;
                CloudTracks[Best].Tall   = CloudPuffs[i].Tall;
                CloudTracks[Best].Fade   = CloudPuffs[i].Fade;
            }
        }
        // The ones nothing matched this frame: kept for a moment, thinning, and put back into the list so that they
        // leave instead of blinking out. Then dropped.
        for (INT t = 0; t < CloudTrackCount; )
        {
            if (CloudTracks[t].bUsed)
            {
                t++;
                continue;
            }
            // Back into view space from the world, which is where it was kept - and it is needed BEFORE the
            // fade is decided, because where it stands is what says whether it died or merely left the view.
            FLOAT V[3];
            {
                const FLOAT d0 = CloudTracks[t].World[0] - CamEye[0];
                const FLOAT d1 = CloudTracks[t].World[1] - CamEye[1];
                const FLOAT d2 = CloudTracks[t].World[2] - CamEye[2];
                for (INT k = 0; k < 3; k++)
                    V[k] = CamAxes[k * 3 + 0] * d0 + CamAxes[k * 3 + 1] * d1 + CamAxes[k * 3 + 2] * d2;
            }
            // OUT OF THE VIEW, OR DEAD? The engine culls a sprite whose card is off the screen, so it simply
            // stops arriving - and treating that as a death is what made a cloud lose its lower half the
            // moment the player looked up. Its own card's size is what it covers, so the test is the card
            // against the screen, widened by itself: a volume reaches past the card it came from.
            const FLOAT Reach = Max(CloudTracks[t].Radius, CloudTracks[t].Tall);
            UBOOL bAway = 0;
            if (V[2] <= 1.0f)
                bAway = 1;                   // beside the camera or behind it
            else
            {
                const FLOAT Sx = CloudFocal * V[0] / V[2] + CloudScreen[0];
                const FLOAT Sy = CloudFocal * V[1] / V[2] + CloudScreen[1];
                const FLOAT Rp = CloudFocal * Reach / V[2];
                if (Sx + Rp < CloudRect[0] || Sy + Rp < CloudRect[1] || Sx - Rp > CloudRect[2] ||
                    Sy - Rp > CloudRect[3])
                    bAway = 1;
            }
            FLOAT Out;
            if (bAway && CloudTracks[t].Away < RdxMath::CloudKeepAway)
            {
                // Kept whole. Fading it would be the same mistake in slower motion.
                CloudTracks[t].Away += Step;
                Out = 1.0f;
            }
            else
            {
                CloudTracks[t].Gone += Step;
                Out = (RdxMath::CloudFadeOut > 0.0f)
                      ? 1.0f - Clamp(CloudTracks[t].Gone / RdxMath::CloudFadeOut, 0.0f, 1.0f) : 0.0f;
            }
            if (Out > 0.02f)
            {
                // Thinning is less smoke, not darker smoke: the fade goes into its thickness (AddCloudPuff).
                FCloudPuff P;
                for (INT k = 0; k < 3; k++)
                {
                    P.Centre[k] = V[k];
                    P.Colour[k] = CloudTracks[t].Colour[k];
                }
                P.Radius = CloudTracks[t].Radius;
                P.Tall   = CloudTracks[t].Tall;
                P.Fade   = CloudTracks[t].Fade * Out;
                AddCloudPuff(P);
                t++;
            }
            else
            {
                CloudTracks[t] = CloudTracks[--CloudTrackCount];
                CloudLostNow++;          // and one let go for good
            }
        }
    }

    // Whether the following ABOVE is doing anything at all. A tracker that never finds a new puff and never lets one
    // go is indistinguishable from no tracker, and it would look exactly like this one: the picture would simply be
    // what the game sent. So the counts are reported, sparsely.
    {
        static INT Births = 0, Deaths = 0, Frames = 0;
        Births += CloudMadeNow;
        Deaths += CloudLostNow;
        // WHY A CLOUD WILL NOT HOLD STILL. A shiver is either the LIST changing under the walk from one frame
        // to the next, or a card whose put-back size jumps about - so both are measured, at their worst rather
        // than on average, because an average hides exactly the frame that did it.
        //
        // EVENTS, not states: the first try counted how many puffs were young or leaving, which for a cloud
        // that emits without stopping is nearly all of them all the time - a number that looked alarming and
        // meant nothing. What matters is how many the tracker FAILED TO RECOGNISE from the frame before.
        if (CloudMadeNow + CloudLostNow > CloudChurnMax)
            CloudChurnMax = CloudMadeNow + CloudLostNow;
        CloudMadeNow = 0;
        CloudLostNow = 0;
        for (INT q = 0; q < CloudPuffCount; q++)
        {
            const FLOAT* const C = CloudPuffs[q].Centre;
            const FLOAT Away = appSqrt(C[0] * C[0] + C[1] * C[1] + C[2] * C[2]);
            if (Away < Max(CloudPuffs[q].Radius, CloudPuffs[q].Tall))
            {
                CloudInside++;
                break;
            }
        }
        if (++Frames % 600 == 0)
        {
            RdxLog(TEXT("volumetric smoke (600 frames): %i puffs this frame, %i followed; %i new and %i let go "
                        "(counted over the 600); the longest list of a tile %i puffs; %i had no memory"),
                   CloudPuffCount, CloudTrackCount, Births, Deaths, CloudTileMost, CloudPuffLost);
            RdxLog(TEXT("volumetric smoke, why it may not hold still (600 frames): in ONE frame up to %i puffs "
                        "were not recognised from the frame before; the widest a clipped card had to be put "
                        "back was %.1fx; the camera stood inside a puff in %i of the frames; cards seen this "
                        "frame %i, depth copy %s"),
                   CloudChurnMax, CloudGrewMax, CloudInside, CloudCardsSeen,
                   bCloudDepthLast ? TEXT("there") : TEXT("MISSING - the flat sprites are drawn"));
            Births        = 0;
            Deaths        = 0;
            CloudChurnMax = 0;
            CloudGrewMax  = 1.0f;
            CloudInside   = 0;
            CloudTileMost = 0;
        }
    }

    FVolumetrics::FWalk W;
    appMemzero(&W, sizeof(W));
    W.Screen[0] = CloudScreen[0];
    W.Screen[1] = CloudScreen[1];
    W.Focal     = CloudFocal;
    // How many of the engine's pixels one pixel of the walk's target covers: the whole screen over the whole target.
    // Not twice the view's centre - a conversation's picture sits between two black bars of different heights, and
    // its centre is not the screen's.
    W.EngPerPixel[0] = (FLOAT)Max(Width, 1) / (FLOAT)Max(Volume.TargetW(), 1);
    W.EngPerPixel[1] = (FLOAT)Max(Height, 1) / (FLOAT)Max(Volume.TargetH(), 1);
    W.Time = FireClock();
    for (INT i = 0; i < 3; i++)
    {
        W.EyeWorld[i] = CamEye[i];
        W.AxisX[i]    = CamAxes[i];
        W.AxisY[i]    = CamAxes[3 + i];
        W.AxisZ[i]    = CamAxes[6 + i];
    }
    // The waterfalls' columns, made from this frame's sheets (RdxMath::FallColumnsMake).
    static_assert((INT)FVolumetrics::FALL_FLOATS == (INT)RdxMath::FallColumnFloats, "a column's numbers differ");
    static FLOAT Falls[FVolumetrics::MAX_FALLS * FVolumetrics::FALL_FLOATS];
    const INT NFalls = bFalls ? RdxMath::FallColumnsMake(Fall.Pieces, Fall.Count, Falls, FVolumetrics::MAX_FALLS) : 0;
    {
        static INT Frames = 0;
        if (NFalls > 0 && ++Frames % 600 == 1)
            RdxLog(TEXT("waterfalls as volumes: %i sheets made %i columns; the first at %.0f %.0f from %.0f up to %.0f, "
                        "%.0f by %.0f across, %s"), Fall.Count, NFalls, Falls[0], Falls[1], Falls[2], Falls[3],
                   2.0f * Falls[6], 2.0f * Falls[7], (Falls[11] > 0.5f) ? TEXT("into water") : TEXT("onto no water"));
    }

    // The nearest lamp to the whole cloud, not to each puff: one walk lights them all, and a cloud lit from two
    // sides at once by two different lamps is not something these levels ever show. With no smoke, to the first
    // waterfall.
    FLOAT LampFor[3] = { 0.0f, 0.0f, 0.0f };
    UBOOL bLampFor   = 0;
    if (CloudPuffCount > 0)
    {
        for (INT k = 0; k < 3; k++)
            LampFor[k] = CloudPuffs[0].Centre[k];
        bLampFor = 1;
    }
    else if (NFalls > 0)
    {
        const FLOAT d[3] = { Falls[0] - CamEye[0], Falls[1] - CamEye[1], 0.5f * (Falls[2] + Falls[3]) - CamEye[2] };
        for (INT k = 0; k < 3; k++)
            LampFor[k] = CamAxes[k * 3 + 0] * d[0] + CamAxes[k * 3 + 1] * d[1] + CamAxes[k * 3 + 2] * d[2];
        bLampFor = 1;
    }
    if (bLampFor)
    {
        INT   Best  = -1;
        FLOAT BestD = 0.0f;
        for (INT i = 0; i < Shadow.GetLightCount(); i++)
        {
            const FShadowLightFrame* const L = LightAt(i);
            if (!L)
                continue;
            const FLOAT dx = L->Cam[0] - LampFor[0];
            const FLOAT dy = L->Cam[1] - LampFor[1];
            const FLOAT dz = L->Cam[2] - LampFor[2];
            const FLOAT d  = dx * dx + dy * dy + dz * dz;
            if (Best < 0 || d < BestD)
            {
                Best  = i;
                BestD = d;
            }
        }
        if (Best >= 0)
        {
            const FShadowLightFrame* const L = LightAt(Best);
            W.LampView[0] = L->Cam[0];
            W.LampView[1] = L->Cam[1];
            W.LampView[2] = L->Cam[2];
            W.LampReach   = (L->Radius > 1.0f) ? L->Radius : 1.0f;
            W.LampHue[0]  = L->Colour[0];
            W.LampHue[1]  = L->Colour[1];
            W.LampHue[2]  = L->Colour[2];
            W.LampLit     = L->Bright;
        }
    }
    W.Grain   = CloudTune.Grain;
    W.Ambient = CloudTune.Ambient;
    W.Rise    = CloudTune.Rise;
    W.Warp    = CloudTune.Warp;
    W.SelfShade = CloudTune.SelfShade;
    W.StepWorld = CloudTune.StepWorld;
    W.Steps   = CloudTune.Steps;
    if (bVolumeDepthView)
        W.Steps |= (1 << 30);             // the walk's measurement view (SetVolumeDepthView)
    W.Falls       = Falls;
    W.FallCount   = NFalls;
    W.FallDensity = RdxMath::WaterfallVolumeDensity;
    W.FallAmbient = RdxMath::WaterfallAmbient;
    W.FallRate    = RdxMath::WaterfallStreakRate;
    W.FallReactive  = WalkReactiveFalls;
    W.SmokeReactive = WalkReactiveSmoke;
    // The fires' flame bodies, made from this frame's pieces (RdxMath::FlameBodiesMake): a body a piece at the most,
    // and the lists made as long as that.
    static_assert((INT)FVolumetrics::FIRE_FLOATS == (INT)RdxMath::FlameBodyFloats, "a flame body's numbers differ");
    INT NFires = 0, FireTileUints = 0;
    if (bFires)
    {
        if (HoldMore(FireBodies, FireBodyHeld, Fire.Count * (INT)FVolumetrics::FIRE_FLOATS, TEXT("RdxFireBodies")) &&
            HoldMore(FireGroups, FireGroupHeld, Fire.Count, TEXT("RdxFireGroups")))
        {
            NFires = RdxMath::FlameBodiesMake(Fire.Pieces, Fire.Count, FireBodies, Fire.Count,
                                              W.EngPerPixel[0] / CloudFocal, FireGroups);
            // Their lists by tile of the walk's target: the walk asks a pixel only the bodies its ray can meet.
            FireTileUints = BuildFireTiles(FireBodies, NFires, W);
            if (NFires > 0 && FireTileUints <= 0)
            {
                RefuseFireLists(TEXT("the bodies' lists by tile"), NFires);
                NFires = 0;
            }
        }
        else
            RefuseFireLists(TEXT("the bodies' list"), Fire.Count);
    }
    {
        // The most a frame has held since the last line, with it: the frame that matters is the fullest one.
        static INT Frames = 0, MostPieces = 0, MostBodies = 0;
        MostPieces = Max(MostPieces, Fire.Count);
        MostBodies = Max(MostBodies, NFires);
        if (NFires > 0 && ++Frames % 600 == 1)
        {
            RdxLog(TEXT("fires as volumes: %i pieces made %i flame bodies (the most in a frame since the last line: %i "
                        "pieces, %i bodies; the longest list of a tile %i bodies); the first at %.0f %.0f from %.0f up "
                        "to %.0f, %.0f long and %.0f across"),
                   Fire.Count, NFires, MostPieces, MostBodies, FireTileMost, FireBodies[0], FireBodies[1],
                   FireBodies[2], FireBodies[3], 2.0f * FireBodies[6], 2.0f * FireBodies[7]);
            MostPieces   = 0;
            MostBodies   = 0;
            FireTileMost = 0;
        }
    }
    W.Fires         = FireBodies;
    W.FireCount     = NFires;
    W.FireTiles     = FireTileData;
    W.FireTileUints = FireTileUints;
    W.FireGlow  = RdxMath::FireGlow;
    W.FireRate  = RdxMath::FireRise * FireSpeed;
    W.bSparks   = bFireSparks;

    // THE LAMPS THE SHAFTS COME FROM. The same lights the shadow maps already drew, so a shaft is cut by the
    // same walls that cut the shadows - and it costs no map of its own. The strongest few of those whose reach can
    // show on the screen, as strong as the shadows weigh them and eased in at the pick's edge
    // (RdxMath::ShaftLampPick): a lamp behind the eye takes no place from one in view, and none comes or goes at
    // once. Which of them a pixel's ray can meet is listed by tile of the walk's target (BuildShaftTiles).
    static FLOAT Lamps[FVolumetrics::MAX_LAMPS * FVolumetrics::LAMP_FLOATS];
    INT NLamps         = 0;
    INT ShaftTileUints = 0;
    if (bShafts && !bShaftListsRefused)
    {
        INT   Seen[FVolumetrics::MAX_LAMPS];
        FLOAT Score[FVolumetrics::MAX_LAMPS];
        INT   NSeen = 0;
        const INT TilesAcross = RdxMath::CloudTilesAlong(Volume.TargetW());
        const INT TilesDown   = RdxMath::CloudTilesAlong(Volume.TargetH());
        for (INT i = 0; i < Shadow.GetLightCount() && NSeen < FVolumetrics::MAX_LAMPS; i++)
        {
            if (!Shadow.IsLightReady(i) || Shadow.GetSlot(i) < 0)
                continue;
            const FShadowLightFrame* const L = LightAt(i);
            INT Box[4];
            if (!L || L->Bright <= 0.0f || L->Radius <= 1.0f || L->Weight <= 0.0f ||
                !RdxMath::CloudPuffTiles(L->Cam, L->Radius, W.Focal, W.Screen, W.EngPerPixel, TilesAcross, TilesDown,
                                         Box[0], Box[1], Box[2], Box[3]))
                continue;
            Seen[NSeen]  = i;
            Score[NSeen] = L->Bright * Min(L->Weight, 1.0f);
            NSeen++;
        }
        INT   Order[RdxMath::ShaftLampPlaces];
        FLOAT Share[RdxMath::ShaftLampPlaces];
        const INT Picked = RdxMath::ShaftLampPick(Score, NSeen, RdxMath::ShaftLampPlaces, Order, Share);
        ShaftLeftOut = Max(ShaftLeftOut, NSeen - Picked);
        for (INT k = 0; k < Picked; k++)
        {
            if (!(Share[k] > 0.0f))
                continue;
            const INT                      i     = Seen[Order[k]];
            const FShadowLightFrame* const L     = LightAt(i);
            const FLOAT                    Light = Score[Order[k]] * Share[k];
            FLOAT* const                   R     = Lamps + NLamps * FVolumetrics::LAMP_FLOATS;
            R[0] = L->Cam[0]; R[1] = L->Cam[1]; R[2] = L->Cam[2]; R[3] = L->Radius;
            R[4] = L->Colour[0] * Light;
            R[5] = L->Colour[1] * Light;
            R[6] = L->Colour[2] * Light;
            R[7] = (FLOAT)Shadow.GetSlot(i);
            NLamps++;
        }
        if (NLamps > 0)
        {
            ShaftTileUints = BuildShaftTiles(Lamps, NLamps, W);
            if (ShaftTileUints <= 0)
            {
                bShaftListsRefused = 1;
                RdxLog(TEXT("light shafts: the lists for %i lamps could not be made - no shafts from here on"), NLamps);
                NLamps = 0;
            }
        }
        if (NLamps > 0)
        {
            ShaftLampsMost = Max(ShaftLampsMost, NLamps);
            if (++ShaftFrames >= 600)
            {
                RdxLog(TEXT("light shafts: in the last %i frames with them the most lamps a frame had was %i, the "
                            "longest list of a tile %i lamps; the most that could show and were left out: %i"),
                       ShaftFrames, ShaftLampsMost, ShaftTileMost, ShaftLeftOut);
                ShaftFrames = ShaftLampsMost = ShaftTileMost = ShaftLeftOut = 0;
            }
        }
    }
    ShaftLamps      = NLamps;
    W.Lamps         = Lamps;
    W.LampCount     = NLamps;
    W.LampTiles     = ShaftTileData;
    W.LampTileUints = ShaftTileUints;
    W.ShaftSteps    = RdxMath::ShaftStepsDefault;
    W.ShaftStrength = ShaftStrength;
    W.ShaftNear     = Shadow.GetNear();
    W.ShaftMaxZ     = RdxMath::ShaftMaxWorld;
    W.ShaftBias     = RdxMath::ShaftBiasWorld;
    W.ShaftG        = RdxMath::ShaftForward;
    D3D12_SHADER_RESOURCE_VIEW_DESC OwnView;
    OwnBeamsInto(W, OwnView);

    GpuTime(Cmd, 20);
    PutAfterWorldDepth(Cmd);
    // A compute shader reads it, and "pixel shader resource" is not a state a compute shader may read from. Put back
    // afterwards, because everything else in the frame expects it where it was.
    D3D12_RESOURCE_BARRIER D;
    appMemzero(&D, sizeof(D));
    D.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    D.Transition.pResource   = WaterDepth.Get();
    D.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    const D3D12_RESOURCE_STATES DepthWas = WaterDepthState;
    if (DepthWas != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        D.Transition.StateBefore = DepthWas;
        D.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &D);
    }
    // The view's format is NOT the resource's: with the shadows on the depth buffer carries a stencil beside it, and
    // a view that does not say so is a device removed rather than a wrong picture. The same choice the water's own
    // views are made with (EnsureWaterTargets).
    // The shafts read the level's own shadow cubes. FShadowMaps::Render leaves both cube arrays readable by a
    // shader, which is the state a compute pass wants - so nothing has to move here.
    D3D12_SHADER_RESOURCE_VIEW_DESC CubeView;
    appMemzero(&CubeView, sizeof(CubeView));
    if (NLamps > 0)
        Shadow.DescribeCubeView(CubeView);
    // The puffs by tile of the walk's target, for the walk to ask only those a pixel's ray can meet.
    const INT   TilesX    = RdxMath::CloudTilesAlong(Volume.TargetW());
    const INT   TilesY    = RdxMath::CloudTilesAlong(Volume.TargetH());
    const INT   Walked    = Solid.GetCloudsOn() ? CloudPuffCount : 0;
    // A flame has no motion vectors, so a temporal upscaler that reads the reactive mask is told where the fires burn -
    // the mask the splashes write, and only when the upscaler reads it; the upscale takes it from there. The smoke and
    // the waterfalls' columns the same, where a share of their cover is asked to be told (SetWalkReactive).
    const UBOOL bTell = (NFires > 0 || (Walked > 0 && W.SmokeReactive > 0.0f) || (NFalls > 0 && W.FallReactive > 0.0f))
                        ? 1 : 0;
    ID3D12Resource* WalkMask = nullptr;
    if (bTell && Volume.WritesReactive() && UpscaleMode && bFsr3Wanted &&
        ReactiveBegin(Cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        WalkMask = Reactive.Get();
    if (WalkMask && ((Walked > 0 && W.SmokeReactive > 0.0f) || (NFalls > 0 && W.FallReactive > 0.0f)))
    {
        static INT Frames = 0;
        if (Frames++ % 600 == 0)
            RdxLog(TEXT("reactive mask: the walk writes it - %i flame bodies; %i puffs, %.2f of their cover told; %i "
                        "columns, %.2f of theirs"), NFires, Walked, W.SmokeReactive, NFalls, W.FallReactive);
    }
    const INT   TileUints = (Walked > 0) ? BuildCloudTiles(CloudPuffs, CloudPuffCount, CloudFocal, CloudScreen, TilesX,
                                                           TilesY, W.EngPerPixel) : 0;
    const UBOOL bLists    = Volume.Record(Cmd, Device.Get(), WaterDepth.Get(),
                                          bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT,
                                          CloudPuffs, (TileUints > 0) ? Walked : 0, CloudTileData, TileUints, TilesX, W,
                                          FrameIndex, (NLamps > 0) ? Shadow.GetWorldCubes() : nullptr,
                                          (NLamps > 0) ? &CubeView : nullptr, WalkMask);
    OwnStatBeams += (W.OwnCount > 0 && Volume.Wrote()) ? 1 : 0;
    if (Walked > 0 && (TileUints <= 0 || !bLists) && !bCloudListsRefused)
    {
        // This frame's smoke is not there; from the next one the cards are drawn again, and stay.
        bCloudListsRefused = 1;
        Solid.SetCloudsOn(0);
        RdxLog(TEXT("volumetric smoke: the lists for %i puffs could not be made - the game's own flat sprites are "
                    "drawn instead from here on"), Walked);
    }
    // The same for the flames: this frame's are not there, and from the next one the game draws its own.
    if (NFires > 0 && !Volume.FiresHeld())
        RefuseFireLists(TEXT("the walk's list"), NFires);
    // And for the shafts: none this frame, and none from the next one on.
    if (NLamps > 0 && !Volume.LampsHeld() && !bShaftListsRefused)
    {
        bShaftListsRefused = 1;
        RdxLog(TEXT("light shafts: the walk's lists for %i lamps could not be made - no shafts from here on"), NLamps);
    }
    if (DepthWas != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        D.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        D.Transition.StateAfter  = DepthWas;
        Cmd->ResourceBarrier(1, &D);
    }

    // The result, read instead of written, and laid over the scene target alone - the light and added targets belong
    // to the surfaces the shadows darken, and smoke is not a surface. Only what this frame's walk wrote: where it had
    // nothing to walk, the target holds an earlier frame's smoke and light (FVolumetrics::Wrote).
    if (Volume.Wrote())
    {
        D3D12_RESOURCE_BARRIER B;
        appMemzero(&B, sizeof(B));
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Volume.Target();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &B);

        D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
        Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;      // the scene target's view (BindSceneTargets)
        Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
        D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
        // Within the world's view only: in a conversation the walk would otherwise lay its smoke over the black bars
        // above and below the picture, where the engine draws no card either.
        D3D12_RECT Sc = { 0, 0, SceneWidth, SceneHeight };
        if (CloudRect[2] > CloudRect[0] && CloudRect[3] > CloudRect[1] && Width > 0 && Height > 0)
        {
            const FLOAT Kx = (FLOAT)SceneWidth / (FLOAT)Width, Ky = (FLOAT)SceneHeight / (FLOAT)Height;
            Sc.left   = Clamp(appFloor(CloudRect[0] * Kx), 0, SceneWidth);
            Sc.top    = Clamp(appFloor(CloudRect[1] * Ky), 0, SceneHeight);
            Sc.right  = Clamp(appCeil(CloudRect[2] * Kx), (INT)Sc.left, SceneWidth);
            Sc.bottom = Clamp(appCeil(CloudRect[3] * Ky), (INT)Sc.top, SceneHeight);
        }
        Cmd->RSSetViewports(1, &Vp);
        Cmd->RSSetScissorRects(1, &Sc);
        Volume.RecordComposite(Cmd);

        B.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Cmd->ResourceBarrier(1, &B);
    }
    // The air over the fires, once their flames are on the picture: it bends what is behind it, the flames' own tops
    // too. Read through the depth copy as the pixel shaders read it - the state it is back in by now.
    if (bHeatHaze && NFires > 0 && Volume.FiresHeld() && SceneColor &&
        (WaterDepthState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
    {
        D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
        Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;      // the scene target's view (BindSceneTargets)
        D3D12_RECT View = { 0, 0, SceneWidth, SceneHeight };
        if (CloudRect[2] > CloudRect[0] && CloudRect[3] > CloudRect[1] && Width > 0 && Height > 0)
        {
            const FLOAT Kx = (FLOAT)SceneWidth / (FLOAT)Width, Ky = (FLOAT)SceneHeight / (FLOAT)Height;
            View.left   = Clamp(appFloor(CloudRect[0] * Kx), 0, SceneWidth);
            View.top    = Clamp(appFloor(CloudRect[1] * Ky), 0, SceneHeight);
            View.right  = Clamp(appCeil(CloudRect[2] * Kx), (INT)View.left, SceneWidth);
            View.bottom = Clamp(appCeil(CloudRect[3] * Ky), (INT)View.top, SceneHeight);
        }
        Volume.RecordHaze(Cmd, Device.Get(), SceneColor.Get(), Colours.Draw, Rtv, View, W);
        // Each fire's draw cut the picture to its own part: what comes next gets the view's rectangle back, as the
        // composite above leaves it.
        Cmd->RSSetScissorRects(1, &View);
    }
    GpuTime(Cmd, 21);

    // Both passes above bound a view heap of their own. Everything after this reads through the TEXTURE heap and
    // says so only when it changes, so it goes back here - a pass that reads the wrong heap does not fail, it draws
    // whatever happens to sit at the same place in another one.
    ID3D12DescriptorHeap* Back[1] = { Textures.GetHeap() };
    if (Back[0])
        Cmd->SetDescriptorHeaps(1, Back);
    // The fires' flames in the wet floors' mirror (WetReflect.hlsl's PSWetFlames): the late wet pass drawn again, now
    // that this walk has this frame's flames, its picture read where each wet pixel's mirrored ray meets a flame.
    if (bWetDrawnNow && !bWetMsaaNow && NFires > 0 && Volume.Wrote() && Volume.Target() && WetReflectReady > 0 &&
        WetLampBuffer && WetSpotsNow && Back[0] && (WaterDepthState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
    {
        ID3D12Resource* const Flames = Volume.Target();
        D3D12_RESOURCE_BARRIER B;
        appMemzero(&B, sizeof(B));
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Flames;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &B);
        if (WetFlameOf != Flames)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
            appMemzero(&Sd, sizeof(Sd));
            Sd.Format                  = Flames->GetDesc().Format;
            Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Sd.Texture2D.MipLevels     = 1;
            Device->CreateShaderResourceView(Flames, &Sd, Textures.GetCpuHandle(Textures.GetReservedIndex(WetFlameSrv)));
            WetFlameOf = Flames;
        }
        // Over the picture alone, as the walk's own result is laid on: the shadows' targets are past their use by now.
        D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
        Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;      // the scene target's view (BindSceneTargets)
        Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
        WetReflect.Draw(Cmd, Textures.GetHeap(), WetConstNow, Textures.GetGpuHandle(Textures.GetReservedIndex(WetFlameSrv)),
                        Textures.GetGpuHandle(WaterSlot(0, WaterViewDepth)),
                        Textures.GetGpuHandle(Textures.GetReservedIndex(WetMarkSrv)),
                        Textures.GetGpuHandle(Textures.GetReservedIndex(WetCoverSrv)),
                        WetLampBuffer->GetGPUVirtualAddress() + (UINT64)FrameIndex * RdxMath::WetLampBytes, WetSpotsNow,
                        0, 0, 1);
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Cmd->ResourceBarrier(1, &B);
    }
    // Now and then, whether the fires' flames went over the wet floors, and what held them back where they did not.
    static INT FlameAsked = 0;
    if ((FlameAsked++ % 600) == 0)
        RdxLog(TEXT("wet floors: the fires' flames in their mirror - wet pass drawn %i, multisampled %i, %i flame bodies, ")
               TEXT("walked %i, the pass ready %i, the depth copy readable %i"), bWetDrawnNow, bWetMsaaNow, NFires,
               Volume.Wrote() ? 1 : 0, WetReflectReady, (WaterDepthState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) ? 1 : 0);
    bWetDrawnNow = 0;
    // Whatever runs next set its own targets before this; it has to set them again.
    BindSceneTargets(Cmd, Dsv);
}

// Motion blur: the picture smeared along what MOVED in it. At the world's end and after everything else that belongs
// to the world - the smoke included, because smoke moves too and a shutter does not tell it from a wall.
//
// It needs the frame's motion vectors, which are made for the upscaler where the world ends (DrawMotionVectors). A
// frame without them is said once rather than quietly left alone.
void FBackend::DrawMotionBlur(ID3D12GraphicsCommandList* Cmd)
{
    if (!Cmd || !Device || BlurStrength <= 0.0f || bOutputLive || !SceneColor)
        return;
    if (!bMotionDone || !MotionTarget)
    {
        if (!bBlurToldNoVectors)
        {
            bBlurToldNoVectors = 1;
            RdxLog(TEXT("motion blur: this frame has no motion vectors - they are made FOR THE UPSCALER, so with "
                        "AntiAliasing at Off, MSAA or SSAA there is nothing to smear along"));
        }
        return;
    }
    if (BlurReady == 0)
    {
        BlurReady = Smear.Init(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("motion blur: the pass %s"),
               (BlurReady > 0) ? TEXT("up") : TEXT("FAILED - the picture is left as it was"));
    }
    if (BlurReady < 0 || Refused(BlurRefusal))
        return;
    if (!Smear.EnsureTargets(Device.Get(), SceneWidth, SceneHeight, Colours.Storage, RdxCopyBackWriteFormat(Colours)))
    {
        RefuseTarget(BlurRefusal, TEXT("motion blur"));
        return;
    }

    FMotionBlur::FBlur B;
    B.Strength  = BlurStrength;
    B.MaxPixels = RdxMath::BlurMaxPixels;
    B.Taps      = (INT)RdxMath::BlurTaps;
    // The motion target is made at the scene's size, but a size change is not always in step, so the place is scaled
    // rather than assumed.
    B.ScaleX = (SceneWidth  > 0 && MotionW > 0) ? (FLOAT)MotionW / (FLOAT)SceneWidth  : 1.0f;
    B.ScaleY = (SceneHeight > 0 && MotionH > 0) ? (FLOAT)MotionH / (FLOAT)SceneHeight : 1.0f;

    GpuTime(Cmd, 22);
    D3D12_RESOURCE_BARRIER Bar[2];
    appMemzero(Bar, sizeof(Bar));
    for (INT i = 0; i < 2; i++)
    {
        Bar[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Bar[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    // The picture is READ by a compute shader here, and "render target" is not a state one may read from.
    Bar[0].Transition.pResource   = SceneColor.Get();
    Bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Bar[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Bar[1].Transition.pResource   = MotionTarget.Get();
    Bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Bar[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(2, Bar);

    Smear.Record(Cmd, Device.Get(), SceneColor.Get(), Colours.Draw, MotionTarget.Get(),
                 SceneWidth, SceneHeight, B);

    // And back over the picture. A pass never writes into what it is reading, so the blurred copy is made beside it
    // and put back here.
    Bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Bar[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    Bar[1].Transition.pResource   = Smear.Target();
    Bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Bar[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Cmd->ResourceBarrier(2, Bar);
    Cmd->CopyResource(SceneColor.Get(), Smear.Target());

    Bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Bar[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Bar[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Cmd->ResourceBarrier(2, Bar);
    // The motion vectors back where the rest of the frame expects them.
    Bar[0].Transition.pResource   = MotionTarget.Get();
    Bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Bar[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &Bar[0]);
    GpuTime(Cmd, 23);

    // The pass bound a view heap of its own; everything after this reads through the texture heap.
    ID3D12DescriptorHeap* Back[1] = { Textures.GetHeap() };
    if (Back[0])
        Cmd->SetDescriptorHeaps(1, Back);
}

void FBackend::SetConversationFocus(UBOOL bActive, const FLOAT* WorldPos)
{
    bConvFocus = (bActive && WorldPos) ? 1 : 0;
    if (bConvFocus)
        for (INT i = 0; i < 3; i++)
            ConvFocus[i] = WorldPos[i];
}

// Depth of field (DepthOfField.h), in a conversation's camera only. At the world's end, before the motion blur - a lens
// makes the picture that a shutter then smears - and before the HUD, so the letterbox and the subtitles stay sharp.
void FBackend::DrawDepthOfField(ID3D12GraphicsCommandList* Cmd)
{
    // The fade runs whatever is drawn, so a conversation eases in and out instead of snapping.
    const FLOAT Dt   = Clamp(FrameDeltaMs * 0.001f, 0.0f, 0.1f);
    const FLOAT Want = (DofStrength > 0.0f && bConvFocus) ? 1.0f : 0.0f;
    const FLOAT Step = Dt / RdxMath::DofFadeSeconds;
    DofAmount = (Want > DofAmount) ? Min(Want, DofAmount + Step) : Max(Want, DofAmount - Step);
    if (!bConvFocus && DofAmount <= 0.0f)
        bDofFocusValid = 0;
    if (DofAmount <= 0.0f || !Cmd || !Device || bOutputLive || !SceneColor || !bCamSet)
        return;
    // The focus: the speaker's distance along the view axis, which is what the depth measures - set on the face, a
    // little in front of the eyes' place (RdxMath::DofFocusFor). A new speaker is followed over a quarter of a
    // second, not jumped to - a focus pull.
    if (bConvFocus)
    {
        const FLOAT Along = (ConvFocus[0] - CamEye[0]) * CamAxes[6] + (ConvFocus[1] - CamEye[1]) * CamAxes[7] +
                            (ConvFocus[2] - CamEye[2]) * CamAxes[8];
        const FLOAT Target = RdxMath::DofFocusFor(Along, DofLead);
        DofAlong = Along;
        if (!bDofFocusValid)
            DofFocus = Target;
        else
            DofFocus += (Target - DofFocus) * (1.0f - appExp(-Dt / RdxMath::DofFocusSeconds));
        bDofFocusValid = 1;
    }
    // The depth has to be readable, and single-sampled: with MSAA, or with neither the shadows nor an upscaler asking
    // for a readable depth, there is none, and this says so once.
    if (!NeedsDepthRead() || !DepthBuffer || !DepthBuffer2 || MsaaOn())
    {
        if (!bDofToldNoDepth)
        {
            bDofToldNoDepth = 1;
            RdxLog(TEXT("depth of field: this setting gives it no depth to read (MSAA, or neither the shadows nor an "
                        "upscaler) - skipped"));
        }
        return;
    }
    if (DofReady == 0)
    {
        DofReady = Dof.Init(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("depth of field: the pass %s"),
               (DofReady > 0) ? TEXT("up") : TEXT("FAILED - the picture is left as it was"));
    }
    if (DofReady < 0 || Refused(DofRefusal))
        return;
    if (!Dof.EnsureTargets(Device.Get(), SceneWidth, SceneHeight, Colours.Storage, RdxCopyBackWriteFormat(Colours)))
    {
        RefuseTarget(DofRefusal, TEXT("depth of field"));
        return;
    }

    FDepthOfField::FParams P;
    P.Focus  = DofFocus;
    P.MaxCoc = RdxMath::DofMaxCocOfHeight * (FLOAT)SceneHeight * DofStrength;
    P.Band   = DofBand;
    P.Amount = DofAmount;
    if ((DofFrames++ % 300) == 0)
        RdxLog(TEXT("depth of field: focus %.1f units along the view, the speaker's eyes' place at %.1f, the "
                    "largest circle %.1f pixels, the sharp band %.3f, at %.0f%%"),
               P.Focus, DofAlong, P.MaxCoc, P.Band, P.Amount * 100.0f);

    ID3D12Resource* const Depth = (WorldDepthIndex() == 1) ? DepthBuffer2.Get() : DepthBuffer.Get();
    GpuTime(Cmd, 27);
    D3D12_RESOURCE_BARRIER Bar[2];
    appMemzero(Bar, sizeof(Bar));
    for (INT i = 0; i < 2; i++)
    {
        Bar[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Bar[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    Bar[0].Transition.pResource   = SceneColor.Get();
    Bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Bar[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Bar[1].Transition.pResource   = Depth;
    Bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    Bar[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(2, Bar);

    Dof.Record(Cmd, Device.Get(), SceneColor.Get(), Colours.Draw, Depth,
               bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT, SceneWidth, SceneHeight, P);

    // Back over the picture, as the motion blur does; and the depth back to being the world's.
    Bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Bar[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    Bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Bar[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    Cmd->ResourceBarrier(2, Bar);
    Bar[1].Transition.pResource   = Dof.Target();
    Bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Bar[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Cmd->ResourceBarrier(1, &Bar[1]);
    Cmd->CopyResource(SceneColor.Get(), Dof.Target());
    Bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Bar[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Bar[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Cmd->ResourceBarrier(2, Bar);
    GpuTime(Cmd, 28);

    // The pass bound a view heap of its own; everything after this reads through the texture heap.
    ID3D12DescriptorHeap* Back[1] = { Textures.GetHeap() };
    if (Back[0])
        Cmd->SetDescriptorHeaps(1, Back);
}

void FBackend::NoteCorona(INT Srv, FLOAT X0, FLOAT Y0, FLOAT X1, FLOAT Y1, FLOAT U0, FLOAT V0, FLOAT U1, FLOAT V1,
                          FLOAT R, FLOAT G, FLOAT B)
{
    if (!bInFrame || Srv < 0 || BloomCoronaCount >= MAX_BLOOM_CORONAS || Width <= 0 || Height <= 0)
        return;
    // Into clip space the way the solid pass takes the engine's pixels: over the OUTPUT's size, whatever size the
    // picture is drawn at (SolidPass.hlsl, VSMain).
    FBloomCorona& K = BloomCoronas[BloomCoronaCount++];
    K.X0 = X0 * 2.0f / (FLOAT)Width - 1.0f;
    K.Y0 = 1.0f - Y0 * 2.0f / (FLOAT)Height;
    K.X1 = X1 * 2.0f / (FLOAT)Width - 1.0f;
    K.Y1 = 1.0f - Y1 * 2.0f / (FLOAT)Height;
    K.U0 = U0;
    K.V0 = V0;
    K.U1 = U1;
    K.V1 = V1;
    K.R  = R;
    K.G  = G;
    K.B  = B;
    K.Texture = Textures.GetGpuHandle(Srv);
}

// The lens and the film, on the finished picture: the colours split towards the corners, the corners darkened, and
// grain. Whatever the HUD is about to be drawn onto is what this works on - the output target where there is one, the
// scene target where the picture is already the right size - so the HUD itself never gets any of it.
void FBackend::DrawFilmLook(ID3D12GraphicsCommandList* Cmd)
{
    if (!Cmd || !Device)
        return;
    if (!FilmLookWanted())
        return;
    ID3D12Resource* const Picture = HasOutputTarget() ? OutputColor.Get() : SceneColor.Get();
    const INT W = HasOutputTarget() ? Width : SceneWidth;
    const INT H = HasOutputTarget() ? Height : SceneHeight;
    if (!Picture || W <= 1 || H <= 1)
        return;
    if (FilmReady == 0)
    {
        FilmReady = Film.Init(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("film look: the pass %s"),
               (FilmReady > 0) ? TEXT("up") : TEXT("FAILED - the picture is left as it was"));
    }
    if (FilmReady < 0 || Refused(FilmRefusal))
        return;
    if (!Film.EnsureTargets(Device.Get(), W, H, Colours.Storage, RdxCopyBackWriteFormat(Colours)))
    {
        RefuseTarget(FilmRefusal, TEXT("film look"));
        return;
    }

    FFilmLook::FLook L;
    L.Aberration = LookAberration;
    L.Vignette   = LookVignette;
    L.Grain      = LookGrain;
    L.Bloom      = LookBloom;
    L.BloomCoronas = LookBloomCoronas;
    L.Flare      = LookFlare;
    L.Frame      = ++FilmFrame;

    GpuTime(Cmd, 24);
    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 2; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    B[0].Transition.pResource   = Picture;
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &B[0]);

    // The halo FIRST, because the pass below adds it while it already has the whole picture in hand. It reads
    // the same picture, in the same state, so nothing has to move in between.
    ID3D12Resource* Bloomed       = nullptr;
    ID3D12Resource* CoronaBloomed = nullptr;
    ID3D12Resource* Ghosts        = nullptr;
    if (LookBloom > 0.0f || LookBloomCoronas > 0.0f || LookFlare > 0.0f)
    {
        if (BloomReady == 0)
        {
            BloomReady = Halo.Init(Device.Get()) ? 1 : -1;
            RdxLog(TEXT("bloom: the passes %s"),
                   (BloomReady > 0) ? TEXT("up") : TEXT("FAILED - the picture keeps its own brightness"));
        }
        const UBOOL bHaloAsked = (BloomReady > 0 && !Refused(BloomRefusal)) ? 1 : 0;
        if (bHaloAsked && Halo.EnsureTargets(Device.Get(), W, H))
        {
            // The coronas' halo and the lens's ghosts are made of the frame's coronas alone, drawn small into the
            // bloom's own picture first - and with none on the screen there is neither; the picture's halo is made
            // all the same. That draw leaves its own target, viewport and scissor set; SwitchToOutput binds the
            // targets again, the other two are put back here.
            const UBOOL bCoronas = (BloomCoronaCount > 0 && (LookBloomCoronas > 0.0f || LookFlare > 0.0f)) ? 1 : 0;
            // The own lamps throw the lens's ghosts too (MakeLampFlares), but no part of the coronas' halo.
            const UBOOL bLampGhosts = (BloomLampCount > 0 && LookFlare > 0.0f) ? 1 : 0;
            if (bCoronas)
            {
                Halo.DrawCoronas(Cmd, Textures.GetHeap(), BloomCoronas, BloomCoronaCount);
                D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)W, (FLOAT)H, 0.0f, 1.0f };
                D3D12_RECT     Sc = { 0, 0, (LONG)W, (LONG)H };
                Cmd->RSSetViewports(1, &Vp);
                Cmd->RSSetScissorRects(1, &Sc);
            }
            const FBloom::FTake PictureTake = { RdxMath::BloomThreshold, RdxMath::BloomSpread, 1 };
            const FBloom::FTake CoronaTake  = { RdxMath::BloomCoronaThreshold, RdxMath::BloomCoronaSpread,
                                                RdxMath::BloomCoronaRounds };
            const FBloom::FTake GhostTake   = { RdxMath::FlareCoronaLine, RdxMath::FlareSpread, 1 };
            Halo.Record(Cmd, Device.Get(), Picture, Colours.Draw, W, H,
                        (LookBloom > 0.0f) ? &PictureTake : nullptr,
                        (bCoronas && LookBloomCoronas > 0.0f) ? &CoronaTake : nullptr,
                        ((bCoronas || bLampGhosts) && LookFlare > 0.0f) ? &GhostTake : nullptr,
                        bLampGhosts ? BloomLamps : nullptr, bLampGhosts ? BloomLampCount : 0, bCoronas);
            Bloomed       = Halo.Target();
            CoronaBloomed = Halo.CoronaTarget();
            Ghosts        = Halo.GhostTarget();
        }
        else if (bHaloAsked)
            RefuseTarget(BloomRefusal, TEXT("bloom"));
    }

    Film.Record(Cmd, Device.Get(), Picture, Colours.Draw, Bloomed, CoronaBloomed, Ghosts, W, H, L);

    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    B[1].Transition.pResource   = Film.Target();
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Cmd->ResourceBarrier(2, B);
    Cmd->CopyResource(Picture, Film.Target());

    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Cmd->ResourceBarrier(2, B);
    GpuTime(Cmd, 25);

    ID3D12DescriptorHeap* Back[1] = { Textures.GetHeap() };
    if (Back[0])
        Cmd->SetDescriptorHeaps(1, Back);
}

void FBackend::SetAmbientOcclusion(INT Mode, UBOOL bAlone)
{
    const RdxMath::EAmbientOcclusion M = (Mode > RdxMath::Ao_Off && Mode < RdxMath::Ao_Count)
                                         ? (RdxMath::EAmbientOcclusion)Mode : RdxMath::Ao_Off;
    const UBOOL A = (M != RdxMath::Ao_Off && bAlone) ? 1 : 0;
    if (M == AoMode && A == bAoAlone)
        return;
    AoMode   = M;
    bAoAlone = A;
    TCHAR Name[16];
    RdxMath::AoName(M, Name, 16);
    RdxLog(TEXT("ambient occlusion: %s%s%s"), Name,
           A ? TEXT(", alone - in place of the shadows, to show what it does by itself") : TEXT(""),
           (M != RdxMath::Ao_Off && !bShadows)
               ? TEXT(" - but the shadows are off, and it darkens THEIR factor, so nothing shows until they are on")
               : TEXT(""));
}

void FBackend::ApplyShadows(ID3D12GraphicsCommandList* Cmd)
{
    if (!bShadows || !Shadow.IsReady() || Shadow.GetLightCount() <= 0 || !DepthBuffer2 || !ShadowSrvHeap || !LightTarget ||
        !AddedTarget)
        return;
    if (ShadowReady == 0)
    {
        const DXGI_FORMAT DrawFmt = Colours.Draw;
        ShadowReady = Final.InitShadow(Device.Get(), DrawFmt) ? 1 : -1;
        RdxLog(TEXT("shadows: both pipelines %s"),
               (ShadowReady > 0) ? TEXT("up") : TEXT("FAILED - shadows are skipped, the game draws its own blob again"));
    }
    if (ShadowReady < 0 || Refused(ShadowRefusal))
        return;
    if (!EnsureShadowTarget())
    {
        // The shadows are off while the sizes stand, and the engine's blob shadow comes back (WantsShadows).
        RefuseTarget(ShadowRefusal, TEXT("shadows"));
        return;
    }

    // The RT passes (RequestRtShadows, RequestRtAo): the factor from rays against the scene, no maps drawn; the
    // occlusion's raw pass from rays. Both run in the 64-bit host (RtHostPasses), on copies of what they read here,
    // and the frame is split where they are traced (FHostRayFeed) - each where the host has the pass for it, and
    // with the scene listed this frame; otherwise the maps and the SSAO.
    const UINT HostPasses = (Cmd == CommandList.Get()) ? RtHostPasses() : 0;
    UBOOL bRtFactor    = ((bRtShadows || bRtShadowSetting) && (HostPasses & Host64::RayPass_Shadow) != 0) ? 1 : 0;
    UBOOL bRtOcclusion = (IsRtAoRequested() && (HostPasses & Host64::RayPass_Ao) != 0) ? 1 : 0;

    // The motion vectors first, while the world's depth is still whole. At the frame's end it is not: the HUD's own
    // clear has taken it by then - measured, where even the depth view showed "nothing drawn" everywhere.
    DrawMotionVectors(Cmd);

    // The see-through water's own shadows: where this frame's water wrote its shade (WaterShade), the factor of the
    // surface's own place is drawn beside the world's (WaterFactor) - by the maps, or by the rays where the host has
    // the pass for it - and the apply pass gives each part of a water pixel the shadow of its own place.
    UBOOL bWaterShadow = (!bWaterShadowMarkOff && bWaterShadeLive && WaterSurface && WaterShade && EnsureWaterFactor())
                         ? 1 : 0;
    if (bWaterShadow)
    {
        WaterSurfaceTo(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        WaterShadeTo(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (bWaterShadowViewsDirty)
        WriteWaterShadowViews();
    // The pack first (WaterPack): the water's own depth and light at the factor's samples, for the surface's factor to
    // read - here, or in the host, which is handed it below.
    if (bWaterShadow)
    {
        if (WaterPackReady == 0)
        {
            WaterPackReady = Final.InitWaterPack(Device.Get()) ? 1 : -1;
            if (WaterPackReady < 0)
                RdxLog(TEXT("water shadow: the pack's pipeline FAILED - the see-through water takes the shadow of what ")
                       TEXT("lies under it"));
        }
        bWaterShadow = (WaterPackReady > 0) ? 1 : 0;
    }
    if (bWaterShadow)
    {
        D3D12_RESOURCE_BARRIER PB;
        SplashBarrier(PB, WaterPack.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Cmd->ResourceBarrier(1, &PB);
        D3D12_CPU_DESCRIPTOR_HANDLE PackRtv = WaterFactorRtvHeap->GetCPUDescriptorHandleForHeapStart();
        PackRtv.ptr += Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        Cmd->OMSetRenderTargets(1, &PackRtv, FALSE, nullptr);
        const D3D12_VIEWPORT PackVp = { 0.0f, 0.0f, (FLOAT)WaterPackW, (FLOAT)WaterPackH, 0.0f, 1.0f };
        const D3D12_RECT     PackSc = { 0, 0, WaterPackW, WaterPackH };
        Cmd->RSSetViewports(1, &PackVp);
        Cmd->RSSetScissorRects(1, &PackSc);
        D3D12_GPU_DESCRIPTOR_HANDLE PackIn = ShadowSrvHeap->GetGPUDescriptorHandleForHeapStart();
        PackIn.ptr += (UINT64)SHADOW_WATER_PACK_IN *
                      Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Final.DrawWaterPack(Cmd, ShadowSrvHeap.Get(), PackIn,
                            (UINT)(WaterPackCoarse * Max(1, (INT)(UpscaleMode ? ShadowFactorDivisor()
                                                                               : (SceneScale * ShadowFactorDivisor())))));
        SplashBarrier(PB, WaterPack.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Cmd->ResourceBarrier(1, &PB);
    }
    UBOOL bRtSurfaceTraced = 0;

    // The maps first: they bind targets of their own, and read the masks' view (SHADOW_MASK_SRV). For the RT factor the
    // scene instead, listed from the same triangles the maps are drawn from (FRtPass::ListScene); should it not be
    // listed this frame, the maps after all.
    GpuTime(Cmd, 1);
    const INT World = WorldDepthIndex();
    FHostRayFeed::FViews HostViews;
    appMemzero(&HostViews, sizeof(HostViews));
    UBOOL bScene = 0;
    if (bRtFactor || bRtOcclusion)
    {
        // The host's scene: the frame's bodies are listed here and the structures made there - once the share for this
        // level and these sizes is taken (FHostRayFeed::Prepare); the maps and the SSAO until then. What the passes read
        // of the frame's own is named as it stands where they are fed, below: readable by a pixel shader.
        HostRayViews(HostViews, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (!bRtSceneTried)
        {
            RtHostBodies  = Rt.ListScene((INT)FrameIndex, Shadow);
            bRtSceneBuilt = RtHostBodies >= 0 &&
                            RayFeed.Prepare(Device.Get(), Host.GetLink(), Rt, Shadow, HostViews, RtHostBodies);
            bRtSceneTried = 1;
        }
        bScene = bRtSceneBuilt;
    }
    bRtFactor    = bRtFactor && bScene;
    bRtOcclusion = bRtOcclusion && bScene;
    D3D12_GPU_DESCRIPTOR_HANDLE MaskSrv = ShadowSrvHeap->GetGPUDescriptorHandleForHeapStart();
    MaskSrv.ptr += SHADOW_MASK_SRV * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // The light shafts ask the maps themselves, at every step of their walk (DrawVolumetrics): where they are switched
    // on, the maps are drawn beside the RT factor too, for the shafts alone.
    if (!bRtFactor || ShaftStrength > 0.0f)
    {
        Shadow.Render(Cmd, ShadowSrvHeap.Get(), MaskSrv);
        bCubesDrawn = 1;              // and so the light shafts may ask them
    }
    else
        Shadow.UploadMasks(Cmd);      // no maps, but the rays test the masks
    GpuTime(Cmd, 2);

    // Read: both depth buffers and the light target. Drawn into: the factor target.
    D3D12_RESOURCE_BARRIER B[4];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 4; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    B[0].Transition.pResource   = DepthBuffer.Get();
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[1] = B[0];
    B[1].Transition.pResource   = DepthBuffer2.Get();
    B[2].Transition.pResource   = ShadowColor.Get();
    B[2].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[2].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[3].Transition.pResource   = LightTarget.Get();
    B[3].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[3].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(4, B);

    D3D12_CPU_DESCRIPTOR_HANDLE FactorRtv = ShadowRtvHeap->GetCPUDescriptorHandleForHeapStart();
    Cmd->OMSetRenderTargets(1, &FactorRtv, FALSE, nullptr);
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)ShadowW, (FLOAT)ShadowH, 0.0f, 1.0f };
    D3D12_RECT     Sc = { 0, 0, ShadowW, ShadowH };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);

    D3D12_GPU_VIRTUAL_ADDRESS Constants = 0;
    BYTE* const Mapped = Shadow.GetLookupConstants((INT)FrameIndex, Constants);
    INT Order[RdxMath::ShadowMaxLights];
    INT Ordered = 0;
    if (Mapped)
        FillShadowConstants(Mapped, bRtFactor, Order, Ordered);
    const UINT Inc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE Table = ShadowSrvHeap->GetGPUDescriptorHandleForHeapStart();
    if (World == 1)
        Table.ptr += SHADOW_TABLE * Inc;
    // The rays' own constants and table, written where the host's passes are fed them from
    // (FRtPass::FillShadowExclusion): the lights' excluded geometries, the geometries' places, the occlusion's two
    // numbers.
    D3D12_GPU_VIRTUAL_ADDRESS RayParams = 0;
    if (bRtFactor || bRtOcclusion)
    {
        // The factor's two costs - RdxMath's, or with the bench (RequestRtShadows' Mode 5) a row of this table every
        // RT_BENCH_FRAMES frames, the first row the defaults: lights a pixel, samples an edge.
        INT Traced = (RtKnobLights > 0) ? RtKnobLights : RdxMath::RtShadowTracedLights;
        INT Edges  = (RtKnobLights > 0) ? RtKnobEdges : RdxMath::RtShadowEdgeSamples;
        if (bRtShadowBench && bRtFactor)
        {
            enum { RT_BENCH_FRAMES = 300, RT_BENCH_ROWS = 7 };
            static const INT Bench[RT_BENCH_ROWS][2] = { { 6, 4 }, { 3, 4 }, { 6, 2 }, { 6, 0 }, { 2, 0 }, { 8, 4 }, { 6, 8 } };
            const INT Row = (RtBenchFrames / RT_BENCH_FRAMES) % RT_BENCH_ROWS;
            Traced = Bench[Row][0];
            Edges  = Bench[Row][1];
            if (RtBenchFrames % RT_BENCH_FRAMES == 0)
                RdxLog(TEXT("ray tracing (bench): %i lights a pixel, %i samples an edge - from here, for %i frames"),
                       Traced, Edges, (INT)RT_BENCH_FRAMES);
            RtBenchFrames++;
        }
        RayParams = Rt.FillShadowExclusion((INT)FrameIndex, Shadow, Order, bRtFactor ? Ordered : 0,
                                           (AoRadiusSet > 0.0f) ? AoRadiusSet : RdxMath::AoRadius,
                                           (AoStrengthSet > 0.0f) ? AoStrengthSet : RdxMath::RtAoStrength,
                                           Traced, Edges, RtAoInstances);
    }
    // The host's rays. Everything the two passes read - written just above, or drawn by now - is copied into what is
    // shared with the host, the frame's list is filled, and the frame is split: what was recorded so far is executed, the
    // host's queue makes the scene and traces, and this list goes on after it with nothing set on it - so the factor's
    // target and its view are set again here. Should the link go down on the way, the frame goes on with the maps' factor
    // and the SSAO: for that one frame of maps that were not drawn, where the rays were to cast alone.
    if (bRtFactor || bRtOcclusion)
    {
        // The water surface's factor with the world's, where the host has the pass and the share its textures.
        const UBOOL bRtSurface = (bRtFactor && bWaterShadow && (HostPasses & Host64::RayPass_Surface) != 0 &&
                                  RayFeed.HasSurface()) ? 1 : 0;
        if (bRtSurface)
        {
            HostViews.SurfacePack  = WaterPack.Get();
            HostViews.SurfaceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        const UINT Passes = (bRtFactor ? (UINT)Host64::RayPass_Shadow : 0u) |
                            (bRtOcclusion ? (UINT)Host64::RayPass_Ao : 0u) |
                            (bRtSurface ? (UINT)Host64::RayPass_Surface : 0u);
        GpuTime(Cmd, 18);
        bRtHostTraced = Mapped && RayParams &&
                        RayFeed.Feed(Cmd, Host.GetLink(), Rt, Shadow, HostViews, (INT)FrameIndex, RtHostBodies, Passes) &&
                        (NoteFrameSubmit(1), RayFeed.Handover(Cmd, Allocators[FrameIndex], Queue.Get(), Host.GetLink()));
        if (bRtHostTraced)
        {
            bRtSurfaceTraced = bRtSurface;
            Cmd->OMSetRenderTargets(1, &FactorRtv, FALSE, nullptr);
            Cmd->RSSetViewports(1, &Vp);
            Cmd->RSSetScissorRects(1, &Sc);
        }
        else
            bRtFactor = bRtOcclusion = 0;
        GpuTime(Cmd, 19);
    }
    // The RT factor into a target of its own first, then smoothed into the factor target (FFinalPass::DrawRtFactorBlur):
    // a pixel's few samples of a light's disc make a grainy soft edge alone. Should the smoothing not be had, straight
    // into the factor as the rays gave it.
    // The target is one of the occlusion's (EnsureAoTarget), so a refusal here is the occlusion's as well, and the
    // other way round.
    UBOOL bRtSmooth = 0;
    if (bRtFactor && AoSrvHeap && !Refused(AoRefusal))
    {
        if (!EnsureAoTarget())
            RefuseTarget(AoRefusal, TEXT("ambient occlusion and the RT factor's smoothing"));
        else
        {
            if (RtFactorBlurReady == 0)
            {
                RtFactorBlurReady = Final.InitRtFactorBlur(Device.Get()) ? 1 : -1;
                RdxLog(TEXT("shadows: the RT factor's smoothing %s"),
                       (RtFactorBlurReady > 0) ? TEXT("up") : TEXT("FAILED - the factor goes on as the rays give it"));
            }
            bRtSmooth = (RtFactorBlurReady > 0);
        }
    }
    D3D12_RESOURCE_BARRIER RawB;
    appMemzero(&RawB, sizeof(RawB));
    RawB.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    RawB.Transition.pResource   = RtFactorRaw.Get();
    RawB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (bRtSmooth)
    {
        RawB.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        RawB.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Cmd->ResourceBarrier(1, &RawB);
        D3D12_CPU_DESCRIPTOR_HANDLE RawRtv = AoRtvHeap->GetCPUDescriptorHandleForHeapStart();
        RawRtv.ptr += 2 * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        Cmd->OMSetRenderTargets(1, &RawRtv, FALSE, nullptr);
    }
    // The host's factor is there already, in what it shares: copied into the target the maps' pass draws into.
    if (bRtFactor && bRtHostTraced)
        RayFeed.CopyFactor(Cmd, bRtSmooth ? RtFactorRaw.Get() : ShadowColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    else
        Final.DrawShadowFactor(Cmd, ShadowSrvHeap.Get(), Table, Mapped ? Constants : 0);
    if (bRtSmooth)
    {
        RawB.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        RawB.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &RawB);
        Cmd->OMSetRenderTargets(1, &FactorRtv, FALSE, nullptr);
        UINT AoConstants[RdxMath::AoCbCount];
        FillAoConstants(AoConstants);
        D3D12_GPU_DESCRIPTOR_HANDLE AoTable = AoSrvHeap->GetGPUDescriptorHandleForHeapStart();
        if (World == 1)
            AoTable.ptr += AO_TABLE * Inc;
        Final.DrawRtFactorBlur(Cmd, AoSrvHeap.Get(), AoTable, AoConstants);
    }
    GpuTime(Cmd, 3);
    // The compensation's sample is taken HERE, before the occlusion joins the factor, so the metric says what the
    // SHADOWS took and nothing else.
    //
    // What the controller does, once the lamps are at their ceiling, is hold the MEASURED factor average at a fixed
    // value (1 - 1/BoostMax, which is 0.667 for the SDR ceiling): it eases the shadows until the average gets there.
    // With the occlusion inside that average, the occlusion spends part of the same budget and the shadows are eased
    // to make room for it. That converges - it is not a runaway - but it means switching the occlusion on quietly
    // weakens the shadows.
    //
    // Measured on the RUG save, the same scene and the same holds both ways (docs/ejszakai-naplo.md, 2026-09-23):
    //
    //   sample AFTER the occlusion    shadow strength 0.867 -> 0.743 (SSAO) -> 0.734 (RT); picture -1.1% / -0.5%
    //   sample BEFORE it              shadow strength 0.867 -> 0.871       -> 0.869;       picture -1.5% / -2.5%
    //
    // So the old placement bought about one percent of picture brightness with thirteen points of shadow strength.
    // Two percent of overall brightness is below what an eye finds; thirteen points of shadow is not. And a quality
    // setting that silently moves because a different feature was switched on is the wrong kind of surprise.
    SampleShadowMetric(Cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // The ambient occlusion goes INTO the factor here, before anything reads it - the apply below.
    // The two points around it are placed whether it runs or not (Backend.h, the GPU timer).
    GpuTime(Cmd, 10);
    DrawAmbientOcclusion(Cmd, World, bRtOcclusion && bRtHostTraced);
    GpuTime(Cmd, 11);

    // The water surface's factor: the host's, copied where the rays traced it; with the maps, drawn here from the
    // water's own depth - the factor's six views from the run's third on. Where neither is had, the apply pass is told,
    // and the water takes the shadow of what lies under it.
    if (bWaterShadow)
    {
        if (bRtFactor)
            bWaterShadow = (bRtSurfaceTraced &&
                            RayFeed.CopySurfaceFactor(Cmd, WaterFactor.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
                         ? 1 : 0;
        else if (Mapped)
        {
            D3D12_RESOURCE_BARRIER WB;
            SplashBarrier(WB, WaterFactor.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Cmd->ResourceBarrier(1, &WB);
            D3D12_CPU_DESCRIPTOR_HANDLE WaterRtv = WaterFactorRtvHeap->GetCPUDescriptorHandleForHeapStart();
            Cmd->OMSetRenderTargets(1, &WaterRtv, FALSE, nullptr);
            const D3D12_VIEWPORT WaterVp = { 0.0f, 0.0f, (FLOAT)WaterFactorW, (FLOAT)WaterFactorH, 0.0f, 1.0f };
            const D3D12_RECT     WaterSc = { 0, 0, WaterFactorW, WaterFactorH };
            Cmd->RSSetViewports(1, &WaterVp);
            Cmd->RSSetScissorRects(1, &WaterSc);
            D3D12_GPU_DESCRIPTOR_HANDLE WaterTable = ShadowSrvHeap->GetGPUDescriptorHandleForHeapStart();
            WaterTable.ptr += (UINT64)SHADOW_WATER_FACTOR_TABLE * Inc;
            Final.DrawShadowFactor(Cmd, ShadowSrvHeap.Get(), WaterTable, Constants);
            SplashBarrier(WB, WaterFactor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Cmd->ResourceBarrier(1, &WB);
        }
        else
            bWaterShadow = 0;
    }

    // The depth buffers stay readable through the apply pass, which reads them for the water's pixels.
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B[2].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[2].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[3].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[3].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Cmd->ResourceBarrier(2, &B[2]);

    D3D12_CPU_DESCRIPTOR_HANDLE SceneRtv = RtvStart;
    SceneRtv.ptr += BACKEND_FRAME_COUNT * RtvStride;
    Cmd->OMSetRenderTargets(1, &SceneRtv, FALSE, nullptr);
    D3D12_VIEWPORT SceneVp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    D3D12_RECT     SceneSc = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &SceneVp);
    Cmd->RSSetScissorRects(1, &SceneSc);
    // The apply pass reads the world's run of the water's views whole: the factor and the added target first.
    D3D12_GPU_DESCRIPTOR_HANDLE ApplySrv = ShadowSrvHeap->GetGPUDescriptorHandleForHeapStart();
    ApplySrv.ptr += (UINT64)(SHADOW_WATER_RUN + World * SHADOW_WATER_RUN_VIEWS) * Inc;
    // The added target beside the factor (SHADOW_ADDED_SRV): readable for the apply pass, a render target again after it.
    D3D12_RESOURCE_BARRIER AB;
    appMemzero(&AB, sizeof(AB));
    AB.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    AB.Transition.pResource   = AddedTarget.Get();
    AB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    AB.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    AB.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &AB);
    Final.DrawShadowApply(Cmd, ShadowSrvHeap.Get(), ApplySrv, 1.0f / (FLOAT)SceneWidth, 1.0f / (FLOAT)SceneHeight,
                          bWaterShadow ? 1.0f : 0.0f, (WorldDepthIndex() != ActiveDepth) ? 0.0f : 1.0f);
    AB.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    AB.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Cmd->ResourceBarrier(1, &AB);
    Cmd->ResourceBarrier(2, B);
    // Whether the water's own shadows ran: counted, said at the first frame and every 600th after it.
    static INT WaterShadowFrames = 0, WaterShadowMissed = 0;
    if (bWaterShadow && (WaterShadowFrames++ % 600) == 0)
        RdxLog(TEXT("water shadow: %i frames so far with the see-through water's own shadows - its surface's factor by ")
               TEXT("the %s"), WaterShadowFrames, bRtFactor ? TEXT("rays") : TEXT("maps"));
    if (bWaterShadeLive && !bWaterShadow && !bWaterShadowMarkOff && (WaterShadowMissed++ % 600) == 0)
        RdxLog(TEXT("water shadow: %i frames so far with the water's shade but no factor of its surface - the water ")
               TEXT("takes the shadow of what lies under it"), WaterShadowMissed);
    GpuTime(Cmd, 4);
}

// GPU time by pass (see Backend.h): the query heap and its readback buffer, mapped for as long as it lives. Should either
// fail, the timer is off - it only writes the log.
void FBackend::CreateGpuTimer()
{
    TimeMapped = nullptr;
    for (INT s = 0; s < BACKEND_FRAME_COUNT; s++)
    {
        TimePlaced[s] = 0;
        TimeLive[s]   = 0;
    }
    LARGE_INTEGER Freq;
    TimeTick = (::QueryPerformanceFrequency(&Freq) && Freq.QuadPart > 0) ? 1.0 / (DOUBLE)Freq.QuadPart : 0.0;
    D3D12_QUERY_HEAP_DESC Q;
    appMemzero(&Q, sizeof(Q));
    Q.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    Q.Count = BACKEND_FRAME_COUNT * GPU_TIME_POINTS;
    if (FAILED(Device->CreateQueryHeap(&Q, __uuidof(ID3D12QueryHeap), (void**)TimeHeap.Slot())))
    {
        TimeHeap.Release();
        RDX_LOG("gpu timing: the timestamp queries could NOT be created - timing is off");
        return;
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC R;
    appMemzero(&R, sizeof(R));
    R.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    R.Width            = (UINT64)Q.Count * sizeof(UINT64);
    R.Height           = 1;
    R.DepthOrArraySize = 1;
    R.MipLevels        = 1;
    R.SampleDesc.Count = 1;
    R.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &R, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               __uuidof(ID3D12Resource), (void**)TimeReadback.Slot())) ||
        FAILED(TimeReadback->Map(0, nullptr, (void**)&TimeMapped)) || !TimeMapped)
    {
        TimeMapped = nullptr;
        TimeReadback.Release();
        TimeHeap.Release();
        RDX_LOG("gpu timing: the read-back buffer could NOT be created - timing is off");
        return;
    }
    RDX_LOGF("gpu timing: up - %i timestamps per frame, logged every %i frames", (INT)GPU_TIME_POINTS, (INT)GPU_TIME_LOG_FRAMES);
}

void FBackend::GpuTime(ID3D12GraphicsCommandList* Cmd, INT Point)
{
    if (!TimeHeap || !TimeMapped || !Cmd || Point < 0 || Point >= GPU_TIME_POINTS || FrameIndex >= BACKEND_FRAME_COUNT)
        return;
    if (Point == 0)
    {
        TimePlaced[FrameIndex] = 0;
        TimeRound              = 0;
    }
    Cmd->EndQuery(TimeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, FrameIndex * GPU_TIME_POINTS + (UINT)Point);
    TimePlaced[FrameIndex] |= (UINT64)1 << Point;
}

// A set's times, when its allocator is reset (BeginFrame): each part whose two points were ended in their place.
void FBackend::ReadGpuTime(UINT Slot)
{
    const DOUBLE CpuNow = appSeconds();
    if (TimeCpuLast > 0.0)
    {
        TimeCpuGapSum += CpuNow - TimeCpuLast;
        TimeCpuGaps++;
    }
    TimeCpuLast = CpuNow;
    if (!TimeMapped || Slot >= BACKEND_FRAME_COUNT || !TimeLive[Slot] || TimeTick <= 0.0)
        return;
    TimeLive[Slot] = 0;
    const UINT64* const T      = TimeMapped + Slot * GPU_TIME_POINTS;
    const UINT64        Placed = TimePlaced[Slot];
    // The input latency measurement's: the moments the GPU began this frame and was through it (its points 0 and 5),
    // brought onto the CPU's clock by the queue's own calibration, beside the moment the frame's Present returned.
    if (bLatencyStamps && LatencySlotPresent[Slot] && ((Placed >> 5) & 1) && (Placed & 1))
    {
        UINT64        GpuFreq = 0, GpuNow = 0, CpuNow = 0;
        LARGE_INTEGER QpcFreq;
        if (SUCCEEDED(Queue->GetTimestampFrequency(&GpuFreq)) && GpuFreq &&
            SUCCEEDED(Queue->GetClockCalibration(&GpuNow, &CpuNow)) && ::QueryPerformanceFrequency(&QpcFreq))
        {
            const DOUBLE Scale = (DOUBLE)QpcFreq.QuadPart / (DOUBLE)GpuFreq;
            const DOUBLE Begun = (DOUBLE)CpuNow + (DOUBLE)(INT64)(T[0] - GpuNow) * Scale;
            const DOUBLE Done  = (DOUBLE)CpuNow + (DOUBLE)(INT64)(T[5] - GpuNow) * Scale;
            RDX_LOGF("latency: gpu present %I64d done %I64d begun %I64d", LatencySlotPresent[Slot], (LONGLONG)Done,
                     (LONGLONG)Begun);
        }
    }
    LatencySlotPresent[Slot] = 0;
    // The frame's GPU time, start to end with the host's waits in it, for the host's path's pacing.
    if ((Placed & 1) && ((Placed >> 5) & 1) && T[5] > T[0])
    {
        UINT64 GpuFreq = 0;
        if (SUCCEEDED(Queue->GetTimestampFrequency(&GpuFreq)) && GpuFreq)
        {
            PaceGpu[PaceGpuCount % PACE_KEPT] = (DOUBLE)(T[5] - T[0]) / (DOUBLE)GpuFreq;
            PaceGpuCount++;
        }
    }
    // The parts: the maps 1-2, the factor 2-3, the apply 11-4, the shadows 1-4 (the ambient occlusion inside them), the
    // frame 0-5, the motion vectors 6-7, the upscaler 8-9, the ambient occlusion 10-11, the water's copies 12-13, the
    // ripples' steps 14-15, the splashes 16-17, the ray tracing scene 18-19, the volumetric smoke 20-21, the
    // motion blur 22-23, the lens and the film 24-25; through the host, the copies into its textures 8-26 and the rest
    // 26-9 (the crossing both ways, its work, the draw back); the depth of field 27-28; the fog in its rooms 29-30.
    static const INT From[GPU_TIME_PARTS] = { 1, 2, 11, 1, 0, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 8,  26, 27, 29,
                                              31, 32, 33 };
    static const INT To[GPU_TIME_PARTS]   = { 2, 3, 4,  4, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 26, 9,  28, 30,
                                              32, 33, 34 };
    for (INT k = 0; k < GPU_TIME_PARTS; k++)
    {
        if (!((Placed >> From[k]) & 1) || !((Placed >> To[k]) & 1) || T[To[k]] < T[From[k]])
            continue;
        const DOUBLE S = (DOUBLE)(T[To[k]] - T[From[k]]) * TimeTick;
        TimeSum[k] += S;
        TimeMax[k]  = Max(TimeMax[k], S);
        TimeCount[k]++;
    }
    // The rays' list: the frame's hand-overs together, each the three stretches between its four points.
    DOUBLE Round[GPU_TIME_ROUND_PARTS] = { 0.0, 0.0, 0.0 };
    INT    Rounds = 0;
    for (INT r = 0; r < GPU_TIME_ROUNDS; r++)
    {
        const INT P = GPU_TIME_ROUND_FIRST + 4 * r;
        if (((Placed >> P) & 0xF) != 0xF || T[P + 1] < T[P] || T[P + 2] < T[P + 1] || T[P + 3] < T[P + 2])
            continue;
        for (INT k = 0; k < GPU_TIME_ROUND_PARTS; k++)
            Round[k] += (DOUBLE)(T[P + k + 1] - T[P + k]) * TimeTick;
        Rounds++;
    }
    if (Rounds)
    {
        for (INT k = 0; k < GPU_TIME_ROUND_PARTS; k++)
        {
            RoundSum[k] += Round[k];
            RoundMax[k]  = Max(RoundMax[k], Round[k]);
        }
        RoundFrames++;
        RoundCount += Rounds;
    }
    if (Placed & 1u)
    {
        if (TimeLastStart && T[0] > TimeLastStart)
        {
            TimeGapSum += (DOUBLE)(T[0] - TimeLastStart) * TimeTick;
            TimeGaps++;
        }
        TimeLastStart = T[0];
    }
    if (++TimeFrames < GPU_TIME_LOG_FRAMES)
        return;
    // The ticks' unit, from the window itself: the GPU's frame-to-frame time against the CPU's, to the nearest power of ten.
    // Measured (the first timer round): counted in the performance counter's ticks, the GPU's frame took ten
    // times the CPU's, on every line - the driver's tick is a tenth of the counter's.
    const DOUBLE GpuGap = TimeGaps ? TimeGapSum / TimeGaps : 0.0;
    const DOUBLE CpuGap = TimeCpuGaps ? TimeCpuGapSum / TimeCpuGaps : 0.0;
    const DOUBLE Ratio  = (GpuGap > 0.0 && CpuGap > 0.0) ? GpuGap / CpuGap : 1.0;
    const DOUBLE Unit   = pow(10.0, floor(log10(Ratio) + 0.5));
    const DOUBLE Ms     = 1000.0 / Unit;
    RDX_LOGF("gpu time (%i frames, ms, mean / max): shadow maps %.3f / %.2f, factor %.3f / %.2f, apply %.3f / %.2f, "
             "ambient occlusion %.3f / %.2f, shadows in total %.3f / %.2f (over %i frames), motion vectors %.3f / %.2f, upscaling %.3f / %.2f, "
             "our frame in total %.3f / %.2f; frame to frame on the GPU %.3f, on the CPU %.3f "
             "(one tick is 1/%.0f of a counter tick, ratio %.3f); waiting on the GPU: %i uploads, %.3f ms per frame "
             // What was running, APPENDED - so the line says which settings it belongs to when one session
             // measures several, and so that every reader of the line that came before it still matches.
             "(max %.2f), frame change %.3f ms per frame (max %.2f); running %s",
             TimeFrames, TimeCount[0] ? TimeSum[0] / TimeCount[0] * Ms : 0.0, TimeMax[0] * Ms,
             TimeCount[1] ? TimeSum[1] / TimeCount[1] * Ms : 0.0, TimeMax[1] * Ms,
             TimeCount[2] ? TimeSum[2] / TimeCount[2] * Ms : 0.0, TimeMax[2] * Ms,
             TimeCount[7] ? TimeSum[7] / TimeCount[7] * Ms : 0.0, TimeMax[7] * Ms,
             TimeCount[3] ? TimeSum[3] / TimeCount[3] * Ms : 0.0, TimeMax[3] * Ms, TimeCount[3],
             TimeCount[5] ? TimeSum[5] / TimeCount[5] * Ms : 0.0, TimeMax[5] * Ms,
             TimeCount[6] ? TimeSum[6] / TimeCount[6] * Ms : 0.0, TimeMax[6] * Ms,
             TimeCount[4] ? TimeSum[4] / TimeCount[4] * Ms : 0.0, TimeMax[4] * Ms,
             GpuGap * Ms, CpuGap * 1000.0, Unit, Ratio, WaitUploads, WaitUploadSum * 1000.0 / (DOUBLE)TimeFrames,
             WaitUploadMax * 1000.0, WaitSlotSum * 1000.0 / (DOUBLE)TimeFrames, WaitSlotMax * 1000.0,
             FrameCapLabel[0] ? appFromAnsi(FrameCapLabel) : TEXT("-"));
    // The water's copies on a line of their own, and only where there was water: the line above is read by scripts.
    if (TimeCount[8])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the water's copies of the picture and the depth %.3f / %.2f, "
                 "in %i of the frames", TimeFrames, TimeSum[8] / TimeCount[8] * Ms, TimeMax[8] * Ms, TimeCount[8]);
    if (TimeCount[9])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the ripples' steps %.3f / %.2f, in %i of the frames", TimeFrames,
                 TimeSum[9] / TimeCount[9] * Ms, TimeMax[9] * Ms, TimeCount[9]);
    if (TimeCount[19])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the plants' motion vectors - the grass %.3f / %.2f, the bark "
                 "%.3f / %.2f, the leaves %.3f / %.2f, in %i of the frames", TimeFrames,
                 TimeSum[19] / TimeCount[19] * Ms, TimeMax[19] * Ms,
                 TimeCount[20] ? TimeSum[20] / TimeCount[20] * Ms : 0.0, TimeMax[20] * Ms,
                 TimeCount[21] ? TimeSum[21] / TimeCount[21] * Ms : 0.0, TimeMax[21] * Ms, TimeCount[19]);
    // The caustics have no timer of their own - the net is in the frame's total, the laying-on in the water's copies -
    // but how often they ran says whether they were asked for at all.
    if (CausticNetFrames || CausticLaidFrames)
        RDX_LOGF("caustics (%i frames): the net drawn in %i of the frames, laid on the picture in %i", TimeFrames,
                 CausticNetFrames, CausticLaidFrames);
    CausticNetFrames = CausticLaidFrames = 0;
    if (TimeCount[10])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the water's splashes %.3f / %.2f, in %i of the frames", TimeFrames,
                 TimeSum[10] / TimeCount[10] * Ms, TimeMax[10] * Ms, TimeCount[10]);
    if (TimeCount[11])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the ray tracing scene %.3f / %.2f, in %i of the frames", TimeFrames,
                 TimeSum[11] / TimeCount[11] * Ms, TimeMax[11] * Ms, TimeCount[11]);
    if (TimeCount[12])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the smoke walked as a volume %.3f / %.2f, in %i of the frames",
                 TimeFrames, TimeSum[12] / TimeCount[12] * Ms, TimeMax[12] * Ms, TimeCount[12]);
    if (TimeCount[13])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the motion blur %.3f / %.2f, in %i of the frames",
                 TimeFrames, TimeSum[13] / TimeCount[13] * Ms, TimeMax[13] * Ms, TimeCount[13]);
    if (TimeCount[14])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the lens and the film %.3f / %.2f, in %i of the frames",
                 TimeFrames, TimeSum[14] / TimeCount[14] * Ms, TimeMax[14] * Ms, TimeCount[14]);
    if (TimeCount[17])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the depth of field %.3f / %.2f, in %i of the frames",
                 TimeFrames, TimeSum[17] / TimeCount[17] * Ms, TimeMax[17] * Ms, TimeCount[17]);
    if (TimeCount[18])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the fog in its rooms %.3f / %.2f, in %i of the frames",
                 TimeFrames, TimeSum[18] / TimeCount[18] * Ms, TimeMax[18] * Ms, TimeCount[18]);
    // The upscale through the host, taken apart: the copies; the rest, which is the crossing both ways, the host's own
    // work and the draw back - and the host's own GPU time for its work, measured on its queue, so that the crossing is
    // what is left.
    if (TimeCount[15])
        RDX_LOGF("gpu time (%i frames, ms, mean / max): through the host - the copies into its textures %.3f / %.2f, then "
                 "until its picture is drawn back %.3f / %.2f (the crossing both ways, its upscale, the draw), of which its "
                 "own upscale on its queue %.3f; in %i of the frames", TimeFrames,
                 TimeSum[15] / TimeCount[15] * Ms, TimeMax[15] * Ms,
                 TimeCount[16] ? TimeSum[16] / TimeCount[16] * Ms : 0.0, TimeMax[16] * Ms,
                 (DOUBLE)Host.WorkUs() / 1000.0, TimeCount[15]);
    // The rays' list through the host, a frame's hand-overs together: what the asking costs here, what the copies,
    // and the wait for the answers - of which the host's own GPU time for a hand-over, measured on its queue, leaves
    // the crossing. And what the hand-overs held.
    if (RoundFrames)
        RDX_LOGF("gpu time (%i frames, ms, mean / max): the rays' list through the host, a frame's hand-overs together "
                 "- the asking draws %.3f / %.2f, the copies for the host %.3f / %.2f, then until its answers are "
                 "there %.3f / %.2f (the crossing both ways and its tracing); %.2f hand-overs a frame, in %i of the "
                 "frames",
                 TimeFrames, RoundSum[0] / RoundFrames * Ms, RoundMax[0] * Ms, RoundSum[1] / RoundFrames * Ms,
                 RoundMax[1] * Ms, RoundSum[2] / RoundFrames * Ms, RoundMax[2] * Ms,
                 (DOUBLE)RoundCount / (DOUBLE)RoundFrames, RoundFrames);
    FHostRayFeed::FListCounts Listed;
    if (RayFeed.TakeListCounts(Listed))
    {
        const UINT Timed = Host.GetLink().WorkListTimed();
        RDX_LOGF("rays' list (%i frames with hand-overs, %.2f a frame): %.0f requests a frame (%i at the most), of "
                 "them %.0f traced by the host, %.0f "
                 "see-through surfaces passed (%i at the most); the host's own GPU time %.3f ms a hand-over of the "
                 "list (%u timed since the last line), %.3f ms for one of its other rays; the requests by the "
                 "frame's hand-overs in their order: %.0f, %.0f, %.0f, %.0f, %.0f",
                 Listed.Frames, (DOUBLE)Listed.Handovers / (DOUBLE)Listed.Frames,
                 (DOUBLE)Listed.Asked / (DOUBLE)Listed.Frames, Listed.AskedMost,
                 (DOUBLE)Listed.Traced / (DOUBLE)Listed.Frames,
                 (DOUBLE)Listed.Passed / (DOUBLE)Listed.Frames, Listed.PassedMost,
                 (DOUBLE)Host.GetLink().WorkListUs() / 1000.0, Timed - ListTimedLast,
                 (DOUBLE)Host.GetLink().WorkRayUs() / 1000.0,
                 (DOUBLE)Listed.Each[0] / (DOUBLE)Listed.Frames, (DOUBLE)Listed.Each[1] / (DOUBLE)Listed.Frames,
                 (DOUBLE)Listed.Each[2] / (DOUBLE)Listed.Frames, (DOUBLE)Listed.Each[3] / (DOUBLE)Listed.Frames,
                 (DOUBLE)Listed.Each[4] / (DOUBLE)Listed.Frames);
        ListTimedLast = Timed;
    }
    // Frame generation: the host's own GPU time for a generated picture, and the rhythm the pacer measured between two
    // generated pictures going out - the engine's frame rate; twice it reaches the screen.
    if (bFgLast)
    {
        RDX_LOGF("frame generation: a generated picture costs the host %.3f ms on its queue; the pictures go out every "
                 "%.2f ms apart (%.0f a second, from the engine's %.0f)", (DOUBLE)Host.WorkFgUs() / 1000.0,
                 Pacer.Interval() * 500.0, (Pacer.Interval() > 0.0) ? 2.0 / Pacer.Interval() : 0.0,
                 (Pacer.Interval() > 0.0) ? 1.0 / Pacer.Interval() : 0.0);
        // What presenting holds the engine for - handing a frame over, nothing more - and how the pacer's thread's side
        // of it goes (FFramePacer::TakeCosts).
        FFramePacer::FCost C[FFramePacer::Cost_Count];
        Pacer.TakeCosts(C);
        auto MeanMs = [](const FFramePacer::FCost& K) { return K.Count ? K.Sum / K.Count * 1000.0 : 0.0; };
        RDX_LOGF("frame generation, presenting (ms, mean / max): the engine hands a frame over in %.2f / %.2f (%i left "
                 "out); the pacer's thread waits for a free list %.2f / %.2f, in the generated picture's Present %.2f / "
                 "%.2f, in the real one's %.2f / %.2f; a generated picture goes through %.2f / %.2f after it is handed "
                 "over, then is held %.2f / %.2f (%i pictures); the pictures go out a generated one to its real one %.2f / "
                 "%.2f apart, a real one to the next generated one %.2f / %.2f",
                 MeanMs(C[FFramePacer::Cost_Submit]), C[FFramePacer::Cost_Submit].Max * 1000.0, Pacer.TakeDropped(),
                 MeanMs(C[FFramePacer::Cost_ListWait]), C[FFramePacer::Cost_ListWait].Max * 1000.0,
                 MeanMs(C[FFramePacer::Cost_PresentGen]), C[FFramePacer::Cost_PresentGen].Max * 1000.0,
                 MeanMs(C[FFramePacer::Cost_PresentReal]), C[FFramePacer::Cost_PresentReal].Max * 1000.0,
                 MeanMs(C[FFramePacer::Cost_GenThrough]), C[FFramePacer::Cost_GenThrough].Max * 1000.0,
                 MeanMs(C[FFramePacer::Cost_Hold]), C[FFramePacer::Cost_Hold].Max * 1000.0,
                 C[FFramePacer::Cost_PresentGen].Count,
                 MeanMs(C[FFramePacer::Cost_GapGenReal]), C[FFramePacer::Cost_GapGenReal].Max * 1000.0,
                 MeanMs(C[FFramePacer::Cost_GapRealGen]), C[FFramePacer::Cost_GapRealGen].Max * 1000.0);
    }
    WaitUploads   = 0;
    WaitUploadSum = WaitUploadMax = 0.0;
    WaitSlotSum   = WaitSlotMax   = 0.0;
    for (INT k = 0; k < GPU_TIME_PARTS; k++)
    {
        TimeSum[k]   = 0.0;
        TimeMax[k]   = 0.0;
        TimeCount[k] = 0;
    }
    for (INT k = 0; k < GPU_TIME_ROUND_PARTS; k++)
        RoundSum[k] = RoundMax[k] = 0.0;
    RoundFrames   = 0;
    RoundCount    = 0;
    TimeGapSum    = 0.0;
    TimeGaps      = 0;
    TimeCpuGapSum = 0.0;
    TimeCpuGaps   = 0;
    TimeFrames    = 0;
}

// The engine's ClearZ within a frame: it clears the depth after the world, before what it draws on top of it.
//
// Where the depth still has to be readable - the shadows and the motion vectors read the world's - that clear must not
// take it. So a clear of the buffer the world was drawn into (MarkWorldDepth) moves the drawing over to the other
// buffer, cleared, and every later clear in the frame stays in that one. A clear before the world, or with no reader,
// is the plain clear.
//
// ONLY the world's buffer moves. Moving at every clear breaks a frame with two clears in a row - a drone camera's, for
// one: the second clear would land on the world's buffer and the main view would lose its shadows.
D3D12_CPU_DESCRIPTOR_HANDLE FBackend::ClearDepthMidFrame(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    if (!NeedsDepthRead() || !DepthBuffer2 || !DsvHeap2 || bOutputLive || WorldDepth < 0 || ActiveDepth != WorldDepth)
    {
        Cmd->ClearDepthStencilView(MsaaWorldLive() ? MsaaDsvHeap->GetCPUDescriptorHandleForHeapStart() : Dsv,
                                   DepthClearFlags(), 0.0f, 0, 0, nullptr);
        return Dsv;
    }
    // With MSAA the drawing never left the multisampled depth buffer, so "keeping" the world's depth means resolving
    // it into the buffer being retired - that is the one the shadows will read (WorldDepthIndex).
    ResolveMsaaDepth(Cmd, ActiveDepth);

    RetiredDepth = ActiveDepth;
    ActiveDepth  = 1 - ActiveDepth;
    D3D12_CPU_DESCRIPTOR_HANDLE NewDsv = (ActiveDepth == 0) ? DsvHeap->GetCPUDescriptorHandleForHeapStart()
                                                            : DsvHeap2->GetCPUDescriptorHandleForHeapStart();
    BindSceneTargets(Cmd, NewDsv);                    // the scene target (and the light target), as BeginFrame bound them
    Cmd->ClearDepthStencilView(MsaaWorldLive() ? MsaaDsvHeap->GetCPUDescriptorHandleForHeapStart() : NewDsv,
                               DepthClearFlags(), 0.0f, 0, 0, nullptr);
    static INT Logged = 0;
    if (Logged++ < 3)
        RdxLog(TEXT("G-buffer: mid-frame ClearZ - drawing moves to depth buffer %i, buffer %i is kept"),
               ActiveDepth + 1, RetiredDepth + 1);
    return NewDsv;
}

void FBackend::MarkWorldDepth()
{
    WorldDepth = ActiveDepth;
}

UBOOL FBackend::SetWater(INT Level)
{
    const INT Was = WaterLevel;
    WaterLevel    = (Level > 0) ? Level : 0;
    const UBOOL bWant = (WaterLevel > 0) ? 1 : 0;
    if (WaterLevel != Was)
        RdxLog(TEXT("water: %s"), bWant ? TEXT("see-through, over copies of the picture and the depth")
                                        : TEXT("the engine's own"));
    if (bWant == bWaterWanted)
        return 1;
    bWaterWanted = bWant;
    // Off leaves the copies where they are: the frame before may still be reading them, and they go with the next change
    // of size anyway. On, the copies are of a depth that can be read: made so here, where it was not.
    return bWant ? EnsureDepthReadable() : 1;
}

UBOOL FBackend::SetReflections(INT Level)
{
    const INT Was   = ReflectionLevel;
    ReflectionLevel = Clamp(Level, 0, (INT)RdxMath::Reflections_Count - 1);
    if (ReflectionLevel != Was)
        RdxLog(TEXT("reflections: %s"),
               (ReflectionLevel == RdxMath::Reflections_RtMedium)
                   ? TEXT("from rays, a quarter of them - of each two by two pixels one traces for the others where ")
                     TEXT("their rays are alike")
               : (ReflectionLevel == RdxMath::Reflections_RtLow)
                   ? TEXT("from rays, about a ninth of them - of each three by three pixels one is traced for the ")
                     TEXT("others where their rays are alike")
               : (ReflectionLevel == RdxMath::Reflections_RtHigh) ? TEXT("from rays")
               : (ReflectionLevel == RdxMath::Reflections_Low)
                   ? TEXT("the wet floors' water and the lamps' streaks, the water's and the glass's far image ")
                     TEXT("(the sky, a skyline) alone - nothing near mirrored")
               : (ReflectionLevel == RdxMath::Reflections_Medium)
                   ? TEXT("from the picture, the wet floors' image coarser")
               : (ReflectionLevel ? TEXT("from the picture") : TEXT("none of ours")));
    // On, the glass's layer reads copies of a depth that can be read: made so here, where it was not.
    return (ReflectionLevel > 0 && !Was) ? EnsureDepthReadable() : 1;
}

UBOOL FBackend::SetSoftFade(FLOAT Units)
{
    Solid.SetSoftFade(Units);
    // Off leaves the targets as they are. On, the fade reads a copy of a depth that can be read: made so here, where it
    // was not.
    return (Units > 0.0f) ? EnsureDepthReadable() : 1;
}

// A wish to read the depth that came after the buffers were made (NeedsDepthRead, bDepthReadable): the targets again,
// as the depth buffer's format is fixed when it is made. Not inside a frame, and not on a device that is gone.
UBOOL FBackend::EnsureDepthReadable()
{
    if (bDepthReadable || bDepthReadRefused || !NeedsDepthRead() || bDeviceLost || bInFrame || !IsReady() ||
        !SceneColor)
        return 1;
    RdxLog(TEXT("depth: something reads it that did not when the targets were made - they are made again, with a depth "
                "that can be read"));
    if (Resize(Width, Height))
        return 1;
    bDepthReadRefused = 1;
    return 0;
}

// The water's copies (see WaterColor), made alike to what they copy - the same format and size, and the depth copy a depth
// buffer too, as that is what another depth buffer can be copied into - with their views in the texture heap's reserved
// slots. Made in the first frame that has water, and again after the targets they copy were made again.
UBOOL FBackend::EnsureWaterTargets()
{
    // Only of a depth that is read, and was made to be. The views below are views of a typeless buffer; and a frame
    // with no reader neither marks the world's start nor keeps its depth through the engine's ClearZ (MarkWorldStart,
    // ClearDepthMidFrame), so copies left from an earlier setting would be filled after that clear - with the weapon
    // in hand alone.
    if (!bDepthReadable || !NeedsDepthRead())
        return 0;
    if (WaterColor && WaterDepth)
        return 1;
    if (!SceneColor || !DepthBuffer || !Textures.GetHeap())
        return 0;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
    appMemzero(&Srv, sizeof(Srv));
    Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Srv.Texture2D.MipLevels     = 1;

    if (!WaterColor)
    {
        D3D12_RESOURCE_DESC Desc = SceneColor->GetDesc();
        Desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   __uuidof(ID3D12Resource), WaterColor.VoidSlot()),
                   TEXT("CreateCommittedResource(water picture)")))
            return 0;
        Srv.Format = Colours.Draw;
        for (INT Set = 0; Set < 2; Set++)
            Device->CreateShaderResourceView(WaterColor.Get(), &Srv, Textures.GetCpuHandle(WaterSlot(Set, WaterViewPicture)));
    }
    if (!WaterDepth)
    {
        const D3D12_RESOURCE_DESC Desc = DepthBuffer->GetDesc();
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                   nullptr, __uuidof(ID3D12Resource), WaterDepth.VoidSlot()),
                   TEXT("CreateCommittedResource(water depth)")))
        {
            WaterColor.Release();
            return 0;
        }
        WaterDepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        Srv.Format = bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT;
        for (INT Set = 0; Set < 2; Set++)
            Device->CreateShaderResourceView(WaterDepth.Get(), &Srv, Textures.GetCpuHandle(WaterSlot(Set, WaterViewDepth)));
        // And a depth view: what the engine draws after the world goes into the copy before the walk
        // (PutAfterWorldDepth). Without one the walk simply goes on to the world's depth.
        if (!WaterDepthDsvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
            appMemzero(&HeapDesc, sizeof(HeapDesc));
            HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            HeapDesc.NumDescriptors = 1;
            Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), WaterDepthDsvHeap.VoidSlot());
        }
        if (WaterDepthDsvHeap)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC Dv;
            appMemzero(&Dv, sizeof(Dv));
            Dv.Format        = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
            Dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            Device->CreateDepthStencilView(WaterDepth.Get(), &Dv, WaterDepthDsvHeap->GetCPUDescriptorHandleForHeapStart());
        }
    }
    // Still water until the ripples and the wave field exist (their own Ensure writes their views over these): a view of
    // nothing reads zero.
    if (!RippleField[0] || !RippleField[1])
    {
        Srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        for (INT Set = 0; Set < 2; Set++)
            Device->CreateShaderResourceView(nullptr, &Srv, Textures.GetCpuHandle(WaterSlot(Set, WaterViewRipples)));
    }
    if (!WaveField)
    {
        Srv.Format = DXGI_FORMAT_R8_UNORM;
        for (INT Set = 0; Set < 2; Set++)
            Device->CreateShaderResourceView(nullptr, &Srv, Textures.GetCpuHandle(WaterSlot(Set, WaterViewWaves)));
    }
    // And no net until the caustics make theirs (EnsureCaustics): the water's table reaches this view too.
    if (!CausticNet)
    {
        Srv.Format = DXGI_FORMAT_R16_FLOAT;
        for (INT Set = 0; Set < 2; Set++)
            Device->CreateShaderResourceView(nullptr, &Srv, Textures.GetCpuHandle(WaterSlot(Set, WaterViewNet)));
    }
    // And the mirror probe's pyramid after it - a view of nothing until the probe is made (EnsureMirrorProbe) - and the
    // probe's own table, which views these copies, to be written again.
    for (INT Set = 0; Set < 2; Set++)
    {
        MirrorProbe.WriteLevelsView(Device.Get(), Textures.GetCpuHandle(WaterSlot(Set, WaterViewProbe)));
        FMirrorBody::WriteView(Device.Get(), MirrorBody.GetPicture(), Textures.GetCpuHandle(WaterSlot(Set, WaterViewBody)));
    }
    bProbeViewsDirty = 1;
    RdxViewsChanged();          // the smoke's walk reads the depth's copy through a view of its own (ViewGeneration.h)
    RdxLog(TEXT("water: the copies of the picture and the depth made, %ix%i"), SceneWidth, SceneHeight);
    return 1;
}

// What the host's rays read of the frame's own (FHostRayFeed::FViews): the world's depth buffer and the one drawn on
// top, the light target, the factor's size - and the scene's size for the mirror rays, where the glass's layers ask any.
void FBackend::HostRayViews(FHostRayFeed::FViews& Views, D3D12_RESOURCE_STATES DepthState, D3D12_RESOURCE_STATES LightState)
{
    const INT World = WorldDepthIndex();
    appMemzero(&Views, sizeof(Views));
    Views.WorldDepth = (World == 1) ? DepthBuffer2.Get() : DepthBuffer.Get();
    Views.OnTopDepth = (World == 1) ? DepthBuffer.Get() : DepthBuffer2.Get();
    Views.DepthState = DepthState;
    Views.Light      = LightTarget.Get();
    Views.LightState = LightState;
    Views.FactorW    = ShadowW;
    Views.FactorH    = ShadowH;
    // The water surface's pack and factor (WaterPack, WaterFactor), of EnsureWaterFactor's sizes.
    const INT S        = Max(1, (INT)(UpscaleMode ? ShadowFactorDivisor() : (SceneScale * ShadowFactorDivisor())));
    Views.SurfacePackW = (ShadowW + WaterPackCoarse - 1) / WaterPackCoarse;
    Views.SurfacePackH = (ShadowH + WaterPackCoarse - 1) / WaterPackCoarse;
    Views.SurfaceW     = (Views.SurfacePackW + S - 1) / S;
    Views.SurfaceH     = (Views.SurfacePackH + S - 1) / S;
    Views.AskW       = IsRtMirrorRunning() ? SceneWidth : 0;
    Views.AskH       = IsRtMirrorRunning() ? SceneHeight : 0;
}

// FTargetSwitch: the mirror rays through the 64-bit host, right after the water's copies (FSolidPass::Flush). The
// frame's bodies are listed and the share for this level and these sizes asked for, as for the shadows' rays
// (ApplyShadows, which then finds them listed); the tables the layer's shader reads here are made - the rays' table and
// the level's colours - and the two targets the rays are written into are bound.
UBOOL FBackend::BeginMirrorAsk(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    return BeginRayAsk(Cmd, Bind, &Dsv, 1);
}

// FTargetSwitch: the panes' rays through the 64-bit host, before the world's runs (FSolidPass::Flush) - where the
// frame's panes give the flashlight's light back (bPaneNow), as BeginPaneKeep has it.
UBOOL FBackend::BeginPaneAsk(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind, FLOAT* Size)
{
    if (!bPaneNow || !Size || !BeginRayAsk(Cmd, Bind, nullptr, 0))
        return 0;
    Size[0] = (FLOAT)SceneWidth;
    Size[1] = (FLOAT)SceneHeight;
    Size[2] = PaneRimCos;
    return 1;
}

UBOOL FBackend::BeginRayAsk(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind,
                            const D3D12_CPU_DESCRIPTOR_HANDLE* Dsv, UBOOL bWorld)
{
    appMemzero(&Bind, sizeof(Bind));
    if (!IsRtMirrorRunning() || Cmd != CommandList.Get() || !Shadow.IsReady() || !bCamSet || !Textures.GetHeap() ||
        FrameIndex >= BACKEND_FRAME_COUNT)
        return 0;
    FHostRayFeed::FViews Views;
    HostRayViews(Views, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
    if (!bRtSceneTried)
    {
        RtHostBodies  = Rt.ListScene((INT)FrameIndex, Shadow);
        bRtSceneBuilt = RtHostBodies >= 0 &&
                        RayFeed.Prepare(Device.Get(), Host.GetLink(), Rt, Shadow, Views, RtHostBodies);
        bRtSceneTried = 1;
    }
    if (!bRtSceneBuilt || !RayFeed.HasList())
        return 0;
    Shadow.UploadMasks(Cmd);
    // The list's views in the texture heap, for this share's resources. For the builds that write their rays down:
    // the pixels' heads and the requests, as a pixel shader writes them. For the builds that read the answers: the
    // heads in the masks' view's place - this process tests no mask - the answers, and the see-through surfaces the
    // rays passed, or a view of nothing where the share has no buffer of them. The far probe's view lies beside
    // the heads'.
    if (RtHitsViewSerial != RayFeed.GetShareSerial())
    {
        const UINT Words = (UINT)RayFeed.GetListRoom() * (RDX_RAY_SLOT_BYTES / 4);
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav;
        appMemzero(&Uav, sizeof(Uav));
        Uav.Format        = DXGI_FORMAT_R32_UINT;
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Device->CreateUnorderedAccessView(RayFeed.GetHeads(), nullptr, &Uav,
                                          Textures.GetCpuHandle(Textures.GetReservedIndex(RayHeadsUav)));
        appMemzero(&Uav, sizeof(Uav));
        Uav.Format             = DXGI_FORMAT_R32_TYPELESS;
        Uav.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Buffer.NumElements = Words;
        Uav.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
        Device->CreateUnorderedAccessView(RayFeed.GetRequests(), nullptr, &Uav,
                                          Textures.GetCpuHandle(Textures.GetReservedIndex(RayAskedUav)));
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
        appMemzero(&Srv, sizeof(Srv));
        Srv.Format                  = DXGI_FORMAT_R32_UINT;
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels     = 1;
        Device->CreateShaderResourceView(RayFeed.GetHeads(), &Srv,
                                         Textures.GetCpuHandle(Textures.GetReservedIndex(RtMaskView)));
        appMemzero(&Srv, sizeof(Srv));
        Srv.Format                  = DXGI_FORMAT_R32_TYPELESS;
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Buffer.NumElements      = Words;
        Srv.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
        Device->CreateShaderResourceView(RayFeed.GetAnswers(), &Srv,
                                         Textures.GetCpuHandle(Textures.GetReservedIndex(RayAnswersView)));
        Srv.Buffer.NumElements      = RayFeed.GetPassed()
                                          ? (UINT)RayFeed.GetPassedRoom() * (RDX_RAY_SLOT_BYTES / 4) : 1;
        Device->CreateShaderResourceView(RayFeed.GetPassed(), &Srv,
                                         Textures.GetCpuHandle(Textures.GetReservedIndex(RayPassedView)));
        for (INT k = 0; k < 3; k++)
        {
            ID3D12Resource* const Corners = RayFeed.GetCorners(k);
            Srv.Buffer.NumElements = Corners ? (UINT)(RayFeed.GetCornerBytes(k) / 4) : 1;
            Device->CreateShaderResourceView(Corners, &Srv,
                                             Textures.GetCpuHandle(Textures.GetReservedIndex(RayCornersView + k)));
        }
        RtHitsViewSerial = RayFeed.GetShareSerial();
    }
    // Reflections=RTLow: its pass and its own list, made where they are not yet - where either cannot be, the round
    // runs as RT, every ray asked traced. The own list's view (u7 of the water's UAV table) is a view of nothing until
    // then.
    RtSparseNow = GetRtMirrorSparse();
    if (RtSparseNow == 2)
    {
        if (!bSparseTried)
        {
            bSparseTried = 1;
            RaySparse.Init(Device.Get());
        }
        if (!RaySparse.IsReady() || !RayFeed.EnsureLow(Device.Get()))
            RtSparseNow = 0;
    }
    if (RtLowViewSerial != RayFeed.GetShareSerial() || RtLowViewHas != (RayFeed.GetLow() ? 1 : 0))
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav;
        appMemzero(&Uav, sizeof(Uav));
        Uav.Format             = DXGI_FORMAT_R32_TYPELESS;
        Uav.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Buffer.NumElements = RayFeed.GetLow() ? (UINT)RayFeed.GetListRoom() * (RDX_RAY_SLOT_BYTES / 4) : 1;
        Uav.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
        Device->CreateUnorderedAccessView(RayFeed.GetLow(), nullptr, &Uav,
                                          Textures.GetCpuHandle(Textures.GetReservedIndex(RayAskedLowUav)));
        RtLowViewSerial = RayFeed.GetShareSerial();
        RtLowViewHas    = RayFeed.GetLow() ? 1 : 0;
    }
    // The table's mode (RtEye.w): RTLow's carries the lamps that cast shadows in its mirror image beside it.
    Bind.Table         = Rt.PrepareWater(Cmd, (INT)FrameIndex, Shadow, CamEye, RtWaterView,
                                         (RtSparseNow == 2) ? 2 + Clamp(RtLowLamps, 0, (INT)RdxMath::RtLowLampsMost)
                                                            : RtSparseNow);
    // Nothing is traced here: the scene's place in the signature takes an address, and the table's is one - until
    // the answers are there, when the requests take that place (EndMirrorAsk).
    Bind.Scene         = Bind.Table;
    Bind.FrameUvw      = Shadow.GetCasterUvw(0);
    Bind.ArenaUvw      = Shadow.GetCasterUvw(1) ? Shadow.GetCasterUvw(1) : Bind.FrameUvw;
    Bind.Masks         = Textures.GetGpuHandle(Textures.GetReservedIndex(RtMaskView));
    Bind.LevelMaterial = Rt.GetLevelMaterial();
    Bind.Asked         = Textures.GetGpuHandle(Textures.GetReservedIndex(WaterSurfaceUavView));
    if (!(Bind.Table && Bind.FrameUvw && Bind.LevelMaterial) || (bWorld && !UpdateRtFar(Cmd, Bind)))
        return 0;
    if (!RayFeed.BeginAsk(Cmd, RtSparseNow == 2))
        return 0;
    GpuTime(Cmd, RoundPoint(0));
    // No target: what the asking draws write, they write into the list. The depth as the layers' own drawing has it:
    // with MSAA on that is the world's own multisampled one while the world is drawn (BindSceneTargets), and the
    // layers' pipelines there are made for its sample count.
    const D3D12_VIEWPORT Whole = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const D3D12_RECT     All   = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &Whole);
    Cmd->RSSetScissorRects(1, &All);
    if (Dsv && MsaaWorldLive())
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE MsDsv = MsaaDsvHeap->GetCPUDescriptorHandleForHeapStart();
        Cmd->OMSetRenderTargets(0, nullptr, FALSE, &MsDsv);
    }
    else
        Cmd->OMSetRenderTargets(0, nullptr, FALSE, Dsv);
    return 1;
}

// ... and the rays handed over: the frame is split here (FHostRayFeed::Handover), so the scene's targets and viewport
// are bound again whatever came of it. The answers are readable by the layers' pixel shader from here to the frame's
// next hand-over, and so are the requests, which the layers find their own answers by.
UBOOL FBackend::EndMirrorAsk(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv, FRtWaterBind& Bind)
{
    GpuTime(Cmd, RoundPoint(1));
    // Reflections=RTLow: of the rays asked, the ones the host is to trace, into its list (FRaySparse) - timed with the
    // copies for it.
    if (RtSparseNow == 2)
        RaySparse.Run(Cmd, Textures.GetHeap(), Textures.GetGpuHandle(Textures.GetReservedIndex(RayHeadsUav)),
                      SceneWidth, SceneHeight, CamEye, RayFeed.GetHeads(), RayFeed.GetLow(), RayFeed.GetRequests());
    UBOOL bTraced = RayFeed.FeedList(Cmd, Host.GetLink(), Rt, Shadow, (INT)FrameIndex, RtHostBodies,
                                     Rt.GetOwnBody((INT)FrameIndex), RtSparseNow == 2);
    GpuTime(Cmd, RoundPoint(2));
    if (bTraced)
        NoteFrameSubmit(1);
    bTraced = bTraced && RayFeed.Handover(Cmd, Allocators[FrameIndex], Queue.Get(), Host.GetLink());
    GpuTime(Cmd, RoundPoint(3));
    TimeRound++;
    if (bTraced)
    {
        RayFeed.NotePassed(Cmd, (INT)FrameIndex);
        RayFeed.ListReadable(Cmd, 1);
        // RTLow's pixels find their blocks in the own list, which says where in the host's their answers are.
        Bind.Scene = ((RtSparseNow == 2) ? RayFeed.GetLow() : RayFeed.GetRequests())->GetGPUVirtualAddress();
    }
    const D3D12_VIEWPORT Whole = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const D3D12_RECT     All   = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &Whole);
    Cmd->RSSetScissorRects(1, &All);
    BindSceneTargets(Cmd, Dsv);
    static INT Told = 0;
    if ((Told++ % 600) == 0)
        RdxLog(TEXT("mirror rays: through the 64-bit host - %s; %i bodies in the rays' scene"),
               bTraced ? TEXT("asked and answered")
                       : TEXT("NOT answered this frame, the surfaces on rays draw without"), RtHostBodies);
    return bTraced;
}

UBOOL FBackend::UpdateRtFar(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind)
{
    // The far probe: its views in the texture heap once, then this frame's far picture into it, before the water reads it.
    // The copies are read by a compute shader for it, so for the moment they are readable by both kinds.
    if (!bRtFarViews && Textures.GetHeap() && Rt.GetFar())
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC  Srv;
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav;
        Rt.DescribeFarViews(Srv, Uav);
        Device->CreateShaderResourceView(Rt.GetFar(), &Srv, Textures.GetCpuHandle(Textures.GetReservedIndex(RtFarView)));
        Device->CreateUnorderedAccessView(Rt.GetFar(), nullptr, &Uav,
                                          Textures.GetCpuHandle(Textures.GetReservedIndex(RtFarUav)));
        bRtFarViews = 1;
    }
    if (!bRtFarViews || WaterDepthState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        return 0;
    {
        D3D12_RESOURCE_BARRIER B[2];
        appMemzero(B, sizeof(B));
        for (INT i = 0; i < 2; i++)
        {
            B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            B[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        B[0].Transition.pResource = WaterColor.Get();
        B[1].Transition.pResource = WaterDepth.Get();
        Cmd->ResourceBarrier(2, B);
        ID3D12DescriptorHeap* Heaps[1] = { Textures.GetHeap() };
        Cmd->SetDescriptorHeaps(1, Heaps);
        static const FLOAT Whole[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        Rt.UpdateFar(Cmd, Textures.GetGpuHandle(WaterSlot(0, WaterViewPicture)),
                     Textures.GetGpuHandle(Textures.GetReservedIndex(RtFarUav)), CamAxes,
                     bShadowViewSet ? ShadowTanX : ViewTanX, bShadowViewSet ? ShadowTanY : ViewTanY,
                     bShadowViewSet ? ShadowViewRect : Whole);
        for (INT i = 0; i < 2; i++)
        {
            B[i].Transition.StateBefore = B[i].Transition.StateAfter;
            B[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        Cmd->ResourceBarrier(2, B);
    }
    Bind.Far     = Textures.GetGpuHandle(Textures.GetReservedIndex(RtFarView));
    RtMirrorIdle = 0;
    return 1;
}

UBOOL FBackend::EnsureMirrorProbe()
{
    if (MirrorProbe.IsReady())
        return 1;
    if (bProbeTried || !Device || !Textures.GetHeap())
        return 0;
    bProbeTried = 1;
    if (!MirrorProbe.Init(Device.Get()))
    {
        bProbeRefused = 1;
        return 0;
    }
    // The own body in the pane beside it: where it cannot be made, the panes show none, and nothing else changes.
    MirrorBody.Init(Device.Get());
    // The glass's layer reads its pyramid from the water's sets from now on.
    for (INT Set = 0; Set < 2; Set++)
        MirrorProbe.WriteLevelsView(Device.Get(), Textures.GetCpuHandle(WaterSlot(Set, WaterViewProbe)));
    bProbeViewsDirty = 1;
    return 1;
}

// The probe's passes' table (FMirrorProbe's VIEW_* order): the copies of the picture and the depth - the depth's own
// float and, in its second plane, the shadows' stencil of surface kinds - and the added target, then the probe's own.
void FBackend::WriteProbeViews()
{
    const D3D12_CPU_DESCRIPTOR_HANDLE First = Textures.GetCpuHandle(Textures.GetReservedIndex(ProbeTableView));
    const UINT Step = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto At = [&](INT View)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE H = First;
        H.ptr += (SIZE_T)View * Step;
        return H;
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
    appMemzero(&Srv, sizeof(Srv));
    Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Srv.Texture2D.MipLevels     = 1;
    Srv.Format                  = Colours.Draw;
    Device->CreateShaderResourceView(WaterColor.Get(), &Srv, At(FMirrorProbe::VIEW_PICTURE));
    Srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    Device->CreateShaderResourceView(WaterDepth.Get(), &Srv, At(FMirrorProbe::VIEW_DEPTH));
    Srv.Format               = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    Srv.Texture2D.PlaneSlice = 1;
    Device->CreateShaderResourceView(WaterDepth.Get(), &Srv, At(FMirrorProbe::VIEW_KINDS));
    Srv.Format               = Colours.Draw;
    Srv.Texture2D.PlaneSlice = 0;
    Device->CreateShaderResourceView(AddedTarget.Get(), &Srv, At(FMirrorProbe::VIEW_ADDED));
    MirrorProbe.WriteOwnViews(First, Step);
    bProbeViewsDirty = 0;
}

// The probe's frame (CopyForWater, right after the copies): the copies and the added target - the geometry pass's third
// target, bound for its draws - readable by the passes for the while, and back as the pass has them.
UBOOL FBackend::UpdateMirrorProbe(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    if (bProbeDone || !IsProbeMirrorRunning() || !bCamSet || !WaterColor || !WaterDepth || !AddedTarget ||
        WaterDepthState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE || !EnsureMirrorProbe())
        return 0;
    if (bProbeViewsDirty)
        WriteProbeViews();
    D3D12_RESOURCE_BARRIER B[3];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 3; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        B[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    B[0].Transition.pResource   = WaterColor.Get();
    B[1].Transition.pResource   = WaterDepth.Get();
    B[2].Transition.pResource   = AddedTarget.Get();
    B[2].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[2].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(3, B);
    ID3D12DescriptorHeap* Heaps[1] = { Textures.GetHeap() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    static const FLOAT Whole[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    MirrorProbe.Update(Cmd, Textures.GetGpuHandle(Textures.GetReservedIndex(ProbeTableView)), CamEye, CamAxes,
                       bShadowViewSet ? ShadowTanX : ViewTanX, bShadowViewSet ? ShadowTanY : ViewTanY,
                       bShadowViewSet ? ShadowViewRect : Whole, (UINT)SceneWidth, (UINT)SceneHeight);
    for (INT i = 0; i < 3; i++)
    {
        const D3D12_RESOURCE_STATES Was = B[i].Transition.StateBefore;
        B[i].Transition.StateBefore = B[i].Transition.StateAfter;
        B[i].Transition.StateAfter  = Was;
    }
    Cmd->ResourceBarrier(3, B);
    bProbeDone = 1;
    DrawMirrorBody(Cmd, Dsv);
    static INT Told = 0;
    if ((Told++ % 600) == 0)
        RdxLog(TEXT("mirror probe: its passes run after the water's copies (%i frames so far)"), Told);
    return 1;
}

void FBackend::DrawMirrorBody(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    UBOOL bMade = 0;
    if (!MirrorBody.IsReady() || !MirrorBody.EnsureTargets(SceneWidth, SceneHeight, bMade))
        return;
    // A picture made anew: the glass's layer reads it from the water's sets from now on.
    if (bMade)
        for (INT Set = 0; Set < 2; Set++)
            FMirrorBody::WriteView(Device.Get(), MirrorBody.GetPicture(),
                                   Textures.GetCpuHandle(WaterSlot(Set, WaterViewBody)));
    // The body: the caster the shadows marked as the own body, where the probe stands before a pane.
    FLOAT                Normal[3] = { 0.0f, 0.0f, 0.0f }, Middle[3] = { 0.0f, 0.0f, 0.0f };
    FMirrorBody::FBody   Body;
    appMemzero(&Body, sizeof(Body));
    const FMirrorBody::FBody* Use = nullptr;
    if (Shadow.IsReady() && MirrorProbe.GetPane(Normal, Middle))
        for (INT i = 0; i < Shadow.GetCasterCount() && !Use; i++)
        {
            D3D12_VERTEX_BUFFER_VIEW Vb;
            INT First = 0, Count = 0;
            const FShadowMaps::FCasterRt* const Rt = Shadow.GetCasterRt(i);
            if (!Shadow.IsCasterOwnBody(i) || !Rt || !Shadow.GetCasterGeometry(i, Vb, First, Count) || Count < 3)
                continue;
            const UBOOL           bArena = Shadow.IsCasterInArena(i);
            ID3D12Resource* const Xyz    = Shadow.GetCasterBuffer(bArena, 0);
            ID3D12Resource* const Uvw    = Shadow.GetCasterBuffer(bArena, 1);
            if (!Xyz || !Uvw)
                continue;
            appMemzero(&Body, sizeof(Body));
            Body.Positions  = Xyz->GetGPUVirtualAddress();
            Body.Places     = Uvw->GetGPUVirtualAddress();
            Body.First      = First;
            Body.Count      = Count;
            Body.bEngineLit = (Rt->Colour & RdxMath::RtAlbedoEngineLit) ? 1 : 0;
            Body.Average    = Rt->Colour;
            for (INT w = 0; w < 6; w++)
                Body.Cube[w] = Rt->Cube[w];
            for (INT k = 0; k < 15 && k < RdxMath::RtCasterKeys; k++)
                Body.Skins[k] = Rt->Texture[k];
            Use = &Body;
        }
    const FLOAT Plane[4] = { Normal[0], Normal[1], Normal[2],
                             Normal[0] * Middle[0] + Normal[1] * Middle[1] + Normal[2] * Middle[2] };
    MirrorBody.Draw(Cmd, Textures.GetHeap(), Use, Plane, CamEye, CamAxes, ViewTanX, ViewTanY, JitterClip,
                    GetLightBoost());
    const D3D12_VIEWPORT Whole = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const D3D12_RECT     All   = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &Whole);
    Cmd->RSSetScissorRects(1, &All);
    BindSceneTargets(Cmd, Dsv);
    static INT Told = 0;
    if ((Told++ % 600) == 0)
        RdxLog(TEXT("mirror body: %s (%i frames so far)"),
               Use ? TEXT("the own body drawn mirrored by the probe's pane") : TEXT("no body to draw this frame"), Told);
}

// FTargetSwitch: the frame's panes of glass go into the mirrors' map (FSolidPass::Flush, before the world's runs).
UBOOL FBackend::BeginPaneKeep(ID3D12GraphicsCommandList* Cmd, FRtWaterBind& Bind, FLOAT* Size)
{
    // Now and then, whether the panes reach the map at all.
    static INT Asked = 0, Drawn = 0;
    // The panes' rays are asked and answered by now (BeginPaneAsk), and Bind is what that left.
    const UBOOL bDraw = (bPaneNow && Size && IsRtMirrorRunning() && Bind.Table != 0 && Bounce.BeginDraw(Cmd)) ? 1 : 0;
    Drawn += bDraw;
    if ((Asked++ % 600) == 0)
        RdxLog(TEXT("flashlight off glass: the panes drawn into the mirrors' map in %i of %i frames with numbered "
                    "panes so far - %i planes of glass now, the flashlight %s"),
               Drawn, Asked, PaneCount, bBeamOn ? TEXT("on") : TEXT("off"));
    if (!bDraw)
        return 0;
    Size[0] = (FLOAT)SceneWidth;
    Size[1] = (FLOAT)SceneHeight;
    Size[2] = PaneRimCos;
    return 1;
}

void FBackend::EndPaneKeep(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    Bounce.EndDraw(Cmd);
    const D3D12_VIEWPORT Whole = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const D3D12_RECT     All   = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &Whole);
    Cmd->RSSetScissorRects(1, &All);
    BindSceneTargets(Cmd, Dsv);
}

// FTargetSwitch: the frame's first water run is next (FSolidPass::Flush). The picture and the depth drawn so far go where
// the water pipeline reads them. With MSAA the world is still in the multisampled set: its depth is resolved into the
// single-sample buffer first (the same resolve the shadows use), and the picture is resolved straight into the copy.
UBOOL FBackend::CopyForWater(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    // The soft particles and the volumetric smoke need the depth half of this copy even where there is no water at
    // all - one to fade where it meets a wall, the other to stop its walk there - so the gate asks for any of the
    // three. Where the water is not one of them the picture is not copied: nothing reads it.
    const UBOOL bWantPicture = bWaterWanted || ReflectionLevel > 0;
    CopyAsked++;
    if ((!bWantPicture && Solid.GetSoftFade() <= 0.0f && !WantsCloudDepth()) || bOutputLive || !EnsureWaterTargets())
    {
        CopyRefused++;
        // WHY IT WAS NOT MADE, in the log and not in anyone's head. Three separate things have been traced back
        // to this copy going missing - the smoke under MSAA, the smoke with the soft particles off, and a cloud
        // that flickered - and each time it took a round of guessing. The values, not a conclusion.
        static INT Told = 0;
        // NOT while the world is over: after the switch the copy cannot be made and is not wanted, and those
        // harmless refusals came first and used up every line the counter allowed.
        if (Told < 8 && !bOutputLive && (Solid.GetCloudsOn() || ShaftStrength > 0.0f))
        {
            Told++;
            RdxLog(TEXT("the depth copy was NOT made, and the smoke or the shafts wanted it: water wanted %i, "
                        "soft fade %.1f, clouds on %i, clouds recent %i, shafts %.2f, the world is over %i, "
                        "its targets %i"),
                   (INT)bWaterWanted, Solid.GetSoftFade(), (INT)Solid.GetCloudsOn(), CloudsRecent,
                   ShaftStrength, (INT)bOutputLive, (INT)EnsureWaterTargets());
        }
        return 0;
    }
    const UBOOL bMsaa = MsaaWorldLive();
    if (bMsaa && (!MsaaColor || !MsaaDepth))
        return 0;
    ID3D12Resource* const Depth   = (ActiveDepth == 1 && DepthBuffer2) ? DepthBuffer2.Get() : DepthBuffer.Get();
    ID3D12Resource* const Picture = bMsaa ? MsaaColor.Get() : SceneColor.Get();
    if (!bExtraCopy)
        GpuTime(Cmd, 12);
    if (bMsaa)
        ResolveMsaaDepth(Cmd, ActiveDepth);

    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 2; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    // The depth first: the caustics are laid on by it.
    B[0].Transition.pResource   = Depth;
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    B[1].Transition.pResource   = WaterDepth.Get();
    B[1].Transition.StateBefore = WaterDepthState;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    Cmd->ResourceBarrier(2, B);
    Cmd->CopyResource(WaterDepth.Get(), Depth);
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    // The depth copy stays readable from here on; the next frame's copy starts from there.
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    WaterDepthState             = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(2, B);

    // The caustics, onto the picture itself and before the water's copy of it is taken: the water then shows them bent,
    // as it shows the bed they lie on. Once a frame - a copy made again for a fall's sheet finds them there.
    const UBOOL bLaid = bExtraCopy ? 0 : LayCaustics(Cmd, bMsaa);

    if (bWantPicture)
    {
        B[0].Transition.pResource   = Picture;
        B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        B[0].Transition.StateAfter  = bMsaa ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
        B[1].Transition.pResource   = WaterColor.Get();
        B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        B[1].Transition.StateAfter  = bMsaa ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST;
        Cmd->ResourceBarrier(2, B);
        if (bMsaa)
            Cmd->ResolveSubresource(WaterColor.Get(), 0, Picture, 0, Colours.Draw);
        else
            Cmd->CopyResource(WaterColor.Get(), Picture);
        for (INT i = 0; i < 2; i++)
        {
            const D3D12_RESOURCE_STATES Was = B[i].Transition.StateBefore;
            B[i].Transition.StateBefore = B[i].Transition.StateAfter;
            B[i].Transition.StateAfter  = Was;
        }
        Cmd->ResourceBarrier(2, B);
    }

    // The mirror probe's passes, on the copies just made: once a frame, at the frame's own copy - one made again for a
    // fall's sheet holds no more of the level's surfaces, only more laid over them.
    if (bWantPicture && !bExtraCopy)
        UpdateMirrorProbe(Cmd, Dsv);
    // The wet floors' late reflections, on the same copies, after the probe has gathered from them - so it never gathers
    // them back - and once a frame: a copy made again finds them on the picture already.
    if (bWantPicture && !bExtraCopy)
        DrawWetReflections(Cmd, Dsv);

    // The depth resolve and the caustics bound targets of their own.
    if (bMsaa || bLaid)
        BindSceneTargets(Cmd, Dsv);
    if (!bExtraCopy)
        GpuTime(Cmd, 13);
    // The volumetric smoke reads this same copy at the world's end rather than making a second one: making it twice
    // in a frame means transitioning resources out of states they are no longer in, and that is a device removed.
    bDepthCopyDone = 1;
    return 1;
}

// FTargetSwitch: a fall's sheet is next, and the picture has been drawn on since the copies were made
// (FSolidPass::Flush). The same copies again - only not the frame's own, for the timing.
UBOOL FBackend::CopyForWaterAgain(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    bExtraCopy = 1;
    const UBOOL bCopied = CopyForWater(Cmd, Dsv);
    bExtraCopy = 0;
    return bCopied;
}

UBOOL FBackend::SetReliefCoverWanted(UBOOL bWant)
{
    bWant = bWant ? 1 : 0;
    if (bWant == bReliefCoverWanted)
        return 1;
    bReliefCoverWanted = bWant;
    RdxLog(TEXT("relief: %s"), bWant ? TEXT("raised surfaces stop under what stands above them (the covers)")
                                     : TEXT("not raised - no covers"));
    // Off leaves the covers where they are: the frame before may still be reading them, and they go with the next change
    // of size anyway. On, the covers are copies of a depth that can be read: made so here, where it was not.
    return bWant ? EnsureDepthReadable() : 1;
}

// The relief's covers (see ReliefCover): made alike to the depth buffer - a depth buffer too, as that is what another depth
// buffer can be copied into, and what the rest of the covers are drawn into - with its depth view in a heap of its own and
// its shader view in the texture heap's reserved slot.
UBOOL FBackend::EnsureReliefCover()
{
    if (ReliefCover)
        return 1;
    if (!DepthBuffer || !Textures.GetHeap() || !NeedsDepthRead() || !bDepthReadable)
        return 0;
    if (!ReliefCoverDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        HeapDesc.NumDescriptors = 1;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), ReliefCoverDsvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(relief covers)")))
            return 0;
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC Desc = DepthBuffer->GetDesc();
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format             = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    Clear.DepthStencil.Depth = 0.0f;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                               &Clear, __uuidof(ID3D12Resource), ReliefCover.VoidSlot()),
               TEXT("CreateCommittedResource(relief covers)")))
        return 0;
    ReliefCoverState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_DEPTH_STENCIL_VIEW_DESC Dsv;
    appMemzero(&Dsv, sizeof(Dsv));
    Dsv.Format        = Clear.Format;
    Dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    Device->CreateDepthStencilView(ReliefCover.Get(), &Dsv, ReliefCoverDsvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
    appMemzero(&Srv, sizeof(Srv));
    Srv.Format                  = bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT;
    Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Srv.Texture2D.MipLevels     = 1;
    Device->CreateShaderResourceView(ReliefCover.Get(), &Srv,
                                     Textures.GetCpuHandle(Textures.GetReservedIndex(ReliefCoverView)));
    RdxLog(TEXT("relief: the covers' depth made, %ix%i"), SceneWidth, SceneHeight);
    return 1;
}

// FTargetSwitch: the relief's first patches are next (FSolidPass::Flush). The depth drawn so far goes into the covers, and
// the covers are left bound as the only target, for the pass to draw the rest of the world's covers into. False - nothing
// bound, nothing changed - when there are none to make this frame: the relief then reads the view of nothing.
UBOOL FBackend::BeginReliefCovers(ID3D12GraphicsCommandList* Cmd)
{
    if (!bReliefCoverWanted || bReliefCoverOff || bOutputLive || MsaaWorldLive() || !EnsureReliefCover())
        return 0;
    ID3D12Resource* const Depth = (ActiveDepth == 1 && DepthBuffer2) ? DepthBuffer2.Get() : DepthBuffer.Get();
    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 2; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    B[0].Transition.pResource   = Depth;
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    B[1].Transition.pResource   = ReliefCover.Get();
    B[1].Transition.StateBefore = ReliefCoverState;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    Cmd->ResourceBarrier(2, B);
    Cmd->CopyResource(ReliefCover.Get(), Depth);
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    Cmd->ResourceBarrier(2, B);
    ReliefCoverState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = ReliefCoverDsvHeap->GetCPUDescriptorHandleForHeapStart();
    Cmd->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
    return 1;
}

// FTargetSwitch: the covers are drawn. Readable by the relief's domain shader from here on, and the scene's targets are
// bound again.
void FBackend::EndReliefCovers(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    if (ReliefCover && ReliefCoverState == D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        D3D12_RESOURCE_BARRIER B;
        appMemzero(&B, sizeof(B));
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.pResource   = ReliefCover.Get();
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &B);
        ReliefCoverState = B.Transition.StateAfter;
    }
    BindSceneTargets(Cmd, Dsv);
}

// Screen-space shadows (RequestShadows): the geometry pass's second target (FSolidPass, PSMainLit) - the light each surface
// receives, the scene target's size, one float. Created in the render target state and cleared to 1 each frame, so where
// nothing writes a light's share stays small. The shadow pass reads it (ApplyShadows), the depth view shows it (mode 4).
UBOOL FBackend::CreateLightTarget()
{
    LightTarget.Release();
    AddedTarget.Release();
    if (!LightRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 4;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), LightRtvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(light RTV)")))
            return 0;
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = SceneWidth;
    Desc.Height           = SceneHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = RDX_LIGHT_TARGET_FORMAT;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format   = RDX_LIGHT_TARGET_FORMAT;
    Clear.Color[0] = 1.0f;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               &Clear, __uuidof(ID3D12Resource), LightTarget.VoidSlot()),
               TEXT("CreateCommittedResource(light target)")))
        return 0;
    D3D12_RENDER_TARGET_VIEW_DESC View;
    appMemzero(&View, sizeof(View));
    View.Format        = RDX_LIGHT_TARGET_FORMAT;
    View.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = LightRtvHeap->GetCPUDescriptorHandleForHeapStart();
    Device->CreateRenderTargetView(LightTarget.Get(), &View, Rtv);
    // Slot 1: beside the output target at 2x, where the geometry pipelines still write two targets. Not a null view: D3D12
    // allows one only where the pipeline's format is UNKNOWN - measured, ERROR #613 on every HUD draw on the validation
    // layer. An output-sized target of the same format instead, written and never read.
    OutputLightTarget.Release();
    Rtv.ptr += RtvStride;
    if (HasOutputTarget())
    {
        Desc.Width  = Width;
        Desc.Height = Height;
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   &Clear, __uuidof(ID3D12Resource), OutputLightTarget.VoidSlot()),
                   TEXT("CreateCommittedResource(output light target)")))
            return 0;
        Device->CreateRenderTargetView(OutputLightTarget.Get(), &View, Rtv);
    }
    // Slots 2 and 3: the added target (FSolidPass's third) in the picture's format, cleared to zero each frame (BeginFrame),
    // and at 2x its output-sized one beside the output target, the same way as the light's slot 1.
    const DXGI_FORMAT AddedFmt = Colours.Draw;
    OutputAddedTarget.Release();
    Desc.Width  = SceneWidth;
    Desc.Height = SceneHeight;
    Desc.Format = AddedFmt;
    D3D12_CLEAR_VALUE Zero;
    appMemzero(&Zero, sizeof(Zero));
    Zero.Format = AddedFmt;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               &Zero, __uuidof(ID3D12Resource), AddedTarget.VoidSlot()),
               TEXT("CreateCommittedResource(added target)")))
        return 0;
    D3D12_RENDER_TARGET_VIEW_DESC AddedView;
    appMemzero(&AddedView, sizeof(AddedView));
    AddedView.Format        = AddedFmt;
    AddedView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE AddedRtv = LightRtvHeap->GetCPUDescriptorHandleForHeapStart();
    AddedRtv.ptr += 2 * RtvStride;
    Device->CreateRenderTargetView(AddedTarget.Get(), &AddedView, AddedRtv);
    if (HasOutputTarget())
    {
        Desc.Width  = Width;
        Desc.Height = Height;
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   &Zero, __uuidof(ID3D12Resource), OutputAddedTarget.VoidSlot()),
                   TEXT("CreateCommittedResource(output added target)")))
            return 0;
        AddedRtv.ptr += RtvStride;
        Device->CreateRenderTargetView(OutputAddedTarget.Get(), &AddedView, AddedRtv);
    }
    RdxLog(TEXT("shadows: light and added targets %ix%i%s"), SceneWidth, SceneHeight,
           HasOutputTarget() ? TEXT(", plus one output-sized pair for the HUD") : TEXT(""));
    bProbeViewsDirty = 1;       // the mirror probe's passes view the added target
    return 1;
}

void FBackend::BindSceneTargets(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    // With MSAA on, the WORLD draws into its own multisampled set, and the caller's single-sample depth view goes
    // with it: from here that one is a RESOLVE DESTINATION, not a draw target. The caller's choice of depth buffer
    // still matters - it says which one the world's depth will be resolved into - it just is not bound.
    // After the HUD's switch (bOutputLive) everything is single-sample again, including the view passed in.
    if (MsaaWorldLive())
    {
        D3D12_CPU_DESCRIPTOR_HANDLE Ms[3];
        Ms[0] = MsaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
        Ms[1] = Ms[0];
        Ms[1].ptr += RtvStride;
        Ms[2] = Ms[0];
        Ms[2].ptr += 2 * RtvStride;
        const D3D12_CPU_DESCRIPTOR_HANDLE MsDsv = MsaaDsvHeap->GetCPUDescriptorHandleForHeapStart();
        Cmd->OMSetRenderTargets((bShadows && MsaaLight && MsaaAdded) ? 3 : 1, Ms, FALSE, &MsDsv);
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtvs[3];
    Rtvs[0] = RtvStart;
    Rtvs[0].ptr += BACKEND_FRAME_COUNT * RtvStride;       // the scene target's view
    if (bShadows && LightTarget && AddedTarget && LightRtvHeap)
    {
        Rtvs[1] = LightRtvHeap->GetCPUDescriptorHandleForHeapStart();
        Rtvs[2] = Rtvs[1];
        Rtvs[2].ptr += 2 * RtvStride;                     // the added target (CreateLightTarget)
        Cmd->OMSetRenderTargets(3, Rtvs, FALSE, &Dsv);
    }
    else
        Cmd->OMSetRenderTargets(1, Rtvs, FALSE, &Dsv);
}

UBOOL FBackend::CreateSceneTarget()
{
    SceneColor.Release();
    WaterColor.Release();       // a copy of this target, made again on first use at the new size (EnsureWaterTargets)
    CausticMask.Release();      // the caustics' mask is of the scene's size too (EnsureCausticMask)
    FluidDepth.Release();       // the splashes' targets follow the scene's size (EnsureSplashes)
    FluidThick.Release();
    FluidSmooth.Release();
    Reactive.Release();         // and so does the reactive mask (EnsureReactive)
    ReactiveState     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    WaterSurface.Release();     // and the water's own depth (EnsureWaterSurface), its views with it
    WaterSurfaceState     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bWaterSurfaceLive     = 0;
    bWaterSurfaceCleared  = 0;
    bWaterSurfaceViewReal = 0;
    WetMark.Release();          // and the wet floors' mark (EnsureWetMark), its views with it
    WetMarkState          = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bWetMarkCleared       = 0;
    bWetMarkViewReal      = 0;
    bWetMarkNone          = 0;
    WaterShade.Release();       // and the water's shade for the shadows beside it (EnsureWaterShade)
    WaterShadeState        = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bWaterShadeLive        = 0;
    bWaterShadeViewReal    = 0;
    bWaterShadeFailed      = 0;
    bWaterShadowViewsDirty = 1;
    RdxViewsChanged();          // the passes' views of them go with them (ViewGeneration.h)

    if (!SceneSrvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        HeapDesc.NumDescriptors = 1;
        HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), SceneSrvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(scene SRV)")))
            return 0;
    }

    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Typeless storage: in the linear mode the frame is drawn through an sRGB view, while
    // the final pass always reads the stored numbers as they are.
    const DXGI_FORMAT DrawFmt = Colours.Draw;

    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = SceneWidth;
    Desc.Height           = SceneHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = Colours.Storage;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // The value BeginFrame clears to in play: the engine's screen colour, black, with alpha 1.
    // A different one here costs the fast clear every frame (debug layer warning #820).
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format   = DrawFmt;
    Clear.Color[3] = 1.0f;

    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &Clear,
                                               __uuidof(ID3D12Resource), SceneColor.VoidSlot()),
               TEXT("CreateCommittedResource(scene)")))
        return 0;

    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc;
    appMemzero(&RtvDesc, sizeof(RtvDesc));
    RtvDesc.Format        = DrawFmt;
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
    Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;
    Device->CreateRenderTargetView(SceneColor.Get(), &RtvDesc, Rtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc;
    appMemzero(&SrvDesc, sizeof(SrvDesc));
    SrvDesc.Format                  = Colours.Draw;
    SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SrvDesc.Texture2D.MipLevels     = 1;
    Device->CreateShaderResourceView(SceneColor.Get(), &SrvDesc,
                                     SceneSrvHeap->GetCPUDescriptorHandleForHeapStart());
    return 1;
}

// ---------------------------------------------------------------------------
// MSAA - the world's own, multisampled set of targets
//
// The idea in one line: the WORLD draws into these, and the moment it is done they are resolved into the targets
// everything else already reads. So the shadow pass, the motion vectors, the upscaler, the post filters and the HUD
// see exactly the single-sample pictures they have always seen - not one of them knows multisampling exists.
//
// Their sizes and formats are the single-sample ones', to the letter: a resolve needs both sides to agree.
// What this card really gives for a wanted sample count, with the rule applied: where the shadows need a
// shader-written stencil the card cannot do, THE MSAA GOES, not the shadows.
INT FBackend::PickMsaaCount(INT Wanted)
{
    if (!Device || Wanted <= 1)
        return 1;
    INT Got = 1;
    for (INT N = Wanted; N >= 2; N /= 2)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS Q;
        appMemzero(&Q, sizeof(Q));
        Q.Format      = Colours.Draw;
        Q.SampleCount = (UINT)N;
        if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &Q, sizeof(Q))) &&
            Q.NumQualityLevels > 0)
        {
            Got = N;
            break;
        }
        RDX_LOGF("antialiasing (MSAA): the card will not give %i samples - trying half of that", N);
    }
    if (Got > 1)
        RDX_LOGF("antialiasing (MSAA): asked for %i samples, got %i", Wanted, Got);
    if (Got > 1 && bShadows && !bStencilRef)
    {
        RDX_LOG("antialiasing (MSAA): shadows need a shader-written stencil and this card cannot do it - "
                "MSAA is dropped, the shadows stay");
        Got = 1;
    }
    return Got;
}

// The sample count and everything that hangs off it, in one place, so the pipelines and the targets can never
// disagree - a pipeline drawing into targets of a different sample count fails on every triangle.
UBOOL FBackend::ApplyMsaaCount(INT Count, UBOOL bMakeTargets)
{
    const INT Want = (Count > 1) ? Count : 1;
    if (Want == MsaaCount)
        return 1;
    MsaaCount = Want;
    Solid.SetMsaaSamples(MsaaCount);
    if (!Solid.RebuildPipelines(Device.Get()))
    {
        RDX_LOG("MSAA: the draw pipelines could not be created with the new sample count - falling back to one sample");
        MsaaCount = 1;
        Solid.SetMsaaSamples(1);
        Solid.RebuildPipelines(Device.Get());
        ReleaseMsaaTargets();
        return 0;
    }
    if (MsaaCount > 1 && !Final.InitMsaaDepth(Device.Get(), MsaaCount, bShadows))
    {
        RDX_LOG("MSAA: the depth resolve pass could not be created - MSAA is dropped");
        MsaaCount = 1;
        Solid.SetMsaaSamples(1);
        Solid.RebuildPipelines(Device.Get());
        ReleaseMsaaTargets();
        return 0;
    }
    return bMakeTargets ? CreateMsaaTargets() : 1;
}

void FBackend::ReleaseMsaaTargets()
{
    MsaaColor.Release();
    MsaaLight.Release();
    MsaaAdded.Release();
    MsaaDepth.Release();
}

UBOOL FBackend::CreateMsaaTargets()
{
    ReleaseMsaaTargets();
    if (MsaaCount <= 1)
        return 1;

    // Shadows mark the surface kind in the stencil, and a multisampled stencil can only be brought down to one
    // sample from a pixel shader (SV_StencilRef). Where the card cannot do that, MSAA is what gets dropped - never
    // the shadows.
    if (bShadows && !bStencilRef)
    {
        RDX_LOG("MSAA: this card cannot write stencil from a shader (PSSpecifiedStencilRef), which the shadows need "
                "- MSAA is dropped, the shadows stay");
        MsaaCount = 1;
        return 1;
    }

    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    const DXGI_FORMAT DrawFmt = Colours.Draw;

    if (!MsaaRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 3;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), MsaaRtvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(MSAA RTV)")))
            return 0;
    }
    if (!MsaaDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        HeapDesc.NumDescriptors = 1;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), MsaaDsvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(MSAA DSV)")))
            return 0;
    }
    if (!MsaaSrvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        HeapDesc.NumDescriptors = 2;
        HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), MsaaSrvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(MSAA SRV)")))
            return 0;
    }

    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = SceneWidth;
    Desc.Height           = SceneHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = (UINT)MsaaCount;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));

    const UINT RtvInc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = MsaaRtvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc;
    appMemzero(&RtvDesc, sizeof(RtvDesc));
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;

    // 1. The colour. Typeless storage like the single-sample one, drawn through the same sRGB view.
    Desc.Format    = Colours.Storage;
    Clear.Format   = DrawFmt;
    Clear.Color[0] = Clear.Color[1] = Clear.Color[2] = 0.0f;
    Clear.Color[3] = 1.0f;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &Clear,
                                               __uuidof(ID3D12Resource), MsaaColor.VoidSlot()),
               TEXT("CreateCommittedResource(MSAA scene)")))
        return 0;
    RtvDesc.Format = DrawFmt;
    Device->CreateRenderTargetView(MsaaColor.Get(), &RtvDesc, Rtv);
    Rtv.ptr += RtvInc;

    // 2 and 3. The light and the added target, only with the shadows on - they are the world pass's second and third
    // outputs, and without shadows the pass has one.
    if (bShadows && LightTarget && AddedTarget)
    {
        Desc.Format    = RDX_LIGHT_TARGET_FORMAT;
        Clear.Format   = RDX_LIGHT_TARGET_FORMAT;
        // Exactly what BeginFrame clears it to - 1 in red and nothing else. A value that differs even in an unused
        // channel costs the fast clear on every frame (debug layer warning #820, measured: 1000+ times a run).
        Clear.Color[0] = 1.0f;
        Clear.Color[1] = Clear.Color[2] = Clear.Color[3] = 0.0f;
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET, &Clear,
                                                   __uuidof(ID3D12Resource), MsaaLight.VoidSlot()),
                   TEXT("CreateCommittedResource(MSAA light)")))
            return 0;
        RtvDesc.Format = RDX_LIGHT_TARGET_FORMAT;
        Device->CreateRenderTargetView(MsaaLight.Get(), &RtvDesc, Rtv);
        Rtv.ptr += RtvInc;

        Desc.Format    = DrawFmt;
        Clear.Format   = DrawFmt;
        Clear.Color[0] = Clear.Color[1] = Clear.Color[2] = Clear.Color[3] = 0.0f;
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET, &Clear,
                                                   __uuidof(ID3D12Resource), MsaaAdded.VoidSlot()),
                   TEXT("CreateCommittedResource(MSAA added)")))
            return 0;
        RtvDesc.Format = DrawFmt;
        Device->CreateRenderTargetView(MsaaAdded.Get(), &RtvDesc, Rtv);
    }

    // 4. The depth, and with the shadows on the stencil beside it. Typeless whatever the single-sample pair is: the
    // resolve pass reads this one as a texture in every frame, and writes those.
    Desc.Format = bShadows ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
    Desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format             = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    Clear.DepthStencil.Depth = 0.0f;      // reversed-Z: the far plane is 0
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &Clear,
                                               __uuidof(ID3D12Resource), MsaaDepth.VoidSlot()),
               TEXT("CreateCommittedResource(MSAA depth)")))
        return 0;

    D3D12_DEPTH_STENCIL_VIEW_DESC DsvDesc;
    appMemzero(&DsvDesc, sizeof(DsvDesc));
    DsvDesc.Format        = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    DsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    Device->CreateDepthStencilView(MsaaDepth.Get(), &DsvDesc, MsaaDsvHeap->GetCPUDescriptorHandleForHeapStart());

    // The two views the resolve pass reads: the depth, and the stencil plane beside it.
    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc;
    appMemzero(&SrvDesc, sizeof(SrvDesc));
    SrvDesc.Format                  = bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT;
    SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const UINT SrvInc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE Srv = MsaaSrvHeap->GetCPUDescriptorHandleForHeapStart();
    Device->CreateShaderResourceView(MsaaDepth.Get(), &SrvDesc, Srv);
    Srv.ptr += SrvInc;
    if (bShadows)
    {
        SrvDesc.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        Device->CreateShaderResourceView(MsaaDepth.Get(), &SrvDesc, Srv);
    }
    else
    {
        // Something valid has to sit in the slot even with no stencil: the pass binds a table of two either way.
        Device->CreateShaderResourceView(MsaaDepth.Get(), &SrvDesc, Srv);
    }

    RDX_LOGF("MSAA: %i samples per pixel, the world has its own targets at %ix%i - resolved when the world ends, after "
             "which nothing changes", MsaaCount, SceneWidth, SceneHeight);
    return 1;
}

INT FBackend::PrepareTexture(const FTextureInfo& Info, DWORD PolyFlags, UBOOL bSrgb, UBOOL bKeepAlpha)
{
    // Uploads are recorded onto the frame's own command list, so this is only
    // valid between BeginFrame and EndFrame.
    if (!bInFrame)
        return -1;
    return Textures.Prepare(Info, PolyFlags, bSrgb, CommandList.Get(), bKeepAlpha);
}

UBOOL FBackend::PreparePom(INT Entry)
{
    // Like a texture upload, this is recorded onto the frame's own command list, so it is only valid between
    // BeginFrame and EndFrame.
    if (!bInFrame)
        return 0;
    return Pom.Prepare(Entry, CommandList.Get(), Textures);
}

UBOOL FBackend::OpenUploadList()
{
    if (bUploadOpen)
        return 1;
    if (!Device)
        return 0;

    if (!UploadAllocator &&
        FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              __uuidof(ID3D12CommandAllocator), (void**)&UploadAllocator)))
        return 0;

    if (!UploadList)
    {
        if (FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator, nullptr,
                                             __uuidof(ID3D12GraphicsCommandList), UploadList.VoidSlot())))
            return 0;
        bUploadOpen = 1;     // created in the recording state
        return 1;
    }

    if (FAILED(UploadAllocator->Reset()) || FAILED(UploadList->Reset(UploadAllocator, nullptr)))
        return 0;
    bUploadOpen = 1;
    return 1;
}

void FBackend::SubmitUploadList()
{
    if (!bUploadOpen)
        return;
    bUploadOpen = 0;

    if (SUCCEEDED(UploadList->Close()))
    {
        ID3D12CommandList* Lists[1] = { UploadList.Get() };
        Queue->ExecuteCommandLists(1, Lists);
    }

    // Wait, and free what these copies used - now, not with a frame slot. The list goes to
    // the queue AFTER the previous frame's fence was signalled, so that fence says nothing
    // about it: releasing its staging when the slot next comes round could pull memory
    // from under copies still running. Out-of-frame uploads are a level's precache, so a
    // stall here costs a moment at load and nothing during play.
    const DOUBLE U0 = appSeconds();
    WaitForGpu();
    const DOUBLE UploadWait = appSeconds() - U0;
    WaitUploads++;
    WaitUploadSum += UploadWait;
    WaitUploadMax  = Max(WaitUploadMax, UploadWait);
    Textures.ReleaseCurrentStaging();
}

ID3D12Resource* FBackend::MakeCardBuffer(UINT64 Bytes, const TCHAR* What)
{
    if (!Device || Bytes == 0)
        return nullptr;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = Bytes;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_UNKNOWN;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* Made = nullptr;
    const HRESULT   Hr   = Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                           __uuidof(ID3D12Resource), (void**)&Made);
    if (FAILED(Hr) || !Made)
    {
        RdxLog(TEXT("card buffer: the %s could not be made on the card's own memory (%I64u KB, 0x%08X)"), What,
               Bytes >> 10, (INT)Hr);
        return nullptr;
    }
    return Made;
}

UBOOL FBackend::WriteCardBuffer(ID3D12Resource* Dest, UINT64 At, const void* Data, UINT64 Bytes)
{
    if (!Device || !Queue || !Dest || (!Data && Bytes))
        return 0;
    if (!CardStaging)
    {
        D3D12_HEAP_PROPERTIES Heap;
        appMemzero(&Heap, sizeof(Heap));
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc;
        appMemzero(&Desc, sizeof(Desc));
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = (UINT64)CARD_STAGING_BYTES;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc.Count = 1;
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_RANGE NoRead = { 0, 0 };
        if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   __uuidof(ID3D12Resource), CardStaging.VoidSlot())) ||
            FAILED(CardStaging->Map(0, &NoRead, (void**)&CardStagingMapped)) || !CardStagingMapped)
        {
            CardStaging.Release();
            CardStagingMapped = nullptr;
            RdxLog(TEXT("card buffer: no staging buffer to write through"));
            return 0;
        }
    }
    if (!CardAllocator && FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                __uuidof(ID3D12CommandAllocator),
                                                                CardAllocator.VoidSlot())))
        return 0;
    if (!CardList)
    {
        // Made recording; closed at once, so that every piece below begins the same way.
        if (FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CardAllocator.Get(), nullptr,
                                             __uuidof(ID3D12GraphicsCommandList), CardList.VoidSlot())) ||
            FAILED(CardList->Close()))
        {
            CardList.Release();
            return 0;
        }
    }
    const BYTE* const From = (const BYTE*)Data;
    for (UINT64 Done = 0; Done < Bytes;)
    {
        const UINT64 Piece = Min<UINT64>(Bytes - Done, (UINT64)CARD_STAGING_BYTES);
        appMemcpy(CardStagingMapped, From + Done, (INT)Piece);
        if (FAILED(CardAllocator->Reset()) || FAILED(CardList->Reset(CardAllocator.Get(), nullptr)))
            return 0;
        CardList->CopyBufferRegion(Dest, At + Done, CardStaging.Get(), 0, Piece);
        if (FAILED(CardList->Close()))
            return 0;
        ID3D12CommandList* Lists[1] = { CardList.Get() };
        Queue->ExecuteCommandLists(1, Lists);
        // The staging buffer is the next piece's, and the allocator the next list's: the card is done with this one
        // first.
        WaitForGpu();
        Done += Piece;
    }
    return 1;
}

void FBackend::ReleaseCardWriter()
{
    CardList.Release();
    CardAllocator.Release();
    if (CardStaging && CardStagingMapped)
        CardStaging->Unmap(0, nullptr);
    CardStaging.Release();
    CardStagingMapped = nullptr;
}

INT FBackend::PrepareTexturePrecache(const FTextureInfo& Info, DWORD PolyFlags, UBOOL bSrgb)
{
    // Inside a frame the ordinary path is right; this only exists for the between-frames
    // case, which is where the engine actually precaches.
    if (bInFrame)
        return Textures.Prepare(Info, PolyFlags, bSrgb, CommandList.Get());
    if (!OpenUploadList())
        return -1;

    const INT Srv = Textures.Prepare(Info, PolyFlags, bSrgb, UploadList.Get());

    // All of a level's textures arrive between two frames, so they all land in the same
    // staging list. Once it fills, every further upload is refused and that texture is
    // left to be uploaded later, in the middle of play. Measured: 1063 uploaded and then
    // hundreds refused. Flushing and waiting here costs a moment at load and removes the
    // hitch that would otherwise appear when the view turns onto something new.
    if (Textures.GetStagingCount() >= Textures.GetStagingLimit() - 16 || Textures.RingNearlyFull())
    {
        SubmitUploadList();
        WaitForGpu();
        Textures.ReleaseCurrentStaging();
    }
    return Srv;
}

void FBackend::FlushTextures()
{
    if (!Device)
        return;
    SubmitUploadList();
    WaitForGpu();
    Textures.ReleaseAll();
    ReleaseTreeMesh();
    PresentsAfterFlush = 40;
}

BYTE* FBackend::TreeMeshBuffer(INT Bytes, D3D12_GPU_VIRTUAL_ADDRESS& Address)
{
    Address = 0;
    if (!Device || Bytes <= 0 || TreeMesh)
        return nullptr;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = (UINT64)Bytes;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_UNKNOWN;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, __uuidof(ID3D12Resource), (void**)&TreeMeshStaging)))
    {
        TreeMeshStaging = nullptr;
        RdxLog(TEXT("trees: no buffer of %i KB for the level's plants - the game's own stay"), Bytes >> 10);
        return nullptr;
    }
    const D3D12_RANGE NoRead = { 0, 0 };
    if (FAILED(TreeMeshStaging->Map(0, &NoRead, (void**)&TreeMeshMapped)))
    {
        TreeMeshStaging->Release();
        TreeMeshStaging = nullptr;
        TreeMeshMapped  = nullptr;
        return nullptr;
    }
    // The mesh the plants are drawn from on the card's own memory: every pass that lays a plant reads all of its
    // vertices each frame, and across the bus from the CPU's memory that is slow for a level of grown plants. What is
    // written here is copied there (TreeMeshWritten). Where the card has no room, or no list is open for the copy, the
    // plants are drawn from the written memory itself.
    D3D12_HEAP_PROPERTIES Card = Heap;
    Card.Type = D3D12_HEAP_TYPE_DEFAULT;
    if ((bInFrame || OpenUploadList()) &&
        SUCCEEDED(Device->CreateCommittedResource(&Card, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON,
                                                  nullptr, __uuidof(ID3D12Resource), (void**)&TreeMesh)))
        TreeMeshBytes = Bytes;
    else
    {
        TreeMesh        = TreeMeshStaging;
        TreeMeshStaging = nullptr;
        TreeMeshBytes   = 0;
        RdxLog(TEXT("trees: the level's plants drawn from the CPU's memory - no room for %i KB on the card"), Bytes >> 10);
    }
    Address = TreeMesh->GetGPUVirtualAddress();
    return TreeMeshMapped;
}

// The plants' mesh written into TreeMeshBuffer's memory: copied onto the card's - into the frame's list, ahead of every
// draw the frame batches (they are put on the list at its end), or between frames into the upload list, sent and
// waited for. The written memory goes once the card is past that frame (BeginFrame).
void FBackend::TreeMeshWritten()
{
    if (!TreeMesh || !TreeMeshStaging || TreeMeshBytes <= 0)
        return;
    ID3D12GraphicsCommandList* const Cmd = bInFrame ? CommandList.Get() : (OpenUploadList() ? UploadList.Get() : nullptr);
    if (!Cmd)
        return;
    // A buffer is made in the common state and taken to the copy's own by the copy itself; from there to the draws'.
    Cmd->CopyBufferRegion(TreeMesh, 0, TreeMeshStaging, 0, (UINT64)TreeMeshBytes);
    D3D12_RESOURCE_BARRIER Bar;
    appMemzero(&Bar, sizeof(Bar));
    Bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Bar.Transition.pResource   = TreeMesh;
    Bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;
    Cmd->ResourceBarrier(1, &Bar);
    TreeMeshBytes = 0;
    if (bInFrame)
        TreeMeshStagingFrames = BACKEND_FRAME_COUNT + 2;
    else
    {
        SubmitUploadList();
        ReleaseTreeMeshStaging();
    }
}

void FBackend::ReleaseTreeMeshStaging()
{
    if (!TreeMeshStaging)
        return;
    TreeMeshStaging->Unmap(0, nullptr);
    TreeMeshStaging->Release();
    TreeMeshStaging       = nullptr;
    TreeMeshMapped        = nullptr;
    TreeMeshStagingFrames = 0;
}

void FBackend::ReleaseTreeMesh()
{
    if (!TreeMesh && !TreeMeshStaging)
        return;
    ReleaseTreeMeshStaging();
    // Drawn from the written memory itself (no room on the card): that is TreeMesh, and still mapped.
    if (TreeMesh && TreeMeshMapped)
        TreeMesh->Unmap(0, nullptr);
    if (TreeMesh)
        TreeMesh->Release();
    TreeMesh       = nullptr;
    TreeMeshMapped = nullptr;
    TreeMeshBytes  = 0;
    TreeMeshGeneration++;
}

// The plants' mesh let go while the game runs - the way they grow changed, and they grow again into a new one. The
// frames already sent may still draw it: the card is waited for first. A full stall, once, at a frame's start.
void FBackend::DropTreeMesh()
{
    if (!Device || !TreeMesh)
        return;
    WaitForGpu();
    ReleaseTreeMesh();
}

void FBackend::ClearDepth()
{
    // NOT recorded on the command list here. Every draw of the frame is batched and
    // submitted in one go at EndFrame, so a clear recorded at this moment would land
    // before all of them and change nothing. It goes into the run list instead, and the
    // flush performs it in the place the engine asked for.
    if (bInFrame)
        Solid.AddDepthClear();
}

// FTargetSwitch: a mirror's view of the level has drawn all it has (FSolidPass::Flush, at the run MirrorViewDone
// left). What is laid over the whole of it goes on while its own depth is there, in the world's view's order: the
// rooms' fog from the mirror on - the way from the eye to the mirror is laid with the world's own view, at its end
// (DrawFogRooms); the frame's fog lights are the world's view's, by now this frame's - and then the walk of what
// the view gathered (WalkMirrorView). Not with the world drawn multisampled: that depth is not read.
void FBackend::MirrorViewEnd(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv, INT Index)
{
    if (!Cmd || !Device || Index < 0 || Index >= MirrorEndCount || bOutputLive || MsaaWorldLive())
        return;
    const FMirrorEnd&  E = MirrorEnds[Index];
    const FLOAT* const M = E.View;
    // No mirror's plane with the view: it is no mirror's - the world's own, asked for as a measurement - and what is
    // laid over it is the world's to lay.
    if (M[12] <= 1.0f || E.Plane[0] * E.Plane[0] + E.Plane[1] * E.Plane[1] + E.Plane[2] * E.Plane[2] < 0.25f)
        return;
    const UBOOL bFog  = (FogLightCount > 0 && FogRoomsCanRun()) ? 1 : 0;
    const UBOOL bWalk = (bDepthReadable && ((Solid.GetCloudsOn() && E.PuffCount > 0) || E.FireCount > 0 ||
                                            E.FallCount > 0 || OwnBeamsNow())) ? 1 : 0;
    const UBOOL bKeep = (bBounceNow && bDepthReadable && E.TriCount > 0) ? 1 : 0;
    if (!bFog && !bWalk && !bKeep)
        return;
    // The view's own depth, read where it stands: by the fog's pixel shader and by the walk's compute shader.
    ID3D12Resource* const Depth = (ActiveDepth == 1 && DepthBuffer2) ? DepthBuffer2.Get() : DepthBuffer.Get();
    const D3D12_RESOURCE_STATES Read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Depth;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B.Transition.StateAfter  = Read;
    Cmd->ResourceBarrier(1, &B);

    if (bFog)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
        Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;      // the scene target's view (BindSceneTargets)
        Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
        const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
        const D3D12_RECT     Sc = { 0, 0, SceneWidth, SceneHeight };
        Cmd->RSSetViewports(1, &Vp);
        Cmd->RSSetScissorRects(1, &Sc);

        FFogRooms::FView V;
        appMemzero(&V, sizeof(V));
        V.Screen[0]      = M[13];
        V.Screen[1]      = M[14];
        V.Focal          = M[12];
        V.EngPerPixel[0] = (FLOAT)Max(Width, 1) / (FLOAT)Max(SceneWidth, 1);
        V.EngPerPixel[1] = (FLOAT)Max(Height, 1) / (FLOAT)Max(SceneHeight, 1);
        for (INT i = 0; i < 3; i++)
        {
            V.Eye[i]   = M[i];
            V.AxisX[i] = M[3 + i];
            V.AxisY[i] = M[6 + i];
            V.AxisZ[i] = M[9 + i];
        }
        const UBOOL bLaid = FogRoom.Record(Cmd, Device.Get(), Colours.Draw, Depth,
                                           bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT,
                                           nullptr, V, FogLights, FogLightCount, FrameIndex, E.Plane);
        // Now and then, whether it runs at all: how many mirrors' views the fog was laid over.
        static INT Laid = 0, Asked = 0;
        Laid += bLaid ? 1 : 0;
        if ((Asked++ % 600) == 0)
            RdxLog(TEXT("fog rooms: laid over %i of %i mirrors' views so far, from the mirror on - %i lights now"),
                   Laid, Asked, FogLightCount);
    }
    if (bWalk)
        WalkMirrorView(Cmd, Index, Depth);
    // The view's depth into the mirrors' map, inside the mirror's own polygons: by it the world's view's surfaces
    // take the flashlight's light off this mirror. And the lamp itself as the mirror shows it - the eye's own image,
    // twice as far as the mirror along the mirror's normal - by how strongly the cone shines that way (FBeamBounce).
    if (bKeep)
    {
        const FLOAT Sx = (FLOAT)Max(SceneWidth, 1) / (FLOAT)Max(Width, 1);
        const FLOAT Sy = (FLOAT)Max(SceneHeight, 1) / (FLOAT)Max(Height, 1);
        FBeamBounce::FView V;
        V.Screen[0] = CloudScreen[0] * Sx;
        V.Screen[1] = CloudScreen[1] * Sy;
        V.Focal     = CloudFocal * Sx;
        V.W         = SceneWidth;
        V.H         = SceneHeight;
        const UBOOL bKept = Bounce.Keep(Cmd, Device.Get(), Depth,
                                        bShadows ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT,
                                        (INT)FrameIndex, MirrorTris + E.TriFirst * 3, E.TriCount, Index + 1, V);
        const FLOAT* const N = E.ViewPlane;
        if (bKept && N[2] > 0.05f && N[3] > 1.0f)
        {
            const FLOAT* const Cone = BeamBlockNow;
            const FLOAT Hot   = Clamp((N[2] - Cone[4]) * Cone[5], 0.0f, 1.0f);
            const FLOAT Spill = Clamp((N[2] - Cone[6]) * Cone[7], 0.0f, 1.0f);
            const FLOAT HotS  = Hot * Hot * (3.0f - 2.0f * Hot);
            const FLOAT Most  = Max(Max(Cone[0], Cone[1]), Max(Cone[2], 1e-4f));
            FBeamBounce::FLamp L;
            L.Depth     = 2.0f * N[3] * N[2];
            L.Centre[0] = V.Screen[0] + V.Focal * N[0] / N[2];
            L.Centre[1] = V.Screen[1] + V.Focal * N[1] / N[2];
            L.Core      = Max(V.Focal * FBeamBounce::LampRadius / L.Depth, 1.5f);
            L.Halo      = V.Focal * FBeamBounce::GlareRadius / L.Depth;
            L.Strength  = HotS + (1.0f - HotS) * Cone[11] * Spill * Spill * (3.0f - 2.0f * Spill);
            for (INT k = 0; k < 3; k++)
                L.Colour[k] = Cone[k] / Most;
            D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
            Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;      // the scene target's view (BindSceneTargets)
            Bounce.Glare(Cmd, Device.Get(), Colours.Draw, Rtv, Index + 1, L, V);
        }
        // Now and then, whether it runs at all.
        static INT Kept = 0, Asked = 0;
        Kept += bKept ? 1 : 0;
        if ((Asked++ % 600) == 0)
            RdxLog(TEXT("flashlight off mirrors: the depth of %i of %i mirrors' views kept so far - this one's mirror "
                        "is %i triangles, its plane %.3f %.3f %.3f at %.1f from the eye"),
                   Kept, Asked, E.TriCount / 3, N[0], N[1], N[2], N[3]);
    }

    B.Transition.StateBefore = Read;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    Cmd->ResourceBarrier(1, &B);
    // The frame goes on in this target with the viewport and the scissor it began with (BeginFrame): the solid pass
    // sets neither, and the walk's laying-on cut the picture to the mirror's part of it.
    const D3D12_VIEWPORT Whole = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const D3D12_RECT     All   = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &Whole);
    Cmd->RSSetScissorRects(1, &All);
    BindSceneTargets(Cmd, Dsv);
}

void FBackend::SetBrightness(FLOAT Brightness)
{
    OutputExponent = RdxMath::OutputExponent(Brightness);

    // Say so when it changes: whether the setting arrives at all is a fact, not a hope.
    static FLOAT Last = -1.0f;
    if (Brightness != Last)
    {
        Last = Brightness;
        RdxLog(TEXT("brightness: %.3f -> exponent %.3f"), Brightness, OutputExponent);
    }
}

void FBackend::SetOutputViewport(ID3D12GraphicsCommandList* Cmd) const
{
    D3D12_VIEWPORT Viewport;
    Viewport.TopLeftX = 0.0f;
    Viewport.TopLeftY = 0.0f;
    Viewport.Width    = (FLOAT)Width;
    Viewport.Height   = (FLOAT)Height;
    Viewport.MinDepth = 0.0f;
    Viewport.MaxDepth = 1.0f;
    Cmd->RSSetViewports(1, &Viewport);

    D3D12_RECT Rect;
    Rect.left   = 0;
    Rect.top    = 0;
    Rect.right  = Width;
    Rect.bottom = Height;
    Cmd->RSSetScissorRects(1, &Rect);
}

UBOOL FBackend::CreateOutputTarget()
{
    OutputColor.Release();
    OutputDepth.Release();
    RdxViewsChanged();          // the passes' views of them go with them (ViewGeneration.h)

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
    appMemzero(&HeapDesc, sizeof(HeapDesc));
    if (!OutputRtvHeap)
    {
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 2;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), OutputRtvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(output RTV)")))
            return 0;
    }
    if (!OutputDsvHeap)
    {
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        HeapDesc.NumDescriptors = 1;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), OutputDsvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(output DSV)")))
            return 0;
    }
    if (!OutputSrvHeap)
    {
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        HeapDesc.NumDescriptors = 1;
        HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), OutputSrvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(output SRV)")))
            return 0;
    }

    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // The scene target's own arrangement (CreateSceneTarget): typeless storage, drawn into through the
    // scene's format, read as the stored numbers. Between frames it rests readable.
    const DXGI_FORMAT DrawFmt = Colours.Draw;

    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = Width;
    Desc.Height           = Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = Colours.Storage;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // No clear value: nothing clears it - the box filter's full-screen triangle covers every pixel.
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                               __uuidof(ID3D12Resource), OutputColor.VoidSlot()),
               TEXT("CreateCommittedResource(output)")))
        return 0;

    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc;
    appMemzero(&RtvDesc, sizeof(RtvDesc));
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = OutputRtvHeap->GetCPUDescriptorHandleForHeapStart();
    RtvDesc.Format = DrawFmt;
    Device->CreateRenderTargetView(OutputColor.Get(), &RtvDesc, Rtv);
    Rtv.ptr += RtvStride;
    RtvDesc.Format = Colours.BackBuffer;
    Device->CreateRenderTargetView(OutputColor.Get(), &RtvDesc, Rtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc;
    appMemzero(&SrvDesc, sizeof(SrvDesc));
    SrvDesc.Format                  = Colours.BackBuffer;
    SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SrvDesc.Texture2D.MipLevels     = 1;
    Device->CreateShaderResourceView(OutputColor.Get(), &SrvDesc,
                                     OutputSrvHeap->GetCPUDescriptorHandleForHeapStart());

    // Its depth buffer, for 3D that still comes after the HUD's first tile. Reversed-Z, cleared to 0 like
    // the scene's (CreateDepthBuffer).
    // With the shadows on, the geometry pass's pipelines expect a stencil in every depth buffer they draw with.
    Desc.Format = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    Desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format             = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    Clear.DepthStencil.Depth = 0.0f;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &Clear,
                                               __uuidof(ID3D12Resource), OutputDepth.VoidSlot()),
               TEXT("CreateCommittedResource(output depth)")))
        return 0;

    D3D12_DEPTH_STENCIL_VIEW_DESC DsvDesc;
    appMemzero(&DsvDesc, sizeof(DsvDesc));
    DsvDesc.Format        = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    DsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    Device->CreateDepthStencilView(OutputDepth.Get(), &DsvDesc, OutputDsvHeap->GetCPUDescriptorHandleForHeapStart());

    // The middle picture, at the output's size in the back buffer's format: the resolve draws into it when a post
    // filter follows (the filter cannot read and write the same texture). Not needed when the filter reads the scene
    // itself - see ResolveScene.
    UpscaleTemp.Release();
    // Where it cannot be made, the renderer does NOT stop: every place that uses it already asks whether it is there -
    // the post filter falls back to reading the scene itself (ResolveScene), SMAA to not running at all. Only the
    // making of it used to be fatal, and it was the one step in the chain that had no business being: this is a better
    // picture, not a picture. Tried with its create forced to fail (the marker SecondRevision-fail-alloc.txt).
    if (PostAaKind && (SceneScale > 1 || UpscaleMode))
    {
        UBOOL bTemp = 1;
        if (!UpscaleRtvHeap)
        {
            appMemzero(&HeapDesc, sizeof(HeapDesc));
            HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            HeapDesc.NumDescriptors = 1;
            if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), UpscaleRtvHeap.VoidSlot()),
                       TEXT("CreateDescriptorHeap(upscale RTV)")))
                bTemp = 0;
        }
        if (bTemp && !UpscaleSrvHeap)
        {
            appMemzero(&HeapDesc, sizeof(HeapDesc));
            HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            HeapDesc.NumDescriptors = 1;
            HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), UpscaleSrvHeap.VoidSlot()),
                       TEXT("CreateDescriptorHeap(upscale SRV)")))
                bTemp = 0;
        }
        if (bTemp)
        {
        D3D12_RESOURCE_DESC TempDesc;
        appMemzero(&TempDesc, sizeof(TempDesc));
        TempDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        TempDesc.Width            = Width;
        TempDesc.Height           = Height;
        TempDesc.DepthOrArraySize = 1;
        TempDesc.MipLevels        = 1;
        TempDesc.Format           = Colours.BackBuffer;
        TempDesc.SampleDesc.Count = 1;
        TempDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &TempDesc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   __uuidof(ID3D12Resource), UpscaleTemp.VoidSlot()),
                   TEXT("CreateCommittedResource(upscale temp)")))
            bTemp = 0;
        }
        if (bTemp && UpscaleTemp)
        {
            Device->CreateRenderTargetView(UpscaleTemp.Get(), nullptr, UpscaleRtvHeap->GetCPUDescriptorHandleForHeapStart());
            Device->CreateShaderResourceView(UpscaleTemp.Get(), nullptr, UpscaleSrvHeap->GetCPUDescriptorHandleForHeapStart());
        }
        else
        {
            UpscaleTemp.Release();
            RdxLog(TEXT("upscaling: no room for the intermediate picture at %ix%i - the post filter reads the scene "
                        "itself"), Width, Height);
        }
    }

    RDX_LOGF("output target %ix%i (the HUD and the final image; the scene is %ix%i)%s", Width, Height, SceneWidth, SceneHeight,
             UpscaleTemp ? TEXT(", plus the intermediate image") : TEXT(""));
    return 1;
}

// Motion vectors (RequestMotionVectors): the target, R16G16_FLOAT at the scene's size - two numbers a pixel, in pixels.
// The targets themselves. EnsureMotionTarget below is what everything calls; this one is asked once a size.
UBOOL FBackend::MakeMotionTarget()
{
    MotionTarget.Release();
    RdxViewsChanged();          // the motion blur's view of it goes with it (ViewGeneration.h)
    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
    if (!MotionRtvHeap)
    {
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 1;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), MotionRtvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(motion RTV)")))
            return 0;
    }
    if (!MotionSrvHeap)
    {
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        HeapDesc.NumDescriptors = 1;
        HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), MotionSrvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(motion SRV)")))
            return 0;
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = SceneWidth;
    Desc.Height           = SceneHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R16G16_FLOAT;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    // The value DrawMotionVectors clears it to when there is no previous camera to measure against - named here, so
    // the clear is the fast one the driver prepared for (the debug layer's #820 otherwise).
    D3D12_CLEAR_VALUE NoMotion;
    appMemzero(&NoMotion, sizeof(NoMotion));
    NoMotion.Format = DXGI_FORMAT_R16G16_FLOAT;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &NoMotion,
                                               __uuidof(ID3D12Resource), MotionTarget.VoidSlot()),
               TEXT("CreateCommittedResource(motion)")))
        return 0;
    Device->CreateRenderTargetView(MotionTarget.Get(), nullptr, MotionRtvHeap->GetCPUDescriptorHandleForHeapStart());
    Device->CreateShaderResourceView(MotionTarget.Get(), nullptr, MotionSrvHeap->GetCPUDescriptorHandleForHeapStart());
    MotionW = SceneWidth;
    MotionH = SceneHeight;
    RdxLog(TEXT("motion vectors: target %ix%i"), MotionW, MotionH);
    return 1;
}

// Asked once a size. A create the driver refused is refused for a reason - out of video memory, almost always - and
// asking again on the next frame does not change that; it only costs the asking and fills the log. Measured with the
// create forced to fail (the marker SecondRevision-fail-alloc.txt): 4133 refusals in one short run before this.
UBOOL FBackend::EnsureMotionTarget()
{
    if (MotionTarget && MotionW == SceneWidth && MotionH == SceneHeight)
        return 1;
    if (bMotionFailed && MotionFailedW == SceneWidth && MotionFailedH == SceneHeight)
        return 0;
    if (MakeMotionTarget())
    {
        bMotionFailed = 0;
        return 1;
    }
    bMotionFailed = 1;
    MotionFailedW = SceneWidth;
    MotionFailedH = SceneHeight;
    RdxLog(TEXT("motion vectors: no target at %ix%i - the frame goes on without them, and this is not asked again "
                "until the size changes"), SceneWidth, SceneHeight);
    return 0;
}

// One pass at the frame's end, with the world's depth readable: where each pixel was on the previous frame. Without a
// previous camera - the first frame after a load, or a frame with no world - the target is cleared to zero instead.
void FBackend::DrawMotionVectors(ID3D12GraphicsCommandList* Cmd)
{
    // Same reason as the jitter's: with the mode switched off in play nobody reads these vectors, so they are not
    // drawn. bMotionForced keeps the diagnostic view (the marker) working whatever the upscaler does.
    if ((!bMotionWanted && !bMotionForced) || (!UpscaleMode && !bMotionForced) || bMotionDone || !Cmd || !Device ||
        !DepthSrvHeap || !DepthBuffer || !DepthBuffer2)   // the pass's own two timestamps are 6 and 7
        return;
    if (MotionReady == 0)
    {
        MotionReady = Final.InitMotion(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("motion vectors: the pipeline %s"), (MotionReady > 0) ? TEXT("up") : TEXT("FAILED - skipped"));
    }
    if (MotionReady < 0 || !EnsureMotionTarget())
        return;
    GpuTime(Cmd, 6);

    const INT World = WorldDepthIndex();
    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    B[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B[1] = B[0];
    B[0].Transition.pResource   = (World == 1) ? DepthBuffer2.Get() : DepthBuffer.Get();
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[1].Transition.pResource   = MotionTarget.Get();
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Cmd->ResourceBarrier(2, B);

    const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = MotionRtvHeap->GetCPUDescriptorHandleForHeapStart();
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)MotionW, (FLOAT)MotionH, 0.0f, 1.0f };
    D3D12_RECT     Sc = { 0, 0, MotionW, MotionH };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    // Counted, because a picture of the vectors cannot tell a drawn zero from a cleared one.
    static INT Drawn = 0, Cleared = 0;
    if (!bPrevCamValid || !bCamSet)
    {
        Cleared++;
        const FLOAT Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Cmd->ClearRenderTargetView(Rtv, Zero, 0, nullptr);
    }
    else
    {
        // GFinalMotionShader's cbuffer: four values a row - three of an axis or a place, then a tangent or a size.
        FLOAT C[32];
        appMemzero(C, sizeof(C));
        for (INT k = 0; k < 3; k++)
        {
            C[k]      = CamAxes[k];
            C[4 + k]  = CamAxes[3 + k];
            C[8 + k]  = CamAxes[6 + k];
            C[12 + k] = CamEye[k];
            C[16 + k] = PrevAxes[k];
            C[20 + k] = PrevAxes[3 + k];
            C[24 + k] = PrevAxes[6 + k];
            C[28 + k] = PrevEye[k];
        }
        // The self-test (RequestMotionProbe): the previous camera is this one turned a degree about the world's up axis
        // (Z in this engine), from the same place. Everything must then move by about (angle / tangent) * half the width.
        if (MotionProbeMode)
        {
            const FLOAT A = 1.0f * PI / 180.0f;
            const FLOAT Cs = appCos(A), Sn = appSin(A);
            for (INT a = 0; a < 3; a++)
            {
                const FLOAT X = CamAxes[a * 3], Y = CamAxes[a * 3 + 1];
                C[16 + a * 4]     = X * Cs - Y * Sn;
                C[16 + a * 4 + 1] = X * Sn + Y * Cs;
                C[16 + a * 4 + 2] = CamAxes[a * 3 + 2];
            }
            for (INT k = 0; k < 3; k++)
                C[28 + k] = CamEye[k];
            C[27] = (FLOAT)MotionProbeMode;      // 2: the shader writes a fixed vector - see GFinalMotionShader
        }
        C[3]  = ViewTanX;
        C[7]  = ViewTanY;
        C[11] = (FLOAT)MotionW;
        C[15] = (FLOAT)MotionH;
        C[19] = PrevTanX;
        C[23] = PrevTanY;
        const UINT Inc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_GPU_DESCRIPTOR_HANDLE Srv = DepthSrvHeap->GetGPUDescriptorHandleForHeapStart();
        Srv.ptr += (UINT64)World * Inc;
        Final.DrawMotion(Cmd, DepthSrvHeap.Get(), Srv, C);
        // The see-through water over it: from its own depth instead of what lies under it (WaterSurface).
        if (bWaterSurfaceLive && WaterSurface)
        {
            WaterSurfaceTo(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            C[31] = 1.0f;       // GFinalMotionShader's Over: where there is no water, the vector stays
            D3D12_GPU_DESCRIPTOR_HANDLE Water = DepthSrvHeap->GetGPUDescriptorHandleForHeapStart();
            Water.ptr += (UINT64)WaterSurfaceDepthSrv * Inc;
            Final.DrawMotion(Cmd, DepthSrvHeap.Get(), Water, C);
        }
        Drawn++;
    }
    if (((Drawn + Cleared) % 150) == 0)
    {
        // A turn moves the eye not at all, only the axes: the angle between the two forward axes is what says whether
        // there was movement at all. At the picture's centre a turn of this angle shifts a pixel by about
        // (angle / tangent) * half the width - the same number the pass should be writing there.
        const FLOAT Dot   = CamAxes[6] * PrevAxes[6] + CamAxes[7] * PrevAxes[7] + CamAxes[8] * PrevAxes[8];
        const FLOAT Angle = acosf(Clamp(Dot, -1.0f, 1.0f));
        const FLOAT Shift = (ViewTanX > 0.0f) ? Angle / ViewTanX * 0.5f * (FLOAT)MotionW : 0.0f;
        const FLOAT Move  = appSqrt((CamEye[0] - PrevEye[0]) * (CamEye[0] - PrevEye[0]) +
                                    (CamEye[1] - PrevEye[1]) * (CamEye[1] - PrevEye[1]) +
                                    (CamEye[2] - PrevEye[2]) * (CamEye[2] - PrevEye[2]));
        RdxLog(TEXT("motion vectors (%i frames): %i drawn, %i cleared; the camera moved %.2f units and turned %.3f degrees ")
               TEXT("(that is %.2f pixels at the centre); objects given a vector of their own: %i, %i of them with a ")
               TEXT("place for every vertex; turned away for want of room: %i"),
               Drawn + Cleared, Drawn, Cleared, Move, Angle * 180.0f / PI, Shift, MotionObjDrawn, MotionVertsDrawn,
               MotionVbRefused);
        MotionObjDrawn   = 0;
        MotionVertsDrawn = 0;
        MotionVbRefused  = 0;
    }

    // The bodies that move by themselves, over what the camera's pass wrote (docs/fsr3-atadas.md 5/3.). Their shapes are
    // in world units: the shadow maps' own casters where a body is one, the motion buffer's where it is not - so this
    // does not wait for the shadows to be on. The depth decides which of their pixels are really seen. Nothing here
    // writes depth - the pipeline's write mask is zero - but the buffer has to be in its writing state to be a
    // depth-stencil view at all, so it returns to that state before this pass.
    UBOOL bDepthBack = 0;
    INT   ObjDrawn   = 0;
    MotionBodiesLast = 0;
    if ((MotionBodyCount > 0 || Solid.PlantMotionCount() > 0) && bPrevCamValid && bCamSet && MotionReady > 0)
    {
        // For the depth as it is now: with the shadows it carries the stencil kinds, without them it does not
        // (CreateDepthBuffer), and a pipeline made for the one cannot draw against the other.
        const DXGI_FORMAT DepthFormat = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
        if (MotionObjReady == 0 || (MotionObjReady > 0 && MotionObjFormat != DepthFormat))
        {
            MotionObjReady  = Final.InitMotionObjects(Device.Get(), DepthFormat, bMotionNoDepth) ? 1 : -1;
            MotionObjFormat = DepthFormat;
            RdxLog(TEXT("motion vectors (objects): the pipeline %s (%s)"),
                   (MotionObjReady > 0) ? TEXT("up") : TEXT("FAILED - the camera vector alone is used"),
                   bShadows ? TEXT("the depth with the stencil kinds") : TEXT("the plain depth"));
        }
        if (MotionObjReady > 0)
        {
            B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            Cmd->ResourceBarrier(1, &B[0]);
            bDepthBack = 1;

            const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = (World == 1) ? DsvHeap2->GetCPUDescriptorHandleForHeapStart()
                                                                 : DsvHeap->GetCPUDescriptorHandleForHeapStart();
            Cmd->OMSetRenderTargets(1, &Rtv, FALSE, &Dsv);

            FLOAT C[FFinalPass::MOTION_OBJ_CONSTANTS];
            appMemzero(C, sizeof(C));
            for (INT k = 0; k < 3; k++)
            {
                C[k]      = CamAxes[k];
                C[4 + k]  = CamAxes[3 + k];
                C[8 + k]  = CamAxes[6 + k];
                C[12 + k] = CamEye[k];
                C[16 + k] = PrevAxes[k];
                C[20 + k] = PrevAxes[3 + k];
                C[24 + k] = PrevAxes[6 + k];
                C[28 + k] = PrevEye[k];
            }
            C[3]  = ViewTanX;
            C[7]  = ViewTanY;
            C[11] = (FLOAT)MotionW;
            C[15] = (FLOAT)MotionH;
            C[19] = PrevTanX;
            C[23] = PrevTanY;
            C[27] = JitterClip[0];
            C[31] = JitterClip[1];
            C[44] = MotionNearer;
            const UINT   Stride   = 3 * sizeof(FLOAT);
            const UBOOL  bOwn     = (MotionVbState > 0 && MotionVb[FrameIndex]) ? 1 : 0;
            const UINT64 OwnStart = bOwn ? MotionVb[FrameIndex]->GetGPUVirtualAddress() : 0;
            const UBOOL  bCasters = (bShadows && Shadow.IsReady()) ? 1 : 0;
            for (INT i = 0; i < MotionBodyCount; i++)
            {
                const FMotionBody& Body = MotionBodies[i];
                // Its triangles, as a view that begins at its first vertex - so that a second stream can begin at its
                // own first as well.
                D3D12_VERTEX_BUFFER_VIEW Vb;
                INT First = 0, Count = 0;
                if (Body.Caster >= 0)
                {
                    if (!bCasters || !Shadow.GetCasterGeometry(Body.Caster, Vb, First, Count))
                        continue;
                }
                else
                {
                    First = Body.First;
                    Count = Body.Count;
                    if (!bOwn || First < 0 || Count < 3 || First + Count > MotionVbUsed)
                        continue;
                    Vb.BufferLocation = OwnStart;
                    Vb.SizeInBytes    = (UINT)MotionVbUsed * Stride;
                    Vb.StrideInBytes  = Stride;
                }
                if (Body.PrevFirst >= 0 && bOwn && Body.PrevFirst + Count <= MotionVbUsed)
                {
                    D3D12_VERTEX_BUFFER_VIEW Now = Vb, Before;
                    Now.BufferLocation   += (UINT64)First * Now.StrideInBytes;
                    Now.SizeInBytes       = (UINT)Count * Now.StrideInBytes;
                    Before.BufferLocation = OwnStart + (UINT64)Body.PrevFirst * Stride;
                    Before.SizeInBytes    = (UINT)Count * Stride;
                    Before.StrideInBytes  = Stride;
                    Final.DrawMotionObjectVerts(Cmd, Now, Before, Count, C);
                    MotionVertsDrawn++;
                }
                else
                {
                    appMemcpy(&C[32], Body.Prev, 12 * sizeof(FLOAT));
                    Final.DrawMotionObject(Cmd, Vb, First, Count, C);
                }
                ObjDrawn++;
            }
            MotionObjDrawn  += ObjDrawn;
            MotionBodiesLast = ObjDrawn;
            // The plants: what the wind moves of the grass, the bark and the leaves (PlantMotion.h) - a kind at a time,
            // each timed (points 31-34).
            GpuTime(Cmd, 31);
            for (INT k = 0; k < 3; k++)
            {
                Solid.DrawPlantMotion(Device.Get(), Cmd, Textures, DepthFormat, C, k);
                GpuTime(Cmd, 32 + k);
            }
        }
    }

    if (!bDepthBack)
    {
        B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(2, B);
    }
    else
    {
        B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &B[1]);
    }
    bMotionDone = 1;
    GpuTime(Cmd, 7);
}

// FSR 3.1 writes its result through an unordered access view. The output target cannot be that: it is typeless
// storage carrying two render target views, and an unordered access view may not be typeless. So the upscaler
// gets a plain target of its own, and the last step of the upscale copies it onto the output target.
// The target itself. EnsureFsr3Target below is what everything calls; this one is asked once a size.
UBOOL FBackend::MakeFsr3Target()
{
    Fsr3Output.Release();
    if (!Fsr3SrvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        HeapDesc.NumDescriptors = 1;
        HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), Fsr3SrvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(FSR3 SRV)")))
            return 0;
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = AaWidth;
    Desc.Height           = AaHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = Colours.Compute;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               __uuidof(ID3D12Resource), Fsr3Output.VoidSlot()),
               TEXT("CreateCommittedResource(FSR3 output)")))
        return 0;
    Device->CreateShaderResourceView(Fsr3Output.Get(), nullptr, Fsr3SrvHeap->GetCPUDescriptorHandleForHeapStart());
    Fsr3W = AaWidth;
    Fsr3H = AaHeight;
    RdxLog(TEXT("FSR3: output target %ix%i"), Fsr3W, Fsr3H);
    return 1;
}

// Asked once a size, for the reason the motion target has the same pair: a refused create stays refused, and asking
// again on every frame only costs the asking. Measured with it forced to fail: 2038 refusals in one short run.
UBOOL FBackend::EnsureFsr3Target()
{
    if (Fsr3Output && Fsr3W == AaWidth && Fsr3H == AaHeight)
        return 1;
    if (bFsr3Failed && Fsr3FailedW == AaWidth && Fsr3FailedH == AaHeight)
        return 0;
    if (MakeFsr3Target())
    {
        bFsr3Failed = 0;
        return 1;
    }
    bFsr3Failed = 1;
    Fsr3FailedW = AaWidth;
    Fsr3FailedH = AaHeight;
    RdxLog(TEXT("FSR3: no output target at %ix%i - the upscaler cannot run, and this is not asked again until the "
                "size changes"), AaWidth, AaHeight);
    return 0;
}

// SMAA's two passes: the edges first, into their own picture, then the blend that reads the colour AND the edges.
// The colour view in SmaaSrvHeap[0] was made at startup and points at the same texture ColourSrv does; the heap
// exists because the blending pass needs the two side by side in one table.
void FBackend::DrawSmaa(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* ColourHeap,
                        D3D12_GPU_DESCRIPTOR_HANDLE ColourSrv, D3D12_CPU_DESCRIPTOR_HANDLE OutRtv)
{
    if (!SmaaEdges || !SmaaRtvHeap || !SmaaSrvHeap)
        return;
    const FLOAT InvW = 1.0f / (FLOAT)Width, InvH = 1.0f / (FLOAT)Height;
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = SmaaEdges.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Cmd->ResourceBarrier(1, &B);
    const D3D12_CPU_DESCRIPTOR_HANDLE EdgeRtv = SmaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const FLOAT Black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    Cmd->OMSetRenderTargets(1, &EdgeRtv, FALSE, nullptr);
    Cmd->ClearRenderTargetView(EdgeRtv, Black, 0, nullptr);
    Final.DrawSmaaEdges(Cmd, ColourHeap, ColourSrv, InvW, InvH, PostAaSpan, 0.05f);
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &B);
    Cmd->OMSetRenderTargets(1, &OutRtv, FALSE, nullptr);
    Final.DrawSmaaBlend(Cmd, SmaaSrvHeap.Get(), SmaaSrvHeap->GetGPUDescriptorHandleForHeapStart(), InvW, InvH,
                        PostAaSpan, 0.05f);
}

void FBackend::ResolveScene(ID3D12GraphicsCommandList* Cmd)
{
    // THE DEPTH COPY, IF THE SOLID PASS NEVER ASKED FOR ONE. It makes the copy at its first water or
    // soft-particle run, and with neither in the frame nothing asks - so the smoke had no depth to stop its
    // walk at and fell back to flat sprites. The attempt at the end of that pass's runs only ever ran for the
    // HUD's batch, where the world is already over and the copy cannot be made; measured, in the log, after
    // assuming otherwise for too long.
    //
    // HERE it certainly runs, and BEFORE the shadows: the pass has just finished, so the scene target and the
    // depth are still in the states the copy expects - the same ones the pass's own mid-draw copy relies on.
    // Once a frame either way: bDepthCopyDone says whether one was made, and making it twice is a device
    // removed rather than a wrong picture.
    if (!bDepthCopyDone && !bOutputLive && WantsCloudDepth())
        CopyForWater(Cmd, (ActiveDepth == 1 && DsvHeap2)
                          ? DsvHeap2->GetCPUDescriptorHandleForHeapStart()
                          : DsvHeap->GetCPUDescriptorHandleForHeapStart());
    // The motion vectors, whatever the shadows do: the upscaler below takes the frame only with this frame's vectors,
    // and this is the last place where the world's depth is still whole. Once a frame (bMotionDone).
    DrawMotionVectors(Cmd);
    ApplyShadows(Cmd);
    // The weapon lamps as the eye sees them - each one's disc and glare where nothing nearer stands in front of it -
    // over the finished world, so that no shadow takes any of it.
    DrawLampGlares(Cmd, WorldDepthIndex());
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = (ActiveDepth == 1 && DsvHeap2)
                                                ? DsvHeap2->GetCPUDescriptorHandleForHeapStart()
                                                : DsvHeap->GetCPUDescriptorHandleForHeapStart();
        DrawFogRooms(Cmd);
        DrawVolumetrics(Cmd, Dsv);
        DrawHoloShell(Cmd);
        DrawFieldVolume(Cmd);
        DrawDepthOfField(Cmd);
        DrawMotionBlur(Cmd);
    }

    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    B[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B[0].Transition.pResource   = SceneColor.Get();
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B[1] = B[0];
    B[1].Transition.pResource   = OutputColor.Get();
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Cmd->ResourceBarrier(2, B);

    // Slot 1: the box pass's pipeline writes the back buffer's format - and so do the upscaling passes.
    D3D12_CPU_DESCRIPTOR_HANDLE BoxRtv = OutputRtvHeap->GetCPUDescriptorHandleForHeapStart();
    BoxRtv.ptr += RtvStride;
    SetOutputViewport(Cmd);
    const D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv = SceneSrvHeap->GetGPUDescriptorHandleForHeapStart();

    // The host's upscaler first (Upscaler=Auto, HostUpscale.h): up, with its textures for these sizes handed over, it
    // takes the frame. Otherwise the built-in one below does the work - while the host starts, and after it went down.
    UBOOL bHost = 0;
    if (HostWanted() && bMotionDone && MotionTarget && SceneColor && Host.IsUp())
    {
        bHost = Host.Prepare(Device.Get(), SceneWidth, SceneHeight, AaWidth, AaHeight, SceneColor->GetDesc().Format,
                             Colours.Compute, bFgMode && bFrameGenWanted, Colours.BackBuffer, Width, Height);
        // The built-in context is not needed beside it, and it holds a good part of a gigabyte at 4K: it goes, once the
        // frames that used it are done - which is what the SDK asks before a context is destroyed.
        if (bHost && Fsr3Ready > 0)
        {
            WaitForGpu();
            Fsr3.Shutdown();
            Fsr3Ready = 0;
            Fsr3Output.Release();
            Fsr3W = Fsr3H = 0;
            RDX_LOG("FSR3: the built-in context let go - the host's upscaler runs");
            // The wait asks the host whether it still answers (WaitFenceEvent): one that went down in it leaves this
            // frame to the built-in upscaler, which makes its context again below.
            bHost = Host.IsUp();
        }
    }
    bHostUpscaled = bHost;
    // Frame generation for this frame: only with the host's upscale - its first half rides along with it (the depth
    // and the motion are the host's copies of this frame), its second half comes at the frame's end (EndFrame).
    bFgFrame = (bHost && bFgMode && bFrameGenWanted && Host.FrameGenReady()) ? 1 : 0;
    // Whichever library the host upscales with generates there too - where the card has that library's frame
    // generation: a host that is up and offers none is the card's answer (5).
    FgWhy    = bFgFrame ? 0
             : !bFrameGenWanted ? 1
             : !bFgMode ? 2
             : !HostWanted() ? 3
             : (!Host.IsUp() || !bHost || Host.IsPreparing()) ? 4 : 5;

    // FSR 3.1 needs this frame's motion vectors and a context built for these two sizes. The context is made on
    // first use, because the sizes are only settled by then; if anything is missing the frame falls back to
    // the bilinear pass rather than showing nothing.
    UBOOL bFsr3 = 0;
    if (!bHost && UpscaleMode && bFsr3Wanted && Fsr3Ready >= 0 && bMotionDone && MotionTarget)
    {
        if (!Fsr3.Matches(SceneWidth, SceneHeight, AaWidth, AaHeight))
        {
            // The SDK's own parameter checking goes with the validation run, not with a measurement.
            // bFsr3ForceFail: the marker SecondRevision-fsr3-fail.txt, so that the fallback - the bilinear pass - can be
            // TRIED instead of trusted: it is what stands between a broken SDK and a black picture on screen.
            Fsr3Ready = (!bFsr3ForceFail &&
                         Fsr3.Init(Device.Get(), SceneWidth, SceneHeight, AaWidth, AaHeight, bValidation)) ? 1 : -1;
            if (Fsr3Ready < 0)
                RdxLog(TEXT("FSR3: the context did not come up - falling back to the bilinear pass"));
            else
            {
                // The context's own resources are made HERE, not in Init - and they are the ones that can push a big
                // scene over the card's budget. Over it NOTHING FAILS: every call returns success, Windows starts
                // moving resources across the PCIe bus, and the frame quietly goes from tens of milliseconds to
                // hundreds. Measured on an RX 9070 XT, 4K with SuperSample 4 (a 15360x8640 scene):
                //   NativeAA    11709 MB used against an 11037 MB budget -> 635 ms a frame (1.6 FPS)
                //   Performance  5369 MB against 15417 MB                ->  20.9 ms a frame (48 FPS)
                // The upscaler itself is not slow - the same pass costs about 21 ms in both. The difference is the
                // paging. So the check is on the MEASURED numbers, not on an estimate of what a mode ought to need,
                // and when it does not fit we take the fallback that works instead of a picture nobody can play with.
                DOUBLE MemUsed = 0.0, MemBudget = 0.0;
                VideoMemoryMB(MemUsed, MemBudget);
                const UBOOL bOver = (MemBudget > 0.0) && (MemUsed > MemBudget * 0.95);
                RdxLog(TEXT("video memory after the FSR context: %.0f MB used, %.0f MB budget%s"), MemUsed, MemBudget,
                       bOver ? TEXT(" - OVER BUDGET") : TEXT(""));
                if (bOver)
                {
                    Fsr3.Shutdown();
                    Fsr3Ready = -1;
                    RdxLog(TEXT("FSR3: the context does not fit the card's memory at this scene size (%ix%i -> %ix%i) "
                               "- falling back to the bilinear pass. A smaller scene scale or a more aggressive mode helps."),
                           SceneWidth, SceneHeight, AaWidth, AaHeight);
                }
            }
            bFsr3Reset = 1;      // a brand new context has no history behind it
        }
        bFsr3 = (Fsr3Ready > 0) && EnsureFsr3Target();
    }

    // A post filter (FXAA) reads a finished picture and writes another. Two cases:
    //   - nothing else resolves (no supersampling, no upscaling): the filter IS the resolve, scene -> output, one pass;
    //   - something else resolves: that writes the middle picture, and the filter turns it into the output.
    const UBOOL bPostAa   = (PostAaKind >= 1) && (FxaaReady > 0);
    const UBOOL bPostOnly = bPostAa && (SceneScale <= 1) && !UpscaleMode;
    const UBOOL bPostAfter = bPostAa && !bPostOnly && UpscaleTemp && UpscaleRtvHeap && UpscaleSrvHeap;
    D3D12_RESOURCE_BARRIER TempBar;
    appMemzero(&TempBar, sizeof(TempBar));
    TempBar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    TempBar.Transition.pResource   = UpscaleTemp.Get();
    TempBar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (bPostAfter)
    {
        TempBar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        TempBar.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Cmd->ResourceBarrier(1, &TempBar);
        BoxRtv = UpscaleRtvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    if (bPostOnly)
    {
        GpuTime(Cmd, 8);          // the same two points the upscaler uses: one key, so only one of them ever runs
        if (PostAaKind == 2)
            DrawSmaa(Cmd, SceneSrvHeap.Get(), SceneSrv, BoxRtv);
        else
        {
            Cmd->OMSetRenderTargets(1, &BoxRtv, FALSE, nullptr);
            Final.DrawFxaa(Cmd, SceneSrvHeap.Get(), SceneSrv, 1.0f / (FLOAT)Width, 1.0f / (FLOAT)Height, PostAaSpan);
        }
        GpuTime(Cmd, 9);
    }
    else if (!UpscaleMode)
    {
        Cmd->OMSetRenderTargets(1, &BoxRtv, FALSE, nullptr);
        // Without the curve: it comes once, in the final pass, after every blend - the HUD's too.
        Final.DrawBox(Cmd, SceneSrvHeap.Get(), SceneSrv, OutputExponent, SceneScale, 0, bLinearMix);
    }
    else if (bHost)
    {
        // The inputs into the host's textures, the frame's first half executed, the frame handed over, and the second
        // half opened on the same list (HostUpscale.h). Nothing set on the list survives that: the viewport and the
        // target are set again below, and every pass after this one sets its own state. The world's depth is in
        // DEPTH_WRITE here, as for the built-in upscaler.
        const INT World = WorldDepthIndex();
        ID3D12Resource* const DepthRes    = (World == 1) ? DepthBuffer2.Get() : DepthBuffer.Get();
        // The see-through water's own depth into the depth the upscaler reads (StampWaterSurface).
        StampWaterSurface(Cmd, (World == 1 && DsvHeap2) ? DsvHeap2->GetCPUDescriptorHandleForHeapStart()
                                                        : DsvHeap->GetCPUDescriptorHandleForHeapStart());
        // The reactive mask where its last writer left it - the walk's UAV, the splashes' target - and read from here.
        ReactiveTo(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* const ReactiveRes = (bReactiveLive && Reactive) ? Reactive.Get() : nullptr;
        ID3D12Resource* const Sources[4]  = { SceneColor.Get(), DepthRes, MotionTarget.Get(), ReactiveRes };
        const D3D12_RESOURCE_STATES Was[4] = { D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // 8 to 9 is everything the upscale costs this way, as it is for the built-in one; 26 splits it - the copies
        // before it, the crossing both ways, the host's work and the draw back after it.
        GpuTime(Cmd, 8);
        Host.CopyInputs(Cmd, Sources, Was);
        GpuTime(Cmd, 26);

        // The frame's numbers, as the built-in upscaler is given them (FFsr3Pass::Dispatch).
        Host64::FFrame Frame;
        appMemzero(&Frame, sizeof(Frame));
        Frame.Services     = Host64::Service_Upscale;
        Frame.Flags        = (bFsr3Reset ? (uint32_t)Host64::FrameFlag_Reset : 0u) |
                             (ReactiveRes ? (uint32_t)Host64::FrameFlag_Reactive : 0u);
        Frame.RenderWidth  = (uint32_t)SceneWidth;
        Frame.RenderHeight = (uint32_t)SceneHeight;
        Frame.OutputWidth  = (uint32_t)AaWidth;
        Frame.OutputHeight = (uint32_t)AaHeight;
        Frame.JitterX      = JitterPx[0];
        Frame.JitterY      = JitterPx[1];
        Frame.FrameTimeMs  = FrameDeltaMs;
        Frame.FovY         = 2.0f * atanf(ViewTanY);
        Frame.NearZ        = 1.0f;          // z/w = 1/W: the near plane is one unit out
        Frame.Sharpness    = Fsr3Sharpness;
        if (bFgFrame)
        {
            // Frame generation's first half: the frame's depth and motion (the host's copies above) and its camera -
            // the engine's axes are right, down and forward; the generator's are right, up and forward.
            Frame.Services |= Host64::Service_FramePrepare;
            for (INT k = 0; k < 3; k++)
            {
                Frame.CameraPosition[k] = CamEye[k];
                Frame.CameraRight[k]    = CamAxes[k];
                Frame.CameraUp[k]       = -CamAxes[3 + k];
                Frame.CameraForward[k]  = CamAxes[6 + k];
            }
        }
        bFsr3Reset = 0;
        NoteFrameSubmit(1);
        const UBOOL bHandedOver = Host.Handover(Cmd, Allocators[FrameIndex], Queue.Get(), Frame);
        // The frame's reset as it went (Handover adds one where the textures are new): its generated picture's too.
        bFgReset = (Frame.Flags & Host64::FrameFlag_Reset) ? 1 : 0;
        if (bHandedOver)
        {
            SetOutputViewport(Cmd);
            D3D12_RESOURCE_BARRIER Out;
            appMemzero(&Out, sizeof(Out));
            Out.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            Out.Transition.pResource   = Host.GetOutput();
            Out.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            Out.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            Out.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            Cmd->ResourceBarrier(1, &Out);
            Cmd->OMSetRenderTargets(1, &BoxRtv, FALSE, nullptr);
            // As after the built-in upscaler: the box filter down to the output with supersampling, a plain copy without.
            ID3D12DescriptorHeap* const Heap = Host.GetOutputHeap();
            if (SceneScale > 1)
                Final.DrawBox(Cmd, Heap, Heap->GetGPUDescriptorHandleForHeapStart(), OutputExponent, SceneScale, 0,
                              bLinearMix);
            else
                Final.DrawLinear(Cmd, Heap, Heap->GetGPUDescriptorHandleForHeapStart(), 1.0f / (FLOAT)Width,
                                 1.0f / (FLOAT)Height);
            GpuTime(Cmd, 9);
            Out.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            Out.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
            Cmd->ResourceBarrier(1, &Out);
        }
    }
    else if (bFsr3)
    {
        // Everything the upscaler reads has to be in the state it is told it is in, and everything goes back
        // afterwards, so the rest of this frame and the next one find what they expect. The world's depth is in
        // DEPTH_WRITE here whether the shadows ran or not: both ApplyShadows and DrawMotionVectors leave it so.
        const INT World = WorldDepthIndex();
        ID3D12Resource* const DepthRes = (World == 1) ? DepthBuffer2.Get() : DepthBuffer.Get();
        // The see-through water's own depth into the depth the upscaler reads (StampWaterSurface).
        StampWaterSurface(Cmd, (World == 1 && DsvHeap2) ? DsvHeap2->GetCPUDescriptorHandleForHeapStart()
                                                        : DsvHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_RESOURCE_BARRIER In[3];
        appMemzero(In, sizeof(In));
        for (INT i = 0; i < 3; i++)
        {
            In[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            In[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            In[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        In[0].Transition.pResource   = SceneColor.Get();
        In[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        In[1].Transition.pResource   = DepthRes;
        In[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        In[2].Transition.pResource   = MotionTarget.Get();
        In[2].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(3, In);

        // The reactive mask, in a frame whose splashes or fires wrote it (DrawSplashes, DrawVolumetrics), from wherever its
        // last writer left it.
        ReactiveTo(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* const ReactiveRes = (bReactiveLive && Reactive) ? Reactive.Get() : nullptr;
        D3D12_RESOURCE_BARRIER Rb;
        if (ReactiveRes)
        {
            SplashBarrier(Rb, ReactiveRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Cmd->ResourceBarrier(1, &Rb);
        }
        GpuTime(Cmd, 8);
        if (!Fsr3.Dispatch(Cmd, SceneColor.Get(), DepthRes, MotionTarget.Get(), Fsr3Output.Get(),
                           JitterPx[0], JitterPx[1], FrameDeltaMs, 2.0f * atanf(ViewTanY), bFsr3Reset, Fsr3Sharpness,
                           ReactiveRes))
            Fsr3Ready = -1;      // one spoiled frame, and the bilinear pass takes over from the next
        bFsr3Reset = 0;
        if (ReactiveRes)
        {
            SplashBarrier(Rb, ReactiveRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Cmd->ResourceBarrier(1, &Rb);
        }

        for (INT i = 0; i < 3; i++)
        {
            const D3D12_RESOURCE_STATES Was = In[i].Transition.StateBefore;
            In[i].Transition.StateBefore = In[i].Transition.StateAfter;
            In[i].Transition.StateAfter  = Was;
        }
        Cmd->ResourceBarrier(3, In);

        // The upscaled picture is already at the output's size, so this is a one-to-one copy onto the output
        // target - the one thing the upscaler cannot write into itself.
        D3D12_RESOURCE_BARRIER Out;
        appMemzero(&Out, sizeof(Out));
        Out.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Out.Transition.pResource   = Fsr3Output.Get();
        Out.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Out.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Out.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &Out);
        Cmd->OMSetRenderTargets(1, &BoxRtv, FALSE, nullptr);
        // The upscaler wrote the ANTIALIASED image (AaWidth x AaHeight). With supersampling that is bigger than the
        // output, and the same box filter that serves the no-upscaling path averages it down - one pass either way,
        // because this step was always here. Without supersampling the two sizes are equal and it is a plain copy.
        if (SceneScale > 1)
            Final.DrawBox(Cmd, Fsr3SrvHeap.Get(), Fsr3SrvHeap->GetGPUDescriptorHandleForHeapStart(),
                          OutputExponent, SceneScale, 0, bLinearMix);
        else
            Final.DrawLinear(Cmd, Fsr3SrvHeap.Get(), Fsr3SrvHeap->GetGPUDescriptorHandleForHeapStart(),
                             1.0f / (FLOAT)Width, 1.0f / (FLOAT)Height);
        GpuTime(Cmd, 9);
        Out.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Out.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Cmd->ResourceBarrier(1, &Out);
    }
    else
    {
        Cmd->OMSetRenderTargets(1, &BoxRtv, FALSE, nullptr);
        Final.DrawLinear(Cmd, SceneSrvHeap.Get(), SceneSrv, 1.0f / (FLOAT)Width, 1.0f / (FLOAT)Height);
    }
    if (bPostAfter)
    {
        // The middle picture is finished: read it, and write the output the HUD will be drawn onto.
        TempBar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        TempBar.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &TempBar);
        D3D12_CPU_DESCRIPTOR_HANDLE OutRtv = OutputRtvHeap->GetCPUDescriptorHandleForHeapStart();
        OutRtv.ptr += RtvStride;
        GpuTime(Cmd, 8);
        if (PostAaKind == 2)
            DrawSmaa(Cmd, UpscaleSrvHeap.Get(), UpscaleSrvHeap->GetGPUDescriptorHandleForHeapStart(), OutRtv);
        else
        {
            Cmd->OMSetRenderTargets(1, &OutRtv, FALSE, nullptr);
            Final.DrawFxaa(Cmd, UpscaleSrvHeap.Get(), UpscaleSrvHeap->GetGPUDescriptorHandleForHeapStart(),
                           1.0f / (FLOAT)Width, 1.0f / (FLOAT)Height, PostAaSpan);
        }
        GpuTime(Cmd, 9);
    }
    bOutputLive = 1;
}

// The world's end in a frame that has no output target to go over to: what works on the finished world goes onto it
// in the scene target, and the frame goes on there with the depth buffer it was drawing into. At the HUD's first tile
// (SwitchToOutput), or at the frame's end where no HUD tile came (EndFrame). The multisampled set is down to one sample
// by now (ResolveMsaaWorld).
D3D12_CPU_DESCRIPTOR_HANDLE FBackend::EndWorldInPlace(ID3D12GraphicsCommandList* Cmd)
{
    const D3D12_CPU_DESCRIPTOR_HANDLE Active = (ActiveDepth == 1 && DsvHeap2)
                                               ? DsvHeap2->GetCPUDescriptorHandleForHeapStart()
                                               : DsvHeap->GetCPUDescriptorHandleForHeapStart();
    // The same as in ResolveScene, and for the same reason: this is the path with no output target.
    if (!bDepthCopyDone && !bOutputLive && WantsCloudDepth())
        CopyForWater(Cmd, Active);
    // Nothing upscales on this path: the vectors are drawn here for their own view alone (the marker).
    DrawMotionVectors(Cmd);
    ApplyShadows(Cmd);
    DrawLampGlares(Cmd, WorldDepthIndex());
    // Before bOutputLive: from there on the world is over and the depth is the HUD's to clear, and the walk is
    // stopped by that depth.
    DrawFogRooms(Cmd);
    DrawVolumetrics(Cmd, Active);
    DrawHoloShell(Cmd);
    DrawFieldVolume(Cmd);
    DrawDepthOfField(Cmd);
    DrawMotionBlur(Cmd);
    DrawFilmLook(Cmd);
    bOutputLive = 1;          // the world is finished: a ClearZ after this clears, it keeps no depth
    // The frame goes on in this target, and the passes above cut the picture to the world's view - in a conversation
    // narrower than the screen: the HUD gets the whole of it back, as the frame began.
    const D3D12_VIEWPORT Whole = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const D3D12_RECT     All   = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &Whole);
    Cmd->RSSetScissorRects(1, &All);
    BindSceneTargets(Cmd, Active);
    return Active;
}

D3D12_CPU_DESCRIPTOR_HANDLE FBackend::SwitchToOutput(ID3D12GraphicsCommandList* Cmd)
{
    // The world is done here - so this is where the multisampled set comes down to one sample, BEFORE the shadows,
    // which read the depth and the stencil and expect one sample of each.
    ResolveMsaaWorld(Cmd);

    // 1x - BeginHud queues this for the shadows, or for a pass of the world's end (WorldEndWanted), and there is no
    // other target to go to.
    if (!HasOutputTarget())
        return EndWorldInPlace(Cmd);

    ResolveScene(Cmd);
    DrawFilmLook(Cmd);
    // The world is finished and the HUD not begun: frame generation's HUD-less picture is this one, through the final pass.
    if (bFgFrame)
        CaptureHudLess(Cmd);

    // Slot 0: the geometry pass's pipelines draw in the scene's format. With the shadows on those pipelines write a second
    // and a third target too: after the scene output-sized light and added targets take them (CreateLightTarget - not null
    // views, ERROR #613).
    D3D12_CPU_DESCRIPTOR_HANDLE Rtvs[3];
    Rtvs[0] = OutputRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv = OutputDsvHeap->GetCPUDescriptorHandleForHeapStart();
    if (bShadows && LightRtvHeap && OutputLightTarget && OutputAddedTarget)
    {
        Rtvs[1] = LightRtvHeap->GetCPUDescriptorHandleForHeapStart();
        Rtvs[1].ptr += RtvStride;
        Rtvs[2] = Rtvs[1];
        Rtvs[2].ptr += 2 * RtvStride;
        Cmd->OMSetRenderTargets(3, Rtvs, FALSE, &Dsv);
    }
    else
        Cmd->OMSetRenderTargets(1, Rtvs, FALSE, &Dsv);
    // The HUD's own tiles pass whatever the buffer holds (they sit at the nearest depth, RdxMath::DepthPasses).
    // Cleared for 3D that may still come after them - counted in the log (RenderDevice.cpp, FHudSwitchStats).
    Cmd->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    return Dsv;
}

UBOOL FBackend::BeginHud()
{
    if (!bInFrame || bHudQueued)
        return 0;
    if (!HasOutputTarget() && !bShadows && !WorldEndWanted())
    {
        // No switch at 1x without the shadows, where nothing works on the finished world either - but where the world
        // ends still has to be known: the copy of its depth is made no later, and the world's blended runs wait no
        // further (FSolidPass::Flush) - the HUD's tiles after it keep the order they came in. The world's start is
        // marked where the depth is readable (MarkWorldStart), so its end is too.
        if (NeedsDepthRead() && !bWorldEndQueued)
            bWorldEndQueued = Solid.AddWorldEnd();
        return 0;
    }
    bHudQueued = Solid.AddTargetSwitch();
    return bHudQueued;
}

UBOOL FBackend::ReadScene(FColor* Out, INT OutW, INT OutH)
{
    if (!Device || !SceneColor || !Out || OutW <= 0 || OutH <= 0)
        return 0;
    if (bInFrame)
    {
        // Mid-frame the target holds half a picture. Not seen yet; say so if it happens.
        static INT Reported = 0;
        if (++Reported <= 2)
            RdxLog(TEXT("ReadPixels inside a frame - skipped"));
        return 0;
    }

    // Supersampling: the output target - the finished frame, HUD included, at the output's size; not the
    // scene target, which at 2x in 4K would put 133 MB into this 32-bit process for every snapshot. Off: the
    // scene target, as always.
    ID3D12Resource* const Source = HasOutputTarget() ? OutputColor.Get() : SceneColor.Get();
    if (!Source)
        return 0;

    // The source's layout in readback memory.
    const D3D12_RESOURCE_DESC Desc = Source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout;
    UINT   Rows = 0;
    UINT64 RowBytes = 0, Total = 0;
    Device->GetCopyableFootprints(&Desc, 0, 1, 0, &Layout, &Rows, &RowBytes, &Total);
    // Typeless storage: the copy needs a format, and it has to be the one the target really holds.
    Layout.Footprint.Format = (Source == OutputColor.Get()) ? Colours.BackBuffer : Colours.Draw;

    if (!Readback || ReadbackSize < Total)
    {
        Readback.Release();
        ReadbackSize = 0;

        D3D12_HEAP_PROPERTIES Heap;
        appMemzero(&Heap, sizeof(Heap));
        Heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC Buf;
        appMemzero(&Buf, sizeof(Buf));
        Buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Buf.Width            = Total;
        Buf.Height           = 1;
        Buf.DepthOrArraySize = 1;
        Buf.MipLevels        = 1;
        Buf.SampleDesc.Count = 1;
        Buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Buf,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   __uuidof(ID3D12Resource), Readback.VoidSlot())))
            return 0;
        ReadbackSize = Total;
    }

    // The copy goes on the out-of-frame list, which waits for the GPU when submitted.
    if (!OpenUploadList())
        return 0;

    // Between frames the source is in PIXEL_SHADER_RESOURCE, whichever of the two it is. Both hold the
    // picture before the brightness curve, which the loop below applies.
    const D3D12_RESOURCE_STATES Resting = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    D3D12_RESOURCE_BARRIER Barrier;
    appMemzero(&Barrier, sizeof(Barrier));
    Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource   = Source;
    Barrier.Transition.StateBefore = Resting;
    Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UploadList->ResourceBarrier(1, &Barrier);

    D3D12_TEXTURE_COPY_LOCATION Dst;
    appMemzero(&Dst, sizeof(Dst));
    Dst.pResource       = Readback.Get();
    Dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    Dst.PlacedFootprint = Layout;

    D3D12_TEXTURE_COPY_LOCATION Src;
    appMemzero(&Src, sizeof(Src));
    Src.pResource        = Source;
    Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Src.SubresourceIndex = 0;
    UploadList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);

    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Barrier.Transition.StateAfter  = Resting;
    UploadList->ResourceBarrier(1, &Barrier);

    SubmitUploadList();

    BYTE* Mapped = nullptr;
    D3D12_RANGE ReadAll;
    ReadAll.Begin = 0;
    ReadAll.End   = (SIZE_T)Total;
    if (FAILED(Readback->Map(0, &ReadAll, (void**)&Mapped)))
        return 0;

    // The screen shows the scene through the brightness curve; the snapshot should show
    // the same thing, so the curve is applied here too - as a 256-entry table where the source is eight bits.
    // The table is the whole reason the eight-bit path is kept separate below: it is one lookup a channel
    // against a powf a channel, on up to eight million pixels.
    BYTE Curve[256];
    for (INT i = 0; i < 256; i++)
    {
        const FLOAT V = 255.0f * powf((FLOAT)i / 255.0f, OutputExponent) + 0.5f;
        Curve[i] = (BYTE)((V < 0.0f) ? 0 : (V > 255.0f) ? 255 : (INT)V);
    }

    // A screenshot is eight bits a channel whatever the frame was drawn in, so the wider modes are brought down
    // here. In HDR that also means everything above one has to come somewhere: the same shoulder the picture
    // goes through, so the file shows what the screen showed rather than a flat white patch where the lamps are.
    const DXGI_FORMAT ReadFmt = Layout.Footprint.Format;
    // The shoulder only where there is something above white to bring down. Ten bits is the same RANGE as eight,
    // finer - a value there cannot exceed one, and bending the top quarter of it would make a ten-bit screenshot
    // differ from an eight-bit one of the same frame, which is exactly what that mode promises not to do.
    const UBOOL bAboveWhite = (ReadFmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 1 : 0;
    const INT W = (OutW < Width)  ? OutW : Width;
    const INT H = (OutH < Height) ? OutH : Height;
    for (INT y = 0; y < OutH; y++)
    {
        FColor* Row = Out + y * OutW;
        const BYTE* S = Mapped + (SIZE_T)y * Layout.Footprint.RowPitch;
        for (INT x = 0; x < OutW; x++)
        {
            if (y < H && x < W)
            {
                // Blue into R and red into B. The engine's own consumer says so: in the
                // natural order its screenshot came out with red and blue swapped -
                // measured, the railing at (53.4, 26.3, 1.7) on screen was (1.7, 26.3, 53.4)
                // in the file.
                if (ReadFmt == DXGI_FORMAT_R8G8B8A8_UNORM || ReadFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                {
                    Row[x].R = Curve[S[x * 4 + 2]];
                    Row[x].G = Curve[S[x * 4 + 1]];
                    Row[x].B = Curve[S[x * 4 + 0]];
                }
                else
                {
                    FLOAT C[3];
                    if (ReadFmt == DXGI_FORMAT_R10G10B10A2_UNORM)
                    {
                        UINT Word;
                        appMemcpy(&Word, S + x * 4, sizeof(Word));
                        RdxMath::UnpackTenBit(Word, C[0], C[1], C[2]);
                    }
                    else
                    {
                        unsigned short Halfs[4];
                        appMemcpy(Halfs, S + x * 8, sizeof(Halfs));
                        for (INT c = 0; c < 3; c++)
                            C[c] = RdxMath::HalfToFloat(Halfs[c]);
                    }
                    for (INT c = 0; c < 3; c++)
                    {
                        FLOAT V = (C[c] > 0.0f) ? powf(C[c], OutputExponent) : 0.0f;
                        if (bAboveWhite)
                            V = RdxMath::HighlightShoulder(V, RdxMath::HdrShoulderKnee, 1.0f);
                        V = 255.0f * V + 0.5f;
                        C[c] = (V > 255.0f) ? 255.0f : V;
                    }
                    Row[x].R = (BYTE)C[2];
                    Row[x].G = (BYTE)C[1];
                    Row[x].B = (BYTE)C[0];
                }
            }
            else
            {
                Row[x].R = Row[x].G = Row[x].B = 0;
            }
            Row[x].A = 255;
        }
    }

    D3D12_RANGE NoWrite;
    NoWrite.Begin = 0;
    NoWrite.End   = 0;
    Readback->Unmap(0, &NoWrite);
    return 1;
}

UBOOL FBackend::Init(HWND Window, INT InWidth, INT InHeight)
{
    Width  = InWidth  > 0 ? InWidth: 1;
    Height = InHeight > 0 ? InHeight : 1;
    // The two settings compose - see RdxMath::SceneSideFor. Aa* is what the upscaler produces and what the box filter
    // then averages down to the output; with supersampling off it IS the output's size.
    AaWidth     = RdxMath::AntialiasedSideFor(Width,  SceneScale);
    AaHeight    = RdxMath::AntialiasedSideFor(Height, SceneScale);
    SceneWidth  = RdxMath::SceneSideFor(Width,  SceneScale, (RdxMath::EUpscaling)UpscaleMode);
    SceneHeight = RdxMath::SceneSideFor(Height, SceneScale, (RdxMath::EUpscaling)UpscaleMode);
    OutputExponent = 1.0f;
    ReadbackSize   = 0;

    RDX_LOGF("initialising, %ix%i", Width, Height);

    RdxStartupMark(TEXT("backend: begins"));
    if (!CreateDeviceAndQueue())  return 0;
    RdxStartupMark(TEXT("backend: the device and its queue"));
    // Anti-Lag 2 where the driver has it, asked once with the device (AntiLag2.h) - in this 32-bit game through the
    // driver's 32-bit extension, which AMD's own header does not ask.
    AntiLagHr = RdxAntiLag2::Initialize(&AntiLag, Device.Get());
    RDX_LOGF("anti-lag 2: %s (0x%08X; the interface 0x%08X, its state 0x%08X, in the 64-bit layout 0x%08X%s)",
             (AntiLagHr == S_OK) ? TEXT("the driver has it")
                                 : TEXT("not here - another card, an older driver, or none for a 32-bit game"),
             (UINT)AntiLagHr, (UINT)AntiLag.m_createHr, (UINT)AntiLag.m_stateHr, (UINT)AntiLag.m_wideHr,
             AntiLag.m_wide ? TEXT(", which it took") : TEXT(""));
    // NVIDIA Reflex where the driver has it (ReflexLink.h): the same key on NVIDIA's cards.
    if (!AmdAntiLagUp())
        RdxReflexStart(Device.Get());
    SetAntiLag(bAntiLagWanted);
    // Frame generation asked for: the chain is made on its queue from the start (SetFrameGen moves it later).
    bFgMode = (bFrameGenWanted && Pacer.Init(Device.Get())) ? 1 : 0;
    if (!CreateSwapChain(Window)) return 0;
    RdxStartupMark(TEXT("backend: the swap chain"));
    if (!CreateFrameResources())  return 0;
    PresentBlack();
    // The game's window to the front before the shaders are compiled: until then the engine's own start dialog is in
    // front of it - a small window, so the taskbar shows over the game - and once that dialog closes Windows hands the
    // front to whatever program was there before, not to the game. The front is still this process's now, so Windows
    // lets it be passed on. The dialog is then laid back over the game's window without taking the front, so it is
    // seen as before until it closes.
    const HWND Front = ::GetForegroundWindow();
    if (Window && Front && Front != Window)
    {
        DWORD FrontProcess = 0;
        ::GetWindowThreadProcessId(Front, &FrontProcess);
        if (FrontProcess == ::GetCurrentProcessId())
        {
            const BOOL bMoved = ::SetForegroundWindow(Window);
            if (bMoved)
                ::SetWindowPos(Front, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RDX_LOGF("the window: brought to the front over the start's own window (%s)",
                     bMoved ? TEXT("done, that window laid back over it") : TEXT("refused"));
        }
    }
    RdxStartupMark(TEXT("backend: the frame resources, a black picture"));
    // From here to the start's end, whichever way it ends: the caption before every shader that is compiled, and the
    // compile itself on a thread of its own while this one answers the window (CompilingWait).
    struct FCompilingWatch
    {
        FCompilingWatch(FBackend* Who) { RdxCompileWatch(&FBackend::CompilingWatch, Who, &FBackend::CompilingWait); }
        ~FCompilingWatch() { RdxCompileWatch(nullptr, nullptr); }
    };
    CompilingThread = ::GetCurrentThreadId();
    const FCompilingWatch Watching(this);
    // What the card will actually give for multisampling. A count it refuses is not an error - we step down and say
    // so, because a setting that silently does nothing is worse than one that says it could not.
    MsaaCount = 1;
    // ALWAYS, not only when MSAA was asked for at startup: the renderer's menu can turn it on at any time, and a
    // capability we never looked up reads as "the card cannot" - which is how a machine that CAN write stencil from
    // a shader was told it could not, and refused MSAA for the rest of the run (measured, starting from
    // AntiAliasing=Off).
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS O;
        appMemzero(&O, sizeof(O));
        if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &O, sizeof(O))))
            bStencilRef = O.PSSpecifiedStencilRefSupported ? 1 : 0;
        RDX_LOGF("antialiasing (MSAA): shader-written stencil (PSSpecifiedStencilRef): %s - without it MSAA and shadows "
                 "cannot run together", bStencilRef ? TEXT("YES") : TEXT("NO"));
    }
    if (MsaaWanted > 1)
    {
        // The stencil decides whether MSAA can run at all. With shadows on, drawing marks the surface kinds in
        // the stencil and the shadow shader reads them back; hardware cannot resolve a multisampled stencil
        // (ResolveSubresource handles colour only), so the value has to be written from the pixel shader, which
        // needs PSSpecifiedStencilRef. PickMsaaCount is the single place that weighs this up.
        MsaaCount = PickMsaaCount(MsaaWanted);
    }
    CreateGpuTimer();
    RdxStartupMark(TEXT("backend: the GPU timer, the MSAA count"));
    // Shadow maps before the depth buffer, which takes their views. Should they fail, the frame is drawn as with the
    // shadows off.
    ShadowGeneration++;
    if (bShadows && !Shadow.Init(Device.Get(), ShadowPreset))
    {
        RDX_LOG("shadow maps: could not be created - shadows are skipped");
        bShadows     = 0;
        ShadowPreset = RdxMath::ShadowPresetFor(RdxMath::ShadowQuality_Off);
    }
    RdxStartupMark(TEXT("backend: the shadow maps"));
    // FSR 3.1 cannot work without the jitter and the motion vectors, so asking for it asks for both - whatever
    // their own markers said. It has to be settled here: the vectors are what make the depth buffer readable,
    // and that decides the format it is created with (NeedsDepthRead).
    if (bFsr3Wanted)
    {
        bMotionWanted = 1;
        bJitterWanted = 1;
    }
    // The scene-sized targets, with a way down. At SuperSample 4 in 4K the scene is 15360x8640, and the targets that
    // follow it (colour, light, the added pass, two depth buffers, the motion vectors) are a few gigabytes together:
    // on a card that has not got the room, the honest answer is a smaller scale and a log line saying so, not a
    // renderer that refuses to start. Each step halves the scale; 1 is the vanilla path and cannot fail for size.
    for (;;)
    {
        if (CreateDepthBuffer() && CreateSceneTarget() && CreateMsaaTargets() && (!HasOutputTarget() || CreateOutputTarget()))
            break;
        if (SceneScale <= 1)
            return 0;
        const INT Smaller = SceneScale / 2;
        RDX_LOGF("SSAA: the targets for the %ix scene (%ix%i) could not be created - stepping down to %ix",
                 SceneScale, SceneWidth, SceneHeight, Smaller);
        SceneScale  = Smaller;
        AaWidth     = RdxMath::AntialiasedSideFor(Width,  SceneScale);
        AaHeight    = RdxMath::AntialiasedSideFor(Height, SceneScale);
        SceneWidth  = RdxMath::SceneSideFor(Width,  SceneScale, (RdxMath::EUpscaling)UpscaleMode);
        SceneHeight = RdxMath::SceneSideFor(Height, SceneScale, (RdxMath::EUpscaling)UpscaleMode);
    }
    // Before Init: the solid pass builds its second, multisampled set of pipelines from this.
    RdxStartupMark(TEXT("backend: the scene's targets"));
    Solid.SetMsaaSamples(MsaaCount);
    // The rays are the 64-bit host's (RtHostPasses - known only once it is up): the pipelines that write their rays
    // down and those that read its answers are made now, with the rest, on a card that takes their shader model.
    {
        D3D12_FEATURE_DATA_SHADER_MODEL Sm;
        Sm.HighestShaderModel = D3D_SHADER_MODEL_6_5;
        Solid.SetRtHosted(SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &Sm, sizeof(Sm))) &&
                          Sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5);
        Solid.SetRayListViews(RayAnswersView - RtMaskView, RayHeadsUav - WaterSurfaceUavView,
                              RayAskedLowUav - WaterSurfaceUavView, RayCornersView - RtMaskView);
        Solid.SetOwnLampViews(OwnLampTableView - RtMaskView, OwnLampTableView - BounceMapView);
    }
    Solid.SetWaterShadeView(WaterShadeUav - WaterSurfaceUavView);
    Solid.SetWetMarkView(WetMarkUav - WaterSurfaceUavView, WetMarkSrv - RtMaskView, WetCoverSrv - WaterSurfaceUavView);
    Solid.SetCardWriter(this);
    if (!Solid.Init(Device.Get(), bShadows)) return 0;
    RdxStartupMark(TEXT("backend: the solid pass (its pipelines)"));
    if (!Final.Init(Device.Get())) return 0;
    RdxStartupMark(TEXT("backend: the final pass"));
    // The depth resolve, without which the multisampled world could not hand its depth (and the surface kinds) on.
    // Should it not build, MSAA is off for this run - and the log says why, instead of a black or flat picture.
    if (MsaaCount > 1 && !Final.InitMsaaDepth(Device.Get(), MsaaCount, bShadows))
    {
        RDX_LOG("MSAA: the depth resolve pass could not be created - MSAA is off for this run");
        MsaaCount = 1;
        // Everything that was built for it goes with it: the targets, and the solid pass's second set of pipelines.
        // Leaving the pass on its multisampled set while the backend binds single-sample targets is a draw that
        // fails on every triangle.
        ReleaseMsaaTargets();
        Solid.SetMsaaSamples(1);
    }
    // The box pass exists only when supersampling is on, the upscaling passes only with upscaling (docs/szabaly-vanilla.md 2.).
    if (SceneScale > 1 && !Final.InitBox(Device.Get())) return 0;
    if (PostAaKind == 1)
    {
        FxaaReady = Final.InitFxaa(Device.Get()) ? 1 : -1;
        if (FxaaReady < 0)
            RDX_LOG("antialiasing: the FXAA pass could NOT be created - the picture runs unsmoothed");
    }
    else if (PostAaKind == 2)
    {
        FxaaReady = (Final.InitSmaa(Device.Get()) && CreateSmaaTargets()) ? 1 : -1;
        if (FxaaReady < 0)
            RDX_LOG("antialiasing: the SMAA passes could NOT be created - the picture runs unsmoothed");
    }
    if (UpscaleMode && !Final.InitUpscale(Device.Get())) return 0;
    RdxStartupMark(TEXT("backend: the post passes, the upscaling passes"));
    if (!Textures.Init(Device.Get())) return 0;
    RdxStartupMark(TEXT("backend: the texture cache"));
    Pom.Init(Device.Get());     // the relief's height maps; a set that is not on disk simply has none
    RdxStartupMark(TEXT("backend: the relief's height maps"));
    WarmShaders();
    RdxStartupMark(TEXT("backend: every other shader (WarmShaders)"));
    // The caption's total against what the start compiled: where the cache's file gave nothing the two are the same,
    // unless a shader came or went since START_SHADERS was set.
    if (RdxShaderCacheRead() == 0 && RdxStartupShaders() != START_SHADERS)
        RDX_LOGF("shaders: the start compiled %i with nothing from the cache's file, its caption counted on %i "
                 "(START_SHADERS) - that number is to follow it", RdxStartupShaders(), (INT)START_SHADERS);
    RdxShaderWarmDone();
    Textures.SetCheckParallel(bValidation);
    Textures.SetGpuConvert(bGpuConvert);

    {
        DOUBLE MemUsed = 0.0, MemBudget = 0.0;
        VideoMemoryMB(MemUsed, MemBudget);
        RDX_LOGF("video memory: %.0f MB used, %.0f MB budget%s", MemUsed, MemBudget,
                 (MemBudget > 0.0 && MemUsed > MemBudget) ? TEXT(" - OVER BUDGET, the system pages over PCIe") : TEXT(""));
    }
    RDX_LOG("initialised");
    if (UpscaleMode)
        RDX_LOGF("upscaling: the scene is %ix%i, the output %ix%i (%.2f times), %s", SceneWidth, SceneHeight, Width, Height,
                 (DOUBLE)RdxMath::UpscalingRatio((RdxMath::EUpscaling)UpscaleMode),
                 bUpscaleLinear ? TEXT("bilinear (the marker)")
                                : TEXT("the temporal upscaler - the bilinear pass where it cannot run"));
    if (SceneScale > 1)
        RDX_LOGF("SSAA: the scene is %ix%i (%ix%i samples per pixel), the output %ix%i, averaged: %s",
                 SceneWidth, SceneHeight, SceneScale, SceneScale, Width, Height,
                 bLinearMix ? TEXT("in linear light") : TEXT("over the stored values"));
    return 1;
}

// ---------------------------------------------------------------------------
// Teardown and resize
// ---------------------------------------------------------------------------

void FBackend::ReleaseBackBuffers()
{
    for (UINT i = 0; i < BACKEND_FRAME_COUNT; i++)
    {
        if (BackBuffers[i])
        {
            BackBuffers[i]->Release();
            BackBuffers[i] = nullptr;
        }
    }
}

void FBackend::WaitForGpu()
{
    if (!Queue || !Fence || !FenceEvent)
        return;

    const UINT64 Target = FenceValues[FrameIndex];
    if (FAILED(Queue->Signal(Fence.Get(), Target)))
        return;

    if (Fence->GetCompletedValue() < Target)
    {
        // A timeout, never INFINITE. If the fence somehow never signals, an infinite wait stops the game's
        // message loop too: the window goes "not responding" and can only be killed. With a timeout the log says
        // what happened and the game stays alive.
        if (SUCCEEDED(Fence->SetEventOnCompletion(Target, FenceEvent)) &&
            WaitFenceEvent(5000) == WAIT_TIMEOUT)
        {
            RdxLog(TEXT("fence timeout: waited for %I64u, reached %I64u"),
                   Target, Fence->GetCompletedValue());
        }
    }
    FenceValues[FrameIndex]++;
    // Frame generation's presenting queue reads the pictures and writes the back buffers: whoever waits for the GPU to
    // let go of them waits for it too.
    if (bFgMode)
        Pacer.Flush();
}

DWORD FBackend::WaitFenceEvent(DWORD TotalMs)
{
    if (!Host.IsOn())
        return WaitForSingleObject(FenceEvent, TotalMs);
    for (DWORD Waited = 0;; Waited += 50)
    {
        const DWORD Got = WaitForSingleObject(FenceEvent, 50);
        if (Got != WAIT_TIMEOUT || Waited + 50 >= TotalMs)
            return Got;
        Host.Poll();
    }
}

// The host follows what the frames want: started when a temporal upscale is on with Upscaler=Auto or DLSS - asked for
// the backend that says - and stopped when not. A host that went down is not started again by itself - the built-in
// upscaler runs, and the readout says why - but a change of the setting or of the antialiasing tries once more
// (SetUpscaler, SetAntiAliasing, SetUpscaling).
void FBackend::SyncHost()
{
    // Wanted for its upscalers, with the backend that upscales - or for the rays alone (HostRaysWanted), with the
    // one that needs no library: what the card traces is the card's own, whatever the backend.
    const UBOOL bUpscale = HostWanted();
    const UBOOL bWant    = bUpscale || HostRaysWanted();
    const UINT  Backend  = !bUpscale ? (UINT)Host64::Backend_Test
                         : (UpscalerChoice == 2)                             ? (UINT)Host64::Backend_Dlss
                         : (UpscalerChoice == (INT)RdxMath::Upscaler_Xess) ? (UINT)Host64::Backend_Xess
                                                                             : (UINT)Host64::Backend_Fsr;
    // A host that was started for the rays alone has no upscaler: with one wanted now, it is another host.
    if (bUpscale && Host.IsOn() && !Host.IsDown() && HostBackend != Backend)
    {
        WaitForGpu();
        StopHost();
    }
    if (bWant && !Host.IsOn())
    {
        Host.Start(Device.Get(), bValidation, Backend);
        HostBackend = Backend;
    }
    else if (!bWant && Host.IsOn())
    {
        // Frames still on the GPU may be waiting for it, or reading its textures: they finish first.
        WaitForGpu();
        StopHost();
    }
    Host.Poll();
    // Up for the rays alone, on a card that traces none: it has nothing to do. It goes, and is not started for the
    // rays again - the settings on rays run their fallbacks, as without it.
    if (!bUpscale && !bInFrame && Host.IsUp() && Host.RayPasses() == 0 && HostRaysWanted())
    {
        RDX_LOG("ray tracing: the 64-bit host is up and its card traces no rays - the host is let go, and the "
                "settings on rays run their fallbacks");
        bRtHostUseless = 1;
        WaitForGpu();
        StopHost();
    }
    // The rays (docs/terv-rt-host.md): once the host that is up says it traces, the scene's lists and the shadows'
    // tables are kept from here on, for the host - and the level is asked for again (GetShadowGeneration), because
    // the rays' own triangles come with it. Once: a refusal is not tried again every frame.
    if (!Rt.IsHosted() && !bRtHostRefused && !bInFrame && Host.RayPasses() != 0)
    {
        if (!Device5)
            Device->QueryInterface(__uuidof(ID3D12Device5), Device5.VoidSlot());
        if (Device5 && Rt.InitHosted(Device5.Get()))
        {
            ShadowGeneration++;
            RDX_LOGF("ray tracing: through the 64-bit host - its passes 0x%X (1 the shadows' factor, 2 the occlusion, "
                     "4 the rays' list)", Host.RayPasses());
        }
        else
            bRtHostRefused = 1;
    }
}

UBOOL FBackend::SetUpscaler(INT Which)
{
    if (!Device || bInFrame)
        return 0;
    const INT Want = (Which >= 0 && Which < (INT)RdxMath::Upscaler_Count) ? Which : 0;
    // The same choice again still retries a host that went down: that is what choosing it again means. Another backend
    // is another host.
    if (Host.IsOn() && (Want != UpscalerChoice || Host.IsDown()))
    {
        WaitForGpu();
        StopHost();
    }
    UpscalerChoice = Want;
    bFsr3Reset     = 1;      // either way the next frame's upscaler has no history of this picture
    RDX_LOGF("upscaler: %s",
             (Want == 1) ? TEXT("Builtin - FSR 3.1 in the renderer")
           : (Want == 2) ? TEXT("DLSS - NVIDIA's DLSS through the host, the built-in FSR 3.1 until it is up or if it cannot")
           : (Want == (INT)RdxMath::Upscaler_Xess)
                         ? TEXT("XeSS - Intel's XeSS through the host, the built-in FSR 3.1 until it is up or if it cannot")
                         : TEXT("Auto - the host's FSR DLLs, the built-in FSR 3.1 until it is up or if it cannot"));
    return 1;
}

UBOOL FBackend::SetFrameGen(UBOOL bOn)
{
    if (!Device || !SwapChain || bInFrame)
        return 0;
    bFrameGenWanted = bOn ? 1 : 0;
    if (bFrameGenWanted == bFgMode)
        return 1;
    // Everything queued finished first - on the pacer's queue too while it presents (WaitForGpu).
    WaitForGpu();
    if (bFrameGenWanted && !Pacer.Init(Device.Get()))
    {
        RDX_LOG("frame generation: no presenting queue - it stays off");
        return 0;
    }
    const UBOOL bWas = bFgMode;
    bFgMode = bFrameGenWanted;
    if (!RecreateSwapChain())
    {
        RDX_LOG("frame generation: the swap chain could not be made again on the other queue - going back");
        bFgMode = bWas;
        // With no chain at all nothing can be presented: said as a lost device, so that the renderer is taken down
        // and brought up again rather than left drawing into nothing.
        if (!RecreateSwapChain())
            NoteDeviceLost(E_FAIL, TEXT("no swap chain could be made again"));
        return 0;
    }
    if (!bFgMode)
    {
        Pacer.Shutdown();
        FgFinal.Release();
    }
    bFsr3Reset = 1;
    RDX_LOGF("frame generation: %s", bFgMode ? TEXT("on - two pictures a frame, presented from a queue of its own")
                                             : TEXT("off - one picture a frame, presented as always"));
    return 1;
}

// The swap chain made again, on the queue frame generation's mode says (CreateSwapChain's choice), with everything it
// takes: the back buffers and all that is sized with them (Resize). A chain in exclusive fullscreen leaves it first -
// DXGI's rule for letting a chain go - and goes back into it after.
UBOOL FBackend::RecreateSwapChain()
{
    if (!OutputWindow || !Factory)
        return 0;
    WaitForGpu();
    ReleaseBackBuffers();
    // No chain to let go on the way back from one that could not be made (SetFrameGen): the first try has let the
    // old one go already - and taken it out of exclusive fullscreen, which is then not gone back into.
    UBOOL bFull = 0;
    if (SwapChain)
    {
        bFull = (QueryFullscreen() == 1) ? 1 : 0;
        if (bFull)
            SwapChain->SetFullscreenState(FALSE, nullptr);
        SwapChain.Release();
    }

    DXGI_SWAP_CHAIN_DESC1 Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Width            = Width;
    Desc.Height           = Height;
    Desc.Format           = Colours.BackBuffer;
    Desc.SampleDesc.Count = 1;
    Desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    Desc.BufferCount      = SwapBuffers;
    Desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Desc.Flags            = bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ID3D12CommandQueue* const ChainQueue = (bFgMode && Pacer.IsUp()) ? Pacer.GetQueue() : Queue.Get();
    TComRef<IDXGISwapChain1> Chain1;
    if (!Check(Factory->CreateSwapChainForHwnd(ChainQueue, OutputWindow, &Desc, nullptr, nullptr, Chain1.Slot()),
               TEXT("CreateSwapChainForHwnd (again)")))
        return 0;
    Factory->MakeWindowAssociation(OutputWindow, DXGI_MWA_NO_ALT_ENTER);
    if (!Check(Chain1->QueryInterface(__uuidof(IDXGISwapChain3), SwapChain.VoidSlot()),
               TEXT("QueryInterface(IDXGISwapChain3) (again)")))
        return 0;
    ApplyColourSpace();
    if (bFull)
        SwapChain->SetFullscreenState(TRUE, nullptr);
    RDX_LOGF("swap chain: made again, on %s", (ChainQueue == Queue.Get()) ? TEXT("the rendering queue")
                                                                          : TEXT("frame generation's presenting queue"));
    return Resize(Width, Height);
}

// Frame generation's mode, a frame that does not generate: its finished picture in a texture of ours, at the back
// buffer's size and format. Let go with the sizes and the formats (Resize, RebuildForColours), where the GPU is idle.
UBOOL FBackend::EnsureFgFinal()
{
    if (FgFinal)
        return 1;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = (UINT64)Width;
    Desc.Height           = (UINT)Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = Colours.BackBuffer;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!FgFinalRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 1;
        if (FAILED(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), FgFinalRtvHeap.VoidSlot())))
            return 0;
    }
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               __uuidof(ID3D12Resource), FgFinal.VoidSlot())))
    {
        static INT Told = 0;
        if (Told++ < 3)
            RdxLog(TEXT("frame generation: the finished picture's texture could not be made (%ix%i) - the frame is not "
                        "presented"), Width, Height);
        return 0;
    }
    Device->CreateRenderTargetView(FgFinal.Get(), nullptr, FgFinalRtvHeap->GetCPUDescriptorHandleForHeapStart());
    return 1;
}

// The HUD-less picture: the finished world, the HUD not begun, through the final pass - so that it is the same numbers as
// the finished picture everywhere the HUD is not. The output target is a render target here, and is left one.
void FBackend::CaptureHudLess(ID3D12GraphicsCommandList* Cmd)
{
    ID3D12Resource* const HudLess = Host.GetHudLess();
    if (!HudLess || !OutputColor || !OutputSrvHeap)
        return;
    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 2; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    B[0].Transition.pResource   = OutputColor.Get();
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B[1].Transition.pResource   = HudLess;
    B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Cmd->ResourceBarrier(2, B);
    const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = Host.HudLessRtv();
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    SetOutputViewport(Cmd);
    DrawFinal(Cmd, OutputSrvHeap.Get(), OutputSrvHeap->GetGPUDescriptorHandleForHeapStart());
    for (INT i = 0; i < 2; i++)
    {
        const D3D12_RESOURCE_STATES Was = B[i].Transition.StateBefore;
        B[i].Transition.StateBefore = B[i].Transition.StateAfter;
        B[i].Transition.StateAfter  = Was;
    }
    Cmd->ResourceBarrier(2, B);
    bHudLessDone = 1;
}

void FBackend::MoveToNextFrame()
{
    const UINT64 Current = FenceValues[FrameIndex];
    if (FAILED(Queue->Signal(Fence.Get(), Current)))
        return;
    // The host's path's pacing waits for the frame before this one (PaceThroughHost).
    PaceFencePrev = PaceFenceLast;
    PaceFenceLast = Current;
    PaceSlotPrev  = PaceSlotLast;
    PaceSlotLast  = (INT)FrameIndex;

    // Frame generation's mode: the back buffers are the pacer's thread's, and a frame slot is only a frame slot - the next
    // one in turn. Otherwise the slot goes with the back buffer drawn into.
    FrameIndex = bFgMode ? (FrameIndex + 1) % SwapBuffers : SwapChain->GetCurrentBackBufferIndex();

    // Only stall if the GPU has not yet finished the frame that last used this slot.
    // Bounded, not INFINITE - see WaitForGpu for why. Timed for the long-frame diagnostic.
    FrameWaitSec = 0.0;
    if (Fence->GetCompletedValue() < FenceValues[FrameIndex])
    {
        const DOUBLE W0 = appSeconds();
        if (SUCCEEDED(Fence->SetEventOnCompletion(FenceValues[FrameIndex], FenceEvent)) &&
            WaitFenceEvent(5000) == WAIT_TIMEOUT)
        {
            RdxLog(TEXT("fence timeout on frame change: waited for %I64u, reached %I64u"),
                   FenceValues[FrameIndex], Fence->GetCompletedValue());
        }
        FrameWaitSec = appSeconds() - W0;
        WaitSlotSum += FrameWaitSec;
        WaitSlotMax  = Max(WaitSlotMax, FrameWaitSec);
    }

    FenceValues[FrameIndex] = Current + 1;
}

// Diagnostic: the time between our Present calls and the time spent in each, on the
// performance counter - the clock PresentMon uses too. Measured: PresentMon reported 15-21
// ms frames on a fixed beat, each followed by one of 0.1-0.2 ms, which this engine cannot
// produce (its own work between two frames alone takes 4 ms), while the engine's frame timer
// saw a steady 5 ms. This settles whose account is right.
void FBackend::NotePresent(LONGLONG Q0, LONGLONG Q1)
{
    if (QpcMs == 0.0)
    {
        LARGE_INTEGER F;
        ::QueryPerformanceFrequency(&F);
        QpcMs = 1000.0 / (DOUBLE)F.QuadPart;
    }
    if (FrameFile)
    {
        // The local wall clock (to match the test script's phase marks) and the time since the
        // previous Present, by the performance counter.
        if (FrameCapPrevQpc)
        {
            FILETIME Ft, Lt;
            SYSTEMTIME St;
            ::GetSystemTimePreciseAsFileTime(&Ft);
            ::FileTimeToLocalFileTime(&Ft, &Lt);
            ::FileTimeToSystemTime(&Lt, &St);
            const ULONGLONG Ticks = ((ULONGLONG)Lt.dwHighDateTime << 32) | Lt.dwLowDateTime;
            const INT N = _snprintf_s(FrameCapBuf + FrameCapLen, FRAMECAP_BYTES - FrameCapLen, _TRUNCATE,
                                      "%04d-%02d-%02d %02d:%02d:%02d.%06d,%.3f,%s\r\n",
                                      St.wYear, St.wMonth, St.wDay, St.wHour, St.wMinute, St.wSecond,
                                      (INT)((Ticks % 10000000ULL) / 10ULL),
                                      (DOUBLE)(Q0 - FrameCapPrevQpc) * QpcMs, FrameCapLabel);
            if (N > 0)
                FrameCapLen += N;
            // Room left for the longest row: the time, the frame time and a full label - a row that does not fit is
            // cut off by _snprintf_s and lost.
            if (FrameCapLen > FRAMECAP_BYTES - (FRAMECAP_LABEL + 64))
                FlushFrameCapture();
        }
        FrameCapPrevQpc = Q0;
    }
    const DOUBLE Call = (DOUBLE)(Q1 - Q0) * QpcMs;
    if (PrevPresentQpc)
    {
        const DOUBLE Gap = (DOUBLE)(Q0 - PrevPresentQpc) * QpcMs;
        PresentN++;
        if (Gap > 12.0) PresentLong++;
        if (Gap < 1.0)  PresentShort++;
        if (Gap  > PresentMaxGap)  PresentMaxGap  = Gap;
        if (Call > PresentMaxCall) PresentMaxCall = Call;
        if ((Gap > 12.0 || Gap < 1.0 || Call > 5.0) && PresentLogged < 200)
        {
            PresentLogged++;
            RdxLog(TEXT("present %i: %.2f ms since the last one, the call itself %.2f ms"), PresentN, Gap, Call);
        }
        else if (PresentsAfterFlush > 0)
        {
            // After a level change every Present is logged, whatever it cost: there, a short
            // block is an answer too (see the timer experiment in RenderDevice.cpp).
            RdxLog(TEXT("present %i (after a level load): %.2f ms since the last one, the call itself %.2f ms"), PresentN, Gap, Call);
        }
        if (PresentsAfterFlush > 0)
            PresentsAfterFlush--;
        if (PresentN % 1000 == 0)
        {
            RdxLog(TEXT("present clock (%i): gaps over 12 ms %i, under 1 ms %i, longest gap %.2f ms, "
                        "longest call %.2f ms"),
                   PresentN, PresentLong, PresentShort, PresentMaxGap, PresentMaxCall);
            PresentLong = PresentShort = 0;
            PresentMaxGap = PresentMaxCall = 0.0;
        }
    }
    PrevPresentQpc = Q0;
}

// The debug layer's messages, when it is on (RequestValidation): corruption, errors and
// warnings. Each kind of message is logged in full the first time and only counted after -
// the same mistake repeats every frame. bFinal adds the counts, at shutdown.
void FBackend::DrainDebugMessages(UBOOL bFinal)
{
    if (!InfoQueue)
        return;
    const UINT64 N = InfoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < N; i++)
    {
        SIZE_T Len = 0;
        UINT64 Buf[1024];                       // 8 KB, aligned for D3D12_MESSAGE
        if (FAILED(InfoQueue->GetMessage(i, nullptr, &Len)) || Len == 0 || Len > sizeof(Buf))
            continue;
        D3D12_MESSAGE* M = reinterpret_cast<D3D12_MESSAGE*>(Buf);
        if (FAILED(InfoQueue->GetMessage(i, M, &Len)) || M->Severity > D3D12_MESSAGE_SEVERITY_WARNING)
            continue;
        INT k = 0;
        while (k < DebugIdCount && DebugIds[k] != (INT)M->ID)
            k++;
        if (k == DebugIdCount && DebugIdCount < MAX_DEBUG_IDS)
        {
            DebugIds[DebugIdCount]    = (INT)M->ID;
            DebugCounts[DebugIdCount] = 0;
            DebugIdCount++;
            const TCHAR* Sev = M->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? TEXT("CORRUPTION")
                                                                                : M->Severity == D3D12_MESSAGE_SEVERITY_ERROR      ? TEXT("ERROR")
                                                                                                                                   :                                                    TEXT("WARNING");
            RdxLog(TEXT("D3D12 %s #%i: %S"), Sev, (INT)M->ID, M->pDescription ? M->pDescription : "");
        }
        if (k < DebugIdCount)
        {
            // The test scripts close the game by force, before any summary at shutdown - so the
            // counts go to the log as they grow.
            const INT C = ++DebugCounts[k];
            // The next few of a kind in full as well: one kind covers many mistakes - the same complaint of another
            // resource, another stage, another draw - and the count does not tell them apart.
            if (C > 1 && C <= 6)
                RdxLog(TEXT("D3D12 #%i again: %S"), DebugIds[k], M->pDescription ? M->pDescription : "");
            if (C == 10 || C == 100 || C == 1000 || C == 10000)
                RdxLog(TEXT("D3D12 #%i: %i times now"), DebugIds[k], C);
        }
    }
    InfoQueue->ClearStoredMessages();
    if (bFinal)
    {
        RdxLog(TEXT("D3D12 debug layer: %i kinds of message"), DebugIdCount);
        for (INT k = 0; k < DebugIdCount; k++)
            RdxLog(TEXT("  #%i: %i times"), DebugIds[k], DebugCounts[k]);
    }
}

// One file per game session, truncated on the first open only - the device is re-created on
// every return from Alt+Tab - and readable while the game runs.
static UBOOL GFrameCapOpened = 0;

void FBackend::StartFrameCapture()
{
    if (FrameFile)
        return;
    HANDLE F = ::CreateFileW(L"SecondRevision-frames.csv", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, GFrameCapOpened ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (F == INVALID_HANDLE_VALUE)
    {
        RDX_LOG("frame capture: the file cannot be opened");
        return;
    }
    ::SetFilePointer(F, 0, nullptr, FILE_END);
    if (!GFrameCapOpened)
    {
        static const char Head[] = "time,frame_time_ms,running\r\n";
        DWORD Written = 0;
        ::WriteFile(F, Head, sizeof(Head) - 1, &Written, nullptr);
    }
    GFrameCapOpened = 1;
    FrameFile       = F;
    FrameCapLen     = 0;
    FrameCapPrevQpc = 0;
    RDX_LOG("frame capture: SecondRevision-frames.csv");
}

void FBackend::SetFrameCaptureLabel(const char* Text)
{
    INT k = 0;
    // Commas and line breaks would split a row in two, so they are turned into something harmless rather than
    // trusted never to appear: the label is built from settings names, and a name is free to change.
    for (; Text && Text[k] && k < FRAMECAP_LABEL - 1; k++)
    {
        const char C = Text[k];
        FrameCapLabel[k] = (C == ',' || C == '\r' || C == '\n') ? ' ' : C;
    }
    FrameCapLabel[k] = 0;
}

// The device is gone. Said ONCE, with the reason, and the frame capture closed on the spot.
//
// Once is deliberate. Saying it from every failed Present left 13818 lines in one run, which buries the one line
// that mattered under its own repetition.
//
// Closing the capture matters more. A failed Present returns AT ONCE, so what follows are not frame times at all
// - they are the speed of failing. Left open, the file fills with rows that read as a very fast machine (199 FPS
// beside a 57 ms GPU time, measured), and nothing afterwards can tell them from real ones. Ending the file here
// means the last row in it is the last row that was true.
void FBackend::NoteDeviceLost(HRESULT Hr, const TCHAR* Where)
{
    if (bDeviceLost)
        return;
    bDeviceLost = 1;
    const HRESULT Reason = Device ? Device->GetDeviceRemovedReason() : Hr;
    RdxLog(TEXT("THE DEVICE WAS LOST: %s 0x%08X, reason 0x%08X - nothing is drawn until the renderer is up again"),
           Where, (INT)Hr, (INT)Reason);
    // How full the video memory was: over the budget the system moves resources out, and a frame can take seconds.
    {
        DOUBLE Used = 0.0, Budget = 0.0;
        VideoMemoryMB(Used, Budget);
        RdxLog(TEXT("video memory at the loss: %.0f MB used, %.0f MB budget; the scene %ix%i, the output %ix%i"),
               Used, Budget, SceneWidth, SceneHeight, Width, Height);
    }
    if (bDredWanted && Device)
        LogDred();
    RdxLog(TEXT("the frame capture ends at this line: every frame time after it measures the failure, not the game"));

    // The WINDOW TITLE, and deliberately not the readout. From here every Present fails, so nothing new ever
    // reaches the screen: the last frame that did get there stays, readout and all, still showing a frame rate.
    // A caption saying the device is gone would be drawn into frames that can no longer be presented - measured,
    // by taking the device away on purpose and photographing what was left on screen. The title is the one thing
    // still reachable, and it is where someone looks when a game stops answering: the taskbar and Alt+Tab.
    if (OutputWindow)
        ::SetWindowTextW(OutputWindow, L"Deus Ex - GRAPHICS DEVICE LOST - recovering");

    FlushFrameCapture();
    if (FrameFile)
    {
        ::CloseHandle(FrameFile);
        FrameFile = nullptr;
    }
}

// What DRED kept of a lost device (RequestDred): every command list the GPU had not finished - its name, how far it
// got, the operations around the one it stopped at, and the markers before it - then a page fault's address and the
// allocations near it. Only lists left unfinished: a finished one says nothing about where the GPU stopped.
static const TCHAR* DredOpName(D3D12_AUTO_BREADCRUMB_OP Op)
{
    switch (Op)
    {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:                return TEXT("marker");
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:               return TEXT("begin event");
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:                 return TEXT("end event");
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:            return TEXT("draw");
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:     return TEXT("draw indexed");
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:                 return TEXT("dispatch");
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:         return TEXT("copy buffer region");
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:        return TEXT("copy texture region");
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:             return TEXT("copy resource");
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:       return TEXT("resolve");
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:    return TEXT("clear target");
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return TEXT("clear UAV");
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:    return TEXT("clear depth");
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:          return TEXT("barrier");
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT:                  return TEXT("present");
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:         return TEXT("resolve query data");
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return TEXT("build acceleration structure");
    default:                                                return TEXT("other");
    }
}

static const TCHAR* DredKindName(D3D12_DRED_ALLOCATION_TYPE Type)
{
    switch (Type)
    {
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE:          return TEXT("resource");
    case D3D12_DRED_ALLOCATION_TYPE_HEAP:              return TEXT("heap");
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP:   return TEXT("descriptor heap");
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE:    return TEXT("pipeline");
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return TEXT("command allocator");
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST:      return TEXT("command list");
    default:                                           return TEXT("other");
    }
}

// DRED on (RequestDred): the picture-sized resources named, every frame - most are made again with the picture's size,
// and a new one has no name. A page fault's report tells the allocations at its address by these (LogDred).
void FBackend::NameResources()
{
    auto Name = [](ID3D12Object* Object, const WCHAR* Text) { if (Object) Object->SetName(Text); };
    Name(SceneColor.Get(),        L"SceneColor");
    Name(DepthBuffer.Get(),       L"DepthBuffer");
    Name(DepthBuffer2.Get(),      L"DepthBuffer2");
    Name(OutputColor.Get(),       L"OutputColor");
    Name(OutputDepth.Get(),       L"OutputDepth");
    Name(LightTarget.Get(),       L"LightTarget");
    Name(AddedTarget.Get(),       L"AddedTarget");
    Name(OutputLightTarget.Get(), L"OutputLightTarget");
    Name(OutputAddedTarget.Get(), L"OutputAddedTarget");
    Name(MsaaColor.Get(),         L"MsaaColor");
    Name(MsaaLight.Get(),         L"MsaaLight");
    Name(MsaaAdded.Get(),         L"MsaaAdded");
    Name(MsaaDepth.Get(),         L"MsaaDepth");
    Name(WaterColor.Get(),        L"WaterColor");
    Name(WaterDepth.Get(),        L"WaterDepth");
    Name(WaterSurface.Get(),      L"WaterSurface");
    Name(ReliefCover.Get(),       L"ReliefCover");
    Name(FluidDepth.Get(),        L"FluidDepth");
    Name(FluidThick.Get(),        L"FluidThick");
    Name(FluidSmooth.Get(),       L"FluidSmooth");
    Name(Reactive.Get(),          L"Reactive");
    Name(MotionTarget.Get(),      L"MotionTarget");
    Name(UpscaleTemp.Get(),       L"UpscaleTemp");
    Name(Fsr3Output.Get(),        L"Fsr3Output");
    Name(SmaaEdges.Get(),         L"SmaaEdges");
    Name(ShadowColor.Get(),       L"ShadowColor");
    Name(FgFinal.Get(),           L"FgFinal");
    Name(WaveField.Get(),         L"WaveField");
    Name(RippleField[0].Get(),    L"RippleField0");
    Name(RippleField[1].Get(),    L"RippleField1");
    Volume.NameResources();
    Shadow.NameResources();
}

void FBackend::LogDred()
{
    TComRef<ID3D12DeviceRemovedExtendedData1> Dred;
    if (FAILED(Device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData1), Dred.VoidSlot())))
    {
        RDX_LOG("DRED: nothing to read");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 Crumbs;
    if (SUCCEEDED(Dred->GetAutoBreadcrumbsOutput1(&Crumbs)))
    {
        INT Nodes = 0, Unfinished = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* Node = Crumbs.pHeadAutoBreadcrumbNode; Node && Nodes < 256;
             Node = Node->pNext, Nodes++)
        {
            const UINT Done = Node->pLastBreadcrumbValue ? *Node->pLastBreadcrumbValue : 0;
            if (Done >= Node->BreadcrumbCount || Unfinished >= 4)
                continue;
            Unfinished++;
            RdxLog(TEXT("DRED: command list '%s' on queue '%s' - %u of its %u operations done"),
                   Node->pCommandListDebugNameW ? Node->pCommandListDebugNameW : L"?",
                   Node->pCommandQueueDebugNameW ? Node->pCommandQueueDebugNameW : L"?", Done, Node->BreadcrumbCount);
            const UINT From = (Done > 6) ? Done - 6 : 0;
            const UINT To   = Min(Node->BreadcrumbCount, Done + 3);
            for (UINT k = From; k < To; k++)
                RdxLog(TEXT("DRED:   operation %u: %s%s"), k, DredOpName(Node->pCommandHistory[k]),
                       (k == Done) ? TEXT("   <- the first not done") : TEXT(""));
            // The markers just before and at the stop: which run it was in.
            for (UINT c = 0; c < Node->BreadcrumbContextsCount; c++)
            {
                const D3D12_DRED_BREADCRUMB_CONTEXT& Context = Node->pBreadcrumbContexts[c];
                const UINT  At    = Context.BreadcrumbIndex;
                const UBOOL bNear = (At + 12 >= Done && At <= Done + 2) ? 1 : 0;
                if (bNear && Context.pContextString)
                    RdxLog(TEXT("DRED:   marker at operation %u: %s"), Context.BreadcrumbIndex, Context.pContextString);
            }
        }
        RdxLog(TEXT("DRED: %i command lists recorded, %i of them unfinished"), Nodes, Unfinished);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 Fault;
    if (SUCCEEDED(Dred->GetPageFaultAllocationOutput1(&Fault)) && Fault.PageFaultVA)
    {
        RdxLog(TEXT("DRED: page fault at GPU address 0x%016llX"), (unsigned long long)Fault.PageFaultVA);
        INT n = 0;
        for (const D3D12_DRED_ALLOCATION_NODE1* A = Fault.pHeadExistingAllocationNode; A && n < 8; A = A->pNext, n++)
            RdxLog(TEXT("DRED:   near it, alive: %s (%s)"), A->ObjectNameW ? A->ObjectNameW : L"(no name)",
                   DredKindName(A->AllocationType));
        n = 0;
        for (const D3D12_DRED_ALLOCATION_NODE1* A = Fault.pHeadRecentFreedAllocationNode; A && n < 8; A = A->pNext, n++)
            RdxLog(TEXT("DRED:   near it, freed lately: %s (%s)"), A->ObjectNameW ? A->ObjectNameW : L"(no name)",
                   DredKindName(A->AllocationType));
    }
    else
        RDX_LOG("DRED: no page fault recorded");
}

void FBackend::FlushFrameCapture()
{
    if (!FrameFile || FrameCapLen <= 0)
        return;
    DWORD Written = 0;
    ::WriteFile(FrameFile, FrameCapBuf, (DWORD)FrameCapLen, &Written, nullptr);
    FrameCapLen = 0;
}

// DXGI's own fullscreen state - what a D3D10-era renderer asks for, and what Windows gives
// the "independent flip" path: the frame goes to the screen without the desktop compositor
// copying it. Our borderless window, covering the monitor exactly, did NOT get that path:
// measured, PresentMon "Composed: Flip" in every frame (the B8G8R8A8 format did not change
// it), while the stock D3D10 renderer got "Hardware: Independent Flip" in the same desktop
// state. Experiment - the device only asks for it with RDX_EXCLUSIVE=1 in the environment.
UBOOL FBackend::SetFullscreen(UBOOL bOn)
{
    if (!SwapChain)
        return 0;
    BOOL bNow = FALSE;
    SwapChain->GetFullscreenState(&bNow, nullptr);
    if ((bNow ? 1 : 0) == (bOn ? 1 : 0))
    {
        bExclusive = bOn ? 1 : 0;
        return 1;
    }
    // Frame generation's mode: nothing of the pacer's thread's in flight while the chain changes state.
    if (bFgMode)
        Pacer.Flush();
    const HRESULT Hr = SwapChain->SetFullscreenState(bOn ? TRUE : FALSE, nullptr);
    if (FAILED(Hr))
    {
        // For one, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE while the window is not in front. The
        // device tries again later; the log gets the first few.
        static INT Failures = 0;
        if (++Failures <= 20)
            RdxLog(TEXT("SetFullscreenState(%i) failed 0x%08X"), bOn ? 1 : 0, (INT)Hr);
        return 0;
    }
    bExclusive = bOn ? 1 : 0;
    // DXGI's rule: a change of fullscreen state is followed by ResizeBuffers.
    const UBOOL Ok = Resize(Width, Height);
    RdxLog(TEXT("DXGI fullscreen: %s, now %i"), bOn ? TEXT("on") : TEXT("off"), QueryFullscreen());
    return Ok;
}

INT FBackend::QueryFullscreen() const
{
    BOOL bNow = FALSE;
    if (!SwapChain || FAILED(SwapChain->GetFullscreenState(&bNow, nullptr)))
        return -1;
    return bNow ? 1 : 0;
}

UBOOL FBackend::Resize(INT InWidth, INT InHeight)
{
    if (!SwapChain)
        return 0;
    if (InWidth <= 0 || InHeight <= 0)
        return 0;

    // Each step of a resize says it is about to begin: a crash between two of them is then placed by the last line.
    RdxLog(TEXT("resize: to %ix%i at %ix supersampling - waiting for the GPU"), InWidth, InHeight, SceneScale);
    WaitForGpu();
    // From here the targets are let go one after another and made again: until the last of them is, there is nothing
    // whole to draw into (TargetsUnmade) - every way out below that fails leaves it said.
    bTargetsUnmade = 1;
    ReleaseBackBuffers();
    // The host's textures follow the sizes: the next frame that needs them hands it new ones (FHostUpscale::Prepare).
    // Frame generation's finished picture too (EnsureFgFinal).
    Host.ReleaseTextures();
    FgFinal.Release();

    Width  = InWidth;
    Height = InHeight;
    // The two settings compose - see RdxMath::SceneSideFor. Aa* is what the upscaler produces and what the box filter
    // then averages down to the output; with supersampling off it IS the output's size.
    AaWidth     = RdxMath::AntialiasedSideFor(Width,  SceneScale);
    AaHeight    = RdxMath::AntialiasedSideFor(Height, SceneScale);
    SceneWidth  = RdxMath::SceneSideFor(Width,  SceneScale, (RdxMath::EUpscaling)UpscaleMode);
    SceneHeight = RdxMath::SceneSideFor(Height, SceneScale, (RdxMath::EUpscaling)UpscaleMode);

    // The FORMAT is passed again and it is the same one: a resize does not change the output mode, only the size.
    // That is also why the colour space is not set again here - DXGI keeps it across a resize that keeps the
    // format, and the one place the format really moves (SetHdrMode) sets it itself.
    const UINT Flags = bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    RdxLog(TEXT("resize: the swap chain's buffers, the scene %ix%i"), SceneWidth, SceneHeight);
    const HRESULT Resized = SwapChain->ResizeBuffers(SwapBuffers, Width, Height, Colours.BackBuffer, Flags);
    // A device lost since the last Present shows here first: said as such, so that nothing is drawn into targets this
    // resize has let go and could not make again.
    if (Resized == DXGI_ERROR_DEVICE_REMOVED || Resized == DXGI_ERROR_DEVICE_RESET || Resized == DXGI_ERROR_DEVICE_HUNG)
        NoteDeviceLost(Resized, TEXT("ResizeBuffers"));
    if (!Check(Resized, TEXT("ResizeBuffers")))
        return 0;

    FrameIndex = SwapChain->GetCurrentBackBufferIndex();

    // Every slot restarts from the same fence value. Without this the new FrameIndex can
    // point at a slot whose old value was never signalled and never will be, and the very
    // next frame waits on it with INFINITE - the window stops answering and the game is
    // hung. Measured: Alt+Tab back made the engine call SetRes (fullscreen 3840x2160 ->
    // windowed 3840x2119), and the game froze immediately after "resized to".
    UINT64 Highest = 0;
    for (UINT i = 0; i < BACKEND_FRAME_COUNT; i++)
        if (FenceValues[i] > Highest)
            Highest = FenceValues[i];
    for (UINT i = 0; i < BACKEND_FRAME_COUNT; i++)
        FenceValues[i] = Highest;

    // A resize also invalidates whatever the swap chain thought about occlusion - and the previous frame's camera, whose
    // picture was another size.
    bOccluded     = 0;
    bPrevCamValid = 0;

    RdxLog(TEXT("resize: the frame's resources"));
    if (!CreateFrameResources())
        return 0;
    RdxLog(TEXT("resize: the depth buffers"));
    const UBOOL bShadowsWere = bShadows;
    if (!CreateDepthBuffer())
        return 0;
    // The shadows' own targets could not be made, and the shadows went (CreateDepthBuffer): the pipelines standing for
    // their depth format are made again for the plain one.
    if (bShadows != bShadowsWere && !RebuildForDepthFormat())
        return 0;
    RdxLog(TEXT("resize: the scene target"));
    if (!CreateSceneTarget())
        return 0;
    RdxLog(TEXT("resize: the multisampled targets"));
    if (!CreateMsaaTargets())
        return 0;
    RdxLog(TEXT("resize: the output target"));
    if (HasOutputTarget() && !CreateOutputTarget())
        return 0;
    // SMAA's edge picture is the output's size, and its view of the colour points at a target that has just been made
    // again: both follow every resize, whoever asked for it.
    if (PostAaKind == 2 && FxaaReady > 0 && !CreateSmaaTargets())
    {
        FxaaReady = -1;
        RDX_LOG("antialiasing: SMAA's own targets could NOT be made again - the picture runs unsmoothed");
    }

    bTargetsUnmade = 0;
    RDX_LOGF("resized to %ix%i", Width, Height);
    return 1;
}

// The geometry pass's pipelines and the multisampled depth's resolve are made for one depth format - with the shadows
// the depth buffer carries the stencil kinds (D32_FLOAT_S8X24_UINT) and the pass writes three targets, without them
// D32_FLOAT and one. Drawn with the other's pipelines, every draw of the world would disagree with the depth buffer it
// is drawn into: made again here for what bShadows says now - where the shadow quality changes, and where the shadows
// go because their own targets could not be made (CreateDepthBuffer).
UBOOL FBackend::RebuildForDepthFormat()
{
    Solid.SetStencilKinds(bShadows);
    if (!Solid.RebuildPipelines(Device.Get()))
    {
        RDX_LOG("shadows: the geometry pass's pipelines could not be made for the new depth format");
        return 0;
    }
    if (MsaaCount > 1 && !Final.InitMsaaDepth(Device.Get(), MsaaCount, bShadows))
    {
        RDX_LOG("MSAA: the depth resolve pass could not be made for the new depth format - MSAA is dropped");
        MsaaCount = 1;
        Solid.SetMsaaSamples(1);
        Solid.RebuildPipelines(Device.Get());
        ReleaseMsaaTargets();
    }
    return 1;
}

// The shadow preset changed in the RUNNING game (the renderer's menu). Only between frames.
//
// The order is the point: the cube arrays are made first, because CreateDepthBuffer (inside Resize) writes the views
// that point AT them, and a view left pointing at a freed array is a black picture at best. Resize also decides the
// depth buffer's format from bShadows and makes or drops the light and added targets, so turning the shadows on from
// nothing works too - it is the same path the device takes at startup.
UBOOL FBackend::SetShadowQuality(INT Quality)
{
    if (!Device || bInFrame)
        return 0;
    const RdxMath::FShadowPreset OldPreset = ShadowPreset;
    const UBOOL                  bWas      = bShadows;
    RequestShadowQuality(Quality);
    OverrideShadowPreset(OverrideActorMap, OverrideTaps, OverrideFullRes);   // the ini's own tweaks, again
    // Nothing of the shadows' own to change - and the menu applies its WHOLE state on every change it makes, the
    // occlusion's and the output's alike, so that is most of the time. Making the maps again is neither free nor
    // harmless: the cube arrays are some 300 MB at High, torn down and built again between two frames, and every shape
    // the level had placed in their arena goes with them. What else a preset holds (the reach, the taps, the lights at
    // full strength) is read frame by frame; the factor's resolution is not, so a change of it takes the long way below.
    if (bShadows == bWas && (!bShadows || (Shadow.IsReady() && RdxMath::ShadowMapsSame(ShadowPreset, OldPreset) &&
                                           ShadowPreset.bFullResolution == OldPreset.bFullResolution)))
        return 1;
    // How long each part takes, for the log: the change stops the game while it runs.
    const DOUBLE T0 = appSeconds();
    WaitForGpu();                 // the maps are still in the GPU's hands until this returns
    const DOUBLE T1 = appSeconds();
    // From here on the maps are made again (or dropped), and the arena and the masks with them. The generation tells
    // RenderDevice that what it placed in them is gone: it hands the level over again and forgets its shapes, exactly as
    // after a device made again (GetShadowGeneration).
    ShadowGeneration++;
    if (bShadows)
    {
        if (!Shadow.Init(Device.Get(), ShadowPreset))
        {
            RDX_LOG("shadows: the new quality's maps could not be created - keeping the previous one");
            ShadowPreset = OldPreset;
            bShadows     = bWas;
            if (bShadows)
                Shadow.Init(Device.Get(), ShadowPreset);
            return 0;
        }
    }
    else
        Shadow.Shutdown();
    const DOUBLE T2 = appSeconds();
    // The Resize below makes the buffers in the new format: the pipelines first (RebuildForDepthFormat).
    if (bShadows != bWas && !RebuildForDepthFormat())
        return 0;
    const DOUBLE T3 = appSeconds();
    if (!Resize(Width, Height))
        return 0;
    if (bShadows && !EnsureShadowTarget())
        return 0;
    const DOUBLE T4 = appSeconds();
    RDX_LOGF("shadows: quality changed while running - %i lights, actor cube %i, level cube %i, %i taps, factor %s; in "
             "%.0f ms (the GPU's last frames %.0f, the maps %.0f, the pipelines %.0f, the targets %.0f)",
             ShadowPreset.MaxLights, ShadowPreset.ActorMapSize, ShadowPreset.WorldMapSize, ShadowPreset.FilterTaps,
             ShadowPreset.bFullResolution ? TEXT("full") : TEXT("half"), (T4 - T0) * 1000.0, (T1 - T0) * 1000.0,
             (T2 - T1) * 1000.0, (T3 - T2) * 1000.0, (T4 - T3) * 1000.0);
    return 1;
}

// Everything the AntiAliasing key decides, changed in the RUNNING game (the renderer's menu): the upscaling mode,
// the supersampling factor and the post filter, in one step, because they share the targets and rebuilding those
// twice would be wasted work. Only between frames.
//
// Everything the menu can change has to take effect in the running game. A setting that only answers on the next
// start does not answer at all.
UBOOL FBackend::SetAntiAliasing(INT Mode, INT Scale, INT PostKind, FLOAT PostSpan, INT Msaa)
{
    if (!Device || bInFrame)
        return 0;
    // The multisampling first, and on its own: it owns the pipelines as well as the targets, and ApplyMsaaCount
    // returns everything to one sample if any part of it will not come up. Whatever it settles on, the Resize below
    // makes the targets to match.
    RdxLog(TEXT("antialiasing: asked for upscaling %i, %ix supersampling, filter %i, MSAA %i"), Mode, Scale, PostKind, Msaa);
    // The GPU first: what follows makes pipelines again - the multisampled set, the upscaler's, the filters', the box -
    // and a pipeline made again releases the one before it, which the frames still in the GPU's hands draw with. From
    // SSAA4X to SSAA9X, say, the box filter's would go while the last frames still draw with it: a general protection
    // fault.
    WaitForGpu();
    const INT OldMsaa = MsaaCount, OldWanted = MsaaWanted;
    MsaaWanted = (Msaa > 1) ? Msaa : 1;
    // No targets yet: the Resize at the end of this function builds them, and at the NEW scene size. Making them
    // here would make them at the old one first - measured: stepping from SSAA16X to MSAA2X allocated a
    // 15360x8640 two-sample set (about a gigabyte) for the moment it took to get to the Resize.
    ApplyMsaaCount(PickMsaaCount(MsaaWanted), 0);
    const INT   WantMode  = (Mode >= 1 && Mode <= 4) ? Mode : 0;
    const INT   WantScale = (Scale < 1) ? 1 : ((Scale > (INT)RdxMath::SuperSampleMax) ? (INT)RdxMath::SuperSampleMax
                                                                                      : Scale);
    const INT   WantPost  = (PostKind >= 1 && PostKind <= 2) ? PostKind : 0;
    if (WantMode == UpscaleMode && WantScale == SceneScale && WantPost == PostAaKind)
    {
        // Nothing else changed, so no Resize is coming: if the sample count moved, its targets are made here - and
        // if it did not, nothing is touched at all. Rebuilding them for an unchanged count would throw away a good
        // set every time the menu applies.
        if (MsaaCount == OldMsaa || CreateMsaaTargets())
            return 1;
        // The new count's targets could not be made, and the pipelines already draw with it: back to the count that
        // ran, with its targets - and to one sample where those will not come back either. Pipelines and targets of
        // different counts fail on every triangle.
        MsaaWanted = OldWanted;
        if (!ApplyMsaaCount(OldMsaa, 1))
            ApplyMsaaCount(1, 1);
        return 0;
    }

    const INT   OldMode = UpscaleMode, OldScale = SceneScale, OldPost = PostAaKind;
    const FLOAT OldSpan = PostAaSpan;
    // What ran before, for every way out below that fails: the mode, the size and the filter - and the sample count,
    // applied above already. Left as it is, the size that ran would be made again with a count that never ran at it:
    // on the way back from a multisampled mode to SSAA16X that is a multisampled set of sixteen times the picture.
    const auto PutBack = [&]()
    {
        UpscaleMode = OldMode;
        SceneScale  = OldScale;
        PostAaKind  = OldPost;
        PostAaSpan  = OldSpan;
        MsaaWanted  = OldWanted;
        ApplyMsaaCount(OldMsaa, 0);
    };
    UpscaleMode = WantMode;
    SceneScale  = WantScale;
    PostAaKind  = WantPost;
    PostAaSpan  = PostSpan;
    if (UpscaleMode)
    {
        if (!Final.InitUpscale(Device.Get()))
        {
            PutBack();
            return 0;
        }
        bMotionWanted = 1;
        bJitterWanted = 1;
        if (!bUpscaleLinear)
            bFsr3Wanted = 1;
    }
    if (PostAaKind == 1)
    {
        FxaaReady = Final.InitFxaa(Device.Get()) ? 1 : -1;
        if (FxaaReady < 0)
            RDX_LOG("antialiasing: the FXAA pass could NOT be created - the picture runs unsmoothed");
    }
    else if (PostAaKind == 2)
    {
        FxaaReady = Final.InitSmaa(Device.Get()) ? 1 : -1;
        if (FxaaReady < 0)
            RDX_LOG("antialiasing: the SMAA passes could NOT be created - the picture runs unsmoothed");
    }
    // The scene's size and every target that follows it. The box pass is needed as soon as supersampling is on.
    RdxLog(TEXT("antialiasing: the box filter, then the targets"));
    if (SceneScale > 1 && !Final.InitBox(Device.Get()))
    {
        PutBack();
        return 0;
    }
    if (!Resize(Width, Height))
    {
        // The targets before the change, made again. Where they will not come back either, the next frame finds them
        // unmade and the renderer is brought up again whole (TargetsUnmade).
        PutBack();
        Resize(Width, Height);
        return 0;
    }
    // The upscaler's context belongs to the old sizes; the next frame builds one for the new - one that was refused
    // at the old sizes too: it may fit the new ones.
    Fsr3.Shutdown();
    Fsr3Ready  = 0;
    bFsr3Reset = 1;
    // A host that went down is tried once more for the new setting (SyncHost starts it at the next frame).
    if (Host.IsDown())
        StopHost();
    RDX_LOGF("antialiasing: mode %i, %ix%i samples, filter %i - scene %ix%i, smoothed image %ix%i, output %ix%i",
             UpscaleMode, SceneScale, SceneScale, PostAaKind, SceneWidth, SceneHeight, AaWidth, AaHeight,
             Width, Height);
    return 1;
}

UBOOL FBackend::SetUpscaling(INT Mode)
{
    const INT Want = (Mode >= 1 && Mode <= 4) ? Mode : 0;
    if (!Device || bInFrame)
        return 0;
    if (Want == UpscaleMode)
        return 1;

    const INT Was = UpscaleMode;
    UpscaleMode = Want;
    if (UpscaleMode)
    {
        if (!Final.InitUpscale(Device.Get()))
        {
            UpscaleMode = Was;
            return 0;
        }
        // Turned on from Off, everything the temporal upscaler needs has to appear now. None of it is a startup-only decision after all:
        // - the jitter and the vector pass are flags, and their work follows UpscaleMode from here on;
        // - the motion target makes itself on first use (EnsureMotionTarget);
        // - the upscaler's context is built lazily, in the first frame that needs it (ResolveScene);
        // - the depth buffer is the one real resource question - it must be READABLE (NeedsDepthRead), and its format
        // is fixed when it is created. Setting the flags here, BEFORE the Resize below, is what makes that Resize
        // build it in the readable format. With shadows on it already was one.
        bMotionWanted = 1;
        bJitterWanted = 1;
        if (!bUpscaleLinear)
            bFsr3Wanted = 1;
    }
    // The scene's size changed: the depth buffer, the scene target, the output target and the light targets go with it -
    // which is what Resize makes, and the swap chain's buffers with them, costing nothing at the same size.
    if (!Resize(Width, Height))
    {
        // The targets of the size before, made again (see SetAntiAliasing).
        UpscaleMode = Was;
        Resize(Width, Height);
        return 0;
    }
    // The upscaler's context belongs to the OLD scene size, and the SDK asks for an idle GPU before one is destroyed.
    // Here is the one place where that is certainly true: Resize has just waited. Left to itself, the destroy would happen
    // in the next frame's ResolveScene, in the middle of recording - safe today only because of that same wait, and a trap
    // for whoever moves either of them. The next frame builds a new context for the new size.
    Fsr3.Shutdown();
    bFsr3Reset = 1;
    if (Host.IsDown())
        StopHost();
    DOUBLE MemUsed = 0.0, MemBudget = 0.0;
    VideoMemoryMB(MemUsed, MemBudget);
    RDX_LOGF("video memory: %.0f MB used, %.0f MB budget%s", MemUsed, MemBudget,
             (MemBudget > 0.0 && MemUsed > MemBudget) ? TEXT(" - OVER BUDGET, the system pages over PCIe: this can mean ten times the "
                                                             "frame time") : TEXT(""));
    RDX_LOGF("FSR: mode %i - scene %ix%i, smoothed image %ix%i, output %ix%i%s", UpscaleMode, SceneWidth,
             SceneHeight, AaWidth, AaHeight, Width, Height,
             (!Was && UpscaleMode) ? TEXT(" (switched on while running: jitter and motion vectors start now)")
                                   : ((Was && !UpscaleMode) ? TEXT(" (switched off: no jitter, no motion vectors)")
                                                            : TEXT("")));
    return 1;
}

void FBackend::Shutdown()
{
    if (Device)
        WaitForGpu();
    ReleaseTreeMesh();
    if (CloudPuffs)
        appFree(CloudPuffs);
    if (CloudTracks)
        appFree(CloudTracks);
    if (CloudTileData)
        appFree(CloudTileData);
    if (CloudPuffRects)
        appFree(CloudPuffRects);
    if (CloudCellHead)
        appFree(CloudCellHead);
    if (CloudCellNext)
        appFree(CloudCellNext);
    CloudPuffs      = nullptr;
    CloudTracks     = nullptr;
    CloudTileData   = nullptr;
    CloudPuffRects  = nullptr;
    CloudCellHead   = nullptr;
    CloudCellNext   = nullptr;
    CloudPuffHeld   = 0;
    CloudTrackHeld  = 0;
    CloudTileHeld   = 0;
    CloudRectHeld   = 0;
    CloudCellHeld   = 0;
    CloudNextHeld   = 0;
    CloudPuffCount  = 0;
    CloudTrackCount = 0;
    if (FirePieces)
        appFree(FirePieces);
    if (FireBodies)
        appFree(FireBodies);
    if (FireGroups)
        appFree(FireGroups);
    if (MirrorFlames)
        appFree(MirrorFlames);
    if (MirrorBodies)
        appFree(MirrorBodies);
    if (MirrorGroups)
        appFree(MirrorGroups);
    if (MirrorEnds)
        appFree(MirrorEnds);
    if (MirrorTris)
        appFree(MirrorTris);
    if (PanePlanes)
        appFree(PanePlanes);
    if (MirrorFalls)
        appFree(MirrorFalls);
    if (MirrorClouds)
        appFree(MirrorClouds);
    if (FireTileData)
        appFree(FireTileData);
    if (FireRects)
        appFree(FireRects);
    if (ShaftTileData)
        appFree(ShaftTileData);
    if (ShaftRects)
        appFree(ShaftRects);
    ShaftTileData = nullptr;
    ShaftRects    = nullptr;
    ShaftTileHeld = 0;
    ShaftRectHeld = 0;
    if (FogLights)
        appFree(FogLights);
    FogLights     = nullptr;
    FogLightHeld  = 0;
    FogLightCount = 0;
    if (FieldKeys)
        appFree(FieldKeys);
    if (FieldSizes)
        appFree(FieldSizes);
    if (FieldPoints)
        appFree(FieldPoints);
    if (FieldRowsList)
        appFree(FieldRowsList);
    if (FieldKinds)
        appFree(FieldKinds);
    if (FieldKindNames)
        appFree(FieldKindNames);
    if (FieldKindLooks)
        appFree(FieldKindLooks);
    if (FieldFewSizes)
        appFree(FieldFewSizes);
    if (FieldFewKinds)
        appFree(FieldFewKinds);
    if (FieldFewPoints)
        appFree(FieldFewPoints);
    FieldKeys      = nullptr;
    FieldSizes     = nullptr;
    FieldPoints    = nullptr;
    FieldRowsList  = nullptr;
    FieldKinds     = nullptr;
    FieldKindNames = nullptr;
    FieldKindLooks = nullptr;
    FieldFewSizes  = nullptr;
    FieldFewKinds  = nullptr;
    FieldFewPoints = nullptr;
    FieldKeysHeld = FieldSizesHeld = FieldPointsHeld = FieldRowsHeld = 0;
    FieldKindsHeld = FieldKindNamesHeld = FieldKindLooksHeld = 0;
    FieldFewSizesHeld = FieldFewKindsHeld = FieldFewPointsHeld = 0;
    FieldCount     = 0;
    FieldKindCount = 0;
    if (HoloVerts)
        appFree(HoloVerts);
    if (HoloRuns)
        appFree(HoloRuns);
    if (HoloRunSrvs)
        appFree(HoloRunSrvs);
    HoloVerts     = nullptr;
    HoloRuns      = nullptr;
    HoloRunSrvs   = nullptr;
    HoloVertCount = HoloVertHeld = HoloRunCount = HoloRunHeld = HoloRunSrvHeld = 0;
    FirePieces    = nullptr;
    FireBodies    = nullptr;
    FireGroups    = nullptr;
    MirrorFlames  = nullptr;
    MirrorBodies  = nullptr;
    MirrorGroups  = nullptr;
    MirrorFlameCount = MirrorFlameHeld = MirrorBodyCount = MirrorBodyHeld = MirrorGroupHeld = 0;
    MirrorEnds       = nullptr;
    MirrorEndCount   = MirrorEndHeld = 0;
    MirrorTris       = nullptr;
    MirrorTriCount   = MirrorTriHeld = 0;
    PanePlanes       = nullptr;
    PaneCount        = PaneHeld = 0;
    MirrorFalls      = nullptr;
    MirrorFallCount  = MirrorFallHeld = MirrorFallMark = 0;
    MirrorClouds     = nullptr;
    MirrorCloudCount = MirrorCloudHeld = MirrorCloudMark = 0;
    FireTileData  = nullptr;
    FireRects     = nullptr;
    FirePieceHeld = 0;
    FireBodyHeld  = 0;
    FireGroupHeld = 0;
    FireTileHeld  = 0;
    FireRectHeld  = 0;
    Fire.Hold(nullptr, 0);
    Fire.Begin();
    // The upscaler holds a context and targets of its own: they have to go while the device is still here and
    // the GPU is idle - the SDK asks for exactly that before a context is destroyed. The host's share the device too.
    Fsr3.Shutdown();
    StopHost();
    RayFeed.Release();
    // After the host: its fence lets the presenting queue's last wait go.
    Pacer.Shutdown();
    // The pacer's thread is gone: nothing calls Anti-Lag 2 any more, and it goes before the device does - as Reflex
    // does.
    Pacer.SetAntiLag(nullptr);
    RdxAntiLag2::DeInitialize(&AntiLag);
    RdxReflexStop();
    if (PaceEvent)
        ::CloseHandle(PaceEvent);
    PaceEvent     = nullptr;
    PaceCpuCount  = PaceGpuCount = 0;
    PaceFenceLast = PaceFencePrev = 0;
    PaceSlotLast  = PaceSlotPrev = -1;
    FgFinal.Release();
    DrainDebugMessages(1);
    FlushFrameCapture();
    if (FrameFile)
    {
        ::CloseHandle(FrameFile);
        FrameFile = nullptr;
    }

    // DXGI's rule: a swap chain must not be released while in its fullscreen state.
    if (SwapChain && QueryFullscreen() == 1)
        SwapChain->SetFullscreenState(FALSE, nullptr);
    bExclusive = 0;

    ReleaseBackBuffers();

    UploadList.Release();
    if (UploadAllocator)
    {
        UploadAllocator->Release();
        UploadAllocator = nullptr;
    }
    bUploadOpen = 0;
    ReleaseCardWriter();

    for (UINT i = 0; i < BACKEND_FRAME_COUNT; i++)
    {
        if (Allocators[i])
        {
            Allocators[i]->Release();
            Allocators[i] = nullptr;
        }
    }

    if (FenceEvent)
    {
        CloseHandle(FenceEvent);
        FenceEvent = nullptr;
    }

    Pom.Shutdown();
    Textures.Shutdown();
    Solid.Shutdown();
    Final.Shutdown();
    Dof.Shutdown();
    DofReady = 0;
    // The passes made on first use hold the device's own objects: with it they go, and are made again when next asked.
    Field.Shutdown();
    FieldReady = 0;
    Shell.Shutdown();
    ShellReady = 0;
    HoloMisses = 0;
    SceneColor.Release();
    SceneSrvHeap.Release();
    WaterColor.Release();
    WaterDepth.Release();
    ReliefCover.Release();
    ReliefCoverDsvHeap.Release();
    bReliefCoverNone = 0;
    WaveField.Release();
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        WaveUpload[f].Release();
        WaveUploadMapped[f] = nullptr;
    }
    WaterLampBuffer.Release();
    WaterLampMapped  = nullptr;
    bWaterLampFailed = 0;
    WetLampBuffer.Release();
    WetLampMapped  = nullptr;
    bWetLampFailed = 0;
    ReliefLampBuffer.Release();
    ReliefLampMapped  = nullptr;
    bReliefLampFailed = 0;
    FluidDepth.Release();
    FluidThick.Release();
    FluidSmooth.Release();
    Reactive.Release();
    ReactiveState     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ReactiveRtvHeap.Release();
    WaterSurface.Release();
    WaterSurfaceState     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bWaterSurfaceLive     = 0;
    bWaterSurfaceCleared  = 0;
    bWaterSurfaceViewReal = 0;
    WaterSurfaceRtvHeap.Release();
    WetMark.Release();
    WetMarkState          = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bWetMarkCleared       = 0;
    bWetMarkViewReal      = 0;
    bWetMarkNone          = 0;
    WetMarkRtvHeap.Release();
    WaterShade.Release();
    WaterShadeState        = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bWaterShadeLive        = 0;
    bWaterShadeViewReal    = 0;
    bWaterShadeFailed      = 0;
    WaterFactor.Release();
    WaterPack.Release();
    WaterFactorRtvHeap.Release();
    WaterFactorW = WaterFactorH = WaterPackW = WaterPackH = 0;
    WaterPackReady = 0;
    bWaterShadowViewsDirty = 1;
    SplashRtvHeap.Release();
    SplashDropPipeline.Release();
    SplashSmoothPipeline[0].Release();
    SplashSmoothPipeline[1].Release();
    SplashRoot.Release();
    SplashConstBuffer.Release();
    SplashConstMapped = nullptr;
    bSplashFailed     = 0;
    RippleField[0].Release();
    RippleField[1].Release();
    CausticNet.Release();
    CausticSrvHeap.Release();
    CausticRtvHeap.Release();
    CausticSplat.Release();
    CausticHalve.Release();
    CausticRoot.Release();
    CausticLays[0].Release();
    CausticLays[1].Release();
    CausticMaskPipeline.Release();
    CausticLayRoot.Release();
    CausticMask.Release();
    CausticMaskRtvHeap.Release();
    CausticPortalBuffer.Release();
    CausticPortalMapped = nullptr;
    bCausticsDrawn = 0;
    RippleSrvHeap.Release();
    RippleRtvHeap.Release();
    RipplePipeline.Release();
    RippleRoot.Release();
    Readback.Release();
    OutputColor.Release();
    OutputDepth.Release();
    UpscaleTemp.Release();
    UpscaleRtvHeap.Release();
    UpscaleSrvHeap.Release();
    MotionTarget.Release();
    MotionRtvHeap.Release();
    MotionSrvHeap.Release();
    MotionW = MotionH = 0;
    // The moving bodies' buffers and the passes' pipelines (Final.Shutdown let those go): made again on first use.
    for (INT s = 0; s < BACKEND_FRAME_COUNT; s++)
    {
        MotionVb[s].Release();
        MotionVbMapped[s] = nullptr;
    }
    MotionVbState   = 0;
    MotionVbUsed    = 0;
    MotionBodyCount = 0;
    MotionReady     = 0;
    MotionViewReady = 0;
    MotionObjReady  = 0;
    OutputRtvHeap.Release();
    OutputDsvHeap.Release();
    OutputSrvHeap.Release();
    DepthBuffer.Release();
    DsvHeap.Release();
    DepthSrvHeap.Release();
    DepthBuffer2.Release();
    DsvHeap2.Release();
    DepthViewReady = 0;
    ShadowColor.Release();
    ShadowRtvHeap.Release();
    ShadowSrvHeap.Release();
    LightTarget.Release();
    OutputLightTarget.Release();
    AddedTarget.Release();
    OutputAddedTarget.Release();
    LightRtvHeap.Release();
    Shadow.Shutdown();
    ShadowReady = 0;
    ShadowW = ShadowH = 0;
    AoRaw.Release();
    AoDepth.Release();
    RtFactorRaw.Release();
    AoRtvHeap.Release();
    AoSrvHeap.Release();
    AoReady = 0;
    RtFactorBlurReady = 0;
    AoW = AoH = 0;
    CaptionReady   = 0;
    CaptionText[0][0] = 0;
    if (TimeReadback && TimeMapped)
        TimeReadback->Unmap(0, nullptr);
    TimeMapped = nullptr;
    TimeReadback.Release();
    TimeHeap.Release();
    for (INT s = 0; s < BACKEND_FRAME_COUNT; s++)
    {
        TimePlaced[s] = 0;
        TimeLive[s]   = 0;
    }
    Fence.Release();
    CommandList.Release();
    RtvHeap.Release();
    SwapChain.Release();
    Queue.Release();
    InfoQueue.Release();
    Rt.Shutdown();
    MirrorProbe.Shutdown();
    MirrorBody.Shutdown();
    WetReflect.Shutdown();
    WetReflectReady = 0;
    RaySparse.Shutdown();
    bSparseTried     = 0;
    bProbeTried      = 0;
    bProbeRefused    = 0;
    bProbeViewsDirty = 1;
    Device5.Release();
    Device.Release();
    Factory.Release();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void FBackend::BeginFrame(const FPlane& ClearColour)
{
    if (!Device || bInFrame)
        return;
    // The start's clock (RdxStartupMark): the engine's first frame.
    static UBOOL bFirstFrame = 1;
    if (bFirstFrame)
    {
        bFirstFrame = 0;
        RdxStartupMark(TEXT("the first frame begins"));
    }
    DrainDebugMessages(0);
    SyncHost();
    // The plants' written memory, once their copy onto the card is done with (TreeMeshWritten).
    if (TreeMeshStaging && TreeMeshStagingFrames > 0 && --TreeMeshStagingFrames == 0)
        ReleaseTreeMeshStaging();
    if (bDredWanted)
        NameResources();
    bFgFrame     = 0;
    bHudLessDone = 0;
    FgWhy        = !bFrameGenWanted ? 1 : (!bFgMode ? 2 : 3);
    // The passes' targets that could not be made (RefuseTarget): each refusal with what the targets are made by now,
    // which is another only between frames.
    {
        const RdxMath::FTargetAsk    Ask   = TargetAsk();
        RdxMath::FTargetRefusal* const All[] = { &VolumeRefusal, &BlurRefusal, &DofRefusal, &FilmRefusal,
                                                 &BloomRefusal, &ShadowRefusal, &AoRefusal, &CausticRefusal };
        for (INT i = 0; i < (INT)(sizeof(All) / sizeof(All[0])); i++)
            All[i]->Keep(Ask);
    }
    // The puffs of smoke are a frame's worth, not a level's: they are gathered as the game's cards arrive and drawn
    // together at the end of the world, so the list starts empty every frame. What the LAST frame held decides
    // whether the depth copy is worth making for them at all.
    if (CloudPuffCount > 0 || CloudCardsSeen > 0)
        CloudsRecent = CLOUDS_RECENT_FRAMES;
    else if (CloudsRecent > 0)
        CloudsRecent--;
    CloudCardsSeen = 0;
    ClearCloudPuffs();
    if (Fall.Count > 0 || FallSheetsSeen > 0)
        FallsRecent = CLOUDS_RECENT_FRAMES;
    else if (FallsRecent > 0)
        FallsRecent--;
    FallSheetsSeen = 0;
    Fall.Begin();
    if (Fire.Count > 0 || FireSheetsSeen > 0)
        FiresRecent = CLOUDS_RECENT_FRAMES;
    else if (FiresRecent > 0)
        FiresRecent--;
    FireSheetsSeen = 0;
    Fire.Begin();
    BloomCoronaCount = 0;         // the coronas, for the bloom's own halo, are a frame's worth too (NoteCorona)
    OwnLampCount     = 0;         // and the own lamps, the flashlight's images and what is made of them
    bOwnWorld        = 0;
    OwnRowsLamps     = 0;
    LampImageCount   = 0;
    BloomLampCount   = 0;
    LampGlareCount   = 0;
    // Before it is cleared: whether the copy the walk needs was there at all. DrawTile reads this to decide
    // whether a cloud card may be gathered rather than drawn.
    {
        // THE MOMENT IT FLIPS, with everything that could have flipped it. Asking the player to repeat a test
        // is not a diagnosis: this is one line, at the one frame where the answer is.
        const UBOOL Was = bCloudDepthLast;
        bCloudDepthLast = (bDepthCopyDone && WaterDepth) ? 1 : 0;
        if (Was != bCloudDepthLast)
            RdxLog(TEXT("the smoke is gathered as volumes: %s (the depth copy %i, its target %i, clouds on %i, "
                        "soft fade %.1f, water wanted %i, cards seen %i, clouds recent %i, the copy was asked "
                        "for %i times and said no %i times)"),
                   bCloudDepthLast ? TEXT("YES") : TEXT("NO - the flat sprites are drawn"),
                   (INT)bDepthCopyDone, (INT)(WaterDepth.Get() != nullptr), (INT)Solid.GetCloudsOn(),
                   Solid.GetSoftFade(), (INT)bWaterWanted, CloudCardsSeen, CloudsRecent, CopyAsked, CopyRefused);
    }
    bDepthCopyDone = 0;
    // Where the mirror probe stands this frame, from the panes the last one drew and where its camera was; its passes
    // not run yet.
    if (bCamSet && IsProbeMirrorRunning())
        MirrorProbe.BeginFrame(CamEye);
    bProbeDone = 0;
    // The frame's mirrors' views, and all that was gathered for their walks.
    MirrorEndCount    = 0;
    MirrorTriCount    = 0;
    bBounceNow        = 0;
    PaneCount         = 0;
    bPaneNow          = 0;
    MirrorFlameCount  = MirrorBodyCount = 0;
    MirrorCloudCount  = MirrorCloudMark = 0;
    MirrorFallCount   = MirrorFallMark = 0;
    bMirrorTellsKnown = 0;
    bWeaponDepthPut = 0;
    FieldCount = 0;
    FieldKindCount = 0;
    HoloVertCount = 0;
    HoloRunCount  = 0;
    bCubesDrawnLast = bCubesDrawn;
    bCubesDrawn    = 0;
    CopyAsked      = 0;
    CopyRefused    = 0;

    // Anything precached between frames goes to the queue first, so the copies are done
    // before the drawing that samples them, and the frame's fence covers their staging.
    SubmitUploadList();

    ID3D12CommandAllocator* Allocator = Allocators[FrameIndex];
    if (FAILED(Allocator->Reset()))
        return;
    // The allocator's last frame is done on the GPU (its reset went through): its times, before this frame's overwrite them.
    ReadGpuTime(FrameIndex);
    RayFeed.NoteListTaken((INT)FrameIndex);
    if (FAILED(CommandList->Reset(Allocator, nullptr)))
        return;
    GpuTime(CommandList.Get(), 0);
    // The water's ripples take their step before anything of the scene is bound: all of that is set below. The
    // caustics' net with them, where the frame before had something to lay it on (CausticsNow, asked before this
    // frame's view takes its place): a level whose water is all opaque, or a view with no clear water near, draws no
    // net. The first frame with clear water before it goes without its caustics; the next has the net.
    const UBOOL bNetWanted = CausticsNow();
    StepRipples(CommandList.Get());
    if (bNetWanted)
        DrawCausticNet(CommandList.Get());
    // This frame's world has not said its view yet (SetCausticView), and its picture has no caustics on it.
    bCausticView  = 0;
    bCausticsLaid = 0;

    D3D12_RESOURCE_BARRIER Barrier;
    appMemzero(&Barrier, sizeof(Barrier));
    Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    // The frame is drawn into a target of its own, not into the back buffer: the
    // brightness curve has to come after every blend, so it is a pass of its own at the
    // end - see FFinalPass. The back buffer is touched only there.
    Barrier.Transition.pResource   = SceneColor.Get();
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    CommandList->ResourceBarrier(1, &Barrier);

    // These two decide what BindSceneTargets binds just below, so they are set BEFORE it, not at the end of the
    // frame's set-up: leaving last frame's bOutputLive standing here would bind the single-sample targets for a
    // multisampled world, and the first draw would fail on the sample-count mismatch.
    bOutputLive   = 0;
    bMsaaResolved = 0;
    Solid.SetMsaaLive(MsaaOn());

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvStart;
    Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;       // the scene target's view
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvHeap->GetCPUDescriptorHandleForHeapStart();
    BindSceneTargets(CommandList.Get(), Dsv);
    CommandList->ClearDepthStencilView(Dsv, DepthClearFlags(), 0.0f, 0, 0, nullptr);

    const FLOAT Colour[4] = { ClearColour.X, ClearColour.Y, ClearColour.Z, 1.0f };
    CommandList->ClearRenderTargetView(Rtv, Colour, 0, nullptr);

    // The multisampled set is cleared beside the single-sample one, to the very same values: the world is drawn into
    // it, and the single-sample targets still have to be clean for the HUD, which comes after the resolve.
    if (MsaaOn())
    {
        D3D12_CPU_DESCRIPTOR_HANDLE MsRtv = MsaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
        CommandList->ClearRenderTargetView(MsRtv, Colour, 0, nullptr);
        if (bShadows && MsaaLight && MsaaAdded)
        {
            const FLOAT One[4]  = { 1.0f, 0.0f, 0.0f, 0.0f };
            const FLOAT Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            MsRtv.ptr += RtvStride;
            CommandList->ClearRenderTargetView(MsRtv, One, 0, nullptr);
            MsRtv.ptr += RtvStride;
            CommandList->ClearRenderTargetView(MsRtv, Zero, 0, nullptr);
        }
        CommandList->ClearDepthStencilView(MsaaDsvHeap->GetCPUDescriptorHandleForHeapStart(),
                                           DepthClearFlags(), 0.0f, 0, 0, nullptr);
    }
    // The shadows' light target: 1 where no surface writes its light, so a light's share there stays small. The added
    // target: nothing laid over anything yet.
    if (bShadows && LightTarget && LightRtvHeap)
    {
        const FLOAT One[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        CommandList->ClearRenderTargetView(LightRtvHeap->GetCPUDescriptorHandleForHeapStart(), One, 0, nullptr);
        if (AddedTarget)
        {
            const FLOAT Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            D3D12_CPU_DESCRIPTOR_HANDLE AddedRtv = LightRtvHeap->GetCPUDescriptorHandleForHeapStart();
            AddedRtv.ptr += 2 * RtvStride;
            CommandList->ClearRenderTargetView(AddedRtv, Zero, 0, nullptr);
        }
    }

    // The scene target's size. The engine's pixel coordinates still reach clip space through
    // SolidPass's InvHalfSize of the OUTPUT size, and clip space knows no resolution - so with
    // supersampling on, this viewport alone is what draws the frame at the larger size.
    D3D12_VIEWPORT Viewport;
    Viewport.TopLeftX = 0.0f;
    Viewport.TopLeftY = 0.0f;
    Viewport.Width    = (FLOAT)SceneWidth;
    Viewport.Height   = (FLOAT)SceneHeight;
    Viewport.MinDepth = 0.0f;
    Viewport.MaxDepth = 1.0f;
    CommandList->RSSetViewports(1, &Viewport);

    D3D12_RECT Scissor;
    Scissor.left   = 0;
    Scissor.top    = 0;
    Scissor.right  = SceneWidth;
    Scissor.bottom = SceneHeight;
    CommandList->RSSetScissorRects(1, &Scissor);

    // The staging buffers this slot used two frames ago are now safely consumed.
    Textures.ReleaseFrameStaging(FrameIndex);
    Pom.ReleaseStaging(FrameIndex);
    Textures.SetFrame(FrameIndex);
    Textures.EnsureWhite(CommandList.Get());

    bHudQueued  = 0;          // bOutputLive and bMsaaResolved were cleared before the targets were bound, above
    bWorldEndQueued = 0;
    ActiveDepth  = 0;         // BeginFrame bound and cleared the first depth buffer
    RetiredDepth = -1;
    WorldDepth   = -1;
    // The previous frame's camera for the motion vectors: this frame's becomes the old one, with the tangents it was
    // drawn with. It is usable only if a world was drawn last frame - in menus and while loading none is.
    bPrevCamValid = bCamSet;
    if (bCamSet)
    {
        for (INT i = 0; i < 3; i++)
            PrevEye[i] = CamEye[i];
        for (INT i = 0; i < 9; i++)
            PrevAxes[i] = CamAxes[i];
        PrevTanX = ViewTanX;
        PrevTanY = ViewTanY;
    }
    bCamSet         = 0;
    bMotionDone     = 0;
    bShadowViewSet  = 0;
    ShadowSkipCount = 0;
    bBeamOn         = 0;
    Solid.SetBeamLit(0);
    Solid.SetWet(0);
    bWetLayerNow    = 0;
    bRtSceneTried   = 0;
    bRtSceneBuilt   = 0;
    RtHostBodies    = -1;
    bRtHostTraced   = 0;
    RayFeed.Tick();
    if (RtMirrorIdle < RT_MIRROR_IDLE_FRAMES)
        RtMirrorIdle++;
    // The jitter for a temporal upscaler (RequestJitter): a fraction of a pixel along the Halton sequence, turned into
    // clip space. Without it the geometry pass adds nothing at all.
    // Last frame's shadow metric, if its fence has passed by now (never waits - see CollectShadowMetric).
    CollectShadowMetric();
    // UpscaleMode, not bJitterWanted alone: the wish is settled once, at startup, but the MODE can change in play
    // (Num Lock). Switched to Off there is no temporal pass left to resolve the offsets, and a jitter nobody resolves
    // is a picture that shivers half a pixel every frame.
    if (bJitterWanted && UpscaleMode && SceneWidth > 0 && SceneHeight > 0)
    {
        FLOAT Jx = 0.0f, Jy = 0.0f;
        RdxMath::FsrJitterOffset(JitterFrame++, RdxMath::FsrJitterPhaseCount(SceneWidth, Width), Jx, Jy);
        JitterClip[0] = 2.0f * Jx / (FLOAT)SceneWidth;
        JitterClip[1] = -2.0f * Jy / (FLOAT)SceneHeight;
        Solid.SetJitter(JitterClip[0], JitterClip[1]);
        // The upscaler is told the same offset, but in pixels: it has to know where this frame's samples fell.
        JitterPx[0] = Jx;
        JitterPx[1] = Jy;
    }
    else
    {
        Solid.SetJitter(0.0f, 0.0f);
        JitterClip[0] = 0.0f;
        JitterClip[1] = 0.0f;
        JitterPx[0] = 0.0f;
        JitterPx[1] = 0.0f;
    }
    // The upscaler's own clock: how much time the picture it blends into the history covers. Our present
    // diagnostics already measure what a performance-counter tick is worth (QpcMs).
    {
        LARGE_INTEGER Now;
        ::QueryPerformanceCounter(&Now);
        if (PrevFrameQpc && QpcMs > 0.0)
            FrameDeltaMs = (FLOAT)Clamp((DOUBLE)(Now.QuadPart - PrevFrameQpc) * QpcMs, 0.1, 1000.0);
        PrevFrameQpc = Now.QuadPart;
    }
    // The world's textures' mip bias: only while the temporal upscaler itself runs (not the bilinear pass) -
    // by XeSS's rule while the host's XeSS does the upscale, by FSR's otherwise.
    {
        const UBOOL bXess = bHostUpscaled && UpscalerChoice == (INT)RdxMath::Upscaler_Xess;
        const FLOAT Bias  = !(Fsr3Running() && !bUpscaleLinear) ? 0.0f
                          : bXess ? RdxMath::XessMipBias((RdxMath::EUpscaling)UpscaleMode)
                                  : RdxMath::FsrMipBias((RdxMath::EUpscaling)UpscaleMode, bNativeMipBias ? true : false);
        Solid.SetMipBias(Bias);
        if (Bias != MipBiasLogged)
        {
            MipBiasLogged = Bias;
            RdxLog(TEXT("FSR: the world's textures sampled with a mip bias of %.2f"), Bias);
        }
    }
    Solid.Begin(Width, Height, FrameIndex);
    // The water's lamps for this frame slot (SetWaterLamps writes them during the frame); without the buffer the water
    // pipeline cannot run, and the water is drawn the plain translucent way.
    bReactiveLive = 0;
    bReactiveInit = 0;
    bWaterSurfaceLive = 0;
    bWaterSurfaceCleared = 0;
    bWetMarkCleared = 0;
    bWaterShadeLive = 0;
    Solid.SetSplashViews(Textures.GetReservedIndex(SplashViewDepth), Textures.GetReservedIndex(SplashViewThick));
    // The relief's covers' view, and the view of nothing read in its place (ReliefCover): written once, before any covers
    // are made. A view of nothing reads zero, which the relief takes as nothing standing in front of anything.
    if (!bReliefCoverNone && Textures.GetHeap())
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
        appMemzero(&Srv, sizeof(Srv));
        Srv.Format                  = DXGI_FORMAT_R32_FLOAT;
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels     = 1;
        Device->CreateShaderResourceView(nullptr, &Srv, Textures.GetCpuHandle(Textures.GetReservedIndex(ReliefCoverNone)));
        if (!ReliefCover)
            Device->CreateShaderResourceView(nullptr, &Srv,
                                             Textures.GetCpuHandle(Textures.GetReservedIndex(ReliefCoverView)));
        bReliefCoverNone = 1;
    }
    Solid.SetCoverViews(bReliefCoverNone ? Textures.GetReservedIndex(ReliefCoverView) : -1,
                        bReliefCoverNone ? Textures.GetReservedIndex(ReliefCoverNone) : -1);
    // The wet floors' mark's two views: views of nothing until the mark is made (EnsureWetMark), and again once it is
    // let go - the wet floors' layer on rays reads the one in every frame it runs.
    if (!bWetMarkViewReal && !bWetMarkNone && Textures.GetHeap())
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
        appMemzero(&Ud, sizeof(Ud));
        Ud.Format        = DXGI_FORMAT_R32_UINT;
        Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Device->CreateUnorderedAccessView(nullptr, nullptr, &Ud, Textures.GetCpuHandle(Textures.GetReservedIndex(WetMarkUav)));
        D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
        appMemzero(&Sd, sizeof(Sd));
        Sd.Format                  = DXGI_FORMAT_R32_UINT;
        Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Sd.Texture2D.MipLevels     = 1;
        Device->CreateShaderResourceView(nullptr, &Sd, Textures.GetCpuHandle(Textures.GetReservedIndex(WetMarkSrv)));
        bWetMarkNone = 1;
    }
    // The rain's cover's view: of nothing until a level's cover is made (SetWetCover) - the wet runs' table holds it.
    if (!bWetCoverViewSet && Textures.GetHeap())
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
        appMemzero(&Sd, sizeof(Sd));
        Sd.Format                  = DXGI_FORMAT_R32G32_FLOAT;
        Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Sd.Texture2D.MipLevels     = 1;
        Device->CreateShaderResourceView(nullptr, &Sd, Textures.GetCpuHandle(Textures.GetReservedIndex(WetCoverSrv)));
        bWetCoverViewSet = 1;
    }
    // The mirrors' map's view (FBeamBounce): a view of nothing until there is a map - no mirror anywhere.
    if (!bBounceNone && Textures.GetHeap())
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
        appMemzero(&Srv, sizeof(Srv));
        Srv.Format                  = DXGI_FORMAT_R32G32_FLOAT;
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels     = 1;
        Device->CreateShaderResourceView(nullptr, &Srv,
                                         Textures.GetCpuHandle(Textures.GetReservedIndex(BounceMapView)));
        bBounceNone = 1;
        BounceMade  = -1;
    }
    Solid.SetBounceView(bBounceNone ? Textures.GetReservedIndex(BounceMapView) : -1);
    // The own lamps' table and maps: views of nothing until there are lamps - and again where the maps went with the
    // shadows' preset and nothing made them again.
    if ((!bOwnViewsNone || (OwnMapsMade >= 0 && !Shadow.GetSpotMaps())) && Textures.GetHeap())
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
        appMemzero(&Srv, sizeof(Srv));
        Srv.Format                  = DXGI_FORMAT_R32G32B32A32_FLOAT;
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels     = 1;
        if (!bOwnViewsNone)
            Device->CreateShaderResourceView(nullptr, &Srv,
                                             Textures.GetCpuHandle(Textures.GetReservedIndex(OwnLampTableView)));
        Shadow.DescribeSpotView(Srv);
        Device->CreateShaderResourceView(nullptr, &Srv,
                                         Textures.GetCpuHandle(Textures.GetReservedIndex(OwnLampMapsView)));
        bOwnViewsNone = 1;
        OwnMapsMade   = -1;
    }
    Solid.SetWaterLamps((bWaterWanted && EnsureWaterLamps())
                            ? WaterLampBuffer->GetGPUVirtualAddress() + (UINT64)FrameIndex * WaterLampBytes : 0);
    // The water's views, written before any copy is made: the sludge reads the ripples and the wave field from them with
    // no copy of its own (FSolidPass::CanSludge), and until they are written the heap's slots hold nothing.
    Solid.SetWaterTableReady((bWaterWanted && EnsureWaterTargets()) ? 1 : 0);
    // The relief's lamps for this slot, emptied until SetReliefLamps fills them during the frame.
    appMemzero(ReliefLampCopy, sizeof(ReliefLampCopy));
    if (EnsureReliefLamps())
    {
        appMemzero(ReliefLampMapped + FrameIndex * WaterLampBytes, 16);
        Solid.SetReliefLamps(ReliefLampBuffer->GetGPUVirtualAddress() + (UINT64)FrameIndex * WaterLampBytes);
    }
    else
        Solid.SetReliefLamps(0);
    // The wet floors' lamps for this slot, emptied until SetWetLamps fills them during the frame.
    if (EnsureWetLamps())
        appMemzero(WetLampMapped + FrameIndex * RdxMath::WetLampBytes, 16);
    // The shadow maps' casters and lights are this frame's only - RenderDevice.cpp hands them over before EndFrame.
    if (bShadows)
        Shadow.BeginFrame((INT)FrameIndex);
    // The moving bodies and their buffer likewise.
    MotionBodyCount = 0;
    MotionVbUsed    = 0;
    bInFrame = 1;
}

// G-buffer, first step (docs/terv-latvany-roadmap.md 1.4): instead of the picture, the scene's depth or the normals
// computed from it - only when asked for (RequestDepthView). The depth buffer leaves its depth-writing state for this
// draw and returns to it, which BeginFrame's clear expects. False when off or not ready: the picture is drawn then.
UBOOL FBackend::DrawDepthView(ID3D12GraphicsCommandList* Cmd)
{
    if (!DepthViewMode || !DepthSrvHeap || !DepthBuffer)
        return 0;
    if (DepthViewReady == 0)
    {
        DepthViewReady = Final.InitDepth(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("G-buffer view (%s): %s"), (DepthViewMode == 1) ? TEXT("depth") : TEXT("normal"),
               (DepthViewReady > 0) ? TEXT("up") : TEXT("FAILED - the normal picture stays"));
    }
    if (DepthViewReady < 0)
        return 0;

    // The shadow factor (RequestShadowQuality), or with mode 5 their share map: already readable, the output's size or half
    // of it.
    if (DepthViewMode == 3 || DepthViewMode == 5)
    {
        if (!ShadowColor || !ShadowSrvHeap)
            return 0;
        D3D12_GPU_DESCRIPTOR_HANDLE F = ShadowSrvHeap->GetGPUDescriptorHandleForHeapStart();
        F.ptr += SHADOW_FACTOR_SRV * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Final.DrawDepth(Cmd, ShadowSrvHeap.Get(), F, 3, ShadowFactorDivisor(), ViewTanX, ViewTanY);
        return 1;
    }
    // Mode 7: the see-through water surface's own factor (WaterFactor), as mode 3 shows the world's - white where the
    // water's surface is lit, or where there is no water.
    if (DepthViewMode == 7)
    {
        if (!WaterFactor || !ShadowSrvHeap)
            return 0;
        D3D12_GPU_DESCRIPTOR_HANDLE F = ShadowSrvHeap->GetGPUDescriptorHandleForHeapStart();
        F.ptr += (UINT64)(SHADOW_WATER_RUN + SHADOW_WATER_OWN_AT) *
                 Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Final.DrawDepth(Cmd, ShadowSrvHeap.Get(), F, 3, Max(1, (Width + WaterFactorW - 1) / Max(WaterFactorW, 1)),
                        ViewTanX, ViewTanY);
        return 1;
    }
    // Mode 4: the light each surface receives (the geometry pass's second target) - out of its render target state for
    // this draw and back.
    if (DepthViewMode == 4)
    {
        if (!LightTarget || !ShadowSrvHeap)
            return 0;
        D3D12_GPU_DESCRIPTOR_HANDLE L = ShadowSrvHeap->GetGPUDescriptorHandleForHeapStart();
        L.ptr += 3 * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_RESOURCE_BARRIER LB;
        appMemzero(&LB, sizeof(LB));
        LB.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        LB.Transition.pResource   = LightTarget.Get();
        LB.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        LB.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        LB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Cmd->ResourceBarrier(1, &LB);
        Final.DrawDepth(Cmd, ShadowSrvHeap.Get(), L, 4, SceneScale, ViewTanX, ViewTanY);
        LB.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        LB.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Cmd->ResourceBarrier(1, &LB);
        return 1;
    }

    // Mode 6: the motion vectors as a picture (RequestMotionVectors) - already readable, the scene's size, so the view
    // samples it by the ratio of the two sizes. Eight pixels of movement is full blue.
    if (DepthViewMode == 6)
    {
        if (!MotionTarget || !MotionSrvHeap)
            return 0;
        if (MotionViewReady == 0)
        {
            MotionViewReady = Final.InitMotionView(Device.Get()) ? 1 : -1;
            RdxLog(TEXT("motion vector view: the pipeline %s"),
                   (MotionViewReady > 0) ? TEXT("up") : TEXT("FAILED - the normal picture stays"));
        }
        if (MotionViewReady < 0)
            return 0;
        const FLOAT SX = (Width  > 0) ? (FLOAT)MotionW / (FLOAT)Width: 1.0f;
        const FLOAT SY = (Height > 0) ? (FLOAT)MotionH / (FLOAT)Height : 1.0f;
        Final.DrawMotionView(Cmd, MotionSrvHeap.Get(), MotionSrvHeap->GetGPUDescriptorHandleForHeapStart(), 8.0f, SX, SY);
        return 1;
    }

    // The world's depth (WorldDepthIndex).
    const INT       Shown = WorldDepthIndex();
    ID3D12Resource* Res   = (Shown == 1 && DepthBuffer2) ? DepthBuffer2.Get() : DepthBuffer.Get();
    D3D12_GPU_DESCRIPTOR_HANDLE Srv = DepthSrvHeap->GetGPUDescriptorHandleForHeapStart();
    if (Res == DepthBuffer2.Get())
        Srv.ptr += Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Res;
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Cmd->ResourceBarrier(1, &B);

    Final.DrawDepth(Cmd, DepthSrvHeap.Get(), Srv, DepthViewMode, SceneScale, ViewTanX, ViewTanY);

    B.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    Cmd->ResourceBarrier(1, &B);
    return 1;
}

// The shadows' share of the picture (GetShadowMetric), measured rather than guessed. The factor target (R8, 1.0 = lit,
// 0.0 = fully shadowed) is copied into a readback buffer every couple of seconds; the average is taken a few frames
// later, once that frame's fence has passed, so the GPU is never waited on. the number is on screen all
// the time, and the compensation will be driven by it.
//
// Why the average of the FACTOR and not the brightness of the finished picture: this measures what the shadows took,
// not how dark the scene is. A room that is dark because nothing lights it reads 1.0 - so no compensation starts there,
// which is exactly the edge case (a room with no light of its own, three lights next door).
//
// And WHERE it is taken matters as much as what: the caller copies the factor before the ambient occlusion joins it
// (FBackend::ApplyShadows), so the number is the shadows' share alone. Taking it afterwards made the occlusion spend
// the shadows' budget - measured, see the note at that call.
void FBackend::SampleShadowMetric(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES Rest)
{
    // At zero strength nothing is shadowed, so the factor target is all ones and the sample would only say so at the
    // price of the copy. The screen shows zeroes from the switch itself while the shadows are off.
    if (!Device || !Cmd || !bShadows || !ShadowColor || MetricPending >= 0 || ShadowStrength <= 0.0f)
        return;
    const DOUBLE Now = appSeconds();
    if (Now < MetricNextAt)
        return;

    const D3D12_RESOURCE_DESC Desc = ShadowColor->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout;
    UINT   Rows = 0;
    UINT64 RowBytes = 0, Total = 0;
    Device->GetCopyableFootprints(&Desc, 0, 1, 0, &Layout, &Rows, &RowBytes, &Total);
    if (Total == 0)
        return;
    if (!MetricReadback || MetricBytes < Total)
    {
        MetricReadback.Release();
        D3D12_HEAP_PROPERTIES Heap;
        appMemzero(&Heap, sizeof(Heap));
        Heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC R;
        appMemzero(&R, sizeof(R));
        R.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        R.Width            = Total;
        R.Height           = 1;
        R.DepthOrArraySize = 1;
        R.MipLevels        = 1;
        R.SampleDesc.Count = 1;
        R.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &R, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, __uuidof(ID3D12Resource), MetricReadback.VoidSlot()),
                   TEXT("CreateCommittedResource(shadow metric)")))
        {
            MetricNextAt = Now + 30.0;     // do not try again every frame
            return;
        }
        MetricBytes = Total;
    }
    MetricW     = Layout.Footprint.Width;
    MetricH     = Layout.Footprint.Height;
    MetricPitch = Layout.Footprint.RowPitch;

    // The light target beside it - the weight. It is scene-sized (with supersampling larger than the factor target) and
    // R16_FLOAT, and unlike the factor it rests as a RENDER TARGET, so its barrier runs the other way.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT LightLayout;
    UINT   LightRows = 0;
    UINT64 LightRowBytes = 0, LightTotal = 0;
    // Only when asked for (RequestShadowMetricLit): this copy is 66 MB against the factor's 8.3 MB.
    // TComRef has operator bool but no comparison with nullptr - see Pch.h.
    const UBOOL bLight = (bMetricLit && LightTarget) ? 1 : 0;
    if (bLight)
    {
        const D3D12_RESOURCE_DESC LDesc = LightTarget->GetDesc();
        Device->GetCopyableFootprints(&LDesc, 0, 1, 0, &LightLayout, &LightRows, &LightRowBytes, &LightTotal);
        if (LightTotal > 0 && (!MetricLight || MetricLightBytes < LightTotal))
        {
            MetricLight.Release();
            D3D12_HEAP_PROPERTIES LHeap;
            appMemzero(&LHeap, sizeof(LHeap));
            LHeap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC LR;
            appMemzero(&LR, sizeof(LR));
            LR.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            LR.Width            = LightTotal;
            LR.Height           = 1;
            LR.DepthOrArraySize = 1;
            LR.MipLevels        = 1;
            LR.SampleDesc.Count = 1;
            LR.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (Check(Device->CreateCommittedResource(&LHeap, D3D12_HEAP_FLAG_NONE, &LR, D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr, __uuidof(ID3D12Resource), MetricLight.VoidSlot()),
                      TEXT("CreateCommittedResource(shadow metric light)")))
                MetricLightBytes = LightTotal;
            else
                MetricLight.Release();
        }
    }
    if (MetricLight && LightTotal > 0)
    {
        MetricLightW     = LightLayout.Footprint.Width;
        MetricLightH     = LightLayout.Footprint.Height;
        MetricLightPitch = LightLayout.Footprint.RowPitch;

        D3D12_RESOURCE_BARRIER LB;
        appMemzero(&LB, sizeof(LB));
        LB.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        LB.Transition.pResource   = LightTarget.Get();
        LB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        LB.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        LB.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        Cmd->ResourceBarrier(1, &LB);

        D3D12_TEXTURE_COPY_LOCATION LSrc, LDst;
        appMemzero(&LSrc, sizeof(LSrc));
        appMemzero(&LDst, sizeof(LDst));
        LSrc.pResource        = LightTarget.Get();
        LSrc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        LSrc.SubresourceIndex = 0;
        LDst.pResource       = MetricLight.Get();
        LDst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        LDst.PlacedFootprint = LightLayout;
        Cmd->CopyTextureRegion(&LDst, 0, 0, 0, &LSrc, nullptr);

        LB.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        LB.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Cmd->ResourceBarrier(1, &LB);
    }
    else
    {
        MetricLightW = MetricLightH = MetricLightPitch = 0;
    }

    // For the copy the factor target goes to COPY_SOURCE and straight back to wherever the caller had it.
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = ShadowColor.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = Rest;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Cmd->ResourceBarrier(1, &B);

    D3D12_TEXTURE_COPY_LOCATION Src, Dst;
    appMemzero(&Src, sizeof(Src));
    appMemzero(&Dst, sizeof(Dst));
    Src.pResource        = ShadowColor.Get();
    Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Src.SubresourceIndex = 0;
    Dst.pResource       = MetricReadback.Get();
    Dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    Dst.PlacedFootprint = Layout;
    Cmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);

    B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    B.Transition.StateAfter  = Rest;
    Cmd->ResourceBarrier(1, &B);

    MetricPending = (INT)FrameIndex;
    MetricFence   = FenceValues[FrameIndex];
    // What the shadows were drawn at for THIS copy: the compensation divides by it, and by the time the sample comes
    // back the strength it asked for may already be another one (see GetShadowMetricStrength).
    MetricPendingStrength = ShadowStrength;
}

// The average, once the frame that recorded the copy is done on the GPU. Never waits: if the fence has not passed yet,
// it simply comes back next frame. Every fourth pixel in each direction is enough for an average and keeps the CPU cost
// to a fraction of a millisecond even at 4K.
void FBackend::CollectShadowMetric()
{
    if (MetricPending < 0 || !MetricReadback || !Fence)
        return;
    if (Fence->GetCompletedValue() < MetricFence)
        return;

    BYTE*       Data = nullptr;
    D3D12_RANGE All  = { 0, (SIZE_T)MetricBytes };
    if (SUCCEEDED(MetricReadback->Map(0, &All, (void**)&Data)) && Data)
    {
        // The light target too, when its copy came with this sample: the weight for the light-weighted number.
        BYTE*       Light    = nullptr;
        D3D12_RANGE LightAll = { 0, (SIZE_T)MetricLightBytes };
        const UBOOL bLight   = MetricLight && MetricLightW > 0 && MetricLightH > 0 && MetricW > 0 && MetricH > 0 &&
                               SUCCEEDED(MetricLight->Map(0, &LightAll, (void**)&Light)) && Light != nullptr;
        DOUBLE SumLitFactor = 0.0, SumLit = 0.0;

        UINT64 Sum = 0, Count = 0;
        for (UINT y = 0; y < MetricH; y += 4)
        {
            const BYTE* Row = Data + (UINT64)y * MetricPitch;
            // The light target is scene-sized: with supersampling it is larger, so the row is picked by proportion.
            const BYTE* LightRow = bLight
                ? Light + (UINT64)((UINT64)y * MetricLightH / MetricH) * MetricLightPitch
                : nullptr;
            for (UINT x = 0; x < MetricW; x += 4)
            {
                Sum += Row[x];
                Count++;
                if (LightRow)
                {
                    // R16_FLOAT: the half's bits assembled into a float. Subnormals and infinities are not worth the
                    // branch here - a weight of zero or a huge one changes nothing about the ratio being measured.
                    const UINT Lx = (UINT)((UINT64)x * MetricLightW / MetricW);
                    UINT16 H = 0;
                    appMemcpy(&H, LightRow + (SIZE_T)Lx * 2, sizeof(H));
                    const UINT Bits = ((UINT)(H & 0x8000u) << 16) | ((((UINT)(H & 0x7C00u)) + 0x1C000u) << 13) |
                                      ((UINT)(H & 0x03FFu) << 13);
                    FLOAT L = 0.0f;
                    appMemcpy(&L, &Bits, sizeof(L));
                    if (L > 0.0f && L < 1.0e6f)
                    {
                        SumLit        += L;
                        SumLitFactor  += L * ((DOUBLE)Row[x] / 255.0);
                    }
                }
            }
        }
        if (bLight)
        {
            D3D12_RANGE LightNone = { 0, 0 };
            MetricLight->Unmap(0, &LightNone);
            if (SumLit > 0.0)
                ShadowMetricLit = (FLOAT)(SumLitFactor / SumLit);
        }
        // The lowest and highest sample beside the average. A scene whose picture is 24% darker with the shadows on
        // can still average ~1.0, and then the caption reads -0%: the range is what says whether the average means
        // anything. The log gets what the buffer really holds, not what it ought to hold.
        BYTE Lo = 255, Hi = 0;
        for (UINT y = 0; y < MetricH; y += 16)
        {
            const BYTE* Row = Data + (UINT64)y * MetricPitch;
            for (UINT x = 0; x < MetricW; x += 16)
            {
                if (Row[x] < Lo) Lo = Row[x];
                if (Row[x] > Hi) Hi = Row[x];
            }
        }
        D3D12_RANGE None = { 0, 0 };
        MetricReadback->Unmap(0, &None);
        if (Count > 0)
        {
            ShadowMetric         = (FLOAT)((DOUBLE)Sum / (DOUBLE)Count / 255.0);
            MetricSampleStrength = MetricPendingStrength;
        }
        // Enough lines to reach the loaded scene: of a run's first dozen samples only the last one or two belong to
        // it - the rest are the main menu and the loading map, identical between two different saves. The scene's own
        // number moves from frame to frame as well, so one sample decides nothing.
        // The boost actually applied goes into the same line, because the metric alone cannot show it: the boost does
        // not touch the light target, so the metric reads the same at a ceiling of 0 and of 50.
        // Once a second, so this is ten minutes of it - enough for a measuring session that steps through several
        // settings and waits for the controller to settle on each. It was 120, and a three-state measurement ran out
        // of lines in the middle of the third one.
        static INT Told = 0;
        if (Told++ < 600)
            RdxLog(TEXT("shadow metric: factor %ix%i, light %ix%i; %I64u samples; surface ratio %.4f (lowest %i, ")
                   TEXT("highest %i), light-weighted %.4f; shadow strength when measured %.3f, now %.3f; the lights' ")
                   TEXT("lift is now %.3fx"),
                   (INT)MetricW, (INT)MetricH, (INT)MetricLightW, (INT)MetricLightH, Count, ShadowMetric,
                   (INT)Lo, (INT)Hi, ShadowMetricLit, MetricSampleStrength, ShadowStrength, Solid.GetLightBoost());
    }
    MetricPending = -1;
    MetricNextAt  = appSeconds() + 1.0;     // it may be slow; once a second is plenty
}

// Several lines, split at '\n' - at most RdxMath::CaptionLines of them, each at most RdxMath::CaptionMaxChars long, which
// is the root signature's limit and so a limit on THE LINE, not on the caption. Every line is then padded with spaces to
// the longest: the boxes end up the same width and the same centre, so a readout of four lines stacks into a block
// instead of a ragged column.
// The text into one block of lines, padded to a common width so the lines share a centre. Shared by the caption and
// the notice, which differ only in where they are drawn and in what overwrites them.
static void FillTextBlock(char (*Block)[RdxMath::CaptionLineChars + 1], INT MaxLines, const char* Text)
{
    for (INT n = 0; n < MaxLines; n++)
        Block[n][0] = 0;
    INT Line = 0, i = 0, Longest = 0;
    for (const char* P = Text; P && *P && Line < MaxLines; P++)
    {
        if (*P == '\n')
        {
            Block[Line][i] = 0;
            if (i > Longest)
                Longest = i;
            Line++;
            i = 0;
            continue;
        }
        if (i < RdxMath::CaptionLineChars)
            Block[Line][i++] = *P;
    }
    INT Used = Line;
    if (Line < MaxLines)
    {
        Block[Line][i] = 0;
        if (i > Longest)
            Longest = i;
        Used = Line + 1;
    }
    for (INT n = 0; n < Used; n++)
    {
        INT L = 0;
        while (Block[n][L])
            L++;
        while (L < Longest)
            Block[n][L++] = ' ';
        Block[n][L] = 0;
    }
}

void FBackend::ShowCaption(const char* Text, FLOAT Seconds)
{
    FillTextBlock(CaptionText, RdxMath::CaptionLines, Text);
    CaptionUntil = appSeconds() + Seconds;
}

void FBackend::ShowMenuPanel(const char* Text, FLOAT Seconds)
{
    FillTextBlock(MenuText, RdxMath::CaptionLines, Text);
    MenuUntil = appSeconds() + Seconds;
}

// Both come from our own settings file, and the preset can override them (Vanilla runs with neither). Logged when
// something changes, because "why is it capped at 57" is a question the log should answer by itself.
void FBackend::SetFrameLimit(INT Setting, UBOOL bVsyncOn)
{
    const INT   WasTarget = FpsTarget;
    const UBOOL bWasSync  = bVsync;
    FpsSetting = Setting;
    bVsync     = bVsyncOn ? 1 : 0;
    FpsTarget  = RdxMath::FpsLimitTarget(FpsSetting, RefreshHz, (RdxMath::EVrr)VrrState);
    NextFrameAt = 0.0;
    if (FpsTarget != WasTarget || bVsync != bWasSync)
        RdxLog(TEXT("frame rate: limit %s, vsync %s (the screen refreshes %i times a second, variable refresh %s)"),
               (FpsTarget > 0) ? *FString::Printf(TEXT("%i"), FpsTarget) : TEXT("off"),
               bVsync ? TEXT("on") : TEXT("off"), RefreshHz,
               (VrrState == RdxMath::Vrr_On) ? TEXT("on") : ((VrrState == RdxMath::Vrr_Off) ? TEXT("off") : TEXT("not known")));
}

// What the graphics driver says about the screen's variable refresh (RdxAskVrr). Taken again with every screen
// change, because the answer is the screen's, not the machine's.
void FBackend::SetVrrState(INT State)
{
    if (State == VrrState)
        return;
    VrrState = State;
    const INT WasTarget = FpsTarget;
    FpsTarget = RdxMath::FpsLimitTarget(FpsSetting, RefreshHz, (RdxMath::EVrr)VrrState);
    NextFrameAt = 0.0;
    if (FpsTarget != WasTarget)
        RdxLog(TEXT("frame rate: variable refresh %s - the limit is now %s"),
               (VrrState == RdxMath::Vrr_On) ? TEXT("on") : ((VrrState == RdxMath::Vrr_Off) ? TEXT("off")
                                                                                           : TEXT("not known")),
               (FpsTarget > 0) ? *FString::Printf(TEXT("%i"), FpsTarget) : TEXT("off"));
}

// The window has a new screen under it (or this is the first one): the refresh rate is that screen's, so an Auto
// limit follows the window from monitor to monitor.
void FBackend::SetDisplayRefresh(INT Hz)
{
    if (Hz == RefreshHz)
        return;
    RefreshHz = Hz;
    const INT WasTarget = FpsTarget;
    FpsTarget = RdxMath::FpsLimitTarget(FpsSetting, RefreshHz, (RdxMath::EVrr)VrrState);
    NextFrameAt = 0.0;
    if (FpsTarget != WasTarget)
        RdxLog(TEXT("frame rate: the screen refreshes %i times a second - the limit is now %s"), RefreshHz,
               (FpsTarget > 0) ? *FString::Printf(TEXT("%i"), FpsTarget) : TEXT("off"));
}

// Waiting out what is left of a frame's slot. Sleep hands the machine back to everything else but is only good to
// about a millisecond and a half, so the last stretch is spun - and even that yields, rather than burning a core.
static void WaitUntilSeconds(DOUBLE Until)
{
    for (;;)
    {
        const DOUBLE Left = Until - appSeconds();
        if (Left <= 0.0)
            return;
        if (Left > 0.002)
            ::Sleep((DWORD)((Left - 0.002) * 1000.0));
        else
            ::SwitchToThread();
    }
}

void FBackend::PaceThroughHost()
{
    if (!bPaceHostWanted || !bPaceHostFrame || bFgMode || PaceFirst <= 0.0 || PaceWake <= 0.0 || !Fence)
        return;
    const DOUBLE Took = PaceFirst - PaceWake;
    if (Took < 0.25)
    {
        PaceCpu[PaceCpuCount % PACE_KEPT] = Took;
        PaceCpuCount++;
    }
    if (PaceCpuCount < PACE_KEPT || PaceGpuCount < PACE_KEPT)
        return;
    if (!PaceEvent)
        PaceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!PaceEvent)
        return;
    // The frame just submitted begins on the GPU no earlier than its first submission, and no earlier than the frame
    // before's end - waited for where it has not come yet. Unknown, it is the earlier of the two: the wait ends early.
    DOUBLE Begins  = PaceFirst;
    UBOOL  bWaited = 0;
    if (PaceFencePrev && Fence->GetCompletedValue() < PaceFencePrev)
    {
        if (FAILED(Fence->SetEventOnCompletion(PaceFencePrev, PaceEvent)) ||
            ::WaitForSingleObject(PaceEvent, 100) != WAIT_OBJECT_0)
            return;
        bWaited = 1;
    }
    // The frame before's end is its own last timestamp (point 5), still unread in its slot, brought onto this clock by
    // the queue's calibration: it may have ended well before this got here - waited for in MoveToNextFrame, or behind
    // a Present that blocked -, and where it was waited for here, the thread woke a little after it.
    UBOOL bEnd = 0;
    if (PaceSlotPrev >= 0 && PaceSlotPrev < BACKEND_FRAME_COUNT && TimeMapped && TimeLive[PaceSlotPrev] &&
        ((TimePlaced[PaceSlotPrev] >> 5) & 1))
    {
        UINT64        GpuFreq = 0, GpuNow = 0, CpuNow = 0;
        LARGE_INTEGER QpcFreq, QpcNow;
        if (SUCCEEDED(Queue->GetTimestampFrequency(&GpuFreq)) && GpuFreq &&
            SUCCEEDED(Queue->GetClockCalibration(&GpuNow, &CpuNow)) && ::QueryPerformanceFrequency(&QpcFreq) &&
            ::QueryPerformanceCounter(&QpcNow) && QpcFreq.QuadPart > 0)
        {
            const DOUBLE AppNow = appSeconds();
            const UINT64 End    = TimeMapped[PaceSlotPrev * GPU_TIME_POINTS + 5];
            const DOUBLE EndQpc = (DOUBLE)CpuNow + (DOUBLE)(INT64)(End - GpuNow) * (DOUBLE)QpcFreq.QuadPart / (DOUBLE)GpuFreq;
            const DOUBLE EndApp = AppNow + (EndQpc - (DOUBLE)QpcNow.QuadPart) / (DOUBLE)QpcFreq.QuadPart;
            if (EndApp <= AppNow && EndApp > AppNow - 0.5)
            {
                Begins = Max(Begins, EndApp);
                bEnd   = 1;
            }
        }
    }
    if (bWaited && !bEnd)
        Begins = Max(Begins, appSeconds());
    DOUBLE Gpu = PaceGpu[0], Cpu = PaceCpu[0];
    for (INT i = 1; i < PACE_KEPT; i++)
    {
        Gpu = Min(Gpu, PaceGpu[i]);
        Cpu = Max(Cpu, PaceCpu[i]);
    }
    const DOUBLE Margin = 0.0015;
    const DOUBLE Wake   = Begins + Gpu - Cpu - Margin;
    const DOUBLE Now    = appSeconds();
    PaceFrames++;
    if (Wake > Now && Wake - Now < 0.100)
    {
        WaitUntilSeconds(Wake);
        PaceWaited += Wake - Now;
        PaceWaits++;
    }
    if (PaceFrames >= 600)
    {
        RdxLog(TEXT("anti-lag (the host's path): %i frames paced, %i of them waited, %.2f ms a frame on average; the GPU's ")
               TEXT("frame %.2f ms at least, the CPU to its first submission %.2f ms at most (the last %i)"),
               PaceFrames, PaceWaits, PaceWaited * 1000.0 / PaceFrames, Gpu * 1000.0, Cpu * 1000.0, (INT)PACE_KEPT);
        PaceFrames = PaceWaits = 0;
        PaceWaited = 0.0;
    }
}

void FBackend::ShowNotice(const char* Text, FLOAT Seconds)
{
    FillTextBlock(NoticeText, NoticeLines, Text);
    NoticeUntil = appSeconds() + Seconds;
}

// The caption (ShowCaption), onto the back buffer EndFrame has bound: in the top right-hand corner, clear of the game's
// column of augmentation icons (RdxMath::CaptionRightGap). The notice centred. The scissor is the text's box - its
// shader colours every pixel it runs for.
void FBackend::DrawCaption(ID3D12GraphicsCommandList* Cmd)
{
    const DOUBLE Now = appSeconds();
    if (CaptionText[0][0] && Now >= CaptionUntil)
        CaptionText[0][0] = 0;
    if (MenuText[0][0] && Now >= MenuUntil)
        MenuText[0][0] = 0;
    if (NoticeText[0][0] && Now >= NoticeUntil)
        NoticeText[0][0] = 0;
    if (!CaptionText[0][0] && !MenuText[0][0] && !NoticeText[0][0])
        return;
    if (CaptionReady == 0)
    {
        CaptionReady = Final.InitCaption(Device.Get()) ? 1 : -1;
        RdxLog(TEXT("caption: the pipeline %s"), (CaptionReady > 0) ? TEXT("up") : TEXT("FAILED - skipped"));
    }
    if (CaptionReady < 0)
        return;

    const INT Scale = RdxMath::CaptionScale(Width, Height);
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)Width, (FLOAT)Height, 0.0f, 1.0f };
    Cmd->RSSetViewports(1, &Vp);
    // The caption at the top right, clear of the game's column of augmentation icons; the menu at the top left, clear
    // of the health icon, so the two stand side by side and neither hides the other. The notice a third of the height
    // down, in the middle, where it is in the way on purpose.
    const INT Top = (INT)((FLOAT)Height * RdxMath::CaptionTopGap);
    const INT Gap     = Max(1, (INT)((FLOAT)Height * RdxMath::CaptionRightGap));
    const INT LeftGap = Max(1, (INT)((FLOAT)Height * RdxMath::CaptionLeftGap));
    // Each block at the scale it fits the picture at (RdxMath::CaptionScaleToFit).
    INT CaptionCount = 0, MenuCount = 0;
    while (CaptionCount < RdxMath::CaptionLines && CaptionText[CaptionCount][0])
        CaptionCount++;
    while (MenuCount < RdxMath::CaptionLines && MenuText[MenuCount][0])
        MenuCount++;
    if (CaptionText[0][0])
        DrawTextLines(Cmd, CaptionText, RdxMath::CaptionLines, Top,
                      RdxMath::CaptionScaleToFit(CaptionCount, Top, Height, Scale), Gap);
    if (MenuText[0][0])
        DrawTextLines(Cmd, MenuText, RdxMath::CaptionLines, Top,
                      RdxMath::CaptionScaleToFit(MenuCount, Top, Height, Scale), -LeftGap);
    if (NoticeText[0][0])
        DrawTextLines(Cmd, NoticeText, NoticeLines, Height / 3, Scale, 0);
}

// One block of lines with its top at TopY: a draw for every CaptionMaxChars characters of a line, each with its own
// scissor - the shader colours every pixel it runs for, so the scissors must tile the line's box without overlapping,
// or the dark box would be laid twice where they met. The lines are padded to a common width (FillTextBlock), so they
// share a centre - and, set against an edge, that edge as well. Gap: 0 centres the block, a positive one keeps it that
// many pixels clear of the RIGHT edge, a negative one that many clear of the LEFT.
void FBackend::DrawTextLines(ID3D12GraphicsCommandList* Cmd, const char (*Block)[RdxMath::CaptionLineChars + 1],
                             INT Lines, INT TopY, INT Scale, INT RightGap)
{
    const INT BoxH = RdxMath::CaptionHeight(Scale);
    for (INT n = 0; n < Lines && Block[n][0]; n++)
    {
        INT Count = 0;
        while (Block[n][Count] && Count < RdxMath::CaptionLineChars)
            Count++;
        const INT BoxW = RdxMath::CaptionWidth(Count, Scale);
        const INT X    = (RightGap > 0) ? Max(0, Width - RightGap - BoxW)
                       : ((RightGap < 0) ? Min(-RightGap, Max(0, Width - BoxW))
                                         : ((Width > BoxW) ? (Width - BoxW) / 2 : 0));
        const INT Y    = TopY + n * BoxH;
        if (Y + BoxH > Height)
            break;
        for (INT First = 0; First < Count; First += RdxMath::CaptionMaxChars)
        {
            const INT Piece = (Count - First < RdxMath::CaptionMaxChars) ? Count - First : RdxMath::CaptionMaxChars;
            UINT V[RdxMath::CaptionConstants];
            appMemzero(V, sizeof(V));
            for (INT c = 0; c < Piece; c++)
            {
                unsigned Lo = 0, Hi = 0;
                RdxMath::CaptionGlyph(Block[n][First + c], Lo, Hi);
                V[4 + c * 2] = Lo;
                V[5 + c * 2] = Hi;
            }
            // The piece's origin is where the whole line's would put its first character, less the one dot of margin
            // the shader counts before it - so the pieces join without a seam. The first piece's scissor starts at the
            // box's left edge (the margin is its own), every later one where its first character does.
            const INT Origin = X + First * 6 * Scale;
            const INT Left   = (First == 0) ? X : Origin + Scale;
            const INT Right  = (First + Piece >= Count) ? X + BoxW : Origin + Scale + Piece * 6 * Scale;
            V[0] = (UINT)Origin;
            V[1] = (UINT)Y;
            V[2] = (UINT)Scale;
            V[3] = (UINT)Piece;
            D3D12_RECT Sc = { Left, Y, (Right < Width) ? Right : Width, (Y + BoxH < Height) ? Y + BoxH : Height };
            Cmd->RSSetScissorRects(1, &Sc);
            Final.DrawCaption(Cmd, V);
        }
    }
}

// How much video memory this process is using and how much the system says it may use, in MB. Both zero when DXGI
// will not say (an adapter older than IDXGIAdapter3). Going OVER the budget is not an error: Windows lets it happen
// and starts moving resources across the PCIe bus instead, which turns a 30 ms frame into a 300 ms one. That cliff is
// invisible from inside D3D12 - nothing fails, everything just becomes slow - so the numbers have to be logged.
void FBackend::VideoMemoryMB(DOUBLE& Used, DOUBLE& Budget) const
{
    Used   = 0.0;
    Budget = 0.0;
    if (!MemoryAdapter)
        return;
    DXGI_QUERY_VIDEO_MEMORY_INFO Info;
    appMemzero(&Info, sizeof(Info));
    if (FAILED(MemoryAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)))
        return;
    Used   = (DOUBLE)Info.CurrentUsage / (1024.0 * 1024.0);
    Budget = (DOUBLE)Info.Budget       / (1024.0 * 1024.0);
}

void FBackend::EndFrame(UBOOL Present)
{
    if (!Device || !bInFrame)
        return;

    const DOUBLE E0 = appSeconds();
    // The start's clock: the first frame's own parts - the pipelines made on first use are in them.
    static UBOOL bFirstEnd = 1;
    if (bFirstEnd)
        RdxStartupMark(TEXT("first frame: the engine drew it (its end begins)"));
    // What the compiler made in the last few seconds goes to the shader cache's file (RdxCompile) - once it is quiet.
    RdxShaderCacheSave(0);
    // Frame generation's mode: the finished picture goes into a texture, not a back buffer - the pacer's queue presents
    // it (FFramePacer). Into the host's own texture where the frame generates, which it then reads; into ours otherwise.
    ID3D12Resource* FinalTex = nullptr;
    // The own lamps' maps and rows, before anything of the world is drawn: the world reads them.
    PrepareOwnLamps();
    // The flashlight's cone for the runs that take it (SetBeam): from the world view's eye along its axis, the view's
    // centre and projection in the pixels of the target the world is drawn into.
    {
        FLOAT       Block[RdxMath::BeamBlockFloats], Thin[RdxMath::BeamBlockFloats];
        const UBOOL bCone   = (bBeamOn && CloudFocal > 1.0f) ? 1 : 0;
        const FLOAT Sx      = (FLOAT)Max(SceneWidth, 1) / (FLOAT)Max(Width, 1);
        const FLOAT Sy      = (FLOAT)Max(SceneHeight, 1) / (FLOAT)Max(Height, 1);
        const FLOAT Less[3] = { BeamColour[0] * RdxMath::BeamSeeThrough, BeamColour[1] * RdxMath::BeamSeeThrough,
                                BeamColour[2] * RdxMath::BeamSeeThrough };
        RdxMath::BeamBlock(bCone ? BeamColour : nullptr, CloudScreen[0] * Sx, CloudScreen[1] * Sy, CloudFocal * Sx,
                           Block);
        RdxMath::BeamBlock(bCone ? Less : nullptr, CloudScreen[0] * Sx, CloudScreen[1] * Sy, CloudFocal * Sx, Thin);
        // The level's mirrors that give the cone back (FBeamBounce): every mirror's view of the frame that came with
        // its mirror's polygons, by the plane the mirror lies in and its number in the map. The map is emptied for
        // them here; the flush writes it at each of those views' ends, before the world's view's surfaces read it.
        // More of them than a block holds, or no map: no light off any mirror for the frame.
        FLOAT Mirrors[FSolidPass::BEAM_MIRRORS * 8];
        INT   MirrorCount = 0, With = 0;
        for (INT i = 0; i < MirrorEndCount; i++)
            With += (MirrorEnds[i].TriCount > 0) ? 1 : 0;
        // And the frame's panes of glass (NoteGlassPane), drawn into the map with the rays before the world: after
        // the mirrors, where the block has room for them all.
        const UBOOL bPanes = (bCone && PaneCount > 0 && Solid.CanKeepPanes() && IsRtMirrorRunning()) ? 1 : 0;
        if (bCone && (With > 0 || bPanes) && !MsaaOn() && bBounceNone)
        {
            if (BounceReady == 0)
                BounceReady = Bounce.Init(Device.Get()) ? 1 : -1;
            if (BounceReady > 0 && Bounce.EnsureTarget(Device.Get(), SceneWidth, SceneHeight))
            {
                if (With > 0 && With <= (INT)FSolidPass::BEAM_MIRRORS && bDepthReadable)
                {
                    for (INT i = 0; i < MirrorEndCount; i++)
                    {
                        const FMirrorEnd& E = MirrorEnds[i];
                        if (E.TriCount <= 0)
                            continue;
                        FLOAT* const M = Mirrors + MirrorCount++ * 8;
                        for (INT k = 0; k < 4; k++)
                            M[k] = E.ViewPlane[k];
                        M[4] = FBeamBounce::MirrorShare;
                        M[5] = (FLOAT)(i + 1);
                        M[6] = M[7] = 0.0f;
                    }
                    bBounceNow = 1;
                }
                if (bPanes && MirrorCount + PaneCount <= (INT)FSolidPass::BEAM_MIRRORS)
                {
                    for (INT i = 0; i < PaneCount; i++)
                    {
                        FLOAT* const M = Mirrors + MirrorCount++ * 8;
                        for (INT k = 0; k < 4; k++)
                            M[k] = PanePlanes[i * 4 + k];
                        M[4] = bPaneAll ? 1.0f : FBeamBounce::GlassShare;
                        M[5] = (FLOAT)(PANE_FIRST_NUMBER + i);
                        M[6] = bPaneAll ? 0.0f : 1.0f;      // glass: by how the light meets it
                        M[7] = 0.0f;
                    }
                    bPaneNow   = 1;
                    PaneRimCos = RdxMath::BeamMapRimCos(CloudFocal * Sx);
                }
                if (With > (INT)FSolidPass::BEAM_MIRRORS || (bPanes && !bPaneNow))
                {
                    static INT Said = 0;
                    if ((Said++ % 600) == 0)
                        RdxLog(TEXT("flashlight off mirrors: %i mirrors' views and %i planes of glass in a frame, more "
                                    "than the %i a frame's flashlight is given back by - the mirrors %s, the glass %s"),
                               With, PaneCount, (INT)FSolidPass::BEAM_MIRRORS,
                               bBounceNow ? TEXT("give it back") : TEXT("do not"),
                               bPaneNow ? TEXT("gives it back") : TEXT("does not"));
                }
                if (bBounceNow || bPaneNow)
                {
                    if (BounceMade != Bounce.TargetMade())
                    {
                        const INT View = Textures.GetReservedIndex(BounceMapView);
                        Device->CreateShaderResourceView(Bounce.Target(), nullptr, Textures.GetCpuHandle(View));
                        BounceMade = Bounce.TargetMade();
                    }
                    Bounce.Begin(CommandList.Get(), Device.Get(), (INT)FrameIndex);
                }
            }
        }
        // The own lamps' views: the world's, and each mirror's of the level that ended with its place in the level -
        // where each stands, its centre and projection, and the first lamp its surfaces take: the world's view takes
        // the flashlight as its cone, not as a lamp.
        FLOAT Views[FSolidPass::BEAM_VIEWS * FSolidPass::BEAM_VIEW_FLOATS];
        appMemzero(Views, sizeof(Views));
        const UBOOL bOwn = (OwnRowsLamps > 0 && CloudFocal > 1.0f) ? 1 : 0;
        if (bOwn)
        {
            auto PutView = [&](INT V, const FLOAT* C, FLOAT First)
            {
                FLOAT* const O = Views + V * FSolidPass::BEAM_VIEW_FLOATS;
                for (INT k = 0; k < 4; k++)
                {
                    O[k * 4 + 0] = C[k * 3 + 0];
                    O[k * 4 + 1] = C[k * 3 + 1];
                    O[k * 4 + 2] = C[k * 3 + 2];
                }
                O[3]  = 1.0f;
                O[7]  = First;
                O[11] = 1.0f;
                O[16] = CloudScreen[0] * Sx;
                O[17] = CloudScreen[1] * Sy;
                O[18] = CloudFocal * Sx;
            };
            if (bOwnWorld)
                PutView(0, OwnWorld, OwnLamps[0].bEye ? 1.0f : 0.0f);
            for (INT m = 0; m < MirrorEndCount && m < (INT)FSolidPass::BEAM_MIRRORS; m++)
                if (MirrorEnds[m].bOwnCoords)
                {
                    PutView(1 + m, MirrorEnds[m].OwnCoords, 0.0f);
                    OwnStatMirrors++;
                }
        }
        Solid.SetBeam((INT)FrameIndex, bCone ? Block : nullptr, bCone ? Thin : nullptr, Mirrors, MirrorCount,
                      bOwn ? OwnRowsNow : nullptr, Views);
        appMemcpy(BeamBlockNow, Block, sizeof(BeamBlockNow));
        // The wet floors' blocks (SolidPass.hlsl's WetConsts): where the world's view stands in the level, its centre in
        // the target's pixels with this frame's jitter in it - the world is drawn that far off - its projection and the
        // measuring view asked for, then the look's numbers; one for each kind - stone, metal, wood. None without the
        // view.
        if (bCamSet && CloudFocal > 1.0f)
        {
            FLOAT Wet[FSolidPass::WET_KINDS][RdxMath::WetBlockFloats];
            for (INT k = 0; k < FSolidPass::WET_KINDS; k++)
            {
                FLOAT* const W = Wet[k];
                for (INT i = 0; i < 3; i++)
                {
                    W[i]                = CamEye[i];
                    W[4 + i]            = CamAxes[i];
                    W[8 + i]            = CamAxes[3 + i];
                    W[12 + i]           = CamAxes[6 + i];
                }
                W[3]  = (FLOAT)(k + 1);
                W[7]  = W[11] = W[15] = 0.0f;
                W[16] = CloudScreen[0] * Sx + JitterPx[0];
                W[17] = CloudScreen[1] * Sy + JitterPx[1];
                W[18] = CloudFocal * Sx;
                W[19] = (FLOAT)WetShowView;
                W[20] = RdxMath::WetPatchUnits;
                W[21] = RdxMath::WetLevelBase;
                W[22] = RdxMath::WetLevelSpread;
                W[23] = RdxMath::WetStoneDarkest;
                W[24] = RdxMath::WetFloodLeast;
                W[25] = RdxMath::WetFloodMost;
                W[26] = RdxMath::WetFloodSpread;
                W[27] = RdxMath::WetPuddleEdge;
                // The rain's cover over the level (SetWetCover): its grid's corner, and one over its span along X and Y; none
                // until a level's is made.
                for (INT c = 0; c < 4; c++)
                    W[28 + c] = WetCoverNow[c];
            }
            Solid.SetWetBlocks((INT)FrameIndex, &Wet[0][0]);
            appMemcpy(WetBlockNow, Wet[0], sizeof(WetBlockNow));
            bWetBlockNow = 1;
        }
        else
        {
            Solid.SetWetBlocks((INT)FrameIndex, nullptr);
            bWetBlockNow = 0;
        }
        // What the frame's lamps give the eye and the lens: the weapon lamps' glares and ghosts, and the
        // flashlight's images in its mirrors and panes.
        MakeLampFlares(bCone ? Block : nullptr);
        OwnStatGlares += LampGlareCount;
        OwnStatGhosts += BloomLampCount;
        OwnStatImages += LampImageCount;
        if (OwnStatFrames >= 300)
        {
            RdxLog(TEXT("own lamps: %i frames with them, %i at the most in one, %i of those frames without their maps; "
                        "%i mirrors' views took them; %i glares, %i lamps for the lens's ghosts, %i of the "
                        "flashlight's images in mirrors and glass; their beams in the air walked in %i frames and %i "
                        "mirrors' views"),
                   OwnStatFrames, OwnStatMost, OwnStatUnmapped, OwnStatMirrors, OwnStatGlares, OwnStatGhosts,
                   OwnStatImages, OwnStatBeams, OwnStatBeamMirrors);
            OwnStatFrames = OwnStatMost = OwnStatUnmapped = OwnStatMirrors = OwnStatGlares = OwnStatGhosts =
                OwnStatImages = OwnStatBeams = OwnStatBeamMirrors = 0;
        }
    }
    // Supersampling: at the HUD's first tile the flush itself moves to the output target (SwitchToOutput).
    Solid.Flush(CommandList.Get(), FrameIndex, Textures,
                DsvHeap->GetCPUDescriptorHandleForHeapStart(),
                (HasOutputTarget() || NeedsDepthRead() || WorldEndWanted()) ? static_cast<FTargetSwitch*>(this)
                                                                            : nullptr);
    const DOUBLE E1 = appSeconds();
    if (bFirstEnd)
        RdxStartupMark(TEXT("first frame: the solid pass's flush (the shadows, the ambient occlusion)"));

    // The finished frame, HUD included, goes to the back buffer through the brightness
    // curve. Supersampling: from the output target, where the HUD went onto the box-filtered
    // scene - a frame without a HUD tile has its scene put there first.
    {
        // A fallback only: the vectors were made at the world's end (ResolveScene, SwitchToOutput), where its depth is
        // still whole. A frame without a HUD tile never reached SwitchToOutput, so nothing has resolved the
        // multisampled world yet - and the motion vectors read the depth. Here, before everything.
        ResolveMsaaWorld(CommandList.Get());
        DrawMotionVectors(CommandList.Get());
        if (HasOutputTarget() && !bOutputLive)
        {
            ResolveScene(CommandList.Get());
            // As at the HUD's first tile (SwitchToOutput): the film look belongs to the finished world, HUD or none.
            DrawFilmLook(CommandList.Get());
            LateWorldEnds++;
        }
        // 1x in a frame without a HUD tile (SwitchToOutput did not come): the world ends here, with everything that
        // was gathered for its end - a frame that left it out would have neither the game's smoke nor the walk's.
        if (!HasOutputTarget() && !bOutputLive && (bShadows || WorldEndWanted()))
        {
            EndWorldInPlace(CommandList.Get());
            LateWorldEnds++;
        }
        // Said only where it happens: in play every frame has a HUD tile.
        if (++LateWorldEndFrames >= 300)
        {
            if (LateWorldEnds > 0)
                RdxLog(TEXT("world's end: at the frame's end in %i of the last %i frames - they had no HUD tile"),
                       LateWorldEnds, LateWorldEndFrames);
            LateWorldEnds = LateWorldEndFrames = 0;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE Back = RtvStart;
        Back.ptr += FrameIndex * RtvStride;
        if (bFgMode)
        {
            if (bFgFrame && Host.GetPresent())
            {
                FinalTex = Host.GetPresent();
                Back     = Host.PresentRtv();
            }
            else if (EnsureFgFinal())
            {
                bFgFrame = 0;
                FinalTex = FgFinal.Get();
                Back     = FgFinalRtvHeap->GetCPUDescriptorHandleForHeapStart();
            }
            else
                bFgFrame = 0;
        }

        // A texture of ours waits in COMMON between frames, as a back buffer waits in PRESENT: the same state.
        D3D12_RESOURCE_BARRIER B[2];
        appMemzero(B, sizeof(B));
        B[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[0].Transition.pResource   = HasOutputTarget() ? OutputColor.Get() : SceneColor.Get();
        B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        B[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B[1] = B[0];
        B[1].Transition.pResource   = FinalTex ? FinalTex : BackBuffers[FrameIndex];
        B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        CommandList->ResourceBarrier(2, B);

        CommandList->OMSetRenderTargets(1, &Back, FALSE, nullptr);
        if (HasOutputTarget())
        {
            // The output target is the back buffer's size, and the output's viewport is set again
            // here rather than trusted to what the switch left behind. On the vanilla path
            // BeginFrame's viewport already is the output's - nothing to do.
            SetOutputViewport(CommandList.Get());
            if (!DrawDepthView(CommandList.Get()))
                DrawFinal(CommandList.Get(), OutputSrvHeap.Get(),
                          OutputSrvHeap->GetGPUDescriptorHandleForHeapStart());
        }
        else
        {
            if (!DrawDepthView(CommandList.Get()))
                DrawFinal(CommandList.Get(), SceneSrvHeap.Get(),
                          SceneSrvHeap->GetGPUDescriptorHandleForHeapStart());
        }
        // A generating frame that never reached the HUD's switch has no HUD: its HUD-less picture is this one, before
        // the caption goes on.
        if (bFgFrame && !bHudLessDone && Host.GetHudLess())
        {
            D3D12_RESOURCE_BARRIER C[2];
            appMemzero(C, sizeof(C));
            for (INT i = 0; i < 2; i++)
            {
                C[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                C[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            C[0].Transition.pResource   = FinalTex;
            C[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            C[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
            C[1].Transition.pResource   = Host.GetHudLess();
            C[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            C[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            CommandList->ResourceBarrier(2, C);
            CommandList->CopyResource(Host.GetHudLess(), FinalTex);
            for (INT i = 0; i < 2; i++)
            {
                const D3D12_RESOURCE_STATES Was = C[i].Transition.StateBefore;
                C[i].Transition.StateBefore = C[i].Transition.StateAfter;
                C[i].Transition.StateAfter  = Was;
            }
            CommandList->ResourceBarrier(2, C);
            CommandList->OMSetRenderTargets(1, &Back, FALSE, nullptr);
            bHudLessDone = 1;
        }
        DrawCaption(CommandList.Get());
    }

    D3D12_RESOURCE_BARRIER Barrier;
    appMemzero(&Barrier, sizeof(Barrier));
    Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource   = FinalTex ? FinalTex : BackBuffers[FrameIndex];
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    CommandList->ResourceBarrier(1, &Barrier);
    GpuTime(CommandList.Get(), 5);
    UBOOL bTimeResolved = 0;
    if (TimeHeap && TimeMapped && FrameIndex < BACKEND_FRAME_COUNT)
    {
        // Every point of the set ended - one the frame did not reach (no shadows this frame) here at the end, and left out
        // of the sums (TimePlaced): a resolve of a query never ended gives nothing defined.
        for (INT p = 0; p < GPU_TIME_POINTS; p++)
            if (!((TimePlaced[FrameIndex] >> p) & 1))
                CommandList->EndQuery(TimeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, FrameIndex * GPU_TIME_POINTS + (UINT)p);
        CommandList->ResolveQueryData(TimeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, FrameIndex * GPU_TIME_POINTS,
                                      GPU_TIME_POINTS, TimeReadback.Get(), (UINT64)FrameIndex * GPU_TIME_POINTS * sizeof(UINT64));
        bTimeResolved = 1;
    }
    const DOUBLE E2 = appSeconds();
    if (bFirstEnd)
        RdxStartupMark(TEXT("first frame: the final passes (the upscaling, the picture's end)"));

    if (FAILED(CommandList->Close()))
    {
        bInFrame = 0;
        return;
    }
    if (bTimeResolved)
        TimeLive[FrameIndex] = 1;

    // Frame generation's mode: this part draws into a picture the pacer copies out - the last one handed to it has to be
    // out first; and with it the last generated picture, before the host is asked for the next one into the same texture.
    // The pacer copies both before its half-frame's hold, so this waits for the copies, never for the hold. (Where the
    // frame went through the host, the world's half is already running.)
    if (bFgMode && Pacer.RealReleased())
        Queue->Wait(Pacer.GetCopyFence(), Pacer.RealReleased());
    NoteFrameSubmit(0);
    ID3D12CommandList* Lists[1] = { CommandList.Get() };
    Queue->ExecuteCommandLists(1, Lists);
    const DOUBLE E3 = appSeconds();
    DOUBLE       E4 = E3;
    if (bFirstEnd)
    {
        bFirstEnd = 0;
        RdxStartupMark(TEXT("first frame: its list closed and sent"));
    }

    if (Present)
    {
        if (bOccluded)
        {
            // The window is occluded (Alt+Tab). Nothing is presented; we only ask whether presenting is possible
            // again, because a tearing-enabled Present returns an error while occluded and the game stops there.
            // The test present can fail the same way a real one can - a driver update or a hang while the window
            // was hidden - and then nothing here would ever say so: the occluded path has no other way out, so
            // the game would sit waiting to become visible again forever.
            // Frame generation's mode: the pacer's thread presents - whatever it still has goes out before this one.
            if (bFgMode)
                Pacer.Flush();
            const HRESULT Test = SwapChain->Present(0, DXGI_PRESENT_TEST);
            if (Test == S_OK)
            {
                bOccluded = 0;
                RdxLog(TEXT("the window is visible again, presentation resumed"));
            }
            else if (Test == DXGI_ERROR_DEVICE_REMOVED || Test == DXGI_ERROR_DEVICE_RESET)
            {
                NoteDeviceLost(Test);
            }
        }
        else
        {
#if RDX_TEST_DEVICE_LOSS
            // A TEST BUILD ONLY, and never built by default. Takes the device away on purpose at a known frame so
            // that the handling for a lost device can be SEEN to work rather than only read.
            //
            // ID3D12Device5::RemoveDevice is made for this: it removes the device for THIS PROCESS, without
            // hanging the GPU and without touching anything else on the machine - unlike the accident that made
            // the handling necessary, which took a real device hang to produce.
            // Twice in a run: a backend built anew counts its frames from nothing, and the second loss is the test
            // of a device that has already come back once. Once for a backend: with frame generation a frame or two
            // still go out before the loss shows.
            static INT Losses = 0;
            if (bTestLoss && !bTestLossTaken && !bDeviceLost && Losses < 2 && PresentN >= RDX_TEST_DEVICE_LOSS)
            {
                bTestLossTaken = 1;
                Losses++;
                TComRef<ID3D12Device5> Dev5;
                if (SUCCEEDED(Device->QueryInterface(__uuidof(ID3D12Device5), Dev5.VoidSlot())))
                {
                    RdxLog(TEXT("TEST BUILD: taking the device away on purpose at frame %i"), (INT)PresentN);
                    Dev5->RemoveDevice();
                }
            }
#endif
            // DXGI refuses the tearing flag in its own fullscreen state (DXGI_ERROR_INVALID_CALL);
            // there an interval of 0 tears by itself.
            // With vsync the display paces us: an interval of one, and no tearing flag - DXGI refuses those two
            // together. Without it the present returns at once, and the limit below does whatever pacing there is.
            const UINT    Sync  = bVsync ? 1 : 0;
            const UINT    Flags = (!bVsync && bAllowTearing && !bExclusive) ? DXGI_PRESENT_ALLOW_TEARING : 0;
            // Frame generation's mode: the generated picture asked of the host (its queue waits for the finished
            // pictures, the pacer's thread for its result - this queue goes on), and both pictures handed to the pacer.
            UINT64 RealValue = 0;
            if (bFgMode)
            {
                if (bFgFrame)
                {
                    Host64::FFrame Fg;
                    appMemzero(&Fg, sizeof(Fg));
                    Fg.Services     = Host64::Service_FrameGen;
                    Fg.Flags        = (bFgReset ? (uint32_t)Host64::FrameFlag_Reset : 0u) |
                                      ((Colours.BackBuffer == DXGI_FORMAT_R16G16B16A16_FLOAT) ? (uint32_t)Host64::FrameFlag_ScRgb
                                                                                              : 0u);
                    Fg.RenderWidth  = (uint32_t)SceneWidth;
                    Fg.RenderHeight = (uint32_t)SceneHeight;
                    Fg.OutputWidth  = (uint32_t)Width;
                    Fg.OutputHeight = (uint32_t)Height;
                    Fg.FrameTimeMs  = FrameDeltaMs;
                    if (!Host.SubmitFrameGen(Queue.Get(), Fg))
                        bFgFrame = 0;
                }
                RealValue = FenceValues[FrameIndex];
                Queue->Signal(Fence.Get(), RealValue);
            }
            // The frame's work is submitted: Anti-Lag 2 told so before the picture goes out, as AMD asks; Reflex the
            // same, and the Present wrapped (with frame generation, the hand-over to the pacer).
            if (bAntiLagWanted && AmdAntiLagUp())
                RdxAntiLag2::MarkEndOfFrameRendering(&AntiLag);
            RdxReflexMark(RdxReflexMark_SubmitEnd);
            RdxReflexMark(RdxReflexMark_PresentStart);
            LARGE_INTEGER Q0, Q1;
            ::QueryPerformanceCounter(&Q0);
            RdxPresentWatchEnter(PresentN + 1);
            const HRESULT Hr = !bFgMode ? SwapChain->Present(Sync, Flags)
                             : FinalTex ? Pacer.Submit(SwapChain.Get(), BackBuffers, SwapBuffers,
                                                       bFgFrame ? Host.GetGenerated() : nullptr, Host.DoneFence(),
                                                       Host.DoneValue(), FinalTex, Fence.Get(), RealValue, Sync, Flags)
                                        : S_OK;
            ::QueryPerformanceCounter(&Q1);
            RdxPresentWatchLeave();
            RdxReflexMark(RdxReflexMark_PresentEnd);
            NotePresent(Q0.QuadPart, Q1.QuadPart);
            LatencyPresentAt = Q1.QuadPart;
            LatencyJob       = (bFgMode && FinalTex) ? Pacer.RealReleased() : 0;
            if (bLatencyStamps && FrameIndex < BACKEND_FRAME_COUNT)
                LatencySlotPresent[FrameIndex] = Q1.QuadPart;
            // The start's clock: the first pictures, and the report once they are out - the white window is over by then.
            static INT StartPresents = 0;
            if (StartPresents < 30)
            {
                StartPresents++;
                if (StartPresents == 1)
                    RdxStartupMark(TEXT("the first picture presented"));
                else if (StartPresents == 2)
                    RdxStartupMark(TEXT("the second picture presented"));
                else if (StartPresents == 10)
                    RdxStartupMark(TEXT("the 10th picture presented"));
                else if (StartPresents == 30)
                    RdxStartupReport(TEXT("the 30th picture presented"));
            }
            bFgLast = bFgFrame;

            // The frame rate: measured here, where one frame's present to the next is exactly one frame. The wait for
            // the limit goes AFTER the present - the picture is already on its way to the screen, so the wait costs
            // the player nothing. The slots: RdxMath::NextFrameSlot - whole periods, and a frame later than its
            // period (or a stall, a level load) goes on at once.
            {
                const DOUBLE Now = appSeconds();
                if (LastPresentAt > 0.0)
                {
                    const DOUBLE D = Now - LastPresentAt;
                    FrameSeconds = (FrameSeconds <= 0.0 || D > 0.5) ? D : (FrameSeconds * 0.9 + D * 0.1);
                }
                LastPresentAt = Now;
                if (FpsTarget > 0)
                {
                    // With frame generation the limit counts the pictures that reach the screen: two a frame.
                    const INT EngineTarget = (bFgFrame && FpsTarget > 1) ? FpsTarget / 2 : FpsTarget;
                    NextFrameAt = RdxMath::NextFrameSlot(NextFrameAt, Now, 1.0 / (DOUBLE)EngineTarget);
                    WaitUntilSeconds(NextFrameAt);
                }
                else
                    NextFrameAt = 0.0;
            }
            if (Hr == DXGI_STATUS_OCCLUDED)
            {
                bOccluded = 1;
                RdxLog(TEXT("the window became occluded, presentation is paused"));
            }
            else if (Hr == DXGI_ERROR_DEVICE_REMOVED || Hr == DXGI_ERROR_DEVICE_RESET)
            {
                NoteDeviceLost(Hr);
            }
            else if (FAILED(Hr))
            {
                RdxLog(TEXT("Present failed 0x%08X"), (INT)Hr);
            }
        }
        E4 = appSeconds();
        MoveToNextFrame();
    }
    else
    {
        // Even with nothing presented the frame has to be closed off towards the GPU, or the next frame would
        // reset the very command allocator the GPU is still reading.
        const UINT64 Current = FenceValues[FrameIndex];
        if (SUCCEEDED(Queue->Signal(Fence.Get(), Current)))
        {
            // Never INFINITE, here least of all: a wait that cannot end stops the game's
            // message loop with it - which is exactly what the Alt+Tab freeze looked like.
            // The other two waits were bounded then; this one was missed.
            if (Fence->GetCompletedValue() < Current &&
                SUCCEEDED(Fence->SetEventOnCompletion(Current, FenceEvent)) &&
                WaitFenceEvent(5000) == WAIT_TIMEOUT)
            {
                RdxLog(TEXT("fence timeout with nothing presented: waited for %I64u, reached %I64u"),
                       Current, Fence->GetCompletedValue());
            }
            FenceValues[FrameIndex] = Current + 1;
        }
    }

    const DOUBLE E5 = appSeconds();
    FgWhyLast = (FgWhy == 0 && !bFgLast) ? 4 : FgWhy;
    EndParts[0] = E1 - E0;
    EndParts[1] = E2 - E1;
    EndParts[2] = E3 - E2;
    EndParts[3] = E4 - E3;
    EndParts[4] = E5 - E4;
    bInFrame = 0;
}

// ---------------------------------------------------------------------------
// The ripples things make in water (Water=Low and up)
// ---------------------------------------------------------------------------

static const char* GRippleShader =
"// The ripples' step (FBackend::StepRipples): the wave equation on a height field around the camera, and the pushes of\n"
"// the things moving through the water. The field is two numbers a texel - the height now, and one step before.\n"
"Texture2D Field : register(t0);\n"
"cbuffer Step : register(b0)\n"
"{\n"
"    int2   Shift;      // the field moved this many texels since the last step: each texel reads where it was\n"
"    float  Keep;       // what is left of a wave after a step\n"
"    float  Speed2;     // the wave's speed squared, in texels a step\n"
"    uint   Pulses;\n"
"    float  Limit;      // the most the field may lift or lower the surface\n"
"    uint2  Unused;\n"
"    float4 Pulse[12];  // texel X, Y, radius in texels, push in world units of height\n"
"};\n"
"float4 RippleVS(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"float2 RipplePS(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    // Outside the field a read gives zero: the water beyond it is still.\n"
"    const int2   P = int2(Pos.xy) + Shift;\n"
"    const float2 C = Field.Load(int3(P, 0)).xy;\n"
"    const float  N = Field.Load(int3(P + int2(1, 0), 0)).x + Field.Load(int3(P - int2(1, 0), 0)).x +\n"
"                     Field.Load(int3(P + int2(0, 1), 0)).x + Field.Load(int3(P - int2(0, 1), 0)).x;\n"
"    float H = (2.0f * C.x - C.y + Speed2 * (N - 4.0f * C.x)) * Keep;\n"
"    for (uint i = 0; i < Pulses; i++)\n"
"    {\n"
"        const float2 D = Pos.xy - Pulse[i].xy;\n"
"        H += Pulse[i].w * exp(-dot(D, D) / (Pulse[i].z * Pulse[i].z));\n"
"    }\n"
"    return float2(clamp(H, -Limit, Limit), C.x);\n"
"}\n";

// The state the water's two fields rest in - the ripples' and the texture's own waves. The water's domain shader lifts
// the surface by them (DSWater) and its pixel shader reads them too, so both read states at once: the pixel shader's
// alone is not a state another stage may read in, and a card that holds the two apart can stop at such a read.
static const D3D12_RESOURCE_STATES GWaterFieldRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// What the step's constants hold, in the order the shader reads them.
struct FRippleStep
{
    INT   ShiftX, ShiftY;
    FLOAT Keep, Speed2;
    UINT  Pulses;
    FLOAT Limit;
    UINT  Unused[2];
    FLOAT Pulse[RdxMath::RippleMaxPulses * 4];
};

void FBackend::SetWaterWaves(const BYTE* Heights, INT W, INT H)
{
    if (!Heights || W <= 0 || H <= 0 || W * H > (INT)sizeof(WaveHeights))
        return;
    appMemcpy(WaveHeights, Heights, W * H);
    WaveW    = W;
    WaveH    = H;
    bWaveNew = 1;
}

// The wave field's GPU copy and its upload buffers (one per frame slot, mapped for good), and its view in the fourth slot
// of both water view sets. Made the first time a wave field arrives; a failure is said once and leaves the water with the
// waves it computes itself.
// The water's splashes (DrawSplashes). Every drop is worked out where it is from its splash's start alone - thrown up and
// falling under gravity - so there is nothing to simulate and nothing kept from frame to frame. The drops go in as small
// spheres, their nearest depth and their thickness into two targets; the depth is then smoothed, so neighbouring drops run
// together into one surface - a sheet where they are close, beads where they part - and the geometry pass lays that
// surface over the picture as water (FSolidPass, PSSplash).
static const char* const GSplashShader =
"cbuffer SplashConsts : register(b0)\n"
"{\n"
"    float4 SplashHead;      // how many splashes, the time now (seconds), gravity (units a second squared), the focal length\n"
"    float4 SplashScreen;    // the screen's centre in the engine's pixels; 2 / the engine's width and height\n"
"    float4 SplashJitter;    // the jitter in clip space; the targets' width and height in pixels\n"
"    float4 SplashAX;        // the view's three axes in the world\n"
"    float4 SplashAY;\n"
"    float4 SplashAZ;\n"
"    float4 Splash[24];      // two a splash: its place from the camera (world units) and its start; its radius, strength, seed\n"
"    float4 CurtainHead;     // how many fountains, how much water they let down (of 1), gravity, its speed at\n"
"                            // the slot\n"
"    float4 Curtain[24];     // three a fountain: the slot's one end from the camera and the fall's height; its\n"
"                            // other end, how high its chute climbs, and a seed; how far the water spreads off\n"
"                            // its plane, where the fall is in its turn, the turns' count, a turn's seconds\n"
"    float4 Chute[56];       // seven a fountain - the chute its water comes down to the slot by: six pieces from\n"
"                            // the slot upwards (its way on the level as an angle, its drop for a unit of that,\n"
"                            // the water's seconds over it, how far it goes in them), then where the water's\n"
"                            // round of the chute stands, the round's seconds, the pieces' count and how far\n"
"                            // back from the slot they go in all\n"
"};\n"
"// t0 is the copy of the picture (the water's set), not read here.\n"
"Texture2D SceneDepth : register(t1);     // the copy of the depth: 1/z\n"
"Texture2D BlurFrom   : register(t2);     // across: the drops' depth; down: the first pass's depth and thickness\n"
"Texture2D BlurThick  : register(t3);     // across: the drops' thickness\n"
"\n"
"// A number in [0, 1) from any other: multiplies by large odd numbers between shifts - enough to scatter drops.\n"
"float SplashRandom(uint N)\n"
"{\n"
"    N ^= N >> 16;\n"
"    N *= 0x9E3779B9U;\n"
"    N ^= N >> 13;\n"
"    N *= 0xA24BAED5U;\n"
"    N ^= N >> 16;\n"
"    return float(N >> 8) * (1.0f / 16777216.0f);\n"
"}\n"
"\n"
"struct SplashDrop\n"
"{\n"
"    float4 Pos    : SV_Position;\n"
"    float2 Corner : TEXCOORD0;      // where on the drop's square, -1 to 1 either way\n"
"    float  Z      : TEXCOORD1;      // the drop's centre, as far ahead as the view looks\n"
"    float  Size   : TEXCOORD2;      // its radius, world units\n"
"    float  Above  : TEXCOORD3;      // how high over its pool - a fountain's drops\n"
"};\n"
"\n"
"// One drop: its splash and which of the splash's drops from the instance, the corner of its square from the vertex. Three\n"
"// kinds: the crown - a sheet thrown outwards and up from the rim at once; the column that rises from the middle a moment\n"
"// later, as the hole the thing made closes; and fine spray, thrown faster and further. Each is where its throw and gravity\n"
"// have put it by now, and gone once it is back under the surface.\n"
"SplashDrop SplashParticleVS(uint Vertex : SV_VertexID, uint Instance : SV_InstanceID)\n"
"{\n"
"    SplashDrop Out;\n"
"    Out.Corner = float2((Vertex & 1) ? 1.0f : -1.0f, (Vertex & 2) ? 1.0f : -1.0f);\n"
"    Out.Pos    = float4(0.0f, 0.0f, -1.0f, 1.0f);     // outside the depth range: nothing is drawn\n"
"    Out.Z      = 0.0f;\n"
"    Out.Size   = 0.0f;\n"
"    Out.Above  = 1.0e6f;\n"
"    const uint S = Instance / RDX_SPLASH_DROPS;\n"
"    const uint I = Instance % RDX_SPLASH_DROPS;\n"
"    if (S >= (uint)SplashHead.x)\n"
"        return Out;\n"
"    const float4 A    = Splash[S * 2];\n"
"    const float4 B    = Splash[S * 2 + 1];\n"
"    const uint   Seed = (uint)B.z * 2654435761U + I * 40503U;\n"
"    const float  R    = B.x;\n"
"    const float  St   = B.y;\n"
"    const float  Turn = 6.2831853f * SplashRandom(Seed + 1U);\n"
"    const float2 Dir  = float2(cos(Turn), sin(Turn));\n"
"    float  Delay, Size;\n"
"    float3 P0, V0;\n"
"    if (I < 64U)\n"
"    {\n"
"        // The column: drawn up from the middle one after another, the first the fastest - so they rise as one column, tip\n"
"        // first, and it breaks into drops as it falls back.\n"
"        const float K = float(I) / 64.0f;\n"
"        Delay = 0.16f + 0.10f * K;\n"
"        P0    = float3(Dir * (R * 0.08f * SplashRandom(Seed + 3U)), 0.0f);\n"
"        // B.w: the column's scale (AddSplashes) - a waterfall's foot throws a lower one; zero is an older record's one.\n"
"        V0    = float3(Dir * (St * 0.03f * SplashRandom(Seed + 4U)),\n"
"                       St * (1.4f - 0.6f * K + 0.05f * SplashRandom(Seed + 5U)) * ((B.w > 0.0f) ? B.w : 1.0f));\n"
"        Size  = R * (0.14f + 0.10f * K);\n"
"    }\n"
"    else if (I < 448U)\n"
"    {\n"
"        // The crown: a sheet flaring out from the rim - every drop of it thrown at much the same slant, only faster or\n"
"        // slower, so they stay on one surface, and its top edge breaks into drops.\n"
"        Delay = 0.03f * SplashRandom(Seed + 2U);\n"
"        P0    = float3(Dir * (R * (0.85f + 0.2f * SplashRandom(Seed + 3U))), 0.0f);\n"
"        const float Up = St * (0.3f + 0.5f * SplashRandom(Seed + 5U));\n"
"        V0    = float3(Dir * (Up * (0.35f + 0.1f * SplashRandom(Seed + 4U))), Up);\n"
"        Size  = R * (0.09f + 0.07f * SplashRandom(Seed + 6U));\n"
"    }\n"
"    else\n"
"    {\n"
"        Delay = 0.02f * SplashRandom(Seed + 2U);\n"
"        P0    = float3(Dir * (R * 0.9f), 0.0f);\n"
"        V0    = float3(Dir * (St * (0.35f + 0.45f * SplashRandom(Seed + 4U))), St * (0.55f + 0.65f * SplashRandom(Seed + 5U)));\n"
"        Size  = R * (0.025f + 0.03f * SplashRandom(Seed + 6U));\n"
"    }\n"
"    const float T = SplashHead.y - A.w - Delay;\n"
"    if (T <= 0.0f)\n"
"        return Out;\n"
"    const float3 P = P0 + V0 * T + float3(0.0f, 0.0f, 0.5f * SplashHead.z * T * T);\n"
"    if (P.z < -Size)\n"
"        return Out;\n"
"    Size *= saturate(1.2f - 0.4f * T);                 // the sheet thins as it spreads\n"
"    const float3 Q = A.xyz + P;\n"
"    float3 V = float3(dot(Q, SplashAX.xyz), dot(Q, SplashAY.xyz), dot(Q, SplashAZ.xyz));\n"
"    if (V.z < 4.0f + Size)\n"
"        return Out;\n"
"    Out.Z    = V.z;\n"
"    Out.Size = Size;\n"
"    V.xy += Out.Corner * Size;\n"
"    // Onto the screen as the engine projects, then into clip space as the geometry pass does.\n"
"    const float2 Scr = SplashScreen.xy + V.xy * (SplashHead.w / V.z);\n"
"    Out.Pos = float4((Scr.x * SplashScreen.z - 1.0f + SplashJitter.x) * V.z,\n"
"                     (1.0f - Scr.y * SplashScreen.w + SplashJitter.y) * V.z, 0.5f * V.z, V.z);\n"
"    return Out;\n"
"}\n"
"\n"
"// A stream's sway at a time: smooth, between -1 and 1, each stream's its own.\n"
"float CurtainSway(float X, uint Seed)\n"
"{\n"
"    const float F = floor(X);\n"
"    const float W = X - F;\n"
"    const float A = SplashRandom(Seed + (uint)(int)F * 7919U);\n"
"    const float B = SplashRandom(Seed + (uint)((int)F + 1) * 7919U);\n"
"    return lerp(A, B, W * W * (3.0f - 2.0f * W)) * 2.0f - 1.0f;\n"
"}\n"
"\n"
"// An ornamental fountain's drops (FBackend::SetCurtains). A fountain is RDX_CURTAIN_STREAMS streams side by side\n"
"// along its slot, each RDX_CURTAIN_STREAM_DROPS drops leaving the slot one after another at even times and falling\n"
"// as a thrown thing does, and RDX_CURTAIN_SPRAY drops thrown up where the water comes down. A drop is drawn long\n"
"// enough to reach the next one of its stream, so a stream is one line of water. How much water there is\n"
"// (CurtainHead.y) says how many streams, how thick and how close: at the most they are one sheet, flowing\n"
"// everywhere; with less they stand apart and sway - and meet, run together and part again as they do.\n"
"SplashDrop CurtainParticleVS(uint Vertex : SV_VertexID, uint Instance : SV_InstanceID)\n"
"{\n"
"    SplashDrop Out;\n"
"    Out.Corner = float2((Vertex & 1) ? 1.0f : -1.0f, (Vertex & 2) ? 1.0f : -1.0f);\n"
"    Out.Pos    = float4(0.0f, 0.0f, -1.0f, 1.0f);     // outside the depth range: nothing is drawn\n"
"    Out.Z      = 0.0f;\n"
"    Out.Size   = 0.0f;\n"
"    Out.Above  = 1.0e6f;\n"
"    const uint Falling = RDX_CURTAIN_STREAMS * RDX_CURTAIN_STREAM_DROPS;\n"
"    const uint Each    = Falling + RDX_CURTAIN_SPRAY + RDX_CURTAIN_STREAMS * RDX_CHUTE_STREAM_DROPS;\n"
"    const uint F = Instance / Each;\n"
"    const uint I = Instance % Each;\n"
"    if (F >= (uint)CurtainHead.x)\n"
"        return Out;\n"
"    const float4 A      = Curtain[F * 3];\n"
"    const float4 B      = Curtain[F * 3 + 1];\n"
"    const float4 E      = Curtain[F * 3 + 2];\n"
"    const float  Rise   = max(A.w, 1.0f);\n"
"    const float  Long   = max(length(B.xy - A.xy), 1.0f);\n"
"    const float2 U      = (B.xy - A.xy) / Long;           // along the slot, on the level\n"
"    const float2 Off    = float2(U.y, -U.x);              // off the curtain's plane\n"
"    const float  G      = CurtainHead.z;\n"
"    const float  V0     = CurtainHead.w;\n"
"    const float  Much   = saturate(CurtainHead.y);\n"
"    const uint   Base   = (uint)B.w * 7919U + 17U;\n"
"    // The drop from the slot's first end: x along the slot, y off the plane, z up - how long it is drawn, for its\n"
"    // width, and which way it is long: the way it goes, on the level's axes - down, where it falls.\n"
"    float3 P;\n"
"    float  Size, Len;\n"
"    float3 Flow = float3(0.0f, 0.0f, -1.0f);\n"
"    if (I < Falling)\n"
"    {\n"
"        const uint S = I / RDX_CURTAIN_STREAM_DROPS;\n"
"        const uint K = I % RDX_CURTAIN_STREAM_DROPS;\n"
"        // As many streams as the slot's length asks for: close enough to be one sheet all the way down, or - the\n"
"        // same water, less of it - apart, as strands.\n"
"        const uint Count = min((uint)RDX_CURTAIN_STREAMS, (uint)ceil(Long / lerp(1.6f, 0.5f, Much)));\n"
"        if (S >= Count)\n"
"            return Out;\n"
"        const uint  Seed  = Base + S * 40503U;\n"
"        // Where the drop is in its turn: how long it has been falling.\n"
"        const float Turn  = E.y + ((float)K + SplashRandom(Seed + 1U)) / (float)RDX_CURTAIN_STREAM_DROPS;\n"
"        const float T     = frac(Turn) * E.w;\n"
"        const float Fell  = V0 * T + 0.5f * G * T * T;\n"
"        if (Fell >= Rise)\n"
"            return Out;\n"
"        const float Speed = V0 + G * T;\n"
"        // What the stream was doing when the drop left the slot is what the drop carries down: the stream sways,\n"
"        // slowly, and every drop goes on the way it was let go - so a stream is one unbroken line that wanders,\n"
"        // meets the next one, runs with it for a while and parts from it again.\n"
"        const float Left  = (E.z + E.y) * E.w - T;\n"
"        const float Sway  = lerp(5.0f, 3.0f, Much);\n"
"        // Which drop of the stream this is: every turn's are scattered their own way.\n"
"        const uint  Drop  = Seed + (((uint)E.z + (uint)floor(Turn)) * RDX_CURTAIN_STREAM_DROPS + K) * 2654435761U;\n"
"        // Now and then a little of the water is thrown clear of its stream - sideways, or out from the curtain.\n"
"        const bool  bStray = SplashRandom(Drop + 21U) < 0.07f;\n"
"        // The water thins as it gathers speed, once it has any to speak of - only a little, or it would tear. And\n"
"        // it never leaves the slot evenly: more comes for a moment, then less, much the same all along the slot -\n"
"        // waves in the film's thickness that go down with the water.\n"
"        Size = lerp(0.34f, 0.50f, Much) * max(sqrt(40.0f / max(Speed, 40.0f)), 0.85f) *\n"
"               (1.0f + 0.22f * CurtainSway(Left * 7.0f + 0.11f * (float)S, Base + 31U) +\n"
"                0.12f * CurtainSway(Left * 17.0f + 0.37f * (float)S, Base + 37U));\n"
"        // Where the stream leaves the slot wanders a little; which way it leaves sways - slowly, and with a\n"
"        // quicker flutter - so the streams lean across one another, run into one another and part again.\n"
"        const float Lean = CurtainSway(Left * 1.1f + 13.0f * SplashRandom(Seed + 4U), Seed + 11U) +\n"
"                           0.6f * CurtainSway(Left * 3.7f + 5.0f * SplashRandom(Seed + 9U), Seed + 17U);\n"
"        P.x  = ((float)S + 0.5f + 0.5f * (SplashRandom(Seed + 3U) - 0.5f)) * Long / (float)Count +\n"
"               0.5f * CurtainSway(Left * 0.45f + 3.0f * SplashRandom(Seed + 10U), Seed + 19U) + Sway * Lean * T;\n"
"        P.y  = (SplashRandom(Seed + 5U) * 2.0f - 1.0f) * E.x +\n"
"               0.35f * Sway * CurtainSway(Left * 0.9f + 7.0f * SplashRandom(Seed + 6U), Seed + 13U) * T;\n"
"        P.z  = -Fell;\n"
"        // Drawn as long as the way to the stream's next drop, so the stream holds together all the way down.\n"
"        Len  = max(1.25f * Speed * E.w / (float)RDX_CURTAIN_STREAM_DROPS / (2.0f * Size), 1.0f);\n"
"        if (bStray)\n"
"        {\n"
"            P.x  += (SplashRandom(Drop + 22U) - 0.5f) * 26.0f * T;\n"
"            P.y  += (SplashRandom(Drop + 23U) - 0.5f) * 16.0f * T;\n"
"            Size *= 0.6f;\n"
"            Len   = min(Len, 2.5f);\n"
"        }\n"
"    }\n"
"    else if (I < Falling + RDX_CURTAIN_SPRAY)\n"
"    {\n"
"        // The spray: small drops thrown up and outwards from where the water comes down, each for a time of its own.\n"
"        const uint  Seed = Base + (I - Falling) * 96769U + 3U;\n"
"        const float Life = 0.22f + 0.20f * SplashRandom(Seed + 1U);\n"
"        const float Turn = (E.z + E.y) * E.w / Life + SplashRandom(Seed + 2U);\n"
"        const uint  Drop = Seed + (uint)floor(Turn) * 2654435761U;\n"
"        const float T    = frac(Turn) * Life;\n"
"        if (SplashRandom(Drop + 9U) < 0.5f * (1.0f - Much))\n"
"            return Out;                                 // less water throws less of it up\n"
"        const float Up   = 30.0f + 70.0f * SplashRandom(Drop + 3U);\n"
"        P.x  = SplashRandom(Drop + 6U) * Long + (SplashRandom(Drop + 4U) - 0.5f) * 50.0f * T;\n"
"        P.y  = (SplashRandom(Drop + 7U) * 2.0f - 1.0f) * E.x + (SplashRandom(Drop + 5U) - 0.5f) * 90.0f * T;\n"
"        P.z  = Up * T - 0.5f * G * T * T - Rise;\n"
"        if (P.z < -Rise)\n"
"            return Out;\n"
"        Size = 0.22f + 0.26f * SplashRandom(Drop + 8U);\n"
"        Len  = 1.0f;\n"
"    }\n"
"    else\n"
"    {\n"
"        // The water on its way to the slot, down the fountain's chute (Chute): the same streams before they fall,\n"
"        // RDX_CHUTE_STREAM_DROPS drops of each one after another all the way down it, every one heading for the\n"
"        // place its stream leaves the slot from at the moment it gets there. It runs slower than it falls, so the\n"
"        // drops lie closer and larger: a deeper sheet of the same water than the curtain it becomes.\n"
"        const uint   J     = I - Falling - RDX_CURTAIN_SPRAY;\n"
"        const uint   S     = J / RDX_CHUTE_STREAM_DROPS;\n"
"        const uint   K     = J % RDX_CHUTE_STREAM_DROPS;\n"
"        const float4 Head  = Chute[F * 7 + 6];\n"
"        const uint   Count = min((uint)RDX_CURTAIN_STREAMS, (uint)ceil(Long / lerp(1.6f, 0.5f, Much)));\n"
"        if (S >= Count || Head.y <= 0.0f)\n"
"            return Out;\n"
"        const uint   Seed  = Base + S * 40503U;\n"
"        // How long before the drop is at the slot: they all go round in the chute's whole time, evenly apart.\n"
"        const float  Round = Head.x + ((float)K + SplashRandom(Seed + 41U)) / (float)RDX_CHUTE_STREAM_DROPS;\n"
"        const float  Ahead = (1.0f - frac(Round)) * Head.y;\n"
"        const float  Left  = (E.z + E.y) * E.w + Ahead;\n"
"        // Up the chute from the slot, piece by piece.\n"
"        float2 At   = float2(0.0f, 0.0f);\n"
"        float  High = 0.0f;\n"
"        float  More = Ahead;\n"
"        [loop] for (uint k = 0U; k < 6U; k++)\n"
"        {\n"
"            const float4 Piece = Chute[F * 7 + k];\n"
"            if (Piece.z <= 0.0f)\n"
"                break;\n"
"            const float Spent = min(More, Piece.z);\n"
"            const float Went  = Piece.w * Spent / Piece.z;\n"
"            At   -= float2(cos(Piece.x), sin(Piece.x)) * Went;\n"
"            High += Piece.y * Went;\n"
"            Flow  = normalize(float3(cos(Piece.x), sin(Piece.x), -Piece.y));\n"
"            More -= Spent;\n"
"            if (More <= 0.0f)\n"
"                break;\n"
"        }\n"
"        if (More > 0.001f)\n"
"            return Out;                                 // above the chute's top: not there yet\n"
"        const uint  Drop = Seed + ((uint)floor(Round) * RDX_CHUTE_STREAM_DROPS + K) * 2654435761U;\n"
"        // The film's thickness comes in waves that go down with the water, as the curtain's does.\n"
"        Size = lerp(0.62f, 0.86f, Much) *\n"
"               (1.0f + 0.22f * CurtainSway(Left * 7.0f + 0.11f * (float)S, Base + 31U) +\n"
"                0.12f * CurtainSway(Left * 17.0f + 0.37f * (float)S, Base + 37U));\n"
"        // On the chute the water lies as one film from side to side: a stream's drops are spread over its own\n"
"        // place and its neighbours' - the middle the likeliest, so all the streams together lie evenly - turned\n"
"        // back in at the chute's sides, and gather into their stream only as they come to the slot.\n"
"        const float Apart = Long / (float)Count;\n"
"        const float Home  = ((float)S + 0.5f + 0.5f * (SplashRandom(Seed + 3U) - 0.5f)) * Apart +\n"
"                            0.5f * CurtainSway(Left * 0.45f + 3.0f * SplashRandom(Seed + 10U), Seed + 19U);\n"
"        const float Aside = (frac((float)K * 0.6180340f + SplashRandom(Seed + 43U)) + SplashRandom(Drop + 25U) -\n"
"                             1.0f) * Apart;\n"
"        P.x  = lerp(Home, Long - abs(Long - abs(Home + Aside)), smoothstep(0.0f, 0.6f, Ahead)) + dot(At, U);\n"
"        P.y  = (SplashRandom(Seed + 5U) * 2.0f - 1.0f) * E.x + dot(At, Off);\n"
"        P.z  = High + 0.35f * Size + (SplashRandom(Drop + 24U) - 0.5f) * 0.5f;\n"
"        // Long the way the water runs: on the chute it goes as streaks, not as beads.\n"
"        Len  = 2.5f;\n"
"    }\n"
"    const float3 Q = A.xyz + float3(U * P.x + Off * P.y, P.z);\n"
"    float3 V = float3(dot(Q, SplashAX.xyz), dot(Q, SplashAY.xyz), dot(Q, SplashAZ.xyz));\n"
"    if (V.z < 4.0f + Size)\n"
"        return Out;\n"
"    Out.Z    = V.z;\n"
"    Out.Size = Size;\n"
"    // The corner's height over the pool: what of a long drop reaches under the water's surface is not drawn.\n"
"    Out.Above = Rise + P.z - Out.Corner.y * Size * Len;\n"
"    // Long the way it goes - down, where it falls - as the view sees it: not at all where the view looks along it.\n"
"    const float2 Down = float2(dot(Flow, SplashAX.xyz), dot(Flow, SplashAY.xyz));\n"
"    const float  Seen = length(Down);\n"
"    const float2 Way  = (Seen > 0.05f) ? Down / Seen : float2(0.0f, 1.0f);\n"
"    V.xy += float2(Way.y, -Way.x) * (Out.Corner.x * Size) + Way * (Out.Corner.y * Size * max(Len * Seen, 1.0f));\n"
"    const float2 Scr = SplashScreen.xy + V.xy * (SplashHead.w / V.z);\n"
"    Out.Pos = float4((Scr.x * SplashScreen.z - 1.0f + SplashJitter.x) * V.z,\n"
"                     (1.0f - Scr.y * SplashScreen.w + SplashJitter.y) * V.z, 0.5f * V.z, V.z);\n"
"    return Out;\n"
"}\n"
"\n"
"struct SplashDropOut\n"
"{\n"
"    float Depth : SV_Target0;\n"
"    float Thick : SV_Target1;\n"
"};\n"
"\n"
"// The drop: its front at this pixel into the depth target (the nearest of all the drops stays), and its water into the\n"
"// thickness target, where all of them add up - thickest in the middle and falling to nothing at the edge without a step,\n"
"// so that drops close together make one smooth body of water there. Behind the level's own geometry it is not seen.\n"
"SplashDropOut SplashParticlePS(SplashDrop In)\n"
"{\n"
"    const float D2 = dot(In.Corner, In.Corner);\n"
"    if (D2 >= 1.0f || In.Above < 0.0f)\n"
"        discard;\n"
"    const float H     = sqrt(1.0f - D2) * In.Size;\n"
"    const float Front = In.Z - H;\n"
"    const float Scene = 1.0f / max(SceneDepth.Load(int3(In.Pos.xy, 0)).r, 1e-6f);\n"
"    if (Front >= Scene)\n"
"        discard;\n"
"    SplashDropOut Out;\n"
"    Out.Depth = Front;\n"
"    Out.Thick = min(2.0f * In.Size * (1.0f - D2) * (1.0f - D2), Scene - Front);\n"
"    return Out;\n"
"}\n"
"\n"
"float4 SplashScreenVS(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    const float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"\n"
"// The drops' depth and thickness at a pixel: from their own targets in the first pass, from the first pass's in the second.\n"
"float2 SplashTaken(int2 P, bool bFirst)\n"
"{\n"
"    return bFirst ? float2(BlurFrom.Load(int3(P, 0)).r, BlurThick.Load(int3(P, 0)).r) : BlurFrom.Load(int3(P, 0)).rg;\n"
"}\n"
"\n"
"// One pass of the smoothing, one direction - across, then down - every pixel or every other along a line of 25, so nothing\n"
"// of a grid shows. The thickness is blurred plainly, over about two world units at the splash: the drops' water runs\n"
"// together, and where there is enough of it is the surface - its slant the thickness's own (the laying-on, PSSplash). The\n"
"// depth only says where the water is, for its light: the nearest drop around is enough.\n"
"// Whether a pixel's view goes through an ornamental fountain's water - through its curtain: the slot's length\n"
"// across, the fall's height down, and a little more around for the spray; or through the film on its chute - and\n"
"// how wide the smoothing is to be there, in world units; zero where it does not. Finer than a splash's: a\n"
"// fountain's streams are to stay streams until they meet, and its drops drops.\n"
"#define CURTAIN_SMOOTH 0.925f\n"
"// The stretch of a line - from From, Step for a unit of its way - that lies between Lo and Hi: where it begins and\n"
"// where it ends, the end before the beginning where none of it does.\n"
"float2 CurtainSlab(float From, float Step, float Lo, float Hi)\n"
"{\n"
"    if (abs(Step) < 1.0e-5f)\n"
"        return (From >= Lo && From <= Hi) ? float2(-1.0e9f, 1.0e9f) : float2(1.0f, 0.0f);\n"
"    const float T0 = (Lo - From) / Step;\n"
"    const float T1 = (Hi - From) / Step;\n"
"    return float2(min(T0, T1), max(T0, T1));\n"
"}\n"
"float CurtainAt(int2 P)\n"
"{\n"
"    const int Count = (int)CurtainHead.x;\n"
"    if (Count <= 0)\n"
"        return 0.0f;\n"
"    // The view through the pixel, on the level's axes: the targets' pixels to the engine's, then as the drops are\n"
"    // put on the screen, backwards.\n"
"    const float2 Eng = (float2(P) + 0.5f) / (SplashJitter.zw * SplashScreen.zw * 0.5f);\n"
"    const float2 Tan = (Eng - SplashScreen.xy) / SplashHead.w;\n"
"    const float3 Dir = Tan.x * SplashAX.xyz + Tan.y * SplashAY.xyz + SplashAZ.xyz;\n"
"    [loop] for (int i = 0; i < Count; i++)\n"
"    {\n"
"        const float4 A    = Curtain[i * 3];\n"
"        const float4 B    = Curtain[i * 3 + 1];\n"
"        const float  Long = max(length(B.xy - A.xy), 1.0f);\n"
"        const float2 U    = (B.xy - A.xy) / Long;\n"
"        const float2 Off  = float2(U.y, -U.x);\n"
"        // Through the film on its chute: over the slot's length, from the slot back as far as the chute goes and\n"
"        // as high as it climbs.\n"
"        const float4 Top  = Chute[i * 7 + 6];\n"
"        if (Top.w > 0.0f)\n"
"        {\n"
"            const float2 Back = -float2(cos(Chute[i * 7].x), sin(Chute[i * 7].x));\n"
"            const float2 TS   = CurtainSlab(-dot(A.xy, U), dot(Dir.xy, U), -8.0f, Long + 8.0f);\n"
"            const float2 TB   = CurtainSlab(-dot(A.xy, Back), dot(Dir.xy, Back), -2.0f, Top.w + 4.0f);\n"
"            const float2 TZ   = CurtainSlab(-A.z, Dir.z, -2.0f, B.z + 6.0f);\n"
"            if (max(max(TS.x, TB.x), max(TZ.x, 0.0f)) < min(min(TS.y, TB.y), TZ.y))\n"
"                return CURTAIN_SMOOTH;\n"
"        }\n"
"        const float  Turn = dot(Dir.xy, Off);\n"
"        if (abs(Turn) < 1.0e-3f)\n"
"            continue;                               // along the curtain's own plane: it is a line there\n"
"        const float T = dot(A.xy, Off) / Turn;\n"
"        if (T <= 0.0f)\n"
"            continue;\n"
"        const float3 H = Dir * T - A.xyz;\n"
"        const float  S = dot(H.xy, U);\n"
"        if (S > -8.0f && S < Long + 8.0f && H.z < 4.0f && H.z > -(A.w + 4.0f))\n"
"            return CURTAIN_SMOOTH;\n"
"    }\n"
"    return 0.0f;\n"
"}\n"
"\n"
"float2 SplashSmooth(int2 P, int2 Step, bool bFirst)\n"
"{\n"
"    const int2   Hi = int2(SplashJitter.zw) - 1;\n"
"    const float2 C  = SplashTaken(P, bFirst);\n"
"    // Two world units at the nearest drop's depth, in the targets' pixels (the focal length is in the engine's); where\n"
"    // there is no drop, as for one at the splash's usual distance. A fountain's water by its own, finer width - and\n"
"    // beside a drop far less than on it, which keeps a drop's edge sharp: smoothed alike on both sides the water\n"
"    // comes out soft all over, as if drawn small and blown up.\n"
"    const float Fine      = CurtainAt(P);\n"
"    const float PerEngine = SplashJitter.z * SplashScreen.z * 0.5f;\n"
"    const float Radius    = (C.x < 1.0e5f)\n"
"                          ? clamp(((Fine > 0.0f) ? Fine : 2.0f) * SplashHead.w * PerEngine / C.x, 1.0f, 20.0f)\n"
"                          : ((Fine > 0.0f) ? 4.0f * Fine : 8.0f);\n"
"    const float Spacing   = max(Radius / 8.0f, 1.0f);\n"
"    float Depth = 1.0e6f, Thick = 0.0f, Weight = 0.0f;\n"
"    [loop] for (int k = -12; k <= 12; k++)\n"
"    {\n"
"        const float  O = k * Spacing;\n"
"        const float2 S = SplashTaken(clamp(P + Step * (int)round(O), int2(0, 0), Hi), bFirst);\n"
"        const float  G = exp(-O * O / (0.5f * Radius * Radius));\n"
"        Thick  += S.y * G;\n"
"        Weight += G;\n"
"        if (G > 0.1f)\n"
"            Depth = min(Depth, S.x);\n"
"    }\n"
"    return float2(Depth, Thick / max(Weight, 1e-6f));\n"
"}\n"
"\n"
"float2 SplashBlurXPS(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    return SplashSmooth(int2(Pos.xy), int2(1, 0), true);\n"
"}\n"
"\n"
"struct SplashSmoothOut\n"
"{\n"
"    float Depth    : SV_Target0;\n"
"    float Thick    : SV_Target1;\n"
"    float Reactive : SV_Target2;\n"
"};\n"
"\n"
"// The second pass, back into the drops' targets, and the upscaler's reactive mask with it: where the water is, it keeps\n"
"// less of the frames before - the drops have no motion vectors of their own.\n"
"SplashSmoothOut SplashBlurYPS(float4 Pos : SV_Position)\n"
"{\n"
"    const float2 S = SplashSmooth(int2(Pos.xy), int2(0, 1), false);\n"
"    SplashSmoothOut Out;\n"
"    Out.Depth    = S.x;\n"
"    Out.Thick    = S.y;\n"
"    Out.Reactive = (S.x < 1.0e5f) ? 0.9f * saturate(S.y / 3.0f) : 0.0f;\n"
"    return Out;\n"
"}\n";

static void SplashBarrier(D3D12_RESOURCE_BARRIER& B, ID3D12Resource* R, D3D12_RESOURCE_STATES From, D3D12_RESOURCE_STATES To)
{
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = R;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = From;
    B.Transition.StateAfter  = To;
}

UBOOL FBackend::EnsureReactive()
{
    if (Reactive && ReactiveRtvHeap)
        return 1;
    if (!Device || !Textures.GetHeap() || SceneWidth <= 0 || SceneHeight <= 0)
        return 0;
    static INT Told = 0;
    if (!ReactiveRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 1;
        if (FAILED(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), ReactiveRtvHeap.VoidSlot())))
        {
            if (Told++ < 3)
                RdxLog(TEXT("reactive mask: its render target view's heap would not be made"));
            return 0;
        }
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = SceneWidth;
    Desc.Height           = SceneHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Format           = DXGI_FORMAT_R8_UNORM;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format = DXGI_FORMAT_R8_UNORM;
    Reactive.Release();
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               &Clear, __uuidof(ID3D12Resource), Reactive.VoidSlot())))
    {
        Reactive.Release();
        if (Told++ < 3)
            RdxLog(TEXT("reactive mask: the %ix%i texture would not be made"), SceneWidth, SceneHeight);
        return 0;
    }
    ReactiveState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Device->CreateRenderTargetView(Reactive.Get(), nullptr, ReactiveRtvHeap->GetCPUDescriptorHandleForHeapStart());
    Volume.ForgetReactive();    // the fires' walk holds a view of the mask in a heap of its own
    RdxViewsChanged();
    RdxLog(TEXT("reactive mask: ready, %ix%i - written by the water's splashes and the walk (fires, columns)"),
           SceneWidth, SceneHeight);
    return 1;
}

void FBackend::ReactiveTo(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES State)
{
    if (!Reactive || ReactiveState == State)
        return;
    D3D12_RESOURCE_BARRIER B;
    SplashBarrier(B, Reactive.Get(), ReactiveState, State);
    Cmd->ResourceBarrier(1, &B);
    ReactiveState = State;
}

UBOOL FBackend::ReactiveBegin(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES State)
{
    if (!EnsureReactive())
        return 0;
    // The upscaler reads the whole mask: the frame's first writer clears it, and every writer after that adds to it -
    // the splashes by keeping the larger of theirs and what is there (their pipeline's blend), the water by writing where
    // its surface is.
    if (!bReactiveInit)
    {
        ReactiveTo(Cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const FLOAT Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Cmd->ClearRenderTargetView(ReactiveRtvHeap->GetCPUDescriptorHandleForHeapStart(), Zero, 0, nullptr);
        bReactiveInit = 1;
    }
    ReactiveTo(Cmd, State);
    bReactiveLive = 1;
    return 1;
}

INT FBackend::WaterSurfaceView(ID3D12GraphicsCommandList* Cmd, UBOOL& bWrite)
{
    bWrite = 0;
    const INT View = Textures.GetReservedIndex(WaterSurfaceUavView);
    // The water itself reads it in every mode - which of its own points is the nearest at a pixel (the solid pass's
    // two goes over a water run). After the pass only a temporal upscaler does, and the motion vectors are drawn for
    // one alone: for those it is live under one only.
    if (EnsureWaterSurface())
    {
        if (!bWaterSurfaceCleared)
        {
            WaterSurfaceTo(Cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const FLOAT Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            Cmd->ClearRenderTargetView(WaterSurfaceRtvHeap->GetCPUDescriptorHandleForHeapStart(), Zero, 0, nullptr);
            bWaterSurfaceCleared = 1;
            bWaterSurfaceLive    = UpscaleMode ? 1 : 0;
        }
        WaterSurfaceTo(Cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // With the shadows, the shade beside it (u6): not cleared - the apply pass reads it where the depth says the
        // water is, and each such pixel's nearest point wrote it.
        if (bShadows && EnsureWaterShade())
        {
            WaterShadeTo(Cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            bWaterShadeLive = 1;
        }
        else
            WaterShadeViewNone();
        bWrite = 1;
        return View;
    }
    WaterShadeViewNone();
    // Bound all the same, and so a view the run can hold without writing it: its own, or one of nothing.
    if (!bWaterSurfaceViewReal)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
        appMemzero(&Ud, sizeof(Ud));
        Ud.Format        = DXGI_FORMAT_R32_UINT;
        Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Device->CreateUnorderedAccessView(nullptr, nullptr, &Ud, Textures.GetCpuHandle(View));
    }
    return View;
}

void FBackend::WaterSurfaceSettle(ID3D12GraphicsCommandList* Cmd)
{
    if (!WaterSurface || !Cmd)
        return;
    // The shade with it: a run's points that are drawn write it with plain stores, and the run before may have written
    // the same pixel - its stores land before this run's.
    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    B[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    B[0].UAV.pResource = WaterSurface.Get();
    B[1]               = B[0];
    B[1].UAV.pResource = WaterShade.Get();
    Cmd->ResourceBarrier((bWaterShadeLive && WaterShade) ? 2u : 1u, B);
}

INT FBackend::WetMarkView(ID3D12GraphicsCommandList* Cmd)
{
    const INT View = Textures.GetReservedIndex(WetMarkUav);
    if (EnsureWetMark())
    {
        if (!bWetMarkCleared)
        {
            WetMarkTo(Cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const FLOAT Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            Cmd->ClearRenderTargetView(WetMarkRtvHeap->GetCPUDescriptorHandleForHeapStart(), Zero, 0, nullptr);
            bWetMarkCleared = 1;
        }
        WetMarkTo(Cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    else if (!bWetMarkViewReal)
    {
        // Bound all the same: a view of nothing, which takes no write.
        D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
        appMemzero(&Ud, sizeof(Ud));
        Ud.Format        = DXGI_FORMAT_R32_UINT;
        Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Device->CreateUnorderedAccessView(nullptr, nullptr, &Ud, Textures.GetCpuHandle(View));
    }
    // The table the run binds starts at the water's own depth's view; the mark's is WetMarkUav's distance after it.
    return Textures.GetReservedIndex(WaterSurfaceUavView);
}

UBOOL FBackend::EnsureWetMark()
{
    if (WetMark && WetMarkRtvHeap)
        return 1;
    if (!Device || !Textures.GetHeap() || SceneWidth <= 0 || SceneHeight <= 0)
        return 0;
    static INT Told = 0;
    if (!WetMarkRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 1;
        if (FAILED(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), WetMarkRtvHeap.VoidSlot())))
        {
            if (Told++ < 3)
                RdxLog(TEXT("wet floors: their mark's render target view's heap would not be made"));
            return 0;
        }
    }
    // Typeless: cleared through a float view of the same bits (zero is zero either way), written and read as a whole
    // number.
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = (UINT64)SceneWidth * 2;     // a pixel's mark at its even column, its relief's normal beside it
    Desc.Height           = SceneHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Format           = DXGI_FORMAT_R32_TYPELESS;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format = DXGI_FORMAT_R32_FLOAT;
    WetMark.Release();
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               &Clear, __uuidof(ID3D12Resource), WetMark.VoidSlot())))
    {
        WetMark.Release();
        if (Told++ < 3)
            RdxLog(TEXT("wet floors: their %ix%i mark would not be made - they are drawn wet, with no reflection"),
                   SceneWidth, SceneHeight);
        return 0;
    }
    WetMarkState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RENDER_TARGET_VIEW_DESC Rd;
    appMemzero(&Rd, sizeof(Rd));
    Rd.Format        = DXGI_FORMAT_R32_FLOAT;
    Rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    Device->CreateRenderTargetView(WetMark.Get(), &Rd, WetMarkRtvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
    appMemzero(&Ud, sizeof(Ud));
    Ud.Format        = DXGI_FORMAT_R32_UINT;
    Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Device->CreateUnorderedAccessView(WetMark.Get(), nullptr, &Ud, Textures.GetCpuHandle(Textures.GetReservedIndex(WetMarkUav)));
    D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
    appMemzero(&Sd, sizeof(Sd));
    Sd.Format                  = DXGI_FORMAT_R32_UINT;
    Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Sd.Texture2D.MipLevels     = 1;
    Device->CreateShaderResourceView(WetMark.Get(), &Sd, Textures.GetCpuHandle(Textures.GetReservedIndex(WetMarkSrv)));
    bWetMarkViewReal = 1;
    RdxLog(TEXT("wet floors: their mark ready, %ix%i - the nearest wet floor at each pixel and its relief's normal, for ")
           TEXT("the late reflection pass"), SceneWidth * 2, SceneHeight);
    return 1;
}

// Its pipelines carry the picture's format, the depth's and the sample count: where one of them moved since they were
// made - the HDR key, the shadows on or off, the multisampling, each changed after waiting for the GPU, so nothing in
// flight still draws with them - they are made again. Drawn with the old ones, every pixel of the pass would be
// written in the wrong format (D3D12 ERROR #613).
UBOOL FBackend::EnsureWetReflect()
{
    const DXGI_FORMAT WetDepth = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    if (WetReflectReady != 0 &&
        (WetReflectFor[0] != Colours.Draw || WetReflectFor[1] != WetDepth || WetReflectSamples != MsaaCount))
    {
        WetReflect.Shutdown();
        WetReflectReady = 0;
        RdxLog(TEXT("wet floors: the reflection pass made again - the picture's or the depth's format, or the samples, "
                    "moved"));
    }
    if (WetReflectReady == 0 && Device)
    {
        WetReflectReady   = WetReflect.Init(Device.Get(), Colours.Draw, RDX_LIGHT_TARGET_FORMAT, WetDepth, MsaaCount)
                                ? 1 : -1;
        WetReflectFor[0]  = Colours.Draw;
        WetReflectFor[1]  = WetDepth;
        WetReflectSamples = MsaaCount;
    }
    return WetReflectReady > 0;
}

// Every shader the renderer may ask for, compiled while it starts, so that no level's load and no change in the menu
// waits on the compiler (Laci's rule: every shader at the start, none later). The passes made at their first use are
// made now, by their own flags where they have them; the passes of the modes not running - HDR, the picture's
// scaling, the upscaling, FXAA and SMAA, the occlusion, the shadows' application and their maps, the rays' shadows'
// smoothing, the rays' far probe, the motion vectors and the measuring views - are made now and left unused, their shaders kept; and the
// builds that differ by the shadows - the solid pass's (bStencilKinds) and the depth's resolve with the multisampling -
// are made for the other way too, then made again as the shadows are, that second time from the shader cache. A pass
// whose own flag is left alone is made again where it is first used, from the cache. With the cache whole none of this
// compiles anything; after a new renderer or without the cache this is where the time goes, once.
void FBackend::WarmShaders()
{
    ID3D12Device* const D = Device.Get();
    if (!D)
        return;
    const DXGI_FORMAT DepthFormat = bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    // The passes made at their first use.
    if (BounceReady == 0)
        BounceReady = Bounce.Init(D) ? 1 : -1;
    if (DepthNearerReady == 0)
        DepthNearerReady = Final.InitDepthNearer(D) ? 1 : -1;
    if (FogRoomsReady == 0)
        FogRoomsReady = FogRoom.Init(D) ? 1 : -1;
    if (FieldReady == 0)
        FieldReady = Field.Init(D) ? 1 : -1;
    if (DofReady == 0)
        DofReady = Dof.Init(D) ? 1 : -1;
    WalkReady();
    Volume.WarmHaze(D, Colours.Draw);
    EnsureMirrorProbe();
    EnsureWetReflect();
    if (MotionObjReady == 0)
    {
        MotionObjReady  = Final.InitMotionObjects(D, DepthFormat, bMotionNoDepth) ? 1 : -1;
        MotionObjFormat = DepthFormat;
    }
    Final.InitWaterStamp(D, DepthFormat);
    EnsureSplashes();
    EnsureRipples();
    EnsureCaustics();
    EnsureCausticLay(Colours.Draw, (MsaaCount > 1) ? (UINT)MsaaSampleCount() : 1u);
    // The modes not running: their passes made, left unused.
    Final.InitHdr(D);
    Final.InitBox(D);
    Final.InitUpscale(D);
    Final.InitFxaa(D);
    Final.InitSmaa(D);
    Final.InitAo(D);
    Final.InitShadow(D, Colours.Draw);
    Final.InitWaterPack(D);
    Final.InitRtFactorBlur(D);
    Final.InitMotion(D);
    Final.InitDepth(D);
    Final.InitMotionView(D);
    Final.InitCaption(D);
    FShadowMaps::WarmShaders();
    FRtPass::WarmShaders();
    // The builds that differ by the shadows: the other way first, then this one again.
    Final.InitMsaaDepth(D, 4, !bShadows);
    Final.InitMsaaDepth(D, 4, bShadows);
    if (MsaaCount > 1)
        Final.InitMsaaDepth(D, MsaaCount, bShadows);
    Solid.SetStencilKinds(!bShadows);
    Solid.RebuildPipelines(D);
    Solid.SetStencilKinds(bShadows);
    if (!Solid.RebuildPipelines(D))
        RDX_LOG("shaders: the geometry pass's pipelines could not be made again after the other build's");
}

UBOOL FBackend::DrawWetReflections(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    // Only where a wet floor was drawn this frame - its first run cleared the mark - and the view had its place. The
    // mark readable from here on whatever this pass does: the wet floors' layer on rays reads it too.
    bWetDrawnNow = 0;
    if (!bWetMarkCleared || !WetMark || !Cmd)
        return 0;
    WetMarkTo(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // At the Default reflections the wet floors give nothing back: their darkened stone is the solid pass's own.
    if (!bWetBlockNow || !Textures.GetHeap() || ReflectionLevel == 0)
        return 0;
    // The lamps' slot is the pass's constant buffer: without it there is no pass (the frame's start made it).
    if (!EnsureWetReflect() || !EnsureWetLamps())
        return 0;
    BindSceneTargets(Cmd, Dsv);
    FLOAT C[FWetReflect::CONSTANTS];
    appMemcpy(C, WetBlockNow, sizeof(WetBlockNow));
    FLOAT* const More = C + RdxMath::WetBlockFloats;
    More[0] = (FLOAT)SceneWidth;
    More[1] = (FLOAT)SceneHeight;
    More[2] = 1.0f / (FLOAT)Max(SceneWidth, 1);
    More[3] = 1.0f / (FLOAT)Max(SceneHeight, 1);
    More[4] = RdxMath::WetFilmShare;
    More[5] = RdxMath::WetDampSpread;
    More[6] = RdxMath::WetStreakStrength;
    More[7] = RdxMath::WetStreakRough;
    More[8] = RdxMath::WetStreakPuddleRough;
    More[9] = More[10] = More[11] = 0.0f;
    // Whether the water's image is the rays' this frame (the layer on rays, AddWetLayer): then this pass leaves it.
    More[12] = (bWetLayerNow && IsRtMirrorRunning()) ? 1.0f : 0.0f;
    More[13] = WetRainNow[0];
    More[14] = WetRainNow[1];
    // The wet floors' image by the level: none (Low), coarse (Medium), the full trace.
    More[15] = RdxMath::ReflectionsWetImage(ReflectionLevel) ? (RdxMath::ReflectionsWetCoarse(ReflectionLevel) ? 1.0f : 2.0f)
                                                              : 0.0f;
    // The own lamps - the flashlight, the weapon lamps - for their glints (WetReflect.hlsl's WetSpots): each one's lens
    // in the view's space as the wet floors' view has it, the glass of FBeamBounce::LampRadius, its light as its cone
    // throws it - the cone's own numbers, as the solid pass's own lamps - and its axis. The lens as bright as that
    // light makes it: the cone's light at a distance d is its colour by BeamHalfDist squared over d squared, a ball of
    // the lens's radius gives that at a brightness of that over pi r squared.
    {
        enum { SPOT_ROWS = 2 + 3 * RdxMath::OwnLampMost };
        FLOAT* const S = GrassBlock(SPOT_ROWS, WetSpotsNow);
        if (S)
        {
            appMemzero(S, SPOT_ROWS * 4 * sizeof(FLOAT));
            const FLOAT* const V = WetBlockNow;          // the view's eye, then its axes: right, down, forward
            const FLOAT        R = FBeamBounce::LampRadius;
            const FLOAT        Bright = RdxMath::BeamHalfDist * RdxMath::BeamHalfDist / (PI * R * R);
            INT                N = 0;
            for (INT i = 0; i < OwnLampCount && i < (INT)RdxMath::OwnLampMost; i++)
            {
                const FOwnLamp& L = OwnLamps[i];
                const FLOAT     Q[3] = { L.At[0] - V[0], L.At[1] - V[1], L.At[2] - V[2] };
                FLOAT* const    O = S + 8 + 12 * N++;
                for (INT a = 0; a < 3; a++)
                {
                    O[a]     = Q[0] * V[4 * (a + 1)] + Q[1] * V[4 * (a + 1) + 1] + Q[2] * V[4 * (a + 1) + 2];
                    O[8 + a] = L.Aim[0] * V[4 * (a + 1)] + L.Aim[1] * V[4 * (a + 1) + 1] + L.Aim[2] * V[4 * (a + 1) + 2];
                    O[4 + a] = L.Colour[a] * Bright;
                }
                O[3] = R;
            }
            S[0] = (FLOAT)N;
            S[1] = RdxMath::BeamSpillShare;
            S[4] = RdxMath::BeamRimCos;
            S[5] = 1.0f / (RdxMath::BeamHotCos - RdxMath::BeamRimCos);
            S[6] = RdxMath::BeamSpillRimCos;
            S[7] = 1.0f / (RdxMath::BeamSpillHotCos - RdxMath::BeamSpillRimCos);
        }
    }
    if (!WetSpotsNow)
        return 0;
    // The targets BindSceneTargets bound: three with the shadows' where it found them, else the picture alone.
    const UBOOL bMsaa  = MsaaWorldLive();
    const UBOOL bThree = (bShadows && (bMsaa ? (MsaaLight && MsaaAdded)
                                             : (LightTarget && AddedTarget && LightRtvHeap))) ? 1 : 0;
    WetReflect.Draw(Cmd, Textures.GetHeap(), C, Textures.GetGpuHandle(WaterSlot(0, WaterViewPicture)),
                    Textures.GetGpuHandle(WaterSlot(0, WaterViewDepth)),
                    Textures.GetGpuHandle(Textures.GetReservedIndex(WetMarkSrv)),
                    Textures.GetGpuHandle(Textures.GetReservedIndex(WetCoverSrv)),
                    WetLampBuffer->GetGPUVirtualAddress() + (UINT64)FrameIndex * RdxMath::WetLampBytes, WetSpotsNow,
                    bThree, bMsaa);
    // Kept for the second draw, the fires' flames, after the volumetric walk (DrawVolumetrics).
    appMemcpy(WetConstNow, C, sizeof(C));
    bWetDrawnNow = 1;
    bWetMsaaNow  = bMsaa;
    return 1;
}

void FBackend::SetWetCover(const FLOAT* Cells, INT W, INT H, FLOAT X, FLOAT Y, FLOAT Cell)
{
    for (INT c = 0; c < 4; c++)
        WetCoverNow[c] = 0.0f;
    if (!Device || !Textures.GetHeap())
        return;
    const INT  Pair = 1 - WetCoverPair;
    const UINT Row  = (UINT)W * 4 * sizeof(FLOAT);
    const UINT Pitch = (Row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UBOOL      bMade = 0;
    if (Cells && W > 0 && H > 0 && Cell > 0.0f && bInFrame && CommandList)
    {
        // The pair two levels old goes; this level's is made in its place.
        WetCoverTex[Pair].Release();
        WetCoverUp[Pair].Release();
        D3D12_HEAP_PROPERTIES Heap;
        appMemzero(&Heap, sizeof(Heap));
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC Desc;
        appMemzero(&Desc, sizeof(Desc));
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = (UINT64)W;
        Desc.Height           = (UINT)H;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.SampleDesc.Count = 1;
        Desc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D12_HEAP_PROPERTIES Up;
        appMemzero(&Up, sizeof(Up));
        Up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC UpDesc;
        appMemzero(&UpDesc, sizeof(UpDesc));
        UpDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        UpDesc.Width            = (UINT64)Pitch * (UINT64)H;
        UpDesc.Height           = 1;
        UpDesc.DepthOrArraySize = 1;
        UpDesc.MipLevels        = 1;
        UpDesc.SampleDesc.Count = 1;
        UpDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        BYTE*       Mapped = nullptr;
        D3D12_RANGE NoRead = { 0, 0 };
        if (SUCCEEDED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr, __uuidof(ID3D12Resource), WetCoverTex[Pair].VoidSlot())) &&
            SUCCEEDED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &UpDesc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      __uuidof(ID3D12Resource), WetCoverUp[Pair].VoidSlot())) &&
            SUCCEEDED(WetCoverUp[Pair]->Map(0, &NoRead, (void**)&Mapped)) && Mapped)
        {
            for (INT y = 0; y < H; y++)
                appMemcpy(Mapped + (SIZE_T)Pitch * y, Cells + (SIZE_T)y * W * 4, Row);
            WetCoverUp[Pair]->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION Dst;
            appMemzero(&Dst, sizeof(Dst));
            Dst.pResource        = WetCoverTex[Pair].Get();
            Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            Dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION Src;
            appMemzero(&Src, sizeof(Src));
            Src.pResource                          = WetCoverUp[Pair].Get();
            Src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            Src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R32G32B32A32_FLOAT;
            Src.PlacedFootprint.Footprint.Width    = (UINT)W;
            Src.PlacedFootprint.Footprint.Height   = (UINT)H;
            Src.PlacedFootprint.Footprint.Depth    = 1;
            Src.PlacedFootprint.Footprint.RowPitch = Pitch;
            CommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
            D3D12_RESOURCE_BARRIER B;
            appMemzero(&B, sizeof(B));
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = WetCoverTex[Pair].Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &B);
            WetCoverPair  = Pair;
            WetCoverNow[0] = X;
            WetCoverNow[1] = Y;
            WetCoverNow[2] = 1.0f / (Cell * (FLOAT)W);
            WetCoverNow[3] = 1.0f / (Cell * (FLOAT)H);
            bMade          = 1;
        }
        else
        {
            WetCoverTex[Pair].Release();
            WetCoverUp[Pair].Release();
            RdxLog(TEXT("wet floors: the rain's cover's %ix%i texture would not be made - everywhere open"), W, H);
        }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
    appMemzero(&Sd, sizeof(Sd));
    Sd.Format                  = DXGI_FORMAT_R32G32B32A32_FLOAT;
    Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Sd.Texture2D.MipLevels     = 1;
    Device->CreateShaderResourceView(bMade ? WetCoverTex[WetCoverPair].Get() : nullptr, &Sd,
                                     Textures.GetCpuHandle(Textures.GetReservedIndex(WetCoverSrv)));
    bWetCoverViewSet = 1;
}

void FBackend::WetMarkTo(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES State)
{
    if (!WetMark || WetMarkState == State)
        return;
    D3D12_RESOURCE_BARRIER B;
    SplashBarrier(B, WetMark.Get(), WetMarkState, State);
    Cmd->ResourceBarrier(1, &B);
    WetMarkState = State;
}

// The view the water's runs bind at u6 where the shade is not made: one of nothing, which takes no write.
void FBackend::WaterShadeViewNone()
{
    if (bWaterShadeViewReal || !Textures.GetHeap())
        return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
    appMemzero(&Ud, sizeof(Ud));
    Ud.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Device->CreateUnorderedAccessView(nullptr, nullptr, &Ud, Textures.GetCpuHandle(Textures.GetReservedIndex(WaterShadeUav)));
}

UBOOL FBackend::EnsureWaterShade()
{
    if (WaterShade)
        return 1;
    if (bWaterShadeFailed || !Device || !Textures.GetHeap() || SceneWidth <= 0 || SceneHeight <= 0)
        return 0;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = SceneWidth;
    Desc.Height           = SceneHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               nullptr, __uuidof(ID3D12Resource), WaterShade.VoidSlot())))
    {
        WaterShade.Release();
        bWaterShadeFailed = 1;      // until the sizes change
        RdxLog(TEXT("water shade: the %ix%i texture would not be made - the see-through water takes the shadow of what ")
               TEXT("lies under it"), SceneWidth, SceneHeight);
        return 0;
    }
    WaterShadeState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
    appMemzero(&Ud, sizeof(Ud));
    Ud.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Device->CreateUnorderedAccessView(WaterShade.Get(), nullptr, &Ud,
                                      Textures.GetCpuHandle(Textures.GetReservedIndex(WaterShadeUav)));
    bWaterShadeViewReal    = 1;
    bWaterShadowViewsDirty = 1;
    RdxLog(TEXT("water shade: ready, %ix%i - the see-through water's shadows: its surface's own, and the bed's where ")
           TEXT("it is seen through it"), SceneWidth, SceneHeight);
    return 1;
}

void FBackend::WaterShadeTo(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES State)
{
    if (!WaterShade || !Cmd || WaterShadeState == State)
        return;
    D3D12_RESOURCE_BARRIER B;
    SplashBarrier(B, WaterShade.Get(), WaterShadeState, State);
    Cmd->ResourceBarrier(1, &B);
    WaterShadeState = State;
}

UBOOL FBackend::EnsureWaterFactor()
{
    // The pack at the factor's size over WaterPackCoarse; the water surface's factor over it by the factor's own Scale.
    const INT S  = Max(1, (INT)(UpscaleMode ? ShadowFactorDivisor() : (SceneScale * ShadowFactorDivisor())));
    const INT PW = (ShadowW + WaterPackCoarse - 1) / WaterPackCoarse;
    const INT PH = (ShadowH + WaterPackCoarse - 1) / WaterPackCoarse;
    const INT FW = (PW + S - 1) / S, FH = (PH + S - 1) / S;
    if (WaterFactor && WaterPack && WaterFactorW == FW && WaterFactorH == FH && WaterPackW == PW && WaterPackH == PH)
        return 1;
    WaterFactor.Release();
    WaterPack.Release();
    WaterFactorW = WaterFactorH = WaterPackW = WaterPackH = 0;
    if (!Device || ShadowW <= 0 || ShadowH <= 0)
        return 0;
    static INT Told = 0;
    if (!WaterFactorRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 2;
        if (FAILED(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), WaterFactorRtvHeap.VoidSlot())))
        {
            if (Told++ < 3)
                RdxLog(TEXT("water shadow: its factor's render target view's heap would not be made"));
            return 0;
        }
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = FW;
    Desc.Height           = FH;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Format           = DXGI_FORMAT_R8_UNORM;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_DESC PackDesc = Desc;
    PackDesc.Width  = PW;
    PackDesc.Height = PH;
    PackDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               nullptr, __uuidof(ID3D12Resource), WaterFactor.VoidSlot())) ||
        FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &PackDesc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                               __uuidof(ID3D12Resource), WaterPack.VoidSlot())))
    {
        WaterFactor.Release();
        WaterPack.Release();
        if (Told++ < 3)
            RdxLog(TEXT("water shadow: the %ix%i factor or its %ix%i pack would not be made - the see-through water ")
                   TEXT("takes the shadow of what lies under it"), FW, FH, PW, PH);
        return 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = WaterFactorRtvHeap->GetCPUDescriptorHandleForHeapStart();
    Device->CreateRenderTargetView(WaterFactor.Get(), nullptr, Rtv);
    Rtv.ptr += Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    Device->CreateRenderTargetView(WaterPack.Get(), nullptr, Rtv);
    WaterFactorW           = FW;
    WaterFactorH           = FH;
    WaterPackW             = PW;
    WaterPackH             = PH;
    bWaterShadowViewsDirty = 1;
    RdxLog(TEXT("water shadow: the surface's factor %ix%i, over its %ix%i pack"), FW, FH, PW, PH);
    return 1;
}

// The two runs of ShadowSrvHeap the water's shadow reads (SHADOW_WATER_RUN): written whole from what is there, a view
// of nothing for what is not - so neither pass ever reads a view of something gone.
void FBackend::WriteWaterShadowViews()
{
    if (!ShadowSrvHeap || !Device)
        return;
    bWaterShadowViewsDirty = 0;
    const UINT Inc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto Plain = [](DXGI_FORMAT Format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC V;
        appMemzero(&V, sizeof(V));
        V.Format                  = Format;
        V.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        V.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        V.Texture2D.MipLevels     = 1;
        return V;
    };
    const D3D12_SHADER_RESOURCE_VIEW_DESC Factor = Plain(DXGI_FORMAT_R8_UNORM);
    const D3D12_SHADER_RESOURCE_VIEW_DESC Added  = Plain(Colours.Draw);
    const D3D12_SHADER_RESOURCE_VIEW_DESC Water  = Plain(DXGI_FORMAT_R32_FLOAT);
    const D3D12_SHADER_RESOURCE_VIEW_DESC Depth  = Plain(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
    const D3D12_SHADER_RESOURCE_VIEW_DESC Kind   = Plain(DXGI_FORMAT_R8G8_UINT);
    const D3D12_SHADER_RESOURCE_VIEW_DESC Shade  = Plain(DXGI_FORMAT_R16G16B16A16_FLOAT);
    D3D12_SHADER_RESOURCE_VIEW_DESC Cube;
    appMemzero(&Cube, sizeof(Cube));
    const UBOOL bCubes = Shadow.IsReady() && Shadow.GetWorldCubes() && Shadow.GetActorCubes();
    if (bCubes)
        Shadow.DescribeCubeView(Cube);
    else
    {
        Cube.Format                     = DXGI_FORMAT_R32_FLOAT;
        Cube.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        Cube.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Cube.TextureCubeArray.MipLevels = 1;
        Cube.TextureCubeArray.NumCubes  = 1;
    }
    for (INT t = 0; t < 2; t++)
    {
        ID3D12Resource* const Own   = (t == 0) ? DepthBuffer.Get() : DepthBuffer2.Get();
        ID3D12Resource* const Other = (t == 0) ? DepthBuffer2.Get() : DepthBuffer.Get();
        struct FView { ID3D12Resource* Res; const D3D12_SHADER_RESOURCE_VIEW_DESC* Desc; };
        const FView Run[SHADOW_WATER_RUN_VIEWS] = {
            { ShadowColor.Get(), &Factor }, { AddedTarget.Get(), &Added }, { WaterSurface.Get(), &Water },
            { Other, &Depth }, { nullptr, &Kind }, { WaterShade.Get(), &Shade },
            { bCubes ? Shadow.GetWorldCubes() : nullptr, &Cube }, { bCubes ? Shadow.GetActorCubes() : nullptr, &Cube },
            { WaterFactor.Get(), &Factor }, { Own, &Depth } };
        D3D12_CPU_DESCRIPTOR_HANDLE S = ShadowSrvHeap->GetCPUDescriptorHandleForHeapStart();
        S.ptr += (SIZE_T)(SHADOW_WATER_RUN + t * SHADOW_WATER_RUN_VIEWS) * Inc;
        for (INT i = 0; i < SHADOW_WATER_RUN_VIEWS; i++)
        {
            Device->CreateShaderResourceView(Run[i].Res, Run[i].Desc, S);
            S.ptr += Inc;
        }
    }
    // The water surface factor's table, and the pack's two. The pack's light is its second float, read as the first.
    const D3D12_SHADER_RESOURCE_VIEW_DESC Pack = Plain(DXGI_FORMAT_R32G32_FLOAT);
    D3D12_SHADER_RESOURCE_VIEW_DESC PackLight = Pack;
    PackLight.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, 1, 1, 1);
    struct FView { ID3D12Resource* Res; const D3D12_SHADER_RESOURCE_VIEW_DESC* Desc; };
    const FView Table[8] = {
        { WaterPack.Get(), &Pack }, { nullptr, &Water }, { nullptr, &Kind }, { WaterPack.Get(), &PackLight },
        { bCubes ? Shadow.GetWorldCubes() : nullptr, &Cube }, { bCubes ? Shadow.GetActorCubes() : nullptr, &Cube },
        { WaterSurface.Get(), &Water }, { WaterShade.Get(), &Shade } };
    D3D12_CPU_DESCRIPTOR_HANDLE S = ShadowSrvHeap->GetCPUDescriptorHandleForHeapStart();
    S.ptr += (SIZE_T)SHADOW_WATER_FACTOR_TABLE * Inc;
    for (INT i = 0; i < 8; i++)
    {
        Device->CreateShaderResourceView(Table[i].Res, Table[i].Desc, S);
        S.ptr += Inc;
    }
}

UBOOL FBackend::EnsureWaterSurface()
{
    if (WaterSurface && WaterSurfaceRtvHeap)
        return 1;
    if (!Device || !Textures.GetHeap() || SceneWidth <= 0 || SceneHeight <= 0)
        return 0;
    static INT Told = 0;
    if (!WaterSurfaceRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 1;
        if (FAILED(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap),
                                                WaterSurfaceRtvHeap.VoidSlot())))
        {
            if (Told++ < 3)
                RdxLog(TEXT("water depth: its render target view's heap would not be made"));
            return 0;
        }
    }
    // Typeless: cleared through a float view, written through a whole-number one (an atomic keeps the nearer), read
    // through a float one again - the same bits all three ways, as the water writes the float's bits.
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = SceneWidth;
    Desc.Height           = SceneHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Format           = DXGI_FORMAT_R32_TYPELESS;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format = DXGI_FORMAT_R32_FLOAT;
    WaterSurface.Release();
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               &Clear, __uuidof(ID3D12Resource), WaterSurface.VoidSlot())))
    {
        WaterSurface.Release();
        if (Told++ < 3)
            RdxLog(TEXT("water depth: the %ix%i texture would not be made"), SceneWidth, SceneHeight);
        return 0;
    }
    WaterSurfaceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RENDER_TARGET_VIEW_DESC Rd;
    appMemzero(&Rd, sizeof(Rd));
    Rd.Format        = DXGI_FORMAT_R32_FLOAT;
    Rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    Device->CreateRenderTargetView(WaterSurface.Get(), &Rd, WaterSurfaceRtvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
    appMemzero(&Ud, sizeof(Ud));
    Ud.Format        = DXGI_FORMAT_R32_UINT;
    Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Device->CreateUnorderedAccessView(WaterSurface.Get(), nullptr, &Ud,
                                      Textures.GetCpuHandle(Textures.GetReservedIndex(WaterSurfaceUavView)));
    bWaterSurfaceViewReal = 1;
    if (DepthSrvHeap)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
        appMemzero(&Sd, sizeof(Sd));
        Sd.Format                  = DXGI_FORMAT_R32_FLOAT;
        Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Sd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE At = DepthSrvHeap->GetCPUDescriptorHandleForHeapStart();
        At.ptr += (UINT64)WaterSurfaceDepthSrv * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Device->CreateShaderResourceView(WaterSurface.Get(), &Sd, At);
    }
    bWaterShadowViewsDirty = 1;
    RdxLog(TEXT("water depth: ready, %ix%i - the see-through water's own depth: which of its points is the nearest, "
                "and for the motion vectors and the upscaler"), SceneWidth, SceneHeight);
    return 1;
}

void FBackend::WaterSurfaceTo(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES State)
{
    if (!WaterSurface || WaterSurfaceState == State)
        return;
    D3D12_RESOURCE_BARRIER B;
    SplashBarrier(B, WaterSurface.Get(), WaterSurfaceState, State);
    Cmd->ResourceBarrier(1, &B);
    WaterSurfaceState = State;
}

void FBackend::StampWaterSurface(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv)
{
    if (!bWaterSurfaceLive || !WaterSurface || !DepthSrvHeap || !Cmd)
        return;
    if (!Final.InitWaterStamp(Device.Get(), bShadows ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT))
        return;
    WaterSurfaceTo(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Cmd->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    D3D12_RECT     Sc = { 0, 0, SceneWidth, SceneHeight };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    D3D12_GPU_DESCRIPTOR_HANDLE Srv = DepthSrvHeap->GetGPUDescriptorHandleForHeapStart();
    Srv.ptr += (UINT64)WaterSurfaceDepthSrv * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Final.DrawWaterStamp(Cmd, DepthSrvHeap.Get(), Srv);
}

UBOOL FBackend::EnsureSplashes()
{
    if (FluidDepth && FluidThick && FluidSmooth && SplashDropPipeline && SplashCurtainPipeline &&
        SplashSmoothPipeline[0] && SplashSmoothPipeline[1] && SplashConstMapped)
        return EnsureReactive();
    if (bSplashFailed || !Device || !Textures.GetHeap() || SceneWidth <= 0 || SceneHeight <= 0)
        return 0;
    bSplashFailed = 1;          // until everything below has been made

    if (!SplashRoot)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 4;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), SplashRtvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(splashes RTV)")))
            return 0;

        D3D12_DESCRIPTOR_RANGE Ranges[2];
        appMemzero(Ranges, sizeof(Ranges));
        Ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Ranges[0].NumDescriptors     = 2;
        Ranges[0].BaseShaderRegister = 0;
        Ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Ranges[1].NumDescriptors     = 2;
        Ranges[1].BaseShaderRegister = 2;
        D3D12_ROOT_PARAMETER Params[3];
        appMemzero(Params, sizeof(Params));
        Params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[0].Descriptor.ShaderRegister           = 0;
        Params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[1].DescriptorTable.NumDescriptorRanges = 1;
        Params[1].DescriptorTable.pDescriptorRanges   = &Ranges[0];
        Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[2].DescriptorTable.NumDescriptorRanges = 1;
        Params[2].DescriptorTable.pDescriptorRanges   = &Ranges[1];
        Params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC RootDesc;
        appMemzero(&RootDesc, sizeof(RootDesc));
        RootDesc.NumParameters = 3;
        RootDesc.pParameters   = Params;
        TComRef<ID3DBlob> Blob, Error, DropVs, CurtainVs, DropPs, ScreenVs, SmoothXPs, SmoothYPs;
        if (FAILED(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
            !Check(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                               __uuidof(ID3D12RootSignature), SplashRoot.VoidSlot()),
                   TEXT("CreateRootSignature(splashes)")))
            return 0;
        D3D_SHADER_MACRO Macros[5];
        Macros[0].Name       = "RDX_SPLASH_DROPS";
        Macros[0].Definition = RDX_STRINGIFY(RDX_SPLASH_DROPS_VALUE);
        Macros[1].Name       = "RDX_CURTAIN_STREAMS";
        Macros[1].Definition = RDX_STRINGIFY(RDX_CURTAIN_STREAMS_VALUE);
        Macros[2].Name       = "RDX_CURTAIN_STREAM_DROPS";
        Macros[2].Definition = RDX_STRINGIFY(RDX_CURTAIN_STREAM_DROPS_VALUE);
        Macros[3].Name       = "RDX_CURTAIN_SPRAY";
        Macros[3].Definition = RDX_STRINGIFY(RDX_CURTAIN_SPRAY_VALUE);
        Macros[4].Name       = "RDX_CHUTE_STREAM_DROPS";
        Macros[4].Definition = RDX_STRINGIFY(RDX_CHUTE_STREAM_DROPS_VALUE);
        Macros[5].Name       = nullptr;
        Macros[5].Definition = nullptr;
        const SIZE_T Len = strlen(GSplashShader);
        if (FAILED(RdxCompile(GSplashShader, Len, "Splashes", Macros, nullptr, "SplashParticleVS", "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, DropVs.Slot(), Error.Slot())) ||
            FAILED(RdxCompile(GSplashShader, Len, "Splashes", Macros, nullptr, "CurtainParticleVS", "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, CurtainVs.Slot(), Error.Slot())) ||
            FAILED(RdxCompile(GSplashShader, Len, "Splashes", Macros, nullptr, "SplashParticlePS", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, DropPs.Slot(), Error.Slot())) ||
            FAILED(RdxCompile(GSplashShader, Len, "Splashes", Macros, nullptr, "SplashScreenVS", "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ScreenVs.Slot(), Error.Slot())) ||
            FAILED(RdxCompile(GSplashShader, Len, "Splashes", Macros, nullptr, "SplashBlurXPS", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, SmoothXPs.Slot(), Error.Slot())) ||
            FAILED(RdxCompile(GSplashShader, Len, "Splashes", Macros, nullptr, "SplashBlurYPS", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, SmoothYPs.Slot(), Error.Slot())))
        {
            RdxLog(TEXT("water: the splashes' shader failed: %s - no splashes"),
                   Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
            SplashRoot.Release();
            return 0;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
        appMemzero(&Pso, sizeof(Pso));
        Pso.pRootSignature        = SplashRoot.Get();
        Pso.VS.pShaderBytecode    = DropVs->GetBufferPointer();
        Pso.VS.BytecodeLength     = DropVs->GetBufferSize();
        Pso.PS.pShaderBytecode    = DropPs->GetBufferPointer();
        Pso.PS.BytecodeLength     = DropPs->GetBufferSize();
        Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Pso.NumRenderTargets      = 2;
        Pso.RTVFormats[0]         = DXGI_FORMAT_R32_FLOAT;
        Pso.RTVFormats[1]         = DXGI_FORMAT_R16_FLOAT;
        Pso.SampleDesc.Count      = 1;
        Pso.SampleMask            = UINT_MAX;
        Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        Pso.RasterizerState.DepthClipEnable = TRUE;
        // The nearest drop's depth stays; the thicknesses add up.
        Pso.BlendState.IndependentBlendEnable = TRUE;
        for (INT t = 0; t < 2; t++)
        {
            D3D12_RENDER_TARGET_BLEND_DESC& T = Pso.BlendState.RenderTarget[t];
            T.BlendEnable           = TRUE;
            T.SrcBlend              = D3D12_BLEND_ONE;
            T.DestBlend             = D3D12_BLEND_ONE;
            T.BlendOp               = t ? D3D12_BLEND_OP_ADD : D3D12_BLEND_OP_MIN;
            T.SrcBlendAlpha         = D3D12_BLEND_ONE;
            T.DestBlendAlpha        = D3D12_BLEND_ONE;
            T.BlendOpAlpha          = t ? D3D12_BLEND_OP_ADD : D3D12_BLEND_OP_MIN;
            T.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        if (!Check(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), SplashDropPipeline.VoidSlot()),
                   TEXT("CreateGraphicsPipelineState(splash drops)")))
            return 0;
        // A fountain's drops: the same targets and the same drop, from a vertex shader of their own.
        Pso.VS.pShaderBytecode = CurtainVs->GetBufferPointer();
        Pso.VS.BytecodeLength  = CurtainVs->GetBufferSize();
        if (!Check(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState),
                                                       SplashCurtainPipeline.VoidSlot()),
                   TEXT("CreateGraphicsPipelineState(fountain drops)")))
            return 0;
        Pso.VS.pShaderBytecode = ScreenVs->GetBufferPointer();
        Pso.VS.BytecodeLength  = ScreenVs->GetBufferSize();
        Pso.PS.pShaderBytecode = SmoothXPs->GetBufferPointer();
        Pso.PS.BytecodeLength  = SmoothXPs->GetBufferSize();
        Pso.NumRenderTargets   = 1;
        Pso.RTVFormats[0]      = DXGI_FORMAT_R32G32_FLOAT;
        Pso.RTVFormats[1]      = DXGI_FORMAT_UNKNOWN;
        appMemzero(&Pso.BlendState, sizeof(Pso.BlendState));
        for (INT t = 0; t < 3; t++)
            Pso.BlendState.RenderTarget[t].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (!Check(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), SplashSmoothPipeline[0].VoidSlot()),
                   TEXT("CreateGraphicsPipelineState(splash smoothing across)")))
            return 0;
        Pso.PS.pShaderBytecode = SmoothYPs->GetBufferPointer();
        Pso.PS.BytecodeLength  = SmoothYPs->GetBufferSize();
        Pso.NumRenderTargets   = 3;
        Pso.RTVFormats[0]      = DXGI_FORMAT_R32_FLOAT;
        Pso.RTVFormats[1]      = DXGI_FORMAT_R16_FLOAT;
        Pso.RTVFormats[2]      = DXGI_FORMAT_R8_UNORM;
        // The reactive mask keeps the larger of the splashes' and what the see-through water wrote before them
        // (ReactiveBegin); the two others are written as they are.
        Pso.BlendState.IndependentBlendEnable = TRUE;
        {
            D3D12_RENDER_TARGET_BLEND_DESC& R = Pso.BlendState.RenderTarget[2];
            R.BlendEnable    = TRUE;
            R.SrcBlend       = D3D12_BLEND_ONE;
            R.DestBlend      = D3D12_BLEND_ONE;
            R.BlendOp        = D3D12_BLEND_OP_MAX;
            R.SrcBlendAlpha  = D3D12_BLEND_ONE;
            R.DestBlendAlpha = D3D12_BLEND_ONE;
            R.BlendOpAlpha   = D3D12_BLEND_OP_MAX;
        }
        if (!Check(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), SplashSmoothPipeline[1].VoidSlot()),
                   TEXT("CreateGraphicsPipelineState(splash smoothing down)")))
            return 0;

        D3D12_HEAP_PROPERTIES Upload;
        appMemzero(&Upload, sizeof(Upload));
        Upload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Buf;
        appMemzero(&Buf, sizeof(Buf));
        Buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Buf.Width            = SplashConstBytes * BACKEND_FRAME_COUNT * 2;      // the world's view's, and a mirror's
        Buf.Height           = 1;
        Buf.DepthOrArraySize = 1;
        Buf.MipLevels        = 1;
        Buf.SampleDesc.Count = 1;
        Buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!Check(Device->CreateCommittedResource(&Upload, D3D12_HEAP_FLAG_NONE, &Buf, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, __uuidof(ID3D12Resource), SplashConstBuffer.VoidSlot()),
                   TEXT("CreateCommittedResource(splash constants)")) ||
            FAILED(SplashConstBuffer->Map(0, nullptr, (void**)&SplashConstMapped)))
        {
            SplashConstBuffer.Release();
            SplashConstMapped = nullptr;
            return 0;
        }
    }

    // The targets, at the scene's size (CreateSceneTarget lets them go with it). The reactive mask the smoothing writes as
    // well is not theirs alone - the see-through water writes it too (EnsureReactive).
    if (!FluidDepth || !FluidThick || !FluidSmooth)
    {
        FluidDepth.Release();
        FluidThick.Release();
        FluidSmooth.Release();
        D3D12_HEAP_PROPERTIES Heap;
        appMemzero(&Heap, sizeof(Heap));
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC Desc;
        appMemzero(&Desc, sizeof(Desc));
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = SceneWidth;
        Desc.Height           = SceneHeight;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.SampleDesc.Count = 1;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const DXGI_FORMAT Formats[3] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R32G32_FLOAT };
        TComRef<ID3D12Resource>* const Targets[3] = { &FluidDepth, &FluidThick, &FluidSmooth };
        const UINT RtvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (INT t = 0; t < 3; t++)
        {
            Desc.Format = Formats[t];
            D3D12_CLEAR_VALUE Clear;
            appMemzero(&Clear, sizeof(Clear));
            Clear.Format   = Formats[t];
            Clear.Color[0] = (t == 0) ? 1.0e6f : 0.0f;      // no drop anywhere: as far as can be
            if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &Clear,
                                                       __uuidof(ID3D12Resource), Targets[t]->VoidSlot()),
                       TEXT("CreateCommittedResource(splash target)")))
                return 0;
            D3D12_CPU_DESCRIPTOR_HANDLE Rtv = SplashRtvHeap->GetCPUDescriptorHandleForHeapStart();
            Rtv.ptr += t * RtvStep;
            Device->CreateRenderTargetView(Targets[t]->Get(), nullptr, Rtv);
        }
        Device->CreateShaderResourceView(FluidSmooth.Get(), nullptr,
                                         Textures.GetCpuHandle(Textures.GetReservedIndex(SplashViewSmooth)));
        Device->CreateShaderResourceView(FluidDepth.Get(), nullptr, Textures.GetCpuHandle(Textures.GetReservedIndex(SplashViewDepth)));
        Device->CreateShaderResourceView(FluidThick.Get(), nullptr, Textures.GetCpuHandle(Textures.GetReservedIndex(SplashViewThick)));
        RdxLog(TEXT("water: splashes ready, %ix%i"), SceneWidth, SceneHeight);
    }
    bSplashFailed = 0;
    return EnsureReactive();
}

void FBackend::AddSplashes(const FLOAT* Falls, INT Count, FLOAT Column)
{
    const DOUBLE Now = appSeconds();
    for (INT i = 0; i < Count; i++)
    {
        const FLOAT* const F = Falls + i * 5;
        FSplash& S = Splashes[SplashNext];
        SplashNext = (SplashNext + 1) % RdxMath::SplashMax;
        S.X        = F[0];
        S.Y        = F[1];
        S.Z        = F[2];
        S.Radius   = Clamp(F[3], 6.0f, 48.0f);
        S.Strength = RdxMath::SplashStrength(F[4], F[3]);
        S.Seed     = (FLOAT)(SplashSeed++ & 0xFFFF);
        S.Column   = (Column > 0.0f) ? Column : 1.0f;
        S.Start    = Now;
    }
}

void FBackend::SetCurtains(const FLOAT* List, INT Count)
{
    CurtainCount = (List && Count > 0) ? Min(Count, (INT)RdxMath::CurtainMax) : 0;
    for (INT i = 0; i < CurtainCount * RdxMath::CurtainFloats; i++)
        Curtains[i] = List[i];
}

UBOOL FBackend::CurtainsDrawn() const
{
    return (appSeconds() - CurtainAble < 0.25) ? 1 : 0;
}

void FBackend::SetSplashView(const FLOAT* Origin, const FLOAT* Axes, FLOAT Focal, FLOAT CentreX, FLOAT CentreY)
{
    for (INT k = 0; k < 3; k++)
        SplashOrigin[k] = Origin[k];
    for (INT k = 0; k < 9; k++)
        SplashAxes[k] = Axes[k];
    SplashFocal   = Focal;
    SplashCentreX = CentreX;
    SplashCentreY = CentreY;
    bSplashView   = 1;
}

// FTargetSwitch: where the world ends, with a splash alive. A fresh copy of the picture and the depth - every water surface
// is in it now - the drops into their depth and thickness, the depth smoothed along with the upscaler's reactive mask, and
// the scene's targets bound again for the geometry pass to lay the surface on.
UBOOL FBackend::DrawSplashes(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv, const FLOAT* Frame,
                             FLOAT* Composite, UBOOL bUnderWater)
{
    if (!bWaterWanted || bOutputLive || !bSplashView || FrameIndex >= BACKEND_FRAME_COUNT)
        return 0;
    const DOUBLE Now = appSeconds();
    // This call's splashes: the ones whose surface the camera is under, or the ones it is above (FTargetSwitch).
    INT Live = 0;
    for (INT i = 0; i < RdxMath::SplashMax; i++)
        if (Now - Splashes[i].Start < RdxMath::SplashLife &&
            (SplashRun || (SplashOrigin[2] < Splashes[i].Z) == (bUnderWater != 0)))
            Live++;
    // The fountains' drops go with the splashes the camera is above: a fountain stands over its pool.
    const INT Falling = bUnderWater ? 0 : CurtainCount;
    if ((!Live && !Falling) || !EnsureSplashes())
        return 0;
    // This frame slot's constants: the view, then the live splashes - their places from the camera, their times from now.
    const INT    Slot = FrameIndex + SplashRun * BACKEND_FRAME_COUNT;
    FLOAT* const C    = (FLOAT*)(SplashConstMapped + Slot * SplashConstBytes);
    appMemzero(C, SplashConstBytes);
    // And the fountains (the shader's CurtainHead and Curtain): the slot's two ends from the camera, the fall's
    // height, a seed, the water's spread, and where the fall is in its turn - a turn is the time a drop takes down,
    // and every drop of a stream goes round in it, one after another; the turns' count goes with it, so that no two
    // turns' drops are scattered alike.
    static_assert((124 + RdxMath::CurtainMax * (12 + RdxMath::ChuteRows * 4)) * 4 <= SplashConstBytes,
                  "the fountains must fit the constants");
    static_assert(RdxMath::ChutePieces == 6 && RdxMath::ChuteRows == 7, "the shader reads six pieces and a row more");
    if (CurtainEpoch <= 0.0)
        CurtainEpoch = Now;
    C[120] = (FLOAT)Falling;
    C[121] = RdxMath::CurtainAmount;
    C[122] = RdxMath::CurtainGravity;
    C[123] = RdxMath::CurtainStartSpeed;
    for (INT i = 0; i < Falling; i++)
    {
        const FLOAT* const F = Curtains + i * RdxMath::CurtainFloats;
        FLOAT* const       Q = C + 124 + i * 12;
        const FLOAT  Loop  = RdxMath::CurtainFallTime(F[5]);
        const DOUBLE Turns = (Now - CurtainEpoch) / Loop;
        const INT    Whole = (INT)Turns;
        Q[0]  = F[0] - SplashOrigin[0];
        Q[1]  = F[1] - SplashOrigin[1];
        Q[2]  = F[4] + F[5] - SplashOrigin[2];
        Q[3]  = F[5];
        Q[4]  = F[2] - SplashOrigin[0];
        Q[5]  = F[3] - SplashOrigin[1];
        Q[7]  = F[7];
        Q[8]  = F[6];
        Q[9]  = (FLOAT)(Turns - (DOUBLE)Whole);
        Q[10] = (FLOAT)(Whole & 0xFFFF);
        Q[11] = Loop;
        // The chute its water comes down to the slot by (the shader's Chute): the pieces as they were handed over,
        // then where the water's round of it stands - every drop on it goes round in the chute's whole time - the
        // round's seconds and the pieces' count.
        const FLOAT* const Way    = F + RdxMath::CurtainOwnFloats;
        FLOAT* const       K      = C + 124 + RdxMath::CurtainMax * 12 + i * RdxMath::ChuteRows * 4;
        const FLOAT        Round  = Way[RdxMath::ChutePieces * 4];
        const DOUBLE       Rounds = (Round > 0.0f) ? (Now - CurtainEpoch) / Round : 0.0;
        for (INT k = 0; k < RdxMath::ChutePieces * 4; k++)
            K[k] = Way[k];
        K[RdxMath::ChutePieces * 4]     = (FLOAT)(Rounds - (DOUBLE)(INT)Rounds);
        K[RdxMath::ChutePieces * 4 + 1] = Round;
        K[RdxMath::ChutePieces * 4 + 2] = Way[RdxMath::ChutePieces * 4 + 1];
        // And the whole of it: how far back from the slot it goes on the level, and how high it climbs there.
        FLOAT Back = 0.0f, Climb = 0.0f;
        for (INT k = 0; k < RdxMath::ChutePieces; k++)
        {
            Back  += Way[k * 4 + 3];
            Climb += Way[k * 4 + 1] * Way[k * 4 + 3];
        }
        K[RdxMath::ChutePieces * 4 + 3] = Back;
        Q[6] = Climb;
    }
    INT N = 0;
    for (INT i = 0; i < RdxMath::SplashMax; i++)
    {
        const FSplash& S = Splashes[i];
        if (Now - S.Start >= RdxMath::SplashLife ||
            (!SplashRun && (SplashOrigin[2] < S.Z) != (bUnderWater != 0)))
            continue;
        FLOAT* const P = C + 24 + N * 8;
        P[0] = S.X - SplashOrigin[0];
        P[1] = S.Y - SplashOrigin[1];
        P[2] = S.Z - SplashOrigin[2];
        P[3] = (FLOAT)(S.Start - Now);
        P[4] = S.Radius;
        P[5] = S.Strength;
        P[6] = S.Seed;
        P[7] = S.Column;
        N++;
    }
    C[0]  = (FLOAT)N;
    C[1]  = 0.0f;
    C[2]  = -950.0f;                  // the engine's usual gravity
    C[3]  = SplashFocal;
    C[4]  = SplashCentreX;
    C[5]  = SplashCentreY;
    C[6]  = Frame[0];
    C[7]  = Frame[1];
    C[8]  = Frame[2];
    C[9]  = Frame[3];
    C[10] = (FLOAT)SceneWidth;
    C[11] = (FLOAT)SceneHeight;
    for (INT k = 0; k < 3; k++)
    {
        C[12 + k] = SplashAxes[k];
        C[16 + k] = SplashAxes[3 + k];
        C[20 + k] = SplashAxes[6 + k];
    }

    // Where on the screen the splashes can be, in the targets' pixels: each one's box - as wide as its spray flies, as high
    // as its column rises - onto the screen. The passes below work inside it and the smoothing's reach around it (Reach
    // pixels a pass); everything else keeps what the clears put there. A box reaching behind the camera takes the screen.
    const FLOAT ToPxX = (FLOAT)SceneWidth * Frame[0] * 0.5f;       // the engine's pixels to the targets'
    const FLOAT ToPxY = (FLOAT)SceneHeight * Frame[1] * 0.5f;
    FLOAT MinX = 1.0e9f, MinY = 1.0e9f, MaxX = -1.0e9f, MaxY = -1.0e9f;
    UBOOL bWhole = 0;
    for (INT i = 0; i < N && !bWhole; i++)
    {
        const FLOAT* const P = C + 24 + i * 8;
        const FLOAT Wide = P[4] + P[5] * P[5] / 500.0f;
        const FLOAT High = P[4] + P[5] * P[5] / 850.0f;
        for (INT k = 0; k < 8 && !bWhole; k++)
        {
            const FLOAT Q[3] = { P[0] + ((k & 1) ? Wide : -Wide), P[1] + ((k & 2) ? Wide : -Wide), P[2] + ((k & 4) ? High : -8.0f) };
            const FLOAT VX = Q[0] * SplashAxes[0] + Q[1] * SplashAxes[1] + Q[2] * SplashAxes[2];
            const FLOAT VY = Q[0] * SplashAxes[3] + Q[1] * SplashAxes[4] + Q[2] * SplashAxes[5];
            const FLOAT VZ = Q[0] * SplashAxes[6] + Q[1] * SplashAxes[7] + Q[2] * SplashAxes[8];
            if (VZ < 4.0f)
            {
                bWhole = 1;
                break;
            }
            const FLOAT SX = (SplashCentreX + VX * SplashFocal / VZ) * ToPxX;
            const FLOAT SY = (SplashCentreY + VY * SplashFocal / VZ) * ToPxY;
            MinX = Min(MinX, SX);
            MaxX = Max(MaxX, SX);
            MinY = Min(MinY, SY);
            MaxY = Max(MaxY, SY);
        }
    }
    // A fountain's box: its curtain, as far off its plane as its water spreads and its spray flies, and that far past
    // the slot's ends.
    for (INT i = 0; i < Falling && !bWhole; i++)
    {
        const FLOAT* const Q = C + 124 + i * 12;
        const FLOAT Dx = Q[4] - Q[0], Dy = Q[5] - Q[1];
        const FLOAT Long = Max((FLOAT)appSqrt(Dx * Dx + Dy * Dy), 1.0f);
        const FLOAT Ux = Dx / Long, Uy = Dy / Long;
        const FLOAT Off = Q[8] + 16.0f;
        for (INT k = 0; k < 8 && !bWhole; k++)
        {
            const FLOAT Al = (k & 1) ? Long + 16.0f : -16.0f;
            const FLOAT Ac = (k & 2) ? Off : -Off;
            const FLOAT P[3] = { Q[0] + Ux * Al + Uy * Ac, Q[1] + Uy * Al - Ux * Ac,
                                 Q[2] + ((k & 4) ? 2.0f : -Q[3] - 2.0f) };
            const FLOAT VX = P[0] * SplashAxes[0] + P[1] * SplashAxes[1] + P[2] * SplashAxes[2];
            const FLOAT VY = P[0] * SplashAxes[3] + P[1] * SplashAxes[4] + P[2] * SplashAxes[5];
            const FLOAT VZ = P[0] * SplashAxes[6] + P[1] * SplashAxes[7] + P[2] * SplashAxes[8];
            if (VZ < 4.0f)
            {
                bWhole = 1;
                break;
            }
            const FLOAT SX = (SplashCentreX + VX * SplashFocal / VZ) * ToPxX;
            const FLOAT SY = (SplashCentreY + VY * SplashFocal / VZ) * ToPxY;
            MinX = Min(MinX, SX);
            MaxX = Max(MaxX, SX);
            MinY = Min(MinY, SY);
            MaxY = Max(MaxY, SY);
        }
    }
    if (bWhole)
    {
        MinX = MinY = 0.0f;
        MaxX = (FLOAT)SceneWidth;
        MaxY = (FLOAT)SceneHeight;
    }
    if (MaxX < 0.0f || MaxY < 0.0f || MinX > (FLOAT)SceneWidth || MinY > (FLOAT)SceneHeight)
    {
        // None of them in view - but the passes stand ready: a fountain's sheet stays left out (CurtainsDrawn).
        if (Falling)
            CurtainAble = Now;
        return 0;
    }
    const INT Reach = 32;
    auto Around = [&](INT Grow) -> D3D12_RECT
    {
        D3D12_RECT R;
        R.left   = Clamp((INT)MinX - Grow, 0, SceneWidth);
        R.top    = Clamp((INT)MinY - Grow, 0, SceneHeight);
        R.right  = Clamp((INT)MaxX + 1 + Grow, 0, SceneWidth);
        R.bottom = Clamp((INT)MaxY + 1 + Grow, 0, SceneHeight);
        return R;
    };
    const D3D12_RECT DropRect   = Around(8);
    const D3D12_RECT AcrossRect = Around(2 * Reach);
    const D3D12_RECT DownRect   = Around(Reach);

    // A fresh copy of the picture and the depth: every water surface is in it now, and the rain.
    GpuTime(Cmd, 16);
    bExtraCopy = 1;
    const UBOOL bCopied = CopyForWater(Cmd, Dsv);
    bExtraCopy = 0;
    if (!bCopied)
        return 0;

    const UINT RtvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv[4];
    Rtv[0] = SplashRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (INT k = 1; k < 4; k++)
    {
        Rtv[k] = Rtv[0];
        Rtv[k].ptr += k * RtvStep;
    }
    D3D12_RESOURCE_BARRIER B[4];

    // The drops.
    SplashBarrier(B[0], FluidDepth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SplashBarrier(B[1], FluidThick.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Cmd->ResourceBarrier(2, B);
    const FLOAT Far[4]  = { 1.0e6f, 0.0f, 0.0f, 0.0f };
    const FLOAT Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    Cmd->ClearRenderTargetView(Rtv[0], Far, 0, nullptr);
    Cmd->ClearRenderTargetView(Rtv[1], Zero, 0, nullptr);
    Cmd->RSSetScissorRects(1, &DropRect);
    Cmd->OMSetRenderTargets(2, Rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* Heaps[1] = { Textures.GetHeap() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(SplashRoot.Get());
    Cmd->SetGraphicsRootConstantBufferView(0, SplashConstBuffer->GetGPUVirtualAddress() +
                                              (UINT64)Slot * SplashConstBytes);
    Cmd->SetGraphicsRootDescriptorTable(1, Textures.GetGpuHandle(WaterSlot(0, WaterViewPicture)));
    Cmd->SetGraphicsRootDescriptorTable(2, Textures.GetGpuHandle(Textures.GetReservedIndex(SplashViewDepth)));
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    if (N > 0)
    {
        Cmd->SetPipelineState(SplashDropPipeline.Get());
        Cmd->DrawInstanced(4, N * RdxMath::SplashParticles, 0, 0);
    }
    if (Falling > 0)
    {
        Cmd->SetPipelineState(SplashCurtainPipeline.Get());
        Cmd->DrawInstanced(4, Falling * RdxMath::CurtainDrops, 0, 0);
        CurtainAble = Now;
    }

    // Smoothed across, from the drops' depth and thickness into the first pass's target...
    SplashBarrier(B[0], FluidDepth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SplashBarrier(B[1], FluidThick.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SplashBarrier(B[2], FluidSmooth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Cmd->ResourceBarrier(3, B);
    Cmd->RSSetScissorRects(1, &AcrossRect);
    Cmd->OMSetRenderTargets(1, &Rtv[2], FALSE, nullptr);
    Cmd->SetGraphicsRootDescriptorTable(2, Textures.GetGpuHandle(Textures.GetReservedIndex(SplashViewDepth)));
    Cmd->SetPipelineState(SplashSmoothPipeline[0].Get());
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
    // ...then down, back into the drops' targets, with the reactive mask.
    SplashBarrier(B[0], FluidSmooth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SplashBarrier(B[1], FluidDepth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SplashBarrier(B[2], FluidThick.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Cmd->ResourceBarrier(3, B);
    // The mask as a target, cleared unless the water wrote it first this frame (ReactiveBegin) - EnsureSplashes made it.
    ReactiveBegin(Cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE Down[3] = { Rtv[0], Rtv[1], ReactiveRtvHeap->GetCPUDescriptorHandleForHeapStart() };
    Cmd->RSSetScissorRects(1, &DownRect);
    Cmd->OMSetRenderTargets(3, Down, FALSE, nullptr);
    Cmd->SetGraphicsRootDescriptorTable(2, Textures.GetGpuHandle(Textures.GetReservedIndex(SplashViewSmooth)));
    Cmd->SetPipelineState(SplashSmoothPipeline[1].Get());
    Cmd->DrawInstanced(3, 1, 0, 0);
    SplashBarrier(B[0], FluidDepth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SplashBarrier(B[1], FluidThick.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Cmd->ResourceBarrier(2, B);
    ReactiveTo(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    GpuTime(Cmd, 17);

    D3D12_RECT Whole;
    Whole.left   = 0;
    Whole.top    = 0;
    Whole.right  = SceneWidth;
    Whole.bottom = SceneHeight;
    Cmd->RSSetScissorRects(1, &Whole);
    BindSceneTargets(Cmd, Dsv);
    bReactiveLive = 1;
    // The laying-on's constants (PSSplash): the view's axes, the focal length and the screen's centre.
    for (INT k = 0; k < FSolidPass::WATER_CONSTANTS; k++)
        Composite[k] = 0.0f;
    for (INT k = 0; k < 3; k++)
    {
        Composite[k]     = SplashAxes[k];
        Composite[4 + k] = SplashAxes[3 + k];
        Composite[8 + k] = SplashAxes[6 + k];
    }
    Composite[3]  = SplashFocal;
    Composite[7]  = SplashCentreX;
    Composite[11] = SplashCentreY;
    // The water's own mirror looks for the screen's centre where a water run has it (the shader's WaterScreen.xy).
    Composite[16] = SplashCentreX;
    Composite[17] = SplashCentreY;
    // And how the water mirrors, as a water run is told (WaterSpare.x, RdxMath::WaterMirrorSwitch).
    Composite[32] = RdxMath::WaterMirrorSwitch(ReflectionLevel);
    // And the nearest fountains, for their water's own look and their slot's light (the shader's CurtainLightAt), in
    // the numbers the view leaves free - 12 to 15 and 18 to 31 - RdxMath::CurtainLitFloats each: the slot's one end
    // from the camera, its other end on the level, the fall's height; none where that height is zero.
    static const INT Free[18] = { 12, 13, 14, 15, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    static_assert(RdxMath::CurtainLit * RdxMath::CurtainLitFloats <= 18, "the lit fountains must fit the numbers");
    for (INT i = 0; i < Min(Falling, (INT)RdxMath::CurtainLit); i++)
    {
        const FLOAT* const Q      = C + 124 + i * 12;
        const FLOAT        Six[6] = { Q[0], Q[1], Q[2], Q[4], Q[5], Q[3] };
        for (INT k = 0; k < RdxMath::CurtainLitFloats; k++)
            Composite[Free[i * RdxMath::CurtainLitFloats + k]] = Six[k];
        // With each, how far back from the slot its chute goes (the shader's WaterSpare.yzw): nought with no chute,
        // and below zero where it lies on the other side of the curtain's plane than the shader's Out points to.
        static_assert(RdxMath::CurtainLit <= 3, "a lit fountain's chute takes one of three numbers");
        const FLOAT* const K    = C + 124 + RdxMath::CurtainMax * 12 + i * RdxMath::ChuteRows * 4;
        const FLOAT        Side = appSin(K[0]) * (Q[4] - Q[0]) - appCos(K[0]) * (Q[5] - Q[1]);
        Composite[33 + i] = (Side < 0.0f) ? -K[RdxMath::ChutePieces * 4 + 3] : K[RdxMath::ChutePieces * 4 + 3];
    }
    return 1;
}

// FTargetSwitch: a mirror's view of the level has drawn its surfaces - its splashes and its fountains' drops, as
// the world's view has them where it ends. DrawSplashes with that view's place, axes, projection and screen in the
// world's place, and with constants of its own: all the live splashes, on whichever side of their water the view's
// eye lies - behind a mirror on the floor it lies under them all. The lamps the surface is lit by stay as the
// world's eye has them.
UBOOL FBackend::DrawMirrorSplashes(ID3D12GraphicsCommandList* Cmd, D3D12_CPU_DESCRIPTOR_HANDLE Dsv,
                                   const FLOAT* Frame, INT Index, FLOAT* Composite)
{
    if (!Frame || Index < 0 || Index >= MirrorEndCount || !bSplashView)
        return 0;
    const FLOAT* const M = MirrorEnds[Index].View;
    if (M[12] <= 1.0f)
        return 0;
    FLOAT Origin[3], Axes[9];
    const FLOAT Focal = SplashFocal, CentreX = SplashCentreX, CentreY = SplashCentreY;
    for (INT k = 0; k < 3; k++)
    {
        Origin[k]       = SplashOrigin[k];
        SplashOrigin[k] = M[k];
    }
    for (INT k = 0; k < 9; k++)
    {
        Axes[k]       = SplashAxes[k];
        SplashAxes[k] = M[3 + k];
    }
    SplashFocal   = M[12];
    SplashCentreX = M[13];
    SplashCentreY = M[14];
    SplashRun     = 1;
    const UBOOL bDrawn = DrawSplashes(Cmd, Dsv, Frame, Composite, 0);
    SplashRun     = 0;
    for (INT k = 0; k < 3; k++)
        SplashOrigin[k] = Origin[k];
    for (INT k = 0; k < 9; k++)
        SplashAxes[k] = Axes[k];
    SplashFocal   = Focal;
    SplashCentreX = CentreX;
    SplashCentreY = CentreY;
    return bDrawn;
}

UBOOL FBackend::EnsureWaterLamps()
{
    if (WaterLampBuffer)
        return 1;
    if (bWaterLampFailed || !Device)
        return 0;
    bWaterLampFailed = 1;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Buf;
    appMemzero(&Buf, sizeof(Buf));
    Buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Buf.Width            = WaterLampBytes * BACKEND_FRAME_COUNT;
    Buf.Height           = 1;
    Buf.DepthOrArraySize = 1;
    Buf.MipLevels        = 1;
    Buf.SampleDesc.Count = 1;
    Buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               __uuidof(ID3D12Resource), WaterLampBuffer.VoidSlot()),
               TEXT("CreateCommittedResource(water lamps)")) ||
        FAILED(WaterLampBuffer->Map(0, nullptr, (void**)&WaterLampMapped)))
    {
        WaterLampBuffer.Release();
        WaterLampMapped = nullptr;
        return 0;
    }
    appMemzero(WaterLampMapped, WaterLampBytes * BACKEND_FRAME_COUNT);     // no lamps in any slot until they are set
    bWaterLampFailed = 0;
    return 1;
}

UBOOL FBackend::EnsureReliefLamps()
{
    if (ReliefLampBuffer)
        return 1;
    if (bReliefLampFailed || !Device)
        return 0;
    bReliefLampFailed = 1;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Buf;
    appMemzero(&Buf, sizeof(Buf));
    Buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Buf.Width            = WaterLampBytes * BACKEND_FRAME_COUNT;
    Buf.Height           = 1;
    Buf.DepthOrArraySize = 1;
    Buf.MipLevels        = 1;
    Buf.SampleDesc.Count = 1;
    Buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               __uuidof(ID3D12Resource), ReliefLampBuffer.VoidSlot()),
               TEXT("CreateCommittedResource(relief lamps)")) ||
        FAILED(ReliefLampBuffer->Map(0, nullptr, (void**)&ReliefLampMapped)))
    {
        ReliefLampBuffer.Release();
        ReliefLampMapped = nullptr;
        return 0;
    }
    appMemzero(ReliefLampMapped, WaterLampBytes * BACKEND_FRAME_COUNT);
    bReliefLampFailed = 0;
    return 1;
}

void FBackend::SetReliefLamps(const FLOAT* Lamps, INT Count)
{
    if (!ReliefLampMapped || FrameIndex >= BACKEND_FRAME_COUNT)
        return;
    Count = Clamp(Count, 0, (INT)RdxMath::ReliefLampMax);
    // The shader's layout: how many, then two rows of four a lamp - the water's.
    FLOAT* const Slot = (FLOAT*)(ReliefLampMapped + FrameIndex * WaterLampBytes);
    appMemzero(Slot, 4 * sizeof(FLOAT) * (1 + 2 * RdxMath::ReliefLampMax));
    Slot[0] = (FLOAT)Count;
    for (INT i = 0; i < Count * 8; i++)
        Slot[4 + i] = Lamps[i];
    appMemzero(ReliefLampCopy, sizeof(ReliefLampCopy));
    ReliefLampCopy[0] = (FLOAT)Count;
    for (INT i = 0; i < Count * 8; i++)
        ReliefLampCopy[4 + i] = Lamps[i];
}

UBOOL FBackend::EnsureWetLamps()
{
    if (WetLampBuffer)
        return 1;
    if (bWetLampFailed || !Device)
        return 0;
    bWetLampFailed = 1;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Buf;
    appMemzero(&Buf, sizeof(Buf));
    Buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Buf.Width            = (UINT64)RdxMath::WetLampBytes * BACKEND_FRAME_COUNT;
    Buf.Height           = 1;
    Buf.DepthOrArraySize = 1;
    Buf.MipLevels        = 1;
    Buf.SampleDesc.Count = 1;
    Buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               __uuidof(ID3D12Resource), WetLampBuffer.VoidSlot()),
               TEXT("CreateCommittedResource(wet lamps)")) ||
        FAILED(WetLampBuffer->Map(0, nullptr, (void**)&WetLampMapped)))
    {
        WetLampBuffer.Release();
        WetLampMapped = nullptr;
        return 0;
    }
    appMemzero(WetLampMapped, RdxMath::WetLampBytes * BACKEND_FRAME_COUNT);     // no lamps in any slot until they are set
    bWetLampFailed = 0;
    return 1;
}

void FBackend::SetWetLamps(const FLOAT* Lamps, INT Count)
{
    static_assert(RdxMath::WetLampBytes <= 65536 && (RdxMath::WetLampBytes % 256) == 0,
                  "a wet lamp slot is one constant buffer, on a constant buffer's alignment");
    if (!WetLampMapped || FrameIndex >= BACKEND_FRAME_COUNT || !Lamps)
        return;
    Count = Clamp(Count, 0, (INT)RdxMath::WetLampMax);
    // The shader's layout: how many, then two rows of four a lamp - it reads no further than the count.
    FLOAT* const Slot = (FLOAT*)(WetLampMapped + FrameIndex * RdxMath::WetLampBytes);
    appMemzero(Slot, 4 * sizeof(FLOAT));
    Slot[0] = (FLOAT)Count;
    for (INT i = 0; i < Count * 8; i++)
        Slot[4 + i] = Lamps[i];
}

void FBackend::SetWaterLamps(const FLOAT* Lamps, INT Count)
{
    if (!WaterLampMapped || FrameIndex >= BACKEND_FRAME_COUNT)
        return;
    Count = Clamp(Count, 0, (INT)RdxMath::WaterLampMax);
    // The shader's layout: how many, then two rows of four a lamp.
    FLOAT* const Slot = (FLOAT*)(WaterLampMapped + FrameIndex * WaterLampBytes);
    appMemzero(Slot, 4 * sizeof(FLOAT) * (1 + 2 * RdxMath::WaterLampMax));
    Slot[0] = (FLOAT)Count;
    for (INT i = 0; i < Count * 8; i++)
        Slot[4 + i] = Lamps[i];
}

UBOOL FBackend::EnsureWaveField()
{
    if (WaveField)
        return 1;
    if (bWaveFailed || !Device || !Textures.GetHeap() || WaveW <= 0 || WaveH <= 0)
        return 0;
    bWaveFailed = 1;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = WaveW;
    Desc.Height           = WaveH;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R8_UNORM;
    Desc.SampleDesc.Count = 1;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, GWaterFieldRead,
                                               nullptr, __uuidof(ID3D12Resource), WaveField.VoidSlot()),
               TEXT("CreateCommittedResource(wave field)")))
        return 0;
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Buf;
    appMemzero(&Buf, sizeof(Buf));
    Buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Buf.Width            = 256 * 128;       // rows padded to D3D12's 256-byte pitch, 128 of them at most
    Buf.Height           = 1;
    Buf.DepthOrArraySize = 1;
    Buf.MipLevels        = 1;
    Buf.SampleDesc.Count = 1;
    Buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Buf, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, __uuidof(ID3D12Resource), WaveUpload[f].VoidSlot()),
                   TEXT("CreateCommittedResource(wave field upload)")) ||
            FAILED(WaveUpload[f]->Map(0, nullptr, (void**)&WaveUploadMapped[f])))
        {
            WaveField.Release();
            return 0;
        }
    }
    for (INT Set = 0; Set < 2; Set++)
        Device->CreateShaderResourceView(WaveField.Get(), nullptr, Textures.GetCpuHandle(WaterSlot(Set, WaterViewWaves)));
    bWaveFailed = 0;
    RdxLog(TEXT("water: the texture's own wave field on the GPU, %ix%i"), WaveW, WaveH);
    return 1;
}

void FBackend::SetRipples(FLOAT CameraX, FLOAT CameraY, const FLOAT* Pulses, INT Count)
{
    bRippleWanted = 1;
    // This frame's pushes in place of the last's: frames that bring no step do not add up theirs - at 190 frames a second
    // they came three at a time to a step.
    RipplePulseCount = 0;
    RippleCameraX = CameraX;
    RippleCameraY = CameraY;
    for (INT i = 0; i < Count && RipplePulseCount < RdxMath::RippleMaxPulses; i++, RipplePulseCount++)
        for (INT k = 0; k < 4; k++)
            RipplePulses[RipplePulseCount * 4 + k] = Pulses[i * 4 + k];
}

void FBackend::GetRippleOrigin(FLOAT& X, FLOAT& Y) const
{
    const UBOOL bLive = bRippleCleared && bRippleOriginValid && !bRippleFailed;
    X = bLive ? RippleOriginX : 1.0e9f;
    Y = bLive ? RippleOriginY : 1.0e9f;
}

// The two fields, their views - their own heap for the step, and the texture heap's reserved slots 2 and 5 for the water
// - and the step's pass. Made in the first frame with water; a failure is said once and leaves the water without ripples.
UBOOL FBackend::EnsureRipples()
{
    if (RipplePipeline && RippleField[0] && RippleField[1])
        return 1;
    if (bRippleFailed || !Device || !Textures.GetHeap())
        return 0;
    bRippleFailed = 1;          // until everything below has been made

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
    appMemzero(&HeapDesc, sizeof(HeapDesc));
    HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.NumDescriptors = 2;
    HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), RippleSrvHeap.VoidSlot()),
               TEXT("CreateDescriptorHeap(ripples SRV)")))
        return 0;
    HeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), RippleRtvHeap.VoidSlot()),
               TEXT("CreateDescriptorHeap(ripples RTV)")))
        return 0;

    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = RdxMath::RippleTexels;
    Desc.Height           = RdxMath::RippleTexels;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R16G16_FLOAT;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format = DXGI_FORMAT_R16G16_FLOAT;
    const UINT SrvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT RtvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (INT f = 0; f < 2; f++)
    {
        if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                   GWaterFieldRead, &Clear,
                                                   __uuidof(ID3D12Resource), RippleField[f].VoidSlot()),
                   TEXT("CreateCommittedResource(ripples)")))
            return 0;
        D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RippleRtvHeap->GetCPUDescriptorHandleForHeapStart();
        Rtv.ptr += f * RtvStep;
        Device->CreateRenderTargetView(RippleField[f].Get(), nullptr, Rtv);
        D3D12_CPU_DESCRIPTOR_HANDLE Srv = RippleSrvHeap->GetCPUDescriptorHandleForHeapStart();
        Srv.ptr += f * SrvStep;
        Device->CreateShaderResourceView(RippleField[f].Get(), nullptr, Srv);
        Device->CreateShaderResourceView(RippleField[f].Get(), nullptr, Textures.GetCpuHandle(WaterSlot(f, WaterViewRipples)));
    }

    D3D12_DESCRIPTOR_RANGE Range;
    appMemzero(&Range, sizeof(Range));
    Range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER Params[2];
    appMemzero(Params, sizeof(Params));
    Params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[0].DescriptorTable.NumDescriptorRanges = 1;
    Params[0].DescriptorTable.pDescriptorRanges   = &Range;
    Params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[1].Constants.Num32BitValues            = sizeof(FRippleStep) / 4;
    Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC RootDesc;
    appMemzero(&RootDesc, sizeof(RootDesc));
    RootDesc.NumParameters = 2;
    RootDesc.pParameters   = Params;
    TComRef<ID3DBlob> Blob, Error, Vs, Ps;
    if (FAILED(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        !Check(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), RippleRoot.VoidSlot()),
               TEXT("CreateRootSignature(ripples)")))
        return 0;
    const SIZE_T Len = strlen(GRippleShader);
    if (FAILED(RdxCompile(GRippleShader, Len, "Ripples", nullptr, nullptr, "RippleVS", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GRippleShader, Len, "Ripples", nullptr, nullptr, "RipplePS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("water: the ripples' shader failed: %s - no ripples"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature        = RippleRoot.Get();
    Pso.VS.pShaderBytecode    = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength     = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode    = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength     = Ps->GetBufferSize();
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 1;
    Pso.RTVFormats[0]         = DXGI_FORMAT_R16G16_FLOAT;
    Pso.SampleDesc.Count      = 1;
    Pso.SampleMask            = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = TRUE;
    Pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (!Check(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), RipplePipeline.VoidSlot()),
               TEXT("CreateGraphicsPipelineState(ripples)")))
        return 0;

    bRippleFailed  = 0;
    bRippleCleared = 0;
    RippleCurrent  = 0;
    RdxLog(TEXT("water: ripples up - a %ix%i field over %.0f units around the camera, %i steps a second"),
           (INT)RdxMath::RippleTexels, (INT)RdxMath::RippleTexels, RdxMath::RippleWorld, (INT)RdxMath::RippleStepsPerSecond);
    return 1;
}

// The caustics' net (DrawCausticNet). The light that comes down through a square of water is carried to the bed ray by
// ray and counted where it lands: a point for each of CAUSTIC_POINTS x CAUSTIC_POINTS places of the surface, moved
// sideways by the surface's slope there, each leaving the same small share of light on the texel it reaches - so the
// light is all there, gathered where the waves fold it into lines. The square repeats (a whole number of every wave
// goes across it), and so does its picture. The waves are the net's own, smooth ones: the water texture's field is
// eight bits a cell, and a net worked out from its steps is blotches. They run every way: waves that keep to two
// directions fold the light into a lattice of straight lines, and water never does that.
static const char* GCausticShader =
"cbuffer C : register(b0)\n"
"{\n"
"    float Time;        // seconds\n"
"    float Carry;       // how far a unit of slope moves the light sideways, in repeats of the square\n"
"    float Share;       // the light one point leaves\n"
"    float Lattice;     // 1: the waves of the measurement (WaveLattice)\n"
"};\n"
"// How many of each wave go across the square, and where the wave starts: one every twenty degrees or so round the\n"
"// half circle, no two of a length side by side, and three long ones across them.\n"
"static const float3 Wave[12] =\n"
"{\n"
"    float3( 4.0f,  0.0f, 1.4879f), float3( 6.0f,  2.0f, 3.6578f), float3( 3.0f,  2.0f, 2.7214f),\n"
"    float3( 4.0f,  6.0f, 1.0037f), float3( 1.0f,  4.0f, 0.7142f), float3(-1.0f,  7.0f, 3.2468f),\n"
"    float3(-2.0f,  4.0f, 3.6870f), float3(-6.0f,  5.0f, 6.0084f), float3(-5.0f,  2.0f, 4.0749f),\n"
"    float3( 2.0f,  1.0f, 1.8392f), float3(-1.0f,  2.0f, 5.1021f), float3( 1.0f, -3.0f, 0.3310f)\n"
"};\n"
"// A measurement only: waves that keep to two directions a right angle apart.\n"
"static const float3 WaveLattice[10] =\n"
"{\n"
"    float3( 4.0f, -5.0f, 1.4879f), float3(-4.0f,  4.0f, 3.6578f), float3(-6.0f, -5.0f, 2.7214f),\n"
"    float3( 2.0f,  0.0f, 1.0037f), float3( 2.0f,  3.0f, 0.7142f), float3(-1.0f, -1.0f, 3.2468f),\n"
"    float3(-1.0f, -1.0f, 3.6870f), float3(-4.0f,  3.0f, 6.0084f), float3( 4.0f, -3.0f, 4.0749f),\n"
"    float3( 2.0f,  3.0f, 1.8392f)\n"
"};\n"
"#define CAUSTIC_POINTS 2048u\n"
"// Every wave's steepest slope - the two sets fold the light as strongly as one another.\n"
"#define CAUSTIC_STEEP         0.0786f\n"
"#define CAUSTIC_STEEP_LATTICE 0.0949f\n"
"#define CAUSTIC_TILE   256.0f       // the level's units a repeat covers (FBackend::CausticTile)\n"
"// One wave's slope at a place of the square.\n"
"float2 WaveSlope(float3 W, float2 Uv, float Steep)\n"
"{\n"
"    const float Len = length(W.xy);\n"
"    // Shorter waves run faster for their length, as on water.\n"
"    const float Run = sqrt(6.2831853f * Len / CAUSTIC_TILE * 40.0f);\n"
"    return cos(6.2831853f * dot(W.xy, Uv) + W.z + Time * Run) * (Steep / Len) * W.xy;\n"
"}\n"
"float4 SplatVS(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    // The point's place on the surface: its cell of the grid, and a place of its own inside the cell - on the bare\n"
"    // grid the points land in rows, and the rows show.\n"
"    const uint2 Cell = uint2(Id % CAUSTIC_POINTS, Id / CAUSTIC_POINTS);\n"
"    uint H = Id * 2654435761u;\n"
"    H ^= H >> 15;\n"
"    H *= 1103515245u;\n"
"    H ^= H >> 13;\n"
"    H *= 2654435761u;\n"
"    H ^= H >> 16;\n"
"    const float2 Uv = (float2(Cell) + float2(H & 0xFFFFu, H >> 16) / 65535.0f) / (float)CAUSTIC_POINTS;\n"
"    float2 Slope = 0.0f;\n"
"    if (Lattice > 0.5f)\n"
"    {\n"
"        for (int i = 0; i < 10; i++)\n"
"            Slope += WaveSlope(WaveLattice[i], Uv, CAUSTIC_STEEP_LATTICE);\n"
"    }\n"
"    else\n"
"    {\n"
"        for (int i = 0; i < 12; i++)\n"
"            Slope += WaveSlope(Wave[i], Uv, CAUSTIC_STEEP);\n"
"    }\n"
"    const float2 Land = frac(Uv - Slope * Carry);\n"
"    return float4(Land.x * 2.0f - 1.0f, 1.0f - Land.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"float SplatPS() : SV_Target { return Share; }\n"
"// A level half the size from the one above it: each texel the mean of the four it covers.\n"
"Texture2D<float> Net  : register(t0);\n"
"SamplerState     Samp : register(s0);\n"
"struct VOut { float4 Pos : SV_Position; float2 Uv : TEXCOORD0; };\n"
"VOut HalveVS(uint Id : SV_VertexID)\n"
"{\n"
"    VOut o;\n"
"    o.Uv  = float2((Id << 1) & 2, Id & 2);\n"
"    o.Pos = float4(o.Uv.x * 2.0f - 1.0f, 1.0f - o.Uv.y * 2.0f, 0.0f, 1.0f);\n"
"    return o;\n"
"}\n"
"float HalvePS(VOut i) : SV_Target { return Net.SampleLevel(Samp, i.Uv, 0.0f); }\n";

// The net's picture with its levels, its views - for drawing each level, for reading the level above while the next is
// made, and one in each of the water's sets in the texture heap - and its two passes. Made in the first frame that
// asks for the caustics; a failure is said once and leaves the water without them (CausticsCanRun).
UBOOL FBackend::EnsureCaustics()
{
    if (CausticNet && CausticSplat && CausticHalve)
        return 1;
    if (bCausticsFailed || !Device || !Textures.GetHeap())
        return 0;
    bCausticsFailed = 1;        // until everything below has been made
    // A failure part way leaves nothing of the net behind: a net that is there is taken for one with its views
    // (EnsureWaterTargets).
    const auto Fail = [this]() -> UBOOL
    {
        CausticNet.Release();
        CausticSrvHeap.Release();
        CausticRtvHeap.Release();
        CausticSplat.Release();
        CausticHalve.Release();
        CausticRoot.Release();
        return 0;
    };

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
    appMemzero(&HeapDesc, sizeof(HeapDesc));
    HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.NumDescriptors = CausticNetLevels - 1;
    HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), CausticSrvHeap.VoidSlot()),
               TEXT("CreateDescriptorHeap(caustics SRV)")))
        return Fail();
    HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HeapDesc.NumDescriptors = CausticNetLevels;
    HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), CausticRtvHeap.VoidSlot()),
               TEXT("CreateDescriptorHeap(caustics RTV)")))
        return Fail();

    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = CausticNetSize;
    Desc.Height           = CausticNetSize;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = CausticNetLevels;
    Desc.Format           = DXGI_FORMAT_R16_FLOAT;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format = DXGI_FORMAT_R16_FLOAT;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &Clear,
                                               __uuidof(ID3D12Resource), CausticNet.VoidSlot()),
               TEXT("CreateCommittedResource(caustics net)")))
        return Fail();
    const UINT SrvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT RtvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (INT k = 0; k < CausticNetLevels; k++)
    {
        D3D12_RENDER_TARGET_VIEW_DESC Rd;
        appMemzero(&Rd, sizeof(Rd));
        Rd.Format             = DXGI_FORMAT_R16_FLOAT;
        Rd.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
        Rd.Texture2D.MipSlice = (UINT)k;
        D3D12_CPU_DESCRIPTOR_HANDLE Rtv = CausticRtvHeap->GetCPUDescriptorHandleForHeapStart();
        Rtv.ptr += (SIZE_T)k * RtvStep;
        Device->CreateRenderTargetView(CausticNet.Get(), &Rd, Rtv);
        if (k + 1 < CausticNetLevels)
        {
            // One level alone: the level below it is a render target while this one is read.
            D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
            appMemzero(&Sd, sizeof(Sd));
            Sd.Format                    = DXGI_FORMAT_R16_FLOAT;
            Sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            Sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Sd.Texture2D.MostDetailedMip = (UINT)k;
            Sd.Texture2D.MipLevels       = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE Srv = CausticSrvHeap->GetCPUDescriptorHandleForHeapStart();
            Srv.ptr += (SIZE_T)k * SrvStep;
            Device->CreateShaderResourceView(CausticNet.Get(), &Sd, Srv);
        }
    }

    D3D12_DESCRIPTOR_RANGE Range;
    appMemzero(&Range, sizeof(Range));
    Range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER Params[2];
    appMemzero(Params, sizeof(Params));
    Params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[0].Constants.Num32BitValues = 4;
    Params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[1].DescriptorTable.NumDescriptorRanges = 1;
    Params[1].DescriptorTable.pDescriptorRanges   = &Range;
    Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC Sampler;
    appMemzero(&Sampler, sizeof(Sampler));
    Sampler.Filter           = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC RootDesc;
    appMemzero(&RootDesc, sizeof(RootDesc));
    RootDesc.NumParameters     = 2;
    RootDesc.pParameters       = Params;
    RootDesc.NumStaticSamplers = 1;
    RootDesc.pStaticSamplers   = &Sampler;
    TComRef<ID3DBlob> Blob, Error, SplatVs, SplatPs, HalveVs, HalvePs;
    if (FAILED(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        !Check(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), CausticRoot.VoidSlot()),
               TEXT("CreateRootSignature(caustics)")))
        return Fail();
    const SIZE_T Len = strlen(GCausticShader);
    if (FAILED(RdxCompile(GCausticShader, Len, "Caustics", nullptr, nullptr, "SplatVS", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, SplatVs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GCausticShader, Len, "Caustics", nullptr, nullptr, "SplatPS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, SplatPs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GCausticShader, Len, "Caustics", nullptr, nullptr, "HalveVS", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, HalveVs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GCausticShader, Len, "Caustics", nullptr, nullptr, "HalvePS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, HalvePs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("water: the caustics' shader failed: %s - no caustics"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return Fail();
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature        = CausticRoot.Get();
    Pso.VS.pShaderBytecode    = SplatVs->GetBufferPointer();
    Pso.VS.BytecodeLength     = SplatVs->GetBufferSize();
    Pso.PS.pShaderBytecode    = SplatPs->GetBufferPointer();
    Pso.PS.BytecodeLength     = SplatPs->GetBufferSize();
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    Pso.NumRenderTargets      = 1;
    Pso.RTVFormats[0]         = DXGI_FORMAT_R16_FLOAT;
    Pso.SampleDesc.Count      = 1;
    Pso.SampleMask            = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = TRUE;
    // Every point's light is added to what is there: where the waves fold the surface onto a line, the points pile up.
    D3D12_RENDER_TARGET_BLEND_DESC& B = Pso.BlendState.RenderTarget[0];
    B.BlendEnable           = TRUE;
    B.SrcBlend              = D3D12_BLEND_ONE;
    B.DestBlend             = D3D12_BLEND_ONE;
    B.BlendOp               = D3D12_BLEND_OP_ADD;
    B.SrcBlendAlpha         = D3D12_BLEND_ONE;
    B.DestBlendAlpha        = D3D12_BLEND_ONE;
    B.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    B.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (!Check(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), CausticSplat.VoidSlot()),
               TEXT("CreateGraphicsPipelineState(caustics splat)")))
        return Fail();
    Pso.VS.pShaderBytecode    = HalveVs->GetBufferPointer();
    Pso.VS.BytecodeLength     = HalveVs->GetBufferSize();
    Pso.PS.pShaderBytecode    = HalvePs->GetBufferPointer();
    Pso.PS.BytecodeLength     = HalvePs->GetBufferSize();
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    B.BlendEnable             = FALSE;
    if (!Check(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), CausticHalve.VoidSlot()),
               TEXT("CreateGraphicsPipelineState(caustics levels)")))
        return Fail();

    // The water reads all of its levels through the fifth view of its sets.
    for (INT Set = 0; Set < 2; Set++)
        Device->CreateShaderResourceView(CausticNet.Get(), nullptr, Textures.GetCpuHandle(WaterSlot(Set, WaterViewNet)));
    bCausticsFailed = 0;
    bCausticsDrawn  = 0;
    RdxLog(TEXT("water: caustics up - a %ix%i net of %i levels over %.0f units, repeating"), (INT)CausticNetSize,
           (INT)CausticNetSize, (INT)CausticNetLevels, CausticTile);
    return 1;
}

// At the start of the frame, with the ripples' step (BeginFrame): the net as its waves stand now, and its smaller levels
// from it. Only while the caustics are asked for, and where there is clear water for them to fall into (BeginFrame
// asks).
void FBackend::DrawCausticNet(ID3D12GraphicsCommandList* Cmd)
{
    if (!bWaterWanted || !bCausticsWanted || !EnsureCaustics())
        return;
    if (CausticStart <= 0.0)
        CausticStart = appSeconds();
    const UINT RtvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT SrvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                 = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource = CausticNet.Get();
    ID3D12DescriptorHeap* Heaps[1] = { CausticSrvHeap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(CausticRoot.Get());
    const FLOAT K[4] = { (FLOAT)(appSeconds() - CausticStart), CausticCarry / CausticTile,
                         (FLOAT)(CausticNetSize * CausticNetSize) / (FLOAT)(CausticPoints * CausticPoints),
                         bCausticOldNet ? 1.0f : 0.0f };
    Cmd->SetGraphicsRoot32BitConstants(0, 4, K, 0);
    for (INT Level = 0; Level < CausticNetLevels; Level++)
    {
        const INT Side = CausticNetSize >> Level;
        B.Transition.Subresource = (UINT)Level;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Cmd->ResourceBarrier(1, &B);
        D3D12_CPU_DESCRIPTOR_HANDLE Rtv = CausticRtvHeap->GetCPUDescriptorHandleForHeapStart();
        Rtv.ptr += (SIZE_T)Level * RtvStep;
        const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)Side, (FLOAT)Side, 0.0f, 1.0f };
        const D3D12_RECT     Sc = { 0, 0, Side, Side };
        Cmd->RSSetViewports(1, &Vp);
        Cmd->RSSetScissorRects(1, &Sc);
        Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
        if (Level == 0)
        {
            const FLOAT Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            Cmd->ClearRenderTargetView(Rtv, Zero, 0, nullptr);
            Cmd->SetPipelineState(CausticSplat.Get());
            Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
            Cmd->DrawInstanced((UINT)(CausticPoints * CausticPoints), 1, 0, 0);
        }
        else
        {
            D3D12_GPU_DESCRIPTOR_HANDLE Srv = CausticSrvHeap->GetGPUDescriptorHandleForHeapStart();
            Srv.ptr += (UINT64)(Level - 1) * SrvStep;
            Cmd->SetPipelineState(CausticHalve.Get());
            Cmd->SetGraphicsRootDescriptorTable(1, Srv);
            Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            Cmd->DrawInstanced(3, 1, 0, 0);
        }
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &B);
    }
    bCausticsDrawn = 1;
    CausticNetFrames++;
}

// The caustics laid on the picture (LayCaustics), in two steps.
// The mask (PortalVS, PortalPS): the liquids' borders as the level has them, drawn into a picture of their own, each
// writing its value (RdxMath::CausticMaskValue) on the pixels whose depth lies BEYOND it - what is seen through it. The
// nearest border is drawn first, so what a pixel keeps is the last border the view crosses before it.
// The laying-on (LayVS, LayPS), over the whole picture: every pixel is put back into the level from the copy of the
// world's depth; the mask, and where no border lies before the pixel the camera's own zone, say which liquid it lies
// in (Wet); and one under a surface of that liquid (Box, Top, Zone) has the net's light where that light went in above
// it - the light comes down a little aslant (LAY_SLANT), so the net runs on down a wall instead of standing on it in
// bars - and is made that much brighter or darker: Out is half the gain, and the pipeline's blend doubles the picture
// by it. The surface's facing comes from the place's change across the picture, turned towards the camera: one that
// faces up takes the whole net, a wall LAY_WALL of it, one that faces down none. LAY_FOCUS: the depth from which the
// net is whole - just under the surface the light has not come together yet; LAY_DEEP: the depth over which it fades
// to a third again; LAY_SOFT: how much of a level the net is read softer than a pixel asks, which takes the grain of
// its points away. The first table is the water's own set from its second view on (FBackend::WaterSlot) - the depth's
// copy first, the net fourth; the second is the mask, which only the laying-on reads.
static const char* GCausticLayShader =
"Texture2D<float> Depth : register(t0);\n"
"Texture2D<float> Net   : register(t3);\n"
"Texture2D<float> Mask  : register(t4);\n"
"SamplerState     Samp  : register(s0);\n"
"cbuffer L : register(b0)\n"
"{\n"
"    float3 Cam;    float Focal;     // the camera on the level; the engine's projection, in its own pixels\n"
"    float3 AxisX;  float CentreX;   // the view's three axes on the level; the screen's centre, engine pixels\n"
"    float3 AxisY;  float CentreY;\n"
"    float3 AxisZ;  float Boxes;     // how many waters\n"
"    float2 Engine; float2 Jitter;   // the picture across, in the engine's pixels; the upscaler's shift in its 0..1\n"
"    float4 Box[4];                  // each water's surface on the level: its lowest X and Y, its highest X and Y\n"
"    float4 Top;                     // its height\n"
"    float4 Zone;                    // and its water's number, plus one\n"
"    float  CamWet;                  // the water the camera is in, plus one; 0: in none\n"
"    float3 Spare;\n"
"};\n"
"#define LAY_TILE     256.0f\n"
"#define LAY_TEXELS   512.0f\n"
"#define LAY_LEVELS   4.0f\n"
"#define LAY_STRENGTH 1.0f\n"
"#define LAY_FOCUS    24.0f\n"
"#define LAY_DEEP     384.0f\n"
"#define LAY_SOFT     0.5f\n"
"#define LAY_WALL     0.45f\n"
"static const float2 LAY_SLANT = float2(0.31f, 0.19f);\n"
"struct PortalIn  { float3 P : POSITION; float Value : TEXCOORD0; };\n"
"struct PortalOut { float4 Pos : SV_Position; float Z : TEXCOORD0; nointerpolation float Value : TEXCOORD1; };\n"
"PortalOut PortalVS(PortalIn i)\n"
"{\n"
"    const float3 R = i.P - Cam;\n"
"    const float3 V = float3(dot(R, AxisX), dot(R, AxisY), dot(R, AxisZ));\n"
"    // The picture's place from 0 to 1, times the depth: the engine's pixel over the picture's width, and the\n"
"    // upscaler's shift - what LayPS undoes. A border nearer than one unit is cut off there.\n"
"    const float2 Uv = (float2(CentreX, CentreY) * V.z + V.xy * Focal) / Engine + Jitter * V.z;\n"
"    PortalOut o;\n"
"    o.Pos   = float4(Uv.x * 2.0f - V.z, V.z - Uv.y * 2.0f, 1.0f, V.z);\n"
"    o.Z     = V.z;\n"
"    o.Value = i.Value;\n"
"    return o;\n"
"}\n"
"float PortalPS(PortalOut i) : SV_Target\n"
"{\n"
"    const float Z = 1.0f / max(Depth.Load(int3(i.Pos.xy, 0)), 1e-6f);\n"
"    // Only what is seen THROUGH the border: a thing in front of it keeps what it had.\n"
"    if (Z < i.Z * 1.0005f + 0.25f)\n"
"        discard;\n"
"    return i.Value;\n"
"}\n"
"float4 LayVS(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    const float2 P = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(P.x * 2.0f - 1.0f, 1.0f - P.y * 2.0f, 0.0f, 1.0f);\n"
"}\n"
"float4 LayPS(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    float2 Size;\n"
"    Depth.GetDimensions(Size.x, Size.y);\n"
"    const float  Z   = 1.0f / max(Depth.Load(int3(Pos.xy, 0)), 1e-6f);\n"
"    const float2 Eng = (Pos.xy / Size - Jitter) * Engine;\n"
"    const float2 Off = (Eng - float2(CentreX, CentreY)) * (Z / Focal);\n"
"    const float3 P   = Cam + Off.x * AxisX + Off.y * AxisY + Z * AxisZ;\n"
"    const float3 Px  = ddx(P);\n"
"    const float3 Py  = ddy(P);\n"
"    // Which liquid the pixel lies in: the one the last border before it leads into - or, with no border before it,\n"
"    // the one the camera is in.\n"
"    const float M   = round(Mask.Load(int3(Pos.xy, 0)) * 255.0f);\n"
"    const float Wet = (M < 0.5f) ? CamWet : M - 1.0f;\n"
"    if (Wet < 0.5f)\n"
"        discard;\n"
"    float Gain = 1.0f;\n"
"    if (Wet > 0.5f)\n"
"    {\n"
"        // Under which of that liquid's surfaces, and how deep: the one nearest above.\n"
"        float Deep = 1.0e9f;\n"
"        for (int i = 0; i < 4; i++)\n"
"        {\n"
"            const float D = Top[i] - P.z;\n"
"            if ((float)i < Boxes && Zone[i] == Wet && D > 0.0f && D < Deep && P.x > Box[i].x && P.y > Box[i].y &&\n"
"                P.x < Box[i].z && P.y < Box[i].w)\n"
"                Deep = D;\n"
"        }\n"
"        if (Deep < 4096.0f)\n"
"        {\n"
"            float3 N = cross(Px, Py);\n"
"            const float Len = length(N);\n"
"            N = (dot(N, P - Cam) > 0.0f) ? -N : N;\n"
"            const float Up = (Len > 1e-12f) ? N.z / Len : 0.0f;\n"
"            // The net's place: the pixel's own on the level, moved along the light's slant by the depth. How much\n"
"            // of the net a pixel covers comes from how that place changes across the picture.\n"
"            const float2 Uv  = (P.xy + LAY_SLANT * Deep) / LAY_TILE;\n"
"            const float2 Ux  = (Px.xy - LAY_SLANT * Px.z) / LAY_TILE;\n"
"            const float2 Uy  = (Py.xy - LAY_SLANT * Py.z) / LAY_TILE;\n"
"            const float  Lod = log2(max(max(length(Ux), length(Uy)) * LAY_TEXELS, 1.0f)) + LAY_SOFT;\n"
"            const float  Light = Net.SampleLevel(Samp, Uv, Lod);\n"
"            const float  Face  = (Up >= 0.0f) ? lerp(LAY_WALL, 1.0f, saturate(Up / 0.7f))\n"
"                                              : LAY_WALL * saturate(1.0f + Up / 0.3f);\n"
"            const float  Much  = LAY_STRENGTH * saturate(Deep / LAY_FOCUS) * exp(-Deep / LAY_DEEP) *\n"
"                                 saturate(LAY_LEVELS - Lod) * Face;\n"
"            Gain = lerp(1.0f, Light, Much);\n"
"        }\n"
"    }\n"
"    return float4(Gain * 0.5f, Gain * 0.5f, Gain * 0.5f, 1.0f);\n"
"}\n";

// The laying-on's pipelines. The mask's, once. And for the picture as it is now - its format, and as many samples as it
// has: the world's own multisampled picture where the caustics go on before a water's surface, the picture brought
// down to one sample where they go on at the world's end, and a frame may be either - so one pipeline is kept for
// each, made again when its format or its samples are others. A failure is said once and leaves the water without its
// caustics (CausticsCanRun).
UBOOL FBackend::EnsureCausticLay(DXGI_FORMAT Format, UINT Samples)
{
    const INT Which = (Samples > 1) ? 1 : 0;
    TComRef<ID3D12PipelineState>& CausticLay = CausticLays[Which];
    if (CausticMaskPipeline && CausticLay && CausticLayFormat[Which] == Format && CausticLaySamples[Which] == Samples)
        return 1;
    if (bCausticsFailed || !Device)
        return 0;
    CausticLay.Release();
    bCausticsFailed = 1;        // until everything below has been made
    if (!CausticLayRoot)
    {
        D3D12_DESCRIPTOR_RANGE Ranges[2];
        appMemzero(Ranges, sizeof(Ranges));
        // The water's set from its depth up to the net (the probe's view after them is not the caustics').
        Ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Ranges[0].NumDescriptors     = WaterViewNet + 1 - WaterViewDepth;
        Ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Ranges[1].NumDescriptors     = 1;
        Ranges[1].BaseShaderRegister = WaterViewNet + 1 - WaterViewDepth;
        D3D12_ROOT_PARAMETER Params[3];
        appMemzero(Params, sizeof(Params));
        Params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        Params[0].Constants.Num32BitValues = CAUSTIC_LAY_CONSTANTS;
        Params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
        for (INT t = 0; t < 2; t++)
        {
            Params[1 + t].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            Params[1 + t].DescriptorTable.NumDescriptorRanges = 1;
            Params[1 + t].DescriptorTable.pDescriptorRanges   = &Ranges[t];
            Params[1 + t].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_STATIC_SAMPLER_DESC Sampler;
        appMemzero(&Sampler, sizeof(Sampler));
        Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC RootDesc;
        appMemzero(&RootDesc, sizeof(RootDesc));
        RootDesc.NumParameters     = 3;
        RootDesc.pParameters       = Params;
        RootDesc.NumStaticSamplers = 1;
        RootDesc.pStaticSamplers   = &Sampler;
        RootDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        TComRef<ID3DBlob> Blob, Error;
        if (FAILED(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
            !Check(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                               __uuidof(ID3D12RootSignature), CausticLayRoot.VoidSlot()),
                   TEXT("CreateRootSignature(caustics laid on)")))
        {
            CausticLayRoot.Release();
            return 0;
        }
    }
    const SIZE_T Len = strlen(GCausticLayShader);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature        = CausticLayRoot.Get();
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 1;
    Pso.SampleMask            = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = TRUE;
    if (!CausticMaskPipeline)
    {
        TComRef<ID3DBlob> Vs, Ps, Error;
        if (FAILED(RdxCompile(GCausticLayShader, Len, "CausticsLay", nullptr, nullptr, "PortalVS", "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
            FAILED(RdxCompile(GCausticLayShader, Len, "CausticsLay", nullptr, nullptr, "PortalPS", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
        {
            RdxLog(TEXT("water: the shader of the caustics' mask failed: %s - no caustics"),
                   Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
            return 0;
        }
        static const D3D12_INPUT_ELEMENT_DESC Layout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT,       0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC MaskPso = Pso;
        MaskPso.VS.pShaderBytecode = Vs->GetBufferPointer();
        MaskPso.VS.BytecodeLength  = Vs->GetBufferSize();
        MaskPso.PS.pShaderBytecode = Ps->GetBufferPointer();
        MaskPso.PS.BytecodeLength  = Ps->GetBufferSize();
        MaskPso.InputLayout.pInputElementDescs = Layout;
        MaskPso.InputLayout.NumElements        = 2;
        MaskPso.RTVFormats[0]      = DXGI_FORMAT_R8_UNORM;
        MaskPso.SampleDesc.Count   = 1;
        MaskPso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (!Check(Device->CreateGraphicsPipelineState(&MaskPso, __uuidof(ID3D12PipelineState),
                                                       CausticMaskPipeline.VoidSlot()),
                   TEXT("CreateGraphicsPipelineState(caustics' mask)")))
        {
            CausticMaskPipeline.Release();
            return 0;
        }
    }
    TComRef<ID3DBlob> Vs, Ps, Error;
    if (FAILED(RdxCompile(GCausticLayShader, Len, "CausticsLay", nullptr, nullptr, "LayVS", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GCausticLayShader, Len, "CausticsLay", nullptr, nullptr, "LayPS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("water: the shader that lays the caustics on failed: %s - no caustics"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    Pso.VS.pShaderBytecode = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength  = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength  = Ps->GetBufferSize();
    Pso.RTVFormats[0]      = Format;
    Pso.SampleDesc.Count   = Samples;
    // The picture times what the shader says, twice over: half is no change, one doubles it. Its alpha is left alone.
    D3D12_RENDER_TARGET_BLEND_DESC& B = Pso.BlendState.RenderTarget[0];
    B.BlendEnable           = TRUE;
    B.SrcBlend              = D3D12_BLEND_DEST_COLOR;
    B.DestBlend             = D3D12_BLEND_SRC_COLOR;
    B.BlendOp               = D3D12_BLEND_OP_ADD;
    B.SrcBlendAlpha         = D3D12_BLEND_ZERO;
    B.DestBlendAlpha        = D3D12_BLEND_ONE;
    B.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    B.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (!Check(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), CausticLay.VoidSlot()),
               TEXT("CreateGraphicsPipelineState(caustics laid on)")))
    {
        CausticLay.Release();
        return 0;
    }
    CausticLayFormat[Which]  = Format;
    CausticLaySamples[Which] = Samples;
    bCausticsFailed          = 0;
    RdxLog(TEXT("water: the caustics are laid on what lies in the water - the pass for this picture's format, %i "
                "sample(s)"), (INT)Samples);
    return 1;
}

// The borders' triangles: one upload buffer, a place in it for each frame slot (SetCausticView fills this frame's).
UBOOL FBackend::EnsureCausticBorders()
{
    if (CausticPortalBuffer)
        return 1;
    if (bCausticsFailed || !Device)
        return 0;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Buf;
    appMemzero(&Buf, sizeof(Buf));
    Buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Buf.Width            = (UINT64)CausticPortalBytes * BACKEND_FRAME_COUNT;
    Buf.Height           = 1;
    Buf.DepthOrArraySize = 1;
    Buf.MipLevels        = 1;
    Buf.SampleDesc.Count = 1;
    Buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Buf, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, __uuidof(ID3D12Resource), CausticPortalBuffer.VoidSlot()),
               TEXT("CreateCommittedResource(caustics' borders)")) ||
        FAILED(CausticPortalBuffer->Map(0, nullptr, (void**)&CausticPortalMapped)))
    {
        CausticPortalBuffer.Release();
        CausticPortalMapped = nullptr;
        bCausticsFailed     = 1;
        RdxLog(TEXT("water: no room for the caustics' borders - no caustics"));
        return 0;
    }
    return 1;
}

// The mask's picture, at the scene's size, with its view for drawing it and its view in the texture heap's reserved
// slot. Gone with the scene's target (CreateSceneTarget) and made again here at the new size; where it cannot be made
// the caustics are left out until the sizes change (RefuseTarget).
UBOOL FBackend::EnsureCausticMask()
{
    if (CausticMask)
        return 1;
    if (bCausticsFailed || Refused(CausticRefusal) || !Device || !Textures.GetHeap() || SceneWidth <= 0 ||
        SceneHeight <= 0)
        return 0;
    if (!CausticMaskRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = 1;
        if (!Check(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap),
                                                CausticMaskRtvHeap.VoidSlot()),
                   TEXT("CreateDescriptorHeap(caustics' mask)")))
        {
            CausticMaskRtvHeap.Release();
            bCausticsFailed = 1;
            return 0;
        }
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = (UINT64)SceneWidth;
    Desc.Height           = (UINT)SceneHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R8_UNORM;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format = DXGI_FORMAT_R8_UNORM;
    if (!Check(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &Clear,
                                               __uuidof(ID3D12Resource), CausticMask.VoidSlot()),
               TEXT("CreateCommittedResource(caustics' mask)")))
    {
        // Nothing of it is left behind: a mask that is there is taken for one with its views.
        CausticMask.Release();
        RefuseTarget(CausticRefusal, TEXT("caustics"));
        return 0;
    }
    Device->CreateRenderTargetView(CausticMask.Get(), nullptr,
                                   CausticMaskRtvHeap->GetCPUDescriptorHandleForHeapStart());
    Device->CreateShaderResourceView(CausticMask.Get(), nullptr,
                                     Textures.GetCpuHandle(Textures.GetReservedIndex(CausticMaskView)));
    return 1;
}

void FBackend::SetCausticView(const FLOAT* Origin, const FLOAT* Axes, FLOAT Focal, FLOAT CentreX, FLOAT CentreY,
                              const FLOAT* Boxes, INT Count, const FLOAT* Borders, INT Corners, INT CamWet)
{
    CausticBoxCount = 0;
    CausticCorners  = 0;
    CausticCamWet   = 0;
    bCausticView    = 0;
    if (!Origin || !Axes || !(Focal > 0.0f))
        return;
    appMemcpy(CausticOrigin, Origin, sizeof(CausticOrigin));
    appMemcpy(CausticAxes, Axes, sizeof(CausticAxes));
    CausticFocal   = Focal;
    CausticCentreX = CentreX;
    CausticCentreY = CentreY;
    CausticCamWet  = Max(CamWet, 0);
    bCausticView   = 1;
    for (INT i = 0; Boxes && i < Count && CausticBoxCount < (INT)RdxMath::CausticBoxesMost; i++, CausticBoxCount++)
        appMemcpy(CausticBoxes + CausticBoxCount * RdxMath::CausticBoxFloats, Boxes + i * RdxMath::CausticBoxFloats,
                  RdxMath::CausticBoxFloats * sizeof(FLOAT));
    // The borders into this frame slot's place: the frame before may still be drawing from its own.
    Corners = Min(Corners - Corners % 3, (INT)RdxMath::CausticPortalVertsMost);
    if (Borders && Corners > 0 && CausticBoxCount > 0 && FrameIndex < BACKEND_FRAME_COUNT && EnsureCausticBorders())
    {
        appMemcpy(CausticPortalMapped + (SIZE_T)CausticPortalBytes * FrameIndex, Borders,
                  (SIZE_T)Corners * RdxMath::CausticPortalFloats * sizeof(FLOAT));
        CausticCorners = Corners;
    }
}

// The caustics onto the picture, in place (CopyForWater calls, once the depth's copy is this frame's): the mask from
// this frame's borders, then one triangle over all of the picture, the blend doing the rest. With MSAA the picture is
// the multisampled one, and every sample of a pixel takes the pixel's own gain. The targets, the viewport and the
// pass's bindings are the caller's to put back.
UBOOL FBackend::LayCaustics(ID3D12GraphicsCommandList* Cmd, UBOOL bMsaa)
{
    if (!Cmd || bCausticsLaid || !CausticsNow() || !CausticNet || !bCausticsDrawn || !WaterDepth ||
        !Textures.GetHeap() || !EnsureCausticLay(Colours.Draw, bMsaa ? (UINT)MsaaSampleCount() : 1u) ||
        !EnsureCausticMask())
        return 0;
    bCausticsLaid = 1;
    FLOAT K[CAUSTIC_LAY_CONSTANTS];
    appMemzero(K, sizeof(K));
    for (INT a = 0; a < 3; a++)
    {
        K[a]      = CausticOrigin[a];
        K[4 + a]  = CausticAxes[a];
        K[8 + a]  = CausticAxes[3 + a];
        K[12 + a] = CausticAxes[6 + a];
    }
    K[3]  = CausticFocal;
    K[7]  = CausticCentreX;
    K[11] = CausticCentreY;
    K[15] = (FLOAT)CausticBoxCount;
    K[16] = (FLOAT)Width;
    K[17] = (FLOAT)Height;
    K[18] = 0.5f * JitterClip[0];
    K[19] = -0.5f * JitterClip[1];
    for (INT i = 0; i < CausticBoxCount; i++)
    {
        const FLOAT* const Box = CausticBoxes + i * RdxMath::CausticBoxFloats;
        // A little past the surface's own extent: a pool's walls stand on its very edge.
        K[20 + i * 4 + 0] = Box[0] - RdxMath::CausticBoxMargin;
        K[20 + i * 4 + 1] = Box[1] - RdxMath::CausticBoxMargin;
        K[20 + i * 4 + 2] = Box[2] + RdxMath::CausticBoxMargin;
        K[20 + i * 4 + 3] = Box[3] + RdxMath::CausticBoxMargin;
        K[36 + i]         = Box[4];
        K[40 + i]         = Box[5];
    }
    K[44] = (FLOAT)CausticCamWet;
    const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SceneWidth, (FLOAT)SceneHeight, 0.0f, 1.0f };
    const D3D12_RECT     Sc = { 0, 0, SceneWidth, SceneHeight };
    ID3D12DescriptorHeap* Heaps[1] = { Textures.GetHeap() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(CausticLayRoot.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, CAUSTIC_LAY_CONSTANTS, K, 0);
    Cmd->SetGraphicsRootDescriptorTable(1, Textures.GetGpuHandle(WaterSlot(0, WaterViewDepth)));
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // The mask: nothing, and then this frame's borders over it.
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = CausticMask.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Cmd->ResourceBarrier(1, &B);
    const D3D12_CPU_DESCRIPTOR_HANDLE MaskRtv = CausticMaskRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const FLOAT None[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    Cmd->OMSetRenderTargets(1, &MaskRtv, FALSE, nullptr);
    Cmd->ClearRenderTargetView(MaskRtv, None, 0, nullptr);
    if (CausticCorners > 0 && CausticPortalBuffer)
    {
        const UINT Stride = RdxMath::CausticPortalFloats * sizeof(FLOAT);
        D3D12_VERTEX_BUFFER_VIEW View;
        View.BufferLocation = CausticPortalBuffer->GetGPUVirtualAddress() + (UINT64)CausticPortalBytes * FrameIndex;
        View.SizeInBytes    = (UINT)CausticCorners * Stride;
        View.StrideInBytes  = Stride;
        Cmd->SetPipelineState(CausticMaskPipeline.Get());
        Cmd->IASetVertexBuffers(0, 1, &View);
        Cmd->DrawInstanced((UINT)CausticCorners, 1, 0, 0);
    }
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &B);

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv;
    if (bMsaa)
        Rtv = MsaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
    else
    {
        Rtv = RtvStart;
        Rtv.ptr += BACKEND_FRAME_COUNT * RtvStride;       // the scene target's view
    }
    Cmd->SetGraphicsRootDescriptorTable(2, Textures.GetGpuHandle(Textures.GetReservedIndex(CausticMaskView)));
    Cmd->SetPipelineState(CausticLays[bMsaa ? 1 : 0].Get());
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    Cmd->DrawInstanced(3, 1, 0, 0);
    CausticLaidFrames++;
    return 1;
}

// At the start of the frame, before anything of the scene is bound (BeginFrame): as many steps as the time since the last
// one asks for, the first of them taking the field along with the camera and adding this frame's pushes. The water of
// this frame reads what the last step wrote (the reserved view set that holds it: FSolidPass::SetWaterTable).
void FBackend::StepRipples(ID3D12GraphicsCommandList* Cmd)
{
    // The texture's own wave field, when a new one arrived last frame: into this frame slot's upload buffer, then across.
    if (bWaterWanted && bWaveNew && EnsureWaveField() && WaveUploadMapped[FrameIndex] && WaveW <= 128 && WaveH <= 128)
    {
        bWaveNew = 0;
        for (INT y = 0; y < WaveH; y++)
            appMemcpy(WaveUploadMapped[FrameIndex] + y * 256, WaveHeights + y * WaveW, WaveW);
        D3D12_RESOURCE_BARRIER Wb;
        appMemzero(&Wb, sizeof(Wb));
        Wb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Wb.Transition.pResource   = WaveField.Get();
        Wb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Wb.Transition.StateBefore = GWaterFieldRead;
        Wb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        Cmd->ResourceBarrier(1, &Wb);
        D3D12_TEXTURE_COPY_LOCATION Dst, Src;
        appMemzero(&Dst, sizeof(Dst));
        appMemzero(&Src, sizeof(Src));
        Dst.pResource        = WaveField.Get();
        Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.pResource        = WaveUpload[FrameIndex].Get();
        Src.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
        Src.PlacedFootprint.Footprint.Width    = WaveW;
        Src.PlacedFootprint.Footprint.Height   = WaveH;
        Src.PlacedFootprint.Footprint.Depth    = 1;
        Src.PlacedFootprint.Footprint.RowPitch = 256;
        Cmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        Wb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        Wb.Transition.StateAfter  = GWaterFieldRead;
        Cmd->ResourceBarrier(1, &Wb);
    }
    const UBOOL bRun = bWaterWanted && bRippleWanted && EnsureRipples();
    bRippleWanted = 0;
    if (!bRun)
    {
        RipplePulseCount = 0;
        Solid.SetWaterTable(WaterSlot(0, 0));
        return;
    }
    const DOUBLE Now = appSeconds();
    const INT    Steps = RdxMath::RippleSteps(RippleCarry, (RippleLastTime > 0.0) ? (FLOAT)(Now - RippleLastTime) : 0.0f);
    RippleLastTime = Now;

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv0 = RippleRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT RtvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT SrvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Still water to begin with: both fields cleared once.
    if (!bRippleCleared)
    {
        const FLOAT Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (INT f = 0; f < 2; f++)
        {
            B.Transition.pResource   = RippleField[f].Get();
            B.Transition.StateBefore = GWaterFieldRead;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            Cmd->ResourceBarrier(1, &B);
            D3D12_CPU_DESCRIPTOR_HANDLE Rtv = Rtv0;
            Rtv.ptr += f * RtvStep;
            Cmd->ClearRenderTargetView(Rtv, Zero, 0, nullptr);
            B.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            B.Transition.StateAfter  = GWaterFieldRead;
            Cmd->ResourceBarrier(1, &B);
        }
        bRippleCleared     = 1;
        bRippleOriginValid = 0;
        RippleCurrent      = 0;
    }

    if (Steps > 0)
    {
        // The field's corner now, and how far it moved in texels. A jump larger than the field - a teleport, a new
        // level - starts from still water rather than dragging old rings along.
        const FLOAT OX = RdxMath::RippleOrigin(RippleCameraX);
        const FLOAT OY = RdxMath::RippleOrigin(RippleCameraY);
        FRippleStep K;
        appMemzero(&K, sizeof(K));
        if (bRippleOriginValid)
        {
            K.ShiftX = appRound((OX - RippleOriginX) / RdxMath::RippleTexel);
            K.ShiftY = appRound((OY - RippleOriginY) / RdxMath::RippleTexel);
            if (Abs(K.ShiftX) >= RdxMath::RippleTexels || Abs(K.ShiftY) >= RdxMath::RippleTexels)
                K.ShiftX = K.ShiftY = RdxMath::RippleTexels;          // everything read comes from outside: zero
        }
        K.Keep   = RdxMath::RippleKeep;
        K.Limit  = RdxMath::RippleLimit;
        K.Speed2 = RdxMath::RippleSpeed2;
        K.Pulses = (UINT)RipplePulseCount;
        for (INT p = 0; p < RipplePulseCount; p++)
        {
            K.Pulse[p * 4 + 0] = (RipplePulses[p * 4 + 0] - OX) / RdxMath::RippleTexel;
            K.Pulse[p * 4 + 1] = (RipplePulses[p * 4 + 1] - OY) / RdxMath::RippleTexel;
            K.Pulse[p * 4 + 2] = Max(RipplePulses[p * 4 + 2] / RdxMath::RippleTexel, 1.0f);
            K.Pulse[p * 4 + 3] = RipplePulses[p * 4 + 3];
        }

        D3D12_VIEWPORT Vp;
        Vp.TopLeftX = 0.0f;
        Vp.TopLeftY = 0.0f;
        Vp.Width    = (FLOAT)RdxMath::RippleTexels;
        Vp.Height   = (FLOAT)RdxMath::RippleTexels;
        Vp.MinDepth = 0.0f;
        Vp.MaxDepth = 1.0f;
        D3D12_RECT Scissor;
        Scissor.left   = 0;
        Scissor.top    = 0;
        Scissor.right  = RdxMath::RippleTexels;
        Scissor.bottom = RdxMath::RippleTexels;
        ID3D12DescriptorHeap* Heaps[1] = { RippleSrvHeap.Get() };
        Cmd->SetDescriptorHeaps(1, Heaps);
        Cmd->SetGraphicsRootSignature(RippleRoot.Get());
        Cmd->SetPipelineState(RipplePipeline.Get());
        Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Cmd->RSSetViewports(1, &Vp);
        Cmd->RSSetScissorRects(1, &Scissor);
        GpuTime(Cmd, 14);
        for (INT s = 0; s < Steps; s++)
        {
            const INT Src = RippleCurrent, Dst = 1 - RippleCurrent;
            B.Transition.pResource   = RippleField[Dst].Get();
            B.Transition.StateBefore = GWaterFieldRead;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            Cmd->ResourceBarrier(1, &B);
            D3D12_CPU_DESCRIPTOR_HANDLE Rtv = Rtv0;
            Rtv.ptr += Dst * RtvStep;
            Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
            D3D12_GPU_DESCRIPTOR_HANDLE Srv = RippleSrvHeap->GetGPUDescriptorHandleForHeapStart();
            Srv.ptr += Src * SrvStep;
            Cmd->SetGraphicsRootDescriptorTable(0, Srv);
            Cmd->SetGraphicsRoot32BitConstants(1, sizeof(FRippleStep) / 4, &K, 0);
            Cmd->DrawInstanced(3, 1, 0, 0);
            B.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            B.Transition.StateAfter  = GWaterFieldRead;
            Cmd->ResourceBarrier(1, &B);
            RippleCurrent = Dst;
            // The move belongs to the first step alone. The pushes go into every step: a thing in the water pushes for as long
            // as it is there - so many pushes a second, whatever the frame rate (SetRipples keeps the last frame's alone).
            K.ShiftX = K.ShiftY = 0;
        }
        GpuTime(Cmd, 15);
        static INT Told = 0;
        if (RipplePulseCount > 0 && Told < 3)
        {
            Told++;
            RdxLog(TEXT("water: ripples - %i steps, %i pushes, the first at texel %.1f, %.1f (radius %.1f, push %.2f); the field's corner %.0f, %.0f"),
                   Steps, RipplePulseCount, (RipplePulses[0] - OX) / RdxMath::RippleTexel,
                   (RipplePulses[1] - OY) / RdxMath::RippleTexel, RipplePulses[2] / RdxMath::RippleTexel, RipplePulses[3], OX, OY);
        }
        RippleOriginX      = OX;
        RippleOriginY      = OY;
        bRippleOriginValid = 1;
        RipplePulseCount   = 0;
    }
    Solid.SetWaterTable(WaterSlot(RippleCurrent, 0));
}

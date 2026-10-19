// SecondRevisionHost64 - the XeSS backend. See Host64Xess.h.
#include "Host64Xess.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "xess/xess.h"
#include "xess/xess_d3d12.h"

// libxess.dll's calls, taken from the DLL itself: the host is linked to nothing of XeSS's, so it starts without it.
namespace
{
    decltype(&xessGetVersion)              PGetVersion      = nullptr;
    decltype(&xessD3D12CreateContext)      PCreateContext   = nullptr;
    decltype(&xessD3D12Init)               PInit            = nullptr;
    decltype(&xessD3D12Execute)            PExecute         = nullptr;
    decltype(&xessDestroyContext)          PDestroyContext  = nullptr;
    decltype(&xessForceLegacyScaleFactors) PForceLegacy     = nullptr;
    decltype(&xessIsOptimalDriver)         PIsOptimalDriver = nullptr;
    decltype(&xessSetLoggingCallback)      PSetLogging      = nullptr;

    FHostXess::FLogFunc GLogTo = nullptr;

    // XeSS's own warnings and errors, into the host's log.
    void XessSays(const char* Message, xess_logging_level_t Level)
    {
        if (!GLogTo || !Message)
            return;
        char Line[512];
        sprintf_s(Line, "xess %s: %s", (Level >= XESS_LOGGING_LEVEL_ERROR) ? "error" : "warning", Message);
        GLogTo(Line);
    }

    template <typename T> bool Take(HMODULE Library, T& Out, const char* Name)
    {
        Out = (T)::GetProcAddress(Library, Name);
        return Out != nullptr;
    }
}

bool FHostXess::Load(ID3D12Device* InDevice, FLogFunc Log, char* Why, size_t WhyChars)
{
    Shutdown();
    Device = InDevice;
    GLogTo = Log;

    // Beside the host, by its full path: no other folder's libxess.dll is taken for it, and what it needs itself is
    // looked for beside it first.
    wchar_t     Path[MAX_PATH];
    const DWORD N     = ::GetModuleFileNameW(nullptr, Path, MAX_PATH);
    wchar_t*    Slash = (N > 0 && N < MAX_PATH) ? wcsrchr(Path, L'\\') : nullptr;
    if (!Slash || (size_t)(Slash - Path) + 16 >= MAX_PATH)
    {
        sprintf_s(Why, WhyChars, "xess: no folder for its DLL");
        return false;
    }
    wcscpy_s(Slash + 1, MAX_PATH - (size_t)(Slash + 1 - Path), L"libxess.dll");
    Library = ::LoadLibraryExW(Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!Library)
    {
        sprintf_s(Why, WhyChars,
                  "xess: libxess.dll would not load beside the host (%lu) - it is missing, or the Visual C++ runtime "
                  "it needs is not installed",
                  ::GetLastError());
        return false;
    }
    if (!Take(Library, PGetVersion, "xessGetVersion") || !Take(Library, PCreateContext, "xessD3D12CreateContext") ||
        !Take(Library, PInit, "xessD3D12Init") || !Take(Library, PExecute, "xessD3D12Execute") ||
        !Take(Library, PDestroyContext, "xessDestroyContext") ||
        !Take(Library, PForceLegacy, "xessForceLegacyScaleFactors"))
    {
        sprintf_s(Why, WhyChars, "xess: libxess.dll lacks a call this host needs - another version of it?");
        Shutdown();
        return false;
    }
    Take(Library, PIsOptimalDriver, "xessIsOptimalDriver");
    Take(Library, PSetLogging, "xessSetLoggingCallback");

    xess_version_t V = {};
    PGetVersion(&V);
    sprintf_s(Version, "XeSS %u.%u.%u", (unsigned)V.major, (unsigned)V.minor, (unsigned)V.patch);

    xess_context_handle_t H = nullptr;
    const xess_result_t   R = PCreateContext(Device, &H);
    if (R != XESS_RESULT_SUCCESS || !H)
    {
        // Another card without Shader Model 6.4, or a driver XeSS will not work with: the usual answers.
        sprintf_s(Why, WhyChars, "xess: %s will not run on this card (%d)%s", Version, (int)R,
                  (R == XESS_RESULT_ERROR_UNSUPPORTED_DEVICE)   ? " - it needs Shader Model 6.4"
                  : (R == XESS_RESULT_ERROR_UNSUPPORTED_DRIVER) ? " - the driver is too old for it"
                                                                : "");
        Shutdown();
        return false;
    }
    Context = H;
    if (PSetLogging)
        PSetLogging(H, XESS_LOGGING_LEVEL_WARNING, XessSays);
    // The renderer's modes draw at 1.0, 1.5, 1.7 and 2.0 of the output: XeSS's older factors, not its newer ones.
    PForceLegacy(H, true);
    if (Log)
    {
        char Line[160];
        sprintf_s(Line, "xess: %s up%s", Version,
                  (PIsOptimalDriver && PIsOptimalDriver(H) == XESS_RESULT_WARNING_OLD_DRIVER)
                      ? " - the driver is older than it would like"
                      : "");
        Log(Line);
    }
    return true;
}

void FHostXess::Prepare(uint32_t InRenderW, uint32_t InRenderH, uint32_t InOutW, uint32_t InOutH, bool bHdr)
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

// XeSS's quality setting from how much smaller the render is than the output - the ratios the renderer's modes use,
// which are XeSS's older factors (ForceLegacyScaleFactors).
static xess_quality_settings_t QualityFor(uint32_t RenderW, uint32_t OutW)
{
    const float Ratio = RenderW ? (float)OutW / (float)RenderW : 1.0f;
    if (Ratio < 1.15f)
        return XESS_QUALITY_SETTING_AA;
    if (Ratio < 1.4f)
        return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    if (Ratio < 1.6f)
        return XESS_QUALITY_SETTING_QUALITY;
    if (Ratio < 1.85f)
        return XESS_QUALITY_SETTING_BALANCED;
    if (Ratio < 2.5f)
        return XESS_QUALITY_SETTING_PERFORMANCE;
    return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
}

bool FHostXess::Make()
{
    if (bMade && !bStale)
        return true;
    if (!Context || !RenderW || !OutW)
        return false;
    xess_d3d12_init_params_t P = {};
    P.outputResolution.x       = OutW;
    P.outputResolution.y       = OutH;
    P.qualitySetting           = QualityFor(RenderW, OutW);
    // The depth 1 nearest; the colour display-referred unless the frames say HDR; the motion at the render size and
    // without the jitter's - XeSS's own default.
    P.initFlags = (uint32_t)XESS_INIT_FLAG_INVERTED_DEPTH | (bHdrNow ? 0u : (uint32_t)XESS_INIT_FLAG_LDR_INPUT_COLOR);
    const xess_result_t R = PInit((xess_context_handle_t)Context, &P);
    char Line[160];
    if (R != XESS_RESULT_SUCCESS)
    {
        if (GLogTo)
        {
            sprintf_s(Line, "xess: it would not be made for %ux%u -> %ux%u (%d)", RenderW, RenderH, OutW, OutH, (int)R);
            GLogTo(Line);
        }
        bMade = false;
        return false;
    }
    bMade  = true;
    bStale = false;
    if (GLogTo)
    {
        sprintf_s(Line, "xess: made for %ux%u -> %ux%u%s", RenderW, RenderH, OutW, OutH, bHdrNow ? ", HDR" : "");
        GLogTo(Line);
    }
    return true;
}

bool FHostXess::Upscale(ID3D12GraphicsCommandList* List, ID3D12Resource* const R[Host64::Resource_Count],
                        const Host64::FFrame& F)
{
    if (!Make())
        return false;
    xess_d3d12_execute_params_t E = {};
    E.pColorTexture               = R[Host64::Resource_Color];
    E.pVelocityTexture            = R[Host64::Resource_Motion];
    E.pDepthTexture               = R[Host64::Resource_Depth];
    E.pOutputTexture              = R[Host64::Resource_Output];
    E.jitterOffsetX               = F.JitterX;
    E.jitterOffsetY               = F.JitterY;
    E.exposureScale               = 1.0f;
    E.resetHistory                = (F.Flags & Host64::FrameFlag_Reset) ? 1u : 0u;
    E.inputWidth                  = F.RenderWidth;
    E.inputHeight                 = F.RenderHeight;
    return PExecute((xess_context_handle_t)Context, List, &E) == XESS_RESULT_SUCCESS;
}

void FHostXess::Shutdown()
{
    if (Context && PDestroyContext)
        PDestroyContext((xess_context_handle_t)Context);
    Context = nullptr;
    if (Library)
        ::FreeLibrary(Library);
    Library = nullptr;
    Device  = nullptr;
    bMade   = false;
    bStale  = false;
    RenderW = RenderH = OutW = OutH = 0;
    Version[0] = 0;
}

#pragma once

// Asks Intel's graphics driver whether adaptive sync (VESA Adaptive-Sync / HDMI VRR) is ENABLED on one screen.
//
// Shared by the renderer (VrrProbe.cpp) and the standalone VrrCheck tool, so that what the tool reports on a machine
// is exactly what the renderer will decide there. Plain Win32 and Intel's own header - nothing of the engine.
//
// Intel's control library is a DLL the driver installs (ControlLib32.dll for a 32-bit process); it is loaded here at
// run time, so a machine without it simply gets "no answer". Its display properties carry two sets of feature flags,
// SUPPORTED and ENABLED, and the enabled one is the question. The library names a screen by its Windows target ID;
// Windows' own display configuration turns that into the "\\.\DISPLAY1" name everything else uses.

#include <windows.h>
#include "igcl_api.h"       // Intel Graphics Control Library header - external/IGCL, see THIRD-PARTY-NOTICES.md

// Returns 1 when adaptive sync is enabled on DeviceName, 0 when it is not, -1 when there is no answer (no Intel
// driver, or none of its screens is DeviceName). Report, when given, receives a line for every Intel screen found -
// the tool prints them, the renderer passes nothing.
inline int IntelVrrForDisplay(const wchar_t* DeviceName, void (*Report)(const char* Line))
{
    HMODULE Lib = ::LoadLibraryW((sizeof(void*) == 8) ? L"ControlLib.dll" : L"ControlLib32.dll");
    if (!Lib)
        return -1;
    const ctl_pfnInit_t                    Init     = (ctl_pfnInit_t)::GetProcAddress(Lib, "ctlInit");
    const ctl_pfnClose_t                   Close    = (ctl_pfnClose_t)::GetProcAddress(Lib, "ctlClose");
    const ctl_pfnEnumerateDevices_t        Devices  = (ctl_pfnEnumerateDevices_t)::GetProcAddress(Lib, "ctlEnumerateDevices");
    const ctl_pfnEnumerateDisplayOutputs_t Outputs  = (ctl_pfnEnumerateDisplayOutputs_t)::GetProcAddress(Lib, "ctlEnumerateDisplayOutputs");
    const ctl_pfnGetDeviceProperties_t     DevProps = (ctl_pfnGetDeviceProperties_t)::GetProcAddress(Lib, "ctlGetDeviceProperties");
    const ctl_pfnGetDisplayProperties_t    Props    = (ctl_pfnGetDisplayProperties_t)::GetProcAddress(Lib, "ctlGetDisplayProperties");
    if (!Init || !Close || !Devices || !Outputs || !DevProps || !Props)
    {
        ::FreeLibrary(Lib);
        return -1;
    }

    ctl_init_args_t Args;
    ::ZeroMemory(&Args, sizeof(Args));
    Args.Size       = sizeof(Args);
    Args.AppVersion = CTL_IMPL_VERSION;
    ctl_api_handle_t Api = nullptr;
    if (Init(&Args, &Api) != CTL_RESULT_SUCCESS || !Api)
    {
        ::FreeLibrary(Lib);
        return -1;
    }

    // Windows' active display paths: the bridge from Intel's target IDs to the screens' names.
    UINT32 PathCount = 0, ModeCount = 0;
    DISPLAYCONFIG_PATH_INFO* Paths = nullptr;
    DISPLAYCONFIG_MODE_INFO* Modes = nullptr;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &PathCount, &ModeCount) == ERROR_SUCCESS)
    {
        Paths = new DISPLAYCONFIG_PATH_INFO[PathCount ? PathCount : 1];
        Modes = new DISPLAYCONFIG_MODE_INFO[ModeCount ? ModeCount : 1];
        if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &PathCount, Paths, &ModeCount, Modes, nullptr) != ERROR_SUCCESS)
            PathCount = 0;
    }

    int Answer = -1;
    uint32_t DeviceCount = 0;
    if (Devices(Api, &DeviceCount, nullptr) == CTL_RESULT_SUCCESS && DeviceCount)
    {
        ctl_device_adapter_handle_t* Dev = new ctl_device_adapter_handle_t[DeviceCount];
        if (Devices(Api, &DeviceCount, Dev) == CTL_RESULT_SUCCESS)
        {
            for (uint32_t d = 0; d < DeviceCount; d++)
            {
                // The adapter's LUID, so a target ID is matched on the right adapter of a machine with several.
                LUID Luid;
                ::ZeroMemory(&Luid, sizeof(Luid));
                ctl_device_adapter_properties_t DP;
                ::ZeroMemory(&DP, sizeof(DP));
                DP.Size           = sizeof(DP);
                DP.pDeviceID      = &Luid;
                DP.device_id_size = sizeof(Luid);
                const bool bLuid  = DevProps(Dev[d], &DP) == CTL_RESULT_SUCCESS;

                uint32_t OutCount = 0;
                if (Outputs(Dev[d], &OutCount, nullptr) != CTL_RESULT_SUCCESS || !OutCount)
                    continue;
                ctl_display_output_handle_t* Out = new ctl_display_output_handle_t[OutCount];
                if (Outputs(Dev[d], &OutCount, Out) == CTL_RESULT_SUCCESS)
                {
                    for (uint32_t o = 0; o < OutCount; o++)
                    {
                        ctl_display_properties_t P;
                        ::ZeroMemory(&P, sizeof(P));
                        P.Size = sizeof(P);
                        if (Props(Out[o], &P) != CTL_RESULT_SUCCESS ||
                            !(P.DisplayConfigFlags & CTL_DISPLAY_CONFIG_FLAG_DISPLAY_ACTIVE))
                            continue;
                        // Intel's target ID to Windows' name for the same screen.
                        wchar_t Name[CCHDEVICENAME] = L"";
                        for (UINT32 p = 0; p < PathCount; p++)
                        {
                            if (Paths[p].targetInfo.id != P.Os_display_encoder_handle.WindowsDisplayEncoderID)
                                continue;
                            if (bLuid && (Paths[p].targetInfo.adapterId.LowPart != Luid.LowPart ||
                                          Paths[p].targetInfo.adapterId.HighPart != Luid.HighPart))
                                continue;
                            DISPLAYCONFIG_SOURCE_DEVICE_NAME Source;
                            ::ZeroMemory(&Source, sizeof(Source));
                            Source.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                            Source.header.size      = sizeof(Source);
                            Source.header.adapterId = Paths[p].sourceInfo.adapterId;
                            Source.header.id        = Paths[p].sourceInfo.id;
                            if (::DisplayConfigGetDeviceInfo(&Source.header) == ERROR_SUCCESS)
                                ::lstrcpynW(Name, Source.viewGdiDeviceName, CCHDEVICENAME);
                            break;
                        }
                        const bool bSupported = (P.FeatureSupportedFlags & CTL_STD_DISPLAY_FEATURE_FLAG_ADAPTIVESYNC_VRR) != 0;
                        const bool bEnabled   = (P.FeatureEnabledFlags & CTL_STD_DISPLAY_FEATURE_FLAG_ADAPTIVESYNC_VRR) != 0;
                        if (Report)
                        {
                            char Line[256];
                            char NameA[64];
                            ::WideCharToMultiByte(CP_ACP, 0, Name[0] ? Name : L"(no Windows name found)", -1, NameA,
                                                  sizeof(NameA), nullptr, nullptr);
                            ::wsprintfA(Line, "  %s: target id %u, ADAPTIVE SYNC supported %d, ENABLED %d",
                                        NameA, P.Os_display_encoder_handle.WindowsDisplayEncoderID,
                                        bSupported ? 1 : 0, bEnabled ? 1 : 0);
                            Report(Line);
                        }
                        if (Name[0] && DeviceName && ::lstrcmpiW(Name, DeviceName) == 0)
                            Answer = bEnabled ? 1 : 0;
                    }
                }
                delete[] Out;
            }
        }
        delete[] Dev;
    }
    delete[] Paths;
    delete[] Modes;
    Close(Api);
    ::FreeLibrary(Lib);
    return Answer;
}

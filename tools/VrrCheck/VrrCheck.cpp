// VrrCheck - a small standalone tool that asks every source on this machine what it knows about the screens, and
// above all whether they run with variable refresh (FreeSync, G-Sync, Adaptive-Sync).
//
// It exists because the renderer has to decide how far under the refresh rate to hold the frame rate, and Windows
// itself will not say whether a screen varies its refresh: only the graphics driver knows. This tool can be carried
// to a machine the game cannot be carried to - an Intel one, say - and it writes everything it finds into
// VrrCheck.log beside itself.
//
// It needs nothing installed: no game, no renderer. Run it, then send the log back.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <dxgi1_6.h>

#include "amd_ags.h"    // AMD GPU Services, MIT
#include "nvapi.h"      // NVAPI, MIT
#include "IntelVrr.h"   // the renderer's own Intel query, the very same code

#pragma comment(lib, "dxgi.lib")

static FILE* GLog;

static void Say(const char* Fmt, ...)
{
    char Line[2048];
    va_list Args;
    va_start(Args, Fmt);
    _vsnprintf_s(Line, sizeof(Line), _TRUNCATE, Fmt, Args);
    va_end(Args);
    printf("%s\n", Line);
    if (GLog)
    {
        fputs(Line, GLog);
        fputc('\n', GLog);
        fflush(GLog);
    }
}

static void SayW(const wchar_t* Fmt, ...)
{
    wchar_t Line[2048];
    va_list Args;
    va_start(Args, Fmt);
    _vsnwprintf_s(Line, 2048, _TRUNCATE, Fmt, Args);
    va_end(Args);
    char Ansi[2048];
    ::WideCharToMultiByte(CP_ACP, 0, Line, -1, Ansi, sizeof(Ansi), nullptr, nullptr);
    Say("%s", Ansi);
}

// ---------------------------------------------------------------------------
// What Windows knows: the screens, their modes, and the names every other library will use to talk about them.
// ---------------------------------------------------------------------------

static int  GMonitorCount;
static WCHAR GMonitorDevice[16][32];

static BOOL CALLBACK OneMonitor(HMONITOR Mon, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW Info;
    Info.cbSize = sizeof(Info);
    if (!::GetMonitorInfoW(Mon, &Info) || GMonitorCount >= 16)
        return TRUE;
    DEVMODEW Mode;
    ZeroMemory(&Mode, sizeof(Mode));
    Mode.dmSize = sizeof(Mode);
    const BOOL bMode = ::EnumDisplaySettingsW(Info.szDevice, ENUM_CURRENT_SETTINGS, &Mode);

    // The friendly name (what the monitor calls itself) comes from the second level of the display device list.
    DISPLAY_DEVICEW Dev;
    ZeroMemory(&Dev, sizeof(Dev));
    Dev.cb = sizeof(Dev);
    WCHAR Friendly[128] = L"(no name)";
    WCHAR DeviceId[256] = L"";
    if (::EnumDisplayDevicesW(Info.szDevice, 0, &Dev, 0))
    {
        wcscpy_s(Friendly, Dev.DeviceString);
        wcscpy_s(DeviceId, Dev.DeviceID);
    }
    wcscpy_s(GMonitorDevice[GMonitorCount], Info.szDevice);
    GMonitorCount++;

    SayW(L"  %s  %s", Info.szDevice, Friendly);
    SayW(L"      %ix%i at %i,%i, %i Hz, %i bits%s", bMode ? (int)Mode.dmPelsWidth : 0,
         bMode ? (int)Mode.dmPelsHeight : 0, Info.rcMonitor.left, Info.rcMonitor.top,
         bMode ? (int)Mode.dmDisplayFrequency : 0, bMode ? (int)Mode.dmBitsPerPel : 0,
         (Info.dwFlags & MONITORINFOF_PRIMARY) ? L", primary" : L"");
    if (DeviceId[0])
        SayW(L"      %s", DeviceId);
    return TRUE;
}

// ---------------------------------------------------------------------------

static void Dxgi()
{
    Say("");
    Say("DXGI");
    IDXGIFactory1* Factory = nullptr;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&Factory)) || !Factory)
    {
        Say("  no factory - DXGI is not answering");
        return;
    }
    IDXGIFactory5* Factory5 = nullptr;
    if (SUCCEEDED(Factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&Factory5)) && Factory5)
    {
        BOOL Allow = FALSE;
        const HRESULT Hr = Factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &Allow, sizeof(Allow));
        Say("  tearing presents allowed: %s%s", (SUCCEEDED(Hr) && Allow) ? "yes" : "no",
            SUCCEEDED(Hr) ? "" : " (the question itself failed)");
        Say("    (this is an operating system and driver matter - it says NOTHING about the screens)");
        Factory5->Release();
    }
    IDXGIAdapter1* Adapter = nullptr;
    for (UINT i = 0; Factory->EnumAdapters1(i, &Adapter) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 Desc;
        if (SUCCEEDED(Adapter->GetDesc1(&Desc)))
            SayW(L"  adapter %u: %s (vendor 0x%04X, device 0x%04X)", i, Desc.Description, Desc.VendorId, Desc.DeviceId);
        IDXGIOutput* Out = nullptr;
        for (UINT o = 0; Adapter->EnumOutputs(o, &Out) != DXGI_ERROR_NOT_FOUND; o++)
        {
            DXGI_OUTPUT_DESC OutDesc;
            if (SUCCEEDED(Out->GetDesc(&OutDesc)))
                SayW(L"      output %u: %s, attached %i", o, OutDesc.DeviceName, OutDesc.AttachedToDesktop ? 1 : 0);
            Out->Release();
        }
        Adapter->Release();
    }
    Factory->Release();
}

// ---------------------------------------------------------------------------

static void Amd()
{
    Say("");
    Say("AMD (AGS)");
    AGSContext* Ctx = nullptr;
    AGSGPUInfo  Info;
    ZeroMemory(&Info, sizeof(Info));
    const AGSReturnCode Rc = agsInitialize(AGS_CURRENT_VERSION, nullptr, &Ctx, &Info);
    if (Rc != AGS_SUCCESS || !Ctx)
    {
        Say("  not available here (agsInitialize returned %i) - no AMD driver, or no AMD GPU", (int)Rc);
        return;
    }
    Say("  driver %s, radeon software %s, %i device(s)", Info.driverVersion ? Info.driverVersion : "?",
        Info.radeonSoftwareVersion ? Info.radeonSoftwareVersion : "?", Info.numDevices);
    for (int d = 0; d < Info.numDevices; d++)
    {
        const AGSDeviceInfo& Dev = Info.devices[d];
        Say("  device %i: %s, %i display(s)%s", d, Dev.adapterString ? Dev.adapterString : "?", Dev.numDisplays,
            Dev.isPrimaryDevice ? ", primary" : "");
        for (int s = 0; s < Dev.numDisplays && Dev.displays; s++)
        {
            const AGSDisplayInfo& Disp = Dev.displays[s];
            Say("    display %i: %s [%s]", s, Disp.name ? Disp.name : "?", Disp.displayDeviceName);
            Say("        FREESYNC %i, freesync HDR %i, HDR10 %i, dolby vision %i, eyefinity %i",
                (int)Disp.freesync, (int)Disp.freesyncHDR, (int)Disp.HDR10, (int)Disp.dolbyVision,
                (int)Disp.eyefinityInGroup);
            Say("        now %ix%i at %i,%i, %.1f Hz; the display can do up to %ix%i at %.1f Hz; primary %i",
                Disp.currentResolution.width, Disp.currentResolution.height, Disp.currentResolution.offsetX,
                Disp.currentResolution.offsetY, Disp.currentRefreshRate, Disp.maxResolutionX, Disp.maxResolutionY,
                Disp.maxRefreshRate, (int)Disp.isPrimaryDisplay);
        }
    }
    agsDeInitialize(Ctx);
}

// ---------------------------------------------------------------------------

static void Nvidia()
{
    Say("");
    Say("NVIDIA (NVAPI)");
    if (NvAPI_Initialize() != NVAPI_OK)
    {
        Say("  not available here - no NVIDIA driver, or no NVIDIA GPU");
        return;
    }
    NvAPI_ShortString Version = { 0 };
    if (NvAPI_GetInterfaceVersionString(Version) == NVAPI_OK)
        Say("  %s", Version);
    for (int m = 0; m < GMonitorCount; m++)
    {
        char Name[32];
        ::WideCharToMultiByte(CP_ACP, 0, GMonitorDevice[m], -1, Name, sizeof(Name), nullptr, nullptr);
        NvU32 Id = 0;
        const NvAPI_Status ById = NvAPI_DISP_GetDisplayIdByDisplayName(Name, &Id);
        if (ById != NVAPI_OK)
        {
            Say("  %s: not an NVIDIA display (status %i)", Name, (int)ById);
            continue;
        }
        NV_GET_ADAPTIVE_SYNC_DATA Data;
        ZeroMemory(&Data, sizeof(Data));
        Data.version = NV_GET_ADAPTIVE_SYNC_DATA_VER;
        const NvAPI_Status Got = NvAPI_DISP_GetAdaptiveSyncData(Id, &Data);
        if (Got != NVAPI_OK)
        {
            Say("  %s: display id 0x%08X, but adaptive sync data refused (status %i)", Name, Id, (int)Got);
            continue;
        }
        Say("  %s: display id 0x%08X, ADAPTIVE SYNC %s, frame splitting %s, max frame interval %u us", Name, Id,
            Data.bDisableAdaptiveSync ? "OFF" : "ON", Data.bDisableFrameSplitting ? "off" : "on",
            Data.maxFrameInterval);
    }
    NvAPI_Unload();
}

// ---------------------------------------------------------------------------
// Intel. Their control library is a DLL the driver installs; the renderer's own probe is not written yet, so this
// reports what is here to work with: which library loads and which of its entry points exist.
// ---------------------------------------------------------------------------

static void Intel()
{
    Say("");
    Say("Intel (control library)");
    const wchar_t* Names[] = { L"ControlLib32.dll", L"ControlLib.dll", L"ControlLib1.dll", L"ControlLib2.dll" };
    HMODULE Lib = nullptr;
    const wchar_t* Found = nullptr;
    for (int i = 0; i < 4 && !Lib; i++)
    {
        Lib = ::LoadLibraryW(Names[i]);
        if (Lib)
            Found = Names[i];
    }
    if (!Lib)
    {
        Say("  no control library on this machine (ControlLib32.dll / ControlLib.dll) - no Intel driver, or it is"
            " not installed");
        return;
    }
    SayW(L"  loaded %s", Found);
    const char* Wanted[] = { "ctlInit", "ctlClose", "ctlEnumerateDevices", "ctlEnumerateDisplayOutputs",
                             "ctlGetDeviceProperties", "ctlGetDisplayProperties", "ctlGetSet3DFeature",
                             "ctlGetSetDisplaySettings", "ctlGetZeDevice" };
    for (int i = 0; i < 9; i++)
        Say("    %-28s %s", Wanted[i], ::GetProcAddress(Lib, Wanted[i]) ? "present" : "-");
    ::FreeLibrary(Lib);

    // Now the renderer's own question, through the renderer's own code: every screen the driver knows, and then the
    // answer for each Windows screen in turn.
    Say("  the driver's screens:");
    IntelVrrForDisplay(nullptr, [](const char* Line) { Say("  %s", Line); });
    for (int m = 0; m < GMonitorCount; m++)
    {
        const int Answer = IntelVrrForDisplay(GMonitorDevice[m], nullptr);
        SayW(L"  %s -> %s", GMonitorDevice[m],
             (Answer < 0) ? L"no answer (NODATA)" : (Answer ? L"ADAPTIVE SYNC ON" : L"ADAPTIVE SYNC OFF"));
    }
}

// ---------------------------------------------------------------------------

int main()
{
    char Path[MAX_PATH];
    ::GetModuleFileNameA(nullptr, Path, MAX_PATH);
    char* Slash = strrchr(Path, '\\');
    if (Slash)
        strcpy_s(Slash + 1, MAX_PATH - (Slash + 1 - Path), "VrrCheck.log");
    fopen_s(&GLog, Path, "w");

    SYSTEMTIME T;
    ::GetLocalTime(&T);
    Say("VrrCheck - what this machine knows about its screens");
    Say("%04i-%02i-%02i %02i:%02i:%02i, %i-bit process", T.wYear, T.wMonth, T.wDay, T.wHour, T.wMinute, T.wSecond,
        (int)(sizeof(void*) * 8));
    Say("");
    Say("Windows");
    ::EnumDisplayMonitors(nullptr, nullptr, OneMonitor, 0);

    Dxgi();
    Amd();
    Nvidia();
    Intel();

    Say("");
    Say("Written to %s", Path);
    if (GLog)
        fclose(GLog);
    printf("\nPress Enter to close.\n");
    getchar();
    return 0;
}

#include "Pch.h"
#include "SystemReport.h"
#include "Log.h"

#include <psapi.h>
#include <intrin.h>
#include <stdio.h>

// A file's version as its own resource gives it ("1.2.3.4"), or an empty text. Through version.dll loaded here, so the
// renderer needs no more import libraries for a report only LogLevel=Debug writes.
static void FileVersion(const WCHAR* Path, WCHAR* Out, INT Max)
{
    Out[0] = 0;
    typedef DWORD(WINAPI * FSize)(LPCWSTR, LPDWORD);
    typedef BOOL(WINAPI * FInfo)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL(WINAPI * FQuery)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    static HMODULE Version = ::LoadLibraryW(L"version.dll");
    if (!Version)
        return;
    const FSize  Size  = (FSize)::GetProcAddress(Version, "GetFileVersionInfoSizeW");
    const FInfo  Info  = (FInfo)::GetProcAddress(Version, "GetFileVersionInfoW");
    const FQuery Query = (FQuery)::GetProcAddress(Version, "VerQueryValueW");
    if (!Size || !Info || !Query)
        return;
    DWORD       Handle = 0;
    const DWORD Bytes  = Size(Path, &Handle);
    if (!Bytes || Bytes > (1u << 20))
        return;
    BYTE* const Data = (BYTE*)appMalloc(Bytes, TEXT("RdxFileVersion"));
    if (!Data)
        return;
    VS_FIXEDFILEINFO* Fixed = nullptr;
    UINT              Len   = 0;
    if (Info(Path, 0, Bytes, Data) && Query(Data, L"\\", (LPVOID*)&Fixed, &Len) && Fixed &&
        Len >= sizeof(VS_FIXEDFILEINFO))
        swprintf_s(Out, Max, L"%u.%u.%u.%u", HIWORD(Fixed->dwFileVersionMS), LOWORD(Fixed->dwFileVersionMS),
                   HIWORD(Fixed->dwFileVersionLS), LOWORD(Fixed->dwFileVersionLS));
    appFree(Data);
}

static void LogWindows()
{
    typedef LONG(WINAPI * FRtlGetVersion)(OSVERSIONINFOEXW*);
    OSVERSIONINFOEXW V;
    ::ZeroMemory(&V, sizeof(V));
    V.dwOSVersionInfoSize = sizeof(V);
    // RtlGetVersion and not GetVersionEx: the latter answers what the game's manifest admits to, which is Windows 8.
    const FRtlGetVersion Get = (FRtlGetVersion)::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (Get)
        Get(&V);
    DWORD Ubr         = 0;
    WCHAR Release[64] = { 0 };
    HKEY  Key         = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                        KEY_READ | KEY_WOW64_64KEY, &Key) == ERROR_SUCCESS)
    {
        DWORD Type = 0;
        DWORD N    = sizeof(Ubr);
        if (::RegQueryValueExW(Key, L"UBR", nullptr, &Type, (BYTE*)&Ubr, &N) != ERROR_SUCCESS || Type != REG_DWORD)
            Ubr = 0;
        N = sizeof(Release) - sizeof(WCHAR);
        if (::RegQueryValueExW(Key, L"DisplayVersion", nullptr, &Type, (BYTE*)Release, &N) == ERROR_SUCCESS &&
            Type == REG_SZ)
            Release[N / sizeof(WCHAR)] = 0;
        else
            Release[0] = 0;
        ::RegCloseKey(Key);
    }
    BOOL bWow = FALSE;
    ::IsWow64Process(::GetCurrentProcess(), &bWow);
    WCHAR Locale[LOCALE_NAME_MAX_LENGTH] = { 0 };
    ::GetUserDefaultLocaleName(Locale, LOCALE_NAME_MAX_LENGTH);
    RdxLog(TEXT("system: Windows %u.%u build %u.%u%s%s%s, %s; locale %s"), (UINT)V.dwMajorVersion,
           (UINT)V.dwMinorVersion, (UINT)V.dwBuildNumber, (UINT)Ubr, Release[0] ? TEXT(" (") : TEXT(""),
           (const TCHAR*)Release, Release[0] ? TEXT(")") : TEXT(""),
           bWow ? TEXT("64-bit, the game a 32-bit process in it") : TEXT("32-bit"), (const TCHAR*)Locale);
}

static void LogProcessorAndMemory()
{
    int  Regs[4] = { 0, 0, 0, 0 };
    char Brand[49];
    ::ZeroMemory(Brand, sizeof(Brand));
    __cpuid(Regs, (int)0x80000000);
    if ((unsigned)Regs[0] >= 0x80000004u)
    {
        __cpuid((int*)(Brand + 0), (int)0x80000002);
        __cpuid((int*)(Brand + 16), (int)0x80000003);
        __cpuid((int*)(Brand + 32), (int)0x80000004);
    }
    const char* B = Brand;
    while (*B == ' ')
        B++;
    SYSTEM_INFO S;
    ::GetNativeSystemInfo(&S);
    RdxLog(TEXT("system: processor %S - %u logical processors"), *B ? B : "(no name)", (UINT)S.dwNumberOfProcessors);
    MEMORYSTATUSEX M;
    ::ZeroMemory(&M, sizeof(M));
    M.dwLength = sizeof(M);
    if (::GlobalMemoryStatusEx(&M))
        RdxLog(TEXT("system: memory %I64u MB, %I64u MB free (%u%% in use); page file %I64u MB, %I64u MB free; this "
                    "process's address space %I64u MB, %I64u MB free"),
               M.ullTotalPhys >> 20, M.ullAvailPhys >> 20, (UINT)M.dwMemoryLoad, M.ullTotalPageFile >> 20,
               M.ullAvailPageFile >> 20, M.ullTotalVirtual >> 20, M.ullAvailVirtual >> 20);
}

static void LogFiles()
{
    WCHAR Path[MAX_PATH + 1];
    WCHAR Ver[64];
    const DWORD N = ::GetModuleFileNameW(nullptr, Path, MAX_PATH);
    Path[(N < MAX_PATH) ? N : MAX_PATH] = 0;
    FileVersion(Path, Ver, 64);
    RdxLog(TEXT("system: the game %s%s%s"), (const TCHAR*)Path, Ver[0] ? TEXT(", version ") : TEXT(""),
           (const TCHAR*)Ver);
    RdxLog(TEXT("system: its command line %s"), (const TCHAR*)::GetCommandLineW());
    HMODULE Self = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)&LogFiles, &Self) &&
        Self)
    {
        const DWORD M = ::GetModuleFileNameW(Self, Path, MAX_PATH);
        Path[(M < MAX_PATH) ? M : MAX_PATH] = 0;
        WIN32_FILE_ATTRIBUTE_DATA A;
        SYSTEMTIME                T;
        ::ZeroMemory(&T, sizeof(T));
        DWORD Bytes = 0;
        if (::GetFileAttributesExW(Path, GetFileExInfoStandard, &A))
        {
            FILETIME Local;
            ::FileTimeToLocalFileTime(&A.ftLastWriteTime, &Local);
            ::FileTimeToSystemTime(&Local, &T);
            Bytes = A.nFileSizeLow;
        }
        RdxLog(TEXT("system: this renderer %s, %u bytes, written %04u-%02u-%02u %02u:%02u:%02u, built %S %S"),
               (const TCHAR*)Path, (UINT)Bytes, (UINT)T.wYear, (UINT)T.wMonth, (UINT)T.wDay, (UINT)T.wHour,
               (UINT)T.wMinute, (UINT)T.wSecond, __DATE__, __TIME__);
    }
}

// Every card the system lists, the software one too, with its driver's version as the card's own user-mode driver
// says it (CheckInterfaceSupport's answer) and every screen on it.
static void LogCards()
{
    IDXGIFactory1* Factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&Factory)) || !Factory)
    {
        RDX_LOG("system: the graphics cards cannot be listed (no DXGI factory)");
        return;
    }
    for (UINT a = 0;; a++)
    {
        IDXGIAdapter1* Adapter = nullptr;
        if (Factory->EnumAdapters1(a, &Adapter) == DXGI_ERROR_NOT_FOUND || !Adapter)
            break;
        DXGI_ADAPTER_DESC1 D;
        ::ZeroMemory(&D, sizeof(D));
        Adapter->GetDesc1(&D);
        D.Description[127] = 0;
        LARGE_INTEGER Umd;
        Umd.QuadPart        = 0;
        const UBOOL bDriver = SUCCEEDED(Adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &Umd)) ? 1 : 0;
        // The memory sizes as a 32-bit process is told them: DXGI cuts them below 4 GB, so a 16 GB card reads 3 GB here.
        RdxLog(TEXT("system: card %u: %s - vendor 0x%04X device 0x%04X subsystem 0x%08X revision %u; %u MB of its own, "
                    "%u MB shared (as a 32-bit process is told: under 4 GB)%s; driver %u.%u.%u.%u%s"),
               a, (const TCHAR*)D.Description, (UINT)D.VendorId, (UINT)D.DeviceId, (UINT)D.SubSysId, (UINT)D.Revision,
               (UINT)(D.DedicatedVideoMemory >> 20), (UINT)(D.SharedSystemMemory >> 20),
               (D.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? TEXT(", software") : TEXT(""),
               (UINT)HIWORD(Umd.HighPart), (UINT)LOWORD(Umd.HighPart), (UINT)HIWORD(Umd.LowPart),
               (UINT)LOWORD(Umd.LowPart), bDriver ? TEXT("") : TEXT(" (the card would not say)"));
        for (UINT o = 0;; o++)
        {
            IDXGIOutput* Output = nullptr;
            if (Adapter->EnumOutputs(o, &Output) == DXGI_ERROR_NOT_FOUND || !Output)
                break;
            DXGI_OUTPUT_DESC Od;
            ::ZeroMemory(&Od, sizeof(Od));
            Output->GetDesc(&Od);
            Od.DeviceName[31] = 0;
            RdxLog(TEXT("system: card %u screen %u: %s at %i,%i..%i,%i%s, rotation %u"), a, o,
                   (const TCHAR*)Od.DeviceName, (INT)Od.DesktopCoordinates.left, (INT)Od.DesktopCoordinates.top,
                   (INT)Od.DesktopCoordinates.right, (INT)Od.DesktopCoordinates.bottom,
                   Od.AttachedToDesktop ? TEXT("") : TEXT(" (not on the desktop)"), (UINT)Od.Rotation);
            Output->Release();
        }
        Adapter->Release();
    }
    Factory->Release();
}

static BOOL CALLBACK LogScreen(HMONITOR Monitor, HDC, LPRECT, LPARAM Param)
{
    INT&          Index = *(INT*)Param;
    MONITORINFOEXW Mi;
    ::ZeroMemory(&Mi, sizeof(Mi));
    Mi.cbSize = sizeof(Mi);
    if (!::GetMonitorInfoW(Monitor, &Mi))
        return TRUE;
    DEVMODEW Mode;
    ::ZeroMemory(&Mode, sizeof(Mode));
    Mode.dmSize = sizeof(Mode);
    const BOOL bMode = ::EnumDisplaySettingsW(Mi.szDevice, ENUM_CURRENT_SETTINGS, &Mode);
    typedef HRESULT(WINAPI * FGetDpi)(HMONITOR, INT, UINT*, UINT*);
    static HMODULE Shcore = ::LoadLibraryW(L"shcore.dll");
    const FGetDpi  GetDpi = Shcore ? (FGetDpi)::GetProcAddress(Shcore, "GetDpiForMonitor") : nullptr;
    UINT           DpiX = 0, DpiY = 0;
    if (GetDpi)
        GetDpi(Monitor, 0 /* MDT_EFFECTIVE_DPI */, &DpiX, &DpiY);
    RdxLog(TEXT("system: screen %i: %s%s at %i,%i..%i,%i; mode %ux%u, %u Hz, %u bits; scaling %u%%"), Index,
           (const TCHAR*)Mi.szDevice, (Mi.dwFlags & MONITORINFOF_PRIMARY) ? TEXT(" (the main one)") : TEXT(""),
           (INT)Mi.rcMonitor.left, (INT)Mi.rcMonitor.top, (INT)Mi.rcMonitor.right, (INT)Mi.rcMonitor.bottom,
           bMode ? (UINT)Mode.dmPelsWidth : 0u, bMode ? (UINT)Mode.dmPelsHeight : 0u,
           bMode ? (UINT)Mode.dmDisplayFrequency : 0u, bMode ? (UINT)Mode.dmBitsPerPel : 0u,
           DpiX ? (DpiX * 100u + 48u) / 96u : 0u);
    Index++;
    return TRUE;
}

// Every module in the game's process, in load order, with its version - and which of them are not Windows' own or
// the game's: an overlay, a recorder, an injected wrapper. A d3d12.dll or dxgi.dll from anywhere but the system folder
// is the first thing to look at in a fault no one else has.
static void LogModules()
{
    HMODULE Mods[1024];
    DWORD   Needed = 0;
    if (!::EnumProcessModules(::GetCurrentProcess(), Mods, sizeof(Mods), &Needed))
    {
        RDX_LOG("system: the modules cannot be listed");
        return;
    }
    WCHAR Windows[MAX_PATH + 1];
    UINT  WinLen = ::GetSystemWindowsDirectoryW(Windows, MAX_PATH);
    if (WinLen >= MAX_PATH)
        WinLen = 0;
    WCHAR Game[MAX_PATH + 1];
    DWORD GameLen = ::GetModuleFileNameW(nullptr, Game, MAX_PATH);
    if (GameLen >= MAX_PATH)
        GameLen = 0;
    Game[GameLen] = 0;
    // The game's own folder: everything up to the last separator of the exe's path, and one step up - Revision's
    // modules sit in its System folder and the base game's beside it.
    INT Cut = -1;
    for (INT i = 0; i < (INT)GameLen; i++)
        if (Game[i] == L'\\' || Game[i] == L'/')
            Cut = i;
    INT Up = -1;
    for (INT i = 0; i < Cut; i++)
        if (Game[i] == L'\\' || Game[i] == L'/')
            Up = i;
    const INT GameRoot = (Up > 0) ? Up : Cut;
    const INT Count    = (INT)(Needed / sizeof(HMODULE));
    RdxLog(TEXT("system: %i modules loaded into the game"), Count);
    INT Strangers = 0;
    for (INT m = 0; m < Count && m < 1024; m++)
    {
        WCHAR       Path[MAX_PATH + 1];
        const DWORD N = ::GetModuleFileNameExW(::GetCurrentProcess(), Mods[m], Path, MAX_PATH);
        if (!N)
            continue;
        Path[(N < MAX_PATH) ? N : MAX_PATH] = 0;
        WCHAR Ver[64];
        FileVersion(Path, Ver, 64);
        const UBOOL bWindows = WinLen && !_wcsnicmp(Path, Windows, WinLen);
        const UBOOL bGame    = GameRoot > 0 && !_wcsnicmp(Path, Game, GameRoot);
        if (!bWindows && !bGame)
            Strangers++;
        RdxLog(TEXT("system: module %s%s%s%s"), (const TCHAR*)Path, Ver[0] ? TEXT(" ") : TEXT(""), (const TCHAR*)Ver,
               (bWindows || bGame) ? TEXT("") : TEXT("  <- neither Windows' nor the game's"));
    }
    RdxLog(TEXT("system: %i of them neither Windows' nor the game's"), Strangers);
}

void RdxLogSystemReport()
{
    RDX_LOG("system: LogLevel=Debug - what the machine is, before anything is made on it");
    LogWindows();
    LogProcessorAndMemory();
    LogFiles();
    LogCards();
    INT Index = 0;
    ::EnumDisplayMonitors(nullptr, nullptr, LogScreen, (LPARAM)&Index);
    LogModules();
}

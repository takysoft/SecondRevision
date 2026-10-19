#include "Pch.h"
#include "VrrProbe.h"
#include "RenderMath.h"
#include "Log.h"

// The engine's own clock()/unclock() macros break anything that includes the C runtime's headers, and both vendor
// headers do (docs/nyitott-hibak.md, the FSR integration). They go away for the length of this file.
#undef clock
#undef unclock

#include <stdio.h>
#include <string.h>

#include "amd_ags.h"        // AMD GPU Services, MIT - external/AGS_SDK
#include "nvapi.h"          // NVAPI, MIT - external/NVAPI
#include "IntelVrr.h"       // Intel's control library, loaded at run time - shared with tools/VrrCheck

// Which screen: Windows' own device name for the monitor the window is on, "\\.\DISPLAY1" and the like. Both vendor
// libraries speak that name, which is what makes a per-screen answer possible at all.
static UBOOL MonitorDeviceName(HWND Window, char* Out, INT Max)
{
    MONITORINFOEXW M;
    M.cbSize = sizeof(M);
    if (!Window || !::GetMonitorInfoW(::MonitorFromWindow(Window, MONITOR_DEFAULTTONEAREST), &M))
        return 0;
    INT i = 0;
    for (; M.szDevice[i] && i < Max - 1; i++)
        Out[i] = (char)M.szDevice[i];   // the name is ASCII by construction
    Out[i] = 0;
    return i > 0;
}

// Each vendor's answer as a line for the log, written on the probe's thread and logged on the renderer's (RdxTakeVrr):
// the engine's log is not the probe thread's to write.
static void Say(char* Line, INT Chars, const char* Text, const char* DeviceName)
{
    _snprintf_s(Line, Chars, _TRUNCATE, "VRR: %s on %s", Text, DeviceName);
}

// AMD. AGS lists every display of every AMD adapter with a FreeSync flag on it. The flag says the display SUPPORTS
// FreeSync, which is as close as this library gets: a screen that supports it and has it switched off in the driver
// still reads as supported. Better than nothing, and it is the vendor's own answer about that one screen.
static INT ProbeAmd(const char* DeviceName, char* Line, INT Chars)
{
    AGSContext* Ctx = nullptr;
    AGSGPUInfo  Info;
    memset(&Info, 0, sizeof(Info));
    if (agsInitialize(AGS_CURRENT_VERSION, nullptr, &Ctx, &Info) != AGS_SUCCESS || !Ctx)
        return RdxMath::Vrr_NoData;
    INT State = RdxMath::Vrr_NoData;
    for (int d = 0; d < Info.numDevices && State == RdxMath::Vrr_NoData; d++)
    {
        const AGSDeviceInfo& Dev = Info.devices[d];
        for (int s = 0; s < Dev.numDisplays && Dev.displays; s++)
        {
            if (_stricmp(Dev.displays[s].displayDeviceName, DeviceName) == 0)
            {
                State = Dev.displays[s].freesync ? RdxMath::Vrr_On : RdxMath::Vrr_Off;
                Say(Line, Chars, Dev.displays[s].freesync ? "AMD says FreeSync" : "AMD says no FreeSync", DeviceName);
                break;
            }
        }
    }
    agsDeInitialize(Ctx);
    return State;
}

// NVIDIA. NVAPI answers the question exactly: adaptive sync (G-Sync) is either disabled on that display or it is not.
static INT ProbeNvidia(const char* DeviceName, char* Line, INT Chars)
{
    if (NvAPI_Initialize() != NVAPI_OK)
        return RdxMath::Vrr_NoData;
    INT   State = RdxMath::Vrr_NoData;
    NvU32 Id    = 0;
    if (NvAPI_DISP_GetDisplayIdByDisplayName(DeviceName, &Id) == NVAPI_OK)
    {
        NV_GET_ADAPTIVE_SYNC_DATA Data;
        memset(&Data, 0, sizeof(Data));
        Data.version = NV_GET_ADAPTIVE_SYNC_DATA_VER;
        if (NvAPI_DISP_GetAdaptiveSyncData(Id, &Data) == NVAPI_OK)
        {
            State = Data.bDisableAdaptiveSync ? RdxMath::Vrr_Off : RdxMath::Vrr_On;
            Say(Line, Chars,
                Data.bDisableAdaptiveSync ? "NVIDIA says adaptive sync is off" : "NVIDIA says adaptive sync is on",
                DeviceName);
        }
    }
    NvAPI_Unload();
    return State;
}

// Intel. Their control library says per screen whether adaptive sync is ENABLED, which is the question exactly
// (IntelVrr.h). The library names screens by Windows target ID; the shared code maps that to "\\.\DISPLAY1".
static INT ProbeIntel(const char* DeviceName, char* Line, INT Chars)
{
    wchar_t Wide[64];
    INT i = 0;
    for (; DeviceName[i] && i < 63; i++)
        Wide[i] = (wchar_t)(BYTE)DeviceName[i];
    Wide[i] = 0;
    const int Answer = IntelVrrForDisplay(Wide, nullptr);
    if (Answer < 0)
        return RdxMath::Vrr_NoData;
    Say(Line, Chars, Answer ? "Intel says adaptive sync is on" : "Intel says adaptive sync is off", DeviceName);
    return Answer ? RdxMath::Vrr_On : RdxMath::Vrr_Off;
}

static INT ProbeScreen(const char* DeviceName, char* Line, INT Chars)
{
    Line[0] = 0;
    INT State = ProbeAmd(DeviceName, Line, Chars);
    if (State == RdxMath::Vrr_NoData)
        State = ProbeNvidia(DeviceName, Line, Chars);
    if (State == RdxMath::Vrr_NoData)
        State = ProbeIntel(DeviceName, Line, Chars);
    if (State == RdxMath::Vrr_NoData)
        _snprintf_s(Line, Chars, _TRUNCATE,
                    "VRR: no graphics driver on this machine would say anything about %s - taken as on", DeviceName);
    return State;
}

// What the renderer's thread and the probe's share, under GLock. An answer is handed over only when no newer question
// came meanwhile (GAsked): the window may have moved on to another screen while the probe was still busy with this one.
struct FKnownScreen
{
    char Name[64];
    INT  State;
};
static const INT     MaxKnown    = 8;
static SRWLOCK       GLock       = SRWLOCK_INIT;
static FKnownScreen  GKnown[MaxKnown];
static INT           GKnownCount = 0;
static char          GWanted[64];            // the screen the probe is to ask about next; empty for none
static UINT          GAsked      = 0;        // questions so far; the probe's answer counts only for the latest
static UINT          GWantedAsk  = 0;
static UBOOL         GRunning    = 0;        // the probe's thread is running (or starting)
static HANDLE        GThread     = nullptr;
static volatile LONG GHaveAnswer = 0;
static INT           GAnswer     = RdxMath::Vrr_NoData;
static char          GAnswerLine[192];

// Under GLock.
static void Answer(INT State, const char* Line)
{
    GAnswer = State;
    _snprintf_s(GAnswerLine, sizeof(GAnswerLine), _TRUNCATE, "%s", Line);
    ::InterlockedExchange(&GHaveAnswer, 1);
}

static DWORD WINAPI ProbeThread(LPVOID)
{
    for (;;)
    {
        char Name[64];
        UINT Ask;
        ::AcquireSRWLockExclusive(&GLock);
        if (!GWanted[0])
        {
            GRunning = 0;
            ::ReleaseSRWLockExclusive(&GLock);
            return 0;
        }
        memcpy(Name, GWanted, sizeof(Name));
        Ask        = GWantedAsk;
        GWanted[0] = 0;
        ::ReleaseSRWLockExclusive(&GLock);

        LARGE_INTEGER F, T0, T1;
        ::QueryPerformanceFrequency(&F);
        ::QueryPerformanceCounter(&T0);
        char      Line[160];
        const INT State = ProbeScreen(Name, Line, sizeof(Line));
        ::QueryPerformanceCounter(&T1);
        char Timed[192];
        _snprintf_s(Timed, sizeof(Timed), _TRUNCATE, "%s (asked on a thread of its own, %.0f ms)", Line,
                    (double)(T1.QuadPart - T0.QuadPart) * 1000.0 / (double)F.QuadPart);

        ::AcquireSRWLockExclusive(&GLock);
        if (GKnownCount < MaxKnown)
        {
            memcpy(GKnown[GKnownCount].Name, Name, sizeof(Name));
            GKnown[GKnownCount].State = State;
            GKnownCount++;
        }
        if (Ask == GAsked)
            Answer(State, Timed);
        ::ReleaseSRWLockExclusive(&GLock);
    }
}

void RdxAskVrr(HWND Window, UBOOL bTearingAllowed)
{
    char Name[64];
    const UBOOL bNamed = bTearingAllowed && MonitorDeviceName(Window, Name, 64);
    ::AcquireSRWLockExclusive(&GLock);
    GAsked++;
    // A true negative that needs no vendor at all: with no tearing presents there can be no variable refresh in a
    // window, whatever the screen is capable of.
    if (!bTearingAllowed)
        Answer(RdxMath::Vrr_Off, "");
    else if (!bNamed)
        Answer(RdxMath::Vrr_NoData, "");
    else
    {
        INT Known = -1;
        for (INT i = 0; i < GKnownCount && Known < 0; i++)
            if (_stricmp(GKnown[i].Name, Name) == 0)
                Known = i;
        if (Known >= 0)
        {
            char Line[160];
            _snprintf_s(Line, sizeof(Line), _TRUNCATE, "VRR: %s as the driver said earlier in this run - %s", Name,
                        (GKnown[Known].State == RdxMath::Vrr_On)    ? "on"
                        : (GKnown[Known].State == RdxMath::Vrr_Off) ? "off"
                                                                    : "not known, taken as on");
            Answer(GKnown[Known].State, Line);
        }
        else
        {
            memcpy(GWanted, Name, sizeof(Name));
            GWantedAsk = GAsked;
            if (!GRunning)
            {
                // The one before has ended (it clears GRunning last, under this lock): its handle can go.
                if (GThread)
                    ::CloseHandle(GThread);
                GThread  = ::CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
                GRunning = GThread != nullptr;
                if (!GThread)
                {
                    GWanted[0] = 0;
                    Answer(RdxMath::Vrr_NoData, "VRR: no thread for the driver's question - taken as on");
                }
            }
        }
    }
    ::ReleaseSRWLockExclusive(&GLock);
}

UBOOL RdxTakeVrr(INT& State)
{
    if (!GHaveAnswer)
        return 0;
    char Line[192];
    ::AcquireSRWLockExclusive(&GLock);
    State = GAnswer;
    memcpy(Line, GAnswerLine, sizeof(Line));
    ::InterlockedExchange(&GHaveAnswer, 0);
    ::ReleaseSRWLockExclusive(&GLock);
    if (Line[0])
        RdxLog(TEXT("%s"), appFromAnsi(Line));
    return 1;
}

void RdxVrrWait()
{
    ::AcquireSRWLockExclusive(&GLock);
    const HANDLE Thread  = GThread;
    const UBOOL  Running = GRunning;
    ::ReleaseSRWLockExclusive(&GLock);
    if (!Thread || !Running)
        return;
    const DWORD Waited = ::WaitForSingleObject(Thread, 3000);
    RdxLog(TEXT("VRR: the driver's question was still running at the renderer's end - %s"),
           (Waited == WAIT_OBJECT_0) ? TEXT("waited for it") : TEXT("left running after 3 s"));
}

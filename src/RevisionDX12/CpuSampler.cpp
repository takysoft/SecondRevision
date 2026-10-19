#include "Pch.h"
#include "CpuSampler.h"
#include "Log.h"
#include <dbghelp.h>
#include <algorithm>
#include <stdio.h>
#pragma comment(lib, "dbghelp.lib")

// The game's thread, the sampler's thread and its stop; the places noted - the instruction pointer, one a sample - as
// many as the room holds between two reports; whether the game's thread is reporting (the sampler leaves it alone).
static HANDLE        GSampled   = nullptr;
static HANDLE        GSampler   = nullptr;
static HANDLE        GQuit      = nullptr;
static DWORD*        GPlaces    = nullptr;
static volatile LONG GCount     = 0;
static volatile LONG GPaused    = 0;
static const LONG    PLACES_MOST = 1 << 20;
static DWORD         GFirstTick = 0;
static UBOOL         GSymbols   = 0;

// Every millisecond: the game's thread stopped, its place read, and let go. Nothing here takes a lock the game's
// thread might hold while it stands stopped - no allocation, no symbols.
static DWORD WINAPI SamplerMain(void*)
{
    while (::WaitForSingleObject(GQuit, 1) == WAIT_TIMEOUT)
    {
        if (GPaused || GCount >= PLACES_MOST)
            continue;
        if (::SuspendThread(GSampled) == (DWORD)-1)
            continue;
        CONTEXT C;
        C.ContextFlags = CONTEXT_CONTROL;
        if (::GetThreadContext(GSampled, &C))
            GPlaces[GCount++] = C.Eip;
        ::ResumeThread(GSampled);
    }
    return 0;
}

void RdxCpuSamplerStart()
{
    if (GSampler)
        return;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &GSampled,
                           THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    GPlaces = (DWORD*)::VirtualAlloc(nullptr, PLACES_MOST * sizeof(DWORD), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    GQuit   = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!GPlaces || !GQuit)
        return;
    GCount     = 0;
    GFirstTick = ::GetTickCount();
    GSampler   = ::CreateThread(nullptr, 0, &SamplerMain, nullptr, 0, nullptr);
    if (GSampler)
        ::SetThreadPriority(GSampler, THREAD_PRIORITY_TIME_CRITICAL);
    RdxLogLineFromAnyThread(GSampler ? "cpu samples (marker): the game's thread sampled every millisecond - measurement only\n"
                                     : "cpu samples (marker): the sampler's thread would not start\n");
}

void RdxCpuSamplerStop()
{
    if (GSampler)
    {
        ::SetEvent(GQuit);
        ::WaitForSingleObject(GSampler, 2000);
        ::CloseHandle(GSampler);
        GSampler = nullptr;
    }
    if (GQuit)
        ::CloseHandle(GQuit);
    GQuit = nullptr;
    if (GSampled)
        ::CloseHandle(GSampled);
    GSampled = nullptr;
    if (GPlaces)
        ::VirtualFree(GPlaces, 0, MEM_RELEASE);
    GPlaces = nullptr;
    if (GSymbols)
        ::SymCleanup(::GetCurrentProcess());
    GSymbols = 0;
}

namespace
{
struct FSpot
{
    DWORD64 Start;      // the function's first byte, or the module's base where no symbol is known
    LONG    Count;
    char    Name[160];
};
}

void RdxCpuSamplerFrame(double Seconds, int Top, const char* Label)
{
    if (!GSampler || (double)(::GetTickCount() - GFirstTick) < Seconds * 1000.0)
        return;
    ::InterlockedExchange(&GPaused, 1);
    const LONG Count = GCount;
    if (!GSymbols)
    {
        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        GSymbols = ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE) ? 1 : 0;
    }
    // The places by address, and each run of one address once to its function.
    std::sort(GPlaces, GPlaces + Count);
    static FSpot* Spots     = nullptr;
    static INT    SpotRoom  = 0;
    INT           SpotCount = 0;
    char          SymBuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO*  Sym = (SYMBOL_INFO*)SymBuf;
    for (LONG i = 0; i < Count;)
    {
        LONG j = i + 1;
        while (j < Count && GPlaces[j] == GPlaces[i])
            j++;
        DWORD64 Start = 0;
        char    Name[160];
        Name[0] = 0;
        appMemzero(SymBuf, sizeof(SymBuf));
        Sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        Sym->MaxNameLen   = 255;
        DWORD64 Off       = 0;
        HMODULE Mod       = nullptr;
        char    ModName[MAX_PATH];
        ModName[0] = 0;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 (LPCSTR)(UINT_PTR)GPlaces[i], &Mod) && Mod)
        {
            char Path[MAX_PATH];
            if (::GetModuleFileNameA(Mod, Path, MAX_PATH))
            {
                const char* Base = Path;
                for (const char* p = Path; *p; p++)
                    if (*p == '\\' || *p == '/')
                        Base = p + 1;
                lstrcpynA(ModName, Base, MAX_PATH);
            }
        }
        // The engine's modules export thunks - jumps to the bodies -, so the nearest export names a body wrongly: those
        // by their offset alone (scripts/cpu_sample_names.py names them).
        static const char* const Engine[] = { "Core.dll", "Engine.dll", "Render.dll", "RenderExt.dll", "Window.dll",
                                              "DeusEx.dll", "Extension.dll", "ConSys.dll", "Revision.exe" };
        UBOOL bEngine = 0;
        for (INT e = 0; e < (INT)ARRAY_COUNT(Engine) && !bEngine; e++)
            bEngine = lstrcmpiA(ModName, Engine[e]) == 0;
        if (!bEngine && GSymbols && ::SymFromAddr(::GetCurrentProcess(), (DWORD64)GPlaces[i], &Off, Sym) &&
            Off < 0x10000)
        {
            Start = Sym->Address;
            wsprintfA(Name, "%.40s!%.110s", ModName[0] ? ModName : "?", Sym->Name);
        }
        else
        {
            // No symbol: the place within its module, to a 16-byte step - a module's exports read beside it.
            const DWORD Rel = (DWORD)(GPlaces[i] - (DWORD)(UINT_PTR)Mod) & ~0xFu;
            Start = (DWORD64)(UINT_PTR)Mod + Rel;
            wsprintfA(Name, "%.40s+0x%06lX", ModName[0] ? ModName : "?", Rel);
        }
        INT s = 0;
        while (s < SpotCount && Spots[s].Start != Start)
            s++;
        if (s == SpotCount)
        {
            if (SpotCount >= SpotRoom)
            {
                const INT    Room = Max(SpotRoom * 2, 1024);
                FSpot* const More = (FSpot*)appRealloc(Spots, Room * sizeof(FSpot), TEXT("RdxCpuSpots"));
                if (!More)
                    break;
                Spots    = More;
                SpotRoom = Room;
            }
            Spots[s].Start = Start;
            Spots[s].Count = 0;
            lstrcpynA(Spots[s].Name, Name, 160);
            SpotCount++;
        }
        Spots[s].Count += j - i;
        i = j;
    }
    std::sort(Spots, Spots + SpotCount, [](const FSpot& A, const FSpot& B) { return A.Count > B.Count; });
    char Line[1024];
    wsprintfA(Line, "cpu samples (marker): %s - %ld samples over %lu ms, the top %d functions:\n", Label ? Label : "",
              Count, ::GetTickCount() - GFirstTick, Top);
    RdxLogLineFromAnyThread(Line);
    for (INT s = 0; s < SpotCount && s < Top; s++)
    {
        const LONG Tenths = (LONG)((LONGLONG)Spots[s].Count * 1000 / Max((LONG)1, Count));
        wsprintfA(Line, "cpu samples:   %3ld.%ld%%  %s\n", Tenths / 10, Tenths % 10, Spots[s].Name);
        RdxLogLineFromAnyThread(Line);
    }
    // And by module: the part before the '!'.
    struct FModule
    {
        char Name[48];
        LONG Count;
    };
    FModule Mods[32];
    INT     ModCount = 0;
    for (INT s = 0; s < SpotCount; s++)
    {
        char M[48];
        INT  k = 0;
        while (Spots[s].Name[k] && Spots[s].Name[k] != '!' && Spots[s].Name[k] != '+' && k < 47)
        {
            M[k] = Spots[s].Name[k];
            k++;
        }
        M[k]  = 0;
        INT m = 0;
        while (m < ModCount && lstrcmpA(Mods[m].Name, M) != 0)
            m++;
        if (m == ModCount)
        {
            if (ModCount >= 32)
                continue;
            lstrcpynA(Mods[m].Name, M, 48);
            Mods[m].Count = 0;
            ModCount++;
        }
        Mods[m].Count += Spots[s].Count;
    }
    std::sort(Mods, Mods + ModCount, [](const FModule& A, const FModule& B) { return A.Count > B.Count; });
    char* At = Line;
    At += wsprintfA(At, "cpu samples: by module -");
    for (INT m = 0; m < ModCount && m < 10; m++)
    {
        const LONG Tenths = (LONG)((LONGLONG)Mods[m].Count * 1000 / Max((LONG)1, Count));
        At += wsprintfA(At, " %s %ld.%ld%%", Mods[m].Name, Tenths / 10, Tenths % 10);
    }
    lstrcatA(Line, "\n");
    RdxLogLineFromAnyThread(Line);
    GCount     = 0;
    GFirstTick = ::GetTickCount();
    ::InterlockedExchange(&GPaused, 0);
}

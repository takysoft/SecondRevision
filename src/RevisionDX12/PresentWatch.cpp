#include "Pch.h"
#include "PresentWatch.h"
#include "Log.h"

#include <stdio.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
    enum { LINE_LEN = 1200, PART_LEN = 160, MAX_FRAMES = 20, STACK_BYTES = 32 * 1024, MAX_SAMPLES = 60 };

    HANDLE            GRender     = nullptr;    // the render thread, for suspending
    HANDLE            GWatchdog   = nullptr;
    HANDLE            GStopEvent  = nullptr;
    LONGLONG          GTicksPerMs = 1;
    volatile LONGLONG GSince      = 0;          // QPC at Present entry; 0 outside Present
    volatile LONG     GSeq        = 0;          // one per Present
    volatile LONG     GPresentNo  = 0;          // the renderer's own number for it

    // One caught stack at a time: the watchdog fills the line, the render thread logs it.
    wchar_t           GLine[LINE_LEN];
    volatile LONG     GLinePresent = 0;
    volatile LONG     GLineReady   = 0;
    DWORD             GStack[STACK_BYTES / 4];

    LONGLONG ReadSince()
    {
        return InterlockedCompareExchange64(&GSince, 0, 0);
    }

    // "module!export+0xN" for an address, from the module's own export table - no symbol
    // files, nothing fetched. Far past the nearest export the name would mislead, so there it
    // is "module+0xRVA".
    void Describe(DWORD Addr, wchar_t* Out, size_t Len)
    {
        HMODULE Mod = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(static_cast<UINT_PTR>(Addr)), &Mod) || !Mod)
        {
            _snwprintf_s(Out, Len, _TRUNCATE, L"%08X", Addr);
            return;
        }
        wchar_t Path[MAX_PATH];
        const DWORD N = ::GetModuleFileNameW(Mod, Path, MAX_PATH);
        const wchar_t* Name = N ? Path : L"?";
        for (DWORD i = 0; i < N; i++)
            if (Path[i] == L'\\')
                Name = Path + i + 1;

        const BYTE* Base    = reinterpret_cast<const BYTE*>(Mod);
        const DWORD Rva     = Addr - static_cast<DWORD>(reinterpret_cast<UINT_PTR>(Mod));
        const char* Best    = nullptr;
        DWORD       BestRva = 0;
        const IMAGE_DOS_HEADER* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
        if (Dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            const IMAGE_NT_HEADERS32* Nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(Base + Dos->e_lfanew);
            const IMAGE_DATA_DIRECTORY& D = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (Nt->Signature == IMAGE_NT_SIGNATURE && D.VirtualAddress && D.Size)
            {
                const IMAGE_EXPORT_DIRECTORY* E = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(Base + D.VirtualAddress);
                const DWORD* Funcs = reinterpret_cast<const DWORD*>(Base + E->AddressOfFunctions);
                const DWORD* Names = reinterpret_cast<const DWORD*>(Base + E->AddressOfNames);
                const WORD*  Ords  = reinterpret_cast<const WORD*>(Base + E->AddressOfNameOrdinals);
                for (DWORD i = 0; i < E->NumberOfNames; i++)
                {
                    if (Ords[i] >= E->NumberOfFunctions)
                        continue;
                    const DWORD F = Funcs[Ords[i]];
                    const bool Forwarder = F >= D.VirtualAddress && F < D.VirtualAddress + D.Size;
                    if (!Forwarder && F <= Rva && F >= BestRva)
                    {
                        BestRva = F;
                        Best    = reinterpret_cast<const char*>(Base + Names[i]);
                    }
                }
            }
        }
        if (Best && Rva - BestRva < 0x2000)
            _snwprintf_s(Out, Len, _TRUNCATE, L"%s!%S+0x%X", Name, Best, Rva - BestRva);
        else
            _snwprintf_s(Out, Len, _TRUNCATE, L"%s+0x%X", Name, Rva);
    }

    void Sample(LONGLONG Since, DOUBLE BlockedMs)
    {
        CONTEXT C;
        memset(&C, 0, sizeof(C));
        C.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        DWORD Copied = 0;

        // Suspended only for this: the context, one copy of the stack top, then resumed.
        // Nothing in between may take a lock the render thread could be holding - no
        // allocation, no loader, no log. GetThreadContext also waits until the suspension
        // has really happened (SuspendThread alone only asks for it); that wait is timed.
        LARGE_INTEGER S0, S1;
        ::QueryPerformanceCounter(&S0);
        if (::SuspendThread(GRender) == (DWORD)-1)
            return;
        BOOL Have = ::GetThreadContext(GRender, &C);
        ::QueryPerformanceCounter(&S1);
        // Still the same Present? Otherwise the render thread has moved on, and its stack now
        // shows something else.
        if (Have && ReadSince() != Since)
            Have = FALSE;
        if (Have)
        {
            MEMORY_BASIC_INFORMATION M;
            if (::VirtualQuery(reinterpret_cast<void*>(static_cast<UINT_PTR>(C.Esp)), &M, sizeof(M)) &&
                M.State == MEM_COMMIT)
            {
                const DWORD End = static_cast<DWORD>(reinterpret_cast<UINT_PTR>(M.BaseAddress)) +
                                  static_cast<DWORD>(M.RegionSize);
                Copied = End - C.Esp;
                if (Copied > sizeof(GStack))
                    Copied = sizeof(GStack);
                memcpy(GStack, reinterpret_cast<const void*>(static_cast<UINT_PTR>(C.Esp)), Copied);
            }
        }
        ::ResumeThread(GRender);
        if (!Have)
            return;

        // The instruction pointer; the top stack slot (a system-call stub has no frame of its
        // own and returns through it); then the chain of saved frame pointers, as far as it
        // holds - code built without frame pointers ends it early.
        DWORD Frames[MAX_FRAMES];
        INT   Count = 0;
        Frames[Count++] = C.Eip;
        if (Copied >= 4)
            Frames[Count++] = GStack[0];
        DWORD Ebp = C.Ebp;
        while (Count < MAX_FRAMES && Ebp >= C.Esp && (Ebp & 3) == 0 && Ebp - C.Esp + 8 <= Copied)
        {
            const DWORD* P    = GStack + (Ebp - C.Esp) / 4;
            const DWORD  Next = P[0];
            const DWORD  Ret  = P[1];
            if (!Ret)
                break;
            Frames[Count++] = Ret;
            if (Next <= Ebp)
                break;
            Ebp = Next;
        }

        _snwprintf_s(GLine, LINE_LEN, _TRUNCATE, L"blocking for %.1f ms (capture %.2f ms):", BlockedMs,
                     (DOUBLE)(S1.QuadPart - S0.QuadPart) / (DOUBLE)GTicksPerMs);
        wchar_t Part[PART_LEN];
        for (INT i = 0; i < Count; i++)
        {
            Describe(Frames[i], Part, PART_LEN);
            wcsncat_s(GLine, LINE_LEN, i ? L" <- " : L" ", _TRUNCATE);
            wcsncat_s(GLine, LINE_LEN, Part, _TRUNCATE);
        }
        InterlockedExchange(&GLineReady, 1);
    }

    // ---- The stall watch ----

    enum { STALL_LINE = 6000, STALL_SCAN = 6000, STALL_ADDRS = 48, STALL_MS = 250, STALL_SAMPLES = 16,
           STALL_TOTAL = 400 };

    HANDLE            GStallRender = nullptr;   // the render thread, for suspending
    HANDLE            GStallThread = nullptr;
    HANDLE            GStallStop   = nullptr;
    volatile LONGLONG GFrameAt     = 0;         // QPC at the end of the last frame; 0 before the first
    volatile LONG     GFrameNo     = 0;
    char              GStallLine[STALL_LINE];
    DWORD             GStallStack[STACK_BYTES / 4];

    // Whether Addr is where a call returns to: in the code of a module's image, with a call instruction before it -
    // a relative call, or one through a register or through memory in any of its lengths.
    bool LooksLikeReturn(DWORD Addr)
    {
        static DWORD KnownFrom = 0, KnownTo = 0;      // the last region found to be code
        if (Addr < 0x10000)
            return false;
        if (Addr < KnownFrom + 8 || Addr >= KnownTo)
        {
            MEMORY_BASIC_INFORMATION M;
            if (!::VirtualQuery(reinterpret_cast<void*>(static_cast<UINT_PTR>(Addr)), &M, sizeof(M)) ||
                M.State != MEM_COMMIT || M.Type != MEM_IMAGE ||
                !(M.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                return false;
            KnownFrom = static_cast<DWORD>(reinterpret_cast<UINT_PTR>(M.BaseAddress));
            KnownTo   = KnownFrom + static_cast<DWORD>(M.RegionSize);
            if (Addr < KnownFrom + 8)
                return false;
        }
        const BYTE* const P = reinterpret_cast<const BYTE*>(static_cast<UINT_PTR>(Addr));
        if (P[-5] == 0xE8)
            return true;
        static const int Lengths[5] = { 2, 3, 4, 6, 7 };
        for (int i = 0; i < 5; i++)
            if (P[-Lengths[i]] == 0xFF && (P[-Lengths[i] + 1] & 0x38) == 0x10)
                return true;
        return false;
    }

    void StallAppend(const char* Text)
    {
        strncat_s(GStallLine, STALL_LINE, Text, _TRUNCATE);
    }

    void StallAppendAddress(DWORD Addr)
    {
        wchar_t Wide[PART_LEN];
        char    Narrow[PART_LEN];
        Describe(Addr, Wide, PART_LEN);
        int k = 0;
        for (; Wide[k] && k < PART_LEN - 1; k++)
            Narrow[k] = (Wide[k] < 128) ? static_cast<char>(Wide[k]) : '?';
        Narrow[k] = 0;
        StallAppend(Narrow);
    }

    void StallSample(DOUBLE StalledMs, LONG FrameNo, INT Nth)
    {
        CONTEXT C;
        memset(&C, 0, sizeof(C));
        C.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        DWORD Copied = 0;
        // As the Present watch's sample: suspended for the context and one copy of the stack's top alone.
        if (::SuspendThread(GStallRender) == (DWORD)-1)
            return;
        const BOOL Have = ::GetThreadContext(GStallRender, &C);
        if (Have)
        {
            MEMORY_BASIC_INFORMATION M;
            if (::VirtualQuery(reinterpret_cast<void*>(static_cast<UINT_PTR>(C.Esp)), &M, sizeof(M)) &&
                M.State == MEM_COMMIT)
            {
                const DWORD End = static_cast<DWORD>(reinterpret_cast<UINT_PTR>(M.BaseAddress)) +
                                  static_cast<DWORD>(M.RegionSize);
                Copied = End - C.Esp;
                if (Copied > sizeof(GStallStack))
                    Copied = sizeof(GStallStack);
                memcpy(GStallStack, reinterpret_cast<const void*>(static_cast<UINT_PTR>(C.Esp)), Copied);
            }
        }
        ::ResumeThread(GStallRender);
        if (!Have)
            return;

        _snprintf_s(GStallLine, STALL_LINE, _TRUNCATE, "stall: no frame for %.0f ms after frame %i (sample %i): at ",
                    StalledMs, static_cast<int>(FrameNo), Nth);
        StallAppendAddress(C.Eip);
        // The chain of saved frame pointers, as far as it holds.
        StallAppend("; frames:");
        DWORD Ebp   = C.Ebp;
        INT   Count = 0;
        while (Count < MAX_FRAMES && Ebp >= C.Esp && (Ebp & 3) == 0 && Ebp - C.Esp + 8 <= Copied)
        {
            const DWORD* P    = GStallStack + (Ebp - C.Esp) / 4;
            const DWORD  Next = P[0];
            const DWORD  Ret  = P[1];
            if (!Ret)
                break;
            StallAppend(Count ? " <- " : " ");
            StallAppendAddress(Ret);
            Count++;
            if (Next <= Ebp)
                break;
            Ebp = Next;
        }
        // And what on the stack looks like a return address, from its top down the calls.
        StallAppend("; on the stack:");
        const DWORD Slots = (Copied / 4 < (DWORD)STALL_SCAN) ? Copied / 4 : (DWORD)STALL_SCAN;
        INT   Listed = 0;
        DWORD Last   = 0;
        for (DWORD i = 0; i < Slots && Listed < STALL_ADDRS; i++)
        {
            const DWORD V = GStallStack[i];
            if (V == Last || !LooksLikeReturn(V))
                continue;
            Last = V;
            StallAppend(Listed ? " | " : " ");
            StallAppendAddress(V);
            Listed++;
        }
        StallAppend("\n");
        RdxLogLineFromAnyThread(GStallLine);
    }

    DWORD WINAPI StallWatch(void*)
    {
        LONG     SeenNo     = -1;
        INT      InStall    = 0, Total = 0;
        LONGLONG SampledAt  = 0;
        for (;;)
        {
            if (::WaitForSingleObject(GStallStop, 50) == WAIT_OBJECT_0)
                break;
            const LONGLONG At = InterlockedCompareExchange64(&GFrameAt, 0, 0);
            const LONG     No = GFrameNo;
            if (!At)
                continue;
            if (No != SeenNo)
            {
                SeenNo  = No;
                InStall = 0;
            }
            LARGE_INTEGER Now;
            ::QueryPerformanceCounter(&Now);
            const DOUBLE Ms = (DOUBLE)(Now.QuadPart - At) / (DOUBLE)GTicksPerMs;
            if (Ms < (DOUBLE)STALL_MS || InStall >= STALL_SAMPLES || Total >= STALL_TOTAL)
                continue;
            if (InStall > 0 && (DOUBLE)(Now.QuadPart - SampledAt) / (DOUBLE)GTicksPerMs < (DOUBLE)STALL_MS)
                continue;
            SampledAt = Now.QuadPart;
            InStall++;
            Total++;
            StallSample(Ms, No, InStall);
        }
        return 0;
    }

    DWORD WINAPI Watch(void*)
    {
        // A 1 ms tick without changing the process's timer period.
        HANDLE Timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!Timer)
            Timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        HANDLE Waits[2] = { GStopEvent, Timer };
        LONG   Done     = -1;       // the Present already sampled
        INT    Samples  = 0;
        for (;;)
        {
            if (Timer)
            {
                LARGE_INTEGER Due;
                Due.QuadPart = -10000;      // 1 ms from now, in 100 ns units
                ::SetWaitableTimer(Timer, &Due, 0, nullptr, nullptr, FALSE);
                if (::WaitForMultipleObjects(2, Waits, FALSE, 100) == WAIT_OBJECT_0)
                    break;
            }
            else if (::WaitForSingleObject(GStopEvent, 1) == WAIT_OBJECT_0)
                break;

            const LONGLONG Since = ReadSince();
            const LONG     Seq   = GSeq;
            if (!Since || Seq == Done || GLineReady)
                continue;
            LARGE_INTEGER Now;
            ::QueryPerformanceCounter(&Now);
            const DOUBLE Ms = (DOUBLE)(Now.QuadPart - Since) / (DOUBLE)GTicksPerMs;
            if (Ms < 6.0)
                continue;
            Done = Seq;
            GLinePresent = GPresentNo;
            Sample(Since, Ms);
            if (++Samples >= MAX_SAMPLES)
                break;
        }
        if (Timer)
            ::CloseHandle(Timer);
        return 0;
    }
}

void RdxPresentWatchStart()
{
    if (GWatchdog)
        return;
    LARGE_INTEGER F;
    ::QueryPerformanceFrequency(&F);
    GTicksPerMs = F.QuadPart / 1000 > 0 ? F.QuadPart / 1000 : 1;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &GRender,
                           THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        GRender = nullptr;
    GStopEvent = GRender ? ::CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
    GWatchdog  = GStopEvent ? ::CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr) : nullptr;
    RdxLog(TEXT("present watch: %s"), GWatchdog ? TEXT("started") : TEXT("did NOT start"));
}

void RdxPresentWatchStop()
{
    if (GWatchdog)
    {
        ::SetEvent(GStopEvent);
        ::WaitForSingleObject(GWatchdog, 2000);
        ::CloseHandle(GWatchdog);
        GWatchdog = nullptr;
    }
    if (GStopEvent)
    {
        ::CloseHandle(GStopEvent);
        GStopEvent = nullptr;
    }
    if (GRender)
    {
        ::CloseHandle(GRender);
        GRender = nullptr;
    }
    InterlockedExchange64(&GSince, 0);
    InterlockedExchange(&GLineReady, 0);
}

void RdxStallWatchStart()
{
    if (GStallThread)
        return;
    LARGE_INTEGER F;
    ::QueryPerformanceFrequency(&F);
    GTicksPerMs = F.QuadPart / 1000 > 0 ? F.QuadPart / 1000 : 1;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &GStallRender,
                           THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        GStallRender = nullptr;
    GStallStop   = GStallRender ? ::CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
    GStallThread = GStallStop ? ::CreateThread(nullptr, 0, StallWatch, nullptr, 0, nullptr) : nullptr;
    RdxLog(TEXT("stall watch: %s"), GStallThread ? TEXT("started") : TEXT("did NOT start"));
}

void RdxStallWatchStop()
{
    if (GStallThread)
    {
        ::SetEvent(GStallStop);
        ::WaitForSingleObject(GStallThread, 2000);
        ::CloseHandle(GStallThread);
        GStallThread = nullptr;
    }
    if (GStallStop)
    {
        ::CloseHandle(GStallStop);
        GStallStop = nullptr;
    }
    if (GStallRender)
    {
        ::CloseHandle(GStallRender);
        GStallRender = nullptr;
    }
    InterlockedExchange64(&GFrameAt, 0);
}

void RdxStallWatchFrame(INT FrameNumber)
{
    if (!GStallThread)
        return;
    LARGE_INTEGER Q;
    ::QueryPerformanceCounter(&Q);
    GFrameNo = FrameNumber;
    InterlockedExchange64(&GFrameAt, Q.QuadPart);
}

void RdxPresentWatchEnter(INT PresentNumber)
{
    if (!GWatchdog)
        return;
    LARGE_INTEGER Q;
    ::QueryPerformanceCounter(&Q);
    GPresentNo = PresentNumber;
    InterlockedIncrement(&GSeq);
    InterlockedExchange64(&GSince, Q.QuadPart);
}

void RdxPresentWatchLeave()
{
    if (!GWatchdog)
        return;
    InterlockedExchange64(&GSince, 0);
    if (GLineReady)
    {
        RdxLog(TEXT("present %i: %s"), (INT)GLinePresent, reinterpret_cast<const TCHAR*>(GLine));
        InterlockedExchange(&GLineReady, 0);
    }
}

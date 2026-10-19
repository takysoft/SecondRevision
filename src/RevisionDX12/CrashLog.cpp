#include "Pch.h"
#include "CrashLog.h"
#include "Log.h"

#include <dbghelp.h>
#include <stdio.h>

// The faults worth a report. What goes by: first-chance C++ exceptions (0xE06D7363 - the engine's own unwinding rethrows
// one at every guard on the way up), the debugger's output and thread-naming exceptions - none of them is a fault. The
// stack overflow goes by as well, on purpose: this report needs a few kilobytes of stack, and there are none left then.
static UBOOL IsHardFault(DWORD Code)
{
    switch (Code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case 0xC0000374:                          // STATUS_HEAP_CORRUPTION
        return 1;
    }
    return 0;
}

// A diagnosis only (the marker SecondRevision-crash-all.txt, fresh in %TEMP%): every exception is reported, not only the
// hard faults - for a crash the engine calls a general protection fault that no hard fault's report came before. What
// still goes by: the debugger's output and thread naming, which are chatter, and the stack overflow (no stack left).
static UBOOL GAllCodes = 0;

static UBOOL IsChatter(DWORD Code)
{
    return Code == 0x406D1388 || Code == 0x40010006 || Code == 0x4001000A || Code == EXCEPTION_STACK_OVERFLOW;
}

static UBOOL FreshTempMarker(const WCHAR* Name)
{
    WCHAR Path[MAX_PATH + 64];
    const DWORD N = ::GetTempPathW(MAX_PATH, Path);
    if (N == 0 || N >= MAX_PATH)
        return 0;
    ::lstrcatW(Path, Name);
    WIN32_FILE_ATTRIBUTE_DATA A;
    if (!::GetFileAttributesExW(Path, GetFileExInfoStandard, &A))
        return 0;
    FILETIME Now;
    ::GetSystemTimeAsFileTime(&Now);
    const unsigned __int64 T = ((unsigned __int64)A.ftLastWriteTime.dwHighDateTime << 32) | A.ftLastWriteTime.dwLowDateTime;
    const unsigned __int64 C = ((unsigned __int64)Now.dwHighDateTime << 32) | Now.dwLowDateTime;
    return C >= T && C - T < 10ull * 60 * 10000000;      // FILETIME counts 100 ns
}

// Where an address lies: the module, the offset from its base, and whether it is inside one of its executable sections.
struct FCodePlace
{
    DWORD Base;             // 0: in no module
    DWORD Offset;           // from Base (the address itself when in no module)
    UBOOL bCode;
    DWORD SectionStart;     // the executable section's first address
    WCHAR Name[48];
};

static UBOOL FindCodePlace(DWORD Address, FCodePlace& Out)
{
    Out.Base         = 0;
    Out.Offset       = Address;
    Out.bCode        = 0;
    Out.SectionStart = 0;
    Out.Name[0]      = 0;
    HMODULE Mod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR)(ULONG_PTR)Address, &Mod) || !Mod)
        return 0;
    Out.Base   = (DWORD)(ULONG_PTR)Mod;
    Out.Offset = Address - Out.Base;

    WCHAR       Path[MAX_PATH];
    const DWORD N    = ::GetModuleFileNameW(Mod, Path, MAX_PATH);
    INT         From = 0;
    if (N > 0 && N < MAX_PATH)
    {
        for (DWORD i = 0; i < N; i++)
            if (Path[i] == L'\\' || Path[i] == L'/')
                From = (INT)i + 1;
        INT k = 0;
        for (; Path[From + k] && k < 47; k++)
            Out.Name[k] = Path[From + k];
        Out.Name[k] = 0;
    }

    const IMAGE_DOS_HEADER* Dos = (const IMAGE_DOS_HEADER*)Mod;
    if (Dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 1;
    const IMAGE_NT_HEADERS* Nt = (const IMAGE_NT_HEADERS*)((const BYTE*)Mod + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE)
        return 1;
    const IMAGE_SECTION_HEADER* S = IMAGE_FIRST_SECTION(Nt);
    for (WORD i = 0; i < Nt->FileHeader.NumberOfSections; i++, S++)
    {
        const DWORD Size = (S->Misc.VirtualSize > S->SizeOfRawData) ? S->Misc.VirtualSize : S->SizeOfRawData;
        if (Out.Offset >= S->VirtualAddress && Out.Offset < S->VirtualAddress + Size)
        {
            Out.bCode        = (S->Characteristics & IMAGE_SCN_MEM_EXECUTE) ? 1 : 0;
            Out.SectionStart = Out.Base + S->VirtualAddress;
            break;
        }
    }
    return 1;
}

// Whether the bytes before Address end in a CALL: the mark of a return address. It is what tells one from any other value on
// the stack that happens to point into code - a function pointer, a vtable's entry. The forms: E8 with a 32-bit displacement,
// and FF /2 in each length its ModR/M byte can give (2, 3, 4, 6 and 7 bytes).
static UBOOL FollowsCall(const FCodePlace& P, DWORD Address)
{
    if (!P.bCode || Address - P.SectionStart < 7)
        return 0;
    const BYTE* B = (const BYTE*)(ULONG_PTR)Address;
    if (B[-5] == 0xE8)
        return 1;
    static const INT Lengths[] = { 2, 3, 4, 6, 7 };
    for (INT i = 0; i < 5; i++)
        if (B[-Lengths[i]] == 0xFF && (B[-Lengths[i] + 1] & 0x38) == 0x10)
            return 1;
    return 0;
}

static PVOID GHandler  = nullptr;
static LONG  GReports  = 0;
static LONG  GInReport = 0;
enum { MAX_REPORTS = 8, MAX_FRAMES = 48, FRAMES_PER_LINE = 6, STACK_SLOTS = 16384 };
// The addresses reported already: a fault that some code expects, and handles, must not use the reports up one by one.
static DWORD GReportedAt[MAX_REPORTS];

static void Report(const EXCEPTION_POINTERS* E, LONG Number)
{
    const EXCEPTION_RECORD* R     = E->ExceptionRecord;
    const CONTEXT*          C     = E->ContextRecord;
    const DWORD             Where = (DWORD)(ULONG_PTR)R->ExceptionAddress;

    FCodePlace Place;
    FindCodePlace(Where, Place);
    TCHAR Access[96] = { 0 };
    if (R->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && R->NumberParameters >= 2)
    {
        const ULONG_PTR Kind = R->ExceptionInformation[0];
        appSprintf(Access, TEXT(" - access violation: %s at 0x%08X"),
                   Kind == 0 ? TEXT("read") : (Kind == 1 ? TEXT("write") : TEXT("execute")),
                   (DWORD)R->ExceptionInformation[1]);
    }
    RdxLog(TEXT("crash report #%i (thread %u): exception 0x%08X%s, at: %s+0x%08X (0x%08X)"), (INT)Number,
           (UINT)::GetCurrentThreadId(), (UINT)R->ExceptionCode, Access, Place.Name[0] ? Place.Name : TEXT("?"),
           (UINT)Place.Offset, (UINT)Where);
    RdxLog(TEXT("crash report #%i: EAX %08X EBX %08X ECX %08X EDX %08X ESI %08X EDI %08X EBP %08X ESP %08X"), (INT)Number,
           (UINT)C->Eax, (UINT)C->Ebx, (UINT)C->Ecx, (UINT)C->Edx, (UINT)C->Esi, (UINT)C->Edi, (UINT)C->Ebp, (UINT)C->Esp);

    // The stack from ESP up: every value that points just past a CALL in some module's code. Our release build leaves out
    // the frame pointers, so there is no chain to walk - a scan it is, and a return address left over from an earlier call
    // can be among them. The nearest to ESP come first; those are the fault's own callers.
    const NT_TIB* Tib = (const NT_TIB*)::NtCurrentTeb();
    const DWORD   Top = (DWORD)(ULONG_PTR)Tib->StackBase;
    DWORD         Sp  = C->Esp & ~3u;
    const DWORD   End = (Top > Sp && (Top - Sp) / 4 > STACK_SLOTS) ? Sp + STACK_SLOTS * 4 : Top;
    TCHAR Line[900] = { 0 };
    INT   Frames = 0, OnLine = 0, Part = 0;
    for (; Sp + 4 <= End && Frames < MAX_FRAMES; Sp += 4)
    {
        const DWORD V = *(const DWORD*)(ULONG_PTR)Sp;
        if (V < 0x10000)
            continue;
        FCodePlace P;
        if (!FindCodePlace(V, P) || !FollowsCall(P, V))
            continue;
        TCHAR One[80];
        appSprintf(One, TEXT("%s%s+0x%08X"), OnLine ? TEXT(", ") : TEXT(""), P.Name, (UINT)P.Offset);
        appStrcat(Line, One);
        Frames++;
        if (++OnLine == FRAMES_PER_LINE)
        {
            RdxLog(TEXT("crash report #%i stack %i: %s"), (INT)Number, ++Part, Line);
            Line[0] = 0;
            OnLine  = 0;
        }
    }
    if (OnLine > 0)
        RdxLog(TEXT("crash report #%i stack %i: %s"), (INT)Number, ++Part, Line);
    if (Frames == 0)
        RdxLog(TEXT("crash report #%i stack: no return address found"), (INT)Number);
}

// LogLevel=Debug (RdxCrashLogDumps): the game's memory, once, at the first hard fault. dbghelp.dll is loaded only here, so
// a machine without it loses the dump and nothing else. The kind: the stacks and what they point at, the threads, the
// modules unloaded since - enough for a debugger with the PDB, in a few megabytes rather than the whole process.
static UBOOL GDumps   = 0;
static LONG  GDumped  = 0;

void RdxCrashLogDumps(UBOOL bOn)
{
    GDumps = bOn ? 1 : 0;
}

static void WriteDump(EXCEPTION_POINTERS* E, LONG Number)
{
    typedef BOOL(WINAPI * FDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                 PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE     Help = ::LoadLibraryW(L"dbghelp.dll");
    const FDump Dump = Help ? (FDump)::GetProcAddress(Help, "MiniDumpWriteDump") : nullptr;
    if (!Dump)
    {
        RdxLog(TEXT("crash report #%i: no memory dump - dbghelp.dll could not be loaded"), (INT)Number);
        return;
    }
    SYSTEMTIME T;
    ::GetLocalTime(&T);
    WCHAR Name[96];
    swprintf_s(Name, 96, L"SecondRevision-crash-%04u%02u%02u-%02u%02u%02u.dmp", (UINT)T.wYear, (UINT)T.wMonth,
               (UINT)T.wDay, (UINT)T.wHour, (UINT)T.wMinute, (UINT)T.wSecond);
    HANDLE F = ::CreateFileW(Name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (F == INVALID_HANDLE_VALUE)
    {
        RdxLog(TEXT("crash report #%i: no memory dump - %s could not be created"), (INT)Number, (const TCHAR*)Name);
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION X;
    X.ThreadId          = ::GetCurrentThreadId();
    X.ExceptionPointers = E;
    X.ClientPointers    = FALSE;
    const MINIDUMP_TYPE Type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                               MiniDumpWithUnloadedModules | MiniDumpScanMemory);
    const BOOL  bDone = Dump(::GetCurrentProcess(), ::GetCurrentProcessId(), F, Type, &X, nullptr, nullptr);
    const DWORD Error = bDone ? 0 : ::GetLastError();
    LARGE_INTEGER Size;
    Size.QuadPart = 0;
    ::GetFileSizeEx(F, &Size);
    ::CloseHandle(F);
    if (bDone)
        RdxLog(TEXT("crash report #%i: memory dump %s, %I64d KB - send it with the logs"), (INT)Number,
               (const TCHAR*)Name, Size.QuadPart >> 10);
    else
        RdxLog(TEXT("crash report #%i: the memory dump %s failed (error %u)"), (INT)Number, (const TCHAR*)Name,
               (UINT)Error);
}

static LONG CALLBACK OnException(EXCEPTION_POINTERS* E)
{
    if (!E || !E->ExceptionRecord || !E->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD Code = E->ExceptionRecord->ExceptionCode;
    if (!IsHardFault(Code) && !(GAllCodes && !IsChatter(Code)))
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD Where = (DWORD)(ULONG_PTR)E->ExceptionRecord->ExceptionAddress;
    const LONG  Done  = GReports;
    for (LONG i = 0; i < Done && i < MAX_REPORTS; i++)
        if (GReportedAt[i] == Where)
            return EXCEPTION_CONTINUE_SEARCH;
    // One report at a time, and never from inside one: a fault in the report itself would come back here.
    if (::InterlockedCompareExchange(&GInReport, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    const LONG Number = ::InterlockedIncrement(&GReports);
    if (Number <= MAX_REPORTS)
    {
        GReportedAt[Number - 1] = Where;
        __try
        {
            Report(E, Number);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (GDumps && IsHardFault(E->ExceptionRecord->ExceptionCode) &&
            ::InterlockedCompareExchange(&GDumped, 1, 0) == 0)
        {
            __try
            {
                WriteDump(E, Number);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
    }
    ::InterlockedExchange(&GInReport, 0);
    // Only a report: the engine's own handling goes on as before.
    return EXCEPTION_CONTINUE_SEARCH;
}

void RdxCrashLogInstall()
{
    if (GHandler)
        return;
    // First in the line: a handler before ours could change the picture before we see it.
    GAllCodes = FreshTempMarker(L"SecondRevision-crash-all.txt");
    GHandler = ::AddVectoredExceptionHandler(1, OnException);
    if (GAllCodes)
        RdxLog(TEXT("crash report (marker): every exception code, not only the hard faults - a diagnosis only"));
    // The check address: scripts/crash_symbols.py looks it up in the PDB and must find this very function there - the proof
    // that the offsets it translates belong to this build.
    FCodePlace Self;
    FindCodePlace((DWORD)(ULONG_PTR)&RdxCrashLogInstall, Self);
    RdxLog(TEXT("crash report: %s; check address: %s+0x%08X = RdxCrashLogInstall"),
           GHandler ? TEXT("on") : TEXT("could NOT be installed"), Self.Name[0] ? Self.Name : TEXT("?"),
           (UINT)Self.Offset);
}

void RdxCrashLogRemove()
{
    if (!GHandler)
        return;
    ::RemoveVectoredExceptionHandler(GHandler);
    GHandler = nullptr;
}

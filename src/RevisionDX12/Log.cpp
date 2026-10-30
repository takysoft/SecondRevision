#include "Pch.h"
#include "Log.h"

#include <stdio.h>
#include <stdarg.h>
#include <share.h>

static FILE* GLogFile   = nullptr;
static UBOOL GLogFailed = 0;
static UBOOL GLogOpened = 0;   // opened once already in this process
static UBOOL            GLogDebug  = 0;   // LogLevel=Debug - see RdxLogSetDebug
static UBOOL            GLogCapped = 0;   // the cap reached: nothing more goes to the file
static unsigned __int64 GLogBytes  = 0;   // what this process has written to the file
enum { DebugCapMB = 1024 };

void RdxLogSetDebug(UBOOL bOn)
{
    GLogDebug = bOn ? 1 : 0;
}

UBOOL RdxLogIsDebug()
{
    return GLogDebug;
}

static void EnsureOpen()
{
    if (GLogFile || GLogFailed)
        return;

    // The working directory is the game's System folder while the game runs, so a
    // plain relative name lands next to Revision.log. One file per game session: truncated
    // on the FIRST open only. The engine closes and re-creates the render device within a
    // session - measured: on every return from Alt+Tab - and truncating again then wiped
    // everything before it, the Alt+Tab itself included.
    //
    // _fsopen with _SH_DENYWR, NOT fopen_s: fopen_s takes the file EXCLUSIVELY, so
    // nothing can read the log while the game runs. That is why every look at the log
    // meant killing the game first, and why a script watching it live only ever got
    // "the file is in use by another process".
    //
    // The session before is kept as SecondRevision.1.log, and the one before that as .2: a game that fell over is
    // started again straight away, and its log - the only word of what happened - went with the new start.
    if (!GLogOpened)
    {
        ::MoveFileExA("SecondRevision.1.log", "SecondRevision.2.log", MOVEFILE_REPLACE_EXISTING);
        ::MoveFileExA("SecondRevision.log", "SecondRevision.1.log", MOVEFILE_REPLACE_EXISTING);
    }
    GLogFile = _fsopen("SecondRevision.log", GLogOpened ? "a" : "w", _SH_DENYWR);
    if (!GLogFile)
    {
        GLogFile   = nullptr;
        GLogFailed = 1;      // do not retry every single message
        return;
    }
    GLogOpened = 1;
}

void RdxLog(const TCHAR* Fmt, ...)
{
    TCHAR Buffer[1024];

    va_list Args;
    va_start(Args, Fmt);
    // appSprintf is variadic, not va_list based, so format through the CRT and keep
    // the engine's TCHAR width. With /Zc:wchar_t- TCHAR is unsigned short, which is
    // what the wide CRT expects here.
    _vsnwprintf_s(reinterpret_cast<wchar_t*>(Buffer), 1024, _TRUNCATE,
                  reinterpret_cast<const wchar_t*>(Fmt), Args);
    va_end(Args);

    Buffer[1023] = 0;

    // In-context copy for Revision.log.
    debugf(TEXT("SecondRevision: %s"), Buffer);

    // Durable copy, flushed now so a forced kill cannot lose it.
    EnsureOpen();
    if (GLogFile && !GLogCapped)
    {
        const ANSICHAR* const Text = appToAnsi(Buffer);
        fputs(Text, GLogFile);
        fputc('\n', GLogFile);
        GLogBytes += (unsigned __int64)strlen(Text) + 1;
        if (GLogDebug && GLogBytes > ((unsigned __int64)DebugCapMB << 20))
        {
            fputs("log: LogLevel=Debug reached its cap of 1024 MB - nothing more is written to this file\n", GLogFile);
            GLogCapped = 1;
        }
        fflush(GLogFile);
    }
}

void RdxLogLineFromAnyThread(const char* Line)
{
    if (!GLogFile || !Line)
        return;
    fputs(Line, GLogFile);
    fflush(GLogFile);
}

// See Log.h. What the marker asks to fail: a text the name has to hold, how many of its matches go through first, and
// how many fail after those.
struct FForcedFailure
{
    char Text[64];
    int  Most;      // so many matches fail and the later ones go through; 0 is every one
    int  Skip;      // so many matches go through first: what a start makes, where the one wanted is made again later
    int  Seen;      // the matches so far
};
enum { FORCED_FAILURES = 32 };
static FForcedFailure GForced[FORCED_FAILURES];
static int            GForcedCount = 0;

// The marker's text into GForced: "name", or several with ';' between them, each with "*count" and "@skip" after it,
// or with neither. Lower case - the names are things like CreateCommittedResource(shadow metric), so 'shadow' walks
// the shadows' paths and 'water' the water's.
static void ParseForcedFailures(const char* Buf, int Got)
{
    int i = 0;
    while (i < Got && GForcedCount < FORCED_FAILURES)
    {
        FForcedFailure& F = GForced[GForcedCount];
        F.Most = 0;
        F.Skip = 0;
        F.Seen = 0;
        int n = 0;
        // Spaces inside a name are KEPT: the names are things like "ambient occlusion", and a reader that dropped
        // them matched nothing and said so by finding no failures at all - which reads exactly like a clean run.
        for (; i < Got && Buf[i] != ';' && Buf[i] != '*' && Buf[i] != '@'; i++)
            if (Buf[i] >= ' ' && n < 63 && (n > 0 || Buf[i] != ' '))
                F.Text[n++] = (Buf[i] >= 'A' && Buf[i] <= 'Z') ? (char)(Buf[i] + 32) : Buf[i];
        int* Number = &F.Most;
        for (; i < Got && Buf[i] != ';'; i++)
        {
            if (Buf[i] == '*')
                Number = &F.Most;
            else if (Buf[i] == '@')
                Number = &F.Skip;
            else if (Buf[i] >= '0' && Buf[i] <= '9')
                *Number = *Number * 10 + (Buf[i] - '0');
        }
        i++;        // the ';', or the end
        while (n > 0 && F.Text[n - 1] == ' ')
            n--;
        F.Text[n] = 0;
        if (n > 0)
            GForcedCount++;
    }
}

// Read once, on the first question of the run.
static void ReadForcedFailures()
{
    static bool bRead = false;
    if (bRead)
        return;
    bRead = true;
    WCHAR Path[MAX_PATH + 64];
    const DWORD N = ::GetTempPathW(MAX_PATH, Path);
    if (N == 0 || N >= MAX_PATH)
        return;
    ::lstrcatW(Path, L"SecondRevision-fail-alloc.txt");
    HANDLE F = ::CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (F == INVALID_HANDLE_VALUE)
        return;
    FILETIME Written;
    FILETIME Now;
    char     Buf[512];
    DWORD    Got = 0;
    ::GetSystemTimeAsFileTime(&Now);
    if (::GetFileTime(F, nullptr, nullptr, &Written) && ::ReadFile(F, Buf, sizeof(Buf), &Got, nullptr))
    {
        const ULONGLONG W = ((ULONGLONG)Written.dwHighDateTime << 32) | Written.dwLowDateTime;
        const ULONGLONG T = ((ULONGLONG)Now.dwHighDateTime << 32) | Now.dwLowDateTime;
        // Ten minutes, in hundreds of nanoseconds: a marker left behind from a week ago is not a request.
        if (T >= W && (T - W) < 10ULL * 60ULL * 10000000ULL)
            ParseForcedFailures(Buf, (int)Got);
    }
    ::CloseHandle(F);
}

// Whether What holds Want, What in any case and Want in lower.
static bool NameHolds(const TCHAR* What, const char* Want)
{
    for (int i = 0; What[i]; i++)
    {
        int k = 0;
        for (; Want[k]; k++)
        {
            const TCHAR C = What[i + k];
            if (!C)
                break;
            const char Lower = (C >= 'A' && C <= 'Z') ? (char)(C + 32) : (char)C;
            if (Lower != Want[k])
                break;
        }
        if (!Want[k])
            return true;
    }
    return false;
}

bool RdxForcedFailure(const TCHAR* What)
{
    ReadForcedFailures();
    if (!What)
        return false;
    for (int f = 0; f < GForcedCount; f++)
    {
        FForcedFailure& F = GForced[f];
        if (!NameHolds(What, F.Text))
            continue;
        // The first Skip matches go through, the next Most fail - every one after them where Most is 0 - and the rest
        // go through again. The count stops short of where it would wrap: a name asked about for every texture of
        // every frame gets there in hours.
        if (F.Seen < 0x7FFFFFF0)
            F.Seen++;
        if (F.Seen <= F.Skip || (F.Most > 0 && F.Seen > F.Skip + F.Most))
            continue;
        // A line for the first few, then one in every hundred thousand with the running count. Unbounded, this
        // wrote 18 GB of log in ten minutes the first time it met the texture cache, which asks again for every
        // texture of every frame: the message that mattered was "91 million", not the 91 millionth line.
        static int Told = 0;
        Told++;
        if (Told <= 20)
            RdxLog(TEXT("%s: FORCED FAILURE (the marker SecondRevision-fail-alloc.txt) - a measurement, not a fault%s"),
                   What,
                   (Told == 20) ? TEXT(" - from here only every hundred thousandth is written") : TEXT(""));
        else if ((Told % 100000) == 0)
            RdxLog(TEXT("%s: FORCED FAILURE number %i"), What, Told);
        return true;
    }
    return false;
}

// The start's clock (RdxStartupMark): the marks and their times, in the FILETIME units Windows gives the process's own
// creation in - a hundred nanoseconds.
struct FStartupMark
{
    const TCHAR* What;
    ULONGLONG    At;
    DOUBLE       ShaderMs;     // RdxStartupNoteShader's sums when the mark was made
    INT          Compiled;
    INT          Cached;
};
static FStartupMark GStartupMarks[64];
static INT          GStartupCount    = 0;
static UBOOL        GStartupReported = 0;
static DOUBLE       GShaderMs        = 0.0;
static INT          GShadersCompiled = 0;
static INT          GShadersCached   = 0;

void RdxStartupNoteShader(DOUBLE Ms, UBOOL bCompiled)
{
    GShaderMs += Ms;
    if (bCompiled)
        GShadersCompiled++;
    else
        GShadersCached++;
}

INT RdxStartupShaders()
{
    return GShadersCompiled;
}

static ULONGLONG FileTimeNow()
{
    FILETIME F;
    ::GetSystemTimePreciseAsFileTime(&F);
    return ((ULONGLONG)F.dwHighDateTime << 32) | F.dwLowDateTime;
}

void RdxStartupMark(const TCHAR* What)
{
    if (GStartupReported || GStartupCount >= (INT)(sizeof(GStartupMarks) / sizeof(GStartupMarks[0])))
        return;
    FStartupMark& M = GStartupMarks[GStartupCount++];
    M.What     = What;
    M.At       = FileTimeNow();
    M.ShaderMs = GShaderMs;
    M.Compiled = GShadersCompiled;
    M.Cached   = GShadersCached;
}

// A step's shaders, for its line: nothing where it had none.
static FString StepShaders(const FStartupMark& M, DOUBLE WasMs, INT WasCompiled, INT WasCached)
{
    const INT Compiled = M.Compiled - WasCompiled;
    const INT Cached   = M.Cached - WasCached;
    if (!Compiled && !Cached)
        return FString();
    return FString::Printf(TEXT("  (shaders %.0f ms: %i compiled, %i from the cache)"), M.ShaderMs - WasMs, Compiled,
                           Cached);
}

void RdxStartupReport(const TCHAR* When)
{
    if (GStartupReported)
        return;
    GStartupReported = 1;
    FILETIME Created, Exited, Kernel, User;
    ULONGLONG Start = 0;
    if (::GetProcessTimes(::GetCurrentProcess(), &Created, &Exited, &Kernel, &User))
        Start = ((ULONGLONG)Created.dwHighDateTime << 32) | Created.dwLowDateTime;
    if (!Start && GStartupCount > 0)
        Start = GStartupMarks[0].At;
    const ULONGLONG Now = FileTimeNow();
    RdxLog(TEXT("start: %s - %.0f ms after the game's process began; the steps (ms since it began, and each step's own):"),
           When, (DOUBLE)(Now - Start) / 10000.0);
    ULONGLONG Before      = Start;
    DOUBLE    WasMs       = 0.0;
    INT       WasCompiled = 0;
    INT       WasCached   = 0;
    for (INT i = 0; i < GStartupCount; i++)
    {
        const FStartupMark& M = GStartupMarks[i];
        RdxLog(TEXT("  start %6.0f  +%5.0f  %s%s"), (DOUBLE)(M.At - Start) / 10000.0, (DOUBLE)(M.At - Before) / 10000.0,
               M.What, *StepShaders(M, WasMs, WasCompiled, WasCached));
        Before      = M.At;
        WasMs       = M.ShaderMs;
        WasCompiled = M.Compiled;
        WasCached   = M.Cached;
    }
    FStartupMark Last;
    Last.What     = When;
    Last.At       = Now;
    Last.ShaderMs = GShaderMs;
    Last.Compiled = GShadersCompiled;
    Last.Cached   = GShadersCached;
    RdxLog(TEXT("  start %6.0f  +%5.0f  %s%s"), (DOUBLE)(Now - Start) / 10000.0, (DOUBLE)(Now - Before) / 10000.0, When,
           *StepShaders(Last, WasMs, WasCompiled, WasCached));
    RdxLog(TEXT("start: shaders until here - %.0f ms on them: %i compiled, %i from the cache"), GShaderMs,
           GShadersCompiled, GShadersCached);

    // One line of it kept across sessions, beside the log (SecondRevision-starts.log): a slow start is noticed by
    // whoever plays, and by the time it is asked about, later sessions' logs have taken this one's place. When it
    // was, how long it took, what the shaders cost, and the two longest steps. Past a quarter of a megabyte the
    // file becomes SecondRevision-starts.1.log and a new one begins.
    INT    Long[2] = { -1, -1 };
    DOUBLE Took[2] = { 0.0, 0.0 };
    Before = Start;
    for (INT i = 0; i < GStartupCount; i++)
    {
        const DOUBLE Ms = (DOUBLE)(GStartupMarks[i].At - Before) / 10000.0;
        Before = GStartupMarks[i].At;
        if (Ms > Took[0])
        {
            Took[1] = Took[0];
            Long[1] = Long[0];
            Took[0] = Ms;
            Long[0] = i;
        }
        else if (Ms > Took[1])
        {
            Took[1] = Ms;
            Long[1] = i;
        }
    }
    FILE* Kept = _fsopen("SecondRevision-starts.log", "a", _SH_DENYWR);
    if (Kept)
        fseek(Kept, 0, SEEK_END);
    if (Kept && ftell(Kept) > 256 * 1024)
    {
        fclose(Kept);
        ::MoveFileExA("SecondRevision-starts.log", "SecondRevision-starts.1.log", MOVEFILE_REPLACE_EXISTING);
        Kept = _fsopen("SecondRevision-starts.log", "a", _SH_DENYWR);
    }
    if (Kept)
    {
        SYSTEMTIME T;
        ::GetLocalTime(&T);
        fprintf(Kept, "%04i-%02i-%02i %02i:%02i:%02i  %6.0f ms to %s; shaders %5.0f ms (%i compiled, %i cached)",
                (INT)T.wYear, (INT)T.wMonth, (INT)T.wDay, (INT)T.wHour, (INT)T.wMinute, (INT)T.wSecond,
                (DOUBLE)(Now - Start) / 10000.0, appToAnsi(When), GShaderMs, GShadersCompiled, GShadersCached);
        for (INT k = 0; k < 2; k++)
            if (Long[k] >= 0)
                fprintf(Kept, "; %s %.0f ms: %s", k ? "then" : "the longest step", Took[k],
                        appToAnsi(GStartupMarks[Long[k]].What));
        fputc('\n', Kept);
        fclose(Kept);
    }
}

void RdxLogClose()
{
    if (GLogFile)
    {
        fclose(GLogFile);
        GLogFile = nullptr;
    }
    GLogFailed = 0;
}

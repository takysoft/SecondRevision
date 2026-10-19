#include "Pch.h"
#include "ShaderCache.h"
#include "Log.h"
#include "RenderMath.h"

// The file, beside the DLL: a header, then the shaders one after another. Every number is little-endian, as the machine's.
//   header: "RDXSHC02", the number of shaders (4 bytes)
//   shader: the key (two times 8 bytes), the entry point (48 bytes, zero-padded), the target (16), when it was last
//           asked for (8: the system's time, in 100 ns), the size of the bytecode (4), a check sum of it (4), then the
//           bytecode itself
static const char GMagic[8] = { 'R', 'D', 'X', 'S', 'H', 'C', '0', '2' };
static const INT  EntryChars  = 48;
static const INT  TargetChars = 16;
static const INT  HeaderBytes = 8 + 4;
static const INT  ShaderHead  = 8 + 8 + EntryChars + TargetChars + 8 + 4 + 4;
// A file larger than this is not ours to read: the renderer's shaders together are a few megabytes at most.
static const LONGLONG MaxFileBytes = 64LL * 1024 * 1024;
// What is written back (RdxMath::ShaderCacheWritten): a shader nobody asked for in KeepFor is one whose text a newer
// build changed, or whose feature is not used any more; and the file stays within MaxKeptBytes. Both in the system
// time's 100 ns.
static const UINT64 OneDay       = 24ULL * 3600ULL * 10000000ULL;
static const UINT64 KeepFor      = 30ULL * OneDay;
static const UINT64 MaxKeptBytes = 8ULL * 1024 * 1024;

struct FShader
{
    UINT64 KeyA;
    UINT64 KeyB;
    char   Entry[EntryChars];
    char   Target[TargetChars];
    UINT64 Asked;       // when it was last asked for, to the day: see RdxCompile
    UINT32 Size;
    BYTE*  Bytes;
    UBOOL  bAskedNow;   // asked for, or made, in this process
};

static SRWLOCK  GLock        = SRWLOCK_INIT;
static FShader* GShaders     = nullptr;
static INT      GCount       = 0;
static INT      GMax         = 0;
static UBOOL    GStarted     = 0;      // the file read (or found missing), the DLL's identity taken
static UBOOL    GDisabled    = 0;      // RdxShaderCacheDisable
static UBOOL    GDirty       = 0;      // compiled since the file was read or written
static UBOOL    GTouched     = 0;      // a kept shader's day moved on: written at the renderer's end, not before
static UBOOL    GWriteFailed = 0;      // said once; not tried again every frame
static INT      GNewSince    = 0;      // how many of them are new, for the save's line
static DOUBLE   GLastCompile = 0.0;    // appSeconds of the last compile
static UINT64   GNow         = 0;      // the system's time when the cache started
static UINT32   GDllStamp    = 0;
static UINT32   GDllSize     = 0;
static WCHAR    GPath[MAX_PATH + 32];
static WCHAR    GTempPath[MAX_PATH + 32];
// Where the file goes when the DLL's own folder cannot take it (a game installed where the user may not write): a folder
// of the user's own, %LOCALAPPDATA%\SecondRevision, where every user can. Read from when the DLL's folder holds no file
// of use, written to when writing beside the DLL fails - from then on for the rest of the process.
static WCHAR    GAltDir[MAX_PATH + 32];
static WCHAR    GAltPath[MAX_PATH + 64];
static WCHAR    GAltTempPath[MAX_PATH + 64];
static UBOOL    GUseAlt      = 0;      // the file lives in the user's folder
static UBOOL    GSkipHome    = 0;      // RdxShaderCacheElsewhere: as if the DLL's folder could not be written
// Who is told before a shader is compiled (RdxCompileWatch).
static FRdxCompileWatch GWatch    = nullptr;
static void*            GWatchWho = nullptr;

// Two chains over the same bytes, each 64 bits wide: FNV-1a, and the same shape with a multiplier of its own. Together
// the key is 128 bits; two different questions sharing it is not a thing that happens by accident.
struct FKey
{
    UINT64 A;
    UINT64 B;
};

static void Mix(FKey& K, const void* Data, SIZE_T Length)
{
    const BYTE* P = (const BYTE*)Data;
    UINT64      A = K.A;
    UINT64      B = K.B;
    for (SIZE_T i = 0; i < Length; i++)
    {
        A = (A ^ P[i]) * 0x00000100000001B3ULL;
        B = (B ^ P[i]) * 0x9E3779B97F4A7C15ULL;
    }
    K.A = A;
    K.B = B;
}

static void MixNumber(FKey& K, UINT64 N)
{
    Mix(K, &N, sizeof(N));
}

// A text with its length in front, so that "ab" + "c" and "a" + "bc" are different questions; a missing one is a
// length no text has.
static void MixText(FKey& K, const char* Text)
{
    if (!Text)
    {
        MixNumber(K, ~0ULL);
        return;
    }
    const SIZE_T L = strlen(Text);
    MixNumber(K, L);
    Mix(K, Text, L);
}

static UINT32 CheckSum(const BYTE* Data, UINT32 Size)
{
    UINT32 H = 0x811C9DC5u;
    for (UINT32 i = 0; i < Size; i++)
        H = (H ^ Data[i]) * 0x01000193u;
    return H;
}

static void CopyName(char* Out, INT Chars, const char* Name)
{
    appMemzero(Out, Chars);
    for (INT i = 0; Name && Name[i] && i < Chars - 1; i++)
        Out[i] = Name[i];
}

// This DLL's link stamp and image size, from its own PE header, and the file's path beside it.
static void TakeIdentity()
{
    HMODULE Self = nullptr;
    GPath[0] = GTempPath[0] = 0;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR)&TakeIdentity, &Self) || !Self)
        return;
    const IMAGE_DOS_HEADER* Dos = (const IMAGE_DOS_HEADER*)Self;
    const IMAGE_NT_HEADERS* Nt  = (const IMAGE_NT_HEADERS*)((const BYTE*)Self + Dos->e_lfanew);
    GDllStamp = Nt->FileHeader.TimeDateStamp;
    GDllSize  = Nt->OptionalHeader.SizeOfImage;
    const DWORD N = ::GetModuleFileNameW(Self, GPath, MAX_PATH);
    if (N == 0 || N >= MAX_PATH)
    {
        GPath[0] = 0;
        return;
    }
    WCHAR* Slash = nullptr;
    for (WCHAR* P = GPath; *P; P++)
        if (*P == L'\\' || *P == L'/')
            Slash = P;
    if (!Slash)
    {
        GPath[0] = 0;
        return;
    }
    Slash[1] = 0;
    ::lstrcpyW(GTempPath, GPath);
    ::lstrcatW(GPath, L"SecondRevision.shadercache");
    ::lstrcatW(GTempPath, L"SecondRevision.shadercache.tmp");

    // The user's own folder, unless a measurement named another one.
    if (!GAltDir[0])
    {
        const DWORD Len = ::GetEnvironmentVariableW(L"LOCALAPPDATA", GAltDir, MAX_PATH);
        if (Len == 0 || Len >= MAX_PATH - 16)
            GAltDir[0] = 0;
        else
            ::lstrcatW(GAltDir, L"\\SecondRevision");
    }
    GAltPath[0] = GAltTempPath[0] = 0;
    if (GAltDir[0])
    {
        const INT Len = ::lstrlenW(GAltDir);
        if (Len > 0 && GAltDir[Len - 1] != L'\\' && GAltDir[Len - 1] != L'/')
            ::lstrcatW(GAltDir, L"\\");
        ::lstrcpyW(GAltPath, GAltDir);
        ::lstrcpyW(GAltTempPath, GAltDir);
        ::lstrcatW(GAltPath, L"SecondRevision.shadercache");
        ::lstrcatW(GAltTempPath, L"SecondRevision.shadercache.tmp");
    }
}

static UBOOL Keep(const FKey& K, const char* Entry, const char* Target, const BYTE* Bytes, UINT32 Size, UINT64 Asked,
                  UBOOL bAskedNow)
{
    if (GCount == GMax)
    {
        const INT      NewMax = GMax ? GMax * 2 : 128;
        FShader* const Grown  = (FShader*)realloc(GShaders, sizeof(FShader) * NewMax);
        if (!Grown)
            return 0;
        GShaders = Grown;
        GMax     = NewMax;
    }
    BYTE* const Copy = (BYTE*)malloc(Size ? Size : 1);
    if (!Copy)
        return 0;
    memcpy(Copy, Bytes, Size);
    FShader& S = GShaders[GCount++];
    S.KeyA = K.A;
    S.KeyB = K.B;
    CopyName(S.Entry, EntryChars, Entry);
    CopyName(S.Target, TargetChars, Target);
    S.Asked     = Asked;
    S.Size      = Size;
    S.Bytes     = Copy;
    S.bAskedNow = bAskedNow;
    return 1;
}

static FShader* Find(const FKey& K, const char* Entry, const char* Target)
{
    char E[EntryChars];
    char T[TargetChars];
    CopyName(E, EntryChars, Entry);
    CopyName(T, TargetChars, Target);
    for (INT i = 0; i < GCount; i++)
    {
        FShader& S = GShaders[i];
        if (S.KeyA == K.A && S.KeyB == K.B && memcmp(S.Entry, E, EntryChars) == 0 && memcmp(S.Target, T, TargetChars) == 0)
            return &S;
    }
    return nullptr;
}

static UINT32 ReadU32(const BYTE* P)
{
    UINT32 V;
    memcpy(&V, P, 4);
    return V;
}

static UINT64 ReadU64(const BYTE* P)
{
    UINT64 V;
    memcpy(&V, P, 8);
    return V;
}

static INT ReadFrom(const WCHAR* Path, const TCHAR* Where);

// Once, at the first compile of the process: the identity, the time, and whatever the file holds.
static void Start()
{
    GStarted = 1;
    TakeIdentity();
    FILETIME Now;
    ::GetSystemTimeAsFileTime(&Now);
    GNow = ((UINT64)Now.dwHighDateTime << 32) | Now.dwLowDateTime;
    if (GDisabled)
    {
        RdxLog(TEXT("shader cache (marker): off - every shader goes through the compiler, the file is left alone"));
        return;
    }
    if (!GPath[0] && !GAltPath[0])
    {
        RdxLog(TEXT("shader cache: no place for its file (the DLL's folder is not known) - every start compiles"));
        return;
    }
    // Beside the DLL first; where that holds no file of use, the user's own folder.
    const INT Home = GSkipHome ? 0 : ReadFrom(GPath, TEXT("beside the DLL"));
    if (Home != 1 && GAltPath[0])
    {
        const INT Alt = ReadFrom(GAltPath, TEXT("in the user's folder"));
        if (Alt != 0)
            GUseAlt = 1;
        if (Home == 0 && Alt == 0)
            RdxLog(TEXT("shader cache: no file yet - the shaders compiled now are kept for the next start"));
    }
    else if (Home == 0)
        RdxLog(TEXT("shader cache: no file yet - the shaders compiled now are kept for the next start"));
    if (GSkipHome)
    {
        GUseAlt = 1;
        RdxLog(TEXT("shader cache (marker): the DLL's folder is passed over - the file is %s"), GAltPath);
    }
}

// The file at Path: 1 read (all of it, or up to where it was damaged), -1 there but not of use (laid out another way,
// unreadable), 0 not there. Where says which place it is, for the log.
static INT ReadFrom(const WCHAR* Path, const TCHAR* Where)
{
    if (!Path || !Path[0])
        return 0;
    HANDLE F = ::CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                             nullptr);
    if (F == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER Size;
    BYTE*         Data = nullptr;
    DWORD         Got  = 0;
    if (::GetFileSizeEx(F, &Size) && Size.QuadPart >= HeaderBytes && Size.QuadPart <= MaxFileBytes)
    {
        Data = (BYTE*)malloc((SIZE_T)Size.QuadPart);
        if (Data && (!::ReadFile(F, Data, (DWORD)Size.QuadPart, &Got, nullptr) || Got != (DWORD)Size.QuadPart))
        {
            free(Data);
            Data = nullptr;
        }
    }
    ::CloseHandle(F);
    if (!Data)
    {
        RdxLog(TEXT("shader cache: its file %s could not be read - the shaders are compiled, and the file written anew"),
               Where);
        GDirty = 1;
        return -1;
    }
    if (memcmp(Data, GMagic, 8) != 0)
    {
        RdxLog(TEXT("shader cache: the file %s is laid out another way (an older renderer's) - the shaders are "
                    "compiled, and the file written anew"), Where);
        free(Data);
        GDirty = 1;
        return -1;
    }
    const UINT32 Count = ReadU32(Data + 8);
    SIZE_T       At    = HeaderBytes;
    UINT32       Taken = 0;
    for (; Taken < Count; Taken++)
    {
        if (At + ShaderHead > (SIZE_T)Got)
            break;
        const BYTE* const H    = Data + At;
        const UINT64      When = ReadU64(H + 16 + EntryChars + TargetChars);
        const UINT32      Len  = ReadU32(H + 16 + EntryChars + TargetChars + 8);
        const UINT32      Sum  = ReadU32(H + 16 + EntryChars + TargetChars + 12);
        if (At + ShaderHead + Len > (SIZE_T)Got || CheckSum(H + ShaderHead, Len) != Sum)
            break;
        FKey K;
        K.A = ReadU64(H);
        K.B = ReadU64(H + 8);
        char E[EntryChars + 1];
        char T[TargetChars + 1];
        memcpy(E, H + 16, EntryChars);
        memcpy(T, H + 16 + EntryChars, TargetChars);
        E[EntryChars]  = 0;
        T[TargetChars] = 0;
        if (!Keep(K, E, T, H + ShaderHead, Len, When, 0))
            break;
        At += ShaderHead + Len;
    }
    if (Taken < Count)
    {
        RdxLog(TEXT("shader cache: the file %s is damaged after %u of its %u shaders - the rest are compiled, and the file "
                    "written anew"), Where, Taken, Count);
        GDirty = 1;
    }
    else
        RdxLog(TEXT("shader cache: %u shaders read (%.1f MB) from SecondRevision.shadercache %s"), Taken,
               (DOUBLE)Got / (1024.0 * 1024.0), Where);
    free(Data);
    return 1;
}

HRESULT RdxCompile(LPCVOID Source, SIZE_T Length, LPCSTR SourceName, const D3D_SHADER_MACRO* Macros, ID3DInclude* Include,
                   LPCSTR Entry, LPCSTR Target, UINT Flags1, UINT Flags2, ID3DBlob** Code, ID3DBlob** Errors)
{
    // Texts #included from the disk are not the DLL's, so its build would not cover them: those are never kept.
    const UBOOL bKeepable = Code && Include != D3D_COMPILE_STANDARD_FILE_INCLUDE;
    FKey        K         = { 0xCBF29CE484222325ULL, 0x6A09E667F3BCC908ULL };
    if (bKeepable)
    {
        ::AcquireSRWLockExclusive(&GLock);
        if (!GStarted)
            Start();
        ::ReleaseSRWLockExclusive(&GLock);
        // A text handed over with no way to #include is all there is to the question, and its answer holds for every
        // build. One that #includes through the caller's own handler is not whole: the rest of it lives inside the
        // DLL, so the build is part of its question.
        if (Include)
        {
            MixNumber(K, GDllStamp);
            MixNumber(K, GDllSize);
        }
        MixNumber(K, Length);
        Mix(K, Source, Length);
        MixText(K, SourceName);
        for (const D3D_SHADER_MACRO* M = Macros; M && M->Name; M++)
        {
            MixText(K, M->Name);
            MixText(K, M->Definition);
        }
        MixNumber(K, ~0ULL);        // the end of the macros
        MixText(K, Entry);
        MixText(K, Target);
        MixNumber(K, Flags1);
        MixNumber(K, Flags2);

        ::AcquireSRWLockExclusive(&GLock);
        FShader* const Kept = GDisabled ? nullptr : Find(K, Entry, Target);
        if (Kept)
        {
            const DOUBLE T0   = appSeconds();
            HRESULT      Made = D3DCreateBlob(Kept->Size, Code);
            if (SUCCEEDED(Made) && *Code)
            {
                // Its day moves on a day at a time: the file is not written anew for every start of the game.
                Kept->bAskedNow = 1;
                if (GNow > Kept->Asked && GNow - Kept->Asked > OneDay)
                {
                    Kept->Asked = GNow;
                    GTouched    = 1;
                }
                memcpy((*Code)->GetBufferPointer(), Kept->Bytes, Kept->Size);
                if (Errors)
                    *Errors = nullptr;
                RdxStartupNoteShader((appSeconds() - T0) * 1000.0, 0);
                ::ReleaseSRWLockExclusive(&GLock);
                return S_OK;
            }
        }
        ::ReleaseSRWLockExclusive(&GLock);
    }

    // The watcher, told outside the lock: it draws, and may be long about it.
    ::AcquireSRWLockExclusive(&GLock);
    FRdxCompileWatch const Watch = GWatch;
    void* const            Who   = GWatchWho;
    ::ReleaseSRWLockExclusive(&GLock);
    if (Watch)
        Watch(Who);
    const DOUBLE  T0     = appSeconds();
    const HRESULT Result = D3DCompile(Source, Length, SourceName, Macros, Include, Entry, Target, Flags1, Flags2, Code,
                                      Errors);
    const DOUBLE  T1     = appSeconds();
    ::AcquireSRWLockExclusive(&GLock);
    RdxStartupNoteShader((T1 - T0) * 1000.0, 1);
    if (bKeepable && SUCCEEDED(Result) && *Code && !GDisabled)
    {
        if (Keep(K, Entry, Target, (const BYTE*)(*Code)->GetBufferPointer(), (UINT32)(*Code)->GetBufferSize(), GNow, 1))
        {
            GDirty = 1;
            GNewSince++;
        }
        GLastCompile = T1;
    }
    ::ReleaseSRWLockExclusive(&GLock);
    return Result;
}

void RdxShaderCacheElsewhere(const WCHAR* Folder)
{
    ::AcquireSRWLockExclusive(&GLock);
    if (Folder && Folder[0])
    {
        ::lstrcpynW(GAltDir, Folder, MAX_PATH - 32);
        GSkipHome = 1;
    }
    ::ReleaseSRWLockExclusive(&GLock);
}

void RdxCompileWatch(FRdxCompileWatch Watch, void* Who)
{
    ::AcquireSRWLockExclusive(&GLock);
    GWatch    = Watch;
    GWatchWho = Who;
    ::ReleaseSRWLockExclusive(&GLock);
}

void RdxShaderCacheDisable()
{
    ::AcquireSRWLockExclusive(&GLock);
    GDisabled = 1;
    ::ReleaseSRWLockExclusive(&GLock);
}

static void PutU32(BYTE*& P, UINT32 V)
{
    memcpy(P, &V, 4);
    P += 4;
}

static void PutU64(BYTE*& P, UINT64 V)
{
    memcpy(P, &V, 8);
    P += 8;
}

// Written aside and then put in place in one step: a game killed halfway leaves the old file whole, not half a new one.
static UBOOL WriteTo(const WCHAR* TempPath, const WCHAR* Path, const BYTE* Data, DWORD Total, DWORD& Error)
{
    DWORD  Put = 0;
    HANDLE F   = ::CreateFileW(TempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    UBOOL  bOk = F != INVALID_HANDLE_VALUE && ::WriteFile(F, Data, Total, &Put, nullptr) && Put == Total;
    if (F != INVALID_HANDLE_VALUE)
        ::CloseHandle(F);
    bOk   = bOk && ::MoveFileExW(TempPath, Path, MOVEFILE_REPLACE_EXISTING);
    Error = bOk ? 0 : ::GetLastError();
    if (!bOk)
        ::DeleteFileW(TempPath);
    return bOk;
}

void RdxShaderCacheSave(UBOOL bNow)
{
    // Unlocked first look: once a frame, and nearly always there is nothing to write. A day that moved on is not
    // worth a write of its own in the middle of the game: it goes with the next compile's, or at the renderer's end.
    if (!(GDirty || (bNow && GTouched)) || GDisabled || GWriteFailed || (!GPath[0] && !GAltPath[0]))
        return;
    ::AcquireSRWLockExclusive(&GLock);
    if (!(GDirty || (bNow && GTouched)) || (!bNow && appSeconds() - GLastCompile < 3.0))
    {
        ::ReleaseSRWLockExclusive(&GLock);
        return;
    }
    // Which of them go back into the file (RdxMath::ShaderCacheWritten).
    const INT   Held  = GCount;
    BYTE* const Lists = (BYTE*)malloc((SIZE_T)(Held ? Held : 1) * (sizeof(UINT64) + sizeof(UINT32) + 2));
    if (!Lists)
    {
        ::ReleaseSRWLockExclusive(&GLock);
        return;
    }
    UINT64* const Asked    = (UINT64*)Lists;
    UINT32* const Sizes    = (UINT32*)(Asked + Held);
    BYTE* const   AskedNow = (BYTE*)(Sizes + Held);
    BYTE* const   Written  = AskedNow + Held;
    for (INT i = 0; i < Held; i++)
    {
        Asked[i]    = GShaders[i].Asked;
        Sizes[i]    = GShaders[i].Size;
        AskedNow[i] = GShaders[i].bAskedNow ? 1 : 0;
    }
    const INT Count = RdxMath::ShaderCacheWritten(Asked, Sizes, AskedNow, Held, GNow, KeepFor,
                                                  MaxKeptBytes - HeaderBytes, ShaderHead, Written);
    SIZE_T    Total = HeaderBytes;
    for (INT i = 0; i < Held; i++)
        if (Written[i])
            Total += ShaderHead + GShaders[i].Size;
    BYTE* const Data = (BYTE*)malloc(Total);
    if (!Data)
    {
        free(Lists);
        ::ReleaseSRWLockExclusive(&GLock);
        return;
    }
    BYTE* P = Data;
    memcpy(P, GMagic, 8);
    P += 8;
    PutU32(P, (UINT32)Count);
    for (INT i = 0; i < Held; i++)
    {
        if (!Written[i])
            continue;
        const FShader& S = GShaders[i];
        PutU64(P, S.KeyA);
        PutU64(P, S.KeyB);
        memcpy(P, S.Entry, EntryChars);
        P += EntryChars;
        memcpy(P, S.Target, TargetChars);
        P += TargetChars;
        PutU64(P, S.Asked);
        PutU32(P, S.Size);
        PutU32(P, CheckSum(S.Bytes, S.Size));
        memcpy(P, S.Bytes, S.Size);
        P += S.Size;
    }
    free(Lists);
    const INT New = GNewSince;
    GDirty    = 0;
    GTouched  = 0;
    GNewSince = 0;
    ::ReleaseSRWLockExclusive(&GLock);

    // Beside the DLL, unless the file already lives in the user's folder; where the DLL's folder cannot take it, there.
    DWORD Error = 0;
    UBOOL bOk   = 0;
    if (!GUseAlt && GPath[0])
    {
        bOk = WriteTo(GTempPath, GPath, Data, (DWORD)Total, Error);
        if (!bOk && GAltPath[0])
        {
            RdxLog(TEXT("shader cache: the DLL's folder does not take the file (error %u) - it goes to %s"), Error,
                   GAltPath);
            GUseAlt = 1;
        }
    }
    if (!bOk && GUseAlt && GAltPath[0])
    {
        ::CreateDirectoryW(GAltDir, nullptr);          // an existing folder is fine; a failure shows in the write
        bOk = WriteTo(GAltTempPath, GAltPath, Data, (DWORD)Total, Error);
    }
    free(Data);
    if (!bOk)
    {
        GWriteFailed = 1;
        RdxLog(TEXT("shader cache: SecondRevision.shadercache could not be written (error %u) - the next start compiles "
                    "again; nothing else changes"), Error);
        return;
    }
    RdxLog(TEXT("shader cache: %i shaders written (%.1f MB), %i of them new, %i left out (not asked for in %i days, or "
                "over the %i MB kept), %s"), Count, (DOUBLE)Total / (1024.0 * 1024.0), New, Held - Count,
           (INT)(KeepFor / OneDay), (INT)(MaxKeptBytes / (1024 * 1024)),
           GUseAlt ? TEXT("in the user's folder") : TEXT("beside the DLL"));
}

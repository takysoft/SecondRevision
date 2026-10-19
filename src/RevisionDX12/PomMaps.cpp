#include "Pch.h"

// Backend.h, not PomMaps.h: the texture cache's header leans on what Backend.h declares before it, so this is the
// one order that compiles. It brings PomMaps.h with it.
#include "Backend.h"
#include "Log.h"
#include "PomArchive.h"
#include "RenderMath.h"

// The file the packing tool writes (scripts/pom_pack.py): a header, then the height with its shrunken copies. Where the
// tool packed a colour as well, it follows the height; the relief does not use it, and it is not read.
namespace
{
    struct FPomHeader
    {
        DWORD Magic;          // 'RPOM'
        DWORD Version;
        DWORD Width;
        DWORD Height;
        DWORD Flags;          // see POM_FLAG_*
        DWORD Mips;           // how many shrunken copies follow each map, the full size one counted
        DWORD Spare[2];
    };
    enum { POM_MAGIC = 'MOPR', POM_VERSION = 1,
           POM_FLAG_HEIGHT_BC4 = 2 };  // the height is block compressed

    // How a map's bytes are laid out. A block compressed map keeps one 4x4 square of texels in eight bytes, which is
    // what the card reads: nothing is unpacked here or in the shader. The plain forms are still understood, because
    // the packing tool can still write them and the two are worth comparing.
    struct FMapShape
    {
        DXGI_FORMAT Format;
        INT         RowBytes;      // one row of the SOURCE: of texels, or of blocks
        INT         Rows;          // how many such rows
        INT         Bytes;
    };

    FMapShape ShapeOf(DXGI_FORMAT Format, INT W, INT H)
    {
        FMapShape S;
        S.Format = Format;
        const UBOOL bBlock = (Format == DXGI_FORMAT_BC4_UNORM || Format == DXGI_FORMAT_BC1_UNORM ||
                              Format == DXGI_FORMAT_BC1_UNORM_SRGB);
        if (bBlock)
        {
            S.RowBytes = ((W + 3) / 4) * 8;
            S.Rows     = (H + 3) / 4;
        }
        else
        {
            S.RowBytes = W * ((Format == DXGI_FORMAT_R16_UNORM) ? 2 : 4);
            S.Rows     = H;
        }
        S.Bytes = S.RowBytes * S.Rows;
        return S;
    }

    // A file of our own beside our own DLL, in SecondRevision.pom - the list of choices lives there.
    void PomFolder(WCHAR* Out, INT Max, const char* Method)
    {
        Out[0] = 0;
        HMODULE Self = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCWSTR)&PomFolder, &Self) || !Self)
            return;
        WCHAR Path[MAX_PATH + 64];
        const DWORD N = ::GetModuleFileNameW(Self, Path, MAX_PATH);
        if (N == 0 || N >= MAX_PATH)
            return;
        INT Cut = -1;
        for (INT i = 0; Path[i]; i++)
            if (Path[i] == L'\\' || Path[i] == L'/')
                Cut = i;
        if (Cut < 0)
            return;
        Path[Cut + 1] = 0;
        ::lstrcatW(Path, L"SecondRevision.pom\\");
        WCHAR Wide[64];
        INT w = 0;
        for (; Method[w] && w < 62; w++)
            Wide[w] = (WCHAR)Method[w];
        Wide[w] = 0;
        ::lstrcatW(Path, Wide);
        if ((INT)::lstrlenW(Path) + 2 < Max)
            ::lstrcpyW(Out, Path);
    }

    char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

    // Plain bytes as TCHARs, for a name that has to go into a log line. NOT ANSI_TO_TCHAR: that one allocates with
    // alloca, and alloca inside the argument list of a variadic call moves the stack while the arguments are still
    // being pushed. It crashed the game twice here, both times as a free() of a pointer holding the line's own text.
    void WidenAscii(const char* In, TCHAR* Out, INT Max)
    {
        INT n = 0;
        for (; In && In[n] && n < Max - 1; n++)
            Out[n] = (TCHAR)(BYTE)In[n];
        Out[n] = 0;
    }

    // The key a texture is looked up by: its package and its own name, lower case, joined with a dot. The engine
    // spells a texture Package.Group.Name, and the group is not part of the file's name - the first piece and the last
    // are what identify it.
    template <typename TChar>
    void MakeKey(const TChar* PathName, char* Out, INT Max)
    {
        Out[0] = 0;
        if (!PathName)
            return;
        INT Len = 0;
        while (PathName[Len])
            Len++;
        INT FirstDot = -1, LastDot = -1;
        for (INT i = 0; i < Len; i++)
            if (PathName[i] == (TChar)'.')
            {
                if (FirstDot < 0)
                    FirstDot = i;
                LastDot = i;
            }
        INT w = 0;
        const INT PkgEnd = (FirstDot >= 0) ? FirstDot : Len;
        for (INT i = 0; i < PkgEnd && w < Max - 2; i++)
            Out[w++] = Lower((char)PathName[i]);
        Out[w++] = '.';
        const INT NameStart = (LastDot >= 0) ? LastDot + 1 : 0;
        for (INT i = NameStart; i < Len && w < Max - 1; i++)
            Out[w++] = Lower((char)PathName[i]);
        Out[w] = 0;
    }

    UBOOL SameKey(const char* A, const char* B)
    {
        INT i = 0;
        for (; A[i] && B[i]; i++)
            if (A[i] != B[i])
                return 0;
        return A[i] == B[i];
    }
}

FPomLibrary::FPomLibrary()
    : Device(nullptr), Count(0), Uploaded(0), Hold(0), PackCount(0), bSaidFull(0), StagingCount(0), RetiredCount(0),
      CurrentFrame(0), Blank(nullptr)
{
    appMemzero(Entries, sizeof(Entries));
    appMemzero(Staging, sizeof(Staging));
    appMemzero(StagingFrame, sizeof(StagingFrame));
    appMemzero(Retired, sizeof(Retired));
    appMemzero(RetiredFrame, sizeof(RetiredFrame));
    appMemzero(MemoUsed, sizeof(MemoUsed));
    Method[0] = 0;
}

FPomLibrary::~FPomLibrary()
{
    Shutdown();
}

UBOOL FPomLibrary::Init(ID3D12Device* InDevice)
{
    Device = InDevice;
    return Device != nullptr;
}

void FPomLibrary::Shutdown()
{
    for (INT i = 0; i < Count; i++)
    {
        if (Entries[i].Height)
            Entries[i].Height->Release();
    }
    appMemzero(Entries, sizeof(Entries));
    for (INT i = 0; i < StagingCount; i++)
        if (Staging[i])
            Staging[i]->Release();
    StagingCount = 0;
    for (INT i = 0; i < RetiredCount; i++)
        if (Retired[i])
            Retired[i]->Release();
    RetiredCount = 0;
    if (Blank)
    {
        Blank->Release();
        Blank = nullptr;
    }
    Count = 0;
    Uploaded = 0;
    PackCount = 0;
}

void FPomLibrary::EnsureBlank(FTextureCache& Textures)
{
    if (Blank || !Device)
        return;

    D3D12_HEAP_PROPERTIES DefaultHeap;
    appMemzero(&DefaultHeap, sizeof(DefaultHeap));
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC TexDesc;
    appMemzero(&TexDesc, sizeof(TexDesc));
    TexDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    TexDesc.Width            = 1;
    TexDesc.Height           = 1;
    TexDesc.DepthOrArraySize = 1;
    TexDesc.MipLevels        = 1;
    TexDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    TexDesc.SampleDesc.Count = 1;
    // Nothing is copied into it and nothing ever reads it: what a descriptor needs is to BE one.
    if (FAILED(Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &TexDesc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                               __uuidof(ID3D12Resource), (void**)&Blank)))
    {
        Blank = nullptr;
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
    appMemzero(&Srv, sizeof(Srv));
    Srv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Srv.Texture2D.MipLevels     = 1;
    const INT Views = Textures.GetPomViews();
    for (INT i = 0; i < Views; i++)
        Device->CreateShaderResourceView(Blank, &Srv, Textures.GetCpuHandle(Textures.GetPomIndex(i)));
}

void FPomLibrary::SetMethod(const char* NewMethod)
{
    if (!NewMethod || !NewMethod[0])
        return;
    if (Method[0] && SameKey(Method, NewMethod))
        return;

    // What was put on the card before is kept until the GPU has passed the frames that used it; ReleaseStaging lets
    // it go. Dropping it here would free a resource a draw in flight is still reading.
    for (INT i = 0; i < Count; i++)
    {
        if (Entries[i].Height && RetiredCount < MAX_RETIRED)
        {
            RetiredFrame[RetiredCount] = CurrentFrame;
            Retired[RetiredCount++]    = Entries[i].Height;
        }
        else if (Entries[i].Height)
            Entries[i].Height->Release();
    }
    appMemzero(Entries, sizeof(Entries));
    appMemzero(MemoUsed, sizeof(MemoUsed));     // the remembered answers were about the old files
    Count = 0;
    Uploaded = 0;
    Hold = BACKEND_FRAME_COUNT;

    INT m = 0;
    for (; NewMethod[m] && m < 30; m++)
        Method[m] = Lower(NewMethod[m]);
    Method[m] = 0;
    PackCount = 0;
    bSaidFull = 0;
    TCHAR Where[64];
    WidenAscii(Method, Where, 64);
    RDX_LOGF("relief: height maps from the .%s file beside each texture package", Where);
}







UBOOL FPomLibrary::WriteList()
{
    UBOOL bOk = 1;
    for (INT i = 0; i < PackCount; i++)
        if (Packs[i].bChanged)
            bOk &= WritePack(i);
    return bOk;
}

UBOOL FPomLibrary::WritePack(INT Index)
{
    FPack& P = Packs[Index];
    if (!P.Path[0])
        return 0;
    // Beside the package's file: its own name with .txt after it. Into a file beside that first, which then takes its
    // place: a write cut short leaves the old list whole.
    WCHAR Path[MAX_PATH + 48], Temp[MAX_PATH + 64];
    ::lstrcpyW(Path, P.Path);
    ::lstrcatW(Path, L".txt");
    ::lstrcpyW(Temp, Path);
    ::lstrcatW(Temp, L".writing");
    HANDLE F = ::CreateFileW(Temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    UBOOL bOk = (F != INVALID_HANDLE_VALUE);
    for (INT e = 0; bOk && e < Count; e++)
    {
        const FEntry& E = Entries[e];
        if (E.Pack != Index)
            continue;
        // Name, file and depth to a tenth, tab separated - as the packing tool writes them.
        char  Line[MAX_KEY + MAX_FILE + 24];
        INT   n = 0;
        for (INT k = 0; E.Name[k]; k++)
            Line[n++] = E.Name[k];
        Line[n++] = '\t';
        for (INT k = 0; E.File[k]; k++)
            Line[n++] = E.File[k];
        Line[n++] = '\t';
        const INT Tenths = E.bOn ? (INT)(E.Depth * 10.0f + 0.5f) : 0;
        if (Tenths >= 100)
            Line[n++] = (char)('0' + (Tenths / 100) % 10);
        Line[n++] = (char)('0' + (Tenths / 10) % 10);
        Line[n++] = '.';
        Line[n++] = (char)('0' + Tenths % 10);
        Line[n++] = '\r';
        Line[n++] = '\n';
        DWORD Written = 0;
        bOk = ::WriteFile(F, Line, (DWORD)n, &Written, nullptr) && (INT)Written == n;
    }
    if (F != INVALID_HANDLE_VALUE)
        ::CloseHandle(F);
    bOk = bOk && ::MoveFileExW(Temp, Path, MOVEFILE_REPLACE_EXISTING);
    if (!bOk)
    {
        ::DeleteFileW(Temp);
        if (!P.bSaidUnwritable)
        {
            TCHAR Said[MAX_PATH + 48];
            INT   s = 0;
            for (; Path[s] && s < MAX_PATH + 47; s++)
                Said[s] = (TCHAR)Path[s];
            Said[s] = 0;
            RDX_LOGF("relief: %s could not be written - the depths changed hold until the game closes", Said);
        }
        P.bSaidUnwritable = 1;
        return 0;
    }
    P.bChanged = 0;
    return 1;
}

// One texture's line: the name - in double quotes when it has a space in it - padded to a column, on or off, and
// the depth to a tenth.


UBOOL FPomLibrary::GetOn(INT Entry) const
{
    return (Entry >= 0 && Entry < Count) ? Entries[Entry].bOn : 0;
}

FLOAT FPomLibrary::GetDepth(INT Entry) const
{
    return (Entry >= 0 && Entry < Count) ? Entries[Entry].Depth : RdxMath::PomDepthWorld;
}

void FPomLibrary::SetOn(INT Entry, UBOOL bOn)
{
    if (Entry < 0 || Entry >= Count)
        return;
    FEntry& E = Entries[Entry];
    E.bOn     = bOn ? 1 : 0;
    E.Depth   = bOn ? ((E.Depth > 0.0f) ? E.Depth : 1.0f) : 0.0f;
    if (E.Pack >= 0 && E.Pack < PackCount)
        Packs[E.Pack].bChanged = 1;
}

void FPomLibrary::SetDepth(INT Entry, FLOAT Depth)
{
    if (Entry < 0 || Entry >= Count)
        return;
    FEntry& E = Entries[Entry];
    E.Depth   = Clamp(Depth, 0.0f, 64.0f);
    E.bOn     = (E.Depth > 0.0f) ? 1 : 0;
    if (E.Pack >= 0 && E.Pack < PackCount)
        Packs[E.Pack].bChanged = 1;
}

const char* FPomLibrary::GetKey(INT Entry) const
{
    return (Entry >= 0 && Entry < Count) ? Entries[Entry].Key : "";
}

INT FPomLibrary::Find(const TCHAR* PathName, INT USize, INT VSize)
{
    // No size to match: the file beside the package the engine loads holds the maps made for ITS pictures.
    (void)USize;
    (void)VSize;
    if (!PathName || !Method[0])
        return -1;
    char Key[MAX_KEY];
    MakeKey(PathName, Key, MAX_KEY);
    if (!Key[0])
        return -1;
    for (INT Pass = 0; Pass < 2; Pass++)
    {
        for (INT i = 0; i < Count; i++)
            if (SameKey(Entries[i].Key, Key))
                return i;
        // Not among the packages opened so far: open its own, once, and look again.
        const INT Before = PackCount;
        if (Pass == 0 && (OpenPack(PathName) < 0 || PackCount == Before))
            return -1;
    }
    return -1;
}

INT FPomLibrary::OpenPack(const TCHAR* PathName)
{
    // The package is the path name's first piece.
    TCHAR Pack[MAX_PACK_NAME];
    char  PackKey[MAX_PACK_NAME];
    INT   n = 0;
    for (; PathName[n] && PathName[n] != (TCHAR)'.' && n < MAX_PACK_NAME - 1; n++)
    {
        Pack[n]    = PathName[n];
        PackKey[n] = Lower((char)PathName[n]);
    }
    Pack[n]    = 0;
    PackKey[n] = 0;
    if (n == 0)
        return -1;
    for (INT i = 0; i < PackCount; i++)
        if (SameKey(Packs[i].Name, PackKey))
            return i;
    if (PackCount >= MAX_PACKS)
        return -1;
    const INT Index = PackCount++;
    FPack&    P     = Packs[Index];
    for (INT k = 0; k <= n; k++)
        P.Name[k] = PackKey[k];
    P.Path[0] = 0;
    // Where the engine loads this package from - its own lookup, the one it loads packages with - and beside it the
    // same name with this size's extension.
    TCHAR Found[1024];
    Found[0] = 0;
    if (!appFindPackageFile(Pack, nullptr, Found) || !Found[0])
        return Index;
    INT Len = 0, Dot = -1;
    for (; Found[Len] && Len < MAX_PATH; Len++)
    {
        if (Found[Len] == (TCHAR)'.')
            Dot = Len;
        else if (Found[Len] == (TCHAR)'\\' || Found[Len] == (TCHAR)'/')
            Dot = -1;
    }
    const INT Keep = (Dot >= 0) ? Dot : Len;
    INT       w    = 0;
    for (; w < Keep && w < MAX_PATH; w++)
        P.Path[w] = (WCHAR)Found[w];
    P.Path[w++] = L'.';
    for (INT k = 0; Method[k] && w < MAX_PATH + 30; k++)
        P.Path[w++] = (WCHAR)Method[k];
    P.Path[w] = 0;
    ReadPack(Index, P.Path);
    return Index;
}

// Reads Bytes at Offset of an open file; false when the file does not have them.
static UBOOL ReadAt(HANDLE F, UINT Offset, BYTE* Out, INT Bytes)
{
    LARGE_INTEGER To;
    To.QuadPart = (LONGLONG)Offset;
    DWORD Got   = 0;
    return ::SetFilePointerEx(F, To, nullptr, FILE_BEGIN) && ::ReadFile(F, Out, (DWORD)Bytes, &Got, nullptr) &&
           (INT)Got == Bytes;
}

void FPomLibrary::ReadPack(INT Index, const WCHAR* Path)
{
    HANDLE F = ::CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (F == INVALID_HANDLE_VALUE)
    {
        Packs[Index].Path[0] = 0;           // no maps for this package: its surfaces draw as they always did
        return;
    }
    TCHAR Said[MAX_PATH + 32];
    INT   s = 0;
    for (; Path[s] && s < MAX_PATH + 31; s++)
        Said[s] = (TCHAR)Path[s];
    Said[s] = 0;
    LARGE_INTEGER Size;
    UBOOL         bOk       = ::GetFileSizeEx(F, &Size) && Size.QuadPart >= 22 && Size.QuadPart < 0x7fffffff;
    const INT     FileBytes = bOk ? (INT)Size.QuadPart : 0;
    // The end of the zip's directory is in its last 64 KB and 22 bytes, a comment and all.
    const INT     TailBytes = Min(FileBytes, 65536 + 22);
    unsigned      Items = 0, DirOffset = 0, DirBytes = 0;
    if (bOk)
    {
        BYTE* const Tail = (BYTE*)appMalloc(TailBytes, TEXT("RdxPomZipTail"));
        bOk = Tail && ReadAt(F, (UINT)(FileBytes - TailBytes), Tail, TailBytes) &&
              RdxArchive::ZipEnd(Tail, TailBytes, Items, DirOffset, DirBytes) &&
              (INT)(DirOffset + DirBytes) <= FileBytes && Items > 0;
        if (Tail)
            appFree(Tail);
    }
    RdxArchive::FZipItem* List = nullptr;
    INT                   Got  = 0;
    if (bOk)
    {
        BYTE* const Dir = (BYTE*)appMalloc(DirBytes, TEXT("RdxPomZipDir"));
        List = (RdxArchive::FZipItem*)appMalloc(sizeof(RdxArchive::FZipItem) * Items, TEXT("RdxPomZipItems"));
        bOk  = Dir && List && ReadAt(F, DirOffset, Dir, (INT)DirBytes);
        if (bOk)
            Got = RdxArchive::ZipItems(Dir, (INT)DirBytes, List, (INT)Items);
        bOk = bOk && Got > 0;
        // The items' names in lower case, as the header's are taken: a zip tool may keep either.
        for (INT i = 0; bOk && i < Got; i++)
            for (INT c = 0; List[i].Name[c]; c++)
                List[i].Name[c] = Lower(List[i].Name[c]);
        if (Dir)
            appFree(Dir);
    }
    // The list - a line a texture: the name the game calls it, its item in the zip and its depth, tab separated. The
    // one lying beside the package's file comes first (a depth changed in play is written there); else the zip's own,
    // <Package>.heightmap.txt - or header.txt, the name the first packages had.
    INT Header = -1;
    for (INT i = 0; bOk && i < Got && Header < 0; i++)
    {
        INT n = 0;
        while (List[i].Name[n])
            n++;
        if ((n > 14 && SameKey(List[i].Name + n - 14, ".heightmap.txt")) || SameKey(List[i].Name, "header.txt"))
            Header = i;
    }
    BYTE* Text      = nullptr;
    INT   TextBytes = 0;
    {
        WCHAR Beside[MAX_PATH + 48];
        ::lstrcpyW(Beside, Path);
        ::lstrcatW(Beside, L".txt");
        HANDLE T = ::CreateFileW(Beside, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (T != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER Size;
            if (::GetFileSizeEx(T, &Size) && Size.QuadPart > 0 && Size.QuadPart < 4 * 1024 * 1024)
            {
                Text = (BYTE*)appMalloc((INT)Size.QuadPart, TEXT("RdxPomList"));
                DWORD Read = 0;
                if (Text && ::ReadFile(T, Text, (DWORD)Size.QuadPart, &Read, nullptr) && (INT)Read == (INT)Size.QuadPart)
                    TextBytes = (INT)Read;
                else if (Text)
                {
                    appFree(Text);
                    Text = nullptr;
                }
            }
            ::CloseHandle(T);
        }
    }
    if (!Text)
        bOk = bOk && Header >= 0 &&
              ReadItem(F, List[Header].Local, List[Header].Packed, List[Header].Size, List[Header].Method, Text,
                       TextBytes);
    ::CloseHandle(F);
    INT Made = 0;
    for (INT i = 0; bOk && i < TextBytes;)
    {
        INT End = i;
        while (End < TextBytes && Text[End] != '\n' && Text[End] != '\r')
            End++;
        // The fields: name, file, depth.
        INT Tabs[2] = { End, End };
        for (INT c = i, t = 0; c < End && t < 2; c++)
            if (Text[c] == '\t')
                Tabs[t++] = c;
        const INT From = i;
        i = End;
        while (i < TextBytes && (Text[i] == '\n' || Text[i] == '\r'))
            i++;
        const INT FileEnd = (Tabs[1] < End) ? Tabs[1] : End;
        if (Tabs[0] >= End || Tabs[0] - From >= MAX_KEY || FileEnd - Tabs[0] - 1 >= MAX_FILE)
            continue;
        char  Name[MAX_KEY], File[MAX_FILE], Key[MAX_KEY], Lookup[MAX_FILE];
        INT   k = 0;
        for (INT c = From; c < Tabs[0]; c++, k++)
        {
            Name[k] = (char)Text[c];
            Key[k]  = Lower((char)Text[c]);
        }
        Name[k] = Key[k] = 0;
        k = 0;
        for (INT c = Tabs[0] + 1; c < FileEnd; c++, k++)
        {
            File[k]   = (char)Text[c];
            Lookup[k] = Lower((char)Text[c]);
        }
        File[k] = Lookup[k] = 0;
        // The depth: a number, a comma allowed for the point; none given is the default, 1.
        FLOAT Depth = 1.0f;
        if (Tabs[1] < End)
        {
            FLOAT Whole = 0.0f, Part = 0.0f, Scale = 1.0f;
            UBOOL bPart = 0, bDigit = 0;
            for (INT c = Tabs[1] + 1; c < End; c++)
            {
                const char d = (char)Text[c];
                if (d >= '0' && d <= '9')
                {
                    bDigit = 1;
                    if (bPart)
                        Part += (FLOAT)(d - '0') * (Scale *= 0.1f);
                    else
                        Whole = Whole * 10.0f + (FLOAT)(d - '0');
                }
                else if (d == '.' || d == ',')
                    bPart = 1;
                else if (d != ' ')
                    break;
            }
            if (bDigit)
                Depth = Whole + Part;
        }
        INT Item = -1;
        for (INT j = 0; j < Got && Item < 0; j++)
            if (j != Header && SameKey(List[j].Name, Lookup))
                Item = j;
        if (Item < 0)
            continue;
        if (Count >= MAX_ENTRIES)
        {
            if (!bSaidFull)
                RDX_LOGF("relief: no room for more height maps (%i) - %s and the packages after it go without", Count,
                         Said);
            bSaidFull = 1;
            break;
        }
        FEntry& E = Entries[Count++];
        appMemzero(&E, sizeof(E));
        for (k = 0; Key[k]; k++)
        {
            E.Key[k]  = Key[k];
            E.Name[k] = Name[k];
        }
        for (k = 0; File[k]; k++)
            E.File[k] = File[k];
        E.Pack      = Index;
        E.Local     = List[Item].Local;
        E.Packed    = List[Item].Packed;
        E.Size      = List[Item].Size;
        E.ZipMethod = List[Item].Method;
        E.HeightSrv = -1;
        E.Depth     = Clamp(Depth, 0.0f, 64.0f);
        E.bOn       = (E.Depth > 0.0f) ? 1 : 0;
        Made++;
    }
    if (Text)
        appFree(Text);
    if (List)
        appFree(List);
    if (bOk)
        RDX_LOGF("relief: %i height maps in %s", Made, Said);
    else
        RDX_LOGF("relief: %s could not be read as a zip with its list - its package goes without", Said);
}

UBOOL FPomLibrary::ReadItem(HANDLE F, UINT Local, UINT Packed, UINT Size, UINT ZipMethod, BYTE*& Data, INT& Bytes) const
{
    Data  = nullptr;
    Bytes = 0;
    BYTE Head[30];
    UINT Start = 0;
    if (Size > 256u * 1024u * 1024u || Packed > 256u * 1024u * 1024u || !ReadAt(F, Local, Head, 30) ||
        !RdxArchive::ZipDataStart(Head, Local, Start))
        return 0;
    if (ZipMethod != 0 && ZipMethod != 8)
        return 0;                           // another kind of packing than the two every zip tool writes
    if (ZipMethod == 0 && Packed != Size)
        return 0;
    BYTE* const In = (BYTE*)appMalloc(Packed > 0 ? Packed : 1, TEXT("RdxPomItem"));
    if (!In || !ReadAt(F, Start, In, (INT)Packed))
    {
        if (In)
            appFree(In);
        return 0;
    }
    if (ZipMethod == 0)
    {
        Data  = In;
        Bytes = (INT)Size;
        return 1;
    }
    BYTE* const Out = (BYTE*)appMalloc(Size > 0 ? Size : 1, TEXT("RdxPomItemUnpacked"));
    const UBOOL bOk = Out && RdxArchive::Inflate(In, (INT)Packed, Out, (INT)Size);
    appFree(In);
    if (!bOk)
    {
        if (Out)
            appFree(Out);
        return 0;
    }
    Data  = Out;
    Bytes = (INT)Size;
    return 1;
}

// The id alone is not enough to key on: the engine hands the same texture out at more than one size (the original
// and the high resolution one that replaces it), and a height map belongs to exactly one of them.
static INT MemoSlotOf(QWORD CacheId, INT USize, INT VSize, INT Slots)
{
    return (INT)((CacheId ^ (CacheId >> 32) ^ (QWORD)(USize * 31 + VSize)) & (QWORD)(Slots - 1));
}

INT FPomLibrary::Remembered(QWORD CacheId, INT USize, INT VSize) const
{
    // Before any size is chosen there is nothing to find. With none of the packages opened yet there may still be:
    // a package's file is opened by the first lookup of one of its textures.
    if (!Method[0])
        return -1;
    const INT Slot = MemoSlotOf(CacheId, USize, VSize, MEMO_SLOTS);
    if (MemoUsed[Slot] && MemoId[Slot] == CacheId && MemoU[Slot] == USize && MemoV[Slot] == VSize)
        return MemoEntry[Slot];
    return PomNotAsked;
}

INT FPomLibrary::FindCached(QWORD CacheId, const TCHAR* PathName, INT USize, INT VSize)
{
    if (!Method[0])
        return -1;
    const INT Slot  = MemoSlotOf(CacheId, USize, VSize, MEMO_SLOTS);
    const INT Entry = Find(PathName, USize, VSize);
    MemoId[Slot]    = CacheId;
    MemoU[Slot]     = USize;
    MemoV[Slot]     = VSize;
    MemoEntry[Slot] = Entry;
    MemoUsed[Slot]  = 1;
    return Entry;
}

INT FPomLibrary::GetHeightSrv(INT Entry) const
{
    return (Entry >= 0 && Entry < Count) ? Entries[Entry].HeightSrv : -1;
}

void FPomLibrary::GetSize(INT Entry, FLOAT& OutU, FLOAT& OutV) const
{
    OutU = OutV = 0.0f;
    if (Entry >= 0 && Entry < Count && Entries[Entry].MapU > 0 && Entries[Entry].MapV > 0)
    {
        OutU = 1.0f / (FLOAT)Entries[Entry].MapU;
        OutV = 1.0f / (FLOAT)Entries[Entry].MapV;
    }
}

void FPomLibrary::ReleaseStaging(INT FrameIndex)
{
    // Only what this slot left behind a whole round of frames ago. Letting go of everything every frame would free
    // an upload buffer while the frame that copies from it is still on the card.
    INT Keep = 0;
    for (INT i = 0; i < StagingCount; i++)
    {
        if (Staging[i] && StagingFrame[i] == FrameIndex)
        {
            Staging[i]->Release();
            continue;
        }
        Staging[Keep]      = Staging[i];
        StagingFrame[Keep] = StagingFrame[i];
        Keep++;
    }
    StagingCount = Keep;

    Keep = 0;
    for (INT i = 0; i < RetiredCount; i++)
    {
        if (Retired[i] && RetiredFrame[i] == FrameIndex)
        {
            Retired[i]->Release();
            continue;
        }
        Retired[Keep]      = Retired[i];
        RetiredFrame[Keep] = RetiredFrame[i];
        Keep++;
    }
    RetiredCount = Keep;
    CurrentFrame = FrameIndex;
    if (Hold > 0)
        Hold--;
}

UBOOL FPomLibrary::Prepare(INT Entry, ID3D12GraphicsCommandList* Cmd, FTextureCache& Textures)
{
    if (Entry < 0 || Entry >= Count || !Device || !Cmd)
        return 0;
    FEntry& E = Entries[Entry];
    if (E.HeightSrv >= 0)
        return 1;
    if (E.bTried)
        return 0;                 // read once and failed: not tried again every frame
    if (Hold > 0)
        return 0;                 // just after a set change - see Hold; the surface draws flat for those frames
    // Two descriptors to an entry, and the shader's table reads four from an entry's first: the entry needs the two
    // after its own to exist as well.
    if (Entry * 2 + 4 > Textures.GetPomViews())
        return 0;
    // One upload buffer an entry, and it has to stay alive until the frame that copies from it has been through the
    // card. With no room for one this frame, leave the entry alone and come back next frame - marking it tried here
    // would lose the map for the rest of the level.
    if (!HasStagingRoom())
        return 0;
    EnsureBlank(Textures);
    E.bTried = 1;

    if (E.Pack < 0 || E.Pack >= PackCount || !Packs[E.Pack].Path[0])
        return 0;
    HANDLE F = ::CreateFileW(Packs[E.Pack].Path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (F == INVALID_HANDLE_VALUE)
        return 0;
    BYTE* Dds   = nullptr;
    INT   Bytes = 0;
    const UBOOL bRead = ReadItem(F, E.Local, E.Packed, E.Size, E.ZipMethod, Dds, Bytes);
    ::CloseHandle(F);
    if (!bRead)
        return 0;
    // The DDS's map behind the header Upload reads: its size, how many halvings, block compressed.
    INT W = 0, H = 0, Mips = 0, At = 0;
    UBOOL bOk = RdxArchive::DdsBc4(Dds, Bytes, W, H, Mips, At) &&
                RdxArchive::Bc4Bytes(W, H, Min(Mips, (INT)MAX_MIPS)) <= (long long)(Bytes - At);
    BYTE* Data = nullptr;
    if (bOk)
    {
        const INT Payload = Bytes - At;
        Data = (BYTE*)appMalloc((INT)sizeof(FPomHeader) + Payload, TEXT("RdxPomMap"));
        bOk  = Data != nullptr;
        if (bOk)
        {
            FPomHeader& Hdr = *(FPomHeader*)Data;
            appMemzero(&Hdr, sizeof(Hdr));
            Hdr.Magic   = POM_MAGIC;
            Hdr.Version = POM_VERSION;
            Hdr.Width   = (DWORD)W;
            Hdr.Height  = (DWORD)H;
            Hdr.Flags   = POM_FLAG_HEIGHT_BC4;
            Hdr.Mips    = (DWORD)Min(Mips, (INT)MAX_MIPS);
            appMemcpy(Data + sizeof(FPomHeader), Dds + At, Payload);
            E.MapU = W;
            E.MapV = H;
            bOk    = Upload(E, Entry, Data, (INT)sizeof(FPomHeader) + Payload, Cmd, Textures);
        }
    }
    appFree(Dds);
    if (Data)
        appFree(Data);
    if (bOk)
        Uploaded++;
    return bOk;
}

UBOOL FPomLibrary::Upload(FEntry& E, INT Index, const BYTE* Data, INT Bytes, ID3D12GraphicsCommandList* Cmd,
                          FTextureCache& Textures)
{
    const FPomHeader& Hdr = *(const FPomHeader*)Data;
    const INT W = (INT)Hdr.Width, H = (INT)Hdr.Height;
    const INT Mips = Clamp<INT>((INT)Hdr.Mips, 1, MAX_MIPS);
    const DXGI_FORMAT HeightFormat = (Hdr.Flags & POM_FLAG_HEIGHT_BC4) ? DXGI_FORMAT_BC4_UNORM
                                                                       : DXGI_FORMAT_R16_UNORM;

    // Where every copy sits, in the file and in the buffer that carries it to the card. A row of a texture copy has
    // to start on a 256-byte boundary and each copy on a 512-byte one, so the two layouts are not the same and both
    // have to be worked out.
    struct FPiece
    {
        INT Mip, W, H, Src, SrcRow, Rows, Dst, Pitch;
    };
    FPiece Pieces[MAX_MIPS];
    INT    PieceCount = 0;
    INT    Read = (INT)sizeof(FPomHeader);
    UINT64 Place = 0;
    {
        INT w = W, h = H;
        for (INT m = 0; m < Mips; m++)
        {
            const FMapShape Shape = ShapeOf(HeightFormat, w, h);
            if (Read + Shape.Bytes > Bytes)
                return 0;                 // the file is shorter than its own header says
            FPiece& P = Pieces[PieceCount++];
            P.Mip     = m;
            P.W       = w;
            P.H       = h;
            P.Src     = Read;
            P.SrcRow  = Shape.RowBytes;
            P.Rows    = Shape.Rows;
            P.Pitch   = (Shape.RowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            Place     = (Place + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                        ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
            P.Dst     = (INT)Place;
            Place    += (UINT64)P.Pitch * P.Rows;
            Read     += Shape.Bytes;
            w = (w > 4) ? (w / 2) : w;
            h = (h > 4) ? (h / 2) : h;
        }
    }

    D3D12_HEAP_PROPERTIES DefaultHeap;
    appMemzero(&DefaultHeap, sizeof(DefaultHeap));
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES UploadHeap;
    appMemzero(&UploadHeap, sizeof(UploadHeap));
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC BufDesc;
    appMemzero(&BufDesc, sizeof(BufDesc));
    BufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufDesc.Width            = Place;
    BufDesc.Height           = 1;
    BufDesc.DepthOrArraySize = 1;
    BufDesc.MipLevels        = 1;
    BufDesc.SampleDesc.Count = 1;
    BufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* Upl = nullptr;
    if (FAILED(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               __uuidof(ID3D12Resource), (void**)&Upl)))
        return 0;

    BYTE* Mapped = nullptr;
    D3D12_RANGE NoRead;
    NoRead.Begin = 0;
    NoRead.End   = 0;
    if (FAILED(Upl->Map(0, &NoRead, (void**)&Mapped)))
    {
        Upl->Release();
        return 0;
    }
    for (INT i = 0; i < PieceCount; i++)
    {
        const FPiece& P = Pieces[i];
        for (INT y = 0; y < P.Rows; y++)
            appMemcpy(Mapped + P.Dst + (size_t)y * P.Pitch, Data + P.Src + (size_t)y * P.SrcRow, P.SrcRow);
    }
    Upl->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC TexDesc;
    appMemzero(&TexDesc, sizeof(TexDesc));
    TexDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    TexDesc.Width            = W;
    TexDesc.Height           = H;
    TexDesc.DepthOrArraySize = 1;
    TexDesc.MipLevels        = (UINT16)Mips;
    TexDesc.SampleDesc.Count = 1;

    TexDesc.Format = HeightFormat;
    ID3D12Resource* HeightRes = nullptr;
    if (FAILED(Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &TexDesc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               __uuidof(ID3D12Resource), (void**)&HeightRes)))
    {
        Upl->Release();
        return 0;
    }
    for (INT i = 0; i < PieceCount; i++)
    {
        const FPiece& P = Pieces[i];
        ID3D12Resource* const Res = HeightRes;
        D3D12_TEXTURE_COPY_LOCATION Dst, Src;
        appMemzero(&Dst, sizeof(Dst));
        Dst.pResource        = Res;
        Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Dst.SubresourceIndex = P.Mip;
        appMemzero(&Src, sizeof(Src));
        Src.pResource                          = Upl;
        Src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint.Offset             = (UINT64)P.Dst;
        Src.PlacedFootprint.Footprint.Format   = HeightFormat;
        Src.PlacedFootprint.Footprint.Width    = P.W;
        Src.PlacedFootprint.Footprint.Height   = P.H;
        Src.PlacedFootprint.Footprint.Depth    = 1;
        Src.PlacedFootprint.Footprint.RowPitch = P.Pitch;
        Cmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
    }

    // The height in the first of its entry's two slots; the second keeps the blank view it was given (EnsureBlank).
    struct FOne { ID3D12Resource* Res; DXGI_FORMAT Format; INT* Srv; INT Slot; };
    const FOne Maps[1] =
    {
        { HeightRes, HeightFormat, &E.HeightSrv, Index * 2 },
    };
    for (INT m = 0; m < 1; m++)
    {
        D3D12_RESOURCE_BARRIER Barrier;
        appMemzero(&Barrier, sizeof(Barrier));
        Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource   = Maps[m].Res;
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        // Both read states: as POM the pixel shader walks the height map, as tessellation the domain shader lifts
        // the surface by it (DSRelief) - and the pixel shader's state alone is not one a domain shader may read in.
        // A card that holds the two apart can stop at such a read, and the device is lost.
        Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Cmd->ResourceBarrier(1, &Barrier);

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
        appMemzero(&Srv, sizeof(Srv));
        Srv.Format                  = Maps[m].Format;
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels     = Mips;
        const INT Slot = Textures.GetPomIndex(Maps[m].Slot);
        Device->CreateShaderResourceView(Maps[m].Res, &Srv, Textures.GetCpuHandle(Slot));
        *Maps[m].Srv = Slot;
    }
    E.Height = HeightRes;
    E.Mips   = Mips;
    // What the card was actually given.
    TCHAR Name[MAX_KEY];
    WidenAscii(E.Key, Name, MAX_KEY);
    RDX_LOGF("relief: %s %ix%i, %i levels, flags %i - height in slot %i", Name, W, H, Mips, (INT)Hdr.Flags,
             E.HeightSrv);
    // Prepare made sure of the room before it read the file, so this always takes the first branch; the other one
    // is there so a future caller cannot leak.
    if (StagingCount < MAX_STAGING)
    {
        StagingFrame[StagingCount] = CurrentFrame;
        Staging[StagingCount++]    = Upl;
    }
    else
        Upl->Release();
    return E.HeightSrv >= 0;
}

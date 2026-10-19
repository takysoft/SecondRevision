// VolumeCheck: the renderer's volume walk - smoke, flames, waterfalls - run by itself, with no game.
//
// The walk is a compute pass with a root signature of its own, lists that grow with the frame and a shader that reads
// all of them: what is wrong in it shows as a removed device in the middle of a game, or as smoke that is simply not
// there. This builds the pass from the renderer's own source (Volumetrics.cpp, copied in at build time; the few
// things of the engine it touches are given by the headers beside this file), makes a device, hands the pass scenes
// made of what the game's levels hold - puffs of smoke from a unit and a half to ninety across, a candle's flame, a
// fireplace - and writes what the walk lays over the picture as images, with what the driver's debug layer said.
//
// On the software device (WARP) unless -gpu is given: it takes none of the graphics card's time.
//
//     VolumeCheck [-out <folder>] [-size <width> <height>] [-gpu] [-fail <word>] [-time <seconds>] [-only <scene>]
//
// -only: the one scene of that name, and none of the others.
// -fail: the allocations whose name holds the word are refused, as SecondRevision-fail-alloc.txt does in the game.
// -time: the walk's clock - what flickers by it (a flame's tongues, the discharges in a ball of glow) is somewhere
//        else at another moment, and a run at each of a few shows it.
#include "Pch.h"
#include "Log.h"
#include "ShaderCache.h"
#include "RenderMath.h"
#include "Volumetrics.h"

#include <d3d12sdklayers.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

// ---------------------------------------------------------------------------------------------------------------
// What the pass asks of the renderer around it
// ---------------------------------------------------------------------------------------------------------------

static const wchar_t* GFailWord   = nullptr;
static int            GLogRefused = 0;         // lines of the pass's own log that say something was refused

void RdxLog(const TCHAR* Fmt, ...)
{
    wchar_t Line[2048];
    va_list Args;
    va_start(Args, Fmt);
    _vsnwprintf(Line, 2047, Fmt, Args);
    va_end(Args);
    Line[2047] = 0;
    wprintf(L"    pass: %ls\n", Line);
    if (wcsstr(Line, L"could not") || wcsstr(Line, L"failed") || wcsstr(Line, L"FAILED"))
        GLogRefused++;
}

bool RdxForcedFailure(const TCHAR* What)
{
    return GFailWord && What && wcsstr(What, GFailWord) != nullptr;
}

HRESULT RdxCompile(LPCVOID Source, SIZE_T Length, LPCSTR SourceName, const D3D_SHADER_MACRO* Macros,
                   ID3DInclude* Include, LPCSTR Entry, LPCSTR Target, UINT Flags1, UINT Flags2, ID3DBlob** Code,
                   ID3DBlob** Errors)
{
    return D3DCompile(Source, Length, SourceName, Macros, Include, Entry, Target, Flags1, Flags2, Code, Errors);
}

// ---------------------------------------------------------------------------------------------------------------
// The scene handed to the walk
// ---------------------------------------------------------------------------------------------------------------

// The camera stands at the world's origin and looks along the world's +Y, with +Z up: view space is x to the right,
// y DOWN the screen and z into the picture, so a world point is (x, -z, y) to the view.
enum { MAX_PUFFS = 8192, PUFF_FLOATS = 12 };
static FLOAT GPuffs[MAX_PUFFS * PUFF_FLOATS];
static INT   GPuffCount = 0;
// The flames as the draw path hands them over (FBackend::AddFirePiece, AddFire), in a list made longer as the scene
// asks, as the backend's is (FBackend::HoldFlames) - with it the bodies' and the numbers they are made with. The
// bodies are made of the pieces as the backend makes them. How many flames found no room: none may.
static RdxMath::FFlameGather GFlames;
static RdxMath::FFlamePiece* GFlamePieces   = nullptr;
static FLOAT*                GFires         = nullptr;
static INT*                  GFireGroups    = nullptr;
static INT                   GFireCount     = 0;
static INT                   GFlamesRefused = 0;
// The bodies' lists by tile of the walk's target, as the backend builds them (FBackend::BuildFireTiles), each body's
// rectangle of tiles while they are made - and the same lists with every body in every tile, for the walk that asks
// every flame at every pixel: what the lists by tile must change nothing of but the time.
static INT*                  GFireRects     = nullptr;
static UINT*                 GFireLists     = nullptr;
static UINT*                 GFireListsAll  = nullptr;
static INT                   GFireListsHeld = 0;
static INT                   GFireAllHeld   = 0;
static FLOAT GFalls[FVolumetrics::MAX_FALLS * FVolumetrics::FALL_FLOATS];
static INT   GFallCount = 0;
// The falls' sheets as the draw path hands them over (FBackend::AddFall): the columns are made of these as the
// backend makes them.
static RdxMath::FFallGather GFallSheets;
static INT   GSceneW = 1280, GSceneH = 720;
static FLOAT GFocal  = 0.0f;
static FLOAT GTime   = 12.3f;       // the walk's clock (-time)
static const wchar_t* GOnly = nullptr;      // the one scene to run (-only), or all of them

static void ResetScene()
{
    GPuffCount = 0;
    GFireCount = 0;
    GFallCount = 0;
    GFlames.Begin();
    GFlamesRefused = 0;
    GFallSheets.Begin();
}

// A puff as the draw path gathers it (FBackend::AddCloudPuff): its middle in view space, its reach across and up,
// the colour its picture is painted in, the game's own fade of its card.
static void AddPuff(FLOAT X, FLOAT Y, FLOAT Z, FLOAT Radius, FLOAT Tall, FLOAT Fade, FLOAT Grey)
{
    if (GPuffCount >= MAX_PUFFS)
        return;
    FLOAT* const P = GPuffs + GPuffCount++ * PUFF_FLOATS;
    P[0] = X; P[1] = Y; P[2] = Z;
    P[3] = Radius;
    P[4] = Tall;
    P[5] = P[6] = P[7] = Grey;
    P[8]  = RdxMath::CloudPuffDensity(Min(Radius, Tall), Fade, RdxMath::CloudPuffDepth);
    P[9]  = Fade;
    P[10] = P[11] = 0.0f;
}

// Room for Wanted pieces of flame, twice the room before at least - a small first room, so that the scenes make the
// lists grow.
static bool HoldFlames(INT Wanted)
{
    if (Wanted <= GFlames.Room)
        return true;
    const INT Room = Max(Wanted, Max(GFlames.Room * 2, 16));
    RdxMath::FFlamePiece* const Pieces =
        (RdxMath::FFlamePiece*)realloc(GFlamePieces, (size_t)Room * sizeof(RdxMath::FFlamePiece));
    if (Pieces)
        GFlamePieces = Pieces;
    FLOAT* const Bodies = (FLOAT*)realloc(GFires, (size_t)Room * FVolumetrics::FIRE_FLOATS * sizeof(FLOAT));
    if (Bodies)
        GFires = Bodies;
    INT* const Groups = (INT*)realloc(GFireGroups, (size_t)Room * sizeof(INT));
    if (Groups)
        GFireGroups = Groups;
    INT* const Rects = (INT*)realloc(GFireRects, (size_t)Room * 4 * sizeof(INT));
    if (Rects)
        GFireRects = Rects;
    if (!Pieces || !Bodies || !Groups || !Rects)
        return false;
    GFlames.Hold(GFlamePieces, Room);
    return true;
}

// A level's fire, in the WORLD: Count flame sheets, each Wide by High on Foot with its middle at (X, Y) and lying
// Angle round from the world's X - offered whole, as the level's fires are (RenderDevice.cpp, OfferLevelFire). The
// view looks along +Y, so how deep a sheet stands is its Y.
struct FSheet { FLOAT X, Y, Foot, Wide, High, Angle; };
static void AddLevelFire(const FSheet* Sheets, INT Count)
{
    RdxMath::FFlamePiece Pieces[8];
    Count = Min(Count, 8);
    for (INT i = 0; i < Count; i++)
    {
        const FSheet& S = Sheets[i];
        Pieces[i] = RdxMath::FlameSheetPiece(S.X, S.Y, cosf(S.Angle), sinf(S.Angle), 0.5f * S.Wide, S.Foot,
                                             S.Foot + S.High, S.Y);
    }
    if (!HoldFlames(GFlames.Count + Count) || !GFlames.AddTogether(Pieces, Count))
        GFlamesRefused++;
}

// A candle's flame as the levels have it: two sheets Wide by High crossing at right angles.
static void AddCandle(FLOAT X, FLOAT Y, FLOAT Foot, FLOAT Wide, FLOAT High)
{
    const FSheet Two[2] = { { X, Y, Foot, Wide, High, 0.0f }, { X, Y, Foot, Wide, High, 1.5707963f } };
    AddLevelFire(Two, 2);
}

// The game's own flame card as the draw path hands it over (DrawTile): Wide by High, its middle at (X, Y, MidZ).
static void AddCardFlame(RdxMath::EFlame Flame, FLOAT X, FLOAT Y, FLOAT MidZ, FLOAT Wide, FLOAT High)
{
    if (!HoldFlames(GFlames.Count + 1) || !GFlames.Add(RdxMath::FlameCardPiece(Flame, X, Y, MidZ, Wide, High, 1.0f, Y)))
        GFlamesRefused++;
}

// A card of glow as the draw path hands it over (DrawTile): Wide across, its middle at (X, Y, MidZ), the game's fade
// of it and its picture's colour - the picture four fifths of the card across.
static void AddGlowCard(FLOAT X, FLOAT Y, FLOAT MidZ, FLOAT Wide, FLOAT Fade, const FLOAT* Hue)
{
    if (!HoldFlames(GFlames.Count + 1) ||
        !GFlames.Add(RdxMath::FlameGlowPiece(X, Y, MidZ, Wide, Wide, 0.8f, Fade, Y, Hue)))
        GFlamesRefused++;
}

// A mesh's flame polygon as the draw path hands it over (DrawGouraudPolygon): the box of its points.
static void AddMeshFlame(FLOAT LoX, FLOAT LoY, FLOAT LoZ, FLOAT HiX, FLOAT HiY, FLOAT HiZ)
{
    if (!HoldFlames(GFlames.Count + 1) ||
        !GFlames.Add(RdxMath::FlameMeshPiece(LoX, LoY, LoZ, HiX, HiY, HiZ, 0.5f * (LoY + HiY))))
        GFlamesRefused++;
}

// A level's fall, in the WORLD, offered whole as the level's falls are (RenderDevice.cpp, OfferLevelFall): Count
// sheets from Foot up to Top, each Wide across with its middle at (X, Y) and lying Angle round from the world's X,
// coming down into blue water.
struct FFallOf { FLOAT X, Y, Wide, Angle; };
static void AddLevelFall(const FFallOf* Sheets, INT Count, FLOAT Foot, FLOAT Top)
{
    RdxMath::FFallSheetPiece Pieces[8];
    Count = Min(Count, 8);
    for (INT i = 0; i < Count; i++)
    {
        RdxMath::FFallSheetPiece& P = Pieces[i];
        P.Mid[0]    = Sheets[i].X;
        P.Mid[1]    = Sheets[i].Y;
        P.Across[0] = cosf(Sheets[i].Angle);
        P.Across[1] = sinf(Sheets[i].Angle);
        P.Half      = 0.5f * Sheets[i].Wide;
        P.Bottom    = Foot;
        P.Top       = Top;
        P.Hue[0]    = 0.0f;
        P.Hue[1]    = 0.5f;
        P.Hue[2]    = 1.0f;
        P.bFoot     = 1;
    }
    GFallSheets.AddTogether(Pieces, Count, (INT)FVolumetrics::MAX_FALLS);
}

// A column of smoke as the game's generators make one: Count cards rising from (X, FootY) at depth Z, each growing
// from FromCard to ToCard units across and fading in and out again, as the incense burners' and the vents' do.
static void AddPlume(FLOAT X, FLOAT FootY, FLOAT Z, INT Count, FLOAT Rise, FLOAT FromCard, FLOAT ToCard, UINT Seed)
{
    for (INT i = 0; i < Count; i++)
    {
        Seed = Seed * 1664525u + 1013904223u;
        const FLOAT Age    = ((FLOAT)i + 0.5f) / (FLOAT)Count;
        const FLOAT Card   = FromCard + (ToCard - FromCard) * Age;
        const FLOAT Radius = 0.5f * Card * RdxMath::CloudFatness;
        const FLOAT Fade   = (Age < 0.2f) ? Age / 0.2f : 1.0f - 0.9f * (Age - 0.2f) / 0.8f;
        const FLOAT Sway   = ((FLOAT)((Seed >> 8) & 255) / 255.0f - 0.5f) * 0.6f * Card;
        AddPuff(X + Sway, FootY - Rise * Age, Z + 0.5f * Sway, Radius, Radius, Fade, 0.75f);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Direct3D
// ---------------------------------------------------------------------------------------------------------------

static ID3D12Device*              GDevice    = nullptr;
static ID3D12CommandQueue*        GQueue     = nullptr;
static ID3D12CommandAllocator*    GAllocator = nullptr;
static ID3D12GraphicsCommandList* GCmd       = nullptr;
static ID3D12Fence*               GFence     = nullptr;
static HANDLE                     GFenceHit  = nullptr;
static UINT64                     GFenceAt   = 0;
static ID3D12InfoQueue*           GInfo      = nullptr;
static int                        GErrors    = 0;

static void Barrier(ID3D12Resource* R, D3D12_RESOURCE_STATES From, D3D12_RESOURCE_STATES To)
{
    D3D12_RESOURCE_BARRIER B;
    memset(&B, 0, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = R;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = From;
    B.Transition.StateAfter  = To;
    GCmd->ResourceBarrier(1, &B);
}

// Runs what was recorded and waits for it; false where the device is gone or the wait runs out.
static bool Run()
{
    if (FAILED(GCmd->Close()))
    {
        printf("    the command list did not close\n");
        return false;
    }
    ID3D12CommandList* Lists[1] = { GCmd };
    GQueue->ExecuteCommandLists(1, Lists);
    GQueue->Signal(GFence, ++GFenceAt);
    GFence->SetEventOnCompletion(GFenceAt, GFenceHit);
    const DWORD Wait = WaitForSingleObject(GFenceHit, 600000);
    const HRESULT Gone = GDevice->GetDeviceRemovedReason();
    if (Wait != WAIT_OBJECT_0 || FAILED(Gone))
    {
        printf("    the device did not finish: wait %lu, removed reason 0x%08lX\n", Wait, (unsigned long)Gone);
        return false;
    }
    GAllocator->Reset();
    GCmd->Reset(GAllocator, nullptr);
    return true;
}

// What the debug layer said since the last call; counts the errors.
static void Messages()
{
    if (!GInfo)
        return;
    const UINT64 Count = GInfo->GetNumStoredMessages();
    for (UINT64 i = 0; i < Count; i++)
    {
        SIZE_T Size = 0;
        GInfo->GetMessage(i, nullptr, &Size);
        D3D12_MESSAGE* const M = (D3D12_MESSAGE*)malloc(Size);
        if (M && SUCCEEDED(GInfo->GetMessage(i, M, &Size)))
        {
            const bool bError = M->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                                M->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
            if (bError)
                GErrors++;
            if (bError || M->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
                printf("    debug layer %s: %s\n", bError ? "ERROR" : "warning", M->pDescription);
        }
        free(M);
    }
    GInfo->ClearStoredMessages();
}

static FLOAT HalfToFloat(unsigned short H)
{
    const UINT Sign = (H >> 15) & 1u, Exp = (H >> 10) & 31u, Man = H & 1023u;
    FLOAT V;
    if (Exp == 0)
        V = ldexpf((FLOAT)Man, -24);
    else if (Exp == 31)
        V = Man ? NAN : INFINITY;
    else
        V = ldexpf((FLOAT)(Man | 1024u), (INT)Exp - 25);
    return Sign ? -V : V;
}

static bool WriteBmp(const wchar_t* Path, const FLOAT* Rgba, INT W, INT H)
{
    FILE* const File = _wfopen(Path, L"wb");
    if (!File)
        return false;
    const INT Row = (W * 3 + 3) & ~3;
    BITMAPFILEHEADER Fh;
    BITMAPINFOHEADER Ih;
    memset(&Fh, 0, sizeof(Fh));
    memset(&Ih, 0, sizeof(Ih));
    Fh.bfType      = 0x4D42;
    Fh.bfOffBits   = sizeof(Fh) + sizeof(Ih);
    Fh.bfSize      = Fh.bfOffBits + (DWORD)(Row * H);
    Ih.biSize      = sizeof(Ih);
    Ih.biWidth     = W;
    Ih.biHeight    = H;
    Ih.biPlanes    = 1;
    Ih.biBitCount  = 24;
    fwrite(&Fh, sizeof(Fh), 1, File);
    fwrite(&Ih, sizeof(Ih), 1, File);
    BYTE* const Line = (BYTE*)calloc(Row, 1);
    for (INT y = H - 1; y >= 0 && Line; y--)
    {
        for (INT x = 0; x < W; x++)
        {
            const FLOAT* const P = Rgba + (y * W + x) * 4;
            for (INT k = 0; k < 3; k++)
            {
                const FLOAT V = (P[2 - k] != P[2 - k]) ? 0.0f : P[2 - k];       // blue first; a NaN as black
                Line[x * 3 + k] = (BYTE)(Clamp(V, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
        fwrite(Line, Row, 1, File);
    }
    free(Line);
    fclose(File);
    return true;
}

int wmain(int Argc, wchar_t** Argv)
{
    const wchar_t* OutDir = L".";
    bool bGpu = false;
    for (int i = 1; i < Argc; i++)
    {
        if (!wcscmp(Argv[i], L"-out") && i + 1 < Argc)
            OutDir = Argv[++i];
        else if (!wcscmp(Argv[i], L"-gpu"))
            bGpu = true;
        else if (!wcscmp(Argv[i], L"-fail") && i + 1 < Argc)
            GFailWord = Argv[++i];
        else if (!wcscmp(Argv[i], L"-time") && i + 1 < Argc)
            GTime = (FLOAT)_wtof(Argv[++i]);
        else if (!wcscmp(Argv[i], L"-only") && i + 1 < Argc)
            GOnly = Argv[++i];
        else if (!wcscmp(Argv[i], L"-size") && i + 2 < Argc)
        {
            GSceneW = _wtoi(Argv[++i]);
            GSceneH = _wtoi(Argv[++i]);
        }
    }
    if (GSceneW < 64 || GSceneH < 64 || GSceneW > 4096 || GSceneH > 4096)
    {
        printf("the size has to be between 64 and 4096 a side\n");
        return 2;
    }
    // The view: 75 degrees across, the game's own.
    GFocal = 0.5f * (FLOAT)GSceneW / tanf(37.5f * 3.14159265f / 180.0f);

    // The debug layer first: it has to be on before the device is made. Where it is not installed the run goes on
    // and says so - a refused pipeline or a removed device is still caught.
    ID3D12Debug* Debug = nullptr;
    const bool bLayer = SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void**)&Debug));
    if (bLayer)
        Debug->EnableDebugLayer();
    printf("the debug layer: %s\n", bLayer ? "on" : "NOT INSTALLED - only what the device itself refuses will show");

    IDXGIFactory4* Factory = nullptr;
    IDXGIAdapter*  Adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&Factory)) ||
        FAILED(bGpu ? Factory->EnumAdapters(0, &Adapter)
                    : Factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), (void**)&Adapter)) ||
        FAILED(D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&GDevice)))
    {
        printf("no device\n");
        return 2;
    }
    {
        DXGI_ADAPTER_DESC Desc;
        memset(&Desc, 0, sizeof(Desc));
        Adapter->GetDesc(&Desc);
        printf("the device: %ls\n", Desc.Description);
    }
    if (bLayer)
        GDevice->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&GInfo);

    D3D12_COMMAND_QUEUE_DESC Qd;
    memset(&Qd, 0, sizeof(Qd));
    Qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(GDevice->CreateCommandQueue(&Qd, __uuidof(ID3D12CommandQueue), (void**)&GQueue)) ||
        FAILED(GDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                               (void**)&GAllocator)) ||
        FAILED(GDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, GAllocator, nullptr,
                                          __uuidof(ID3D12GraphicsCommandList), (void**)&GCmd)) ||
        FAILED(GDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&GFence)))
    {
        printf("the queue, the list or the fence could not be made\n");
        return 2;
    }
    GFenceHit = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // The picture the walk is laid over and the depth it stops at: a wall 600 units off, and a nearer one, 90 units
    // off, down the right-hand eighth of the picture - smoke behind that one must end at its edge.
    const FLOAT Back[4]      = { 0.20f, 0.22f, 0.25f, 0.0f };
    const FLOAT FarWall      = 600.0f, NearWall = 90.0f;
    const FLOAT DepthFar[4]  = { 1.0f / FarWall, 0.0f, 0.0f, 0.0f };
    const FLOAT DepthNear[4] = { 1.0f / NearWall, 0.0f, 0.0f, 0.0f };
    const INT   NearFrom     = GSceneW - GSceneW / 8;

    D3D12_HEAP_PROPERTIES Hp;
    memset(&Hp, 0, sizeof(Hp));
    Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Td;
    memset(&Td, 0, sizeof(Td));
    Td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Td.Width            = (UINT64)GSceneW;
    Td.Height           = (UINT)GSceneH;
    Td.DepthOrArraySize = 1;
    Td.MipLevels        = 1;
    Td.SampleDesc.Count = 1;
    Td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE Cv;
    memset(&Cv, 0, sizeof(Cv));
    ID3D12Resource* SceneTex = nullptr;
    ID3D12Resource* Depth = nullptr;
    Td.Format = Cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    memcpy(Cv.Color, Back, sizeof(Back));
    HRESULT Hr = GDevice->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                  &Cv, __uuidof(ID3D12Resource), (void**)&SceneTex);
    Td.Format = Cv.Format = DXGI_FORMAT_R32_FLOAT;
    memcpy(Cv.Color, DepthFar, sizeof(DepthFar));
    if (SUCCEEDED(Hr))
        Hr = GDevice->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Td, D3D12_RESOURCE_STATE_RENDER_TARGET, &Cv,
                                              __uuidof(ID3D12Resource), (void**)&Depth);
    D3D12_DESCRIPTOR_HEAP_DESC Hd;
    memset(&Hd, 0, sizeof(Hd));
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Hd.NumDescriptors = 2;
    ID3D12DescriptorHeap* RtvHeap = nullptr;
    if (SUCCEEDED(Hr))
        Hr = GDevice->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), (void**)&RtvHeap);
    if (FAILED(Hr))
    {
        printf("the picture or the depth could not be made (0x%08lX)\n", (unsigned long)Hr);
        return 2;
    }
    const UINT RtvStep = GDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE SceneRtv = RtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE DepthRtv = SceneRtv;
    DepthRtv.ptr += RtvStep;
    GDevice->CreateRenderTargetView(SceneTex, nullptr, SceneRtv);
    GDevice->CreateRenderTargetView(Depth, nullptr, DepthRtv);

    // Where the picture is read back.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Foot;
    UINT64 ReadBytes = 0;
    {
        const D3D12_RESOURCE_DESC Sd = SceneTex->GetDesc();
        GDevice->GetCopyableFootprints(&Sd, 0, 1, 0, &Foot, nullptr, nullptr, &ReadBytes);
    }
    D3D12_HEAP_PROPERTIES Rp;
    memset(&Rp, 0, sizeof(Rp));
    Rp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC Bd;
    memset(&Bd, 0, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = ReadBytes;
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* Read = nullptr;
    if (FAILED(GDevice->CreateCommittedResource(&Rp, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                __uuidof(ID3D12Resource), (void**)&Read)))
    {
        printf("the read-back buffer could not be made\n");
        return 2;
    }

    // The depth, once: it is the same wall for every scene.
    {
        GCmd->ClearRenderTargetView(DepthRtv, DepthFar, 0, nullptr);
        const D3D12_RECT Strip = { NearFrom, 0, GSceneW, GSceneH };
        GCmd->ClearRenderTargetView(DepthRtv, DepthNear, 1, &Strip);
        Barrier(Depth, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!Run())
            return 1;
    }

    // The weapon lamps' maps, for the beams' scene: three slices as FShadowMaps::RenderSpots leaves them - nothing in
    // the way, and in the first a wall BeamWall units from its lamp across the middle of its view, which takes the
    // whole of the beam's hot part.
    const FLOAT BeamWall = 250.0f;
    ID3D12Resource*       OwnMaps = nullptr;
    ID3D12DescriptorHeap* OwnDsvs = nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC OwnView;
    memset(&OwnView, 0, sizeof(OwnView));
    {
        D3D12_RESOURCE_DESC Md;
        memset(&Md, 0, sizeof(Md));
        Md.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Md.Width            = 256;
        Md.Height           = 256;
        Md.DepthOrArraySize = 3;
        Md.MipLevels        = 1;
        Md.Format           = DXGI_FORMAT_R32_TYPELESS;
        Md.SampleDesc.Count = 1;
        Md.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE Mc;
        memset(&Mc, 0, sizeof(Mc));
        Mc.Format = DXGI_FORMAT_D32_FLOAT;
        D3D12_DESCRIPTOR_HEAP_DESC Dd;
        memset(&Dd, 0, sizeof(Dd));
        Dd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        Dd.NumDescriptors = 3;
        if (FAILED(GDevice->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Md, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                    &Mc, __uuidof(ID3D12Resource), (void**)&OwnMaps)) ||
            FAILED(GDevice->CreateDescriptorHeap(&Dd, __uuidof(ID3D12DescriptorHeap), (void**)&OwnDsvs)))
        {
            printf("the weapon lamps' maps could not be made\n");
            return 2;
        }
        const UINT DsvStep = GDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        for (UINT s = 0; s < 3; s++)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC Dv;
            memset(&Dv, 0, sizeof(Dv));
            Dv.Format                         = DXGI_FORMAT_D32_FLOAT;
            Dv.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            Dv.Texture2DArray.FirstArraySlice = s;
            Dv.Texture2DArray.ArraySize       = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE At = OwnDsvs->GetCPUDescriptorHandleForHeapStart();
            At.ptr += (SIZE_T)s * DsvStep;
            GDevice->CreateDepthStencilView(OwnMaps, &Dv, At);
            GCmd->ClearDepthStencilView(At, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            if (s == 0)
            {
                const D3D12_RECT Wall = { 64, 64, 192, 192 };
                GCmd->ClearDepthStencilView(At, D3D12_CLEAR_FLAG_DEPTH, RdxMath::OwnLampNear / BeamWall, 0, 1, &Wall);
            }
        }
        Barrier(OwnMaps, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!Run())
            return 1;
        OwnView.Format                   = DXGI_FORMAT_R32_FLOAT;
        OwnView.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        OwnView.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        OwnView.Texture2DArray.MipLevels = 1;
        OwnView.Texture2DArray.ArraySize = 3;
    }

    // The pass itself, as the backend brings it up.
    static FVolumetrics Volume;
    const bool bUp = Volume.Init(GDevice) && Volume.InitComposite(GDevice, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                     Volume.EnsureTargets(GDevice, GSceneW, GSceneH);
    Messages();
    printf("the pass: %s; its target %i x %i\n", bUp ? "up" : "NOT UP", Volume.TargetW(), Volume.TargetH());
    if (!bUp)
        return 1;

    FVolumetrics::FWalk W;
    memset(&W, 0, sizeof(W));
    W.Screen[0]      = 0.5f * (FLOAT)GSceneW;
    W.Screen[1]      = 0.5f * (FLOAT)GSceneH;
    W.Focal          = GFocal;
    W.EngPerPixel[0] = (FLOAT)GSceneW / (FLOAT)Volume.TargetW();
    W.EngPerPixel[1] = (FLOAT)GSceneH / (FLOAT)Volume.TargetH();
    W.Time           = GTime;
    W.AxisX[0] = 1.0f;                      // the view's right is the world's +X
    W.AxisY[2] = -1.0f;                     // down the screen is the world's -Z
    W.AxisZ[1] = 1.0f;                      // into the picture is the world's +Y
    // A lamp up and to the right, in front: what the smoke is lit by.
    W.LampView[0] = 150.0f; W.LampView[1] = -200.0f; W.LampView[2] = 150.0f;
    W.LampReach   = 700.0f;
    W.LampHue[0]  = 1.0f; W.LampHue[1] = 0.95f; W.LampHue[2] = 0.85f;
    W.LampLit     = 1.0f;
    W.Grain       = RdxMath::CloudGrainWorld;
    W.Ambient     = RdxMath::CloudAmbient;
    W.Rise        = RdxMath::CloudRiseWorld;
    W.Warp        = RdxMath::CloudWarpWorld;
    W.SelfShade   = RdxMath::CloudSelfShade;
    W.StepWorld   = RdxMath::CloudStepWorld;
    W.Steps       = (INT)RdxMath::CloudSteps;
    W.FallDensity = RdxMath::WaterfallVolumeDensity;
    W.FallAmbient = RdxMath::WaterfallAmbient;
    W.FallRate    = RdxMath::WaterfallStreakRate;
    W.FallReactive  = RdxMath::WaterfallReactive;
    W.SmokeReactive = RdxMath::CloudReactive;
    W.FireGlow    = RdxMath::FireGlow;
    W.FireRate    = RdxMath::FireRise;
    // The level's units a pixel of the walk's target covers at a depth of one (FBackend::DrawVolumetrics).
    const FLOAT UnitsAPixel = W.EngPerPixel[0] / GFocal;

    const INT TilesX = (Volume.TargetW() + RdxMath::CloudTile - 1) / RdxMath::CloudTile;
    const INT TilesY = (Volume.TargetH() + RdxMath::CloudTile - 1) / RdxMath::CloudTile;
    static INT GRects[MAX_PUFFS * 4];
    UINT*      Lists     = nullptr;
    INT        ListsHeld = 0;

    FLOAT* const Picture   = (FLOAT*)malloc((size_t)GSceneW * GSceneH * 4 * sizeof(FLOAT));
    FLOAT* const Reference = (FLOAT*)malloc((size_t)GSceneW * GSceneH * 4 * sizeof(FLOAT));
    enum { SCENES = 19 };
    static const char* const Names[SCENES] = { "empty", "smoke-sizes", "smoke-incense", "smoke-vent", "smoke-many",
                                               "fire-candles", "fire-big", "fire-burning-body", "fire-many",
                                               "fire-long", "fire-in-line", "falls-and-all", "fire-sparks",
                                               "fire-haze", "fire-burning-room", "glow-sparks", "glow-wall",
                                               "beam-lamps", "beam-into" };
    // A fireplace as the levels have it: two sheets 48.7 wide and 32 high crossing at 19 degrees.
    const FLOAT Lean = 0.1658f;
    int Failed = 0;
    for (INT Which = 0; Which < SCENES && Picture; Which++)
    {
        if (GOnly)
        {
            wchar_t Name[64];
            _snwprintf(Name, 63, L"%hs", Names[Which]);
            Name[63] = 0;
            if (_wcsicmp(Name, GOnly) != 0)
                continue;
        }
        ResetScene();
        // The weapon lamps' beams: the beams' scene's alone.
        W.Own         = nullptr;
        W.OwnCount    = 0;
        W.OwnMaps     = nullptr;
        W.OwnMapsView = nullptr;
        W.bSparks = (Which == 12) ? 1 : 0;      // the last scene: the big fires again, throwing sparks
        // Marks: places on the picture (x, y in view space at depth z) where the scene says something should show.
        FLOAT Marks[16][3];
        INT   MarkCount = 0;
        bool  bWantSmoke = false, bWantLight = false;
        INT   WantBodies = -1;                      // how many flame bodies the scene's pieces should make
        INT   WantColumns = 0;                      // and how many columns of water its sheets
        auto Mark = [&](FLOAT X, FLOAT Y, FLOAT Z)
        {
            if (MarkCount < 16)
            {
                Marks[MarkCount][0] = X;
                Marks[MarkCount][1] = Y;
                Marks[MarkCount][2] = Z;
                MarkCount++;
            }
        };
        switch (Which)
        {
        case 0:
            break;
        case 1:
        {
            // One puff of each size the game makes, side by side at 300 units: from where a bullet struck to a vent's
            // steam. Each has to show - none thin away because it is small.
            const FLOAT Radii[5] = { 1.6f, 5.0f, 13.0f, 35.0f, 90.0f };
            const FLOAT At[5]    = { -210.0f, -190.0f, -150.0f, -70.0f, 100.0f };
            for (INT i = 0; i < 5; i++)
            {
                AddPuff(At[i], 0.0f, 300.0f, Radii[i], Radii[i], 1.0f, 0.75f);
                Mark(At[i], 0.0f, 300.0f);
            }
            bWantSmoke = true;
            break;
        }
        case 2:
            // An incense burner's plume: cards 13 units across growing to 35, 150 units off; and a small one, a
            // quarter of that, beside it.
            AddPlume(-30.0f, 40.0f, 150.0f, 40, 110.0f, 12.8f, 35.0f, 7u);
            AddPlume(40.0f, 40.0f, 150.0f, 40, 60.0f, 3.2f, 9.0f, 11u);
            Mark(-30.0f, 10.0f, 150.0f);
            Mark(40.0f, 25.0f, 150.0f);
            bWantSmoke = true;
            break;
        case 3:
            // A vent's steam: cards up to 88 units across, 400 units off, part of it behind the near wall.
            AddPlume(0.0f, 120.0f, 400.0f, 30, 260.0f, 30.0f, 88.0f, 3u);
            AddPlume(300.0f, 120.0f, 400.0f, 30, 260.0f, 30.0f, 88.0f, 5u);
            Mark(0.0f, 40.0f, 400.0f);
            bWantSmoke = true;
            break;
        case 4:
        {
            // More puffs than any list began with: three thousand small ones through a room.
            UINT Seed = 99u;
            auto Next = [&Seed]() -> FLOAT
            {
                Seed = Seed * 1664525u + 1013904223u;
                return (FLOAT)((Seed >> 8) & 0xFFFF) / 65535.0f;
            };
            for (INT i = 0; i < 3000; i++)
            {
                const FLOAT Z = 80.0f + Next() * 450.0f;
                AddPuff((Next() - 0.5f) * 1.2f * Z, (Next() - 0.5f) * 0.7f * Z, Z, 3.0f + Next() * 9.0f,
                        3.0f + Next() * 9.0f, 0.3f + 0.7f * Next(), 0.7f);
            }
            bWantSmoke = true;
            break;
        }
        case 5:
        {
            // Candle flames - a sheet two units by three - nearer and further off, and one lick of a burning body
            // beside the nearest for its size and brightness.
            const FLOAT Depths[6] = { 60.0f, 120.0f, 200.0f, 320.0f, 440.0f, 700.0f };
            for (INT i = 0; i < 6; i++)
            {
                const FLOAT X = (-0.42f + 0.14f * (FLOAT)i) * Depths[i];
                AddCandle(X, Depths[i], -1.5f, 2.0f, 3.0f);
                Mark(X, 0.3f, Depths[i]);
            }
            AddCardFlame(RdxMath::Flame_Lick, 25.0f, 60.0f, -8.0f, 11.2f, 22.4f);
            Mark(25.0f, 12.0f, 60.0f);
            bWantLight = true;
            WantBodies = 7;
            break;
        }
        case 6:
        case 12:
        case 13:
        {
            // A fireplace's flame, a torch's - two sheets 20 wide and 39 high crossing at right angles - and a
            // burning barrel's, two crossing planes of two triangles each.
            const FSheet Hearth[2] = { { -60.0f, 160.0f, -40.0f, 48.7f, 32.0f, -0.5f * Lean },
                                       { -60.0f, 160.0f, -40.0f, 48.7f, 32.0f, 0.5f * Lean } };
            const FSheet Torch[2]  = { { 0.0f, 160.0f, -40.0f, 20.0f, 39.0f, 0.0f },
                                       { 0.0f, 160.0f, -40.0f, 20.0f, 39.0f, 1.5707963f } };
            AddLevelFire(Hearth, 2);
            AddLevelFire(Torch, 2);
            for (INT i = 0; i < 2; i++)
            {
                AddMeshFlame(46.0f, 160.0f, -40.0f, 74.0f, 160.0f, -10.0f);
                AddMeshFlame(60.0f, 146.0f, -40.0f, 60.0f, 174.0f, -10.0f);
            }
            Mark(-60.0f, 30.0f, 160.0f);
            Mark(0.0f, 25.0f, 160.0f);
            Mark(60.0f, 30.0f, 160.0f);
            bWantLight = true;
            WantBodies = 3;
            break;
        }
        case 7:
        {
            // A burning body: a fireball and a lick at each of eight points up a figure ninety units tall.
            for (INT i = 0; i < 8; i++)
            {
                const FLOAT Z = -45.0f + 11.0f * (FLOAT)i;
                const FLOAT X = ((i & 1) ? 6.0f : -6.0f);
                AddCardFlame(RdxMath::Flame_Ball, X, 220.0f, Z, 32.0f, 32.0f);
                AddCardFlame(RdxMath::Flame_Lick, X + 4.0f, 222.0f, Z + 6.0f, 11.2f, 22.4f);
            }
            Mark(0.0f, 0.0f, 220.0f);
            bWantLight = true;
            WantBodies = 16;
            break;
        }
        case 8:
            // More candles than the walk's list of fires begins with: a chapel's, ten rows of sixteen, the far rows
            // offered first as the engine sends them. Every one is walked - the list is made longer.
            for (INT Row = 9; Row >= 0; Row--)
                for (INT i = 0; i < 16; i++)
                    AddCandle(-90.0f + 12.0f * (FLOAT)i, 120.0f + 14.0f * (FLOAT)Row, -30.0f + 3.0f * (FLOAT)Row, 2.0f,
                              3.0f);
            Mark(-6.0f, 1.5f, 246.0f);
            bWantLight = true;
            WantBodies = 160;
            break;
        case 9:
        {
            // A wreck's long blaze as a level has it: three sheets 260 wide and 200 high end to end, a fourth across
            // their end - one fire, one body, as long as all of them.
            const FSheet Blaze[4] = { { -144.0f, 340.0f, -150.0f, 260.0f, 200.0f, 0.0f },
                                      { 0.0f, 348.0f, -150.0f, 260.0f, 152.0f, 0.0f },
                                      { 144.0f, 364.0f, -150.0f, 260.0f, 200.0f, 0.0f },
                                      { -238.0f, 430.0f, -128.0f, 198.6f, 152.0f, 1.4084f } };
            AddLevelFire(Blaze, 4);
            Mark(0.0f, 60.0f, 348.0f);
            bWantLight = true;
            WantBodies = 1;
            break;
        }
        case 10:
        {
            // Flames one behind another along the eye's ray: eight candles in a row straight away from the eye, and
            // a candle in front of a fireplace - and, for what each is on its own, the same candle and the same
            // fireplace beside them. A ray that meets two flames has to show each as it shows alone.
            for (INT i = 0; i < 8; i++)
                AddCandle(0.0f, 80.0f + 20.0f * (FLOAT)i, -1.5f, 2.0f, 3.0f);
            const FSheet Behind[2] = { { -80.0f, 160.0f, -40.0f, 48.7f, 32.0f, -0.5f * Lean },
                                       { -80.0f, 160.0f, -40.0f, 48.7f, 32.0f, 0.5f * Lean } };
            const FSheet Alone[2]  = { { 80.0f, 160.0f, -40.0f, 48.7f, 32.0f, -0.5f * Lean },
                                       { 80.0f, 160.0f, -40.0f, 48.7f, 32.0f, 0.5f * Lean } };
            AddLevelFire(Behind, 2);
            AddLevelFire(Alone, 2);
            AddCandle(-50.0f, 100.0f, -16.5f, 2.0f, 3.0f);
            AddCandle(30.0f, 100.0f, 13.5f, 2.0f, 3.0f);
            Mark(0.0f, 0.0f, 80.0f);
            Mark(-50.0f, 15.0f, 100.0f);
            Mark(30.0f, -15.0f, 100.0f);
            bWantLight = true;
            WantBodies = 12;
            break;
        }
        case 14:
        {
            // A room of burning things, as the game lights them: each wears eight fireballs and some hundred and
            // fifty licks, each a flame of its own - far more bodies than the list of fires begins with, and more
            // than twice that, so the list grows past its doubling. Nine of them, three rows of three.
            UINT Seed = 7u;
            auto Next = [&Seed]() -> FLOAT
            {
                Seed = Seed * 1664525u + 1013904223u;
                return (FLOAT)((Seed >> 8) & 0xFFFF) / 65535.0f;
            };
            for (INT Thing = 0; Thing < 9; Thing++)
            {
                const FLOAT X = -160.0f + 160.0f * (FLOAT)(Thing % 3);
                const FLOAT Y = 260.0f + 120.0f * (FLOAT)(Thing / 3);
                for (INT i = 0; i < 8; i++)
                    AddCardFlame(RdxMath::Flame_Ball, X + (Next() - 0.5f) * 30.0f, Y + (Next() - 0.5f) * 30.0f,
                                 -40.0f + Next() * 60.0f, 40.0f, 40.0f);
                for (INT i = 0; i < 150; i++)
                    AddCardFlame(RdxMath::Flame_Lick, X + (Next() - 0.5f) * 44.0f, Y + (Next() - 0.5f) * 44.0f,
                                 -45.0f + Next() * 90.0f, 11.2f, 22.4f);
            }
            Mark(0.0f, 10.0f, 380.0f);
            bWantLight = true;
            WantBodies = 9 * 158;
            break;
        }
        case 15:
        {
            // The glows that are no flame: one ball of each size the electric sparks' cards come in, side by side,
            // and a burst of them as the game throws it against a wall - a hundred and fifty at once, each a ball of
            // its own, of the cards' own colour.
            const FLOAT Blue[3]  = { 0.55f, 0.72f, 1.0f };
            const FLOAT Sizes[4] = { 6.0f, 12.0f, 24.0f, 32.0f };
            for (INT i = 0; i < 4; i++)
            {
                AddGlowCard(-150.0f + 40.0f * (FLOAT)i, 200.0f, 30.0f, Sizes[i], 1.0f, Blue);
                Mark(-150.0f + 40.0f * (FLOAT)i, -30.0f, 200.0f);
            }
            UINT Seed = 11u;
            auto Next = [&Seed]() -> FLOAT
            {
                Seed = Seed * 1664525u + 1013904223u;
                return (FLOAT)((Seed >> 8) & 0xFFFF) / 65535.0f;
            };
            for (INT i = 0; i < 150; i++)
            {
                const FLOAT X = 40.0f + Next() * 130.0f, Y = 170.0f + Next() * 40.0f, Z = -50.0f + Next() * 70.0f;
                // One after the other, said so: in one list of arguments their order is the compiler's to choose.
                const FLOAT Fade = 0.4f + 0.6f * Next();
                const FLOAT Wide = 6.0f + Next() * 26.0f;
                AddGlowCard(X, Y, Z, Wide, Fade, Blue);
            }
            // And a bolt of plasma in flight, as its mesh's polygons come: four boxes end to end and across.
            {
                const FLOAT Green[3] = { 0.35f, 1.0f, 0.45f };
                const FLOAT Box[4][6] = { { -120.0f, 218.0f, -52.0f, -98.0f, 222.0f, -44.0f },
                                          { -100.0f, 218.0f, -52.0f, -76.0f, 222.0f, -44.0f },
                                          { -120.0f, 216.0f, -50.0f, -98.0f, 224.0f, -46.0f },
                                          { -100.0f, 216.0f, -50.0f, -76.0f, 224.0f, -46.0f } };
                for (INT i = 0; i < 4; i++)
                    if (!HoldFlames(GFlames.Count + 1) ||
                        !GFlames.Add(RdxMath::FlameGlowMeshPiece(Box[i][0], Box[i][1], Box[i][2], Box[i][3],
                                                                 Box[i][4], Box[i][5], 220.0f, Green)))
                        GFlamesRefused++;
                Mark(-98.0f, 48.0f, 220.0f);
            }
            Mark(105.0f, 15.0f, 190.0f);
            bWantLight = true;
            // The four alone, the bolt, and the burst less the four of it that lie all but inside others and are
            // those (FFlameGather::Add).
            WantBodies = 151;
            break;
        }
        case 16:
        {
            // Balls of glow at a wall, as a bolt's splash leaves them: a wall that runs away to the right, ribbed
            // as a relief's is, and balls with their middles on it, behind it and before it. The scene's depth is
            // its own: the scenes after it put the common one back.
            const INT   X0 = GSceneW / 8, X1 = GSceneW - GSceneW / 4;
            auto WallAt = [&](INT x, FLOAT Ribs) -> FLOAT
            {
                const FLOAT Across = (FLOAT)(x - X0) / (FLOAT)(X1 - X0);
                return 70.0f + 110.0f * Across - Ribs * (0.5f + 0.5f * cosf((FLOAT)x * (6.2831853f / 56.0f)));
            };
            Barrier(Depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            for (INT x = X0; x < X1; x++)
            {
                const FLOAT      Is[4] = { 1.0f / WallAt(x, 2.0f), 0.0f, 0.0f, 0.0f };
                const D3D12_RECT Col   = { x, 0, x + 1, GSceneH };
                GCmd->ClearRenderTargetView(DepthRtv, Is, 1, &Col);
            }
            Barrier(Depth, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const FLOAT Violet[3] = { 0.62f, 0.45f, 1.0f };
            // Each ball: where across the wall, how high, its card's width, and how far behind the wall its middle
            // lies in its own half-widths - on it, half sunk in it, standing before it.
            const FLOAT Balls[6][4] = { { 0.12f, 14.0f, 24.0f, 0.0f },  { 0.28f, -16.0f, 32.0f, 0.0f },
                                        { 0.44f, 18.0f, 24.0f, 0.5f },  { 0.58f, -16.0f, 12.0f, 0.0f },
                                        { 0.72f, 20.0f, 24.0f, -0.8f }, { 0.88f, -20.0f, 32.0f, 0.5f } };
            for (INT i = 0; i < 6; i++)
            {
                const INT   Px   = X0 + (INT)(Balls[i][0] * (FLOAT)(X1 - X0));
                const FLOAT Half = 0.7f * 0.8f * Balls[i][2];
                const FLOAT Off  = WallAt(Px, 0.0f) - 1.0f + Balls[i][3] * Half;
                const FLOAT X    = ((FLOAT)Px - 0.5f * (FLOAT)GSceneW) * Off / GFocal;
                AddGlowCard(X, Off, Balls[i][1], Balls[i][2], 1.0f, Violet);
                Mark(X, -Balls[i][1], Off);
            }
            bWantLight = true;
            WantBodies = 6;
            break;
        }
        case 17:
        case 18:
        {
            // The weapon lamps, at the common depth again: the glow wall's scene painted its own into it. Here a
            // lamp across the view in front, its beam cut off by the wall in its map (the first slice) BeamWall units
            // on, and the flashlight's own lamp at the eye, which throws no beam: were it walked, the middle of the
            // picture would be its beam. In the next scene the flashlight's lamp first, and then a lamp further off
            // with nothing in its way, turned towards the eye - whose light then fills the air all round the eye, as
            // such a lamp's does.
            Barrier(Depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            GCmd->ClearRenderTargetView(DepthRtv, DepthFar, 0, nullptr);
            const D3D12_RECT Strip = { NearFrom, 0, GSceneW, GSceneH };
            GCmd->ClearRenderTargetView(DepthRtv, DepthNear, 1, &Strip);
            Barrier(Depth, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            static FLOAT Rows[FVolumetrics::OWN_FLOATS];
            memset(Rows, 0, sizeof(Rows));
            const RdxMath::FVec3 AtA = { -300.0f, 400.0f, 0.0f }, AimA = { 1.0f, 0.0f, 0.0f };
            const RdxMath::FVec3 AtE = { 0.0f, 0.0f, 0.0f }, AimE = { 0.0f, 1.0f, 0.0f };
            const RdxMath::FVec3 AtB = { 150.0f, 560.0f, -40.0f };
            const FLOAT          Lb  = sqrtf(AtB.X * AtB.X + AtB.Y * AtB.Y + AtB.Z * AtB.Z);
            const RdxMath::FVec3 AimB = { -AtB.X / Lb, -AtB.Y / Lb, -AtB.Z / Lb };
            const bool           bHere   = Which == 17;
            const RdxMath::FVec3 Ats[2]  = { bHere ? AtA : AtE, bHere ? AtE : AtB };
            const RdxMath::FVec3 Aims[2] = { bHere ? AimA : AimE, bHere ? AimE : AimB };
            const INT            Eye     = bHere ? 1 : 0;
            RdxMath::OwnLampRowsHead(Rows, 2);
            for (INT i = 0; i < 2; i++)
            {
                float M[16];
                RdxMath::SpotMatrix(Ats[i], Aims[i], RdxMath::OwnLampMapTan, RdxMath::OwnLampNear, M);
                RdxMath::OwnLampRowsLamp(Rows, i, Ats[i], Aims[i], RdxMath::WeaponLampLight, i == Eye, M);
            }
            W.Own         = Rows;
            W.OwnCount    = 2;
            W.OwnMaps     = OwnMaps;
            W.OwnMapsView = &OwnView;
            W.OwnMapsMade = 1;
            W.OwnDust     = RdxMath::OwnBeamDust;
            W.OwnForward  = RdxMath::OwnBeamForward;
            // In view space - x the world's X, y down its Z, z into the picture along its Y. Here: across the near
            // beam 150 units from its lamp, before the wall; 350, past it; up and to the left, in the eye's own cone
            // and in none of the other's. In the next: the lamp itself; and forty-five degrees off it.
            if (bHere)
            {
                Mark(AtA.X + 150.0f, 0.0f, AtA.Y);
                Mark(AtA.X + 350.0f, 0.0f, AtA.Y);
                Mark(-100.0f, -150.0f, 400.0f);
            }
            else
            {
                Mark(AtB.X, -AtB.Z, AtB.Y);
                Mark(AtB.X - 400.0f, 0.0f, 400.0f);
            }
            break;
        }
        default:
        {
            // Everything at once: smoke in front of a fire, a waterfall's column, candles in the smoke.
            const FSheet Hearth[2] = { { 0.0f, 260.0f, -60.0f, 48.7f, 40.0f, -0.5f * Lean },
                                       { 0.0f, 260.0f, -60.0f, 48.7f, 40.0f, 0.5f * Lean } };
            // The canal's fall: four sheets 41, 42, 64 and 45 units wide crossing round one spot.
            const FFallOf  Canal[4]  = { { -149.0f, 301.3f, 41.0f, 1.5708f }, { -134.0f, 294.3f, 42.0f, 0.7679f },
                                         { -151.0f, 309.3f, 64.0f, 0.0f }, { -166.0f, 295.3f, 45.0f, 2.4435f } };
            AddLevelFire(Hearth, 2);
            AddPlume(0.0f, 0.0f, 250.0f, 40, 160.0f, 20.0f, 60.0f, 21u);
            AddLevelFall(Canal, 4, -120.0f, 140.0f);
            WantColumns = 1;
            for (INT i = 0; i < 12; i++)
                AddCandle(60.0f + 6.0f * (FLOAT)i, 180.0f, -20.0f, 2.0f, 3.0f);
            Mark(0.0f, 50.0f, 260.0f);
            bWantSmoke = bWantLight = true;
            WantBodies = 13;
            break;
        }
        }
        // The flames' bodies and the falls' columns, as the backend makes them of the frame's pieces
        // (FBackend::DrawVolumetrics).
        GFireCount = RdxMath::FlameBodiesMake(GFlames.Pieces, GFlames.Count, GFires, GFlames.Count, UnitsAPixel,
                                              GFireGroups);
        GFallCount = RdxMath::FallColumnsMake(GFallSheets.Pieces, GFallSheets.Count, GFalls, FVolumetrics::MAX_FALLS);

        // The puffs' lists by tile, as the backend builds them (FBackend::BuildCloudTiles).
        INT Numbers = 0;
        if (GPuffCount > 0)
        {
            for (INT i = 0; i < GPuffCount; i++)
            {
                FLOAT* const P = GPuffs + i * PUFF_FLOATS;
                INT* const   R = GRects + i * 4;
                P[8] *= RdxMath::VolumeWiden(P[3], P[4],
                                             0.5f * RdxMath::CloudPuffMinPixels * UnitsAPixel * Max(P[2], 1.0f));
                if (!RdxMath::CloudPuffTiles(P, Max(P[3], P[4]) + W.SelfShade, W.Focal, W.Screen, W.EngPerPixel, TilesX,
                                             TilesY, R[0], R[1], R[2], R[3]))
                {
                    R[0] = R[1] = 0;
                    R[2] = R[3] = -1;
                }
            }
            Numbers = RdxMath::CloudTileListsSize(GRects, GPuffCount, TilesX, TilesY);
            if (Numbers > ListsHeld)
            {
                free(Lists);
                Lists     = (UINT*)malloc((size_t)Numbers * sizeof(UINT));
                ListsHeld = Lists ? Numbers : 0;
            }
            if (!Lists)
                return 2;
            RdxMath::CloudTileListsFill(GRects, GPuffCount, TilesX, TilesY, Lists);
        }
        // The bodies' lists by tile, as the backend builds them (FBackend::BuildFireTiles); and the same with every
        // body in every tile's list - one list, which every tile's two numbers name.
        INT FireNumbers = 0, FireNumbersAll = 0, FireListMost = 0;
        if (GFireCount > 0)
        {
            for (INT i = 0; i < GFireCount; i++)
            {
                INT* const R = GFireRects + i * 4;
                if (!RdxMath::FlameBodyTiles(GFires + i * FVolumetrics::FIRE_FLOATS, W.bSparks != 0, W.EyeWorld,
                                             W.AxisX, W.AxisY, W.AxisZ, W.Focal, W.Screen, W.EngPerPixel, TilesX,
                                             TilesY, R[0], R[1], R[2], R[3]))
                {
                    R[0] = R[1] = 0;
                    R[2] = R[3] = -1;
                }
            }
            FireNumbers    = RdxMath::CloudTileListsSize(GFireRects, GFireCount, TilesX, TilesY);
            FireNumbersAll = TilesX * TilesY * 2 + GFireCount;
            if (FireNumbers > GFireListsHeld)
            {
                free(GFireLists);
                GFireLists     = (UINT*)malloc((size_t)FireNumbers * sizeof(UINT));
                GFireListsHeld = GFireLists ? FireNumbers : 0;
            }
            if (FireNumbersAll > GFireAllHeld)
            {
                free(GFireListsAll);
                GFireListsAll = (UINT*)malloc((size_t)FireNumbersAll * sizeof(UINT));
                GFireAllHeld  = GFireListsAll ? FireNumbersAll : 0;
            }
            if (!GFireLists || !GFireListsAll)
                return 2;
            FireListMost = RdxMath::CloudTileListsFill(GFireRects, GFireCount, TilesX, TilesY, GFireLists);
            for (INT Tile = 0; Tile < TilesX * TilesY; Tile++)
            {
                GFireListsAll[Tile * 2]     = (UINT)(TilesX * TilesY * 2);
                GFireListsAll[Tile * 2 + 1] = (UINT)GFireCount;
            }
            for (INT i = 0; i < GFireCount; i++)
                GFireListsAll[TilesX * TilesY * 2 + i] = (UINT)i;
        }
        W.Fires         = GFires;
        W.FireCount     = GFireCount;
        W.FireTiles     = GFireLists;
        W.FireTileUints = FireNumbers;
        W.Falls         = GFalls;
        W.FallCount     = GFallCount;

        printf("%s: %i puffs (%i numbers of lists), %i pieces of flame in %i bodies (%i flames found no room; the "
               "longest list of a tile %i bodies), %i sheets of falling water in %i columns\n", Names[Which],
               GPuffCount, Numbers, GFlames.Count, GFireCount, GFlamesRefused, FireListMost, GFallSheets.Count,
               GFallCount);
        if (GFallCount > 0)
            printf("    the first column at %.1f %.1f, %.1f by %.1f across\n", GFalls[0], GFalls[1], 2.0f * GFalls[6],
                   2.0f * GFalls[7]);
        // One frame of the walk over the scene, read back into Into: the picture cleared, the walk recorded and laid
        // on it as the backend lays it, and in its own scene the heat haze after that. False where the frame could
        // not be run or read.
        UBOOL bWalked = 1;
        DWORD Took    = 0;
        // Which views a shot walks. 0: the world's alone, as every scene's own picture is taken. 1: the world's, laid
        // on the picture, and after it in the same frame a further view's by Further's numbers, laid on nothing. 2: a
        // further view's alone by Further's numbers, laid on the picture.
        INT                 ShootViews = 0;
        FVolumetrics::FWalk Further    = W;
        auto Shoot = [&](FLOAT* Into) -> bool
        {
            GCmd->ClearRenderTargetView(SceneRtv, Back, 0, nullptr);
            if (Which == 13)
            {
                // A wall with lines on it, for the trembling air to bend: without them a bend moves nothing one can
                // see.
                static D3D12_RECT Lines[96];
                INT LineCount = 0;
                for (INT x = 20; x < GSceneW && LineCount < 64; x += 40)
                {
                    const D3D12_RECT L = { x, 0, x + 3, GSceneH };
                    Lines[LineCount++] = L;
                }
                for (INT y = 20; y < GSceneH && LineCount < 96; y += 40)
                {
                    const D3D12_RECT L = { 0, y, GSceneW, y + 3 };
                    Lines[LineCount++] = L;
                }
                const FLOAT Pale[4] = { 0.5f, 0.55f, 0.6f, 1.0f };
                GCmd->ClearRenderTargetView(SceneRtv, Pale, (UINT)LineCount, Lines);
            }
            if (ShootViews == 2)
                bWalked = Volume.Record(GCmd, GDevice, Depth, DXGI_FORMAT_R32_FLOAT, GPuffs, GPuffCount, Lists,
                                        Numbers, TilesX, Further, Which % 3, nullptr, nullptr, nullptr, 1);
            else
                bWalked = Volume.Record(GCmd, GDevice, Depth, DXGI_FORMAT_R32_FLOAT, GPuffs, GPuffCount, Lists,
                                        Numbers, TilesX, W, Which % 3, nullptr, nullptr, nullptr);
            // Laid over the picture as the backend lays it (FBackend::DrawVolumetrics) - where the walk wrote
            // anything.
            if (Volume.Wrote())
            {
                Barrier(Volume.Target(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                GCmd->OMSetRenderTargets(1, &SceneRtv, FALSE, nullptr);
                const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)GSceneW, (FLOAT)GSceneH, 0.0f, 1.0f };
                const D3D12_RECT     Sc = { 0, 0, GSceneW, GSceneH };
                GCmd->RSSetViewports(1, &Vp);
                GCmd->RSSetScissorRects(1, &Sc);
                Volume.RecordComposite(GCmd);
                Barrier(Volume.Target(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            // A further view's walk recorded after the world's: what it puts into its lists and its numbers while
            // the frame is recorded must not be what the world's walk reads when the frame runs.
            if (ShootViews == 1)
                Volume.Record(GCmd, GDevice, Depth, DXGI_FORMAT_R32_FLOAT, GPuffs, GPuffCount, Lists, Numbers, TilesX,
                              Further, Which % 3, nullptr, nullptr, nullptr, 2);
            if (Which == 13 && ShootViews == 0)
            {
                // The heat haze as the backend records it: after the composite, the depth readable by a pixel
                // shader.
                Barrier(Depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                const D3D12_RECT Whole = { 0, 0, GSceneW, GSceneH };
                Volume.RecordHaze(GCmd, GDevice, SceneTex, DXGI_FORMAT_R16G16B16A16_FLOAT, SceneRtv, Whole, W);
                Barrier(Depth,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            Barrier(SceneTex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            {
                D3D12_TEXTURE_COPY_LOCATION Src, Dst;
                memset(&Src, 0, sizeof(Src));
                memset(&Dst, 0, sizeof(Dst));
                Src.pResource       = SceneTex;
                Src.Type            = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                Dst.pResource       = Read;
                Dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                Dst.PlacedFootprint = Foot;
                GCmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
            }
            Barrier(SceneTex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const DWORD T0   = GetTickCount();
            const bool  bRan = Run();
            Took = GetTickCount() - T0;
            Messages();
            if (!bRan)
                return false;
            void* Mapped = nullptr;
            const D3D12_RANGE All = { 0, (SIZE_T)ReadBytes };
            if (FAILED(Read->Map(0, &All, &Mapped)))
            {
                printf("    the picture could not be read back\n");
                return false;
            }
            for (INT y = 0; y < GSceneH; y++)
            {
                const unsigned short* const Row = (const unsigned short*)((const BYTE*)Mapped + Foot.Offset +
                                                                          (size_t)y * Foot.Footprint.RowPitch);
                for (INT x = 0; x < GSceneW * 4; x++)
                    Into[(size_t)y * GSceneW * 4 + x] = HalfToFloat(Row[x]);
            }
            const D3D12_RANGE None = { 0, 0 };
            Read->Unmap(0, &None);
            return true;
        };
        // The lists by tile change nothing of the picture, only what it costs: the scene's flames walked with every
        // body in every tile's list - every pixel asking every flame - come out the same to the last number. A body
        // listed in fewer tiles than its flame shows in is cut off along the tiles' edges, and differs here.
        INT   Differ  = -1;
        DWORD TookAll = 0;
        if (GFireCount > 0 && !GFailWord && Reference)
        {
            W.FireTiles     = GFireListsAll;
            W.FireTileUints = FireNumbersAll;
            if (!Shoot(Reference))
            {
                Failed++;
                break;
            }
            TookAll         = Took;
            Differ          = 0;
            W.FireTiles     = GFireLists;
            W.FireTileUints = FireNumbers;
        }
        if (!Shoot(Picture))
        {
            Failed++;
            break;
        }
        if (Differ == 0)
            for (size_t v = 0; v < (size_t)GSceneW * GSceneH * 4; v++)
                Differ += (memcmp(Picture + v, Reference + v, sizeof(FLOAT)) != 0) ? 1 : 0;

        // What came back: how much of the picture the walk changed, the most it added, how much it covers, and
        // whether any of it is not a number.
        INT   Changed = 0, NotNumbers = 0;
        FLOAT MostAdded = 0.0f, MostCover = 0.0f;
        for (size_t At = 0; At < (size_t)GSceneW * GSceneH; At++)
        {
            const FLOAT* const P = Picture + At * 4;
            bool bChanged = false;
            for (INT k = 0; k < 4; k++)
            {
                if (P[k] != P[k] || P[k] > 1.0e30f || P[k] < -1.0e30f)
                    NotNumbers++;
                else if (k < 3)
                {
                    MostAdded = Max(MostAdded, P[k] - Back[k]);
                    bChanged  = bChanged || Abs(P[k] - Back[k]) > 0.004f;
                }
                else
                    MostCover = Max(MostCover, P[k]);
            }
            Changed += bChanged ? 1 : 0;
        }
        printf("    walked %s%s in %lu ms; %.1f%% of the picture changed; the most light added %.2f, the most cover "
               "%.2f; %i values not numbers\n", bWalked ? "all" : "WITHOUT ITS SMOKE",
               Volume.FiresHeld() ? "" : " WITHOUT ITS FIRES", (unsigned long)Took,
               100.0f * (FLOAT)Changed / (FLOAT)(GSceneW * GSceneH), MostAdded, MostCover, NotNumbers);
        if (Differ >= 0)
            printf("    every flame asked at every pixel: %lu ms, and %i numbers of the picture differ\n",
                   (unsigned long)TookAll, Differ);
        for (INT m = 0; m < MarkCount; m++)
        {
            const INT Px = (INT)(W.Screen[0] + GFocal * Marks[m][0] / Marks[m][2]);
            const INT Py = (INT)(W.Screen[1] + GFocal * Marks[m][1] / Marks[m][2]);
            if (Px < 0 || Py < 0 || Px >= GSceneW || Py >= GSceneH)
                continue;
            const FLOAT* const P = Picture + ((size_t)Py * GSceneW + Px) * 4;
            printf("    at %4i,%4i (%.0f units off): cover %.2f, light %.2f %.2f %.2f over the background's\n", Px, Py,
                   Marks[m][2], P[3], P[0] - Back[0] * (1.0f - P[3]), P[1] - Back[1] * (1.0f - P[3]),
                   P[2] - Back[2] * (1.0f - P[3]));
        }
        // What must hold whatever the look: numbers only; nothing where nothing was handed over or nothing could be
        // walked; smoke where smoke was, light where a flame was; and with a list refused (-fail), the walk says so
        // and the rest still runs.
        bool bGood = NotNumbers == 0;
        if (Which == 0 || !Volume.Wrote())
            bGood = bGood && Changed == 0;
        if (bWantSmoke && bWalked)
            bGood = bGood && MostCover > 0.15f;
        if (bWantLight && Volume.FiresHeld())
            bGood = bGood && MostAdded > 0.2f;
        // Every flame handed over is taken, and without -fail every one is walked: none finds no room.
        if (WantBodies >= 0)
            bGood = bGood && GFireCount == WantBodies && GFlamesRefused == 0;
        if (!GFailWord)
            bGood = bGood && Volume.FiresHeld();
        bGood = bGood && Differ <= 0;
        bGood = bGood && GFallCount == WantColumns;
        // The beams. Light across the beam before its wall, much less of it past the wall, and none in the eye's own
        // cone; looking into a lamp, much more than forty-five degrees off it.
        if (W.OwnCount > 0)
        {
            FLOAT Beam[3] = { 0.0f, 0.0f, 0.0f };
            for (INT m = 0; m < MarkCount && m < 3; m++)
            {
                const INT Px = Clamp((INT)(W.Screen[0] + GFocal * Marks[m][0] / Marks[m][2]), 0, GSceneW - 1);
                const INT Py = Clamp((INT)(W.Screen[1] + GFocal * Marks[m][1] / Marks[m][2]), 0, GSceneH - 1);
                const FLOAT* const P = Picture + ((size_t)Py * GSceneW + Px) * 4;
                Beam[m] = Max(P[0] - Back[0], Max(P[1] - Back[1], P[2] - Back[2]));
            }
            if (Which == 17)
            {
                printf("    the beam: %.3f across it before its wall, %.3f past the wall, %.3f in the eye's own cone\n",
                       Beam[0], Beam[1], Beam[2]);
                bGood = bGood && Beam[0] > 0.01f && Beam[1] < 0.5f * Beam[0] && Beam[2] < 0.004f;
            }
            else
            {
                printf("    the beam: %.3f looking into the lamp, %.3f forty-five degrees off it\n", Beam[0], Beam[1]);
                bGood = bGood && Beam[0] > 0.1f && Beam[0] > 3.0f * Beam[1];
            }
        }
        if (!bGood)
        {
            printf("    NOT AS IT SHOULD BE\n");
            Failed++;
        }
        wchar_t Path[MAX_PATH];
        _snwprintf(Path, MAX_PATH - 1, L"%ls\\%hs.bmp", OutDir, Names[Which]);
        Path[MAX_PATH - 1] = 0;
        if (!WriteBmp(Path, Picture, GSceneW, GSceneH))
            printf("    its picture could not be written\n");

        // A FURTHER VIEW OF THE SAME FRAME - a mirror's (FVolumetrics::Record's View). Three things must hold of
        // it whatever the scene; Reference takes its pictures. Not in the trembling air's scene, whose background is
        // no flat colour to hold a picture against.
        if ((GPuffCount > 0 || GFireCount > 0 || GFallCount > 0 || W.OwnCount > 0) && Volume.Wrote() && !GFailWord &&
            Reference && Which != 13)
        {
            auto Differs = [&](const FLOAT* A, const FLOAT* B, INT X0, INT X1) -> INT
            {
                INT Count = 0;
                for (INT y = 0; y < GSceneH; y++)
                    for (INT x = Max(X0, 0); x < Min(X1, GSceneW); x++)
                    {
                        const size_t At = ((size_t)y * GSceneW + x) * 4;
                        bool bOther = false;
                        for (INT k = 0; k < 3; k++)
                            bOther = bOther || !(Abs(A[At + k] - B[At + k]) <= 0.004f);
                        Count += bOther ? 1 : 0;
                    }
                return Count;
            };
            // Its lists and numbers are its own: recorded after the world's walk with other numbers, it leaves the
            // world's picture as it was to the last number.
            Further              = W;
            Further.Focal        = W.Focal * 2.0f;
            Further.NearPlane[2] = 1.0f;
            Further.NearPlane[3] = 50.0f;
            ShootViews           = 1;
            bool bShot = Shoot(Reference);
            INT  Own   = 0;
            for (size_t v = 0; bShot && v < (size_t)GSceneW * GSceneH * 4; v++)
                Own += (memcmp(Picture + v, Reference + v, sizeof(FLOAT)) != 0) ? 1 : 0;
            // A mirror's plane past everything: no walk begins before it, and the picture stays the background.
            INT Past = 0;
            if (bShot)
            {
                Further              = W;
                Further.NearPlane[2] = 1.0f;
                Further.NearPlane[3] = 1.0e5f;
                ShootViews           = 2;
                bShot = Shoot(Reference);
                for (size_t At = 0; bShot && At < (size_t)GSceneW * GSceneH; At++)
                    for (INT k = 0; k < 3; k++)
                        Past += !(Abs(Reference[At * 4 + k] - Back[k]) <= 0.004f) ? 1 : 0;
            }
            // A plane at the eye and the left half of the target for a window: the left half as the world's own
            // walk left it, the right half the background.
            INT Left = 0, Right = 0;
            if (bShot)
            {
                Further              = W;
                Further.NearPlane[2] = 1.0f;
                Further.NearPlane[3] = 0.001f;
                Further.Window[0]    = 0;
                Further.Window[1]    = 0;
                Further.Window[2]    = Volume.TargetW() / 2;
                Further.Window[3]    = Volume.TargetH();
                ShootViews           = 2;
                bShot = Shoot(Reference);
                const INT Edge = 2 * (Volume.TargetW() / 2);
                Left = bShot ? Differs(Reference, Picture, 0, Edge - 8) : 0;
                for (INT y = 0; bShot && y < GSceneH; y++)
                    for (INT x = Edge + 8; x < GSceneW; x++)
                        for (INT k = 0; k < 3; k++)
                            Right += !(Abs(Reference[((size_t)y * GSceneW + x) * 4 + k] - Back[k]) <= 0.004f) ? 1 : 0;
                wchar_t Half[MAX_PATH];
                _snwprintf(Half, MAX_PATH - 1, L"%ls\\%hs-further-view.bmp", OutDir, Names[Which]);
                Half[MAX_PATH - 1] = 0;
                if (bShot && !WriteBmp(Half, Reference, GSceneW, GSceneH))
                    printf("    the further view's picture could not be written\n");
            }
            ShootViews = 0;
            printf("    a further view of the frame: %i numbers of the world's picture differ after it; behind a plane "
                   "past everything %i numbers differ from the background; in a window of the left half %i pixels "
                   "differ from the world's walk there and %i numbers from the background right of it\n", Own, Past,
                   Left, Right);
            if (!bShot || Own || Past || Left || Right)
            {
                printf("    THE FURTHER VIEW NOT AS IT SHOULD BE\n");
                Failed++;
            }
        }
    }
    printf("\n%i scenes not as they should be, %i debug layer errors, %i refusals in the pass's own log\n", Failed,
           GErrors, GLogRefused);
    return (Failed || GErrors) ? 1 : 0;
}

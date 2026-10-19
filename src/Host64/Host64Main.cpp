// SecondRevisionHost64 - the 64-bit host (docs/terv-fsr4.md, Host64Protocol.h).
//
// Started by the renderer, one per game, in a job object that ends it with the game:
//     SecondRevisionHost64.exe <block handle> <parent pid>
// The block's file mapping is inherited (the renderer passes it in the new process's handle list); everything else - the
// fences, the wake event, the shared textures - the renderer duplicates into this process and writes into the block.
//
// What it does: takes the block, makes a device on the adapter the renderer's device runs on, opens the fences, says
// Ready, and then for every frame the renderer submits: its queue waits for the renderer's fence (the inputs are
// drawn), records the frame's work, and signals its own fence (the outputs are written). The CPU records while the
// renderer's GPU is still drawing the frame, so by the time the GPU gets there the work is waiting for it.
//
// The work itself belongs to a backend, the one the renderer asks for: Fsr (Host64Fsr.cpp), Dlss (Host64Dlss.cpp), or
// Test - the pipeline's own check, the colour copied into the output, no library.
//
// And the rays (Host64Rays.cpp), whatever the backend: on a card whose driver gives a 32-bit process no ray tracing,
// the scene's structures and the passes that trace against them are this process's work.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "Host64Protocol.h"
#include "Host64Fsr.h"
#include "Host64Dlss.h"
#include "Host64Xess.h"
#include "Host64Rays.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

template <typename T>
static void SafeRelease(T*& P)
{
    if (P)
    {
        P->Release();
        P = nullptr;
    }
}

static FILE*           GLog   = nullptr;
static Host64::FBlock* GBlock = nullptr;
static bool            GDebugLayer = false;   // "debug" on the command line: a validation run's, never a player's

// The log beside the executable, begun afresh at every start - the renderer's own log says when the host was started.
static void OpenLog()
{
    wchar_t Path[MAX_PATH];
    const DWORD N = ::GetModuleFileNameW(nullptr, Path, MAX_PATH);
    if (N == 0 || N >= MAX_PATH - 8)
        return;
    wchar_t* Dot = wcsrchr(Path, L'.');
    if (Dot)
        wcscpy_s(Dot, MAX_PATH - (Dot - Path), L".log");
    _wfopen_s(&GLog, Path, L"w");
}

static void Log(const char* Format, ...)
{
    if (!GLog)
        return;
    SYSTEMTIME T;
    ::GetLocalTime(&T);
    fprintf(GLog, "%02d:%02d:%02d.%03d ", T.wHour, T.wMinute, T.wSecond, T.wMilliseconds);
    va_list Args;
    va_start(Args, Format);
    vfprintf(GLog, Format, Args);
    va_end(Args);
    fputc('\n', GLog);
    fflush(GLog);
}

// The block's message: what the renderer's log and readout will say about the host.
static void Say(const char* Text)
{
    Log("%s", Text);
    if (GBlock)
        strncpy_s(GBlock->Message, Host64::MessageChars, Text, _TRUNCATE);
}

static void Fail(const char* Text)
{
    Say(Text);
    if (GBlock)
    {
        ::MemoryBarrier();
        GBlock->HostState = Host64::HostState_Failed;
    }
}

// A marker in the temporary folder, written in the last ten minutes - as the renderer's own markers are read.
static bool MarkerFresh(const wchar_t* Name)
{
    wchar_t Path[MAX_PATH];
    const DWORD N = ::GetTempPathW(MAX_PATH, Path);
    if (N == 0 || N + wcslen(Name) >= MAX_PATH)
        return false;
    wcscat_s(Path, Name);
    WIN32_FILE_ATTRIBUTE_DATA A;
    if (!::GetFileAttributesExW(Path, GetFileExInfoStandard, &A))
        return false;
    FILETIME Now;
    ::GetSystemTimeAsFileTime(&Now);
    const ULONGLONG T = ((ULONGLONG)A.ftLastWriteTime.dwHighDateTime << 32) | A.ftLastWriteTime.dwLowDateTime;
    const ULONGLONG C = ((ULONGLONG)Now.dwHighDateTime << 32) | Now.dwLowDateTime;
    return C >= T && C - T < 10ULL * 60ULL * 10000000ULL;      // ten minutes, in steps of 100 ns
}

// The number on a fresh marker's first line; 0 where there is no such marker or no number on it.
static float MarkerNumber(const wchar_t* Name)
{
    if (!MarkerFresh(Name))
        return 0.0f;
    wchar_t Path[MAX_PATH];
    const DWORD N = ::GetTempPathW(MAX_PATH, Path);
    if (N == 0 || N + wcslen(Name) >= MAX_PATH)
        return 0.0f;
    wcscat_s(Path, Name);
    FILE* File = nullptr;
    if (_wfopen_s(&File, Path, L"rb") != 0 || !File)
        return 0.0f;
    unsigned char Raw[64];
    const size_t  Got = fread(Raw, 1, sizeof(Raw), File);
    fclose(File);
    // The number's own characters only: a marker PowerShell wrote as UTF-16, or with a byte order mark, reads the same.
    char   Text[sizeof(Raw) + 1] = {};
    size_t Len = 0;
    for (size_t i = 0; i < Got && Raw[i] != '\n' && Raw[i] != '\r'; i++)
        if ((Raw[i] >= '0' && Raw[i] <= '9') || Raw[i] == '.' || Raw[i] == '-')
            Text[Len++] = (char)Raw[i];
    return (float)atof(Text);
}

// The version.dll in the host's own folder, if there is one: where a tool that hooks into the upscaler of the program it is
// put beside installs itself. False: none there, or no room for the path.
static bool ToolBesidePath(wchar_t* Path, DWORD Chars)
{
    const DWORD N = ::GetModuleFileNameW(nullptr, Path, Chars);
    wchar_t* const Cut = (N > 0 && N < Chars) ? wcsrchr(Path, L'\\') : nullptr;
    if (!Cut || (DWORD)(Cut - Path) + 13 >= Chars)
        return false;
    wcscpy_s(Cut + 1, Chars - (DWORD)(Cut + 1 - Path), L"version.dll");
    return ::GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

// The system's version.dll for this process - taken here, before anything asks for the name. Windows would load the one
// beside the host the moment anything in the process asked for it (the host's own look at nvngx_dlss.dll's version,
// D3D12's), and a tool that hooks D3D12 from inside the process has been seen to bring the host down on its way up - a
// call to address 0. version.dll is delay-loaded (Host64.vcxproj), and a DLL already in the process answers to its name.
static void TakeVersionDll()
{
    if (const HMODULE Before = ::GetModuleHandleW(L"version.dll"))
    {
        wchar_t Where[MAX_PATH] = L"?";
        ::GetModuleFileNameW(Before, Where, MAX_PATH);
        Log("host: version.dll was in the process before the host could choose: %ls", Where);
    }
    if (!::LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        Log("host: the system's version.dll would not load (error %lu)", ::GetLastError());
}

namespace
{
    struct FHost
    {
        IDXGIAdapter3*             Adapter   = nullptr;      // for the video memory the host's own work takes
        ID3D12Device*              Device    = nullptr;
        ID3D12CommandQueue*        Queue     = nullptr;
        ID3D12CommandAllocator*    Allocators[Host64::FrameSlots] = {};
        ID3D12GraphicsCommandList* List      = nullptr;
        ID3D12Fence*               ToHost    = nullptr;      // the renderer's: the frame's inputs are ready
        ID3D12Fence*               ToRenderer = nullptr;     // ours: the frame's outputs are ready
        ID3D12Fence*               Idle      = nullptr;      // our own, to wait for our queue to empty
        UINT64                     IdleValue = 0;
        HANDLE                     IdleEvent = nullptr;
        ID3D12Resource*            Resources[Host64::Resource_Count] = {};
        uint32_t                   Generation = 0;
        // The host's own GPU time: two timestamps a frame slot around its work, read back when the slot comes round.
        ID3D12QueryHeap*           Times        = nullptr;
        ID3D12Resource*            TimeReadback = nullptr;
        const UINT64*              TimeMapped   = nullptr;
        UINT64                     TimeFreq     = 0;
        // What the slot's last frame did that is worth timing: 0 nothing, 1 an upscale (with frame generation's prepare
        // where it came along), 2 a generated frame, 3 a frame's rays, 4 the rays' list answered.
        int                        SlotTimed[Host64::FrameSlots] = {};
        double                     WorkAvgUs    = 0.0;
        double                     WorkFgAvgUs  = 0.0;
        double                     WorkRayAvgUs = 0.0;
        double                     WorkListAvgUs = 0.0;

        void WaitIdle(DWORD AtMostMs = 5000)
        {
            if (!Queue || !Idle)
                return;
            IdleValue++;
            Queue->Signal(Idle, IdleValue);
            if (Idle->GetCompletedValue() < IdleValue)
            {
                Idle->SetEventOnCompletion(IdleValue, IdleEvent);
                ::WaitForSingleObject(IdleEvent, AtMostMs);
            }
        }

        void ReleaseResources()
        {
            for (int r = 0; r < Host64::Resource_Count; r++)
                SafeRelease(Resources[r]);
        }

        void Shutdown()
        {
            WaitIdle();
            ReleaseResources();
            if (TimeReadback && TimeMapped)
                TimeReadback->Unmap(0, nullptr);
            TimeMapped = nullptr;
            SafeRelease(TimeReadback);
            SafeRelease(Times);
            SafeRelease(List);
            for (int a = 0; a < Host64::FrameSlots; a++)
                SafeRelease(Allocators[a]);
            SafeRelease(Idle);
            SafeRelease(ToRenderer);
            SafeRelease(ToHost);
            SafeRelease(Queue);
            SafeRelease(Device);
            SafeRelease(Adapter);
            if (IdleEvent)
                ::CloseHandle(IdleEvent);
            IdleEvent = nullptr;
        }
    };
}

// A device on the renderer's own adapter: shared textures and fences only open on the device of the adapter that
// made them.
static bool MakeDevice(FHost& H, LUID Luid)
{
    IDXGIFactory4* Factory = nullptr;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&Factory))))
    {
        Fail("host: no DXGI factory");
        return false;
    }
    IDXGIAdapter1* Adapter = nullptr;
    const HRESULT  Found   = Factory->EnumAdapterByLuid(Luid, IID_PPV_ARGS(&Adapter));
    SafeRelease(Factory);
    if (FAILED(Found))
    {
        Fail("host: the renderer's adapter is not among this process's adapters");
        return false;
    }
    DXGI_ADAPTER_DESC1 Desc = {};
    Adapter->GetDesc1(&Desc);
    char Name[128] = {};
    ::WideCharToMultiByte(CP_ACP, 0, Desc.Description, -1, Name, sizeof(Name) - 1, nullptr, nullptr);
    if (GDebugLayer)
    {
        ID3D12Debug* Debug = nullptr;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&Debug))))
        {
            Debug->EnableDebugLayer();
            // The deeper check, the renderer's own marker for it: what a shader reads is held against the state the
            // resource is in on the GPU - the one check that sees a texture read in a state its reader may not read it
            // in. Many times slower; a 64-bit process has the room for it that the game's has not.
            ID3D12Debug1* Deeper = nullptr;
            const bool bDeeper = MarkerFresh(L"SecondRevision-gbv.txt") &&
                                 SUCCEEDED(Debug->QueryInterface(IID_PPV_ARGS(&Deeper)));
            if (bDeeper)
            {
                Deeper->SetEnableGPUBasedValidation(TRUE);
                Deeper->Release();
            }
            Debug->Release();
            Log("host: the D3D12 debug layer is on%s", bDeeper ? ", with GPU-based validation (the marker "
                                                                 "SecondRevision-gbv.txt)" : "");
        }
        else
            Log("host: the D3D12 debug layer was asked for and is not installed");
    }
    const HRESULT Made = ::D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&H.Device));
    Adapter->QueryInterface(IID_PPV_ARGS(&H.Adapter));
    SafeRelease(Adapter);
    if (FAILED(Made))
    {
        Fail("host: D3D12CreateDevice failed on the renderer's adapter");
        return false;
    }
    Log("host: device on %s", Name);

    // A high-priority queue: the renderer's queue stands still while this one works, so what else the card has to do
    // (the desktop's composition, a video in a browser) should not come first.
    D3D12_COMMAND_QUEUE_DESC Q = {};
    Q.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(H.Device->CreateCommandQueue(&Q, IID_PPV_ARGS(&H.Queue))))
    {
        Q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(H.Device->CreateCommandQueue(&Q, IID_PPV_ARGS(&H.Queue))))
        {
            Fail("host: no command queue");
            return false;
        }
    }
    Log("host: queue priority %s", (Q.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH) ? "high" : "normal");

    // The timestamps. Without them the host works the same, it only cannot say what its work costs.
    D3D12_QUERY_HEAP_DESC Qh = {};
    Qh.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    Qh.Count = 2 * Host64::FrameSlots;
    D3D12_HEAP_PROPERTIES Rb = {};
    Rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC Buf = {};
    Buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Buf.Width            = sizeof(UINT64) * 2 * Host64::FrameSlots;
    Buf.Height           = 1;
    Buf.DepthOrArraySize = 1;
    Buf.MipLevels        = 1;
    Buf.SampleDesc.Count = 1;
    Buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* Mapped = nullptr;
    if (FAILED(H.Queue->GetTimestampFrequency(&H.TimeFreq)) ||
        FAILED(H.Device->CreateQueryHeap(&Qh, IID_PPV_ARGS(&H.Times))) ||
        FAILED(H.Device->CreateCommittedResource(&Rb, D3D12_HEAP_FLAG_NONE, &Buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&H.TimeReadback))) ||
        FAILED(H.TimeReadback->Map(0, nullptr, &Mapped)))
    {
        Log("host: no timestamps - its GPU time will not be known");
        SafeRelease(H.TimeReadback);
        SafeRelease(H.Times);
    }
    else
        H.TimeMapped = (const UINT64*)Mapped;
    for (int a = 0; a < Host64::FrameSlots; a++)
        if (FAILED(H.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&H.Allocators[a]))))
        {
            Fail("host: no command allocator");
            return false;
        }
    if (FAILED(H.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, H.Allocators[0], nullptr,
                                           IID_PPV_ARGS(&H.List))))
    {
        Fail("host: no command list");
        return false;
    }
    H.List->Close();
    H.IdleEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(H.Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&H.Idle))) || !H.IdleEvent)
    {
        Fail("host: no fence of its own");
        return false;
    }
    return true;
}

// The shared textures of the generation the renderer wrote, opened afresh - after the queue has let go of the old ones.
static bool OpenResources(FHost& H, const Host64::FBlock& B)
{
    H.WaitIdle();
    H.ReleaseResources();
    for (int r = 0; r < Host64::Resource_Count; r++)
    {
        const uint64_t Handle = B.Resources[r].Handle;
        if (!Handle)
            continue;
        if (FAILED(H.Device->OpenSharedHandle((HANDLE)(uintptr_t)Handle, IID_PPV_ARGS(&H.Resources[r]))))
        {
            Log("host: shared texture %d (handle 0x%llx) would not open", r, (unsigned long long)Handle);
            return false;
        }
        // The handle was duplicated into this process for this one use: the texture now holds the memory.
        ::CloseHandle((HANDLE)(uintptr_t)Handle);
    }
    Log("host: shared textures of generation %u open", B.ResourceGeneration);
    return true;
}

static void Transition(ID3D12GraphicsCommandList* List, ID3D12Resource* R, D3D12_RESOURCE_STATES From,
                       D3D12_RESOURCE_STATES To)
{
    D3D12_RESOURCE_BARRIER B = {};
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = R;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = From;
    B.Transition.StateAfter  = To;
    List->ResourceBarrier(1, &B);
}

// The Fsr backend: the inputs made readable, the output writable, the upscale, and everything back in COMMON - where
// every shared texture is between the two processes. The upscaler is made on the first frame, for the textures' sizes
// and the frame's HDR, and made again when either changes.
static bool RecordFsr(FHost& H, FHostFsr& Fsr, const Host64::FFrame& F)
{
    ID3D12Resource* const* R = H.Resources;
    ID3D12Resource* const  Color  = R[Host64::Resource_Color];
    ID3D12Resource* const  Output = R[Host64::Resource_Output];
    if (!Color || !Output || !R[Host64::Resource_Depth] || !R[Host64::Resource_Motion])
        return false;
    const D3D12_RESOURCE_DESC CD = Color->GetDesc(), OD = Output->GetDesc();
    if (!Fsr.Prepare((uint32_t)CD.Width, CD.Height, (uint32_t)OD.Width, OD.Height, (F.Flags & Host64::FrameFlag_Hdr) != 0))
        return false;
    const int Inputs[4] = { Host64::Resource_Color, Host64::Resource_Depth, Host64::Resource_Motion,
                            Host64::Resource_Reactive };
    for (int i = 0; i < 4; i++)
        if (R[Inputs[i]])
            Transition(H.List, R[Inputs[i]], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(H.List, Output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool bDone = Fsr.Upscale(H.List, R, F);
    Transition(H.List, Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    for (int i = 0; i < 4; i++)
        if (R[Inputs[i]])
            Transition(H.List, R[Inputs[i]], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    return bDone;
}

// The Dlss backend, as the Fsr one: the inputs readable, the output writable, the upscale, everything back in COMMON.
// DLSS is there by the first frame - the host makes it as it takes the textures - for their sizes; a frame whose HDR is
// not what it was made for makes it again, the old one let go only once the GPU is done with it.
static bool RecordDlss(FHost& H, FHostDlss& Dlss, const Host64::FFrame& F)
{
    ID3D12Resource* const* R = H.Resources;
    ID3D12Resource* const  Color  = R[Host64::Resource_Color];
    ID3D12Resource* const  Output = R[Host64::Resource_Output];
    if (!Color || !Output || !R[Host64::Resource_Depth] || !R[Host64::Resource_Motion])
        return false;
    const D3D12_RESOURCE_DESC CD = Color->GetDesc(), OD = Output->GetDesc();
    Dlss.Prepare((uint32_t)CD.Width, CD.Height, (uint32_t)OD.Width, OD.Height, (F.Flags & Host64::FrameFlag_Hdr) != 0);
    if (Dlss.NeedsRebuild())
    {
        H.WaitIdle();
        Dlss.Release();
    }
    const int Inputs[3] = { Host64::Resource_Color, Host64::Resource_Depth, Host64::Resource_Motion };
    for (int i = 0; i < 3; i++)
        Transition(H.List, R[Inputs[i]], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(H.List, Output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool bDone = Dlss.Upscale(H.List, R, F);
    Transition(H.List, Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    for (int i = 0; i < 3; i++)
        Transition(H.List, R[Inputs[i]], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    return bDone;
}

// The Xess backend, as the Dlss one: the inputs readable, the output writable, the upscale, everything back in COMMON.
// XeSS is made by the first frame - the host makes it as it takes the textures - for their sizes; a frame whose HDR is
// not what it was made for makes it again, once the GPU is done with the old one.
static bool RecordXess(FHost& H, FHostXess& Xess, const Host64::FFrame& F)
{
    ID3D12Resource* const* R = H.Resources;
    ID3D12Resource* const  Color  = R[Host64::Resource_Color];
    ID3D12Resource* const  Output = R[Host64::Resource_Output];
    if (!Color || !Output || !R[Host64::Resource_Depth] || !R[Host64::Resource_Motion])
        return false;
    const D3D12_RESOURCE_DESC CD = Color->GetDesc(), OD = Output->GetDesc();
    Xess.Prepare((uint32_t)CD.Width, CD.Height, (uint32_t)OD.Width, OD.Height, (F.Flags & Host64::FrameFlag_Hdr) != 0);
    if (Xess.NeedsRebuild())
        H.WaitIdle();
    const int Inputs[3] = { Host64::Resource_Color, Host64::Resource_Depth, Host64::Resource_Motion };
    for (int i = 0; i < 3; i++)
        Transition(H.List, R[Inputs[i]], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(H.List, Output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool bDone = Xess.Upscale(H.List, R, F);
    Transition(H.List, Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    for (int i = 0; i < 3; i++)
        Transition(H.List, R[Inputs[i]], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    return bDone;
}

// Frame generation's first half: the depth and the motion readable while its configure and prepare take them, and back
// in COMMON after.
static bool RecordFramePrepare(FHost& H, FHostFsr& Fsr, const Host64::FFrame& F)
{
    ID3D12Resource* const* R = H.Resources;
    ID3D12Resource* const  Depth  = R[Host64::Resource_Depth];
    ID3D12Resource* const  Motion = R[Host64::Resource_Motion];
    if (!Depth || !Motion)
        return false;
    Transition(H.List, Depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(H.List, Motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool bDone = Fsr.PrepareFrame(H.List, R, F);
    Transition(H.List, Motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(H.List, Depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    return bDone;
}

// Frame generation's second half: the finished picture (and the HUD-less one, where it came) readable, the generated
// frame writable, and all of them back in COMMON after.
static bool RecordFrameGen(FHost& H, FHostFsr& Fsr, const Host64::FFrame& F)
{
    ID3D12Resource* const* R = H.Resources;
    ID3D12Resource* const  Generated = R[Host64::Resource_Generated];
    const int Inputs[2] = { Host64::Resource_Present, Host64::Resource_HudLess };
    if (!R[Host64::Resource_Present] || !Generated)
        return false;
    for (int i = 0; i < 2; i++)
        if (R[Inputs[i]])
            Transition(H.List, R[Inputs[i]], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(H.List, Generated, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool bDone = Fsr.Generate(H.List, R, F);
    Transition(H.List, Generated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    for (int i = 0; i < 2; i++)
        if (R[Inputs[i]])
            Transition(H.List, R[Inputs[i]], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    return bDone;
}

// The Dlss backend's frame generation, in the same two halves. Its first half keeps the frame's depth and motion in
// copies of its own, so the two are copy sources while it runs; its second takes the pictures as the Fsr one does.
static bool RecordDlssFramePrepare(FHost& H, FHostDlss& Dlss, const Host64::FFrame& F)
{
    ID3D12Resource* const* R = H.Resources;
    ID3D12Resource* const  Depth  = R[Host64::Resource_Depth];
    ID3D12Resource* const  Motion = R[Host64::Resource_Motion];
    if (!Depth || !Motion)
        return false;
    Transition(H.List, Depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(H.List, Motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const bool bDone = Dlss.PrepareFrame(H.List, R, F);
    Transition(H.List, Motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(H.List, Depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    return bDone;
}

static bool RecordDlssFrameGen(FHost& H, FHostDlss& Dlss, const Host64::FFrame& F)
{
    ID3D12Resource* const* R = H.Resources;
    ID3D12Resource* const  Generated = R[Host64::Resource_Generated];
    const int Inputs[2] = { Host64::Resource_Present, Host64::Resource_HudLess };
    const D3D12_RESOURCE_STATES Read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!R[Host64::Resource_Present] || !Generated)
        return false;
    for (int i = 0; i < 2; i++)
        if (R[Inputs[i]])
            Transition(H.List, R[Inputs[i]], D3D12_RESOURCE_STATE_COMMON, Read);
    Transition(H.List, Generated, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool bDone = Dlss.Generate(H.List, R, F);
    Transition(H.List, Generated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    for (int i = 0; i < 2; i++)
        if (R[Inputs[i]])
            Transition(H.List, R[Inputs[i]], Read, D3D12_RESOURCE_STATE_COMMON);
    return bDone;
}

// The Test backend: the colour into the output, the part both have. Every shared texture is in COMMON between the two
// processes; the work takes it out and puts it back.
static void RecordTest(FHost& H, const Host64::FFrame& F)
{
    ID3D12Resource* const Color  = H.Resources[Host64::Resource_Color];
    ID3D12Resource* const Output = H.Resources[Host64::Resource_Output];
    if (!Color || !Output)
        return;
    Transition(H.List, Color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(H.List, Output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION Src = {}, Dst = {};
    Src.pResource = Color;
    Src.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Dst.pResource = Output;
    Dst.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_RESOURCE_DESC CD = Color->GetDesc(), OD = Output->GetDesc();
    D3D12_BOX Box = {};
    Box.right  = (UINT)((CD.Width < OD.Width) ? CD.Width : OD.Width);
    Box.bottom = (CD.Height < OD.Height) ? CD.Height : OD.Height;
    if (F.RenderWidth && F.RenderWidth < Box.right)
        Box.right = F.RenderWidth;
    if (F.RenderHeight && F.RenderHeight < Box.bottom)
        Box.bottom = F.RenderHeight;
    Box.back = 1;
    H.List->CopyTextureRegion(&Dst, 0, 0, 0, &Src, &Box);
    Transition(H.List, Output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Transition(H.List, Color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
}

// With the debug layer on: what it has to say, into the log, and the queue emptied for the next frame's.
static void DrainDebug(ID3D12Device* Device)
{
    if (!GDebugLayer || !Device)
        return;
    ID3D12InfoQueue* Q = nullptr;
    if (FAILED(Device->QueryInterface(IID_PPV_ARGS(&Q))))
        return;
    // The first two hundred are told; every error is counted, into the block - whoever runs the host with the layer on
    // reads there whether it stayed silent.
    static int Told = 0;
    const UINT64 N = Q->GetNumStoredMessages();
    for (UINT64 i = 0; i < N; i++)
    {
        SIZE_T Bytes = 0;
        Q->GetMessage(i, nullptr, &Bytes);
        D3D12_MESSAGE* M = (D3D12_MESSAGE*)malloc(Bytes);
        if (M && SUCCEEDED(Q->GetMessage(i, M, &Bytes)) && M->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            const bool bError = M->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
            if (bError && GBlock)
                ::InterlockedIncrement((volatile LONG*)&GBlock->DebugErrors);
            if (Told < 200)
            {
                Log("d3d12 (%s): %s", bError ? "error" : "warning", M->pDescription);
                Told++;
            }
        }
        free(M);
    }
    Q->ClearStoredMessages();
    Q->Release();
}

// The video memory this process holds on the card, and what the system allows it - the renderer's own figures do not
// include ours.
static void LogMemory(const FHost& H, const char* When)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO Info = {};
    if (H.Adapter && SUCCEEDED(H.Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)))
        Log("host: video memory %s: %llu MB used, %llu MB budget", When,
            (unsigned long long)(Info.CurrentUsage >> 20), (unsigned long long)(Info.Budget >> 20));
}

// The host's CPU work for a frame has to be on its queue before the GPU gets there - the renderer's queue waits for it.
// So no power throttling (Windows may take a process without a window for background work and slow it down), and a
// thread that, asleep nearly all the time, gets the core the moment it is woken.
static void RunAtFullSpeed()
{
    PROCESS_POWER_THROTTLING_STATE Throttle = {};
    Throttle.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    Throttle.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    Throttle.StateMask   = 0;
    const BOOL bSpeed = ::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &Throttle, sizeof(Throttle));
    const BOOL bPrio  = ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    Log("host: power throttling %s, thread priority %s", bSpeed ? "off" : "left to Windows (the call failed)",
        bPrio ? "highest" : "normal (the call failed)");
}

// The GPU time of the slot's last frame, into the running average the renderer reads. Called when the slot comes round,
// after its frame is known to be done.
static void ReadSlotTime(FHost& H, int Slot)
{
    const int Kind = H.SlotTimed[Slot];
    H.SlotTimed[Slot] = 0;
    if (!H.TimeMapped || !H.TimeFreq || !Kind)
        return;
    const UINT64 T0 = H.TimeMapped[2 * Slot], T1 = H.TimeMapped[2 * Slot + 1];
    if (T1 <= T0)
        return;
    const double Us  = (double)(T1 - T0) * 1000000.0 / (double)H.TimeFreq;
    double&      Avg = (Kind == 4) ? H.WorkListAvgUs : (Kind == 3) ? H.WorkRayAvgUs
                     : (Kind == 2) ? H.WorkFgAvgUs : H.WorkAvgUs;
    Avg = (Avg > 0.0) ? Avg * 0.9 + Us * 0.1 : Us;
    uint32_t* const Out = (Kind == 4) ? &GBlock->WorkListUs : (Kind == 3) ? &GBlock->WorkRayUs
                        : (Kind == 2) ? &GBlock->WorkFgUs : &GBlock->WorkUs;
    *(volatile uint32_t*)Out = (uint32_t)(Avg + 0.5);
    if (Kind == 4)
        ::InterlockedIncrement((volatile LONG*)&GBlock->WorkListTimed);
}

static uint64_t ReadSubmitted(const Host64::FBlock* B)
{
    const uint64_t V = *(volatile const uint64_t*)&B->Submitted;
    ::MemoryBarrier();
    return V;
}

int wmain(int Argc, wchar_t** Argv)
{
    OpenLog();
    if (Argc < 3)
    {
        Log("host: started without its block - SecondRevisionHost64.exe <block handle> <parent pid>");
        return 2;
    }
    const HANDLE Mapping   = (HANDLE)(uintptr_t)_wcstoui64(Argv[1], nullptr, 0);
    const DWORD  ParentPid = (DWORD)wcstoul(Argv[2], nullptr, 0);
    GDebugLayer = (Argc > 3 && _wcsicmp(Argv[3], L"debug") == 0);
    GBlock = (Host64::FBlock*)::MapViewOfFile(Mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Host64::FBlock));
    if (!GBlock)
    {
        Log("host: the block would not map (handle %ls)", Argv[1]);
        return 3;
    }
    GBlock->HostPid     = ::GetCurrentProcessId();
    GBlock->HostVersion = Host64::HostBuild;
    if (GBlock->Magic != Host64::Magic || GBlock->Version != Host64::ProtocolVersion)
    {
        Fail("host: the renderer speaks another version of the protocol");
        return 4;
    }
    const HANDLE Parent = ::OpenProcess(SYNCHRONIZE, FALSE, ParentPid);
    if (!Parent || GBlock->RendererPid != ParentPid)
    {
        Fail("host: its parent is not the renderer that wrote the block");
        return 5;
    }
    RunAtFullSpeed();
    wchar_t     ToolPath[MAX_PATH];
    const bool  bToolBeside = ToolBesidePath(ToolPath, MAX_PATH);
    TakeVersionDll();
    // A measurement only (the marker SecondRevision-host-fail-start.txt): the host falls over on its way up, as the tool
    // was seen to make it do - once: the marker goes with it, so the renderer's next start of it comes up.
    if (MarkerFresh(L"SecondRevision-host-fail-start.txt"))
    {
        wchar_t Marker[MAX_PATH];
        const DWORD N = ::GetTempPathW(MAX_PATH, Marker);
        if (N > 0 && N + 40 < MAX_PATH)
        {
            wcscat_s(Marker, L"SecondRevision-host-fail-start.txt");
            ::DeleteFileW(Marker);
        }
        Log("host: falling over on its way up (the marker SecondRevision-host-fail-start.txt)");
        ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u);
    }
    Log("host: version.dll from the system%s", bToolBeside ? " - the one beside the host is left out" : "");

    FHost H;
    LUID  Luid;
    Luid.LowPart  = GBlock->AdapterLuidLow;
    Luid.HighPart = GBlock->AdapterLuidHigh;
    if (!MakeDevice(H, Luid) ||
        FAILED(H.Device->OpenSharedHandle((HANDLE)(uintptr_t)GBlock->FenceToHost, IID_PPV_ARGS(&H.ToHost))) ||
        FAILED(H.Device->OpenSharedHandle((HANDLE)(uintptr_t)GBlock->FenceToRenderer, IID_PPV_ARGS(&H.ToRenderer))))
    {
        if (GBlock->HostState != Host64::HostState_Failed)
            Fail("host: the renderer's fences would not open");
        H.Shutdown();
        return 6;
    }
    // The fences' handles were duplicated into this process for this one use: the fences now hold what they need.
    ::CloseHandle((HANDLE)(uintptr_t)GBlock->FenceToHost);
    ::CloseHandle((HANDLE)(uintptr_t)GBlock->FenceToRenderer);
    const HANDLE Wake = (HANDLE)(uintptr_t)GBlock->WakeEvent;

    // The backend the renderer wants. Test needs nothing; Fsr needs its DLLs and a device that can run an upscaler; Dlss
    // an NVIDIA card with NGX in its driver, and nvngx_dlss.dll beside the host; Xess libxess.dll beside the host and a
    // card with Shader Model 6.4.
    FHostFsr        Fsr;
    FHostDlss       Dlss;
    FHostXess       Xess;
    const uint32_t  Want = GBlock->WantBackend;
    GBlock->Backends = 1u << Host64::Backend_Test;
    GBlock->Services = Host64::Service_Upscale;
    if (Want == Host64::Backend_Fsr)
    {
        char Why[Host64::MessageChars] = {};
        if (!Fsr.Load(H.Device, [](const char* Line) { Log("%s", Line); }, Why, sizeof(Why)))
        {
            Fail(Why);
            H.Shutdown();
            return 7;
        }
        GBlock->Backends |= 1u << Host64::Backend_Fsr;
        if (Fsr.HasFrameGen())
            GBlock->Services |= Host64::Service_FrameGen;
        Say("host: ready - backend Fsr (the upscaler is made when the textures come)");
    }
    else if (Want == Host64::Backend_Dlss)
    {
        // Measurements only: the conventions nobody could try without the card, each turned by a marker of its own.
        static const struct { const wchar_t* Marker; uint32_t Bit; } Probes[] = {
            { L"SecondRevision-dlss-jitter-flip-x.txt", DlssProbe_JitterFlipX },
            { L"SecondRevision-dlss-jitter-flip-y.txt", DlssProbe_JitterFlipY },
            { L"SecondRevision-dlss-motion-flip-x.txt", DlssProbe_MotionFlipX },
            { L"SecondRevision-dlss-motion-flip-y.txt", DlssProbe_MotionFlipY },
            { L"SecondRevision-dlssg-transpose.txt", DlssProbe_FgTranspose },
            { L"SecondRevision-dlssg-no-jitter.txt", DlssProbe_FgNoJitter },
            { L"SecondRevision-dlssg-jitter-flip.txt", DlssProbe_FgJitterFlip },
            { L"SecondRevision-dlssg-no-hudless.txt", DlssProbe_FgNoHudLess },
            { L"SecondRevision-dlssg-invalid-zero.txt", DlssProbe_FgInvalidZero },
        };
        uint32_t Probe = 0;
        for (const auto& P : Probes)
            if (MarkerFresh(P.Marker))
                Probe |= P.Bit;
        Dlss.SetProbe(Probe);
        // One with a number on it: the scale on frame generation's linear depth. Anything else than a sensible one is
        // no scale at all.
        float DepthScale = MarkerNumber(L"SecondRevision-dlssg-depth-scale.txt");
        if (!(DepthScale > 0.0f && DepthScale <= 100.0f))
            DepthScale = 0.0f;
        Dlss.SetFgDepthScale(DepthScale);
        // Said before NGX is asked for anything: a run that ends at NGX's refusal still shows its markers were seen.
        if (Probe)
            Log("dlss: a measurement turns its conventions - probe bits 0x%03x", Probe);
        if (DepthScale > 0.0f)
            Log("dlss: a measurement scales frame generation's linear depth by %g", DepthScale);
        char Why[Host64::MessageChars] = {};
        if (!Dlss.Load(H.Device, [](const char* Line) { Log("%s", Line); }, Why, sizeof(Why)))
        {
            Fail(Why);
            H.Shutdown();
            return 7;
        }
        GBlock->Backends |= 1u << Host64::Backend_Dlss;
        if (Dlss.HasFrameGen())
            GBlock->Services |= Host64::Service_FrameGen;
        Say("host: ready - backend Dlss (DLSS is made when the textures come)");
    }
    else if (Want == Host64::Backend_Xess)
    {
        char Why[Host64::MessageChars] = {};
        if (!Xess.Load(H.Device, [](const char* Line) { Log("%s", Line); }, Why, sizeof(Why)))
        {
            Fail(Why);
            H.Shutdown();
            return 7;
        }
        GBlock->Backends |= 1u << Host64::Backend_Xess;
        Say("host: ready - backend Xess (XeSS is made when the textures come)");
    }
    else
        Say("host: ready - backend Test (a copy, no library)");
    // The rays, whatever the backend: what the card traces is the card's own, no library's. Without them the host
    // serves as it does, and a frame that asks for rays is refused.
    FHostRays Rays;
    {
        char Why[Host64::MessageChars] = {};
        if (Rays.Load(H.Device, [](const char* Line) { Log("%s", Line); }, Why, sizeof(Why)))
        {
            GBlock->RayPasses = Rays.Passes();
            GBlock->Services |= Host64::Service_Rays;
        }
        else
            Log("%s", Why);
    }
    ::MemoryBarrier();
    GBlock->HostState = Host64::HostState_Ready;

    uint64_t Taken       = GBlock->Taken;
    char     Running[64] = {};
    for (;;)
    {
        const HANDLE Waits[2] = { Wake, Parent };
        const DWORD  Why      = ::WaitForMultipleObjects(2, Waits, FALSE, 1000);
        if (Why == WAIT_OBJECT_0 + 1 || Why == WAIT_FAILED)
        {
            Log("host: the renderer's process is gone - leaving");
            break;
        }
        if (GBlock->QuitRequested)
        {
            Log("host: the renderer asked it to leave");
            break;
        }
        if (GBlock->ResourceGeneration != GBlock->ResourceAck)
        {
            const uint32_t Generation = GBlock->ResourceGeneration;
            ::MemoryBarrier();
            if (!OpenResources(H, *GBlock))
            {
                Fail("host: the shared textures would not open");
                break;
            }
            // The upscaler for the new sizes, now: the renderer waits for the answer anyway, while a frame's work
            // that took seconds would look like a host that hung. The HDR flag is the frames' to change.
            ID3D12Resource* const Color  = H.Resources[Host64::Resource_Color];
            ID3D12Resource* const Output = H.Resources[Host64::Resource_Output];
            if (Want == Host64::Backend_Fsr && Color && Output)
            {
                const D3D12_RESOURCE_DESC CD = Color->GetDesc(), OD = Output->GetDesc();
                if (!Fsr.Prepare((uint32_t)CD.Width, CD.Height, (uint32_t)OD.Width, OD.Height, false))
                {
                    Fail("fsr: the upscaler would not come up for these sizes");
                    break;
                }
                LogMemory(H, "with the upscaler");
                // What runs, in the block before the answer: the renderer's readout names it from its first frame.
                strcpy_s(Running, Fsr.Running());
                Say(Running);
            }
            // What runs is known already; DLSS itself is made below, once frame generation's sizes are known too.
            if (Want == Host64::Backend_Dlss && Color && Output)
            {
                strcpy_s(Running, Dlss.Running());
                Say(Running);
            }
            // Frame generation, where its textures came: the depth and motion it shares with the upscaler, the finished
            // picture it generates from, and its own output. Its failure fails no upscale: its frames are refused.
            ID3D12Resource* const Present   = H.Resources[Host64::Resource_Present];
            ID3D12Resource* const Generated = H.Resources[Host64::Resource_Generated];
            ID3D12Resource* const Depth     = H.Resources[Host64::Resource_Depth];
            if (Want == Host64::Backend_Fsr && Fsr.HasFrameGen() && Present && Generated && Depth &&
                H.Resources[Host64::Resource_Motion])
            {
                const D3D12_RESOURCE_DESC DD = Depth->GetDesc(), PD = Present->GetDesc();
                ID3D12Resource* const HudLess = H.Resources[Host64::Resource_HudLess];
                const uint32_t HudLessFormat  = HudLess ? (uint32_t)HudLess->GetDesc().Format : 0u;
                if (Fsr.PrepareFg((uint32_t)DD.Width, DD.Height, (uint32_t)PD.Width, PD.Height, (uint32_t)PD.Format,
                                  HudLessFormat, false))
                {
                    LogMemory(H, "with frame generation");
                    char Line[Host64::MessageChars];
                    sprintf_s(Line, "%s%s%s", Running, Running[0] ? "; " : "", Fsr.RunningFg());
                    Say(Line);
                }
            }
            // The Dlss backend's frame generation: what generates is known already. The queue is idle here
            // (OpenResources waited), which is what letting the old one go needs.
            if (Want == Host64::Backend_Dlss && Dlss.HasFrameGen() && Present && Generated && Depth &&
                H.Resources[Host64::Resource_Motion])
            {
                const D3D12_RESOURCE_DESC DD = Depth->GetDesc(), PD = Present->GetDesc();
                Dlss.PrepareFg((uint32_t)DD.Width, DD.Height, (uint32_t)PD.Width, PD.Height, (uint32_t)PD.Format);
                char Line[Host64::MessageChars];
                sprintf_s(Line, "%s%s%s", Running, Running[0] ? "; " : "", Dlss.RunningFg());
                Say(Line);
            }
            // DLSS and its frame generation are made now rather than with a frame's commands: the making can take
            // seconds the first time on a machine, and a frame's work that long would look like a host that hung.
            // Whether the colour is HDR is the frames' to say: what the last one said is kept. What a frame finds
            // missing or stale it makes itself.
            if (Want == Host64::Backend_Dlss && Color && Output)
            {
                const D3D12_RESOURCE_DESC CD = Color->GetDesc(), OD = Output->GetDesc();
                Dlss.Prepare((uint32_t)CD.Width, CD.Height, (uint32_t)OD.Width, OD.Height, Dlss.Hdr());
                if (Dlss.NeedsRebuild())
                    Dlss.Release();
                H.Allocators[0]->Reset();
                H.List->Reset(H.Allocators[0], nullptr);
                Dlss.Make(H.List);
                H.List->Close();
                ID3D12CommandList* Lists[1] = { H.List };
                H.Queue->ExecuteCommandLists(1, Lists);
                H.WaitIdle(60000);
                LogMemory(H, "with DLSS");
            }
            // XeSS made now, as DLSS is: its making is blocking and builds its pipelines. The queue is idle here
            // (OpenResources waited), which making it again for other sizes needs.
            if (Want == Host64::Backend_Xess && Color && Output)
            {
                const D3D12_RESOURCE_DESC CD = Color->GetDesc(), OD = Output->GetDesc();
                Xess.Prepare((uint32_t)CD.Width, CD.Height, (uint32_t)OD.Width, OD.Height, Xess.Hdr());
                if (!Xess.Make())
                {
                    Fail("xess: the upscaler would not come up for these sizes");
                    break;
                }
                LogMemory(H, "with XeSS");
                strcpy_s(Running, Xess.Running());
                Say(Running);
            }
            H.Generation = Generation;
            ::MemoryBarrier();
            GBlock->ResourceAck = Generation;
        }
        // The rays' share, taken with the GPU done with the last one's. One that would not open fails nothing else:
        // the frames that ask for rays are refused.
        if (GBlock->RayGeneration != GBlock->RayAck)
        {
            const uint32_t Generation = GBlock->RayGeneration;
            ::MemoryBarrier();
            H.WaitIdle();
            const Host64::FRayShare Share = GBlock->Rays;
            if (Rays.Take(Share))
                GBlock->RayTaken = Generation;
            else
                Log("rays: the share of generation %u was not taken - frames that ask for rays are refused", Generation);
            ::MemoryBarrier();
            GBlock->RayAck = Generation;
        }
        for (uint64_t Submitted = ReadSubmitted(GBlock); Taken < Submitted; )
        {
            const uint64_t       Frame = Taken + 1;
            const Host64::FFrame F     = GBlock->Frames[Frame % Host64::FrameSlots];
            if (F.Frame != Frame)
            {
                Fail("host: a frame's slot did not hold that frame");
                break;
            }
            // The slot's allocator was last used FrameSlots frames ago: its work has to be done before it is reset -
            // and then its timestamps are there to read.
            const int Slot = (int)(Frame % Host64::FrameSlots);
            if (Frame > Host64::FrameSlots && H.ToRenderer->GetCompletedValue() < Frame - Host64::FrameSlots)
            {
                H.ToRenderer->SetEventOnCompletion(Frame - Host64::FrameSlots, H.IdleEvent);
                ::WaitForSingleObject(H.IdleEvent, 5000);
            }
            ReadSlotTime(H, Slot);
            H.Allocators[Slot]->Reset();
            H.List->Reset(H.Allocators[Slot], nullptr);
            if (H.Times)
                H.List->EndQuery(H.Times, D3D12_QUERY_TYPE_TIMESTAMP, 2 * Slot);
            // A refused frame still goes through the fences - the renderer's queue is waiting for it - but its output is
            // not this frame's picture, and the renderer is told so: the count, and the run of them.
            bool bRefused = false;
            // The frame's rays first: where they come with an upscale, what they write is the older of the two.
            if ((F.Services & Host64::Service_Rays) && !Rays.Trace(H.List, Frame))
                bRefused = true;
            if (F.Services & Host64::Service_Upscale)
            {
                if (Want == Host64::Backend_Fsr)
                {
                    if (!RecordFsr(H, Fsr, F))
                        bRefused = true;
                    else
                    {
                        // What runs goes into the block the moment it is known or changes: the renderer's readout
                        // names it.
                        if (strcmp(Fsr.Running(), Running) != 0)
                        {
                            strcpy_s(Running, Fsr.Running());
                            char Line[Host64::MessageChars];
                            sprintf_s(Line, "%s", Running);
                            Say(Line);
                        }
                        if (Frame == 1 || (Frame % 1800) == 0)
                            Log("host: frame %llu upscaled by %s", (unsigned long long)Frame, Running);
                    }
                }
                else if (Want == Host64::Backend_Dlss)
                {
                    if (!RecordDlss(H, Dlss, F))
                        bRefused = true;
                    else if (Frame == 1 || (Frame % 1800) == 0)
                        Log("host: frame %llu upscaled by %s", (unsigned long long)Frame, Dlss.Running());
                }
                else if (Want == Host64::Backend_Xess)
                {
                    if (!RecordXess(H, Xess, F))
                        bRefused = true;
                    else if (Frame == 1 || (Frame % 1800) == 0)
                        Log("host: frame %llu upscaled by %s", (unsigned long long)Frame, Xess.Running());
                }
                else
                    RecordTest(H, F);
            }
            // Frame generation, its halves in the SDK's order: prepare (with or after the upscale), then generate.
            if (F.Services & Host64::Service_FramePrepare)
            {
                const bool bPrepared = (Want == Host64::Backend_Fsr)  ? RecordFramePrepare(H, Fsr, F)
                                     : (Want == Host64::Backend_Dlss) ? RecordDlssFramePrepare(H, Dlss, F)
                                                                      : false;
                if (!bPrepared)
                    bRefused = true;
            }
            if (F.Services & Host64::Service_FrameGen)
            {
                const bool bGenerated = (Want == Host64::Backend_Fsr)  ? RecordFrameGen(H, Fsr, F)
                                      : (Want == Host64::Backend_Dlss) ? RecordDlssFrameGen(H, Dlss, F)
                                                                       : false;
                if (!bGenerated)
                    bRefused = true;
                else if (Frame == 1 || (Frame % 1800) == 0)
                    Log("host: frame %llu generated by %s", (unsigned long long)Frame,
                        (Want == Host64::Backend_Dlss) ? Dlss.RunningFg() : Fsr.RunningFg());
            }
            if (bRefused)
            {
                ::InterlockedIncrement((volatile LONG*)&GBlock->FramesFailed);
                ::InterlockedIncrement((volatile LONG*)&GBlock->FailedInARow);
            }
            else
                ::InterlockedExchange((volatile LONG*)&GBlock->FailedInARow, 0);
            if (H.Times)
            {
                H.List->EndQuery(H.Times, D3D12_QUERY_TYPE_TIMESTAMP, 2 * Slot + 1);
                H.List->ResolveQueryData(H.Times, D3D12_QUERY_TYPE_TIMESTAMP, 2 * Slot, 2, H.TimeReadback,
                                         sizeof(UINT64) * 2 * Slot);
                H.SlotTimed[Slot] = bRefused ? 0
                                  : (F.Services & Host64::Service_Rays) ? (Rays.TracedList() ? 4 : 3)
                                  : (F.Services & Host64::Service_Upscale) ? 1
                                  : (F.Services & Host64::Service_FrameGen) ? 2 : 0;
            }
            H.List->Close();
            H.Queue->Wait(H.ToHost, Frame);
            ID3D12CommandList* Lists[1] = { H.List };
            H.Queue->ExecuteCommandLists(1, Lists);
            H.Queue->Signal(H.ToRenderer, Frame);
            Taken = Frame;
            ::InterlockedExchange64((volatile LONG64*)&GBlock->Taken, (LONG64)Taken);
            DrainDebug(H.Device);
            // A lost device signals nothing any more: said at once, so the renderer does not have to wait out its
            // hang time to find out.
            const HRESULT Removed = H.Device->GetDeviceRemovedReason();
            if (FAILED(Removed))
            {
                char Line[96];
                sprintf_s(Line, "host: its device was removed (0x%08lx)", (unsigned long)Removed);
                Fail(Line);
                break;
            }
        }
        if (GBlock->HostState == Host64::HostState_Failed)
            break;
    }
    H.WaitIdle();
    Fsr.Shutdown();
    Dlss.Shutdown();
    Xess.Shutdown();
    Rays.Shutdown();
    H.Shutdown();
    if (GBlock->HostState != Host64::HostState_Failed)
        GBlock->HostState = Host64::HostState_Exited;
    ::UnmapViewOfFile(GBlock);
    ::CloseHandle(Parent);
    Log("host: done");
    if (GLog)
        fclose(GLog);
    return 0;
}

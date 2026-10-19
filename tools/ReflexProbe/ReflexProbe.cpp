// ReflexProbe - a small standalone tool: does NVIDIA Reflex work in a 32-bit Direct3D 12 process, and what does it do
// to the delay between a frame's start and the GPU's end of that frame?
//
// The game the renderer draws for is a 32-bit process, and the renderer's AntiLag key runs NVIDIA Reflex on NVIDIA's
// cards through the very code this tool runs (src/RevisionDX12/Reflex.h) - AMD's Anti-Lag 2 on AMD's
// (src/RevisionDX12/AntiLag2.h), which this tool runs there in Reflex's place. It can be carried to a machine the game
// cannot be carried to: it needs nothing installed.
//
// What it does: a borderless window over the whole main screen, as the game runs, with a GPU-bound load (the GPU's
// share of a frame set to about 12 ms, the CPU's to 1.5 ms - the case where frames queue up before the GPU and the
// driver's help shows), presented as the renderer presents: two buffers, and before each frame the wait for the GPU
// to be through the last frame drawn into the same buffer - so the CPU runs at most one frame ahead, as in the game.
// In phases: the load measured and set, then off, on, on with the boost (Reflex), off again. Each frame's delay is
// measured the same way in every
// phase and on every card: from the frame's start - just after the driver's sleep, where a game reads its input - to
// the moment the GPU finished the frame's work (a timestamp, brought onto the CPU's clock by the queue's calibration).
// The screen's own delay after that is in none of the numbers. Where Reflex runs, NVIDIA's own report of each frame is
// written down too.
//
// It writes beside itself: ReflexProbe-<date>-<time>.txt (the summary), ReflexProbe-<date>-<time>.csv (every frame),
// and with Reflex ReflexProbe-<date>-<time>-driver.csv (the driver's report of every frame it gave).
//
// Run it, leave the machine alone for about a minute, then send the files back. Esc stops it early.
//
//   ReflexProbe.exe [--seconds N] [--gpu-ms M] [--buffers B] [--windowed] [--no-wait]
//     --seconds N   each phase's length (12 by default; the first 2 s of each are not counted)
//     --gpu-ms M    the GPU's share of a frame to aim at (12 by default)
//     --cpu-ms C    the CPU's own share of a frame, the game's simulation (1.5 by default)
//     --buffers B   the swap chain's buffers, 2 (the renderer's, by default) or 3
//     --windowed    a 1280 x 720 window instead of the whole screen (the system's compositor then paces it)
//     --no-help     neither Reflex nor Anti-Lag 2 asked for: the driver's own way, off only
//     --no-wait     close at the end without waiting for Enter

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <vector>
#include <algorithm>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include "Reflex.h"     // the renderer's own Reflex calls - the very same code
#include "AntiLag2.h"   // the renderer's own Anti-Lag 2 - the very same code

// ---------------------------------------------------------------------------------------------------------------------
// The output: the console and the summary file, line by line.
// ---------------------------------------------------------------------------------------------------------------------

static FILE* GLog;

static void Say(const char* Fmt, ...)
{
    char    Line[2048];
    va_list Args;
    va_start(Args, Fmt);
    _vsnprintf_s(Line, sizeof(Line), _TRUNCATE, Fmt, Args);
    va_end(Args);
    printf("%s\n", Line);
    if (GLog)
    {
        fputs(Line, GLog);
        fputc('\n', GLog);
        fflush(GLog);
    }
}

// NVAPI's status as words: the library's own text where it is loaded, a short table where it is not.
static const char* NvStatusText(NvAPI_Status S, bool bLoaded, char* Buf, int Chars)
{
    if (bLoaded)
    {
        NvAPI_ShortString T = {};
        if (NvAPI_GetErrorMessage(S, T) == NVAPI_OK)
        {
            _snprintf_s(Buf, Chars, _TRUNCATE, "%d, %s", (int)S, T);
            return Buf;
        }
    }
    const char* W = (S == NVAPI_OK)                        ? "OK"
                    : (S == NVAPI_LIBRARY_NOT_FOUND)       ? "no NVIDIA driver library here"
                    : (S == NVAPI_NVIDIA_DEVICE_NOT_FOUND) ? "no NVIDIA card here"
                    : (S == NVAPI_NOT_SUPPORTED)           ? "not supported"
                    : (S == NVAPI_INVALID_ARGUMENT)        ? "the driver refused the device"
                    : (S == NVAPI_NO_IMPLEMENTATION)       ? "not implemented"
                    : (S == NVAPI_API_NOT_INITIALIZED)     ? "NVAPI not started"
                                                           : "error";
    _snprintf_s(Buf, Chars, _TRUNCATE, "%d, %s", (int)S, W);
    return Buf;
}

// ---------------------------------------------------------------------------------------------------------------------
// The clock.
// ---------------------------------------------------------------------------------------------------------------------

static double GQpcFreq;

static LONGLONG Qpc()
{
    LARGE_INTEGER Q;
    ::QueryPerformanceCounter(&Q);
    return Q.QuadPart;
}

static double Ms(LONGLONG Ticks)
{
    return (double)Ticks * 1000.0 / GQpcFreq;
}

// ---------------------------------------------------------------------------------------------------------------------
// The window.
// ---------------------------------------------------------------------------------------------------------------------

static UINT Width = 1280, Height = 720;
static bool GStop;

static LRESULT CALLBACK WindowProc(HWND W, UINT Msg, WPARAM WP, LPARAM LP)
{
    switch (Msg)
    {
    case WM_CLOSE:
        GStop = true;
        return 0;
    case WM_KEYDOWN:
        if (WP == VK_ESCAPE)
            GStop = true;
        return 0;
    }
    return ::DefWindowProcW(W, Msg, WP, LP);
}

// Borderless over the whole main screen, as the game runs (the system then lets the picture go to the screen without
// its compositor), or a plain 1280 x 720 window.
static HWND MakeWindow(bool bWindowed)
{
    WNDCLASSEXW C   = {};
    C.cbSize        = sizeof(C);
    C.lpfnWndProc   = WindowProc;
    C.hInstance     = ::GetModuleHandleW(nullptr);
    C.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    C.lpszClassName = L"ReflexProbe";
    ::RegisterClassExW(&C);
    RECT  R     = { 0, 0, (LONG)Width, (LONG)Height };
    DWORD Style = WS_OVERLAPPEDWINDOW;
    int   X = CW_USEDEFAULT, Y = CW_USEDEFAULT;
    if (bWindowed)
        ::AdjustWindowRect(&R, Style, FALSE);
    else
    {
        MONITORINFO M = {};
        M.cbSize      = sizeof(M);
        const POINT Origin = { 0, 0 };
        ::GetMonitorInfoW(::MonitorFromPoint(Origin, MONITOR_DEFAULTTOPRIMARY), &M);
        R      = M.rcMonitor;
        Style  = WS_POPUP;
        X      = R.left;
        Y      = R.top;
        Width  = (UINT)(R.right - R.left);
        Height = (UINT)(R.bottom - R.top);
    }
    HWND W = ::CreateWindowExW(0, L"ReflexProbe", L"ReflexProbe - leave this window alone, Esc stops it", Style, X, Y,
                               R.right - R.left, R.bottom - R.top, nullptr, nullptr, C.hInstance, nullptr);
    if (W)
    {
        ::ShowWindow(W, SW_SHOW);
        ::SetForegroundWindow(W);
    }
    return W;
}

static void PumpMessages()
{
    MSG M;
    while (::PeekMessageW(&M, nullptr, 0, 0, PM_REMOVE))
    {
        ::TranslateMessage(&M);
        ::DispatchMessageW(&M);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Direct3D 12: the device on the strongest card, a swap chain that may tear, and one full-screen draw whose pixels
// each run a loop of a set length - the load.
// ---------------------------------------------------------------------------------------------------------------------

static const UINT MaxBuffers = 3;
static UINT       Buffers    = 2;   // the renderer's two (--buffers)
static const UINT Ring    = 16;     // the timestamps and command allocators kept: far past the frames in flight

static const char* const GShader = R"(
cbuffer Load : register(b0) { uint Iterations; float Time; float Pad0; float Pad1; };
float4 VSMain(uint Id : SV_VertexID) : SV_Position
{
    float2 Uv = float2((Id << 1) & 2, Id & 2);
    return float4(Uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
float4 PSMain(float4 Pos : SV_Position) : SV_Target
{
    float A = Pos.x * 0.001f, B = Pos.y * 0.001f;
    [loop] for (uint i = 0; i < Iterations; i++)
    {
        A = sin(A * 1.7f + B) * 0.9f + 0.1f;
        B = cos(B * 1.3f - A) * 0.9f + 0.1f;
    }
    return float4(frac(A * 0.5f + Time), frac(B * 0.5f), 0.4f, 1.0f);
}
)";

struct FGpu
{
    IDXGIFactory4*             Factory   = nullptr;
    IDXGIAdapter1*             Adapter   = nullptr;
    ID3D12Device*              Device    = nullptr;
    ID3D12CommandQueue*        Queue     = nullptr;
    IDXGISwapChain3*           Chain     = nullptr;
    ID3D12DescriptorHeap*      RtvHeap   = nullptr;
    UINT                       RtvStep   = 0;
    ID3D12Resource*            Back[MaxBuffers] = {};
    ID3D12CommandAllocator*    Alloc[Ring]   = {};
    ID3D12GraphicsCommandList* List      = nullptr;
    ID3D12RootSignature*       Root      = nullptr;
    ID3D12PipelineState*       Pso       = nullptr;
    ID3D12QueryHeap*           Queries   = nullptr;
    ID3D12Resource*            Readback  = nullptr;
    ID3D12Fence*               Fence     = nullptr;
    HANDLE                     Event     = nullptr;
    UINT64                     FenceValue = 0;
    UINT64                     BufferFence[MaxBuffers] = {};   // the last frame drawn into each buffer
    UINT64                     GpuFreq   = 0;
    bool                       bTearing  = false;
    char                       Name[128] = {};
    UINT                       VramMb    = 0;
    UINT                       VendorId  = 0;
};

template <class T> static void Drop(T*& P)
{
    if (P)
    {
        P->Release();
        P = nullptr;
    }
}

static bool Fail(const char* What, HRESULT Hr)
{
    Say("FAILED: %s (0x%08X)", What, (unsigned)Hr);
    return false;
}

static bool StartGpu(FGpu& G, HWND Window)
{
    HRESULT Hr = ::CreateDXGIFactory2(0, IID_PPV_ARGS(&G.Factory));
    if (FAILED(Hr))
        return Fail("CreateDXGIFactory2", Hr);
    // The strongest card: on a laptop with two, the one a game would be given.
    IDXGIFactory6* F6 = nullptr;
    if (SUCCEEDED(G.Factory->QueryInterface(IID_PPV_ARGS(&F6))))
    {
        F6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&G.Adapter));
        F6->Release();
    }
    if (!G.Adapter)
        G.Factory->EnumAdapters1(0, &G.Adapter);
    if (!G.Adapter)
        return Fail("no graphics card", E_FAIL);
    DXGI_ADAPTER_DESC1 D = {};
    G.Adapter->GetDesc1(&D);
    ::WideCharToMultiByte(CP_ACP, 0, D.Description, -1, G.Name, sizeof(G.Name), nullptr, nullptr);
    G.VramMb   = (UINT)(D.DedicatedVideoMemory / (1024 * 1024));
    G.VendorId = D.VendorId;

    Hr = ::D3D12CreateDevice(G.Adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&G.Device));
    if (FAILED(Hr))
        return Fail("D3D12CreateDevice", Hr);
    D3D12_COMMAND_QUEUE_DESC Q = {};
    Q.Type                     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Hr = G.Device->CreateCommandQueue(&Q, IID_PPV_ARGS(&G.Queue));
    if (FAILED(Hr))
        return Fail("CreateCommandQueue", Hr);
    G.Queue->GetTimestampFrequency(&G.GpuFreq);

    IDXGIFactory5* F5 = nullptr;
    if (SUCCEEDED(G.Factory->QueryInterface(IID_PPV_ARGS(&F5))))
    {
        BOOL bAllow = FALSE;
        if (SUCCEEDED(F5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &bAllow, sizeof(bAllow))))
            G.bTearing = bAllow != FALSE;
        F5->Release();
    }
    DXGI_SWAP_CHAIN_DESC1 S = {};
    S.Width                 = Width;
    S.Height                = Height;
    S.Format                = DXGI_FORMAT_R8G8B8A8_UNORM;
    S.SampleDesc.Count      = 1;
    S.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    S.BufferCount           = Buffers;
    S.SwapEffect            = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    S.Flags                 = G.bTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* C1     = nullptr;
    Hr = G.Factory->CreateSwapChainForHwnd(G.Queue, Window, &S, nullptr, nullptr, &C1);
    if (FAILED(Hr))
        return Fail("CreateSwapChainForHwnd", Hr);
    G.Factory->MakeWindowAssociation(Window, DXGI_MWA_NO_ALT_ENTER);
    Hr = C1->QueryInterface(IID_PPV_ARGS(&G.Chain));
    C1->Release();
    if (FAILED(Hr))
        return Fail("IDXGISwapChain3", Hr);

    D3D12_DESCRIPTOR_HEAP_DESC H = {};
    H.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    H.NumDescriptors             = Buffers;
    Hr = G.Device->CreateDescriptorHeap(&H, IID_PPV_ARGS(&G.RtvHeap));
    if (FAILED(Hr))
        return Fail("CreateDescriptorHeap", Hr);
    G.RtvStep = G.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < Buffers; i++)
    {
        Hr = G.Chain->GetBuffer(i, IID_PPV_ARGS(&G.Back[i]));
        if (FAILED(Hr))
            return Fail("GetBuffer", Hr);
        D3D12_CPU_DESCRIPTOR_HANDLE R = G.RtvHeap->GetCPUDescriptorHandleForHeapStart();
        R.ptr += (SIZE_T)i * G.RtvStep;
        G.Device->CreateRenderTargetView(G.Back[i], nullptr, R);
    }
    for (UINT i = 0; i < Ring; i++)
    {
        Hr = G.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&G.Alloc[i]));
        if (FAILED(Hr))
            return Fail("CreateCommandAllocator", Hr);
    }
    Hr = G.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, G.Alloc[0], nullptr, IID_PPV_ARGS(&G.List));
    if (FAILED(Hr))
        return Fail("CreateCommandList", Hr);
    G.List->Close();

    // The root signature: the load's four numbers, nothing else.
    D3D12_ROOT_PARAMETER P      = {};
    P.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P.Constants.ShaderRegister  = 0;
    P.Constants.Num32BitValues  = 4;
    P.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC RD = {};
    RD.NumParameters            = 1;
    RD.pParameters              = &P;
    ID3DBlob* Sig = nullptr;
    ID3DBlob* Err = nullptr;
    Hr = ::D3D12SerializeRootSignature(&RD, D3D_ROOT_SIGNATURE_VERSION_1, &Sig, &Err);
    if (FAILED(Hr))
        return Fail("D3D12SerializeRootSignature", Hr);
    Hr = G.Device->CreateRootSignature(0, Sig->GetBufferPointer(), Sig->GetBufferSize(), IID_PPV_ARGS(&G.Root));
    Drop(Sig);
    Drop(Err);
    if (FAILED(Hr))
        return Fail("CreateRootSignature", Hr);

    ID3DBlob* Vs = nullptr;
    ID3DBlob* Ps = nullptr;
    Hr = ::D3DCompile(GShader, strlen(GShader), "ReflexProbe", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &Vs, &Err);
    if (FAILED(Hr))
    {
        Say("the vertex shader: %s", Err ? (const char*)Err->GetBufferPointer() : "");
        return Fail("D3DCompile", Hr);
    }
    Hr = ::D3DCompile(GShader, strlen(GShader), "ReflexProbe", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &Ps, &Err);
    if (FAILED(Hr))
    {
        Say("the pixel shader: %s", Err ? (const char*)Err->GetBufferPointer() : "");
        return Fail("D3DCompile", Hr);
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC PD = {};
    PD.pRootSignature                     = G.Root;
    PD.VS                                 = { Vs->GetBufferPointer(), Vs->GetBufferSize() };
    PD.PS                                 = { Ps->GetBufferPointer(), Ps->GetBufferSize() };
    D3D12_RENDER_TARGET_BLEND_DESC& Blend = PD.BlendState.RenderTarget[0];
    Blend.SrcBlend                        = D3D12_BLEND_ONE;
    Blend.DestBlend                       = D3D12_BLEND_ZERO;
    Blend.BlendOp                         = D3D12_BLEND_OP_ADD;
    Blend.SrcBlendAlpha                   = D3D12_BLEND_ONE;
    Blend.DestBlendAlpha                  = D3D12_BLEND_ZERO;
    Blend.BlendOpAlpha                    = D3D12_BLEND_OP_ADD;
    Blend.LogicOp                         = D3D12_LOGIC_OP_NOOP;
    Blend.RenderTargetWriteMask           = D3D12_COLOR_WRITE_ENABLE_ALL;
    PD.SampleMask                         = 0xFFFFFFFFu;
    PD.RasterizerState.FillMode           = D3D12_FILL_MODE_SOLID;
    PD.RasterizerState.CullMode           = D3D12_CULL_MODE_NONE;
    PD.RasterizerState.DepthClipEnable    = TRUE;
    PD.PrimitiveTopologyType              = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PD.NumRenderTargets                   = 1;
    PD.RTVFormats[0]                      = DXGI_FORMAT_R8G8B8A8_UNORM;
    PD.SampleDesc.Count                   = 1;
    Hr = G.Device->CreateGraphicsPipelineState(&PD, IID_PPV_ARGS(&G.Pso));
    Drop(Vs);
    Drop(Ps);
    Drop(Err);
    if (FAILED(Hr))
        return Fail("CreateGraphicsPipelineState", Hr);

    // Two timestamps a frame - its work's start and end - and where they are read back.
    D3D12_QUERY_HEAP_DESC QH = {};
    QH.Type                  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    QH.Count                 = Ring * 2;
    Hr = G.Device->CreateQueryHeap(&QH, IID_PPV_ARGS(&G.Queries));
    if (FAILED(Hr))
        return Fail("CreateQueryHeap", Hr);
    D3D12_HEAP_PROPERTIES HP = {};
    HP.Type                  = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC B    = {};
    B.Dimension              = D3D12_RESOURCE_DIMENSION_BUFFER;
    B.Width                  = Ring * 2 * sizeof(UINT64);
    B.Height                 = 1;
    B.DepthOrArraySize       = 1;
    B.MipLevels              = 1;
    B.SampleDesc.Count       = 1;
    B.Layout                 = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Hr = G.Device->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_NONE, &B, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_PPV_ARGS(&G.Readback));
    if (FAILED(Hr))
        return Fail("the readback buffer", Hr);
    Hr = G.Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&G.Fence));
    if (FAILED(Hr))
        return Fail("CreateFence", Hr);
    G.Event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

static void WaitFence(FGpu& G, UINT64 Value)
{
    if (G.Fence->GetCompletedValue() >= Value)
        return;
    G.Fence->SetEventOnCompletion(Value, G.Event);
    ::WaitForSingleObject(G.Event, 5000);
}

static void StopGpu(FGpu& G)
{
    if (G.Queue && G.Fence)
    {
        G.Queue->Signal(G.Fence, ++G.FenceValue);
        WaitFence(G, G.FenceValue);
    }
    Drop(G.Readback);
    Drop(G.Queries);
    Drop(G.Pso);
    Drop(G.Root);
    Drop(G.List);
    for (UINT i = 0; i < Ring; i++)
        Drop(G.Alloc[i]);
    for (UINT i = 0; i < Buffers; i++)
        Drop(G.Back[i]);
    Drop(G.RtvHeap);
    Drop(G.Chain);
    Drop(G.Fence);
    Drop(G.Queue);
    Drop(G.Device);
    Drop(G.Adapter);
    Drop(G.Factory);
    if (G.Event)
        ::CloseHandle(G.Event);
    G.Event = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// The measurement.
// ---------------------------------------------------------------------------------------------------------------------

enum EMode { Mode_Off, Mode_On, Mode_Boost };

struct FPhase
{
    const char* Name;
    EMode       Mode;
    double      Seconds;
    // Filled in at its end.
    NvAPI_Status            StatusHr;
    NV_GET_SLEEP_STATUS_PARAMS Status;
};

struct FFrame
{
    int      Phase;
    UINT     Iterations;
    LONGLONG Start;       // the frame's start: just after the driver's sleep
    LONGLONG SleepTicks;  // in the driver's sleep
    LONGLONG CpuTicks;    // from the start to the Present's return
    LONGLONG PresentTicks; // in the Present itself
    LONGLONG WaitTicks;   // after it, for the GPU to be through the last frame drawn into the next buffer
    UINT64   Fence;
    NvU64    ReflexId;
    double   GpuMs;       // the frame's work on the GPU, from its two timestamps
    double   DelayMs;     // from the start to the GPU's end of the frame's work
    bool     bDone;
};

struct FDriverFrame
{
    int    Phase;
    NvU64  Id;
    double SimUs, SubmitUs, PresentUs, DriverUs, QueueUs, GpuUs, TotalUs;
    NvU32  GpuActiveUs, GpuFrameUs;
};

// The CPU's clock against the GPU's, taken again every quarter second: a GPU timestamp onto the CPU's clock.
struct FCalibration
{
    UINT64 Gpu = 0, Cpu = 0;
};

static double QpcOfGpu(const FCalibration& C, UINT64 GpuFreq, UINT64 T)
{
    const double D = (double)(INT64)(T - C.Gpu);
    return (double)C.Cpu + D / (double)GpuFreq * GQpcFreq;
}

static double Percentile(std::vector<double> V, double P)
{
    if (V.empty())
        return 0.0;
    std::sort(V.begin(), V.end());
    const double I = P * (double)(V.size() - 1);
    const size_t A = (size_t)I;
    const size_t B = (A + 1 < V.size()) ? A + 1 : A;
    return V[A] + (V[B] - V[A]) * (I - (double)A);
}

int main(int Argc, char** Argv)
{
    double PhaseSeconds = 12.0, TargetGpuMs = 12.0, SimMs = 1.5;
    bool   bNoWait      = false;
    bool   bWindowed    = false;
    bool   bNoHelp      = false;
    for (int i = 1; i < Argc; i++)
    {
        if (!strcmp(Argv[i], "--seconds") && i + 1 < Argc)
            PhaseSeconds = atof(Argv[++i]);
        else if (!strcmp(Argv[i], "--gpu-ms") && i + 1 < Argc)
            TargetGpuMs = atof(Argv[++i]);
        else if (!strcmp(Argv[i], "--cpu-ms") && i + 1 < Argc)
            SimMs = atof(Argv[++i]);
        else if (!strcmp(Argv[i], "--no-wait"))
            bNoWait = true;
        else if (!strcmp(Argv[i], "--windowed"))
            bWindowed = true;
        else if (!strcmp(Argv[i], "--no-help"))
            bNoHelp = true;
        else if (!strcmp(Argv[i], "--buffers") && i + 1 < Argc)
            Buffers = (atoi(Argv[++i]) >= 3) ? 3u : 2u;
    }
    PhaseSeconds = (PhaseSeconds < 4.0) ? 4.0 : (PhaseSeconds > 120.0 ? 120.0 : PhaseSeconds);
    TargetGpuMs  = (TargetGpuMs < 2.0) ? 2.0 : (TargetGpuMs > 40.0 ? 40.0 : TargetGpuMs);
    SimMs = (SimMs < 0.0) ? 0.0 : (SimMs > 30.0 ? 30.0 : SimMs);
    const double SettleSeconds = 2.0;

    LARGE_INTEGER Fq;
    ::QueryPerformanceFrequency(&Fq);
    GQpcFreq = (double)Fq.QuadPart;

    // The screen's own pixels, as the game asks for them: without this a scaled screen is reported smaller, the
    // picture is stretched by the system's compositor, and that paces the presents instead of the driver.
    {
        typedef BOOL(WINAPI * PFNSetDpiContext)(HANDLE);
        HMODULE          User = ::GetModuleHandleW(L"user32.dll");
        PFNSetDpiContext Set  = User ? (PFNSetDpiContext)::GetProcAddress(User, "SetProcessDpiAwarenessContext") : nullptr;
        if (Set)
            Set((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }

    // The files beside the exe, named by the moment.
    char Dir[MAX_PATH] = {};
    ::GetModuleFileNameA(nullptr, Dir, MAX_PATH);
    if (char* Slash = strrchr(Dir, '\\'))
        Slash[1] = 0;
    SYSTEMTIME T;
    ::GetLocalTime(&T);
    char Stem[MAX_PATH + 64];
    _snprintf_s(Stem, sizeof(Stem), _TRUNCATE, "%sReflexProbe-%04u%02u%02u-%02u%02u%02u", Dir, T.wYear, T.wMonth, T.wDay,
                T.wHour, T.wMinute, T.wSecond);
    char TxtPath[MAX_PATH + 96], CsvPath[MAX_PATH + 96], DrvPath[MAX_PATH + 96];
    _snprintf_s(TxtPath, sizeof(TxtPath), _TRUNCATE, "%s.txt", Stem);
    _snprintf_s(CsvPath, sizeof(CsvPath), _TRUNCATE, "%s.csv", Stem);
    _snprintf_s(DrvPath, sizeof(DrvPath), _TRUNCATE, "%s-driver.csv", Stem);
    fopen_s(&GLog, TxtPath, "w");

    Say("ReflexProbe - %04u-%02u-%02u %02u:%02u:%02u", T.wYear, T.wMonth, T.wDay, T.wHour, T.wMinute, T.wSecond);
    Say("The process: %d-bit", (int)(sizeof(void*) * 8));
    {
        typedef LONG(WINAPI * PFNRtlGetVersion)(OSVERSIONINFOW*);
        OSVERSIONINFOW V = {};
        V.dwOSVersionInfoSize = sizeof(V);
        HMODULE Nt = ::GetModuleHandleW(L"ntdll.dll");
        PFNRtlGetVersion Get = Nt ? (PFNRtlGetVersion)::GetProcAddress(Nt, "RtlGetVersion") : nullptr;
        if (Get && Get(&V) == 0)
            Say("Windows %lu.%lu.%lu", V.dwMajorVersion, V.dwMinorVersion, V.dwBuildNumber);
    }

    HWND Window = MakeWindow(bWindowed);
    if (!Window)
    {
        Say("FAILED: the window");
        return 1;
    }
    PumpMessages();
    FGpu G;
    if (!StartGpu(G, Window))
    {
        StopGpu(G);
        if (!bNoWait)
        {
            printf("\nPress Enter to close.\n");
            getchar();
        }
        return 1;
    }
    Say("The card: %s (%u MB, vendor 0x%04X)", G.Name, G.VramMb, G.VendorId);
    Say("The window: %u x %u, %s; tearing presents allowed: %s; %u buffers, the CPU at most %u frame(s) ahead", Width,
        Height, bWindowed ? "a window" : "borderless over the main screen", G.bTearing ? "yes" : "no", Buffers,
        Buffers - 1);

    // The driver's help, asked for as the renderer asks: AMD's Anti-Lag 2 first, NVIDIA's Reflex where it is not.
    // --no-help: neither asked for, to see the driver's own way without them.
    RdxAntiLag2::Context AntiLag;
    const HRESULT        AmdHr = bNoHelp ? E_ABORT : RdxAntiLag2::Initialize(&AntiLag, G.Device);
    const bool           bAmd  = (AmdHr == S_OK);
    RdxReflex::FState    Reflex;
    if (!bAmd && !bNoHelp)
        RdxReflex::Start(Reflex, G.Device);
    const bool bReflex = Reflex.bUp;
    char       B1[96], B2[96];
    if (Reflex.bNvapi)
    {
        NvU32             Driver = 0;
        NvAPI_ShortString Branch = {};
        if (NvAPI_SYS_GetDriverAndBranchVersion(&Driver, Branch) == NVAPI_OK)
            Say("The NVIDIA driver: %u.%02u (%s)", Driver / 100, Driver % 100, Branch);
    }
    if (bAmd)
        Say("NVIDIA Reflex: not asked - AMD's Anti-Lag 2 is here");
    else
        Say("NVIDIA Reflex: NvAPI_Initialize %s; NvAPI_D3D_SetSleepMode %s - %s",
            NvStatusText(Reflex.StartStatus, false, B1, sizeof(B1)),
            Reflex.bNvapi ? NvStatusText(Reflex.ModeStatus, true, B2, sizeof(B2)) : "not asked",
            bReflex ? "REFLEX RUNS IN THIS 32-BIT PROCESS" : "Reflex does not run here");
    Say("AMD Anti-Lag 2: 0x%08X (the interface 0x%08X, its state 0x%08X, in the 64-bit layout 0x%08X%s) - %s",
        (unsigned)AmdHr, (unsigned)AntiLag.m_createHr, (unsigned)AntiLag.m_stateHr, (unsigned)AntiLag.m_wideHr,
        AntiLag.m_wide ? ", which it took" : "", bAmd ? "ANTI-LAG 2 RUNS IN THIS 32-BIT PROCESS" : "not here");

    // The phases. The first sets the load and is not counted.
    std::vector<FPhase> Phases;
    Phases.push_back({ "load", Mode_Off, 3.0 });
    Phases.push_back({ "off", Mode_Off, PhaseSeconds });
    if (bReflex || bAmd)
        Phases.push_back({ bReflex ? "on" : "on (Anti-Lag 2)", Mode_On, PhaseSeconds });
    if (bReflex)
        Phases.push_back({ "on+boost", Mode_Boost, PhaseSeconds });
    if (bReflex || bAmd)
        Phases.push_back({ "off again", Mode_Off, PhaseSeconds });
    double RunSeconds = 0.0;
    for (const FPhase& P : Phases)
        RunSeconds += P.Seconds;
    Say("The run: %d phases, %.0f s in all - leave the window alone", (int)Phases.size(), RunSeconds);

    std::vector<FFrame>        Frames;
    std::vector<FDriverFrame>  Driver;
    std::vector<int>           PhaseOfId;      // NVIDIA's frame number -> the phase
    std::vector<unsigned char> SeenId;         // the driver's reports already taken
    Frames.reserve(40000);
    const UINT   MaxIterations = 400000;
    UINT         Iterations    = 16;
    FCalibration Cal;
    G.Queue->GetClockCalibration(&Cal.Gpu, &Cal.Cpu);
    LONGLONG     CalAt    = Qpc();
    LONGLONG     ReportAt = CalAt;
    size_t       PhaseIx  = 0;
    LONGLONG     PhaseAt  = Qpc();
    const LONGLONG RunAt  = PhaseAt;
    LONGLONG     SimTicks = (LONGLONG)(SimMs * GQpcFreq / 1000.0);

    // A frame's result taken in: its timestamps, its delay - and while the load is set, the next frames' loop length.
    auto Collect = [&](size_t i) {
        FFrame& F = Frames[i];
        if (F.bDone)
            return;
        WaitFence(G, F.Fence);
        const UINT  Slot = (UINT)(i % Ring);
        D3D12_RANGE R    = { Slot * 2 * sizeof(UINT64), (Slot + 1) * 2 * sizeof(UINT64) };
        void*       P    = nullptr;
        if (SUCCEEDED(G.Readback->Map(0, &R, &P)) && P)
        {
            const UINT64* Ts = (const UINT64*)((const char*)P + R.Begin);
            const UINT64  T0 = Ts[0], T1 = Ts[1];
            D3D12_RANGE   W  = { 0, 0 };
            G.Readback->Unmap(0, &W);
            if (T1 > T0 && G.GpuFreq)
            {
                F.GpuMs   = (double)(T1 - T0) * 1000.0 / (double)G.GpuFreq;
                F.DelayMs = (QpcOfGpu(Cal, G.GpuFreq, T1) - (double)F.Start) * 1000.0 / GQpcFreq;
            }
        }
        F.bDone = true;
        if (F.Phase == 0 && F.GpuMs > 0.0)
        {
            double Want = (double)F.Iterations * TargetGpuMs / F.GpuMs;
            Want        = std::min(std::max(Want, F.Iterations / 4.0), F.Iterations * 4.0);
            Iterations  = (UINT)std::min(std::max(Want, 8.0), (double)MaxIterations);
        }
    };

    // NVIDIA's own report of the last 64 frames, the new ones kept.
    auto TakeDriverReport = [&]() {
        if (!bReflex)
            return;
        static NV_LATENCY_RESULT_PARAMS L;
        if (RdxReflex::Latency(Reflex, L) != NVAPI_OK)
            return;
        for (int k = 0; k < 64; k++)
        {
            const NV_LATENCY_RESULT_PARAMS::FrameReport& R = L.frameReport[k];
            if (!R.frameID || !R.simStartTime || !R.gpuRenderEndTime || R.frameID >= (NvU64)PhaseOfId.size())
                continue;
            if (SeenId.size() < PhaseOfId.size())
                SeenId.resize(PhaseOfId.size(), 0);
            if (SeenId[(size_t)R.frameID])
                continue;
            SeenId[(size_t)R.frameID] = 1;
            FDriverFrame D;
            D.Phase       = PhaseOfId[(size_t)R.frameID];
            D.Id          = R.frameID;
            D.SimUs       = (double)(INT64)(R.simEndTime - R.simStartTime);
            D.SubmitUs    = (double)(INT64)(R.renderSubmitEndTime - R.renderSubmitStartTime);
            D.PresentUs   = (double)(INT64)(R.presentEndTime - R.presentStartTime);
            D.DriverUs    = (double)(INT64)(R.driverEndTime - R.driverStartTime);
            D.QueueUs     = (double)(INT64)(R.osRenderQueueEndTime - R.osRenderQueueStartTime);
            D.GpuUs       = (double)(INT64)(R.gpuRenderEndTime - R.gpuRenderStartTime);
            D.TotalUs     = (double)(INT64)(R.gpuRenderEndTime - R.simStartTime);
            D.GpuActiveUs = R.gpuActiveRenderTimeUs;
            D.GpuFrameUs  = R.gpuFrameTimeUs;
            Driver.push_back(D);
        }
    };

    auto EnterPhase = [&](size_t Ix) {
        const FPhase& P = Phases[Ix];
        if (bReflex)
        {
            const NvAPI_Status R = RdxReflex::SetOn(Reflex, P.Mode != Mode_Off, P.Mode == Mode_Boost);
            if (R != NVAPI_OK)
                Say("  the driver would not take the sleep mode: %s", NvStatusText(R, true, B1, sizeof(B1)));
        }
        char Title[128];
        _snprintf_s(Title, sizeof(Title), _TRUNCATE, "ReflexProbe - %s - leave this window alone, Esc stops it",
                    P.Name);
        ::SetWindowTextA(Window, Title);
        Say("phase %d/%d: %s (%.0f s)%s", (int)Ix + 1, (int)Phases.size(), P.Name, P.Seconds,
            Ix == 0 ? " - the load is set to about the asked GPU time" : "");
    };
    auto LeavePhase = [&](size_t Ix) {
        FPhase& P = Phases[Ix];
        P.StatusHr = bReflex ? RdxReflex::Status(Reflex, P.Status) : NVAPI_NOT_SUPPORTED;
        if (Ix == 0)
            Say("  the load: %u steps a pixel", Iterations);
    };

    EnterPhase(0);
    while (!GStop)
    {
        PumpMessages();
        if (GStop)
            break;
        const LONGLONG Now = Qpc();
        if (Ms(Now - PhaseAt) >= Phases[PhaseIx].Seconds * 1000.0)
        {
            LeavePhase(PhaseIx);
            if (++PhaseIx >= Phases.size())
                break;
            PhaseAt = Now;
            EnterPhase(PhaseIx);
        }
        if (Ms(Now - CalAt) >= 250.0)
        {
            G.Queue->GetClockCalibration(&Cal.Gpu, &Cal.Cpu);
            CalAt = Now;
        }
        if (Ms(Now - ReportAt) >= 250.0)
        {
            TakeDriverReport();
            ReportAt = Now;
        }
        const FPhase& Ph = Phases[PhaseIx];

        // The frame's start: the driver's sleep (Reflex marks the simulation's start in it), then the moment a game
        // reads its input.
        const LONGLONG S0 = Qpc();
        if (bReflex)
            RdxReflex::Sleep(Reflex);
        if (bAmd)
            RdxAntiLag2::Update(&AntiLag, Ph.Mode != Mode_Off, 0);
        const LONGLONG Start = Qpc();
        // The game's simulation: a fixed stretch of the CPU's time.
        while (Qpc() - Start < SimTicks)
            ;
        if (bReflex)
        {
            RdxReflex::Mark(Reflex, SIMULATION_END);
            RdxReflex::Mark(Reflex, RENDERSUBMIT_START);
        }

        const size_t f    = Frames.size();
        const UINT   Slot = (UINT)(f % Ring);
        if (f >= Ring)
            Collect(f - Ring);
        FFrame F     = {};
        F.Phase      = (int)PhaseIx;
        F.Iterations = Iterations;
        F.Start      = Start;
        F.SleepTicks = Start - S0;
        F.ReflexId   = bReflex ? Reflex.Frame : 0;

        G.Alloc[Slot]->Reset();
        G.List->Reset(G.Alloc[Slot], G.Pso);
        const UINT Index = G.Chain->GetCurrentBackBufferIndex();
        G.List->EndQuery(G.Queries, D3D12_QUERY_TYPE_TIMESTAMP, Slot * 2);
        D3D12_RESOURCE_BARRIER Bar = {};
        Bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Bar.Transition.pResource   = G.Back[Index];
        Bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        Bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        G.List->ResourceBarrier(1, &Bar);
        D3D12_CPU_DESCRIPTOR_HANDLE Rtv = G.RtvHeap->GetCPUDescriptorHandleForHeapStart();
        Rtv.ptr += (SIZE_T)Index * G.RtvStep;
        G.List->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
        const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (float)Width, (float)Height, 0.0f, 1.0f };
        const D3D12_RECT     Sc = { 0, 0, (LONG)Width, (LONG)Height };
        G.List->RSSetViewports(1, &Vp);
        G.List->RSSetScissorRects(1, &Sc);
        G.List->SetGraphicsRootSignature(G.Root);
        const float Time    = (float)(Ms(Start - RunAt) * 0.0002);
        UINT        Load[4] = { Iterations, 0, 0, 0 };
        memcpy(&Load[1], &Time, sizeof(Time));
        G.List->SetGraphicsRoot32BitConstants(0, 4, Load, 0);
        G.List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        G.List->DrawInstanced(3, 1, 0, 0);
        Bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        G.List->ResourceBarrier(1, &Bar);
        G.List->EndQuery(G.Queries, D3D12_QUERY_TYPE_TIMESTAMP, Slot * 2 + 1);
        G.List->ResolveQueryData(G.Queries, D3D12_QUERY_TYPE_TIMESTAMP, Slot * 2, 2, G.Readback,
                                 Slot * 2 * sizeof(UINT64));
        G.List->Close();
        ID3D12CommandList* Lists[1] = { G.List };
        G.Queue->ExecuteCommandLists(1, Lists);

        // The frame's work is submitted, then its Present - each told to the driver as it asks.
        if (bReflex)
        {
            RdxReflex::Mark(Reflex, RENDERSUBMIT_END);
            RdxReflex::Mark(Reflex, PRESENT_START);
        }
        if (bAmd && Ph.Mode != Mode_Off)
            RdxAntiLag2::MarkEndOfFrameRendering(&AntiLag);
        const LONGLONG P0 = Qpc();
        G.Chain->Present(0, G.bTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        const LONGLONG P1 = Qpc();
        if (bReflex)
            RdxReflex::Mark(Reflex, PRESENT_END);
        F.PresentTicks = P1 - P0;
        F.CpuTicks     = P1 - Start;
        G.Queue->Signal(G.Fence, ++G.FenceValue);
        F.Fence              = G.FenceValue;
        G.BufferFence[Index] = G.FenceValue;
        // As the renderer does (FBackend::MoveToNextFrame): the next frame waits for the GPU to be through the last
        // frame drawn into its buffer.
        const LONGLONG W0 = Qpc();
        WaitFence(G, G.BufferFence[G.Chain->GetCurrentBackBufferIndex()]);
        F.WaitTicks = Qpc() - W0;
        Frames.push_back(F);
        if (bReflex)
        {
            if (PhaseOfId.size() <= (size_t)F.ReflexId)
                PhaseOfId.resize((size_t)F.ReflexId + 1024, -1);
            PhaseOfId[(size_t)F.ReflexId] = F.Phase;
        }
    }
    const bool bCut = GStop;
    if (bCut)
        Say("stopped early (Esc or the window closed) - what was measured so far is written");

    // Everything still on its way in, the driver's last report, then the driver's help off and the device gone.
    G.Queue->Signal(G.Fence, ++G.FenceValue);
    WaitFence(G, G.FenceValue);
    G.Queue->GetClockCalibration(&Cal.Gpu, &Cal.Cpu);
    for (size_t i = (Frames.size() > Ring) ? Frames.size() - Ring : 0; i < Frames.size(); i++)
        Collect(i);
    TakeDriverReport();
    RdxReflex::Stop(Reflex);
    RdxAntiLag2::DeInitialize(&AntiLag);
    ::ShowWindow(Window, SW_HIDE);

    // The summary: each counted phase past its first two seconds.
    Say("");
    Say("The load: %u steps a pixel, aimed at %.1f ms of the GPU a frame; the CPU's own share %.1f ms", Iterations,
        TargetGpuMs, SimMs);
    Say("The delay: from a frame's start (just after the driver's sleep, where a game reads its input) to the GPU's");
    Say("end of that frame's work. The screen's own delay after that is in no number here. Milliseconds.");
    Say("");
    Say("phase            frames    fps   interval      sleep   pres.  wait    gpu    delay                driver's delay");
    Say("                                 med   p90    med   p90    med    med    med    med    p90    p99    med    p90");
    for (size_t p = 1; p < Phases.size(); p++)
    {
        std::vector<double> Interval, Sleep, Pres, Wait, Gpu, Delay, Total;
        LONGLONG            First = 0, Last = 0, Prev = 0;
        LONGLONG            From  = 0;
        for (const FFrame& F : Frames)
            if (F.Phase == (int)p)
            {
                From = F.Start;
                break;
            }
        From += (LONGLONG)(SettleSeconds * GQpcFreq);
        size_t Count = 0;
        for (const FFrame& F : Frames)
        {
            if (F.Phase != (int)p || F.Start < From || !F.bDone)
                continue;
            if (!Count)
                First = F.Start;
            Last = F.Start;
            if (Prev)
                Interval.push_back(Ms(F.Start - Prev));
            Prev = F.Start;
            Sleep.push_back(Ms(F.SleepTicks));
            Wait.push_back(Ms(F.WaitTicks));
            Pres.push_back(Ms(F.PresentTicks));
            if (F.GpuMs > 0.0)
            {
                Gpu.push_back(F.GpuMs);
                Delay.push_back(F.DelayMs);
            }
            Count++;
        }
        for (const FDriverFrame& D : Driver)
            if (D.Phase == (int)p)
                Total.push_back(D.TotalUs / 1000.0);
        const double Fps = (Count > 1 && Last > First) ? (double)(Count - 1) * 1000.0 / Ms(Last - First) : 0.0;
        char         Drv[64];
        if (Total.empty())
            _snprintf_s(Drv, sizeof(Drv), _TRUNCATE, "     -      -");
        else
            _snprintf_s(Drv, sizeof(Drv), _TRUNCATE, "%6.1f %6.1f", Percentile(Total, 0.5), Percentile(Total, 0.9));
        Say("%-16s %6u %6.1f %5.1f %5.1f  %5.1f %5.1f  %5.1f  %5.1f  %5.1f  %5.1f  %5.1f  %5.1f %s", Phases[p].Name,
            (unsigned)Count, Fps, Percentile(Interval, 0.5), Percentile(Interval, 0.9), Percentile(Sleep, 0.5),
            Percentile(Sleep, 0.9), Percentile(Pres, 0.5), Percentile(Wait, 0.5), Percentile(Gpu, 0.5),
            Percentile(Delay, 0.5), Percentile(Delay, 0.9), Percentile(Delay, 0.99), Drv);
    }
    if (bReflex)
    {
        Say("");
        Say("The driver's own status at each phase's end (NvAPI_D3D_GetSleepStatus):");
        for (size_t p = 1; p < Phases.size(); p++)
        {
            const FPhase& P = Phases[p];
            if (P.StatusHr != NVAPI_OK)
            {
                Say("  %-16s %s", P.Name, NvStatusText(P.StatusHr, true, B1, sizeof(B1)));
                continue;
            }
            Say("  %-16s low latency mode %d, the game's sleep used %d, its sleep interval %u us, fullscreen flip %d, "
                "variable refresh %d, vsync forced in the control panel %d",
                P.Name, (int)P.Status.bLowLatencyMode, (int)P.Status.bUseGameSleep, (unsigned)P.Status.sleepIntervalUs,
                (int)P.Status.bFullscreenIFlip, (int)P.Status.bFsVrr, (int)P.Status.bCplVsyncOn);
        }
        Say("The driver's report: %u frames (the driver's delay above: from the simulation's start to the GPU's end).",
            (unsigned)Driver.size());
    }
    Say("Run time: %.1f s%s", Ms(Qpc() - RunAt) / 1000.0, bCut ? " (stopped early)" : "");

    // Every frame, and the driver's every frame.
    FILE* Csv = nullptr;
    if (fopen_s(&Csv, CsvPath, "w") == 0 && Csv)
    {
        fprintf(Csv, "phase,frame,start_ms,sleep_ms,cpu_ms,present_ms,wait_ms,gpu_ms,delay_ms,steps\n");
        for (size_t i = 0; i < Frames.size(); i++)
        {
            const FFrame& F = Frames[i];
            fprintf(Csv, "%s,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u\n", Phases[F.Phase].Name, (unsigned)i,
                    Ms(F.Start - RunAt), Ms(F.SleepTicks), Ms(F.CpuTicks), Ms(F.PresentTicks), Ms(F.WaitTicks),
                    F.GpuMs, F.DelayMs, F.Iterations);
        }
        fclose(Csv);
        Say("Every frame: %s", CsvPath);
    }
    if (bReflex && !Driver.empty())
    {
        FILE* Drv = nullptr;
        if (fopen_s(&Drv, DrvPath, "w") == 0 && Drv)
        {
            fprintf(Drv, "phase,frame_id,sim_us,submit_us,present_us,driver_us,os_queue_us,gpu_us,gpu_active_us,"
                         "gpu_frame_us,sim_start_to_gpu_end_us\n");
            for (const FDriverFrame& D : Driver)
                fprintf(Drv, "%s,%llu,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%u,%u,%.0f\n",
                        (D.Phase >= 0) ? Phases[D.Phase].Name : "?", (unsigned long long)D.Id, D.SimUs, D.SubmitUs,
                        D.PresentUs, D.DriverUs, D.QueueUs, D.GpuUs, (unsigned)D.GpuActiveUs, (unsigned)D.GpuFrameUs,
                        D.TotalUs);
            fclose(Drv);
            Say("The driver's frames: %s", DrvPath);
        }
    }
    Say("The summary: %s", TxtPath);

    StopGpu(G);
    ::DestroyWindow(Window);
    if (GLog)
        fclose(GLog);
    if (!bNoWait)
    {
        printf("\nDone - please send back the files above. Press Enter to close.\n");
        getchar();
    }
    return 0;
}

// Host64Probe - the 64-bit host's pipeline tried without the game (docs/terv-fsr4.md, M1 and M2).
//
// A 32-bit process like the renderer's: a device on the first hardware adapter, shared textures the size of a 4K frame,
// the host started from beside this probe's build, and then frames.
//
// First the Test backend: each frame the colour is cleared to a colour of the frame's own, handed over, copied by the
// host into the output, and a corner of the output is read back and checked - so a frame that went wrong, or a frame's
// picture that arrived a frame late, shows. Timed twice over: with the GPU otherwise idle (the host's wake-up is all on
// show), and with a frame's worth of GPU work in front of the hand-over (as in the game: the host's CPU work hides
// behind the GPU's). Each once more without the host - the copy done here - so the difference is what the crossing
// costs.
//
// Then the FSR backend, on the renderer's own formats at ten-bit output: the scene R10G10B10A2, the depth the depth plane
// of a D32S8 buffer copied into a shared R32F texture (the copy is checked too), the motion R16G16F, the upscaler's
// output RGBA16F. A flat colour, a flat depth and no motion go in, and once the upscaler has settled the same colour has
// to come out. The host says which FSR the card got.
//
// And frame generation, the way the renderer would run it: the manual dispatch, no swap chain anywhere, the finished
// picture and the generated one in the ten-bit back buffer's format, and a HUD-less copy of the picture so that the HUD
// - a white box here - is told apart. Between two frames of the same flat colour the generated one has to be that colour
// too, and the box white.
//
// Then the same two series with DLSS in FSR's place. On NVIDIA's card they have to pass as FSR's do; on any other the
// host has to refuse DLSS and say why, and that is what is tried.
//
// Last the rays (docs/terv-rt-host.md): what a card traces in the host where this 32-bit process may trace nothing. A
// small scene goes over the way the renderer's will - the level's triangles once, the bodies every frame, one of them
// moving, the tables and a mask - and the host's three passes run on it: the list - rays written down a slot each, as
// the renderer's pixel shaders write theirs, and what each met held against the same rays traced here on the CPU -
// and the renderer's own shadow factor and occlusion, held against what one light and the bodies must give on a flat
// floor. Then a scene the size of a level, for what a frame's rays cost on this card. What the list pass traces on to
// from what a ray met - the lamps' light there, the see-through surfaces on the ray's way - is not tried: it goes by
// the renderer's own tables of the level, which only the game makes.
//
//     Host64Probe.exe [frames] [--debug] [--rays]
//         --debug: the D3D12 debug layer here and in the host     --rays: the rays' part alone

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../../src/Host64/Host64Link.h"
// The light arrays' length in the shadow factor's constants: the renderer's own number.
#include "../../src/RevisionDX12/RenderLimits.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
    const UINT W    = 3840;
    const UINT H    = 2160;
    const UINT Box  = 16;          // the corner read back
    const int  Busy = 24;          // clears of the whole colour in front of a busy frame: a few milliseconds

    ID3D12Device*              Device     = nullptr;
    ID3D12CommandQueue*        Queue      = nullptr;
    ID3D12CommandAllocator*    Alloc[2]   = {};
    ID3D12GraphicsCommandList* List       = nullptr;
    ID3D12Fence*               Fence      = nullptr;
    UINT64                     FenceValue = 0;
    HANDLE                     FenceEvent = nullptr;
    bool                       bDebug     = false;
    UINT                       AdapterVendor = 0;     // the PCI vendor id of the adapter the device is on

    // The Test part's: the colour and the copy's output, RGBA16F.
    ID3D12Resource* Color  = nullptr;
    ID3D12Resource* Output = nullptr;
    // The FSR part's, in the renderer's formats.
    ID3D12Resource* SceneTen  = nullptr;   // R10G10B10A2, the scene
    ID3D12Resource* DepthBuf  = nullptr;   // D32S8, a depth buffer as the renderer's
    ID3D12Resource* DepthCopy = nullptr;   // R32F, its depth plane, shared
    ID3D12Resource* Motion    = nullptr;   // R16G16F
    ID3D12Resource* Upscaled  = nullptr;   // RGBA16F, written by the upscaler as unordered access
    // Frame generation's, in the ten-bit back buffer's format.
    ID3D12Resource* PresentTex = nullptr;  // the finished picture it generates from, with a HUD
    ID3D12Resource* HudLessTex = nullptr;  // the same without the HUD
    ID3D12Resource* Generated  = nullptr;  // the generated frame, written as unordered access
    ID3D12Resource* Readback  = nullptr;
    ID3D12DescriptorHeap* RtvHeap = nullptr;   // Color, SceneTen, Motion, PresentTex, HudLessTex
    ID3D12DescriptorHeap* DsvHeap = nullptr;   // DepthBuf
    UINT                  RtvStep = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Foot = {};
    LARGE_INTEGER Freq;
    SIZE_T        ReadbackBytes = 0;      // the readback buffer's size: its last row is shorter than the pitch

    void Print(const char* Line) { printf("  %s\n", Line); }

    // With the debug layer on: its errors and warnings, printed, and the queue emptied.
    int DrainDebug()
    {
        if (!bDebug)
            return 0;
        ID3D12InfoQueue* Q = nullptr;
        if (FAILED(Device->QueryInterface(IID_PPV_ARGS(&Q))))
            return 0;
        // Errors are what is counted; of the warnings - a clear without the creation's clear value, mostly - the first
        // few are enough to read.
        static int Warned = 0;
        int Errors = 0;
        const UINT64 N = Q->GetNumStoredMessages();
        for (UINT64 i = 0; i < N; i++)
        {
            SIZE_T Bytes = 0;
            Q->GetMessage(i, nullptr, &Bytes);
            D3D12_MESSAGE* M = (D3D12_MESSAGE*)malloc(Bytes);
            if (M && SUCCEEDED(Q->GetMessage(i, M, &Bytes)) && M->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                const bool bError = M->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
                if (bError ? (Errors < 20) : (Warned++ < 4))
                    printf("  d3d12 (%s): %s\n", bError ? "error" : "warning", M->pDescription);
                if (bError)
                    Errors++;
            }
            free(M);
        }
        Q->ClearStoredMessages();
        Q->Release();
        return Errors;
    }

    void Barrier(ID3D12Resource* R, D3D12_RESOURCE_STATES From, D3D12_RESOURCE_STATES To)
    {
        D3D12_RESOURCE_BARRIER B = {};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = R;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = From;
        B.Transition.StateAfter  = To;
        List->ResourceBarrier(1, &B);
    }

    void Open(int A)
    {
        Alloc[A]->Reset();
        List->Reset(Alloc[A], nullptr);
    }

    void Execute()
    {
        List->Close();
        ID3D12CommandList* L[1] = { List };
        Queue->ExecuteCommandLists(1, L);
    }

    void WaitGpu()
    {
        FenceValue++;
        Queue->Signal(Fence, FenceValue);
        if (Fence->GetCompletedValue() < FenceValue)
        {
            Fence->SetEventOnCompletion(FenceValue, FenceEvent);
            WaitForSingleObject(FenceEvent, INFINITE);
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(int i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE R = RtvHeap->GetCPUDescriptorHandleForHeapStart();
        R.ptr += (SIZE_T)i * RtvStep;
        return R;
    }

    // A colour of the frame's own: every frame differs from the three before it, in every channel.
    void FrameColor(int F, float Out[4])
    {
        Out[0] = (float)((F * 37) % 251) / 250.0f;
        Out[1] = (float)((F * 91) % 241) / 240.0f;
        Out[2] = (float)((F * 53) % 239) / 238.0f;
        Out[3] = 1.0f;
    }

    float HalfToFloat(unsigned short Hf)
    {
        const unsigned Sign = (Hf >> 15) & 1, Exp = (Hf >> 10) & 31, Man = Hf & 1023;
        const float    V    = (Exp == 0) ? ldexpf((float)Man, -24) : ldexpf((float)(Man | 1024), (int)Exp - 25);
        return Sign ? -V : V;
    }

    D3D12_RESOURCE_DESC Tex(DXGI_FORMAT Format, D3D12_RESOURCE_FLAGS Flags)
    {
        D3D12_RESOURCE_DESC T = {};
        T.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        T.Width            = W;
        T.Height           = H;
        T.DepthOrArraySize = 1;
        T.MipLevels        = 1;
        T.Format           = Format;
        T.SampleDesc.Count = 1;
        T.Flags            = Flags;
        return T;
    }

    bool MakeEverything()
    {
        if (bDebug)
        {
            ID3D12Debug* Debug = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&Debug))))
            {
                Debug->EnableDebugLayer();
                Debug->Release();
                printf("  the D3D12 debug layer is on\n");
            }
        }
        // The adapter the renderer takes: the system's high performance one first, so that on a laptop with two cards
        // the probe tries the same one - and its maker, for what the DLSS part may expect of it.
        IDXGIFactory6* Factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&Factory))))
        {
            IDXGIAdapter1* Adapter = nullptr;
            for (UINT i = 0; !Device && Factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                             IID_PPV_ARGS(&Adapter)) == S_OK; i++)
            {
                DXGI_ADAPTER_DESC1 D = {};
                Adapter->GetDesc1(&D);
                if (!(D.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device))))
                {
                    AdapterVendor = D.VendorId;
                    printf("  adapter: %ls (vendor 0x%04X)\n", D.Description, D.VendorId);
                }
                Adapter->Release();
                Adapter = nullptr;
            }
            Factory->Release();
        }
        if (!Device && FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device))))
            return false;
        D3D12_COMMAND_QUEUE_DESC Q = {};
        Q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Device->CreateCommandQueue(&Q, IID_PPV_ARGS(&Queue));
        for (int a = 0; a < 2; a++)
            Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Alloc[a]));
        Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Alloc[0], nullptr, IID_PPV_ARGS(&List));
        List->Close();
        Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence));
        FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        const D3D12_RESOURCE_FLAGS Rt  = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const D3D12_RESOURCE_FLAGS Uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_RESOURCE_STATES Common = D3D12_RESOURCE_STATE_COMMON;
        if (FAILED(FHost64Link::MakeShared(Device, Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, Rt), Common, nullptr, &Color)) ||
            FAILED(FHost64Link::MakeShared(Device, Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, Rt), Common, nullptr, &Output)) ||
            FAILED(FHost64Link::MakeShared(Device, Tex(DXGI_FORMAT_R10G10B10A2_UNORM, Rt), Common, nullptr, &SceneTen)) ||
            FAILED(FHost64Link::MakeShared(Device, Tex(DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE), Common, nullptr,
                                           &DepthCopy)) ||
            FAILED(FHost64Link::MakeShared(Device, Tex(DXGI_FORMAT_R16G16_FLOAT, Rt), Common, nullptr, &Motion)) ||
            FAILED(FHost64Link::MakeShared(Device, Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, Uav), Common, nullptr, &Upscaled)) ||
            FAILED(FHost64Link::MakeShared(Device, Tex(DXGI_FORMAT_R10G10B10A2_UNORM, Rt), Common, nullptr, &PresentTex)) ||
            FAILED(FHost64Link::MakeShared(Device, Tex(DXGI_FORMAT_R10G10B10A2_UNORM, Rt), Common, nullptr, &HudLessTex)))
            return false;
        // The generated frame is written through an unordered access view, and typed stores of R10G10B10A2 are not
        // something every card must have: asked, said, and the frame generation part left out where they are missing.
        D3D12_FEATURE_DATA_FORMAT_SUPPORT Fs = {};
        Fs.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        const bool bTenUav = SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &Fs, sizeof(Fs))) &&
                             (Fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
                             (Fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
        printf("  R10G10B10A2 typed stores through an unordered access view: %s\n", bTenUav ? "yes" : "NO");
        if (bTenUav && FAILED(FHost64Link::MakeShared(Device, Tex(DXGI_FORMAT_R10G10B10A2_UNORM, Uav), Common, nullptr,
                                                      &Generated)))
            printf("  the ten-bit generated frame could not be made\n");

        // The depth buffer, as the renderer makes it with the shadows on: typeless storage, a D32S8 view.
        D3D12_HEAP_PROPERTIES Heap = {};
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE Clear = {};
        Clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        const D3D12_RESOURCE_DESC Db = Tex(DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Db, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                   &Clear, IID_PPV_ARGS(&DepthBuf))))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC Hd = {};
        Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        Hd.NumDescriptors = 5;
        Device->CreateDescriptorHeap(&Hd, IID_PPV_ARGS(&RtvHeap));
        RtvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        Device->CreateRenderTargetView(Color, nullptr, Rtv(0));
        Device->CreateRenderTargetView(SceneTen, nullptr, Rtv(1));
        Device->CreateRenderTargetView(Motion, nullptr, Rtv(2));
        Device->CreateRenderTargetView(PresentTex, nullptr, Rtv(3));
        Device->CreateRenderTargetView(HudLessTex, nullptr, Rtv(4));
        Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        Hd.NumDescriptors = 1;
        Device->CreateDescriptorHeap(&Hd, IID_PPV_ARGS(&DsvHeap));
        D3D12_DEPTH_STENCIL_VIEW_DESC Dv = {};
        Dv.Format        = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        Dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        Device->CreateDepthStencilView(DepthBuf, &Dv, DsvHeap->GetCPUDescriptorHandleForHeapStart());

        // The corner's readback: room for the widest of the textures read (RGBA16F).
        const D3D12_RESOURCE_DESC Corner = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, Box, Box, 1, 1,
                                             DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                             D3D12_RESOURCE_FLAG_NONE };
        UINT64 Bytes = 0;
        Device->GetCopyableFootprints(&Corner, 0, 1, 0, &Foot, nullptr, nullptr, &Bytes);
        ReadbackBytes = (SIZE_T)Bytes;
        D3D12_RESOURCE_DESC B = {};
        B.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        B.Width            = Bytes;
        B.Height           = 1;
        B.DepthOrArraySize = 1;
        B.MipLevels        = 1;
        B.SampleDesc.Count = 1;
        B.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES Rb = {};
        Rb.Type = D3D12_HEAP_TYPE_READBACK;
        return SUCCEEDED(Device->CreateCommittedResource(&Rb, D3D12_HEAP_FLAG_NONE, &B, D3D12_RESOURCE_STATE_COPY_DEST,
                                                         nullptr, IID_PPV_ARGS(&Readback)));
    }

    // A Box x Box square of From at (X, Y) - the corner unless said otherwise - into the readback buffer, in From's own
    // format.
    void ReadCorner(ID3D12Resource* From, UINT X = 0, UINT Y = 0)
    {
        D3D12_TEXTURE_COPY_LOCATION Src = {}, Dst = {};
        Src.pResource       = From;
        Src.Type            = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Dst.pResource       = Readback;
        Dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Dst.PlacedFootprint = Foot;
        Dst.PlacedFootprint.Footprint.Format = From->GetDesc().Format;
        const D3D12_BOX Bx = { X, Y, 0, X + Box, Y + Box, 1 };
        List->CopyTextureRegion(&Dst, 0, 0, 0, &Src, &Bx);
    }

    // Is the corner read back (RGBA16F) the colour C, give or take Tol in each of its three channels - at its four
    // corner pixels?
    bool CornerIs(const float C[4], float Tol)
    {
        const unsigned short* Px   = nullptr;
        const D3D12_RANGE     Read = { 0, ReadbackBytes };
        if (FAILED(Readback->Map(0, &Read, (void**)&Px)))
            return false;
        bool bOk = true;
        for (UINT y = 0; y < Box && bOk; y += Box - 1)
            for (UINT x = 0; x < Box && bOk; x += Box - 1)
            {
                const unsigned short* P = (const unsigned short*)((const char*)Px + y * Foot.Footprint.RowPitch) + 4 * x;
                for (int c = 0; c < 3; c++)
                    bOk = bOk && fabsf(HalfToFloat(P[c]) - C[c]) < Tol;
            }
        const D3D12_RANGE None = { 0, 0 };
        Readback->Unmap(0, &None);
        return bOk;
    }

    // Is the corner read back (R10G10B10A2) the colour C, give or take Tol in each of its three channels - at its four
    // corner pixels?
    bool CornerIsTen(const float C[4], float Tol)
    {
        const unsigned char* Px   = nullptr;
        const D3D12_RANGE    Read = { 0, ReadbackBytes };
        if (FAILED(Readback->Map(0, &Read, (void**)&Px)))
            return false;
        bool bOk = true;
        for (UINT y = 0; y < Box && bOk; y += Box - 1)
            for (UINT x = 0; x < Box && bOk; x += Box - 1)
            {
                const UINT V = *(const UINT*)(Px + y * Foot.Footprint.RowPitch + 4 * x);
                for (int c = 0; c < 3; c++)
                    bOk = bOk && fabsf((float)((V >> (10 * c)) & 1023u) / 1023.0f - C[c]) < Tol;
            }
        const D3D12_RANGE None = { 0, 0 };
        Readback->Unmap(0, &None);
        return bOk;
    }

    // The first float of the corner read back (R32F).
    float CornerFloat()
    {
        const float*      Px   = nullptr;
        const D3D12_RANGE Read = { 0, sizeof(float) };
        if (FAILED(Readback->Map(0, &Read, (void**)&Px)))
            return -1.0f;
        const float V = Px[0];
        const D3D12_RANGE None = { 0, 0 };
        Readback->Unmap(0, &None);
        return V;
    }

    // One Test frame; its time from the first submission to the corner read back, in milliseconds, or -1 when the
    // corner is not the frame's colour, -2 when the link is down.
    double TestFrame(int F, FHost64Link* Link, bool bBusy)
    {
        float C[4];
        FrameColor(F, C);
        LARGE_INTEGER T0, T1;
        QueryPerformanceCounter(&T0);

        // The frame's first half: drawn, and everything handed over in COMMON.
        Open(0);
        Barrier(Color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        for (int k = 0; k < (bBusy ? Busy : 0); k++)
        {
            const float Other[4] = { 0.5f, 0.5f, (float)k / Busy, 1.0f };
            List->ClearRenderTargetView(Rtv(0), Other, 0, nullptr);
        }
        List->ClearRenderTargetView(Rtv(0), C, 0, nullptr);
        Barrier(Color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        if (!Link)
        {
            // Without the host: the same copy, here.
            Barrier(Color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(Output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            List->CopyResource(Output, Color);
            Barrier(Output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Barrier(Color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        Execute();
        if (Link)
        {
            Host64::FFrame Fr = {};
            Fr.Services     = Host64::Service_Upscale;
            Fr.RenderWidth  = W;
            Fr.RenderHeight = H;
            Fr.OutputWidth  = W;
            Fr.OutputHeight = H;
            if (!Link->Submit(Queue, Fr))
                return -2.0;
        }

        // The frame's second half: the output's corner, read back.
        Open(1);
        Barrier(Output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ReadCorner(Output);
        Barrier(Output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Execute();
        WaitGpu();
        QueryPerformanceCounter(&T1);
        return CornerIs(C, 0.002f) ? 1000.0 * (double)(T1.QuadPart - T0.QuadPart) / (double)Freq.QuadPart : -1.0;
    }

    int Compare(const void* A, const void* B)
    {
        const double X = *(const double*)A, Y = *(const double*)B;
        return (X < Y) ? -1 : (X > Y) ? 1 : 0;
    }

    void Report(const char* Name, double* T, int N, int Bad, const char* Tail)
    {
        qsort(T, (size_t)N, sizeof(double), Compare);
        printf("%-34s %4d frames, %d wrong - median %.3f ms, 95%% %.3f ms, worst %.3f ms%s\n", Name, N, Bad,
               N ? T[N / 2] : 0.0, N ? T[(N * 95) / 100] : 0.0, N ? T[N - 1] : 0.0, Tail);
    }

    // Frames Test frames; prints the median, the 95th percentile and the worst; returns the frames that went wrong.
    int TestSeries(const char* Name, int Frames, FHost64Link* Link, bool bBusy, double& Median)
    {
        double* T   = (double*)calloc((size_t)Frames, sizeof(double));
        int     Bad = 0, N = 0;
        for (int f = 1; f <= Frames; f++)
        {
            const double Ms = TestFrame(f, Link, bBusy);
            if (Ms < 0.0)
            {
                Bad++;
                if (Ms < -1.5)
                    break;
                continue;
            }
            if (f > 10)      // the first frames warm the caches and the host's first submissions
                T[N++] = Ms;
        }
        Report(Name, T, N, Bad, "");
        qsort(T, (size_t)N, sizeof(double), Compare);
        Median = N ? T[N / 2] : 0.0;
        free(T);
        return Bad + DrainDebug();
    }

    // The FSR part: a flat colour, a flat depth, no motion. Once the upscaler has settled (its first frames build the
    // history), the colour it gives back has to be the colour it was given.
    int FsrSeries(FHost64Link& Link, int Frames, const char* Library)
    {
        // The depth and the motion, once: they never change. The depth goes in the way the renderer's will - its depth
        // plane copied out of the D32S8 buffer into the shared R32F texture - and the copy is read back.
        Open(0);
        List->ClearDepthStencilView(DsvHeap->GetCPUDescriptorHandleForHeapStart(),
                                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.5f, 0, 0, nullptr);
        Barrier(Motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        List->ClearRenderTargetView(Rtv(2), Zero, 0, nullptr);
        Barrier(Motion, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        Barrier(DepthBuf, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(DepthCopy, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        {
            D3D12_TEXTURE_COPY_LOCATION Src = {}, Dst = {};
            Src.pResource        = DepthBuf;
            Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            Src.SubresourceIndex = 0;            // the depth plane
            Dst.pResource        = DepthCopy;
            Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            List->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        }
        Barrier(DepthBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        Barrier(DepthCopy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ReadCorner(DepthCopy);
        Barrier(DepthCopy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Execute();
        WaitGpu();
        const float Copied = CornerFloat();
        const bool  bDepthOk = fabsf(Copied - 0.5f) < 1e-6f;
        printf("  the depth plane of a D32S8 buffer, copied into R32F: %.6f (%s)\n", Copied, bDepthOk ? "ok" : "WRONG");
        int Bad = bDepthOk ? 0 : 1;
        Bad += DrainDebug();

        float C[4];
        FrameColor(7, C);
        double* T = (double*)calloc((size_t)Frames, sizeof(double));
        int     N = 0;
        for (int f = 1; f <= Frames; f++)
        {
            LARGE_INTEGER T0, T1;
            QueryPerformanceCounter(&T0);
            Open(0);
            Barrier(SceneTen, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            List->ClearRenderTargetView(Rtv(1), C, 0, nullptr);
            Barrier(SceneTen, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            Execute();

            Host64::FFrame Fr = {};
            Fr.Services     = Host64::Service_Upscale;
            Fr.Flags        = (f == 1) ? Host64::FrameFlag_Reset : 0u;
            Fr.RenderWidth  = W;
            Fr.RenderHeight = H;
            Fr.OutputWidth  = W;
            Fr.OutputHeight = H;
            // A jitter that walks the pixel, as the renderer's does.
            Fr.JitterX     = 0.5f * (float)((f * 7) % 16) / 16.0f - 0.25f;
            Fr.JitterY     = 0.5f * (float)((f * 11) % 16) / 16.0f - 0.25f;
            Fr.FrameTimeMs = 16.7f;
            Fr.FovY        = 1.2f;
            Fr.NearZ       = 1.0f;
            Fr.Sharpness   = 0.5f;
            if (!Link.Submit(Queue, Fr))
            {
                Bad++;
                break;
            }
            Open(1);
            Barrier(Upscaled, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ReadCorner(Upscaled);
            Barrier(Upscaled, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Execute();
            WaitGpu();
            QueryPerformanceCounter(&T1);
            if (f > 30)
            {
                if (!CornerIs(C, 0.03f))
                    Bad++;
                T[N++] = 1000.0 * (double)(T1.QuadPart - T0.QuadPart) / (double)Freq.QuadPart;
            }
        }
        char Tail[64], Name[64];
        sprintf_s(Tail, "; the host refused %u", Link.FramesFailed());
        sprintf_s(Name, "%s through the host", Library);
        Report(Name, T, N, Bad, Tail);
        free(T);
        return Bad + (int)Link.FramesFailed() + DrainDebug();
    }

    // Frame generation, after the upscaler's series and on the same host: a flat colour in the finished picture, the
    // depth and motion FsrSeries left (0.5 and none), the camera standing still. From the second frame on the
    // generated one sits between two identical pictures, and has to be that same colour.
    int FgSeries(FHost64Link& Link, int Frames, const char* Library)
    {
        float C[4];
        FrameColor(11, C);
        // The "HUD": a white box in the finished picture that the HUD-less one does not have. Read back on every other
        // frame, the corner on the rest.
        const float     White[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        const D3D12_RECT Hud     = { 64, 64, 64 + 256, 64 + 128 };
        const uint32_t FailedBefore = Link.FramesFailed();
        double* T   = (double*)calloc((size_t)Frames, sizeof(double));
        int     N   = 0, Bad = 0, BadHud = 0;
        for (int f = 1; f <= Frames; f++)
        {
            LARGE_INTEGER T0, T1;
            QueryPerformanceCounter(&T0);
            Open(0);
            Barrier(PresentTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Barrier(HudLessTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            List->ClearRenderTargetView(Rtv(3), C, 0, nullptr);
            List->ClearRenderTargetView(Rtv(3), White, 1, &Hud);
            List->ClearRenderTargetView(Rtv(4), C, 0, nullptr);
            Barrier(HudLessTex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            Barrier(PresentTex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            Execute();

            Host64::FFrame Fr = {};
            Fr.Services     = Host64::Service_FramePrepare | Host64::Service_FrameGen;   // both halves at once
            Fr.Flags        = (f == 1) ? Host64::FrameFlag_Reset : 0u;
            Fr.RenderWidth  = W;
            Fr.RenderHeight = H;
            Fr.OutputWidth  = W;
            Fr.OutputHeight = H;
            Fr.FrameTimeMs  = 16.7f;
            Fr.FovY         = 1.2f;
            Fr.NearZ        = 1.0f;
            // A camera at the origin looking along +Y, +Z up: valid vectors, which is what the generator asks for.
            Fr.CameraUp[2]      = 1.0f;
            Fr.CameraRight[0]   = 1.0f;
            Fr.CameraForward[1] = 1.0f;
            if (!Link.Submit(Queue, Fr))
            {
                Bad++;
                break;
            }
            const bool bHud = (f % 2) == 1;
            Open(1);
            Barrier(Generated, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ReadCorner(Generated, bHud ? 128 : 0, bHud ? 96 : 0);
            Barrier(Generated, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Execute();
            WaitGpu();
            QueryPerformanceCounter(&T1);
            if (f > 10)
            {
                if (!CornerIsTen(bHud ? White : C, 0.02f))
                {
                    Bad++;
                    if (bHud)
                        BadHud++;
                }
                T[N++] = 1000.0 * (double)(T1.QuadPart - T0.QuadPart) / (double)Freq.QuadPart;
            }
        }
        char Tail[96], Name[64];
        sprintf_s(Tail, "; the host refused %u; the HUD wrong in %d", Link.FramesFailed() - FailedBefore, BadHud);
        sprintf_s(Name, "%s frame generation through the host", Library);
        Report(Name, T, N, Bad, Tail);
        free(T);
        return Bad + (int)(Link.FramesFailed() - FailedBefore) + DrainDebug();
    }

    // With the debug layer on in the host too: the errors its layer has had - its own log spells them out. Asked
    // before the host is stopped.
    int HostErrors(FHost64Link& Link)
    {
        const int N = bDebug ? (int)Link.HostDebugErrors() : 0;
        if (N)
            printf("  the host's own debug layer had %d errors (SecondRevisionHost64.log beside it spells them out)\n", N);
        return N;
    }

    // The host with one library - FSR's or DLSS's - on the renderer's formats: the upscaler's series, then frame
    // generation's where the host offers it for this card. False: the host would not come up with that library, and
    // has said why; whether that is a failure is the caller's to say. Bad counts what went wrong once it was up.
    bool LibrarySeries(FHost64Link& Link, const wchar_t* HostExe, uint32_t Backend, const char* Library, int Frames,
                       int& Bad, bool& bAlive)
    {
        // The libraries' DLLs may take a while to come up the first time.
        if (!Link.Start(Device, HostExe, Backend, 20000))
        {
            printf("%s: the host did not come up with it: %s\n", Library, Link.Message());
            return false;
        }
        ID3D12Resource* Shared[Host64::Resource_Count] = {};
        Shared[Host64::Resource_Color]     = SceneTen;
        Shared[Host64::Resource_Depth]     = DepthCopy;
        Shared[Host64::Resource_Motion]    = Motion;
        Shared[Host64::Resource_Output]    = Upscaled;
        Shared[Host64::Resource_Present]   = Generated ? PresentTex : nullptr;
        Shared[Host64::Resource_HudLess]   = Generated ? HudLessTex : nullptr;
        Shared[Host64::Resource_Generated] = Generated;
        // The host makes its upscaler - and its frame generation - when it takes these: the first time on a machine the
        // driver compiles their shaders, hence the time allowed.
        if (!Link.Share(Shared, 60000))
        {
            printf("%s: the host did not take the textures: %s\n", Library, Link.Message());
            Bad++;
        }
        else
        {
            Bad += FsrSeries(Link, (Frames < 90) ? 90 : Frames, Library);
            // Measured on the host's own queue: what is left of a frame's time above is the crossing and the probe.
            printf("  the host's own GPU time for a frame's upscale: %.3f ms\n", (double)Link.WorkUs() / 1000.0);
            if (!Generated)
                printf("%s frame generation: left out - no ten-bit generated frame to write into\n", Library);
            else if (!(Link.Services() & Host64::Service_FrameGen))
                printf("%s frame generation: the host has none for this card (its log says why)\n", Library);
            else
            {
                Bad += FgSeries(Link, (Frames < 90) ? 90 : Frames, Library);
                printf("  the host's own GPU time for a generated frame: %.3f ms\n", (double)Link.WorkFgUs() / 1000.0);
            }
            printf("%s: what ran: %s\n", Library, Link.Message());
        }
        bAlive = Link.CheckAlive(1000) && bAlive;
        Bad += HostErrors(Link);
        Link.Stop();
        return true;
    }

    // ---- The rays ----------------------------------------------------------------------------------------------

    // The rays' own textures: the size of a game's frame on a laptop's panel.
    const UINT RW = 1920, RH = 1200;
    // The scene is a floor a hundred units each way from its middle, seen from a hundred above looking straight down:
    // a pixel is a point of the floor, and its place in the picture is its place on the floor, -100..100 each way.
    const float RayEyeZ = 100.0f;
    const float RayLight[3] = { -65.0f, 0.0f, 60.0f };      // the factor's one light
    const float RayLightReach = 500.0f;
    const float RayAoRadius = 30.0f;

    float WorldX(int Px) { return (((float)Px + 0.5f) / (float)RW * 2.0f - 1.0f) * 100.0f; }
    float WorldY(int Py) { return (((float)Py + 0.5f) / (float)RH * 2.0f - 1.0f) * 100.0f; }
    int   PixelX(float X) { return (int)((X / 100.0f + 1.0f) * 0.5f * (float)RW); }
    int   PixelY(float Y) { return (int)((Y / 100.0f + 1.0f) * 0.5f * (float)RH); }

    // A triangle of the probe's scene, with what the passes go by: the instance it is of (FRtPass's numbers: 0 the
    // level, 1 the bodies, 2 the sky's walls, 3 the see-through surfaces), the geometry among the bodies, the triangle.
    struct FRayTri
    {
        float    P[3][3];
        float    U[3];          // a masked body's texture place across it, a corner each
        uint32_t Instance, Geometry, Prim;
        uint32_t Slot;          // the mask's: 0 solid. The probe's one mask (slot 1) is solid where u < 0.5.
        bool     bAsks;         // not opaque: the pass is asked about it
        bool     bPassed;       // the body every asked ray passes, and the light's own
    };
    // A body's rectangle on its height: what the occlusion's check goes by.
    struct FRayBox { float X0, X1, Y0, Y1; };
    FRayTri RayTris[32];
    int     RayTriCount = 0;
    FRayBox RayBoxes[4];
    const UINT GRayInstanceMask[4] = { 1u, 2u, 4u, 8u };

    void AddQuad(uint32_t Instance, uint32_t Geometry, float X0, float X1, float Y0, float Y1, float Z, bool bAsks,
                 uint32_t Slot, bool bPassed)
    {
        const float C[4][2] = { { X0, Y0 }, { X1, Y0 }, { X1, Y1 }, { X0, Y1 } };
        const int   Order[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
        for (int t = 0; t < 2; t++)
        {
            FRayTri& T = RayTris[RayTriCount++];
            for (int c = 0; c < 3; c++)
            {
                T.P[c][0] = C[Order[t][c]][0];
                T.P[c][1] = C[Order[t][c]][1];
                T.P[c][2] = Z;
                T.U[c]    = (C[Order[t][c]][0] - X0) / (X1 - X0);
            }
            T.Instance = Instance;
            T.Geometry = Geometry;
            T.Prim     = (uint32_t)t;
            T.Slot     = Slot;
            T.bAsks    = bAsks;
            T.bPassed  = bPassed;
        }
        if (Instance == 1)
            RayBoxes[Geometry] = { X0, X1, Y0, Y1 };
    }

    // The frame's scene. The level, the sky's walls and the see-through surfaces never change; of the four bodies the
    // first is in another place on every other frame - a structure left over from the frame before would show.
    //   body 0  the frame's, opaque, moving      body 1  the frame's, masked: solid on its low-x half
    //   body 2  the arena's, opaque              body 3  the frame's, asked about and passed by everything
    void BuildRayScene(int Frame)
    {
        RayTriCount = 0;
        AddQuad(0, 0, -100.0f, 100.0f, -100.0f, 100.0f, 0.0f, false, 0, false);
        AddQuad(2, 0, 60.0f, 100.0f, -100.0f, 100.0f, 50.0f, false, 0, false);
        AddQuad(3, 0, 20.0f, 40.0f, -100.0f, 100.0f, 30.0f, false, 0, false);
        const float Cx = (Frame & 1) ? -80.0f : -50.0f;
        AddQuad(1, 0, Cx - 8.0f, Cx + 8.0f, -8.0f, 8.0f, 10.0f, false, 0, false);
        AddQuad(1, 1, -20.0f, 0.0f, -10.0f, 10.0f, 20.0f, true, 1, false);
        AddQuad(1, 2, 2.0f, 18.0f, -68.0f, -52.0f, 15.0f, false, 0, false);
        AddQuad(1, 3, 2.0f, 18.0f, 52.0f, 68.0f, 25.0f, true, 0, true);
    }

    // Does the ray from O along D meet the triangle? Where along it, and with which weights of its second and third
    // corner - the two numbers a ray query gives.
    bool MeetTri(const float O[3], const float D[3], const FRayTri& T, float& At, float& Bu, float& Bv)
    {
        float E1[3], E2[3], S[3];
        for (int i = 0; i < 3; i++)
        {
            E1[i] = T.P[1][i] - T.P[0][i];
            E2[i] = T.P[2][i] - T.P[0][i];
            S[i]  = O[i] - T.P[0][i];
        }
        const float Pv[3] = { D[1] * E2[2] - D[2] * E2[1], D[2] * E2[0] - D[0] * E2[2], D[0] * E2[1] - D[1] * E2[0] };
        const float Det   = E1[0] * Pv[0] + E1[1] * Pv[1] + E1[2] * Pv[2];
        if (fabsf(Det) < 1e-9f)
            return false;
        Bu = (S[0] * Pv[0] + S[1] * Pv[1] + S[2] * Pv[2]) / Det;
        const float Qv[3] = { S[1] * E1[2] - S[2] * E1[1], S[2] * E1[0] - S[0] * E1[2], S[0] * E1[1] - S[1] * E1[0] };
        Bv = (D[0] * Qv[0] + D[1] * Qv[1] + D[2] * Qv[2]) / Det;
        At = (E2[0] * Qv[0] + E2[1] * Qv[1] + E2[2] * Qv[2]) / Det;
        return Bu >= 0.0f && Bv >= 0.0f && Bu + Bv <= 1.0f;
    }

    // Is the triangle met where the ray crosses it - as the passes decide for a body they are asked about?
    bool SolidAt(const FRayTri& T, float Bu, float Bv)
    {
        if (!T.bAsks)
            return true;
        if (T.bPassed)
            return false;
        return T.Slot == 0 || T.U[0] * (1.0f - Bu - Bv) + T.U[1] * Bu + T.U[2] * Bv < 0.5f;
    }

    // The nearest thing the ray meets within (TMin, TMax) among the instances in Mask; null: nothing.
    const FRayTri* TraceHere(const float O[3], const float D[3], float TMin, float TMax, uint32_t Mask, float& Nearest)
    {
        const FRayTri* Met = nullptr;
        Nearest = TMax;
        for (int t = 0; t < RayTriCount; t++)
        {
            float At, Bu, Bv;
            const FRayTri& T = RayTris[t];
            if ((GRayInstanceMask[T.Instance] & Mask) && MeetTri(O, D, T, At, Bu, Bv) && At > TMin && At < Nearest &&
                SolidAt(T, Bu, Bv))
            {
                Nearest = At;
                Met     = &T;
            }
        }
        return Met;
    }

    // Is the light blocked on its way to the floor's point (X, Y)? The factor's own ray: from a little over the floor
    // to a little short of the light, against the level and the bodies.
    bool LightBlocked(float X, float Y)
    {
        const float O[3] = { X, Y, 0.5f };
        float D[3] = { RayLight[0] - X, RayLight[1] - Y, RayLight[2] - 0.5f };
        const float Len = sqrtf(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
        for (int i = 0; i < 3; i++)
            D[i] /= Len;
        float At;
        return TraceHere(O, D, 0.0f, Len - 1.5f, 0x03u, At) != nullptr;
    }

    // What the list pass is asked: a ray straight down or up from the eye's height over the floor's point (X, Y). None
    // of them over a rectangle's diagonal, where its two triangles meet and either is a right answer. From: how far
    // from its start the ray begins to look - the distance in its answer is from the start all the same.
    struct FRayAsk { float X, Y, Dz; uint32_t Mask; bool bAsked; float From; };
    const FRayAsk RayAsks[] = {
        { -80.0f, -3.0f, -1.0f, 0, true },    // body 0 on the odd frames, the floor on the even
        { -50.0f, -3.0f, -1.0f, 0, true },    // ... and the other way round
        { -15.0f, 0.0f, -1.0f, 0, true },     // body 1 where its mask is solid
        { -5.0f, 0.0f, -1.0f, 0, true },      // ... and where it is clear: the floor under it
        { 10.0f, -63.0f, -1.0f, 0, true },    // body 2, the arena's
        { 10.0f, 60.0f, -1.0f, 0, true },     // body 3, passed: the floor under it
        { 33.0f, 0.0f, -1.0f, 11, true },     // the see-through surface, for a ray that names it
        { 30.0f, 40.0f, -1.0f, 0, true },     // ... and the floor under it for one that does not
        { 85.0f, 0.0f, -1.0f, 7, true },      // the sky's wall, likewise
        { 80.0f, 40.0f, -1.0f, 0, true },
        { -90.0f, -90.0f, 1.0f, 255, true },  // upwards: nothing
        { -90.0f, 90.0f, -1.0f, 0, false },   // no ray asked
        { 30.0f, 40.0f, -1.0f, 0, true, 2.0f },     // the floor again, by rays that begin to look further on
        { 80.0f, 40.0f, -1.0f, 0, true, 10.0f },
        { -15.0f, 0.0f, -1.0f, 0, true, 2.0f },     // ... and a body
    };
    const int RayAskCount = sizeof(RayAsks) / sizeof(RayAsks[0]);

    // The passes' constants, laid out as the shaders declare them (ShadowFactor.hlsl: Shadow, RtShadow; RayList.hlsl).
    struct FShadowConsts
    {
        float    TanX, TanY;
        uint32_t Scale, Count;
        uint32_t NoOnTop, Flags;
        float    Calib, MaxShare;
        float    AxisX[3], Near;
        float    AxisY[3], ActorTexel;
        float    AxisZ[3], WorldTexel;
        uint32_t Taps, Marks;
        float    Spread, LightSize;
        float    ViewRect[4];
        float    SkipRect[4][4];
        uint32_t SkipCount;
        float    Eye[3];
        float    LightCam[RDX_SHADOW_MAX_LIGHTS][4];
        float    LightInfo[RDX_SHADOW_MAX_LIGHTS][4];
        float    LightSpot[RDX_SHADOW_MAX_LIGHTS][4];
    };
    static_assert(sizeof(FShadowConsts) == 192 + 48 * RDX_SHADOW_MAX_LIGHTS, "the factor's constants, as the shader has them");
    struct FRtShadowConsts
    {
        float    AoParams[4];
        uint32_t RtKnobs[4];
        uint32_t RtRows[4];
    };
    struct FListConsts
    {
        uint32_t ListKnobs[4];      // the slots the pass may read; the body a request may pass; nothing traced on
    };

    // A ray's way in one uint, as the list's slots carry it (RayListSlots.hlsli, RayPackWay).
    uint32_t PackWay(float X, float Y, float Z)
    {
        const float Sum = fabsf(X) + fabsf(Y) + fabsf(Z);
        const float D[3] = { X / Sum, Y / Sum, Z / Sum };
        const float Sx = (D[0] >= 0.0f) ? 1.0f : -1.0f, Sy = (D[1] >= 0.0f) ? 1.0f : -1.0f;
        const float Ex = (D[2] >= 0.0f) ? D[0] : (1.0f - fabsf(D[1])) * Sx;
        const float Ey = (D[2] >= 0.0f) ? D[1] : (1.0f - fabsf(D[0])) * Sy;
        const auto  Q  = [](float E) {
            float V = E * 0.5f + 0.5f;
            V = (V < 0.0f) ? 0.0f : ((V > 1.0f) ? 1.0f : V);
            return (uint32_t)(V * 65535.0f + 0.5f);
        };
        return (Q(Ex) << 16) | Q(Ey);
    }

    // A request in its slot (RenderLimits.h, RDX_RAY_*): from where, how far, which way, and its packed word - the
    // kind, the instances it meets, its flags, and how far from its start it begins to look, in sixteenths of a
    // unit. No block of a pixel's here: the key and the chain stay nought.
    void WriteRequest(uint32_t* Slot, float Ox, float Oy, float Oz, float Dx, float Dy, float Dz, float Far,
                      uint32_t Kind, uint32_t Mask, uint32_t Flags, float From = 0.0f)
    {
        const float Head[4] = { Ox, Oy, Oz, Far };
        memcpy(Slot, Head, 16);
        uint32_t Begins = (uint32_t)(From * 16.0f + 0.5f);
        Begins  = (Begins > RDX_RAY_FROM_MASK) ? RDX_RAY_FROM_MASK : Begins;
        Slot[4] = PackWay(Dx, Dy, Dz);
        Slot[5] = 0;
        Slot[6] = 0;
        Slot[7] = Kind | ((Mask & 0xFFu) << RDX_RAY_MASK_SHIFT) | Flags | (Begins << RDX_RAY_FROM_SHIFT);
    }

    // Where things are in the upload buffer the shared ones are filled from, and in the passes' tables.
    enum : UINT { UP_LEVEL = 0, UP_SKY = 512, UP_THIN = 1024, UP_FRAME_XYZ = 2048, UP_ARENA_XYZ = 4096,
                  UP_FRAME_UVW = 6144, UP_ARENA_UVW = 8192, UP_TABLES = 16384, UP_REQUESTS = 24576, UP_MARK = 28672,
                  UP_MASKS = 32768, UP_BYTES = 65536 };
    enum : UINT { AT_FACTOR = 0, AT_RAYS = 2816, AT_LIST = 3072, AT_GEO = 3328, TABLE_BYTES = 4096 };
    enum : UINT { FRAME_STRIDE = 20 };      // the frame's corners have more to a vertex than the three floats read
    enum : UINT { BACK_LIST = 0, BACK_FACTOR = 1u << 20, BACK_AO = 4u << 20, BACK_BYTES = 8u << 20 };
    // The little list's slots, and how much of the answers' buffer is filled with the mark.
    enum : UINT { LIST_ROOM = 2048, MARK_BYTES = 4096 };

    ID3D12Resource*       RayRes[Host64::Ray_Count] = {};
    ID3D12Resource*       RayUp       = nullptr;
    BYTE*                 RayUpMapped = nullptr;
    ID3D12Resource*       RayBack     = nullptr;
    ID3D12DescriptorHeap* RayRtv      = nullptr;      // the depth, where it is R32F
    ID3D12DescriptorHeap* RayDsv      = nullptr;
    bool                  bRayDepthWhole = false;     // the factor's depth is a depth-stencil buffer shared whole
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT RayPicture = {};   // a whole R8 picture of the rays' size, read back

    D3D12_RESOURCE_DESC BufferDesc(UINT64 Bytes)
    {
        D3D12_RESOURCE_DESC B = {};
        B.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        B.Width            = Bytes;
        B.Height           = 1;
        B.DepthOrArraySize = 1;
        B.MipLevels        = 1;
        B.SampleDesc.Count = 1;
        B.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return B;
    }

    D3D12_RESOURCE_DESC RayTex(UINT Wd, UINT Ht, DXGI_FORMAT Format, D3D12_RESOURCE_FLAGS Flags, UINT16 Slices = 1)
    {
        D3D12_RESOURCE_DESC T = Tex(Format, Flags);
        T.Width            = Wd;
        T.Height           = Ht;
        T.DepthOrArraySize = Slices;
        return T;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE RayRtvAt(int i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE R = RayRtv->GetCPUDescriptorHandleForHeapStart();
        R.ptr += (SIZE_T)i * RtvStep;
        return R;
    }

    // Everything the rays' part shares, and what it is filled from and read back into. False: something every card has
    // to make could not be made.
    bool MakeRayResources()
    {
        const D3D12_RESOURCE_STATES Common = D3D12_RESOURCE_STATE_COMMON;
        const D3D12_RESOURCE_FLAGS  Rt     = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        static const int Buffers[] = { Host64::Ray_Level, Host64::Ray_Sky, Host64::Ray_Thin, Host64::Ray_FrameXyz,
                                       Host64::Ray_ArenaXyz, Host64::Ray_Tables, Host64::Ray_FrameUvw,
                                       Host64::Ray_ArenaUvw };
        for (int b : Buffers)
        {
            const HRESULT Made = FHost64Link::MakeShared(Device, BufferDesc(65536), Common, nullptr, &RayRes[b]);
            if (FAILED(Made))
            {
                printf("rays: a shared BUFFER could not be made (resource %d, 0x%08lX)\n", b, (unsigned long)Made);
                return false;
            }
        }
        if (FAILED(FHost64Link::MakeShared(Device, RayTex(8, 8, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, 2), Common,
                                           nullptr, &RayRes[Host64::Ray_Masks])) ||
            FAILED(FHost64Link::MakeShared(Device, RayTex(RW, RH, DXGI_FORMAT_R8_UNORM, Rt), Common, nullptr,
                                           &RayRes[Host64::Ray_Factor])) ||
            FAILED(FHost64Link::MakeShared(Device, RayTex(RW, RH, DXGI_FORMAT_R8_UNORM, Rt), Common, nullptr,
                                           &RayRes[Host64::Ray_Occlusion])))
        {
            printf("rays: a shared texture could not be made\n");
            return false;
        }
        // The list's two buffers, as the renderer shares them - each one that a shader writes: the requests the
        // renderer's own pixel shaders, the answers the host's pass. Not every driver has to share such a buffer.
        D3D12_RESOURCE_DESC ListDesc = BufferDesc((UINT64)LIST_ROOM * RDX_RAY_SLOT_BYTES);
        ListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT ListMade = FHost64Link::MakeShared(Device, ListDesc, Common, nullptr, &RayRes[Host64::Ray_Requests]);
        if (SUCCEEDED(ListMade))
            ListMade = FHost64Link::MakeShared(Device, ListDesc, Common, nullptr, &RayRes[Host64::Ray_Answers]);
        if (FAILED(ListMade))
        {
            printf("rays: a shared buffer that a shader writes could not be made (0x%08lX) - no list\n",
                   (unsigned long)ListMade);
            if (RayRes[Host64::Ray_Requests])
                RayRes[Host64::Ray_Requests]->Release();
            RayRes[Host64::Ray_Requests] = RayRes[Host64::Ray_Answers] = nullptr;
        }
        // The factor's depth as the renderer has it: a depth-stencil buffer, shared whole - its stencil plane says what
        // kind of surface a pixel is. Where that cannot be shared, the depth alone goes over, as a texture of floats.
        D3D12_CLEAR_VALUE Clear = {};
        Clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        const HRESULT Whole = FHost64Link::MakeShared(
            Device, RayTex(RW, RH, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), Common, &Clear,
            &RayRes[Host64::Ray_WorldDepth]);
        bRayDepthWhole = SUCCEEDED(Whole);
        printf("  a depth-stencil buffer on a shared heap: %s (0x%08lX)\n", bRayDepthWhole ? "made" : "REFUSED",
               (unsigned long)Whole);
        if (!bRayDepthWhole && FAILED(FHost64Link::MakeShared(Device, RayTex(RW, RH, DXGI_FORMAT_R32_FLOAT, Rt), Common,
                                                              nullptr, &RayRes[Host64::Ray_WorldDepth])))
            return false;

        D3D12_HEAP_PROPERTIES Up = {}, Rb = {};
        Up.Type = D3D12_HEAP_TYPE_UPLOAD;
        Rb.Type = D3D12_HEAP_TYPE_READBACK;
        const D3D12_RESOURCE_DESC UpDesc = BufferDesc(UP_BYTES), BackDesc = BufferDesc(BACK_BYTES);
        if (FAILED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &UpDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&RayUp))) ||
            FAILED(RayUp->Map(0, nullptr, (void**)&RayUpMapped)) ||
            FAILED(Device->CreateCommittedResource(&Rb, D3D12_HEAP_FLAG_NONE, &BackDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&RayBack))))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC Hd = {};
        Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        Hd.NumDescriptors = 3;
        Device->CreateDescriptorHeap(&Hd, IID_PPV_ARGS(&RayRtv));
        if (bRayDepthWhole)
        {
            Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            Hd.NumDescriptors = 1;
            Device->CreateDescriptorHeap(&Hd, IID_PPV_ARGS(&RayDsv));
            D3D12_DEPTH_STENCIL_VIEW_DESC Dv = {};
            Dv.Format        = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            Dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            Device->CreateDepthStencilView(RayRes[Host64::Ray_WorldDepth], &Dv,
                                           RayDsv->GetCPUDescriptorHandleForHeapStart());
        }
        else
            Device->CreateRenderTargetView(RayRes[Host64::Ray_WorldDepth], nullptr, RayRtvAt(2));
        const D3D12_RESOURCE_DESC Picture = RayTex(RW, RH, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE);
        Device->GetCopyableFootprints(&Picture, 0, 1, 0, &RayPicture, nullptr, nullptr, nullptr);
        return true;
    }

    // The corners of the scene's triangles that are of Instance - and, among the bodies, of Geometry - into Out, Stride
    // bytes a corner; how many corners.
    UINT WriteCorners(BYTE* Out, UINT Stride, uint32_t Instance, uint32_t Geometry)
    {
        UINT N = 0;
        for (int t = 0; t < RayTriCount; t++)
            if (RayTris[t].Instance == Instance && (Instance != 1 || RayTris[t].Geometry == Geometry))
                for (int c = 0; c < 3; c++, N++)
                    memcpy(Out + (size_t)N * Stride, RayTris[t].P[c], 12);
        return N;
    }

    // The same corners' texture places: u, v and the mask's slot, three floats each.
    UINT WriteUvw(BYTE* Out, uint32_t Geometry)
    {
        UINT N = 0;
        for (int t = 0; t < RayTriCount; t++)
            if (RayTris[t].Instance == 1 && RayTris[t].Geometry == Geometry)
                for (int c = 0; c < 3; c++, N++)
                {
                    const float Uvw[3] = { RayTris[t].U[c], 0.5f, (float)RayTris[t].Slot };
                    memcpy(Out + (size_t)N * 12, Uvw, 12);
                }
        return N;
    }

    void CopyUp(int Resource, UINT From, UINT Bytes)
    {
        List->CopyBufferRegion(RayRes[Resource], 0, RayUp, From, Bytes);
    }

    // What never changes, once: the level's three buffers, the arena's body, the mask, the asked rays and the depth.
    // The asked rays lie a slot each after the count's own, as a pixel shader would have written them down - each
    // passing the body the list's constants name -, and the answers' buffer is filled with a mark, so that a slot the
    // pass has to leave alone shows that it was left.
    void FillRayStatics()
    {
        memset(RayUpMapped, 0, UP_BYTES);
        BuildRayScene(1);
        WriteCorners(RayUpMapped + UP_LEVEL, 12, 0, 0);
        WriteCorners(RayUpMapped + UP_SKY, 12, 2, 0);
        WriteCorners(RayUpMapped + UP_THIN, 12, 3, 0);
        WriteCorners(RayUpMapped + UP_ARENA_XYZ, 12, 1, 2);
        WriteUvw(RayUpMapped + UP_ARENA_UVW, 2);
        // The mask: two 8x8 slices, a row 256 bytes apart. Slot 0 is never sampled; slot 1 is solid on its low-u half.
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                RayUpMapped[UP_MASKS + 2048 + y * 256 + x] = (x < 4) ? 255 : 0;
        uint32_t* const Slots = (uint32_t*)(RayUpMapped + UP_REQUESTS);
        Slots[0] = 1u + (uint32_t)RayAskCount;
        Slots[1] = LIST_ROOM;
        for (int a = 0; a < RayAskCount; a++)
            WriteRequest(Slots + 8 * (1 + a), RayAsks[a].X, RayAsks[a].Y, RayEyeZ, 0.0f, 0.0f, RayAsks[a].Dz, 1000.0f,
                         RayAsks[a].bAsked ? RDX_RAY_KIND_FIRST : RDX_RAY_KIND_NONE, RayAsks[a].Mask,
                         RDX_RAY_FLAG_NO_BODY, RayAsks[a].From);
        memset(RayUpMapped + UP_MARK, 0xA5, MARK_BYTES);
        Open(0);
        if (RayRes[Host64::Ray_Requests])
        {
            CopyUp(Host64::Ray_Requests, UP_REQUESTS, (1 + RayAskCount) * RDX_RAY_SLOT_BYTES);
            CopyUp(Host64::Ray_Answers, UP_MARK, MARK_BYTES);
        }
        CopyUp(Host64::Ray_Level, UP_LEVEL, 72);
        CopyUp(Host64::Ray_Sky, UP_SKY, 72);
        CopyUp(Host64::Ray_Thin, UP_THIN, 72);
        CopyUp(Host64::Ray_ArenaXyz, UP_ARENA_XYZ, 72);
        CopyUp(Host64::Ray_ArenaUvw, UP_ARENA_UVW, 72);
        Barrier(RayRes[Host64::Ray_Masks], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        for (UINT s = 0; s < 2; s++)
        {
            D3D12_TEXTURE_COPY_LOCATION Src = {}, Dst = {};
            Src.pResource                          = RayUp;
            Src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            Src.PlacedFootprint.Offset             = UP_MASKS + s * 2048;
            Src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
            Src.PlacedFootprint.Footprint.Width    = 8;
            Src.PlacedFootprint.Footprint.Height   = 8;
            Src.PlacedFootprint.Footprint.Depth    = 1;
            Src.PlacedFootprint.Footprint.RowPitch = 256;
            Dst.pResource        = RayRes[Host64::Ray_Masks];
            Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            Dst.SubresourceIndex = s;
            List->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        }
        Barrier(RayRes[Host64::Ray_Masks], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);

        // The depth: the floor, everywhere - one over its distance from the eye - and the level's kind in the stencil.
        ID3D12Resource* const Depth = RayRes[Host64::Ray_WorldDepth];
        if (bRayDepthWhole)
        {
            Barrier(Depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            List->ClearDepthStencilView(RayDsv->GetCPUDescriptorHandleForHeapStart(),
                                        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f / RayEyeZ, 0, 0, nullptr);
            Barrier(Depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
            const float Floor[4] = { 1.0f / RayEyeZ, 0.0f, 0.0f, 0.0f };
            Barrier(Depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            List->ClearRenderTargetView(RayRtvAt(2), Floor, 0, nullptr);
            Barrier(Depth, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        }
        Execute();
        WaitGpu();
    }

    // What a frame brings: the three bodies of the frame's buffer, their texture places, and the passes' tables.
    void FillRayFrame(int Frame, Host64::FRayBody Bodies[4])
    {
        BuildRayScene(Frame);
        const uint32_t InFrame[3] = { 0, 1, 3 };
        UINT First = 0;
        uint32_t Geo[5] = {};
        for (uint32_t b : InFrame)
        {
            const UINT N = WriteCorners(RayUpMapped + UP_FRAME_XYZ + First * FRAME_STRIDE, FRAME_STRIDE, 1, b);
            WriteUvw(RayUpMapped + UP_FRAME_UVW + First * 12, b);
            Bodies[b].Offset   = First * FRAME_STRIDE;
            Bodies[b].Stride   = FRAME_STRIDE;
            Bodies[b].Vertices = N;
            Bodies[b].Flags    = (b == 0) ? 0u : (uint32_t)Host64::RayBody_Asks;
            Geo[b]             = First;
            First += N;
        }
        Bodies[2].Offset   = 0;
        Bodies[2].Stride   = 12;
        Bodies[2].Vertices = 6;
        Bodies[2].Flags    = Host64::RayBody_Arena;
        Geo[2]             = 0x80000000u;
        // After the geometries, the light's row of bits: the bodies its rays pass - body 3.
        Geo[4] = 1u << 3;

        BYTE* const Tables = RayUpMapped + UP_TABLES;
        memset(Tables, 0, TABLE_BYTES);
        FShadowConsts S = {};
        S.TanX       = 1.0f;
        S.TanY       = 1.0f;
        S.Scale      = 1;
        S.Count      = 1;
        S.NoOnTop    = 1;
        S.Flags      = 4;               // the level casts as the bodies do: whatever is in the way blocks
        S.Calib      = 1.0f;
        S.MaxShare   = 1.0f;
        S.AxisX[0]   = 1.0f;
        S.AxisY[1]   = 1.0f;
        S.AxisZ[2]   = -1.0f;
        S.Near       = 1.0f;
        S.LightSize  = 0.5f;
        S.ViewRect[2] = S.ViewRect[3] = 1.0f;
        S.Eye[2]     = RayEyeZ;
        S.LightCam[0][0]  = RayLight[0];
        S.LightCam[0][1]  = RayLight[1];
        S.LightCam[0][2]  = RayEyeZ - RayLight[2];
        S.LightCam[0][3]  = RayLightReach;
        S.LightInfo[0][1] = 1.0f;
        S.LightInfo[0][2] = 1.0f;
        S.LightSpot[0][3] = -2.0f;      // a point light
        memcpy(Tables + AT_FACTOR, &S, sizeof(S));
        FRtShadowConsts R = {};
        R.AoParams[0] = RayAoRadius;
        R.AoParams[1] = 1.0f;
        R.RtKnobs[3]  = 0xFFFFFFFFu;    // no body the occlusion passes
        R.RtRows[0]   = 4;              // the rows of bits begin after the four geometries
        R.RtRows[1]   = 1;
        memcpy(Tables + AT_RAYS, &R, sizeof(R));
        FListConsts Lc = {};
        Lc.ListKnobs[0] = LIST_ROOM;
        Lc.ListKnobs[1] = 3;
        memcpy(Tables + AT_LIST, &Lc, sizeof(Lc));
        memcpy(Tables + AT_GEO, Geo, sizeof(Geo));
    }

    // A frame's results held against what they must be. Returns how many were wrong; the first few are told.
    int CheckRayFrame(int Frame, uint32_t Passes, int Told[3], int Checked[3])
    {
        const BYTE*       Back = nullptr;
        const D3D12_RANGE Read = { 0, (SIZE_T)BACK_BYTES };
        if (FAILED(RayBack->Map(0, &Read, (void**)&Back)))
            return 1;
        int Wrong = 0;
        if (Passes & Host64::RayPass_List)
            for (int a = 0; a < RayAskCount; a++)
            {
                const FRayAsk&  A = RayAsks[a];
                const uint32_t* G = (const uint32_t*)(Back + BACK_LIST + (size_t)a * RDX_RAY_SLOT_BYTES);
                const float     O[3] = { A.X, A.Y, RayEyeZ }, D[3] = { 0.0f, 0.0f, A.Dz };
                float           At = 0.0f;
                const FRayTri*  T = A.bAsked ? TraceHere(O, D, (A.From > 0.001f) ? A.From : 0.001f, 1000.0f,
                                                         A.Mask ? A.Mask : 0x03u, At)
                                             : nullptr;
                float Got;
                memcpy(&Got, &G[0], 4);
                const uint32_t Mark = 0xA5A5A5A5u;
                const bool bOk = !A.bAsked ? (G[0] == Mark && G[1] == Mark && G[2] == Mark && G[3] == Mark)
                               : !T        ? (G[0] == 0xFFFFFFFFu)
                                           : (fabsf(Got - At) < 0.01f && (G[1] >> 30) == T->Instance &&
                                              (G[1] & 0x00FFFFFFu) == T->Geometry && G[2] == T->Prim);
                Checked[0]++;
                if (!bOk)
                {
                    Wrong++;
                    if (Told[0]++ < 6)
                        printf("  rays: frame %d, ray %d: got %08X %08X %08X %08X (distance %.3f), expected %s "
                               "(instance %u, geometry %u, triangle %u at %.3f)\n", Frame, a, G[0], G[1], G[2], G[3], Got,
                               !A.bAsked ? "the slot left alone" : !T ? "nothing met" : "a hit", T ? T->Instance : 0u,
                               T ? T->Geometry : 0u, T ? T->Prim : 0u, At);
                }
            }
        // The factor: over the floor a point every sixty pixels. Where the light's being blocked does not turn within a
        // few units around - not at a shadow's edge - a lit point is 1, and a shadowed one has lost the light's share.
        const BYTE* const Factor = Back + BACK_FACTOR;
        const BYTE* const Ao     = Back + BACK_AO;
        for (int Py = 30; Py < (int)RH; Py += 60)
            for (int Px = 30; Px < (int)RW; Px += 60)
            {
                const float X = WorldX(Px), Y = WorldY(Py);
                const size_t At = (size_t)Py * RayPicture.Footprint.RowPitch + Px;
                if (Passes & Host64::RayPass_Shadow)
                {
                    const bool bBlocked = LightBlocked(X, Y);
                    if (LightBlocked(X - 2.5f, Y) == bBlocked && LightBlocked(X + 2.5f, Y) == bBlocked &&
                        LightBlocked(X, Y - 2.5f) == bBlocked && LightBlocked(X, Y + 2.5f) == bBlocked)
                    {
                        const float Dx = RayLight[0] - X, Dy = RayLight[1] - Y;
                        const float Att = 1.0f - sqrtf(Dx * Dx + Dy * Dy + RayLight[2] * RayLight[2]) / RayLightReach;
                        const int   Want = bBlocked ? (int)(255.0f * (1.0f - Att * Att) + 0.5f) : 255;
                        Checked[1]++;
                        if (abs((int)Factor[At] - Want) > 3)
                        {
                            Wrong++;
                            if (Told[1]++ < 6)
                                printf("  rays: frame %d, the factor at the floor's (%.1f, %.1f): %d, expected %d (%s)\n",
                                       Frame, X, Y, (int)Factor[At], Want, bBlocked ? "shadowed" : "lit");
                        }
                    }
                }
                if (Passes & Host64::RayPass_Ao)
                {
                    // The occlusion: open - 1 - where no body is within its rays' reach; darker under a body's middle.
                    bool bOpen = true, bUnder = false;
                    for (int b = 0; b < 4; b++)
                    {
                        const FRayBox& B = RayBoxes[b];
                        const float Ox = (X < B.X0) ? B.X0 - X : (X > B.X1) ? X - B.X1 : 0.0f;
                        const float Oy = (Y < B.Y0) ? B.Y0 - Y : (Y > B.Y1) ? Y - B.Y1 : 0.0f;
                        bOpen  = bOpen && sqrtf(Ox * Ox + Oy * Oy) > RayAoRadius + 2.0f;
                        // Bodies 0 and 2 are opaque and low: straight under their middle a ray upwards meets them.
                        bUnder = bUnder || ((b == 0 || b == 2) && X > B.X0 + 5.0f && X < B.X1 - 5.0f && Y > B.Y0 + 5.0f &&
                                            Y < B.Y1 - 5.0f);
                    }
                    if (bOpen || bUnder)
                    {
                        Checked[2]++;
                        if (bOpen ? (Ao[At] < 254) : (Ao[At] > 240))
                        {
                            Wrong++;
                            if (Told[2]++ < 6)
                                printf("  rays: frame %d, the occlusion at the floor's (%.1f, %.1f): %d, expected %s\n", Frame,
                                       X, Y, (int)Ao[At], bOpen ? "255 (open)" : "under 240 (a body over it)");
                        }
                    }
                }
            }
        const D3D12_RANGE None = { 0, 0 };
        RayBack->Unmap(0, &None);
        return Wrong;
    }

    // A whole R8 picture of the rays' size into the readback buffer at At.
    void ReadPicture(ID3D12Resource* From, UINT64 At)
    {
        D3D12_TEXTURE_COPY_LOCATION Src = {}, Dst = {};
        Src.pResource          = From;
        Src.Type               = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Dst.pResource          = RayBack;
        Dst.Type               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Dst.PlacedFootprint    = RayPicture;
        Dst.PlacedFootprint.Offset = At;
        Barrier(From, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        List->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        Barrier(From, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    // ---- What a frame's rays cost ---------------------------------------------------------------------------------

    // A scene the size of a level, for the cost alone - nothing of it is checked: a bumpy floor with pillars on it, as
    // many triangles as a level of the game has; bodies over it as many and as large as a busy frame's, some of them
    // masked; a ray a pixel that goes down at a slant, each pixel its own; and twenty lights.
    const int   CostGrid    = 190;           // the floor: this many squares each way
    const int   CostPillars = 400;
    const int   CostBodies  = 72;
    const int   CostMasked  = 13;
    const int   CostPatch   = 12;            // a body: this many squares each way
    const int   CostLights  = 20;
    const UINT  CostUpBytes = 176u << 20;

    float CostHash(int A, int B)
    {
        unsigned V = (unsigned)A * 73856093u ^ (unsigned)B * 19349663u;
        V = (V ^ (V >> 13)) * 1274126177u;
        return (float)(V & 0xFFFFu) / 65535.0f;
    }

    float CostFloor(float X, float Y) { return 1.5f * sinf(X * 0.11f) * cosf(Y * 0.07f); }

    void CostCorner(float*& Out, float X, float Y, float Z)
    {
        *Out++ = X;
        *Out++ = Y;
        *Out++ = Z;
    }

    // A square as two triangles, its corners given in turn.
    void CostSquare(float*& Out, const float A[3], const float B[3], const float C[3], const float D[3])
    {
        const float* const Order[6] = { A, B, C, A, C, D };
        for (const float* P : Order)
            CostCorner(Out, P[0], P[1], P[2]);
    }

    // The cost series. Returns what went wrong along the way - a refused frame, a debug layer's error - not what the
    // pictures hold.
    int RayCost(FHost64Link& Link, uint32_t Passes, int Frames)
    {
        // The level's and the bodies' own shared buffers, as large as this scene asks; the textures are the check's.
        const UINT LevelCorners = (UINT)(CostGrid * CostGrid * 6 + CostPillars * 24);
        const UINT BodyCorners  = (UINT)(CostPatch * CostPatch * 6);
        const UINT FrameCorners = BodyCorners * (UINT)CostBodies;
        ID3D12Resource* Cost[Host64::Ray_Count] = {};
        memcpy(Cost, RayRes, sizeof(Cost));
        Cost[Host64::Ray_Level] = Cost[Host64::Ray_FrameXyz] = Cost[Host64::Ray_FrameUvw] = nullptr;
        Cost[Host64::Ray_Sky] = Cost[Host64::Ray_Thin] = Cost[Host64::Ray_ArenaXyz] = Cost[Host64::Ray_ArenaUvw] = nullptr;
        // The list as long as a ray a pixel asks: the count's slot and one for every pixel.
        const UINT ListSlots = 1 + RW * RH;
        const bool bList     = RayRes[Host64::Ray_Requests] != nullptr;
        Cost[Host64::Ray_Requests] = Cost[Host64::Ray_Answers] = nullptr;
        D3D12_RESOURCE_DESC ListDesc = BufferDesc((UINT64)ListSlots * RDX_RAY_SLOT_BYTES);
        ListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* Up     = nullptr;
        BYTE*           Mapped = nullptr;
        D3D12_HEAP_PROPERTIES UpHeap = {};
        UpHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_DESC UpDesc = BufferDesc(CostUpBytes);
        const D3D12_RESOURCE_STATES Common = D3D12_RESOURCE_STATE_COMMON;
        if (FAILED(FHost64Link::MakeShared(Device, BufferDesc((UINT64)LevelCorners * 12), Common, nullptr,
                                           &Cost[Host64::Ray_Level])) ||
            FAILED(FHost64Link::MakeShared(Device, BufferDesc((UINT64)FrameCorners * 12), Common, nullptr,
                                           &Cost[Host64::Ray_FrameXyz])) ||
            FAILED(FHost64Link::MakeShared(Device, BufferDesc((UINT64)FrameCorners * 12), Common, nullptr,
                                           &Cost[Host64::Ray_FrameUvw])) ||
            (bList && (FAILED(FHost64Link::MakeShared(Device, ListDesc, Common, nullptr,
                                                      &Cost[Host64::Ray_Requests])) ||
                       FAILED(FHost64Link::MakeShared(Device, ListDesc, Common, nullptr,
                                                      &Cost[Host64::Ray_Answers])))) ||
            FAILED(Device->CreateCommittedResource(&UpHeap, D3D12_HEAP_FLAG_NONE, &UpDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&Up))) ||
            FAILED(Up->Map(0, nullptr, (void**)&Mapped)))
        {
            printf("rays, their cost: the scene's buffers could not be made\n");
            return 1;
        }
        // Where things are in the upload buffer.
        const UINT AtLevel = 0, AtFrame = 4u << 20, AtUvw = 6u << 20, AtTables = 8u << 20, AtCounts = 9u << 20,
                   AtRequests = 10u << 20;
        // The floor and the pillars.
        float* Out = (float*)(Mapped + AtLevel);
        const float Step = 200.0f / (float)CostGrid;
        for (int j = 0; j < CostGrid; j++)
            for (int i = 0; i < CostGrid; i++)
            {
                const float X0 = -100.0f + Step * (float)i, X1 = X0 + Step, Y0 = -100.0f + Step * (float)j, Y1 = Y0 + Step;
                const float A[3] = { X0, Y0, CostFloor(X0, Y0) }, B[3] = { X1, Y0, CostFloor(X1, Y0) },
                            C[3] = { X1, Y1, CostFloor(X1, Y1) }, D[3] = { X0, Y1, CostFloor(X0, Y1) };
                CostSquare(Out, A, B, C, D);
            }
        for (int p = 0; p < CostPillars; p++)
        {
            const float X = -95.0f + 190.0f * CostHash(p, 1), Y = -95.0f + 190.0f * CostHash(p, 2);
            const float R = 0.6f + 1.4f * CostHash(p, 3), Top = 8.0f + 30.0f * CostHash(p, 4);
            const float Xs[4] = { X - R, X + R, X + R, X - R }, Ys[4] = { Y - R, Y - R, Y + R, Y + R };
            for (int s = 0; s < 4; s++)
            {
                const int   t = (s + 1) & 3;
                const float A[3] = { Xs[s], Ys[s], -2.0f }, B[3] = { Xs[t], Ys[t], -2.0f }, C[3] = { Xs[t], Ys[t], Top },
                            D[3] = { Xs[s], Ys[s], Top };
                CostSquare(Out, A, B, C, D);
            }
        }
        // The bodies: a bumpy patch each, and their texture places - the first CostMasked of them masked.
        memset(Mapped + AtTables, 0, TABLE_BYTES);
        Host64::FRayBody* const Bodies = (Host64::FRayBody*)calloc((size_t)CostBodies, sizeof(Host64::FRayBody));
        uint32_t* const Geo = (uint32_t*)(Mapped + AtTables + AT_GEO);
        Out = (float*)(Mapped + AtFrame);
        float* Uvw = (float*)(Mapped + AtUvw);
        for (int b = 0; b < CostBodies; b++)
        {
            const float Cx = -90.0f + 180.0f * CostHash(b, 11), Cy = -90.0f + 180.0f * CostHash(b, 12);
            const float Cz = 5.0f + 15.0f * CostHash(b, 13), Half = 3.0f + 4.0f * CostHash(b, 14);
            const float Slot = (b < CostMasked) ? 1.0f : 0.0f;
            const float Side = 2.0f * Half / (float)CostPatch;
            for (int j = 0; j < CostPatch; j++)
                for (int i = 0; i < CostPatch; i++)
                {
                    const float X0 = Cx - Half + Side * (float)i, X1 = X0 + Side, Y0 = Cy - Half + Side * (float)j, Y1 = Y0 + Side;
                    const float A[3] = { X0, Y0, Cz + 0.3f * CostFloor(X0 * 3.0f, Y0 * 3.0f) },
                                B[3] = { X1, Y0, Cz + 0.3f * CostFloor(X1 * 3.0f, Y0 * 3.0f) },
                                C[3] = { X1, Y1, Cz + 0.3f * CostFloor(X1 * 3.0f, Y1 * 3.0f) },
                                D[3] = { X0, Y1, Cz + 0.3f * CostFloor(X0 * 3.0f, Y1 * 3.0f) };
                    CostSquare(Out, A, B, C, D);
                    const float* const Order[6] = { A, B, C, A, C, D };
                    for (const float* P : Order)
                        CostCorner(Uvw, (P[0] - (Cx - Half)) / (2.0f * Half), (P[1] - (Cy - Half)) / (2.0f * Half), Slot);
                }
            Bodies[b].Offset   = (uint32_t)b * BodyCorners * 12;
            Bodies[b].Stride   = 12;
            Bodies[b].Vertices = BodyCorners;
            Bodies[b].Flags    = (b < CostMasked) ? (uint32_t)Host64::RayBody_Asks : 0u;
            Geo[b]             = (uint32_t)b * BodyCorners;
        }
        // The tables: the check's own, with twenty lights over the floor and the rows of bits a light after the bodies.
        Host64::FRayBody Unused[4] = {};
        FillRayFrame(1, Unused);
        memcpy(Mapped + AtTables, RayUpMapped + UP_TABLES, AT_GEO);
        FShadowConsts* const S = (FShadowConsts*)(Mapped + AtTables + AT_FACTOR);
        S->Count = (uint32_t)CostLights;
        for (int l = 0; l < CostLights; l++)
        {
            S->LightCam[l][0]  = -90.0f + 180.0f * CostHash(l, 21);
            S->LightCam[l][1]  = -90.0f + 180.0f * CostHash(l, 22);
            S->LightCam[l][2]  = RayEyeZ - 45.0f;
            S->LightCam[l][3]  = 150.0f;
            S->LightInfo[l][1] = 1.0f;
            S->LightInfo[l][2] = 1.0f;
            S->LightSpot[l][3] = -2.0f;
        }
        FRtShadowConsts* const R = (FRtShadowConsts*)(Mapped + AtTables + AT_RAYS);
        R->RtKnobs[0] = 6;                                  // the lights a pixel traces, and the samples an edge gets:
        R->RtKnobs[1] = 4;                                  // what the renderer asks for by itself
        R->RtRows[0]  = (uint32_t)CostBodies;
        R->RtRows[1]  = (uint32_t)((CostBodies + 31) / 32);
        FListConsts* const Lc = (FListConsts*)(Mapped + AtTables + AT_LIST);
        Lc->ListKnobs[0] = ListSlots;
        Lc->ListKnobs[1] = 0xFFFFFFFFu;
        // A ray a pixel: from the eye's height over the pixel's own point of the floor, down at a slant of its own -
        // each in its slot after the count's. The count's slot three ways: no ray asked, a quarter of them, all.
        uint32_t* const Requests = (uint32_t*)(Mapped + AtRequests);
        for (UINT y = 0; y < RH; y++)
            for (UINT x = 0; x < RW; x++)
            {
                const float Sx = 0.6f * sinf((float)x * 0.05f), Sy = 0.6f * cosf((float)y * 0.07f);
                WriteRequest(Requests + 8 * (size_t)(1 + y * RW + x), WorldX((int)x), WorldY((int)y), RayEyeZ, Sx, Sy,
                             -1.0f, 1000.0f, RDX_RAY_KIND_FIRST, 3u, 0u);
            }
        // The same rays once more, scattered over the list - a ray's slot far from those of its neighbours on the
        // screen -, as a list holds them that a surface's pixels wrote in no order: what the order costs the host.
        const UINT AtScattered = AtRequests + ((ListSlots * RDX_RAY_SLOT_BYTES + 0xFFFFu) & ~0xFFFFu);
        uint32_t* const Scattered = (uint32_t*)(Mapped + AtScattered);
        const UINT64 Rays = (UINT64)RW * RH;
        for (UINT64 i = 0; i < Rays; i++)
            memcpy(Scattered + 8 * (size_t)(1 + i), Requests + 8 * (size_t)(1 + (i * 1000003ull) % Rays),
                   RDX_RAY_SLOT_BYTES);
        const UINT Counts[3] = { 1, 1 + (RW / 2) * (RH / 2), ListSlots };
        for (int c = 0; c < 3; c++)
        {
            uint32_t* const Head = (uint32_t*)(Mapped + AtCounts + c * RDX_RAY_SLOT_BYTES);
            memset(Head, 0, RDX_RAY_SLOT_BYTES);
            Head[0] = Counts[c];
            Head[1] = ListSlots;
        }

        int Bad = 0;
        if (!Link.ShareRays(Cost, LevelCorners, 0, 0, (uint32_t)CostBodies, 20000))
        {
            printf("rays, their cost: the host did not take the share: %s\n", Link.Message());
            Bad = 1;
        }
        else
        {
            Open(0);
            List->CopyBufferRegion(Cost[Host64::Ray_Level], 0, Up, AtLevel, (UINT64)LevelCorners * 12);
            Execute();
            WaitGpu();
            printf("rays, their cost: a level of %u triangles, %d bodies with %u triangles a frame (%d of them masked), "
                   "%d lights; the host's own GPU time for a frame's rays:\n", LevelCorners / 3, CostBodies, FrameCorners / 3,
                   CostMasked, CostLights);
            const uint32_t Shadow = Host64::RayPass_Shadow, Ao = Host64::RayPass_Ao, Listed = Host64::RayPass_List;
            // Asked: which of the count's three slots the list starts with - 0 no ray, 1 a quarter of them, 2 all.
            // Order: the rays in the list as the screen has them (0), or scattered over it (1).
            const struct { const char* Name; uint32_t Run; UINT Wide, High; int Asked, Order; } Series[] = {
                { "the scene alone (the bodies' structure and the top level made, no ray asked)", Listed, RW, RH, 0,
                  0 },
                { "the list: a ray for every pixel of 1920x1200 and what it met", Listed, RW, RH, 2, 0 },
                { "the list: the same rays, scattered over the list", Listed, RW, RH, 2, 1 },
                { "the list: a ray for every pixel of 960x600 and what it met", Listed, RW / 2, RH / 2, 1, 0 },
                { "the shadows' factor, 1920x1200", Shadow, RW, RH, -1, 0 },
                { "the shadows' factor, 960x600 (the game's size on this panel)", Shadow, RW / 2, RH / 2, -1, 0 },
                { "the occlusion, 1920x1200", Ao, RW, RH, -1, 0 },
                { "the occlusion, 960x600 (the game's size on this panel)", Ao, RW / 2, RH / 2, -1, 0 },
                { "the factor and the occlusion, 960x600 - a frame of the game's", Shadow | Ao, RW / 2, RH / 2, -1, 0 },
            };
            int Filled = -1, Counted = -1;
            for (const auto& Sr : Series)
            {
                if ((Sr.Run & Passes) != Sr.Run || ((Sr.Run & Listed) && !bList))
                    continue;
                // The asked rays, once, and the count this series' list starts with.
                if (Sr.Asked >= 0 && (Filled != Sr.Order || Counted != Sr.Asked))
                {
                    Open(0);
                    if (Filled != Sr.Order)
                        List->CopyBufferRegion(Cost[Host64::Ray_Requests], 0, Up, Sr.Order ? AtScattered : AtRequests,
                                               (UINT64)ListSlots * RDX_RAY_SLOT_BYTES);
                    List->CopyBufferRegion(Cost[Host64::Ray_Requests], 0, Up, AtCounts + Sr.Asked * RDX_RAY_SLOT_BYTES,
                                           RDX_RAY_SLOT_BYTES);
                    Execute();
                    WaitGpu();
                    Filled  = Sr.Order;
                    Counted = Sr.Asked;
                }
                const uint32_t FailedBefore = Link.FramesFailed();
                LARGE_INTEGER  T0, T1;
                QueryPerformanceCounter(&T0);
                for (int f = 1; f <= Frames; f++)
                {
                    // The frame's bodies and tables copied over anew, as the renderer copies them every frame.
                    Open(0);
                    List->CopyBufferRegion(Cost[Host64::Ray_FrameXyz], 0, Up, AtFrame, (UINT64)FrameCorners * 12);
                    List->CopyBufferRegion(Cost[Host64::Ray_FrameUvw], 0, Up, AtUvw, (UINT64)FrameCorners * 12);
                    List->CopyBufferRegion(Cost[Host64::Ray_Tables], 0, Up, AtTables, TABLE_BYTES);
                    Execute();
                    Host64::FRayFrame* const Fr = Link.RayFrame();
                    if (!Fr)
                    {
                        Bad++;
                        break;
                    }
                    Fr->Passes         = Sr.Run;
                    Fr->Bodies         = (uint32_t)CostBodies;
                    Fr->Width          = Sr.Wide;
                    Fr->Height         = Sr.High;
                    Fr->FactorConstsAt = AT_FACTOR;
                    Fr->RayConstsAt    = AT_RAYS;
                    Fr->GeoAt          = AT_GEO;
                    Fr->ListConstsAt   = AT_LIST;
                    Fr->Flags          = 0;
                    Fr->ListRoom       = (Sr.Run & Listed) ? ListSlots : 0;
                    Fr->WaterBytes     = 0;
                    Fr->PassedRoom     = 0;
                    memcpy(Fr + 1, Bodies, (size_t)CostBodies * sizeof(Host64::FRayBody));
                    Host64::FFrame Fm = {};
                    Fm.Services = Host64::Service_Rays;
                    if (!Link.Submit(Queue, Fm))
                    {
                        Bad++;
                        break;
                    }
                    Open(1);
                    Execute();
                    WaitGpu();
                }
                QueryPerformanceCounter(&T1);
                const double Whole = 1000.0 * (double)(T1.QuadPart - T0.QuadPart) / (double)Freq.QuadPart / (double)Frames;
                printf("  %-78s %6.3f ms (a frame here, with the crossing: %.3f ms)\n", Sr.Name,
                       (double)((Sr.Run & Listed) ? Link.WorkListUs() : Link.WorkRayUs()) / 1000.0, Whole);
                Bad += (int)(Link.FramesFailed() - FailedBefore);
            }
        }
        Bad += DrainDebug();
        // Ours go once the GPU is done with them; the host holds the shared three until its link stops.
        WaitGpu();
        free(Bodies);
        Up->Unmap(0, nullptr);
        Up->Release();
        Cost[Host64::Ray_Level]->Release();
        Cost[Host64::Ray_FrameXyz]->Release();
        Cost[Host64::Ray_FrameUvw]->Release();
        if (Cost[Host64::Ray_Requests])
            Cost[Host64::Ray_Requests]->Release();
        if (Cost[Host64::Ray_Answers])
            Cost[Host64::Ray_Answers]->Release();
        return Bad;
    }

    // The rays' part. The host again, with the Test backend - what a card traces is no library's.
    int RaySeries(FHost64Link& Link, const wchar_t* HostExe, int Frames, bool& bAlive)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 O5 = {};
        Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &O5, sizeof(O5));
        printf("rays: this %s process's own device: ray tracing tier %d.%d\n", sizeof(void*) == 4 ? "32-bit" : "64-bit",
               (int)O5.RaytracingTier / 10, (int)O5.RaytracingTier % 10);
        if (!Link.Start(Device, HostExe, Host64::Backend_Test, 20000))
        {
            printf("rays: the host did not come up: %s\n", Link.Message());
            return 1;
        }
        const uint32_t Passes = Link.RayPasses();
        if (!Passes)
        {
            printf("rays: the host traces none on this card (its log says why) - nothing to try\n");
            Link.Stop();
            return 0;
        }
        printf("rays: the host traces them; its passes: shadows %s, occlusion %s, the list %s\n",
               (Passes & Host64::RayPass_Shadow) ? "yes" : "NO", (Passes & Host64::RayPass_Ao) ? "yes" : "NO",
               (Passes & Host64::RayPass_List) ? "yes" : "NO");
        // Where the card traces at all, every pass has to have come up.
        const uint32_t All = (uint32_t)Host64::RayPass_Shadow | Host64::RayPass_Ao | Host64::RayPass_List;
        int Bad = (Passes == All) ? 0 : 1;
        if (!MakeRayResources())
        {
            printf("rays: what the rays' part shares could not be made\n");
            Link.Stop();
            return Bad + 1;
        }
        const uint32_t Run = Passes & (RayRes[Host64::Ray_Answers] ? ~0u : ~(uint32_t)Host64::RayPass_List);
        if (!Link.ShareRays(RayRes, 6, 6, 6, 16, 20000))
        {
            printf("rays: the host did not take the share: %s\n", Link.Message());
            Link.Stop();
            return Bad + 1;
        }
        FillRayStatics();
        Bad += DrainDebug();

        double* T = (double*)calloc((size_t)Frames, sizeof(double));
        int     N = 0, Wrong = 0, Told[3] = {}, Checked[3] = {};
        const uint32_t FailedBefore = Link.FramesFailed();
        for (int f = 1; f <= Frames; f++)
        {
            LARGE_INTEGER T0, T1;
            QueryPerformanceCounter(&T0);
            Host64::FRayBody Bodies[4] = {};
            FillRayFrame(f, Bodies);
            Open(0);
            CopyUp(Host64::Ray_FrameXyz, UP_FRAME_XYZ, 18 * FRAME_STRIDE);
            CopyUp(Host64::Ray_FrameUvw, UP_FRAME_UVW, 18 * 12);
            CopyUp(Host64::Ray_Tables, UP_TABLES, TABLE_BYTES);
            Execute();

            Host64::FRayFrame* const Fr = Link.RayFrame();
            if (!Fr)
            {
                Bad++;
                break;
            }
            Fr->Passes         = Run;
            Fr->Bodies         = 4;
            Fr->Width          = RW;
            Fr->Height         = RH;
            Fr->FactorConstsAt = AT_FACTOR;
            Fr->RayConstsAt    = AT_RAYS;
            Fr->GeoAt          = AT_GEO;
            Fr->ListConstsAt   = AT_LIST;
            Fr->Flags          = 0;
            Fr->ListRoom       = (Run & Host64::RayPass_List) ? (uint32_t)LIST_ROOM : 0u;
            Fr->WaterBytes     = 0;
            Fr->PassedRoom     = 0;
            memcpy(Fr + 1, Bodies, sizeof(Bodies));
            Host64::FFrame Fm = {};
            Fm.Services = Host64::Service_Rays;
            if (!Link.Submit(Queue, Fm))
            {
                Bad++;
                break;
            }

            // What the passes wrote: the asked rays' answers, and the two pictures whole.
            Open(1);
            if (Run & Host64::RayPass_List)
            {
                ID3D12Resource* const Answers = RayRes[Host64::Ray_Answers];
                Barrier(Answers, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                List->CopyBufferRegion(RayBack, BACK_LIST, Answers, RDX_RAY_SLOT_BYTES,
                                       (UINT64)RayAskCount * RDX_RAY_SLOT_BYTES);
                Barrier(Answers, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
            if (Run & Host64::RayPass_Shadow)
                ReadPicture(RayRes[Host64::Ray_Factor], BACK_FACTOR);
            if (Run & Host64::RayPass_Ao)
                ReadPicture(RayRes[Host64::Ray_Occlusion], BACK_AO);
            Execute();
            WaitGpu();
            QueryPerformanceCounter(&T1);
            Wrong += CheckRayFrame(f, Run, Told, Checked);
            if (f > 10)
                T[N++] = 1000.0 * (double)(T1.QuadPart - T0.QuadPart) / (double)Freq.QuadPart;
        }
        const uint32_t Refused = Link.FramesFailed() - FailedBefore;
        char Tail[160];
        sprintf_s(Tail, "; the host refused %u; checked: %d rays, %d points of the factor, %d of the occlusion", Refused,
                  Checked[0], Checked[1], Checked[2]);
        Report("rays through the host", T, N, Wrong, Tail);
        free(T);
        // A frame with the list in it is timed as the list's.
        printf("  the host's own GPU time for a frame's rays (this little scene, %ux%u): %.3f ms\n", RW, RH,
               (double)((Run & Host64::RayPass_List) ? Link.WorkListUs() : Link.WorkRayUs()) / 1000.0);
        Bad += Wrong + (int)Refused + DrainDebug();
        // And what a frame's rays cost on this card, in a scene the size of a level.
        Bad += RayCost(Link, Run, 120);
        bAlive = Link.CheckAlive(1000) && bAlive;
        Bad += HostErrors(Link);
        Link.Stop();
        return Bad;
    }
}

int wmain(int Argc, wchar_t** Argv)
{
    QueryPerformanceFrequency(&Freq);
    int  Frames    = 300;
    bool bRaysOnly = false;
    for (int a = 1; a < Argc; a++)
    {
        if (_wcsicmp(Argv[a], L"--debug") == 0)
            bDebug = true;
        else if (_wcsicmp(Argv[a], L"--rays") == 0)
            bRaysOnly = true;
        else
            Frames = _wtoi(Argv[a]);
    }
    wchar_t HostExe[MAX_PATH];
    GetModuleFileNameW(nullptr, HostExe, MAX_PATH);
    wchar_t* Slash = wcsrchr(HostExe, L'\\');
    if (Slash)
        wcscpy_s(Slash + 1, MAX_PATH - (Slash + 1 - HostExe), L"..\\Release\\SecondRevisionHost64.exe");
    printf("Host64Probe: %d frames at %ux%u, %s process%s, host: %ls\n", Frames, W, H,
           sizeof(void*) == 4 ? "32-bit" : "64-bit", bDebug ? ", debug layer" : "", HostExe);
    if (!MakeEverything())
    {
        printf("FAILED: the device or the textures could not be made\n");
        return 1;
    }
    const LUID Luid = Device->GetAdapterLuid();
    printf("  device on adapter %08lx:%08lx\n", (unsigned long)Luid.HighPart, (unsigned long)Luid.LowPart);

    FHost64Link Link;
    Link.SetLog(Print);
    Link.SetDebugLayer(bDebug);
    int  Bad    = 0;
    bool bAlive = true;
    if (!bRaysOnly)
    {
        if (!Link.Start(Device, HostExe, Host64::Backend_Test, 5000))
        {
            printf("FAILED: the host did not come up: %s\n", Link.Message());
            return 2;
        }
        ID3D12Resource* Shared[Host64::Resource_Count] = {};
        Shared[Host64::Resource_Color]  = Color;
        Shared[Host64::Resource_Output] = Output;
        if (!Link.Share(Shared, 5000))
        {
            printf("FAILED: the host did not take the textures: %s\n", Link.Message());
            return 3;
        }
        double IdleHost = 0, IdleHere = 0, BusyHost = 0, BusyHere = 0;
        Bad += TestSeries("idle GPU, through the host", Frames, &Link, false, IdleHost);
        Bad += TestSeries("idle GPU, here", Frames, nullptr, false, IdleHere);
        Bad += TestSeries("busy GPU, through the host", Frames, &Link, true, BusyHost);
        Bad += TestSeries("busy GPU, here", Frames, nullptr, true, BusyHere);
        bAlive = Link.CheckAlive(1000);
        Bad += HostErrors(Link);
        Link.Stop();
        printf("the crossing costs %.3f ms a frame with the GPU idle, %.3f ms with it busy; host alive at the end: %s\n",
               IdleHost - IdleHere, BusyHost - BusyHere, bAlive ? "yes" : "NO");

        // The host again, with FSR: every card has to run it.
        if (!LibrarySeries(Link, HostExe, Host64::Backend_Fsr, "FSR", Frames, Bad, bAlive))
            Bad++;
        // And with DLSS. On NVIDIA's card it has to come up; on any other the host's refusal is the right answer, and
        // what is tried is that it refuses and says why.
        const bool bNvidia = AdapterVendor == 0x10DEu;
        if (!LibrarySeries(Link, HostExe, Host64::Backend_Dlss, "DLSS", Frames, Bad, bAlive))
        {
            if (bNvidia)
                Bad++;
            else
                printf("DLSS: refused on a card that is not NVIDIA's - as it should be\n");
        }
        else if (!bNvidia)
        {
            printf("DLSS: the host came up with it on a card that is not NVIDIA's - unexpected\n");
            Bad++;
        }
    }
    // Last the rays, where the card traces any.
    Bad += RaySeries(Link, HostExe, Frames, bAlive);
    printf("%s\n", (Bad == 0 && bAlive) ? "RESULT ok" : "RESULT FAILED");
    return (Bad == 0 && bAlive) ? 0 : 4;
}

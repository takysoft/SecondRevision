// ProbeCheck - the probe gathered from the picture (MirrorProbe.hlsl; docs/tukrozodes-szonda-terv.md) tried on the GPU
// without the game.
//
// The shader's passes run as the renderer runs them, on a room made up here: a box-shaped room with a pillar of the
// level's in it and an actor's box, seen from a camera inside - its picture, its depth (the view depth's reciprocal, as
// the renderer's copy holds it) and its surfaces' kinds (the stencil's, in the second channel), all worked out on the CPU.
// What the passes leave - the cells and the pyramid - is read back and held against what RdxMath's rules give for the
// same pixels:
//   - every cell the frame's pixels reach is known, and holds the nearest of them, with that pixel's colour;
//   - each cell's point falls in that very cell as RdxMath lays the cube's faces (the shader's faces are the same);
//   - nothing of the actor's box is kept, and every point kept lies on the room or the pillar;
//   - the pyramid is the cells' (RdxMath::ProbeMipOf up, the holes filled from above at most RDX_PROBE_FILL_UP levels);
//   - the march of the shaders' shared block (through ProbeCheckMarch.hlsl) finds what RdxMath::ProbeMarch finds on the
//     same pyramid;
// then a second frame from elsewhere with the pillar gone: what the eye now sees past is carved out, what it sees where
// it was is kept; and a third with the probe moved, whose picture gives it nothing and takes nothing from it: every
// point it held goes with it, into the cell it falls in round the new centre, the nearest where several do. The
// counting frames' numbers are held against the cells read back.
//
//     ProbeCheck.exe [--debug]        --debug: the D3D12 debug layer

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <vector>

#include "../../src/RevisionDX12/RenderMath.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using RdxMath::FVec3;
using RdxMath::FProbeTexel;

namespace
{
    const UINT  W = 640, H = 360;
    const float TanX = 1.0f, TanY = 0.5625f;
    const int   Side    = RdxMath::ProbeSide;
    const int   Cells   = 6 * Side * Side;
    const int   Entries = RdxMath::ProbeEntries();
    const int   Rays    = 4096;
    const float Reach   = 2048.0f;
    const UINT  CellBytes = 32, EntryBytes = 16, RayBytes = 48;

    // The room, the level's pillar in it, an actor's box, and the probe's centre.
    const FVec3 RoomLo = { -200.0f, -150.0f, -100.0f }, RoomHi = { 300.0f, 250.0f, 120.0f };
    const FVec3 PillarLo = { 80.0f, 40.0f, -100.0f }, PillarHi = { 140.0f, 100.0f, 120.0f };
    const FVec3 ActorLo = { -150.0f, 100.0f, -100.0f }, ActorHi = { -110.0f, 140.0f, -30.0f };
    const FVec3 Centre = { 0.0f, 0.0f, 20.0f };

    ID3D12Device*              Device     = nullptr;
    ID3D12CommandQueue*        Queue      = nullptr;
    ID3D12CommandAllocator*    Alloc      = nullptr;
    ID3D12GraphicsCommandList* List       = nullptr;
    ID3D12Fence*               Fence      = nullptr;
    UINT64                     FenceValue = 0;
    HANDLE                     FenceEvent = nullptr;
    bool                       bDebug     = false;

    ID3D12RootSignature*  Root = nullptr;
    ID3D12PipelineState*  StatsPso = nullptr, *ClearSparePso = nullptr, *TransferKeyPso = nullptr,
                         *TransferGatherPso = nullptr, *TransferBackPso = nullptr;
    ID3D12PipelineState*  CarvePso = nullptr, *KeyPso = nullptr, *GatherPso = nullptr, *BasePso = nullptr,
                         *UpPso = nullptr, *DownPso = nullptr, *MarchPso = nullptr;
    ID3D12DescriptorHeap* Heap = nullptr;
    UINT                  HeapStep = 0;
    ID3D12Resource*       PictureTex = nullptr, *DepthTex = nullptr, *KindTex = nullptr, *AddedTex = nullptr;
    ID3D12Resource*       CellsBuf = nullptr, *KeysBuf = nullptr, *LevelsBuf = nullptr, *RaysBuf = nullptr;
    ID3D12Resource*       SpareBuf = nullptr, *StatsBuf = nullptr;
    const UINT            StatsBytes = 16;
    ID3D12Resource*       Upload = nullptr, *Readback = nullptr;
    UINT64                UploadBytes = 0, ReadbackBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Foot[4] = {};
    bool                  bTexturesFresh = true;      // still in their first state

    int Checked = 0, Failed = 0;
    void Check(bool bOk, const char* What)
    {
        Checked++;
        if (!bOk)
        {
            Failed++;
            printf("  FAILED: %s\n", What);
        }
    }

    FVec3 Add(const FVec3& A, const FVec3& B) { return { A.X + B.X, A.Y + B.Y, A.Z + B.Z }; }
    FVec3 Sub(const FVec3& A, const FVec3& B) { return { A.X - B.X, A.Y - B.Y, A.Z - B.Z }; }
    FVec3 Mul(const FVec3& A, float S) { return { A.X * S, A.Y * S, A.Z * S }; }
    float Len(const FVec3& A) { return sqrtf(RdxMath::Dot(A, A)); }
    FVec3 Norm(const FVec3& A) { return Mul(A, 1.0f / Len(A)); }
    FVec3 Cross(const FVec3& A, const FVec3& B)
    {
        return { A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X };
    }

    float HalfToFloat(unsigned Hf)
    {
        const unsigned Sign = (Hf >> 15) & 1, Exp = (Hf >> 10) & 31, Man = Hf & 1023;
        const float    V    = (Exp == 0) ? ldexpf((float)Man, -24) : ldexpf((float)(Man | 1024), (int)Exp - 25);
        return Sign ? -V : V;
    }
    // A float as the nearest half (ties to even), as the shader's f32tof16 rounds it - for finite values in a half's
    // range, which is all a probe holds.
    float RoundHalf(float F)
    {
        if (F == 0.0f)
            return 0.0f;
        int E;
        frexpf(fabsf(F), &E);                           // |F| = M * 2^E, M in [0.5, 1)
        // Eleven significant bits for a normal half; fewer below its smallest normal (2^-14), whose step is 2^-24.
        const int   Bits = (E - 1 >= -14) ? 11 : std::max(0, 11 - (-14 - (E - 1)));
        const float Step = ldexpf(1.0f, E - Bits);
        const float R    = nearbyintf(fabsf(F) / Step) * Step;
        return (F < 0.0f) ? -R : R;
    }

    // The camera: its place and axes (any right-angled three: the picture below and the shader read them alike).
    struct FCam
    {
        FVec3 Eye, AX, AY, AZ;
    };
    FCam Aim(const FVec3& Eye, const FVec3& Forward)
    {
        FCam C;
        C.Eye = Eye;
        C.AZ  = Norm(Forward);
        C.AX  = Norm(Cross(FVec3{ 0.0f, 0.0f, 1.0f }, C.AZ));
        C.AY  = Cross(C.AZ, C.AX);
        return C;
    }

    // Where a ray from S along R (of any length: T counts in R's lengths) leaves a box it is inside, and where it first
    // meets one it is outside of - 1e30 for none.
    float Leaves(const FVec3& S, const FVec3& R, const FVec3& Lo, const FVec3& Hi)
    {
        const float Sv[3] = { S.X, S.Y, S.Z }, Rv[3] = { R.X, R.Y, R.Z };
        const float Lv[3] = { Lo.X, Lo.Y, Lo.Z }, Hv[3] = { Hi.X, Hi.Y, Hi.Z };
        float T = 1e30f;
        for (int a = 0; a < 3; a++)
            if (Rv[a] != 0.0f)
            {
                const float Ta = ((Rv[a] > 0.0f ? Hv[a] : Lv[a]) - Sv[a]) / Rv[a];
                T = (Ta < T) ? Ta : T;
            }
        return T;
    }
    float Meets(const FVec3& S, const FVec3& R, const FVec3& Lo, const FVec3& Hi)
    {
        const float Sv[3] = { S.X, S.Y, S.Z }, Rv[3] = { R.X, R.Y, R.Z };
        const float Lv[3] = { Lo.X, Lo.Y, Lo.Z }, Hv[3] = { Hi.X, Hi.Y, Hi.Z };
        float T0 = 0.0f, T1 = 1e30f;
        for (int a = 0; a < 3; a++)
        {
            if (Rv[a] == 0.0f)
            {
                if (Sv[a] < Lv[a] || Sv[a] > Hv[a])
                    return 1e30f;
                continue;
            }
            float Ta = (Lv[a] - Sv[a]) / Rv[a], Tb = (Hv[a] - Sv[a]) / Rv[a];
            if (Ta > Tb)
            {
                const float K = Ta;
                Ta = Tb;
                Tb = K;
            }
            T0 = (Ta > T0) ? Ta : T0;
            T1 = (Tb < T1) ? Tb : T1;
            if (T0 > T1)
                return 1e30f;
        }
        return (T0 > 0.0f) ? T0 : 1e30f;
    }
    float Hit(const FVec3& S, const FVec3& R, bool bPillar, bool& bActor)
    {
        float T = Leaves(S, R, RoomLo, RoomHi);
        bActor  = false;
        if (bPillar)
        {
            const float P = Meets(S, R, PillarLo, PillarHi);
            T = (P < T) ? P : T;
        }
        const float A = Meets(S, R, ActorLo, ActorHi);
        if (A < T)
        {
            T      = A;
            bActor = true;
        }
        return T;
    }
    // How far a point lies from a box's surface (inside or out).
    float FromBox(const FVec3& P, const FVec3& Lo, const FVec3& Hi)
    {
        const float Pv[3] = { P.X, P.Y, P.Z }, Lv[3] = { Lo.X, Lo.Y, Lo.Z }, Hv[3] = { Hi.X, Hi.Y, Hi.Z };
        float Out2 = 0.0f, In = 1e30f;
        bool  bInside = true;
        for (int a = 0; a < 3; a++)
        {
            const float D = (Pv[a] < Lv[a]) ? Lv[a] - Pv[a] : (Pv[a] > Hv[a]) ? Pv[a] - Hv[a] : 0.0f;
            Out2 += D * D;
            bInside = bInside && D == 0.0f;
            const float E = fminf(Pv[a] - Lv[a], Hv[a] - Pv[a]);
            In = fminf(In, E);
        }
        return bInside ? In : sqrtf(Out2);
    }
    FVec3 ColourAt(const FVec3& P)
    {
        return { 0.5f + 0.5f * sinf(P.X / 37.0f), 0.5f + 0.5f * sinf(P.Y / 53.0f + 1.0f),
                 0.5f + 0.5f * sinf(P.Z / 41.0f + 2.0f) };
    }

    // The camera's picture, worked out on the CPU: colour (RGBA32F), the view depth's reciprocal (R32F, 0 for nothing),
    // the kinds (R8G8: the kind in the second channel, as the stencil's view reads it), and the shadows' added target
    // (RGBA32F) - what see-through draws laid over each pixel: here a glare over one patch, and over another a trace
    // too faint to count.
    struct FPicture
    {
        std::vector<float>         Colour, Depth, Over;
        std::vector<unsigned char> Kind;
    };
    const UINT GlareX0 = 200, GlareX1 = 280, GlareY0 = 100, GlareY1 = 170;
    const UINT FaintX0 = 400, FaintX1 = 460, FaintY0 = 200, FaintY1 = 260;
    const float OverLeast = 1.0f / 1024.0f;     // MirrorProbe.hlsl's RDX_PROBE_OVER_LEAST
    void Render(const FCam& C, bool bPillar, FPicture& Out)
    {
        Out.Colour.assign((size_t)W * H * 4, 0.0f);
        Out.Depth.assign((size_t)W * H, 0.0f);
        Out.Kind.assign((size_t)W * H * 2, 0);
        Out.Over.assign((size_t)W * H * 4, 0.0f);
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++)
            {
                const float Nx = ((float)x + 0.5f) / (float)W * 2.0f - 1.0f;
                const float Ny = ((float)y + 0.5f) / (float)H * 2.0f - 1.0f;
                const FVec3 D  = Add(C.AZ, Add(Mul(C.AX, Nx * TanX), Mul(C.AY, Ny * TanY)));
                bool        bActor;
                const float T = Hit(C.Eye, D, bPillar, bActor);
                const FVec3 P = Add(C.Eye, Mul(D, T));
                const FVec3 K = ColourAt(P);
                const size_t I = (size_t)y * W + x;
                Out.Colour[I * 4 + 0] = K.X;
                Out.Colour[I * 4 + 1] = K.Y;
                Out.Colour[I * 4 + 2] = K.Z;
                Out.Colour[I * 4 + 3] = 1.0f;
                Out.Depth[I]          = 1.0f / T;       // the ray's own depth along the axis is T: its Z part is one
                Out.Kind[I * 2 + 1]   = bActor ? 1 : 0;
                if (x >= GlareX0 && x < GlareX1 && y >= GlareY0 && y < GlareY1)
                    Out.Over[I * 4 + 0] = Out.Over[I * 4 + 1] = Out.Over[I * 4 + 2] = 0.3f;
                else if (x >= FaintX0 && x < FaintX1 && y >= FaintY0 && y < FaintY1)
                    Out.Over[I * 4 + 0] = OverLeast * 0.25f;
            }
    }

    // What the passes see of a pixel, worked out as ProbePixel does: false where the probe takes nothing from it.
    bool PixelPoint(const FPicture& Pic, const FCam& C, UINT x, UINT y, FVec3& From)
    {
        const size_t I = (size_t)y * W + x;
        const float  Z = Pic.Depth[I];
        if (!(Z > 0.0f) || Pic.Kind[I * 2 + 1] != 0)
            return false;
        if (std::max(Pic.Over[I * 4 + 0], std::max(Pic.Over[I * 4 + 1], Pic.Over[I * 4 + 2])) > OverLeast)
            return false;
        const float Nx = ((float)x + 0.5f) / (float)W * 2.0f - 1.0f;
        const float Ny = ((float)y + 0.5f) / (float)H * 2.0f - 1.0f;
        const float Zv = 1.0f / Z;
        From = Sub(Add(Add(Mul(C.AX, Nx * TanX * Zv), Mul(C.AY, Ny * TanY * Zv)), Mul(C.AZ, Zv)), Sub(Centre, C.Eye));
        const float L = Len(From);
        return L <= Reach && L >= 1.0f;
    }
    int CellOf(const FVec3& D, int Level)
    {
        int   Face;
        float U, V;
        if (!RdxMath::ProbeFaceOf(D, Face, U, V))
            return -1;
        const int S = Side >> Level;
        int       X, Y;
        RdxMath::ProbeTexelOf(U, V, S, X, Y);
        return RdxMath::ProbeLevelStart(Level) + (Face * S + Y) * S + X;
    }

    // ---- the GPU's side

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
    void UavBarrier()
    {
        D3D12_RESOURCE_BARRIER B = {};
        B.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        List->ResourceBarrier(1, &B);
    }
    void Open()
    {
        Alloc->Reset();
        List->Reset(Alloc, nullptr);
    }
    void ExecuteAndWait()
    {
        List->Close();
        ID3D12CommandList* L[1] = { List };
        Queue->ExecuteCommandLists(1, L);
        FenceValue++;
        Queue->Signal(Fence, FenceValue);
        if (Fence->GetCompletedValue() < FenceValue)
        {
            Fence->SetEventOnCompletion(FenceValue, FenceEvent);
            WaitForSingleObject(FenceEvent, INFINITE);
        }
    }
    int DrainDebug()
    {
        if (!bDebug)
            return 0;
        ID3D12InfoQueue* Q = nullptr;
        if (FAILED(Device->QueryInterface(IID_PPV_ARGS(&Q))))
            return 0;
        int          Errors = 0;
        const UINT64 N      = Q->GetNumStoredMessages();
        for (UINT64 i = 0; i < N; i++)
        {
            SIZE_T Bytes = 0;
            Q->GetMessage(i, nullptr, &Bytes);
            D3D12_MESSAGE* M = (D3D12_MESSAGE*)malloc(Bytes);
            if (M && SUCCEEDED(Q->GetMessage(i, M, &Bytes)) && M->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                if (Errors < 20)
                    printf("  d3d12: %s\n", M->pDescription);
                Errors++;
            }
            free(M);
        }
        Q->ClearStoredMessages();
        Q->Release();
        return Errors;
    }

    ID3D12Resource* MakeBuffer(UINT64 Bytes, D3D12_HEAP_TYPE Type, D3D12_RESOURCE_FLAGS Flags, D3D12_RESOURCE_STATES State)
    {
        D3D12_HEAP_PROPERTIES Hp = {};
        Hp.Type = Type;
        D3D12_RESOURCE_DESC D = {};
        D.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        D.Width            = Bytes;
        D.Height           = 1;
        D.DepthOrArraySize = 1;
        D.MipLevels        = 1;
        D.SampleDesc.Count = 1;
        D.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D.Flags            = Flags;
        ID3D12Resource* R  = nullptr;
        return SUCCEEDED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &D, State, nullptr, IID_PPV_ARGS(&R)))
                   ? R : nullptr;
    }
    ID3D12Resource* MakeTexture(DXGI_FORMAT Format)
    {
        D3D12_HEAP_PROPERTIES Hp = {};
        Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC D = {};
        D.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        D.Width            = W;
        D.Height           = H;
        D.DepthOrArraySize = 1;
        D.MipLevels        = 1;
        D.Format           = Format;
        D.SampleDesc.Count = 1;
        ID3D12Resource* R  = nullptr;
        return SUCCEEDED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &D, D3D12_RESOURCE_STATE_COPY_DEST,
                                                         nullptr, IID_PPV_ARGS(&R)))
                   ? R : nullptr;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(int i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE C = Heap->GetCPUDescriptorHandleForHeapStart();
        C.ptr += (SIZE_T)i * HeapStep;
        return C;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(int i)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE G = Heap->GetGPUDescriptorHandleForHeapStart();
        G.ptr += (UINT64)i * HeapStep;
        return G;
    }
    void RawUav(ID3D12Resource* R, UINT64 Bytes, int Slot)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC U = {};
        U.Format             = DXGI_FORMAT_R32_TYPELESS;
        U.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
        U.Buffer.NumElements = (UINT)(Bytes / 4);
        U.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
        Device->CreateUnorderedAccessView(R, nullptr, &U, Cpu(Slot));
    }
    void TexSrv(ID3D12Resource* R, DXGI_FORMAT Format, int Slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC S = {};
        S.Format                  = Format;
        S.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        S.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        S.Texture2D.MipLevels     = 1;
        Device->CreateShaderResourceView(R, &S, Cpu(Slot));
    }

    ID3D12PipelineState* MakePso(const wchar_t* File, const char* Entry)
    {
        ID3DBlob* Code = nullptr;
        ID3DBlob* Err  = nullptr;
        const HRESULT Made = D3DCompileFromFile(File, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, Entry, "cs_5_0",
                                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Err);
        if (FAILED(Made))
        {
            printf("  %s did not compile: %s\n", Entry, Err ? (const char*)Err->GetBufferPointer() : "(no message)");
            if (Err)
                Err->Release();
            return nullptr;
        }
        if (Err)
            Err->Release();
        D3D12_COMPUTE_PIPELINE_STATE_DESC P = {};
        P.pRootSignature     = Root;
        P.CS.pShaderBytecode = Code->GetBufferPointer();
        P.CS.BytecodeLength  = Code->GetBufferSize();
        ID3D12PipelineState* Pso = nullptr;
        Device->CreateComputePipelineState(&P, IID_PPV_ARGS(&Pso));
        Code->Release();
        return Pso;
    }

    bool MakeEverything(const wchar_t* ShaderFile)
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
                    printf("  adapter: %ls\n", D.Description);
                Adapter->Release();
                Adapter = nullptr;
            }
            Factory->Release();
        }
        if (!Device)
            return false;
        D3D12_COMMAND_QUEUE_DESC Q = {};
        Q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Device->CreateCommandQueue(&Q, IID_PPV_ARGS(&Queue));
        Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Alloc));
        Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Alloc, nullptr, IID_PPV_ARGS(&List));
        List->Close();
        Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence));
        FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // As the renderer binds the passes: the three views of the copies, the probe's buffers (and here the rays), and
        // the constants.
        D3D12_DESCRIPTOR_RANGE Ranges[2] = {};
        Ranges[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Ranges[0].NumDescriptors = 4;
        Ranges[1].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        Ranges[1].NumDescriptors = 6;
        D3D12_ROOT_PARAMETER Params[3] = {};
        for (int k = 0; k < 2; k++)
        {
            Params[k].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            Params[k].DescriptorTable.NumDescriptorRanges = 1;
            Params[k].DescriptorTable.pDescriptorRanges   = &Ranges[k];
        }
        Params[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        Params[2].Constants.Num32BitValues = 32;
        D3D12_ROOT_SIGNATURE_DESC Rd = {};
        Rd.NumParameters = 3;
        Rd.pParameters   = Params;
        ID3DBlob* Blob = nullptr;
        ID3DBlob* Err  = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&Rd, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err)) ||
            FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(), IID_PPV_ARGS(&Root))))
            return false;
        Blob->Release();

        CarvePso  = MakePso(ShaderFile, "CarveCS");
        KeyPso    = MakePso(ShaderFile, "KeyCS");
        GatherPso = MakePso(ShaderFile, "GatherCS");
        BasePso   = MakePso(ShaderFile, "BaseCS");
        UpPso     = MakePso(ShaderFile, "UpCS");
        DownPso   = MakePso(ShaderFile, "DownCS");
        MarchPso  = MakePso(ShaderFile, "MarchCS");
        StatsPso          = MakePso(ShaderFile, "StatsClearCS");
        ClearSparePso     = MakePso(ShaderFile, "ClearSpareCS");
        TransferKeyPso    = MakePso(ShaderFile, "TransferKeyCS");
        TransferGatherPso = MakePso(ShaderFile, "TransferGatherCS");
        TransferBackPso   = MakePso(ShaderFile, "TransferBackCS");
        if (!CarvePso || !KeyPso || !GatherPso || !BasePso || !UpPso || !DownPso || !MarchPso || !StatsPso ||
            !ClearSparePso || !TransferKeyPso || !TransferGatherPso || !TransferBackPso)
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC Hd = {};
        Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        Hd.NumDescriptors = 10;
        Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Device->CreateDescriptorHeap(&Hd, IID_PPV_ARGS(&Heap));
        HeapStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        PictureTex = MakeTexture(DXGI_FORMAT_R32G32B32A32_FLOAT);
        DepthTex   = MakeTexture(DXGI_FORMAT_R32_FLOAT);
        KindTex    = MakeTexture(DXGI_FORMAT_R8G8_UINT);
        AddedTex   = MakeTexture(DXGI_FORMAT_R32G32B32A32_FLOAT);
        const D3D12_RESOURCE_FLAGS Uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        CellsBuf  = MakeBuffer((UINT64)Cells * CellBytes, D3D12_HEAP_TYPE_DEFAULT, Uav, D3D12_RESOURCE_STATE_COMMON);
        KeysBuf   = MakeBuffer((UINT64)Cells * 4, D3D12_HEAP_TYPE_DEFAULT, Uav, D3D12_RESOURCE_STATE_COMMON);
        LevelsBuf = MakeBuffer((UINT64)Entries * EntryBytes, D3D12_HEAP_TYPE_DEFAULT, Uav, D3D12_RESOURCE_STATE_COMMON);
        RaysBuf   = MakeBuffer((UINT64)Rays * RayBytes, D3D12_HEAP_TYPE_DEFAULT, Uav, D3D12_RESOURCE_STATE_COMMON);
        SpareBuf  = MakeBuffer((UINT64)Cells * CellBytes, D3D12_HEAP_TYPE_DEFAULT, Uav, D3D12_RESOURCE_STATE_COMMON);
        StatsBuf  = MakeBuffer(StatsBytes, D3D12_HEAP_TYPE_DEFAULT, Uav, D3D12_RESOURCE_STATE_COMMON);
        if (!PictureTex || !DepthTex || !KindTex || !AddedTex || !CellsBuf || !KeysBuf || !LevelsBuf || !RaysBuf ||
            !SpareBuf || !StatsBuf)
            return false;
        TexSrv(PictureTex, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
        TexSrv(DepthTex, DXGI_FORMAT_R32_FLOAT, 1);
        TexSrv(KindTex, DXGI_FORMAT_R8G8_UINT, 2);
        TexSrv(AddedTex, DXGI_FORMAT_R32G32B32A32_FLOAT, 3);
        RawUav(CellsBuf, (UINT64)Cells * CellBytes, 4);
        RawUav(KeysBuf, (UINT64)Cells * 4, 5);
        RawUav(LevelsBuf, (UINT64)Entries * EntryBytes, 6);
        RawUav(SpareBuf, (UINT64)Cells * CellBytes, 7);
        RawUav(StatsBuf, StatsBytes, 8);
        RawUav(RaysBuf, (UINT64)Rays * RayBytes, 9);

        ID3D12Resource* const Texs[4] = { PictureTex, DepthTex, KindTex, AddedTex };
        UploadBytes = 0;
        for (int t = 0; t < 4; t++)
        {
            const D3D12_RESOURCE_DESC D = Texs[t]->GetDesc();
            UINT64 Total = 0;
            Device->GetCopyableFootprints(&D, 0, 1, UploadBytes, &Foot[t], nullptr, nullptr, &Total);
            UploadBytes = (Foot[t].Offset + Total + 511) & ~(UINT64)511;
        }
        UploadBytes += (UINT64)Rays * RayBytes;
        Upload = MakeBuffer(UploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
        ReadbackBytes = (UINT64)Cells * CellBytes + (UINT64)Entries * EntryBytes + (UINT64)Rays * RayBytes + StatsBytes;
        Readback = MakeBuffer(ReadbackBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST);
        if (!Upload || !Readback)
            return false;
        Open();
        Barrier(CellsBuf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(KeysBuf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(LevelsBuf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(RaysBuf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(SpareBuf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(StatsBuf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ExecuteAndWait();
        return true;
    }

    // The passes' constants (MirrorProbe.hlsl's ProbeConsts), 28 of them.
    struct FConsts
    {
        float AxisX[3]; float TanX;
        float AxisY[3]; float TanY;
        float AxisZ[3]; float Reach;
        float Centre[3]; float Spare0;
        float Rect[4];
        float Rules[4];
        UINT  Frame, Pick, Level, Reset;
        float Shift[3]; UINT Count;
    };
    static_assert(sizeof(FConsts) == 32 * 4, "the passes take 32 constants");
    FConsts ConstsFor(const FCam& C, const FVec3& At, UINT Frame, UINT Pick, bool bReset)
    {
        FConsts K = {};
        const FVec3 Ax[3] = { C.AX, C.AY, C.AZ };
        float* const To[3] = { K.AxisX, K.AxisY, K.AxisZ };
        for (int a = 0; a < 3; a++)
        {
            To[a][0] = Ax[a].X;
            To[a][1] = Ax[a].Y;
            To[a][2] = Ax[a].Z;
        }
        K.TanX  = TanX;
        K.TanY  = TanY;
        K.Reach = Reach;
        const FVec3 Rel = Sub(At, C.Eye);
        K.Centre[0] = Rel.X;
        K.Centre[1] = Rel.Y;
        K.Centre[2] = Rel.Z;
        K.Rect[2] = K.Rect[3] = 1.0f;
        K.Rules[0] = RdxMath::ProbeCarveSlack;
        K.Rules[1] = RdxMath::ProbeCarveShare;
        K.Rules[2] = RdxMath::ProbeSameShare;
        K.Frame = Frame;
        K.Pick  = Pick;
        K.Reset = bReset ? 1u : 0u;
        return K;
    }

    void Dispatch(ID3D12PipelineState* Pso, FConsts& K, UINT Level, UINT X, UINT Y, UINT Z)
    {
        K.Level = Level;
        List->SetPipelineState(Pso);
        List->SetComputeRoot32BitConstants(2, 32, &K, 0);
        List->Dispatch(X, Y, Z);
        UavBarrier();
    }
    UINT Groups(UINT N) { return (N + 7) / 8; }

    // One frame of the passes, as the renderer runs them (FMirrorProbe::Update), on a picture, with the probe at At -
    // moved there from MovedFrom where that is given; the cells, the pyramid and the frame's counts read back.
    bool RunFrame(const FPicture& Pic, const FCam& C, const FVec3& At, const FVec3* MovedFrom, UINT Frame, UINT Pick,
                  bool bReset, std::vector<BYTE>& CellsOut, std::vector<BYTE>& LevelsOut, UINT* Counts)
    {
        BYTE* Up = nullptr;
        const D3D12_RANGE None = { 0, 0 };
        if (FAILED(Upload->Map(0, &None, (void**)&Up)))
            return false;
        const void* const Src[4]   = { Pic.Colour.data(), Pic.Depth.data(), Pic.Kind.data(), Pic.Over.data() };
        const UINT        Pixel[4] = { 16, 4, 2, 16 };
        for (int t = 0; t < 4; t++)
            for (UINT y = 0; y < H; y++)
                memcpy(Up + Foot[t].Offset + (UINT64)y * Foot[t].Footprint.RowPitch,
                       (const BYTE*)Src[t] + (size_t)y * W * Pixel[t], (size_t)W * Pixel[t]);
        Upload->Unmap(0, nullptr);

        ID3D12Resource* const Texs[4] = { PictureTex, DepthTex, KindTex, AddedTex };
        Open();
        for (int t = 0; t < 4; t++)
        {
            if (!bTexturesFresh)
                Barrier(Texs[t], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION Dst = {}, From = {};
            Dst.pResource        = Texs[t];
            Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            From.pResource       = Upload;
            From.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            From.PlacedFootprint = Foot[t];
            List->CopyTextureRegion(&Dst, 0, 0, 0, &From, nullptr);
            Barrier(Texs[t], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        bTexturesFresh = false;

        ID3D12DescriptorHeap* Heaps[1] = { Heap };
        List->SetDescriptorHeaps(1, Heaps);
        List->SetComputeRootSignature(Root);
        List->SetComputeRootDescriptorTable(0, Gpu(0));
        List->SetComputeRootDescriptorTable(1, Gpu(4));
        FConsts K = ConstsFor(C, At, Frame, Pick, bReset);
        K.Count   = 1;
        Dispatch(StatsPso, K, 0, 1, 1, 1);
        if (MovedFrom)
        {
            const FVec3 Shift = Sub(*MovedFrom, At);
            K.Shift[0] = Shift.X;
            K.Shift[1] = Shift.Y;
            K.Shift[2] = Shift.Z;
            Dispatch(ClearSparePso, K, 0, Groups(Side), Groups(Side), 6);
            Dispatch(TransferKeyPso, K, 0, Groups(Side), Groups(Side), 6);
            Dispatch(TransferGatherPso, K, 0, Groups(Side), Groups(Side), 6);
            Dispatch(TransferBackPso, K, 0, Groups(Side), Groups(Side), 6);
        }
        Dispatch(CarvePso, K, 0, Groups(Side), Groups(Side), 6);
        Dispatch(KeyPso, K, 0, Groups((W + 1) / 2), Groups((H + 1) / 2), 1);
        Dispatch(GatherPso, K, 0, Groups((W + 1) / 2), Groups((H + 1) / 2), 1);
        Dispatch(BasePso, K, 0, Groups(Side), Groups(Side), 6);
        for (UINT k = 1; k < (UINT)RdxMath::ProbeLevels; k++)
            Dispatch(UpPso, K, k, Groups(Side >> k), Groups(Side >> k), 6);
        for (int k = RdxMath::ProbeFillUp - 1; k >= 0; k--)
            Dispatch(DownPso, K, (UINT)k, Groups(Side >> k), Groups(Side >> k), 6);

        const UINT64 CountsAt = (UINT64)Cells * CellBytes + (UINT64)Entries * EntryBytes + (UINT64)Rays * RayBytes;
        Barrier(CellsBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(LevelsBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(StatsBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        List->CopyBufferRegion(Readback, 0, CellsBuf, 0, (UINT64)Cells * CellBytes);
        List->CopyBufferRegion(Readback, (UINT64)Cells * CellBytes, LevelsBuf, 0, (UINT64)Entries * EntryBytes);
        List->CopyBufferRegion(Readback, CountsAt, StatsBuf, 0, StatsBytes);
        Barrier(CellsBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(LevelsBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(StatsBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ExecuteAndWait();
        if (DrainDebug() > 0)
            return false;
        BYTE* Back = nullptr;
        const D3D12_RANGE All = { 0, (SIZE_T)ReadbackBytes };
        if (FAILED(Readback->Map(0, &All, (void**)&Back)))
            return false;
        CellsOut.assign(Back, Back + (size_t)Cells * CellBytes);
        LevelsOut.assign(Back + (size_t)Cells * CellBytes, Back + (size_t)Cells * CellBytes + (size_t)Entries * EntryBytes);
        if (Counts)
            memcpy(Counts, Back + CountsAt, StatsBytes);
        Readback->Unmap(0, &None);
        return true;
    }

    // The march on the GPU: each ray's start (from the probe's centre), its reach and its way in; T and whether it
    // crossed out.
    bool RunMarch(const std::vector<float>& In, std::vector<float>& OutT, std::vector<UINT>& OutHit)
    {
        BYTE* Up = nullptr;
        const D3D12_RANGE None = { 0, 0 };
        const UINT64 RaysAt = UploadBytes - (UINT64)Rays * RayBytes;
        if (FAILED(Upload->Map(0, &None, (void**)&Up)))
            return false;
        memcpy(Up + RaysAt, In.data(), (size_t)Rays * RayBytes);
        Upload->Unmap(0, nullptr);
        Open();
        Barrier(RaysBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        List->CopyBufferRegion(RaysBuf, 0, Upload, RaysAt, (UINT64)Rays * RayBytes);
        Barrier(RaysBuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* Heaps[1] = { Heap };
        List->SetDescriptorHeaps(1, Heaps);
        List->SetComputeRootSignature(Root);
        List->SetComputeRootDescriptorTable(0, Gpu(0));
        List->SetComputeRootDescriptorTable(1, Gpu(4));
        FConsts K = {};
        Dispatch(MarchPso, K, (UINT)Rays, (UINT)(Rays + 63) / 64, 1, 1);
        const UINT64 Back = (UINT64)Cells * CellBytes + (UINT64)Entries * EntryBytes;
        Barrier(RaysBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        List->CopyBufferRegion(Readback, Back, RaysBuf, 0, (UINT64)Rays * RayBytes);
        Barrier(RaysBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ExecuteAndWait();
        if (DrainDebug() > 0)
            return false;
        BYTE* Map = nullptr;
        const D3D12_RANGE All = { 0, (SIZE_T)ReadbackBytes };
        if (FAILED(Readback->Map(0, &All, (void**)&Map)))
            return false;
        OutT.resize(Rays);
        OutHit.resize(Rays);
        for (int r = 0; r < Rays; r++)
        {
            const BYTE* const R = Map + Back + (size_t)r * RayBytes + 32;
            memcpy(&OutT[r], R, 4);
            memcpy(&OutHit[r], R + 4, 4);
        }
        Readback->Unmap(0, &None);
        return true;
    }

    // ---- reading back

    struct FCellRead
    {
        FVec3    Point;
        float    Dist;
        FVec3    Colour;
        unsigned Frame;
    };
    FCellRead CellAt(const std::vector<BYTE>& B, int i)
    {
        float    F[4];
        unsigned U[4];
        memcpy(F, &B[(size_t)i * CellBytes], 16);
        memcpy(U, &B[(size_t)i * CellBytes + 16], 16);
        FCellRead C;
        C.Point  = { F[0], F[1], F[2] };
        C.Dist   = F[3];
        C.Colour = { HalfToFloat(U[0] & 0xFFFF), HalfToFloat(U[0] >> 16), HalfToFloat(U[1] & 0xFFFF) };
        C.Frame  = U[2];
        return C;
    }
    FProbeTexel EntryAt(const std::vector<BYTE>& B, int i)
    {
        unsigned U[4];
        memcpy(U, &B[(size_t)i * EntryBytes], 16);
        FProbeTexel T;
        T.Colour = { HalfToFloat(U[0] & 0xFFFF), HalfToFloat(U[0] >> 16), HalfToFloat(U[1] & 0xFFFF) };
        T.Trust  = HalfToFloat(U[1] >> 16);
        memcpy(&T.Dist, &U[2], 4);
        T.Frame = 0;
        return T;
    }
    bool OnLevel(const FVec3& P, bool bPillar, float Tolerance)
    {
        float D = fabsf(FromBox(P, RoomLo, RoomHi));
        if (bPillar)
            D = fminf(D, fabsf(FromBox(P, PillarLo, PillarHi)));
        return D <= Tolerance;
    }
    bool Near(float A, float B, float Rel, float Abs) { return fabsf(A - B) <= Abs + Rel * fabsf(B); }
    // A point of the pillar's sides: on the pillar and not on the room - its foot and its top lie in the floor and the
    // ceiling, which the second view sees where the pillar stood.
    bool OnPillarOnly(const FVec3& P, float Dist)
    {
        const float Tolerance = 0.05f + 1e-5f * Dist;
        return fabsf(FromBox(P, PillarLo, PillarHi)) <= Tolerance && fabsf(FromBox(P, RoomLo, RoomHi)) > Tolerance;
    }
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--debug") == 0)
            bDebug = true;
    printf("ProbeCheck - the probe gathered from the picture, on the GPU\n");

    // The shaders from the source tree: this check's file, which takes in MirrorProbe.hlsl.
    wchar_t Here[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, __FILE__, -1, Here, MAX_PATH);
    wchar_t* Slash = wcsrchr(Here, L'\\');
    if (!Slash)
        Slash = wcsrchr(Here, L'/');
    if (!Slash)
        return 2;
    wcscpy_s(Slash + 1, MAX_PATH - (Slash + 1 - Here), L"ProbeCheckMarch.hlsl");
    if (!MakeEverything(Here))
    {
        printf("  the device, the passes or the buffers could not be made\n");
        return 2;
    }

    // ---- the first frame: a fresh probe, the room with its pillar, from one side.
    const FCam C1 = Aim(FVec3{ 200.0f, -100.0f, 30.0f }, FVec3{ -1.0f, 0.55f, -0.12f });
    FPicture   P1;
    Render(C1, true, P1);
    std::vector<BYTE> Cells1, Levels1;
    UINT              Counts1[4] = {};
    if (!RunFrame(P1, C1, Centre, nullptr, 1, 0, true, Cells1, Levels1, Counts1))
    {
        printf("  the first frame failed\n");
        return 2;
    }

    // What the frame's pixels give each cell, by the rules - the nearest point, with its pixel's colour.
    std::vector<float> Best(Cells, 1e30f);
    std::vector<FVec3> BestPoint(Cells);
    int Pixels = 0, Actors = 0, Glared = 0, Faint = 0;
    for (UINT y = 0; y < H; y += 2)
        for (UINT x = 0; x < W; x += 2)
        {
            Actors += P1.Kind[((size_t)y * W + x) * 2 + 1] ? 1 : 0;
            Glared += (P1.Over[((size_t)y * W + x) * 4] > OverLeast) ? 1 : 0;
            Faint += (P1.Over[((size_t)y * W + x) * 4] > 0.0f && P1.Over[((size_t)y * W + x) * 4] <= OverLeast) ? 1 : 0;
            FVec3 From;
            if (!PixelPoint(P1, C1, x, y, From))
                continue;
            Pixels++;
            const int   I = CellOf(From, 0);
            const float L = Len(From);
            if (I >= 0 && L < Best[I])
            {
                Best[I]      = L;
                BestPoint[I] = From;
            }
        }
    int Known = 0, Expected = 0, Agree = 0, Nearest = 0, OwnCell = 0, OnSurface = 0, Coloured = 0;
    for (int i = 0; i < Cells; i++)
    {
        const FCellRead C       = CellAt(Cells1, i);
        const bool      bKnown  = C.Dist > 0.0f;
        const bool      bWanted = Best[i] < 1e29f;
        Known += bKnown ? 1 : 0;
        Expected += bWanted ? 1 : 0;
        Agree += (bKnown == bWanted) ? 1 : 0;
        if (!bKnown || !bWanted)
            continue;
        // The nearest - or a pixel as near within a hair, which is the same point: its distance is the nearest's.
        Nearest += (Near(C.Dist, Best[i], 1e-4f, 1e-3f) && Near(Len(C.Point), C.Dist, 1e-5f, 1e-3f)) ? 1 : 0;
        OwnCell += (CellOf(C.Point, 0) == i) ? 1 : 0;
        const FVec3 World = Add(Centre, C.Point);
        OnSurface += OnLevel(World, true, 0.05f + 1e-5f * C.Dist) ? 1 : 0;
        const FVec3 Want = ColourAt(World);
        Coloured += (Near(C.Colour.X, Want.X, 0.0f, 2e-3f) && Near(C.Colour.Y, Want.Y, 0.0f, 2e-3f) &&
                     Near(C.Colour.Z, Want.Z, 0.0f, 2e-3f)) ? 1 : 0;
    }
    printf("  frame 1: %d pixels gathered (%d of the actor's seen), %d cells expected, %d known\n", Pixels, Actors,
           Expected, Known);
    printf("  %d pixels under the glare kept out, %d under the faint trace taken\n", Glared, Faint);
    Check(Expected > 1000 && Actors > 100 && Glared > 500 && Faint > 500,
          "the made-up view shows the room, its pillar, the actor's box, a glare and a faint trace");
    Check(Agree == Cells, "a cell is known exactly where a gathered pixel's point falls in it");
    printf("  of the known cells: %d hold the nearest point, %d lie in their own cell, %d on the room or the pillar, %d "
           "have their point's colour\n", Nearest, OwnCell, OnSurface, Coloured);
    Check(Nearest == Known, "each cell holds the nearest of its pixels' points");
    Check(OwnCell >= Known - 2, "each cell's point falls in that cell as RdxMath lays the cube's faces");
    Check(OnSurface == Known, "every point kept lies on the room or its pillar - none of the actor's box");
    Check(Coloured == Known, "each cell has its point's colour");
    printf("  frame 1's counts: %u carved out, %u taken from the picture, %u known\n", Counts1[0], Counts1[1], Counts1[2]);
    Check(Counts1[0] == 0 && Counts1[1] == (UINT)Known && Counts1[2] == (UINT)Known && Counts1[3] == 0,
          "a fresh probe's counts: nothing carved, every known cell taken once from the picture");

    // The pyramid: the cells' finest level, the coarser ones from the four under each, then the holes filled from above.
    std::vector<FProbeTexel> Want(Entries);
    for (int i = 0; i < Cells; i++)
    {
        const FCellRead C = CellAt(Cells1, i);
        Want[i] = (C.Dist > 0.0f) ? FProbeTexel{ C.Dist, C.Colour, 1.0f, 0 } : FProbeTexel{ 0.0f, { 0, 0, 0 }, 0.0f, 0 };
    }
    for (int k = 1; k < RdxMath::ProbeLevels; k++)
    {
        const int S = Side >> k, SB = S * 2;
        for (int f = 0; f < 6; f++)
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++)
                {
                    FProbeTexel Four[4];
                    for (int q = 0; q < 4; q++)
                        Four[q] = Want[RdxMath::ProbeLevelStart(k - 1) + (f * SB + y * 2 + (q >> 1)) * SB + x * 2 +
                                       (q & 1)];
                    FProbeTexel M = RdxMath::ProbeMipOf(Four);
                    // The shader keeps a level's colour and trust as halves, and the next level is made from those.
                    if (M.Trust <= 0.0f)
                        M = FProbeTexel{ 0.0f, { 0, 0, 0 }, 0.0f, 0 };
                    else
                        M = FProbeTexel{ M.Dist, { RoundHalf(M.Colour.X), RoundHalf(M.Colour.Y), RoundHalf(M.Colour.Z) },
                                         RoundHalf(M.Trust), 0 };
                    Want[RdxMath::ProbeLevelStart(k) + (f * S + y) * S + x] = M;
                }
    }
    // Down: a hole takes the nearest point and the trust of the texel above it, and the colour of the four nearest
    // above it, by how near each stands and how much it is trusted (DownCS).
    for (int k = RdxMath::ProbeFillUp - 1; k >= 0; k--)
    {
        const int S = Side >> k, SP = S / 2;
        for (int f = 0; f < 6; f++)
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++)
                {
                    FProbeTexel& T = Want[RdxMath::ProbeLevelStart(k) + (f * S + y) * S + x];
                    if (T.Trust > 0.0f)
                        continue;
                    const int          Start = RdxMath::ProbeLevelStart(k + 1) + f * SP * SP;
                    const FProbeTexel& Own   = Want[Start + (y / 2) * SP + x / 2];
                    const float Ax = ((float)x + 0.5f) * 0.5f - 0.5f, Ay = ((float)y + 0.5f) * 0.5f - 0.5f;
                    const int   Lx = (int)floorf(Ax), Ly = (int)floorf(Ay);
                    const float Fx = Ax - (float)Lx, Fy = Ay - (float)Ly;
                    FVec3 Sum    = { 0.0f, 0.0f, 0.0f };
                    float Weight = 0.0f;
                    for (int q = 0; q < 4; q++)
                    {
                        const int Tx = std::min(std::max(Lx + (q & 1), 0), SP - 1);
                        const int Ty = std::min(std::max(Ly + (q >> 1), 0), SP - 1);
                        const float Wq = ((q & 1) ? Fx : 1.0f - Fx) * ((q >> 1) ? Fy : 1.0f - Fy);
                        const FProbeTexel& E = Want[Start + Ty * SP + Tx];
                        Sum    = Add(Sum, Mul(E.Colour, Wq * E.Trust));
                        Weight += Wq * E.Trust;
                    }
                    if (Own.Trust > 0.0f && Weight > 0.0f)
                        T = FProbeTexel{ Own.Dist,
                                         { RoundHalf(Sum.X / Weight), RoundHalf(Sum.Y / Weight), RoundHalf(Sum.Z / Weight) },
                                         Own.Trust, 0 };
                    else
                        T = Own;
                }
    }
    int Same = 0, Holes = 0;
    for (int i = 0; i < Entries; i++)
    {
        const FProbeTexel G = EntryAt(Levels1, i);
        const FProbeTexel& E = Want[i];
        Holes += (i < Cells && G.Trust <= 0.0f) ? 1 : 0;
        // A hole's colour is blended level by level down, each kept as halves on the way: the two sides' roundings
        // part a little more there than where a level is made from the one under it.
        const float Tol   = (E.Trust < 0.999f) ? 6e-3f : 3e-3f;
        const bool  bSame = (G.Trust <= 0.0f && E.Trust <= 0.0f) ||
                            (Near(G.Trust, E.Trust, 2e-3f, 1e-4f) && G.Dist == E.Dist &&
                             Near(G.Colour.X, E.Colour.X, 0.0f, Tol) && Near(G.Colour.Y, E.Colour.Y, 0.0f, Tol) &&
                             Near(G.Colour.Z, E.Colour.Z, 0.0f, Tol));
        Same += bSame ? 1 : 0;
        static int Shown = 0;
        if (!bSame && Shown++ < 8)
        {
            int Level = 0;
            while (Level + 1 < RdxMath::ProbeLevels && RdxMath::ProbeLevelStart(Level + 1) <= i)
                Level++;
            printf("    entry %d (level %d): GPU trust %.5f dist %.3f colour %.5f %.5f %.5f | rules trust %.5f dist %.3f "
                   "colour %.5f %.5f %.5f\n", i, Level, G.Trust, G.Dist, G.Colour.X, G.Colour.Y, G.Colour.Z, E.Trust,
                   E.Dist, E.Colour.X, E.Colour.Y, E.Colour.Z);
        }
    }
    printf("  the pyramid: %d of %d entries as the rules have them; %d cells of the finest level still unknown\n", Same,
           Entries, Holes);
    Check(Same == Entries, "the pyramid is the cells' - up by RdxMath::ProbeMipOf, the holes filled from above");

    // The march: rays from places all over the room, every way, through the same pyramid on both sides.
    {
        unsigned Seed = 4321u;
        auto Rand = [&]() { Seed = Seed * 1664525u + 1013904223u; return (float)(Seed >> 8) / 16777216.0f; };
        std::vector<float> In((size_t)Rays * (RayBytes / 4), 0.0f);
        std::vector<FVec3> Start(Rays), Way(Rays);
        for (int r = 0; r < Rays; r++)
        {
            FVec3 S;
            do
                S = { RoomLo.X + (RoomHi.X - RoomLo.X) * (0.1f + 0.8f * Rand()),
                      RoomLo.Y + (RoomHi.Y - RoomLo.Y) * (0.1f + 0.8f * Rand()),
                      RoomLo.Z + (RoomHi.Z - RoomLo.Z) * (0.1f + 0.8f * Rand()) };
            while (S.X >= PillarLo.X && S.X <= PillarHi.X && S.Y >= PillarLo.Y && S.Y <= PillarHi.Y &&
                   S.Z >= PillarLo.Z && S.Z <= PillarHi.Z);
            FVec3 R;
            do
                R = { Rand() * 2.0f - 1.0f, Rand() * 2.0f - 1.0f, Rand() * 2.0f - 1.0f };
            while (Len(R) < 0.1f);
            Start[r] = Sub(S, Centre);
            Way[r]   = Norm(R);
            float* const Q = &In[(size_t)r * (RayBytes / 4)];
            Q[0] = Start[r].X;
            Q[1] = Start[r].Y;
            Q[2] = Start[r].Z;
            Q[3] = 900.0f;
            Q[4] = Way[r].X;
            Q[5] = Way[r].Y;
            Q[6] = Way[r].Z;
        }
        std::vector<float> T;
        std::vector<UINT>  Crossed;
        if (!RunMarch(In, T, Crossed))
        {
            printf("  the march failed\n");
            return 2;
        }
        auto Lookup = [&](int f, int x, int y)
        {
            static FProbeTexel E;
            E = EntryAt(Levels1, (f * Side + y) * Side + x);
            return &E;
        };
        int Both = 0, Found = 0, Close = 0;
        for (int r = 0; r < Rays; r++)
        {
            float      Tc     = 0.0f;
            const bool bFound = RdxMath::ProbeMarch(FVec3{ 0.0f, 0.0f, 0.0f }, Side, Start[r], Way[r], 900.0f,
                                                    RdxMath::ProbeSteps, RdxMath::ProbeHalvings, Lookup, Tc);
            const bool bGpu = Crossed[r] != 0;
            Both += (bFound == bGpu) ? 1 : 0;
            if (bFound && bGpu)
            {
                Found++;
                Close += Near(T[r], Tc, 0.0f, 900.0f / (float)(RdxMath::ProbeSteps << RdxMath::ProbeHalvings) + 0.01f)
                             ? 1 : 0;
            }
        }
        printf("  the march: %d of %d rays agree on whether they crossed; %d crossed, %d of them at the same place\n",
               Both, Rays, Found, Close);
        Check(Both >= Rays - Rays / 200 && Close >= Found - Found / 200 && Found > Rays / 2,
              "the shaders' march finds what RdxMath::ProbeMarch finds on the same pyramid");
    }

    // ---- the second frame: from elsewhere, the pillar gone. Whatever of it the eye now sees past is carved out; a
    // point of the room it sees where it was is kept.
    const FCam C2 = Aim(FVec3{ -120.0f, 160.0f, 10.0f }, FVec3{ 1.0f, -0.62f, 0.05f });
    FPicture   P2;
    Render(C2, false, P2);
    std::vector<BYTE> Cells2, Levels2;
    UINT              Counts2[4] = {};
    if (!RunFrame(P2, C2, Centre, nullptr, 2, 3, false, Cells2, Levels2, Counts2))
    {
        printf("  the second frame failed\n");
        return 2;
    }
    int PillarBefore = 0, SeenPast = 0, StillThere = 0, WallSeen = 0, WallKept = 0;
    for (int i = 0; i < Cells; i++)
    {
        const FCellRead A = CellAt(Cells1, i);
        if (!(A.Dist > 0.0f))
            continue;
        const FVec3 World = Add(Centre, A.Point);
        const bool  bPillarPoint = OnPillarOnly(World, A.Dist);
        // Where the second view sees that point, and what it shows round it: the shader's own test.
        const FVec3 Q  = Sub(World, C2.Eye);
        const float Vz = RdxMath::Dot(Q, C2.AZ);
        if (Vz < 1.0f)
            continue;
        const float Nx = RdxMath::Dot(Q, C2.AX) / (Vz * TanX), Ny = RdxMath::Dot(Q, C2.AY) / (Vz * TanY);
        if (fabsf(Nx) >= 1.0f || fabsf(Ny) >= 1.0f)
            continue;
        const float Px = (Nx * 0.5f + 0.5f) * (float)W - 0.5f, Py = (Ny * 0.5f + 0.5f) * (float)H - 0.5f;
        const int   Bx = (int)floorf(Px), By = (int)floorf(Py);
        float Nearest2 = 1e30f, Farthest2 = 0.0f;
        for (int q = 0; q < 4; q++)
        {
            const int   X = std::min(std::max(Bx + (q & 1), 0), (int)W - 1);
            const int   Y = std::min(std::max(By + (q >> 1), 0), (int)H - 1);
            const float Z = P2.Depth[(size_t)Y * W + X];
            const float D = (Z > 0.0f) ? 1.0f / Z : 1e30f;
            Nearest2  = fminf(Nearest2, D);
            Farthest2 = fmaxf(Farthest2, D);
        }
        const FCellRead B = CellAt(Cells2, i);
        if (bPillarPoint)
        {
            PillarBefore++;
            if (Nearest2 > Vz + RdxMath::ProbeCarveSlackAt(Vz))
            {
                SeenPast++;
                StillThere += (B.Dist > 0.0f && OnPillarOnly(Add(Centre, B.Point), B.Dist)) ? 1 : 0;
            }
        }
        else if (Farthest2 <= Vz + RdxMath::ProbeCarveSlackAt(Vz))
        {
            // A point of the room the second view sees no further than itself all round: it may not go.
            WallSeen++;
            WallKept += (B.Dist > 0.0f) ? 1 : 0;
        }
    }
    printf("  frame 2: %d cells held the pillar; the second view sees past %d of them, %d still hold it; %d points of "
           "the room seen again, %d kept\n", PillarBefore, SeenPast, StillThere, WallSeen, WallKept);
    Check(SeenPast > 50 && StillThere == 0, "what the eye now sees past - the pillar gone - is carved out");
    Check(WallSeen > 200 && WallKept == WallSeen, "what the eye sees where it was is kept");
    int Known2 = 0;
    for (int i = 0; i < Cells; i++)
        Known2 += (CellAt(Cells2, i).Dist > 0.0f) ? 1 : 0;
    printf("  frame 2's counts: %u carved out, %u taken from the picture, %u known (%d read back)\n", Counts2[0],
           Counts2[1], Counts2[2], Known2);
    Check(Counts2[0] >= (UINT)SeenPast && Counts2[2] == (UINT)Known2 && Counts2[3] == 0,
          "the second frame's counts: at least what the eye saw past carved, the known cells as read back");

    // ---- the third frame: the probe moved, and a picture that gives it nothing and takes nothing from it - an actor's
    // body right before the eye everywhere: no pixel of the level to take, and nothing seen past. What it held is to
    // go with it: each point into the cell it falls in round the new centre, the nearest where several do.
    const FVec3 Centre3 = { 110.0f, -60.0f, -10.0f };
    FPicture    P3;
    P3.Colour.assign((size_t)W * H * 4, 0.5f);
    P3.Depth.assign((size_t)W * H, 0.5f);
    P3.Over.assign((size_t)W * H * 4, 0.0f);
    P3.Kind.assign((size_t)W * H * 2, 0);
    for (size_t i = 0; i < (size_t)W * H; i++)
        P3.Kind[i * 2 + 1] = 1;
    std::vector<BYTE> Cells3, Levels3;
    UINT              Counts3[4] = {};
    if (!RunFrame(P3, C2, Centre3, &Centre, 3, 0, false, Cells3, Levels3, Counts3))
    {
        printf("  the third frame failed\n");
        return 2;
    }
    std::vector<float> Best3(Cells, 1e30f);
    std::vector<FVec3> Colour3(Cells);
    for (int i = 0; i < Cells; i++)
    {
        const FCellRead A = CellAt(Cells2, i);
        if (!(A.Dist > 0.0f))
            continue;
        const FVec3 From = Sub(Add(Centre, A.Point), Centre3);
        const float L    = Len(From);
        if (L > Reach || L < 1.0f)
            continue;
        const int J = CellOf(From, 0);
        if (J >= 0 && L < Best3[J])
        {
            Best3[J]   = L;
            Colour3[J] = A.Colour;
        }
    }
    // The colours of every point as near as the nearest within a hair: the pass may keep any of those (GatherCS's
    // rule), and they are the same point.
    std::vector<std::vector<FVec3>> Alike(Cells);
    for (int i = 0; i < Cells; i++)
    {
        const FCellRead A = CellAt(Cells2, i);
        if (!(A.Dist > 0.0f))
            continue;
        const FVec3 From = Sub(Add(Centre, A.Point), Centre3);
        const float L    = Len(From);
        const int   J    = (L > Reach || L < 1.0f) ? -1 : CellOf(From, 0);
        if (J >= 0 && L <= Best3[J] * 1.00002f)
            Alike[J].push_back(A.Colour);
    }
    int Known3 = 0, Agree3 = 0, Nearest3 = 0, Kept3 = 0;
    for (int j = 0; j < Cells; j++)
    {
        const FCellRead B      = CellAt(Cells3, j);
        const bool      bKnown = B.Dist > 0.0f, bWanted = Best3[j] < 1e29f;
        Known3 += bKnown ? 1 : 0;
        Agree3 += (bKnown == bWanted) ? 1 : 0;
        if (!bKnown || !bWanted)
            continue;
        Nearest3 += (Near(B.Dist, Best3[j], 1e-4f, 1e-3f) && Near(Len(B.Point), B.Dist, 1e-5f, 1e-3f) &&
                     CellOf(B.Point, 0) == j) ? 1 : 0;
        bool bOne = false;
        for (const FVec3& K : Alike[j])
            bOne = bOne || (B.Colour.X == K.X && B.Colour.Y == K.Y && B.Colour.Z == K.Z);
        Kept3 += bOne ? 1 : 0;
    }
    printf("  frame 3, moved: %d cells known round the new centre, %d where the rules want them, %d with the nearest "
           "point, %d with its colour; counts: %u carved, %u taken, %u taken over\n", Known3, Agree3, Nearest3, Kept3,
           Counts3[0], Counts3[1], Counts3[3]);
    Check(Known3 > 1000 && Agree3 == Cells && Nearest3 == Known3 && Kept3 == Known3,
          "as the probe moves, every point it held goes into the cell it falls in round the new centre, the nearest "
          "where several do, with its colour");
    Check(Counts3[0] == 0 && Counts3[1] == 0 && Counts3[3] == (UINT)Known3 && Counts3[2] == (UINT)Known3,
          "a picture that gives the moved probe nothing takes nothing from it; its counts say what it took over");

    printf("%d checks, %d failed\n", Checked, Failed);
    return Failed ? 1 : 0;
}

// RayThinCheck - Reflections=RTLow's pass over the rays' list (RaySparse.hlsl; docs/terv-nedves-feluletek.md 7.2) tried
// on the GPU without the game.
//
// An own list as a round's pixels write it at RTLow (SolidPass.hlsl, RtAskFlushLow) is made up here: blocks of
// requests at the pixels of a small screen, in an order of their own, each pixel's chained from its head, each block's
// sort beside its second request - a surface whose rays change smoothly over most of it, and the cases the pass has to
// tell apart: another sort of rays over a patch the tiles do not line up with, tiles whose middle pixel asks nothing,
// rays going two ways in turn, starts far apart, two blocks at a pixel, a tile with more sorts than it keeps to lend,
// blocks of three requests, and tiles cut by the screen's edges. The pass runs on it as the renderer runs it - built
// by fxc as cs_5_0 (an atomic a block) and as the renderer's build makes it, by dxc (a wave's blocks with one atomic) -
// and what it leaves is held against the same rules worked out here: which blocks are kept and which borrow which,
// byte for byte; the host's list holding the kept blocks' requests, each block's together where its first request
// says, nothing else and nothing twice. Then it is read as the build that reads the answers reads it (RtFind, RtAsk):
// every pixel's block finds answers to requests of its own words. The heads are as they were. And a list that asked
// more than it holds is left as it is.
//
//     RayThinCheck.exe [--debug]        --debug: the D3D12 debug layer

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <vector>

#include "../../src/RevisionDX12/RenderLimits.h"
// RaySparse.hlsl as dxc builds it for the renderer (this project's own build step).
#include "RaySparseCS.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace
{
    // The screen: neither side a multiple of three, so the last tiles are cut.
    const UINT W = 40, H = 26;
    const UINT Room = 8192;                     // each list's slots
    const float Eye[3] = { 0.0f, 0.0f, 0.0f };
    // RayListSlots.hlsli's numbers, as this check holds them.
    const float AlikeCos = 0.99f, AlikeFar = 0.03f;

    ID3D12Device*              Device     = nullptr;
    ID3D12CommandQueue*        Queue      = nullptr;
    ID3D12CommandAllocator*    Alloc      = nullptr;
    ID3D12GraphicsCommandList* List       = nullptr;
    ID3D12Fence*               Fence      = nullptr;
    UINT64                     FenceValue = 0;
    HANDLE                     FenceEvent = nullptr;
    bool                       bDebug     = false;
    ID3D12RootSignature*       Root       = nullptr;
    ID3D12PipelineState*       PsoFxc     = nullptr, *PsoDxc = nullptr;
    ID3D12DescriptorHeap*      Heap       = nullptr;
    ID3D12Resource*            HeadsTex   = nullptr, *LowBuf = nullptr, *TracedBuf = nullptr;
    ID3D12Resource*            Upload     = nullptr, *Readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT HeadsFoot = {};
    UINT64                     HeadsBytes = 0;
    const UINT64               ListBytes  = (UINT64)Room * RDX_RAY_SLOT_BYTES;

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

    // A slot as RenderLimits.h lays it out: where the ray starts and how far it may go; its way, the block's key and
    // the next block at the pixel (a block's first request alone; the second's key place holds the block's sort at
    // RTLow), and its word.
    struct FSlot
    {
        float    O[3];
        float    To;
        uint32_t Way, Key, Next, Word;
    };
    static_assert(sizeof(FSlot) == RDX_RAY_SLOT_BYTES, "a slot is RDX_RAY_SLOT_BYTES");

    // RayListSlots.hlsli's packing, as the shaders do it.
    uint32_t PackWay(const float* Way)
    {
        const float S = std::max(fabsf(Way[0]) + fabsf(Way[1]) + fabsf(Way[2]), 1e-20f);
        const float D[3] = { Way[0] / S, Way[1] / S, Way[2] / S };
        float E[2];
        if (D[2] >= 0.0f)
        {
            E[0] = D[0];
            E[1] = D[1];
        }
        else
        {
            E[0] = (1.0f - fabsf(D[1])) * ((D[0] >= 0.0f) ? 1.0f : -1.0f);
            E[1] = (1.0f - fabsf(D[0])) * ((D[1] >= 0.0f) ? 1.0f : -1.0f);
        }
        uint32_t Q[2];
        for (int k = 0; k < 2; k++)
            Q[k] = (uint32_t)(std::min(std::max(E[k] * 0.5f + 0.5f, 0.0f), 1.0f) * 65535.0f + 0.5f);
        return (Q[0] << 16) | Q[1];
    }
    void UnpackWay(uint32_t P, float* Out)
    {
        const float E[2] = { (float)(P >> 16) / 65535.0f * 2.0f - 1.0f, (float)(P & 0xFFFFu) / 65535.0f * 2.0f - 1.0f };
        float D[3] = { E[0], E[1], 1.0f - fabsf(E[0]) - fabsf(E[1]) };
        if (D[2] < 0.0f)
        {
            const float X = (1.0f - fabsf(E[1])) * ((E[0] >= 0.0f) ? 1.0f : -1.0f);
            const float Y = (1.0f - fabsf(E[0])) * ((E[1] >= 0.0f) ? 1.0f : -1.0f);
            D[0] = X;
            D[1] = Y;
        }
        const float L = sqrtf(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
        for (int k = 0; k < 3; k++)
            Out[k] = D[k] / L;
    }
    uint32_t PackWord(uint32_t Kind, uint32_t Mask, uint32_t Flags, float From, uint32_t Least)
    {
        return Kind | ((Mask & 0xFFu) << RDX_RAY_MASK_SHIFT) | Flags |
               (std::min((uint32_t)(std::max(From, 0.0f) * 16.0f + 0.5f), (uint32_t)RDX_RAY_FROM_MASK)
                << RDX_RAY_FROM_SHIFT) |
               (std::min(Least, 15u) << RDX_RAY_LEAST_SHIFT);
    }
    uint32_t WordCount(uint32_t Word) { return (Word >> RDX_RAY_COUNT_SHIFT) & RDX_RAY_COUNT_MASK; }
    uint32_t WordKind(uint32_t Word) { return Word & 3u; }
    uint32_t SortStep(uint32_t Sig, uint32_t Word) { return Sig * 0x9E3779B1u + Word; }
    uint32_t Bits(float F)
    {
        uint32_t U;
        memcpy(&U, &F, 4);
        return U;
    }

    // What a pixel asks, as SolidPass.hlsl writes it: its blocks in the order its runs draw them - each its requests'
    // words, its first ray's start and way, and the depth that keys it.
    struct FBlock
    {
        std::vector<uint32_t> Words;
        float                 O[3];
        float                 Way[3];
        uint32_t              Key;
    };
    std::vector<std::vector<FBlock>> Asks;      // W * H pixels

    // The own list and the heads as the pixels leave them (RtAskFlushLow): the pixels in an order of their own (the
    // card's), each one's blocks taking their slots together and chained from its head, the last written first; a
    // block's sort - its length, then its words as the list holds them - beside its second request.
    void WriteList(std::vector<FSlot>& Slots, std::vector<uint32_t>& Heads)
    {
        Slots.assign(Room, FSlot{});
        Heads.assign((size_t)W * H, 0u);
        std::vector<UINT> Order((size_t)W * H);
        for (UINT i = 0; i < W * H; i++)
            Order[i] = i;
        uint32_t Seed = 12345u;
        for (UINT i = W * H - 1; i > 0; i--)
        {
            Seed = Seed * 1664525u + 1013904223u;
            std::swap(Order[i], Order[(Seed >> 8) % (i + 1)]);
        }
        uint32_t Next = 1;
        for (UINT p : Order)
            for (const FBlock& B : Asks[p])
            {
                const uint32_t First = Next, Count = (uint32_t)B.Words.size();
                Next += Count;
                for (uint32_t i = 0; i < Count; i++)
                {
                    FSlot& S = Slots[First + i];
                    // A later request of the block starts a little further on, as a lamp's ray from what the first met.
                    for (int k = 0; k < 3; k++)
                        S.O[k] = B.O[k] + (float)i * 3.0f;
                    S.To   = 65536.0f;
                    S.Way  = PackWay(B.Way);
                    S.Word = B.Words[i];
                }
                Slots[First].Key   = B.Key;
                Slots[First].Next  = Heads[p];
                Slots[First].Word |= Count << RDX_RAY_COUNT_SHIFT;
                uint32_t Sig = Count;
                for (uint32_t i = 0; i < Count; i++)
                    Sig = SortStep(Sig, Slots[First + i].Word);
                // The sort in the first request's reach, the reach beside the second request.
                if (Count > 1)
                {
                    Slots[First + 1].Key = Bits(Slots[First].To);
                    memcpy(&Slots[First].To, &Sig, 4);
                }
                Heads[p] = First;
            }
        uint32_t* const Head0 = (uint32_t*)&Slots[0];
        Head0[0] = Next;
        Head0[1] = Room;
    }

    // RaySparse.hlsl's rules, worked out here on the same list: the own list as the pass leaves it - but for where a
    // kept block's requests went, which is the card's to choose (Kept: the kept blocks' first slots, in no order).
    uint32_t SortOf(const std::vector<FSlot>& Slots, uint32_t At)
    {
        const uint32_t Count = WordCount(Slots[At].Word);
        return (Count > 1) ? Bits(Slots[At].To) : SortStep(1, Slots[At].Word);
    }
    // Where a kept block says where its requests went (RayListSlots.hlsli, RaySparseAnswerAt): the slot and the uint.
    uint32_t* AnswerOf(std::vector<FSlot>& Slots, uint32_t At)
    {
        return (WordCount(Slots[At].Word) > 1) ? &Slots[At + 1].Next : (uint32_t*)&Slots[At].To;
    }
    // A request of a kept block as the host's list is to hold it: the first one's reach back in its place.
    FSlot ForHost(const std::vector<FSlot>& Slots, uint32_t At, uint32_t i)
    {
        FSlot S = Slots[At + i];
        if (i == 0 && WordCount(Slots[At].Word) > 1)
            memcpy(&S.To, &Slots[At + 1].Key, 4);
        return S;
    }
    void ThinHere(std::vector<FSlot>& Slots, const std::vector<uint32_t>& Heads, std::vector<uint32_t>& Kept)
    {
        Kept.clear();
        const std::vector<FSlot> Before = Slots;
        const uint32_t Taken = ((const uint32_t*)&Slots[0])[0], Held = ((const uint32_t*)&Slots[0])[1];
        if (Taken > Held)
            return;
        auto HeadAt = [&](UINT x, UINT y) { return (x < W && y < H) ? Heads[(size_t)y * W + x] : 0u; };
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++)
            {
                uint32_t At = HeadAt(x, y);
                if (At == 0)
                    continue;
                // The tile's lender: the first pixel that asks of its middle's two by two, or the first of the tile's
                // pixels that asks, row by row.
                const UINT Bx = x / 3 * 3, By = y / 3 * 3;
                const UINT Fx = (Bx + 1) & ~1u, Fy = (By + 1) & ~1u;
                UINT Lx = x, Ly = y;
                bool bFound = false;
                for (UINT q = 0; q < 4 && !bFound; q++)
                    if (HeadAt(Fx + (q & 1), Fy + (q >> 1)) != 0)
                    {
                        Lx     = Fx + (q & 1);
                        Ly     = Fy + (q >> 1);
                        bFound = true;
                    }
                for (UINT k = 0; k < 9 && !bFound; k++)
                    if (HeadAt(Bx + k % 3, By + k / 3) != 0)
                    {
                        Lx     = Bx + k % 3;
                        Ly     = By + k / 3;
                        bFound = true;
                    }
                const bool     bLender  = Lx == x && Ly == y;
                const uint32_t LenderAt = HeadAt(Lx, Ly);
                for (UINT n = 0; n < RDX_RAY_CHAIN_MOST && At != 0 && At < Taken; n++)
                {
                    const FSlot    S     = Before[At];
                    const uint32_t Count = WordCount(S.Word);
                    if (Count != 0 && Count <= Taken - At)
                    {
                        uint32_t Lent = 0;
                        if (!bLender)
                        {
                            const uint32_t Sig = SortOf(Before, At);
                            float Way[3];
                            UnpackWay(S.Way, Way);
                            const float G[3] = { S.O[0] - Eye[0], S.O[1] - Eye[1], S.O[2] - Eye[2] };
                            const float Reach = AlikeFar * sqrtf(G[0] * G[0] + G[1] * G[1] + G[2] * G[2]) + 1.0f;
                            uint32_t L = LenderAt;
                            for (UINT m = 0; m < RDX_RAY_CHAIN_MOST && L != 0 && L < Taken && Lent == 0; m++)
                            {
                                const FSlot& LS = Before[L];
                                const uint32_t LCount = WordCount(LS.Word);
                                if (LCount != 0 && LCount <= Taken - L && SortOf(Before, L) == Sig)
                                {
                                    float LWay[3];
                                    UnpackWay(LS.Way, LWay);
                                    const float Gap[3] = { S.O[0] - LS.O[0], S.O[1] - LS.O[1], S.O[2] - LS.O[2] };
                                    const float Cos = Way[0] * LWay[0] + Way[1] * LWay[1] + Way[2] * LWay[2];
                                    if (Gap[0] * Gap[0] + Gap[1] * Gap[1] + Gap[2] * Gap[2] < Reach * Reach && Cos > AlikeCos)
                                        Lent = L;
                                }
                                L = LS.Next;
                            }
                        }
                        if (Lent != 0)
                        {
                            Slots[At].Word &= ~3u;
                            memcpy(&Slots[At].O[0], &Lent, 4);
                        }
                        else
                            Kept.push_back(At);
                    }
                    At = S.Next;
                }
            }
    }

    // ---- the device, as the renderer binds the pass: the heads (u0) a table, the two lists (u1, u2) by address; the
    // constants.
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
    ID3D12PipelineState* MakePso(const void* Code, SIZE_T Bytes)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC P = {};
        P.pRootSignature     = Root;
        P.CS.pShaderBytecode = Code;
        P.CS.BytecodeLength  = Bytes;
        ID3D12PipelineState* Pso = nullptr;
        return SUCCEEDED(Device->CreateComputePipelineState(&P, IID_PPV_ARGS(&Pso))) ? Pso : nullptr;
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

        D3D12_DESCRIPTOR_RANGE Range = {};
        Range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        Range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER Params[4] = {};
        Params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[0].DescriptorTable.NumDescriptorRanges = 1;
        Params[0].DescriptorTable.pDescriptorRanges   = &Range;
        Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_UAV;
        Params[1].Descriptor.ShaderRegister           = 1;
        Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_UAV;
        Params[2].Descriptor.ShaderRegister           = 2;
        Params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        Params[3].Constants.Num32BitValues            = 8;
        D3D12_ROOT_SIGNATURE_DESC Rd = {};
        Rd.NumParameters = 4;
        Rd.pParameters   = Params;
        ID3DBlob* Blob = nullptr;
        ID3DBlob* Err  = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&Rd, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err)) ||
            FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(), IID_PPV_ARGS(&Root))))
            return false;
        Blob->Release();

        ID3DBlob* Code = nullptr;
        if (FAILED(D3DCompileFromFile(ShaderFile, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "SparseCS", "cs_5_0",
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Err)))
        {
            printf("  SparseCS did not compile: %s\n", Err ? (const char*)Err->GetBufferPointer() : "(no message)");
            return false;
        }
        PsoFxc = MakePso(Code->GetBufferPointer(), Code->GetBufferSize());
        Code->Release();
        PsoDxc = MakePso(GRaySparseCS, sizeof(GRaySparseCS));
        if (!PsoFxc || !PsoDxc)
        {
            printf("  a pipeline could not be made (fxc's: %s, dxc's: %s)\n", PsoFxc ? "made" : "not made",
                   PsoDxc ? "made" : "not made");
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC Hd = {};
        Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        Hd.NumDescriptors = 1;
        Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Device->CreateDescriptorHeap(&Hd, IID_PPV_ARGS(&Heap));

        D3D12_HEAP_PROPERTIES Hp = {};
        Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC D = {};
        D.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        D.Width            = W;
        D.Height           = H;
        D.DepthOrArraySize = 1;
        D.MipLevels        = 1;
        D.Format           = DXGI_FORMAT_R32_UINT;
        D.SampleDesc.Count = 1;
        D.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &D, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&HeadsTex))))
            return false;
        UINT64 Total = 0;
        Device->GetCopyableFootprints(&D, 0, 1, 0, &HeadsFoot, nullptr, nullptr, &Total);
        HeadsBytes = (Total + 511) & ~(UINT64)511;
        // A buffer starts in COMMON whatever it is made with; the first copy into it takes it to COPY_DEST.
        LowBuf    = MakeBuffer(ListBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COMMON);
        TracedBuf = MakeBuffer(ListBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COMMON);
        Upload    = MakeBuffer(HeadsBytes + 2 * ListBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
        Readback  = MakeBuffer(HeadsBytes + 2 * ListBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
        if (!LowBuf || !TracedBuf || !Upload || !Readback)
            return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC U = {};
        U.Format        = DXGI_FORMAT_R32_UINT;
        U.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Device->CreateUnorderedAccessView(HeadsTex, nullptr, &U, Heap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }

    // The pass on the own list, its heads and the host's list as BeginAsk empties it (its count at one); what it
    // leaves read back.
    bool RunPass(ID3D12PipelineState* Pso, const std::vector<FSlot>& Low, const std::vector<uint32_t>& Heads,
                 std::vector<FSlot>& OutLow, std::vector<FSlot>& OutTraced, std::vector<uint32_t>& OutHeads)
    {
        std::vector<FSlot> Traced(Room, FSlot{});
        ((uint32_t*)&Traced[0])[0] = 1;
        ((uint32_t*)&Traced[0])[1] = Room;
        // What lies past the host's count is another round's: something there, not nothing.
        for (UINT i = 1; i < Room; i++)
            Traced[i].Word = 0xDEAD0001u;
        BYTE* Up = nullptr;
        if (FAILED(Upload->Map(0, nullptr, (void**)&Up)))
            return false;
        for (UINT y = 0; y < H; y++)
            memcpy(Up + HeadsFoot.Offset + (size_t)y * HeadsFoot.Footprint.RowPitch, &Heads[(size_t)y * W], W * 4);
        memcpy(Up + HeadsBytes, Low.data(), (size_t)ListBytes);
        memcpy(Up + HeadsBytes + ListBytes, Traced.data(), (size_t)ListBytes);
        Upload->Unmap(0, nullptr);

        Open();
        D3D12_TEXTURE_COPY_LOCATION Dst = {}, Src = {};
        Dst.pResource       = HeadsTex;
        Dst.Type            = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.pResource       = Upload;
        Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint = HeadsFoot;
        List->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        List->CopyBufferRegion(LowBuf, 0, Upload, HeadsBytes, ListBytes);
        List->CopyBufferRegion(TracedBuf, 0, Upload, HeadsBytes + ListBytes, ListBytes);
        Barrier(HeadsTex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(LowBuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(TracedBuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uint32_t K[8] = { W, H, 0, 0, 0, 0, 0, 0 };
        memcpy(&K[4], Eye, sizeof(Eye));
        List->SetDescriptorHeaps(1, &Heap);
        List->SetComputeRootSignature(Root);
        List->SetComputeRootDescriptorTable(0, Heap->GetGPUDescriptorHandleForHeapStart());
        List->SetComputeRootUnorderedAccessView(1, LowBuf->GetGPUVirtualAddress());
        List->SetComputeRootUnorderedAccessView(2, TracedBuf->GetGPUVirtualAddress());
        List->SetComputeRoot32BitConstants(3, 8, K, 0);
        List->SetPipelineState(Pso);
        List->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        Barrier(HeadsTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(LowBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(TracedBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION RDst = {}, RSrc = {};
        RDst.pResource       = Readback;
        RDst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        RDst.PlacedFootprint = HeadsFoot;
        RSrc.pResource       = HeadsTex;
        RSrc.Type            = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        List->CopyTextureRegion(&RDst, 0, 0, 0, &RSrc, nullptr);
        List->CopyBufferRegion(Readback, HeadsBytes, LowBuf, 0, ListBytes);
        List->CopyBufferRegion(Readback, HeadsBytes + ListBytes, TracedBuf, 0, ListBytes);
        Barrier(HeadsTex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(LowBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(TracedBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        ExecuteAndWait();
        if (DrainDebug() != 0)
            return false;

        BYTE* Back = nullptr;
        if (FAILED(Readback->Map(0, nullptr, (void**)&Back)))
            return false;
        OutHeads.assign((size_t)W * H, 0u);
        for (UINT y = 0; y < H; y++)
            memcpy(&OutHeads[(size_t)y * W], Back + HeadsFoot.Offset + (size_t)y * HeadsFoot.Footprint.RowPitch, W * 4);
        OutLow.assign(Room, FSlot{});
        memcpy(OutLow.data(), Back + HeadsBytes, (size_t)ListBytes);
        OutTraced.assign(Room, FSlot{});
        memcpy(OutTraced.data(), Back + HeadsBytes + ListBytes, (size_t)ListBytes);
        D3D12_RANGE None = { 0, 0 };
        Readback->Unmap(0, &None);
        return true;
    }

    // ---- the screen's asks.
    uint32_t WordA(uint32_t Least) { return PackWord(RDX_RAY_KIND_FIRST, 7u, RDX_RAY_FLAG_LAMPS | RDX_RAY_FLAG_THIN, 2.0f, Least); }
    uint32_t WordB() { return PackWord(RDX_RAY_KIND_ANY, 3u, 0u, 2.0f, 3u); }
    FBlock Surface(UINT x, UINT y, float Dist, uint32_t Key)
    {
        FBlock B;
        B.Words = { WordA(3u), WordB() };
        B.O[0]  = (float)x * 0.5f;
        B.O[1]  = (float)y * 0.5f;
        B.O[2]  = Dist;
        const float L = sqrtf(0.09f + 0.04f + 1.0f);
        B.Way[0] = (0.3f + 0.002f * (float)x) / L;
        B.Way[1] = (0.2f + 0.002f * (float)y) / L;
        B.Way[2] = 1.0f / L;
        B.Key    = Key;
        return B;
    }
    bool In(UINT x, UINT y, UINT X0, UINT Y0, UINT X1, UINT Y1) { return x >= X0 && x < X1 && y >= Y0 && y < Y1; }

    void MakeAsks()
    {
        Asks.assign((size_t)W * H, {});
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++)
            {
                std::vector<FBlock>& A = Asks[(size_t)y * W + x];
                const uint32_t Key = 0x3C000000u + y * W + x;
                FBlock B = Surface(x, y, 300.0f, Key);
                if (In(x, y, 10, 4, 17, 11))
                    B.Words[0] = WordA(5u);                     // another sort, on a patch off the tiles' grid
                if (In(x, y, 18, 0, 27, 9) && x % 3 == 1 && y % 3 == 1)
                    continue;                                   // the tiles' middles ask nothing
                if (In(x, y, 27, 0, 33, 6) && ((x + y) & 1))
                {
                    B.Way[0] = 0.96f;                           // every other pixel's ray goes another way
                    B.Way[1] = 0.0f;
                    B.Way[2] = 0.28f;
                }
                if (In(x, y, 27, 9, 36, 15) && (x & 1))
                    B.O[2] = 800.0f;                            // every other column starts far behind
                if (In(x, y, 12, 18, 15, 21))
                    B.Words[0] = PackWord(RDX_RAY_KIND_FIRST, 1u + (y - 18) * 3 + (x - 12), RDX_RAY_FLAG_LAMPS, 2.0f, 3u);
                if (In(x, y, 15, 18, 18, 21))
                    B.Words[0] = WordA(((y - 18) * 3 + (x - 15)) & 1);   // two sorts in turn
                if (In(x, y, 18, 21, 27, 26))
                    B.Words.push_back(PackWord(RDX_RAY_KIND_ANY, 3u, 0u, 1.0f, 3u));   // blocks of three
                if (In(x, y, 36, 15, 40, 21))
                    B.Words.resize(1);                          // blocks of one request
                if (In(x, y, 0, 21, 9, 26) && ((x | y) & 1))
                    continue;                                   // a two by two's first pixel asks for the four (RTHalf)
                A.push_back(B);
                if (In(x, y, 0, 15, 9, 21))
                {
                    FBlock Far = Surface(x, y, 600.0f, Key + 0x100000u);   // a second pane behind the first
                    A.insert(A.begin(), Far);
                }
            }
    }

    // One build of the pass on the screen's list, held against the rules.
    void CheckBuild(const char* Name, ID3D12PipelineState* Pso, const std::vector<FSlot>& Slots,
                    const std::vector<uint32_t>& Heads, const std::vector<FSlot>& Want, const std::vector<uint32_t>& Kept)
    {
        printf(" %s:\n", Name);
        std::vector<FSlot>    Got, Traced;
        std::vector<uint32_t> GotHeads;
        if (!RunPass(Pso, Slots, Heads, Got, Traced, GotHeads))
        {
            Check(false, "the pass runs");
            return;
        }
        const uint32_t Taken = ((const uint32_t*)&Slots[0])[0];
        char What[256];

        // The own list: as the rules leave it, but for where the kept blocks' requests went.
        std::vector<FSlot> Masked = Got, WantCopy = Want;
        for (uint32_t At : Kept)
            *AnswerOf(Masked, At) = *AnswerOf(WantCopy, At);
        Check(memcmp(Masked.data(), Want.data(), (size_t)ListBytes) == 0,
              "the own list: the same blocks kept, the same borrowing the same, byte for byte");
        Check(GotHeads == Heads, "the heads are as the pixels left them");

        // The host's list: the kept blocks' requests, each block's together from where its first request says, as the
        // pixels wrote them - and nothing else.
        const uint32_t Count = ((const uint32_t*)&Traced[0])[0];
        uint32_t KeptRequests = 0;
        for (uint32_t At : Kept)
            KeptRequests += WordCount(Slots[At].Word);
        sprintf_s(What, "the host's list holds the kept blocks' requests: %u slots taken for %u", Count - 1, KeptRequests);
        Check(Count == 1 + KeptRequests && ((const uint32_t*)&Traced[0])[1] == Room, What);
        std::vector<int> Owner(Room, -1);
        int Wrong = 0, Twice = 0;
        for (uint32_t At : Kept)
        {
            const uint32_t First = *AnswerOf(Got, At);
            const uint32_t N     = WordCount(Slots[At].Word);
            if (First == 0 || First + N > Count)
            {
                Wrong++;
                continue;
            }
            for (uint32_t i = 0; i < N; i++)
            {
                Twice += (Owner[First + i] >= 0) ? 1 : 0;
                Owner[First + i] = (int)At;
                const FSlot Host = ForHost(Slots, At, i);
                Wrong += (memcmp(&Traced[First + i], &Host, RDX_RAY_SLOT_BYTES) != 0) ? 1 : 0;
            }
        }
        int Unowned = 0;
        for (uint32_t s = 1; s < Count; s++)
            Unowned += (Owner[s] < 0) ? 1 : 0;
        sprintf_s(What, "each kept block's requests lie together where it says, as written (%d wrong, %d twice, %d of "
                  "nobody's)", Wrong, Twice, Unowned);
        Check(Wrong == 0 && Twice == 0 && Unowned == 0, What);

        // Read as RtFind and RtAsk read it: each pixel's blocks by their keys; a kept one's answers where it says, a
        // borrowing one's where the block it names says - requests of the pixel's own words, the block's length aside.
        int Blocks = 0, NoAnswers = 0, OtherWords = 0, Lost = 0, Borrowing = 0;
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++)
                for (const FBlock& B : Asks[(size_t)y * W + x])
                {
                    Blocks++;
                    uint32_t At = GotHeads[(size_t)y * W + x], Own = 0;
                    for (UINT n = 0; n < RDX_RAY_CHAIN_MOST && At != 0 && At < Taken; n++)
                    {
                        if (Got[At].Key == B.Key)
                        {
                            Own = At;
                            break;
                        }
                        At = Got[At].Next;
                    }
                    if (Own == 0)
                    {
                        Lost++;
                        continue;
                    }
                    uint32_t KeptAt = Own;
                    if (WordKind(Got[Own].Word) == RDX_RAY_KIND_NONE)
                    {
                        memcpy(&KeptAt, &Got[Own].O[0], 4);
                        Borrowing++;
                    }
                    uint32_t Answer = 0;
                    if (KeptAt != 0 && KeptAt < Taken)
                        Answer = *AnswerOf(Got, KeptAt);
                    const uint32_t N = (KeptAt != 0 && KeptAt < Taken) ? WordCount(Got[KeptAt].Word) : 0;
                    if (Answer == 0 || WordKind(Got[KeptAt].Word) == RDX_RAY_KIND_NONE || N != B.Words.size() ||
                        Answer + N > Count)
                    {
                        NoAnswers++;
                        continue;
                    }
                    for (uint32_t i = 0; i < N; i++)
                        if (((Traced[Answer + i].Word ^ B.Words[i]) & ~(RDX_RAY_COUNT_MASK << RDX_RAY_COUNT_SHIFT)) != 0)
                        {
                            OtherWords++;
                            break;
                        }
                }
        printf("  %d blocks: %zu kept for the host (%u requests), %d borrowing (%.1f per kept one)\n", Blocks, Kept.size(),
               KeptRequests, Borrowing, Kept.empty() ? 0.0 : (double)Borrowing / (double)Kept.size());
        Check(Lost == 0, "every block is found at its pixel by its key, as RtFind finds it");
        Check(NoAnswers == 0, "every block finds answers of its own length in the host's list, its own or the one it borrows");
        Check(OtherWords == 0, "every block's answers are to requests of its own words");

        // The cases one by one: the blocks kept in a tile.
        auto KeptIn = [&](UINT tx, UINT ty)
        {
            int N = 0;
            for (UINT y = ty * 3; y < std::min(ty * 3 + 3, H); y++)
                for (UINT x = tx * 3; x < std::min(tx * 3 + 3, W); x++)
                    for (uint32_t At = GotHeads[(size_t)y * W + x]; At != 0; At = Got[At].Next)
                        N += (WordKind(Got[At].Word) != RDX_RAY_KIND_NONE) ? 1 : 0;
            return N;
        };
        auto Kept1 = [&](UINT x, UINT y)
        {
            const uint32_t At = GotHeads[(size_t)y * W + x];
            return At != 0 && WordKind(Got[At].Word) != RDX_RAY_KIND_NONE;
        };
        // A tile's lender is its middle's two by two's first pixel: (0, 0) for the tile at (0, 0), (36, 0) for (12, 0).
        Check(KeptIn(0, 0) == 1 && KeptIn(12, 0) == 1 && Kept1(0, 0) && Kept1(36, 0),
              "one smooth surface: a tile keeps its lender's block alone, its middle's four's first");
        Check(KeptIn(13, 0) == 1 && KeptIn(13, 8) == 1 && KeptIn(0, 8) == 1,
              "a tile the screen's edges cut keeps one block too");
        // The patch of the other sort covers the lender of (3, 1) and four of its pixels: the other five keep their
        // own; it covers (4, 2) whole; (5, 2)'s lender and two thirds of it, the third column keeping its own.
        Check(KeptIn(3, 1) == 6 && KeptIn(4, 2) == 1 && KeptIn(5, 2) == 4,
              "another sort over a patch: in a tile it cuts, the pixels not of the lender's sort keep their own");
        Check(KeptIn(6, 0) == 1 && Kept1(18, 0), "a tile whose middle asks nothing lends its four's first block");
        // (28, 0) and (30, 4) lend, their rays the smooth surface's way: the five going the other way keep their own.
        Check(KeptIn(9, 0) == 6 && KeptIn(10, 1) == 6,
              "rays going two ways in turn: the pixels going the other way from the lender's keep their own");
        Check(KeptIn(9, 3) == 7 && KeptIn(11, 4) == 7,
              "starts far apart: the pixels of the other columns than the lender's keep their own");
        Check(KeptIn(0, 5) == 2 && KeptIn(2, 6) == 2, "two blocks at each pixel: each borrows the lender's of its pane");
        Check(KeptIn(4, 6) == 9, "nine sorts in a tile: all nine kept");
        // The lender (16, 18) is of the odd sort: the five of the even one keep their own.
        Check(KeptIn(5, 6) == 6, "two sorts in turn: the pixels not of the lender's sort keep their own");
        Check(KeptIn(6, 7) == 1 && KeptIn(8, 8) == 1, "blocks of three requests: a tile keeps one");
        Check(KeptIn(12, 5) == 1 && KeptIn(13, 6) == 1, "blocks of one request: a tile keeps one");
        Check(KeptIn(0, 7) == 1 && KeptIn(1, 7) == 1 && KeptIn(2, 7) == 1 && Kept1(0, 22) && Kept1(4, 22) &&
                  Kept1(6, 22),
              "the fours' first pixels asking for them, as at RTHalf: a tile keeps its lender's block alone");

        // A round that asked more than the own list holds: RtFind gives it up whole, and the pass leaves it alone.
        std::vector<FSlot> Over = Slots, OverGot, OverTraced;
        ((uint32_t*)&Over[0])[0] = Room + 5;
        std::vector<uint32_t> OverHeads;
        if (!RunPass(Pso, Over, Heads, OverGot, OverTraced, OverHeads))
        {
            Check(false, "the pass runs on a list that asked too much");
            return;
        }
        Check(memcmp(OverGot.data(), Over.data(), (size_t)ListBytes) == 0 && OverHeads == Heads &&
                  ((const uint32_t*)&OverTraced[0])[0] == 1,
              "a list that asked more than it holds is left as it is, and nothing goes to the host");
    }
}

namespace
{
    // ---- the timing (--bench): a 1920 x 1080 screen, every pixel one block of two requests, the blocks taken as a
    // pixel shader's waves take them - eight by eight pixels together, those row after row - the rays turning
    // smoothly, and a twentieth of the pixels, in patches, of another sort. The pass is timed on the GPU 20 times
    // over the same list, each time put back first; the middle time is what it says.
    const UINT BW = 1920, BH = 1080;

    // bFours: only each two by two's first pixel asks, as at RTHalf.
    bool Bench(ID3D12PipelineState* Pso, const char* Name, bool bFours)
    {
        const UINT64 BRoom  = (UINT64)BW * BH * 2 + 1;
        const UINT64 BBytes = BRoom * RDX_RAY_SLOT_BYTES;
        std::vector<FSlot>    Low((size_t)BRoom, FSlot{});
        std::vector<uint32_t> BHeads((size_t)BW * BH, 0u);
        uint32_t Next = 1;
        for (UINT ty = 0; ty < BH / 8; ty++)
            for (UINT tx = 0; tx < BW / 8; tx++)
                for (UINT p = 0; p < 64; p++)
                {
                    const UINT x = tx * 8 + p % 8, y = ty * 8 + p / 8;
                    if (bFours && ((x | y) & 1))
                        continue;
                    FBlock B = Surface(x, y, 300.0f + (float)y * 0.05f, 0x3C000000u + y * BW + x);
                    if (((x / 37 + y / 23) % 20) == 0)
                        B.Words[0] = WordA(5u);
                    const uint32_t First = Next;
                    Next += 2;
                    for (uint32_t i = 0; i < 2; i++)
                    {
                        FSlot& S = Low[First + i];
                        for (int k = 0; k < 3; k++)
                            S.O[k] = B.O[k] + (float)i * 3.0f;
                        S.To   = 65536.0f;
                        S.Way  = PackWay(B.Way);
                        S.Word = B.Words[i];
                    }
                    Low[First].Key   = B.Key;
                    Low[First].Word |= 2u << RDX_RAY_COUNT_SHIFT;
                    const uint32_t Sig = SortStep(SortStep(2u, Low[First].Word), Low[First + 1].Word);
                    Low[First + 1].Key = Bits(Low[First].To);
                    memcpy(&Low[First].To, &Sig, 4);
                    BHeads[(size_t)y * BW + x] = First;
                }
        ((uint32_t*)&Low[0])[0] = Next;
        ((uint32_t*)&Low[0])[1] = (uint32_t)BRoom;

        D3D12_HEAP_PROPERTIES Hp = {};
        Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC D = {};
        D.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        D.Width            = BW;
        D.Height           = BH;
        D.DepthOrArraySize = 1;
        D.MipLevels        = 1;
        D.Format           = DXGI_FORMAT_R32_UINT;
        D.SampleDesc.Count = 1;
        D.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* Tex = nullptr;
        if (FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &D, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&Tex))))
            return false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Foot = {};
        UINT64 TexBytes = 0;
        Device->GetCopyableFootprints(&D, 0, 1, 0, &Foot, nullptr, nullptr, &TexBytes);
        TexBytes = (TexBytes + 65535) & ~(UINT64)65535;
        ID3D12Resource* Pristine = MakeBuffer(BBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                              D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource* Work     = MakeBuffer(BBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                              D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource* Host     = MakeBuffer(BBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                              D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource* Up       = MakeBuffer(TexBytes + BBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                              D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource* Back     = MakeBuffer(4096, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                              D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_QUERY_HEAP_DESC Qd = {};
        Qd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        Qd.Count = 64;
        ID3D12QueryHeap* Times = nullptr;
        Device->CreateQueryHeap(&Qd, IID_PPV_ARGS(&Times));
        if (!Pristine || !Work || !Host || !Up || !Back || !Times)
            return false;
        BYTE* Mapped = nullptr;
        if (FAILED(Up->Map(0, nullptr, (void**)&Mapped)))
            return false;
        for (UINT y = 0; y < BH; y++)
            memcpy(Mapped + Foot.Offset + (size_t)y * Foot.Footprint.RowPitch, &BHeads[(size_t)y * BW], BW * 4);
        memcpy(Mapped + TexBytes, Low.data(), (size_t)BBytes);
        // The host's list as BeginAsk leaves it: its count at one, and its room.
        memcpy(Mapped + TexBytes + BBytes - RDX_RAY_SLOT_BYTES, &Low[0], RDX_RAY_SLOT_BYTES);
        ((uint32_t*)(Mapped + TexBytes + BBytes - RDX_RAY_SLOT_BYTES))[0] = 1;
        Up->Unmap(0, nullptr);
        ID3D12DescriptorHeap* BHeap = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC Hd = {};
        Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        Hd.NumDescriptors = 1;
        Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Device->CreateDescriptorHeap(&Hd, IID_PPV_ARGS(&BHeap));
        D3D12_UNORDERED_ACCESS_VIEW_DESC U = {};
        U.Format        = DXGI_FORMAT_R32_UINT;
        U.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Device->CreateUnorderedAccessView(Tex, nullptr, &U, BHeap->GetCPUDescriptorHandleForHeapStart());

        Open();
        D3D12_TEXTURE_COPY_LOCATION Dst = {}, Src = {};
        Dst.pResource       = Tex;
        Dst.Type            = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.pResource       = Up;
        Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint = Foot;
        List->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        List->CopyBufferRegion(Pristine, 0, Up, TexBytes, BBytes);
        Barrier(Tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(Pristine, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ExecuteAndWait();
        const int Runs = 20;
        Open();
        for (int r = 0; r < Runs; r++)
        {
            Barrier(Work, (r == 0) ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(Host, (r == 0) ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            List->CopyBufferRegion(Work, 0, Pristine, 0, BBytes);
            List->CopyBufferRegion(Host, 0, Up, TexBytes + BBytes - RDX_RAY_SLOT_BYTES, RDX_RAY_SLOT_BYTES);
            Barrier(Work, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(Host, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            uint32_t K[8] = { BW, BH, 0, 0, 0, 0, 0, 0 };
            memcpy(&K[4], Eye, sizeof(Eye));
            List->SetDescriptorHeaps(1, &BHeap);
            List->SetComputeRootSignature(Root);
            List->SetComputeRootDescriptorTable(0, BHeap->GetGPUDescriptorHandleForHeapStart());
            List->SetComputeRootUnorderedAccessView(1, Work->GetGPUVirtualAddress());
            List->SetComputeRootUnorderedAccessView(2, Host->GetGPUVirtualAddress());
            List->SetComputeRoot32BitConstants(3, 8, K, 0);
            List->SetPipelineState(Pso);
            List->EndQuery(Times, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)r * 2);
            List->Dispatch((BW + 7) / 8, (BH + 7) / 8, 1);
            List->EndQuery(Times, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)r * 2 + 1);
        }
        List->ResolveQueryData(Times, D3D12_QUERY_TYPE_TIMESTAMP, 0, Runs * 2, Back, 0);
        Barrier(Host, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        List->CopyBufferRegion(Back, 1024, Host, 0, 16);
        ExecuteAndWait();
        UINT64 Freq = 1;
        Queue->GetTimestampFrequency(&Freq);
        UINT64* Ticks = nullptr;
        Back->Map(0, nullptr, (void**)&Ticks);
        std::vector<double> Ms;
        for (int r = 0; r < Runs; r++)
            Ms.push_back((double)(Ticks[r * 2 + 1] - Ticks[r * 2]) * 1000.0 / (double)Freq);
        const uint32_t TracedCount = ((const uint32_t*)Ticks)[1024 / 4];
        Back->Unmap(0, nullptr);
        std::sort(Ms.begin(), Ms.end());
        printf("  %s%s: %.3f ms the middle of %d runs (%.3f .. %.3f) at %u x %u - %u blocks, %u requests traced\n", Name,
               bFours ? " (the fours' first pixels ask)" : "", Ms[Runs / 2], Runs, Ms.front(), Ms.back(), BW, BH,
               (Next - 1) / 2, TracedCount - 1);
        const bool bOk = DrainDebug() == 0;
        Times->Release();
        BHeap->Release();
        Back->Release();
        Up->Release();
        Host->Release();
        Work->Release();
        Pristine->Release();
        Tex->Release();
        return bOk;
    }
}

int main(int argc, char** argv)
{
    bool bBench = false;
    const char* Other = nullptr;    // --bench --shader <file>: a variant of the pass timed beside it, built by fxc
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--debug") == 0)
            bDebug = true;
        if (strcmp(argv[i], "--bench") == 0)
            bBench = true;
        if (strcmp(argv[i], "--shader") == 0 && i + 1 < argc)
            Other = argv[++i];
    }
    printf("RayThinCheck - Reflections=RTLow's pass over the rays' list, on the GPU\n");

    // The pass from the source tree, with the same includes as the renderer's build.
    wchar_t Here[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, __FILE__, -1, Here, MAX_PATH);
    wchar_t* Slash = wcsrchr(Here, L'\\');
    if (!Slash)
        Slash = wcsrchr(Here, L'/');
    if (!Slash)
        return 2;
    wcscpy_s(Slash + 1, MAX_PATH - (Slash + 1 - Here), L"..\\..\\src\\RevisionDX12\\RaySparse.hlsl");
    if (!MakeEverything(Here))
    {
        printf("  the device, the pass or the buffers could not be made\n");
        return 2;
    }
    if (bBench)
    {
        bool bOk = Bench(PsoDxc, "dxc, cs_6_0 (the renderer's)", false) && Bench(PsoDxc, "dxc, cs_6_0 (the renderer's)", true) &&
                   Bench(PsoFxc, "fxc, cs_5_0", false);
        if (Other)
        {
            wchar_t File[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, Other, -1, File, MAX_PATH);
            ID3DBlob* Code = nullptr;
            ID3DBlob* Err  = nullptr;
            ID3D12PipelineState* Pso = nullptr;
            if (SUCCEEDED(D3DCompileFromFile(File, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "SparseCS", "cs_5_0",
                                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Err)))
                Pso = MakePso(Code->GetBufferPointer(), Code->GetBufferSize());
            else
                printf("  %s did not compile: %s\n", Other, Err ? (const char*)Err->GetBufferPointer() : "(no message)");
            bOk = Pso && Bench(Pso, Other, false) && bOk;
        }
        return bOk ? 0 : 1;
    }

    MakeAsks();
    std::vector<FSlot>    Slots, Want;
    std::vector<uint32_t> Heads, Kept;
    WriteList(Slots, Heads);
    Want = Slots;
    ThinHere(Want, Heads, Kept);
    printf("  %u slots asked at %u x %u pixels\n", ((const uint32_t*)&Slots[0])[0] - 1, W, H);
    CheckBuild("fxc, cs_5_0", PsoFxc, Slots, Heads, Want, Kept);
    CheckBuild("dxc, cs_6_0 (the renderer's)", PsoDxc, Slots, Heads, Want, Kept);

    printf("%d checks, %d failed\n", Checked, Failed);
    return Failed ? 1 : 0;
}

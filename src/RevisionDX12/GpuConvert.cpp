#include "Pch.h"
#include "GpuConvert.h"
#include "Log.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"
#include <string.h>

// The three steps, one texel per thread. The buffers are raw words; a texel is R, G, B, A in
// memory - one little-endian word R | G << 8 | B << 16 | A << 24, as the CPU path writes it.
static const char* GConvertShader =
"ByteAddressBuffer   Src  : register(t0);\n"
"RWByteAddressBuffer Prev : register(u0);\n"
"RWByteAddressBuffer Dst  : register(u1);\n"
"cbuffer C : register(b0)\n"
"{\n"
"    uint W, H;        // the level's size\n"
"    uint IndexOff;    // bytes: its indices in Src, W to a row\n"
"    uint TableOff;    // bytes: the 256 ready-made texels in Src\n"
"    uint Off, Pitch;  // bytes: the level in Prev and Dst, and one row of it\n"
"    uint Mark;        // this dilation pass's mark: pass + 1\n"
"    uint Passes;      // the dilation's pass count\n"
"};\n"
"\n"
"uint Index(uint x, uint y)\n"
"{\n"
"    uint a = IndexOff + y * W + x;\n"
"    return (Src.Load(a & ~3u) >> ((a & 3u) * 8u)) & 0xFFu;\n"
"}\n"
"\n"
"[numthreads(8, 8, 1)]\n"
"void ExpandCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= W || t.y >= H) return;\n"
"    Dst.Store(Off + t.y * Pitch + t.x * 4u, Src.Load(TableOff + Index(t.x, t.y) * 4u));\n"
"}\n"
"\n"
"// One pass, as RdxMath::DilateTransparentEdges does it: a texel transparent before the pass\n"
"// takes the average of its neighbours visible before the pass - the 3x3 around it, wrapping,\n"
"// so that on a texture one or two texels across the same neighbour counts more than once, as\n"
"// there - and this pass's mark as alpha. Prev holds the state before the pass, so nothing this\n"
"// pass writes is read by it.\n"
"[numthreads(8, 8, 1)]\n"
"void DilateCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= W || t.y >= H) return;\n"
"    uint c = Prev.Load(Off + t.y * Pitch + t.x * 4u);\n"
"    uint o = c;\n"
"    if ((c >> 24) == 0u)\n"
"    {\n"
"        uint xs[3] = { (t.x == 0u) ? W - 1u : t.x - 1u, t.x, (t.x + 1u >= W) ? t.x + 1u - W : t.x + 1u };\n"
"        uint ys[3] = { (t.y == 0u) ? H - 1u : t.y - 1u, t.y, (t.y + 1u >= H) ? t.y + 1u - H : t.y + 1u };\n"
"        uint r = 0u, g = 0u, b = 0u, n = 0u;\n"
"        [unroll] for (uint j = 0u; j < 3u; j++)\n"
"        [unroll] for (uint i = 0u; i < 3u; i++)\n"
"        {\n"
"            uint v = Prev.Load(Off + ys[j] * Pitch + xs[i] * 4u);\n"
"            if ((v >> 24) != 0u) { r += v & 0xFFu; g += (v >> 8) & 0xFFu; b += (v >> 16) & 0xFFu; n++; }\n"
"        }\n"
"        // Divided by a count that is never zero, whichever way the branch goes. Both of its sides are worked\n"
"        // out and one is chosen after; a driver's own compiler that takes a division by zero for what cannot\n"
"        // happen drops the choice, and the texel with no visible neighbour - every one in the middle of a\n"
"        // masked texture's hole - comes out opaque white instead of staying as it was.\n"
"        uint d = max(n, 1u);\n"
"        if (n != 0u) o = (r / d) | ((g / d) << 8) | ((b / d) << 16) | (Mark << 24);\n"
"    }\n"
"    Dst.Store(Off + t.y * Pitch + t.x * 4u, o);\n"
"}\n"
"\n"
"// The marks go: every alpha 1..Passes becomes 0.\n"
"[numthreads(8, 8, 1)]\n"
"void CloseCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= W || t.y >= H) return;\n"
"    uint v = Prev.Load(Off + t.y * Pitch + t.x * 4u);\n"
"    uint a = v >> 24;\n"
"    Dst.Store(Off + t.y * Pitch + t.x * 4u, (a >= 1u && a <= Passes) ? (v & 0x00FFFFFFu) : v);\n"
"}\n";

UBOOL FGpuConvert::Init(ID3D12Device* Device)
{
    // Constants, the source (indices and table) and the two buffers the steps read and write -
    // all raw buffers, bound as root descriptors: no descriptor heap is needed.
    D3D12_ROOT_PARAMETER P[4];
    appMemzero(P, sizeof(P));
    P[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P[0].Constants.ShaderRegister = 0;
    P[0].Constants.Num32BitValues = 8;
    P[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[1].Descriptor.ShaderRegister = 0;
    P[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    P[2].Descriptor.ShaderRegister = 0;
    P[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    P[3].Descriptor.ShaderRegister = 1;
    for (INT i = 0; i < 4; i++)
        P[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters = 4;
    Desc.pParameters   = P;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("gpu conversion: the root signature could not be created"));
        return 0;
    }

    static const char*  Entry[3]  = { "ExpandCS", "DilateCS", "CloseCS" };
    static const TCHAR* Named[3]  = { TEXT("ExpandCS"), TEXT("DilateCS"), TEXT("CloseCS") };
    TComRef<ID3D12PipelineState>* Pipe[3] = { &Expand, &Dilate, &Close };
    const SIZE_T Len = strlen(GConvertShader);
    for (INT i = 0; i < 3; i++)
    {
        TComRef<ID3DBlob> Cs;
        if (FAILED(RdxCompile(GConvertShader, Len, "GpuConvert", nullptr, nullptr, Entry[i], "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Cs.Slot(), Error.Slot())))
        {
            RdxLog(TEXT("gpu conversion: shader error (%s): %s"), Named[i],
                   Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
            Shutdown();
            return 0;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC Pso;
        appMemzero(&Pso, sizeof(Pso));
        Pso.pRootSignature     = Root.Get();
        Pso.CS.pShaderBytecode = Cs->GetBufferPointer();
        Pso.CS.BytecodeLength  = Cs->GetBufferSize();
        if (FAILED(Device->CreateComputePipelineState(&Pso, __uuidof(ID3D12PipelineState), Pipe[i]->VoidSlot())))
        {
            RdxLog(TEXT("gpu conversion: the %s pipeline could not be created"), Named[i]);
            Shutdown();
            return 0;
        }
    }
    RDX_LOG("gpu conversion: all three compute pipelines are up");
    return 1;
}

void FGpuConvert::Shutdown()
{
    Close.Release();
    Dilate.Release();
    Expand.Release();
    Root.Release();
}

// Every UAV access recorded so far finishes before any after it: between the steps, one pass's
// writes are the next one's reads, and the next one's writes land where this one read.
static void UavBarrier(ID3D12GraphicsCommandList* Cmd)
{
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    B.UAV.pResource = nullptr;
    Cmd->ResourceBarrier(1, &B);
}

// One level's constants, then its dispatch: 8x8 texels a group.
static void DispatchLevel(ID3D12GraphicsCommandList* Cmd, const FGpuConvert::FLevel& L, UINT TableOff,
                          UINT Mark, INT Passes)
{
    if (L.W == 0 || L.H == 0)
        return;
    const UINT C[8] = { L.W, L.H, L.IndexOff, TableOff, L.Off, L.Pitch, Mark, (UINT)Passes };
    Cmd->SetComputeRoot32BitConstants(0, 8, C, 0);
    Cmd->Dispatch((L.W + 7) / 8, (L.H + 7) / 8, 1);
}

void FGpuConvert::RecordLevels(ID3D12GraphicsCommandList* Cmd, D3D12_GPU_VIRTUAL_ADDRESS SrcVA, UINT TableOff,
                               const FLevel* Levels, INT LevelCount,
                               ID3D12Resource* Out, ID3D12Resource* Temp, UBOOL bMasked, INT Passes)
{
    if (!IsReady() || !Levels || LevelCount <= 0)
        return;

    Cmd->SetComputeRootSignature(Root.Get());
    Cmd->SetComputeRootShaderResourceView(1, SrcVA);

    Cmd->SetPipelineState(Expand.Get());
    Cmd->SetComputeRootUnorderedAccessView(2, Out->GetGPUVirtualAddress());
    Cmd->SetComputeRootUnorderedAccessView(3, Out->GetGPUVirtualAddress());
    for (INT m = 0; m < LevelCount; m++)
        DispatchLevel(Cmd, Levels[m], TableOff, 0, Passes);
    if (!bMasked)
        return;

    ID3D12Resource* From = Out;
    ID3D12Resource* To   = Temp;
    Cmd->SetPipelineState(Dilate.Get());
    for (INT p = 0; p < Passes; p++)
    {
        UavBarrier(Cmd);
        Cmd->SetComputeRootUnorderedAccessView(2, From->GetGPUVirtualAddress());
        Cmd->SetComputeRootUnorderedAccessView(3, To->GetGPUVirtualAddress());
        for (INT m = 0; m < LevelCount; m++)
            DispatchLevel(Cmd, Levels[m], TableOff, (UINT)(p + 1), Passes);
        ID3D12Resource* Was = From;
        From = To;
        To   = Was;
    }

    // Closed from wherever the last pass wrote, into Out - in place when that was Out itself:
    // each texel reads and writes only itself.
    UavBarrier(Cmd);
    Cmd->SetPipelineState(Close.Get());
    Cmd->SetComputeRootUnorderedAccessView(2, From->GetGPUVirtualAddress());
    Cmd->SetComputeRootUnorderedAccessView(3, Out->GetGPUVirtualAddress());
    for (INT m = 0; m < LevelCount; m++)
        DispatchLevel(Cmd, Levels[m], TableOff, 0, Passes);
}

#include "Pch.h"
#include "RaySparse.h"
#include "Log.h"

// RaySparse.hlsl as the build compiles it, with dxc (RevisionDX12.vcxproj): it reads the rays' list by the same text
// as the geometry pass's builds on rays (RenderLimits.h, RayListSlots.hlsli).
#include "RaySparseCS.h"

FRaySparse::FRaySparse()
{
}

FRaySparse::~FRaySparse()
{
    Shutdown();
}

UBOOL FRaySparse::Init(ID3D12Device* Device)
{
    if (Pipeline)
        return 1;
    if (!Device)
        return 0;
    // The heads (u0), a table; the own list and the host's (u1, u2), buffers by their addresses; the constants.
    D3D12_DESCRIPTOR_RANGE Range;
    appMemzero(&Range, sizeof(Range));
    Range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER Params[4];
    appMemzero(Params, sizeof(Params));
    Params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[0].DescriptorTable.NumDescriptorRanges = 1;
    Params[0].DescriptorTable.pDescriptorRanges   = &Range;
    Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_UAV;
    Params[1].Descriptor.ShaderRegister           = 1;
    Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_UAV;
    Params[2].Descriptor.ShaderRegister           = 2;
    Params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[3].Constants.Num32BitValues            = CONSTANTS;
    for (INT p = 0; p < 4; p++)
        Params[p].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters = 4;
    Desc.pParameters   = Params;
    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("rays (RTLow): the thinning pass's root signature failed: %s - every ray asked is traced"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        Shutdown();
        return 0;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature     = Root.Get();
    Pso.CS.pShaderBytecode = GRaySparseCS;
    Pso.CS.BytecodeLength  = sizeof(GRaySparseCS);
    if (FAILED(Device->CreateComputePipelineState(&Pso, __uuidof(ID3D12PipelineState), Pipeline.VoidSlot())))
    {
        RdxLog(TEXT("rays (RTLow): the thinning pass's pipeline could not be made - every ray asked is traced"));
        Shutdown();
        return 0;
    }
    RdxLog(TEXT("rays (RTLow): the thinning pass made - of each three by three pixels the host traces a ray a sort"));
    return 1;
}

void FRaySparse::Shutdown()
{
    Pipeline.Release();
    Root.Release();
}

void FRaySparse::Run(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE HeadsView,
                     INT Width, INT Height, const FLOAT* Eye, ID3D12Resource* Heads, ID3D12Resource* Low,
                     ID3D12Resource* Traced)
{
    if (!Pipeline || !Cmd || !Heap || !Eye || !Heads || !Low || !Traced || Width <= 0 || Height <= 0)
        return;
    // What the pixels wrote - and the host's list's emptying - done before the pass reads them; and the pass's own
    // writes done before the lists move on.
    D3D12_RESOURCE_BARRIER B[3];
    appMemzero(B, sizeof(B));
    ID3D12Resource* const Uavs[3] = { Heads, Low, Traced };
    for (INT k = 0; k < 3; k++)
    {
        B[k].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        B[k].UAV.pResource = Uavs[k];
    }
    Cmd->ResourceBarrier(3, B);
    DWORD Consts[CONSTANTS];
    appMemzero(Consts, sizeof(Consts));
    Consts[0] = (DWORD)Width;
    Consts[1] = (DWORD)Height;
    appMemcpy(&Consts[4], Eye, 3 * sizeof(FLOAT));
    Cmd->SetDescriptorHeaps(1, &Heap);
    Cmd->SetComputeRootSignature(Root.Get());
    Cmd->SetComputeRootDescriptorTable(0, HeadsView);
    Cmd->SetComputeRootUnorderedAccessView(1, Low->GetGPUVirtualAddress());
    Cmd->SetComputeRootUnorderedAccessView(2, Traced->GetGPUVirtualAddress());
    Cmd->SetComputeRoot32BitConstants(3, CONSTANTS, Consts, 0);
    Cmd->SetPipelineState(Pipeline.Get());
    Cmd->Dispatch(((UINT)Width + 7) / 8, ((UINT)Height + 7) / 8, 1);
    Cmd->ResourceBarrier(2, &B[1]);
}

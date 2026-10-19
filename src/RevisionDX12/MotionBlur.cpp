#include "Pch.h"
#include "MotionBlur.h"
#include "Log.h"
#include "ViewGeneration.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"

// A few samples back along what the pixel did. The vectors say where the pixel WAS, in pixels of the scene, so the
// walk is simply that vector shortened - no projection, no depth, nothing to get wrong about the camera.
static const char* GMotionBlurShader =
"Texture2D<float4>   Scene  : register(t0);\n"
"Texture2D<float2>   Motion : register(t1);\n"
"RWTexture2D<float4> Out    : register(u0);\n"
"cbuffer C : register(b0)\n"
"{\n"
"    uint2  Size;  uint Taps; uint Pad0;\n"
"    float  Strength; float MaxPixels; float2 MotionScale;\n"
"};\n"
"\n"
"[numthreads(8, 8, 1)]\n"
"void BlurCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= Size.x || t.y >= Size.y) return;\n"
"    float4 Here = Scene.Load(int3(t.xy, 0));\n"
"    // The motion target may be a different size from the picture (the upscaler's doing), so the place is scaled\n"
"    // rather than assumed.\n"
"    int2   M = int2(float2(t.xy) * MotionScale);\n"
"    float2 Was = Motion.Load(int3(M, 0)) * Strength;\n"
"    float  Len = length(Was);\n"
"    // Under a pixel of movement there is nothing to smear, and the copy back is cheaper than a walk that finds\n"
"    // the same texel eight times.\n"
"    if (Len < 1.0f)\n"
"    {\n"
"        Out[t.xy] = Here;\n"
"        return;\n"
"    }\n"
"    // Held to a length: a pixel that leaps across the screen - a cut, a teleport, the first frame after a load -\n"
"    // would otherwise smear the whole picture into a streak.\n"
"    if (Len > MaxPixels)\n"
"        Was *= MaxPixels / Len;\n"
"\n"
"    float4 Sum = Here;\n"
"    float  Weight = 1.0f;\n"
"    uint   N = max(Taps, 2u);\n"
"    [loop] for (uint k = 1u; k <= N; k++)\n"
"    {\n"
"        // BACKWARDS only: forwards is where the pixel is going, and a shutter cannot see the future. Both ways is\n"
"        // the cheap version and it shows - an edge gains a halo ahead of it.\n"
"        float  s = (float)k / (float)N;\n"
"        int2   At = int2(float2(t.xy) + Was * s);\n"
"        At = clamp(At, int2(0, 0), int2(Size) - 1);\n"
"        // The far end of the smear counts for less, as the light gathered at the end of a shutter's travel does.\n"
"        float  w = 1.0f - s * 0.7f;\n"
"        Sum    += Scene.Load(int3(At, 0)) * w;\n"
"        Weight += w;\n"
"    }\n"
"    Out[t.xy] = Sum / Weight;\n"
"}\n";

UBOOL FMotionBlur::Init(ID3D12Device* Device)
{
    if (!Device)
        return 0;
    if (Blur)
        return 1;

    D3D12_DESCRIPTOR_RANGE Ranges[2];
    appMemzero(Ranges, sizeof(Ranges));
    Ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors     = 2;                 // t0 the scene, t1 the motion vectors
    Ranges[0].BaseShaderRegister = 0;
    Ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Ranges[1].NumDescriptors     = 1;
    Ranges[1].BaseShaderRegister = 0;
    Ranges[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_ROOT_PARAMETER P[2];
    appMemzero(P, sizeof(P));
    P[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P[0].Constants.ShaderRegister = 0;
    P[0].Constants.Num32BitValues = 8;
    P[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    P[1].DescriptorTable.NumDescriptorRanges = 2;
    P[1].DescriptorTable.pDescriptorRanges   = Ranges;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters = 2;
    Desc.pParameters   = P;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("motion blur: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    TComRef<ID3DBlob> Cs;
    if (FAILED(RdxCompile(GMotionBlurShader, strlen(GMotionBlurShader), "MotionBlur", nullptr, nullptr,
                          "BlurCS", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Cs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("motion blur: the shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature     = Root.Get();
    Pso.CS.pShaderBytecode = Cs->GetBufferPointer();
    Pso.CS.BytecodeLength  = Cs->GetBufferSize();
    if (FAILED(Device->CreateComputePipelineState(&Pso, __uuidof(ID3D12PipelineState), Blur.VoidSlot())))
    {
        RdxLog(TEXT("motion blur: the pipeline could not be made"));
        return 0;
    }
    return 1;
}

void FMotionBlur::Shutdown()
{
    ReleaseTargets();
    Blur.Release();
    Root.Release();
}

void FMotionBlur::ReleaseTargets()
{
    Work.Release();
    Heap.Release();
    TargetW = TargetH = 0;
    TargetFormat = DXGI_FORMAT_UNKNOWN;
    WriteFormat  = DXGI_FORMAT_UNKNOWN;
    SceneSeen  = nullptr;
    MotionSeen = nullptr;
}

UBOOL FMotionBlur::EnsureTargets(ID3D12Device* Device, INT W, INT H, DXGI_FORMAT Storage, DXGI_FORMAT Write)
{
    const DXGI_FORMAT Format = Storage;
    if (!Device || W <= 1 || H <= 1 || Format == DXGI_FORMAT_UNKNOWN || Write == DXGI_FORMAT_UNKNOWN)
        return 0;
    if (Work && TargetW == W && TargetH == H && TargetFormat == Format && WriteFormat == Write)
        return 1;
    ReleaseTargets();

    D3D12_HEAP_PROPERTIES Hp;
    appMemzero(&Hp, sizeof(Hp));
    Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Td;
    appMemzero(&Td, sizeof(Td));
    Td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Td.Width            = (UINT64)W;
    Td.Height           = (UINT)H;
    Td.DepthOrArraySize = 1;
    Td.MipLevels        = 1;
    Td.Format           = Format;
    Td.SampleDesc.Count = 1;
    Td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // A branch that cannot be tried is a branch nobody knows the state of (SecondRevision-fail-alloc.txt).
    if (RdxForcedFailure(TEXT("motion blur")) ||
        FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Td,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               __uuidof(ID3D12Resource), Work.VoidSlot())))
    {
        RdxLog(TEXT("motion blur: the %ix%i working picture could not be made - the pass is skipped"), W, H);
        ReleaseTargets();
        return 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC Hd;
    appMemzero(&Hd, sizeof(Hd));
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    Hd.NumDescriptors = 3;
    Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(Device->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), Heap.VoidSlot())))
    {
        RdxLog(TEXT("motion blur: its view heap could not be made - the pass is skipped"));
        ReleaseTargets();
        return 0;
    }
    HeapStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE Uav = Heap->GetCPUDescriptorHandleForHeapStart();
    Uav.ptr += 2 * HeapStep;
    D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
    appMemzero(&Ud, sizeof(Ud));
    Ud.Format        = Write;
    Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Device->CreateUnorderedAccessView(Work.Get(), nullptr, &Ud, Uav);

    TargetW = W;
    TargetH = H;
    TargetFormat = Format;
    WriteFormat = Write;
    RdxLog(TEXT("motion blur: its working picture is %ix%i"), W, H);
    return 1;
}

void FMotionBlur::Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Scene,
                         DXGI_FORMAT SceneFormat, ID3D12Resource* Motion, INT W, INT H, const FBlur& B)
{
    if (!Cmd || !Device || !Blur || !Work || !Heap || !Scene || !Motion)
        return;

    // The views, made when a resource changes and not every frame: writing a descriptor a heap the GPU may still be
    // reading is exactly the kind of thing that works until it does not. A resource made again since the views were
    // written counts as changed whatever its pointer (ViewGeneration.h).
    if (ViewsAt != RdxViewGeneration())
    {
        ViewsAt    = RdxViewGeneration();
        SceneSeen  = nullptr;
        MotionSeen = nullptr;
    }
    if (SceneSeen != Scene || MotionSeen != Motion)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
        appMemzero(&Sd, sizeof(Sd));
        Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Sd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
        Sd.Format = SceneFormat;
        Device->CreateShaderResourceView(Scene, &Sd, At);
        At.ptr += HeapStep;
        Sd.Format = DXGI_FORMAT_R16G16_FLOAT;      // the motion target's own (FBackend::MakeMotionTarget)
        Device->CreateShaderResourceView(Motion, &Sd, At);
        SceneSeen  = Scene;
        MotionSeen = Motion;
    }

    UINT C[8];
    appMemzero(C, sizeof(C));
    FLOAT* F = (FLOAT*)C;
    C[0] = (UINT)W; C[1] = (UINT)H; C[2] = (UINT)((B.Taps > 2) ? B.Taps : 2); C[3] = 0;
    F[4] = B.Strength; F[5] = B.MaxPixels; F[6] = B.ScaleX; F[7] = B.ScaleY;

    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetComputeRootSignature(Root.Get());
    Cmd->SetPipelineState(Blur.Get());
    Cmd->SetComputeRoot32BitConstants(0, 8, C, 0);
    Cmd->SetComputeRootDescriptorTable(1, Heap->GetGPUDescriptorHandleForHeapStart());
    Cmd->Dispatch((UINT)((W + 7) / 8), (UINT)((H + 7) / 8), 1);

    D3D12_RESOURCE_BARRIER U;
    appMemzero(&U, sizeof(U));
    U.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    U.UAV.pResource = Work.Get();
    Cmd->ResourceBarrier(1, &U);
}

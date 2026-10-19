#include "Pch.h"
#include "DepthOfField.h"
#include "Log.h"
#include "ViewGeneration.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"

// See DepthOfField.h for the three passes. The depth holds 1 / distance along the view axis (the engine's projection, as
// FFinalPass's motion vectors read it), 0 where nothing was drawn: the sky, which counts as far away.
static const char* GDepthOfFieldShader =
"Texture2D<float4>   Scene    : register(t0);\n"
"Texture2D<float>    Depth    : register(t1);\n"
"Texture2D<float4>   HalfIn   : register(t2);\n"
"Texture2D<float4>   Blurred  : register(t3);\n"
"Texture2D<float4>   Near     : register(t4);\n"
"RWTexture2D<float4> HalfOut0 : register(u0);\n"
"RWTexture2D<float4> HalfOut1 : register(u1);\n"
"RWTexture2D<float4> HalfOut2 : register(u2);\n"
"RWTexture2D<float4> Out      : register(u3);\n"
"SamplerState        Linear   : register(s0);\n"
"cbuffer C : register(b0)\n"
"{\n"
"    uint2 Size;  uint2 HalfSize;\n"
"    float Focus; float MaxCoc; float Band; float Amount;\n"
"    float RadScale; float Pad0; float Pad1; float Pad2;\n"
"};\n"
"\n"
"static const float Far = 60000.0f;     // the sky's distance; within a half float's range\n"
"\n"
"float DistanceOf(float D) { return (D > 0.0f) ? min(1.0f / D, Far) : Far; }\n"
"\n"
"// The circle, in full-size pixels: |1 - focus / distance| - nothing at the focus, towards the largest far behind it,\n"
"// the largest as well (and no more) close in front. The band keeps a little either side of the focus sharp.\n"
"float CocOf(float W)\n"
"{\n"
"    float c = abs(1.0f - Focus / max(W, 1.0f));\n"
"    c = saturate((c - Band) / (1.0f - Band));\n"
"    return c * MaxCoc * Amount;\n"
"}\n"
"\n"
"[numthreads(8, 8, 1)]\n"
"void DownCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= HalfSize.x || t.y >= HalfSize.y) return;\n"
"    int2   F = int2(t.xy) * 2;\n"
"    int2   M = int2(Size) - 1;\n"
"    float3 C = 0.0f;\n"
"    float  W = Far;\n"
"    [unroll] for (int k = 0; k < 4; k++)\n"
"    {\n"
"        int2 P = min(F + int2(k & 1, k >> 1), M);\n"
"        C += Scene.Load(int3(P, 0)).rgb;\n"
"        // The nearest of the four: a near edge keeps its place, and what is behind it does not grow over it.\n"
"        W = min(W, DistanceOf(Depth.Load(int3(P, 0))));\n"
"    }\n"
"    HalfOut0[t.xy] = float4(C * 0.25f, W);\n"
"}\n"
"\n"
"[numthreads(8, 8, 1)]\n"
"void GatherCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= HalfSize.x || t.y >= HalfSize.y) return;\n"
"    float4 Here    = HalfIn.Load(int3(t.xy, 0));\n"
"    float  HereCoc = CocOf(Here.a) * 0.5f;          // in half-size pixels from here on\n"
"    float  Reach   = MaxCoc * Amount * 0.5f;\n"
"    bool   bNear   = Here.a < Focus;\n"
"    // Two sums. The near layer: whatever is out of focus in FRONT of the focus and whose circle reaches this pixel\n"
"    // - the pixel itself, where it is such - with how many samples that is, and the widest of their circles. And\n"
"    // the rest, which lies under that layer: the pixel's own blur; or for a pixel of the layer, what its circle\n"
"    // takes in of the rest - what it hides is not in the picture, so what is seen beside it stands in.\n"
"    float3 Layer    = bNear ? Here.rgb : float3(0.0f, 0.0f, 0.0f);\n"
"    float  LayerTot = bNear ? 1.0f : 0.0f;\n"
"    float  LayerCoc = bNear ? HereCoc : 0.0f;\n"
"    float3 Sum      = bNear ? float3(0.0f, 0.0f, 0.0f) : Here.rgb;\n"
"    float  Tot      = bNear ? 0.0f : 1.0f;\n"
"    int2   M       = int2(HalfSize) - 1;\n"
"    float  Radius  = RadScale;\n"
"    float  Ang     = 0.0f;\n"
"    // A spiral by the golden angle, its steps closing up as it widens: evenly spread samples over the whole disc.\n"
"    [loop] for (; Radius < Reach; Ang += 2.39996323f)\n"
"    {\n"
"        float2 Dir = float2(cos(Ang), sin(Ang));\n"
"        int2   P   = clamp(int2(float2(t.xy) + Dir * Radius + 0.5f), int2(0, 0), M);\n"
"        float4 S   = HalfIn.Load(int3(P, 0));\n"
"        float  Sc  = CocOf(S.a) * 0.5f;\n"
"        if (S.a < Focus)\n"
"        {\n"
"            // Into the layer, as far as its own circle reaches.\n"
"            float m = smoothstep(Radius - 0.5f, Radius + 0.5f, Sc);\n"
"            Layer    += S.rgb * m;\n"
"            LayerTot += m;\n"
"            LayerCoc  = max(LayerCoc, Sc * step(0.01f, m));\n"
"        }\n"
"        else if (bNear)\n"
"        {\n"
"            // Under a pixel of the layer: what its own circle takes in.\n"
"            float m = smoothstep(Radius - 0.5f, Radius + 0.5f, HereCoc);\n"
"            Sum += S.rgb * m;\n"
"            Tot += m;\n"
"        }\n"
"        else\n"
"        {\n"
"            // Behind this pixel a sample may blur no further than this pixel does itself: what is behind cannot\n"
"            // spill over what is in front of it. In front, it spills as far as its own circle reaches.\n"
"            if (S.a > Here.a)\n"
"                Sc = min(Sc, HereCoc * 2.0f);\n"
"            float m = smoothstep(Radius - 0.5f, Radius + 0.5f, Sc);\n"
"            Sum += lerp(Sum / Tot, S.rgb, m);\n"
"            Tot += 1.0f;\n"
"        }\n"
"        Radius += RadScale / Radius;\n"
"    }\n"
"    // How much of this pixel the layer covers: of the samples within the widest of its circles, the share that\n"
"    // reached - the spiral spreads its samples evenly, a circle's area over twice the spiral's pace of them, and a\n"
"    // little fewer, as the spiral ends at the largest circle's rim. A circle under a pixel does not show\n"
"    // (CombineCS), so neither does what it would lay over its neighbours.\n"
"    float  Within = max(LayerCoc * LayerCoc / (2.0f * RadScale) * 0.96f, 1.0f);\n"
"    float  Cover  = saturate(LayerTot / Within) * smoothstep(0.5f, 2.0f, LayerCoc * 2.0f);\n"
"    HalfOut1[t.xy] = float4((Tot > 1.0e-4f) ? Sum / Tot : Here.rgb, HereCoc);\n"
"    HalfOut2[t.xy] = float4((LayerTot > 1.0e-4f) ? Layer / LayerTot * Cover : float3(0.0f, 0.0f, 0.0f), Cover);\n"
"}\n"
"\n"
"[numthreads(8, 8, 1)]\n"
"void CombineCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= Size.x || t.y >= Size.y) return;\n"
"    float4 Sharp = Scene.Load(int3(t.xy, 0));\n"
"    float  Coc   = CocOf(DistanceOf(Depth.Load(int3(t.xy, 0))));\n"
"    float2 Uv    = (float2(t.xy) + 0.5f) / float2(Size);\n"
"    float3 Soft  = Blurred.SampleLevel(Linear, Uv, 0.0f).rgb;\n"
"    float4 Front = Near.SampleLevel(Linear, Uv, 0.0f);\n"
"    // A circle under a pixel does not show; from two pixels on the blurred picture is the whole of it.\n"
"    float  k = smoothstep(0.5f, 2.0f, Coc);\n"
"    // And over that the near layer, by as much as it covers: a thing blurred in front lies soft over what is\n"
"    // behind it, sharp or not, as a lens lays it - its edge is not cut where its own pixels end.\n"
"    Out[t.xy] = float4(lerp(Sharp.rgb, Soft, k) * (1.0f - Front.a) + Front.rgb, Sharp.a);\n"
"}\n";

UBOOL FDepthOfField::Init(ID3D12Device* Device)
{
    if (!Device)
        return 0;
    if (Combine)
        return 1;

    D3D12_DESCRIPTOR_RANGE Ranges[2];
    appMemzero(Ranges, sizeof(Ranges));
    Ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors     = 5;                 // t0 the scene, t1 the depth, t2 Half0, t3 Half1, t4 Half2
    Ranges[0].BaseShaderRegister = 0;
    Ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Ranges[1].NumDescriptors     = 4;                 // u0 Half0, u1 Half1, u2 Half2, u3 the finished picture
    Ranges[1].BaseShaderRegister = 0;
    Ranges[1].OffsetInDescriptorsFromTableStart = 5;

    D3D12_ROOT_PARAMETER P[2];
    appMemzero(P, sizeof(P));
    P[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P[0].Constants.ShaderRegister = 0;
    P[0].Constants.Num32BitValues = 12;
    P[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    P[1].DescriptorTable.NumDescriptorRanges = 2;
    P[1].DescriptorTable.pDescriptorRanges   = Ranges;

    D3D12_STATIC_SAMPLER_DESC S;
    appMemzero(&S, sizeof(S));
    S.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    S.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    S.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    S.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    S.MaxLOD           = D3D12_FLOAT32_MAX;
    S.ShaderRegister   = 0;
    S.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 2;
    Desc.pParameters       = P;
    Desc.NumStaticSamplers = 1;
    Desc.pStaticSamplers   = &S;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("depth of field: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    const char* const          Entries[3] = { "DownCS", "GatherCS", "CombineCS" };
    TComRef<ID3D12PipelineState>* const Pipes[3] = { &Down, &Gather, &Combine };
    for (INT i = 0; i < 3; i++)
    {
        TComRef<ID3DBlob> Cs;
        if (FAILED(RdxCompile(GDepthOfFieldShader, strlen(GDepthOfFieldShader), "DepthOfField", nullptr, nullptr,
                              Entries[i], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Cs.Slot(), Error.Slot())))
        {
            RdxLog(TEXT("depth of field: the shader %s failed: %s"), appFromAnsi(Entries[i]),
                   Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
            Shutdown();
            return 0;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC Pso;
        appMemzero(&Pso, sizeof(Pso));
        Pso.pRootSignature     = Root.Get();
        Pso.CS.pShaderBytecode = Cs->GetBufferPointer();
        Pso.CS.BytecodeLength  = Cs->GetBufferSize();
        if (FAILED(Device->CreateComputePipelineState(&Pso, __uuidof(ID3D12PipelineState), Pipes[i]->VoidSlot())))
        {
            RdxLog(TEXT("depth of field: the pipeline %s could not be made"), appFromAnsi(Entries[i]));
            Shutdown();
            return 0;
        }
    }
    return 1;
}

void FDepthOfField::Shutdown()
{
    ReleaseTargets();
    Combine.Release();
    Gather.Release();
    Down.Release();
    Root.Release();
}

void FDepthOfField::ReleaseTargets()
{
    Work.Release();
    Half2.Release();
    Half1.Release();
    Half0.Release();
    Heap.Release();
    TargetW = TargetH = 0;
    TargetFormat    = DXGI_FORMAT_UNKNOWN;
    WriteFormat     = DXGI_FORMAT_UNKNOWN;
    SceneSeen       = nullptr;
    DepthSeen       = nullptr;
    DepthFormatSeen = DXGI_FORMAT_UNKNOWN;
}

static UBOOL MakeTexture(ID3D12Device* Device, INT W, INT H, DXGI_FORMAT Format, TComRef<ID3D12Resource>& Out)
{
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
    return SUCCEEDED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     nullptr, __uuidof(ID3D12Resource), Out.VoidSlot()));
}

UBOOL FDepthOfField::EnsureTargets(ID3D12Device* Device, INT W, INT H, DXGI_FORMAT Storage, DXGI_FORMAT Write)
{
    if (!Device || W <= 1 || H <= 1 || Storage == DXGI_FORMAT_UNKNOWN || Write == DXGI_FORMAT_UNKNOWN)
        return 0;
    if (Work && TargetW == W && TargetH == H && TargetFormat == Storage && WriteFormat == Write)
        return 1;
    ReleaseTargets();

    const INT HalfW = (W + 1) / 2;
    const INT HalfH = (H + 1) / 2;
    // A branch that cannot be tried is a branch nobody knows the state of (SecondRevision-fail-alloc.txt).
    if (RdxForcedFailure(TEXT("depth of field")) ||
        !MakeTexture(Device, HalfW, HalfH, DXGI_FORMAT_R16G16B16A16_FLOAT, Half0) ||
        !MakeTexture(Device, HalfW, HalfH, DXGI_FORMAT_R16G16B16A16_FLOAT, Half1) ||
        !MakeTexture(Device, HalfW, HalfH, DXGI_FORMAT_R16G16B16A16_FLOAT, Half2) ||
        !MakeTexture(Device, W, H, Storage, Work))
    {
        RdxLog(TEXT("depth of field: its %ix%i working pictures could not be made - the pass is skipped"), W, H);
        ReleaseTargets();
        return 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC Hd;
    appMemzero(&Hd, sizeof(Hd));
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    Hd.NumDescriptors = 9;
    Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(Device->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), Heap.VoidSlot())))
    {
        RdxLog(TEXT("depth of field: its view heap could not be made - the pass is skipped"));
        ReleaseTargets();
        return 0;
    }
    HeapStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The pass's own pictures, read and written: their views do not change while they live.
    const D3D12_CPU_DESCRIPTOR_HANDLE Start = Heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
    appMemzero(&Sd, sizeof(Sd));
    Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Sd.Texture2D.MipLevels     = 1;
    Sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D12_CPU_DESCRIPTOR_HANDLE At = Start;
    At.ptr += 2 * HeapStep;
    Device->CreateShaderResourceView(Half0.Get(), &Sd, At);
    At.ptr += HeapStep;
    Device->CreateShaderResourceView(Half1.Get(), &Sd, At);
    At.ptr += HeapStep;
    Device->CreateShaderResourceView(Half2.Get(), &Sd, At);
    D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
    appMemzero(&Ud, sizeof(Ud));
    Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Ud.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    At.ptr += HeapStep;
    Device->CreateUnorderedAccessView(Half0.Get(), nullptr, &Ud, At);
    At.ptr += HeapStep;
    Device->CreateUnorderedAccessView(Half1.Get(), nullptr, &Ud, At);
    At.ptr += HeapStep;
    Device->CreateUnorderedAccessView(Half2.Get(), nullptr, &Ud, At);
    Ud.Format = Write;
    At.ptr += HeapStep;
    Device->CreateUnorderedAccessView(Work.Get(), nullptr, &Ud, At);

    TargetW      = W;
    TargetH      = H;
    TargetFormat = Storage;
    WriteFormat  = Write;
    RdxLog(TEXT("depth of field: its working pictures are %ix%i and %ix%i"), W, H, HalfW, HalfH);
    return 1;
}

void FDepthOfField::Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Scene,
                           DXGI_FORMAT SceneFormat, ID3D12Resource* Depth, DXGI_FORMAT DepthFormat, INT W, INT H,
                           const FParams& Pr)
{
    if (!Cmd || !Device || !Combine || !Work || !Heap || !Scene || !Depth)
        return;

    // The views of what is read, made when a resource changes and not every frame: writing a descriptor in a heap the
    // GPU may still be reading is exactly the kind of thing that works until it does not. A resource made again since
    // the views were written counts as changed whatever its pointer (ViewGeneration.h).
    if (ViewsAt != RdxViewGeneration())
    {
        ViewsAt   = RdxViewGeneration();
        SceneSeen = nullptr;
        DepthSeen = nullptr;
    }
    if (SceneSeen != Scene || DepthSeen != Depth || DepthFormatSeen != DepthFormat)
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
        Sd.Format = DepthFormat;
        Device->CreateShaderResourceView(Depth, &Sd, At);
        SceneSeen       = Scene;
        DepthSeen       = Depth;
        DepthFormatSeen = DepthFormat;
    }

    const INT HalfW = (W + 1) / 2;
    const INT HalfH = (H + 1) / 2;
    UINT C[12];
    appMemzero(C, sizeof(C));
    FLOAT* F = (FLOAT*)C;
    C[0] = (UINT)W;     C[1] = (UINT)H;
    C[2] = (UINT)HalfW; C[3] = (UINT)HalfH;
    F[4] = Pr.Focus;    F[5] = Pr.MaxCoc;  F[6] = Pr.Band;  F[7] = Pr.Amount;
    // How fast the spiral opens, in half-size pixels: about 2 pi times this many pixels to a sample. Half a pixel gives
    // some seventy samples at 4K's largest circle.
    F[8] = 0.5f;

    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetComputeRootSignature(Root.Get());
    Cmd->SetComputeRoot32BitConstants(0, 12, C, 0);
    Cmd->SetComputeRootDescriptorTable(1, Heap->GetGPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_BARRIER B[3];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 3; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }

    Cmd->SetPipelineState(Down.Get());
    Cmd->Dispatch((UINT)((HalfW + 7) / 8), (UINT)((HalfH + 7) / 8), 1);
    B[0].Transition.pResource   = Half0.Get();
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, B);

    Cmd->SetPipelineState(Gather.Get());
    Cmd->Dispatch((UINT)((HalfW + 7) / 8), (UINT)((HalfH + 7) / 8), 1);
    B[0].Transition.pResource = Half1.Get();
    B[1] = B[0];
    B[1].Transition.pResource = Half2.Get();
    Cmd->ResourceBarrier(2, B);

    Cmd->SetPipelineState(Combine.Get());
    Cmd->Dispatch((UINT)((W + 7) / 8), (UINT)((H + 7) / 8), 1);

    // The half-size pictures back to being written, for the next frame; the finished one is the caller's to copy.
    B[0].Transition.pResource   = Half0.Get();
    B[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    B[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    B[1] = B[0];
    B[1].Transition.pResource = Half1.Get();
    B[2] = B[0];
    B[2].Transition.pResource = Half2.Get();
    Cmd->ResourceBarrier(3, B);
    D3D12_RESOURCE_BARRIER U;
    appMemzero(&U, sizeof(U));
    U.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    U.UAV.pResource = Work.Get();
    Cmd->ResourceBarrier(1, &U);
}

#include "Pch.h"
#include "MirrorProbe.h"
#include "Log.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"
// MirrorProbe.hlsl's text, put into the DLL by the build (scripts/embed-text.ps1): compiled at load time like the other
// passes that need no shader model 6.
#include "MirrorProbeText.h"

FMirrorProbe::FMirrorProbe()
{
    Device      = nullptr;
    bReady      = 0;
    LevelsState = D3D12_RESOURCE_STATE_COMMON;
    bEmpty      = 1;
    bMoved      = 0;
    Moved       = RdxMath::FVec3{ 0.0f, 0.0f, 0.0f };
    bCounted    = 0;
    Told        = 0;
    Frames      = 0;
}

FMirrorProbe::~FMirrorProbe()
{
    Shutdown();
}

ID3D12Resource* FMirrorProbe::MakeBuffer(UINT64 Bytes, const TCHAR* What, D3D12_HEAP_TYPE Type)
{
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = Type;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = Bytes;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Desc.Flags            = (Type == D3D12_HEAP_TYPE_DEFAULT) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                              : D3D12_RESOURCE_FLAG_NONE;
    const D3D12_RESOURCE_STATES State = (Type == D3D12_HEAP_TYPE_READBACK) ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                           : D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* Res = nullptr;
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, State, nullptr,
                                               __uuidof(ID3D12Resource), (void**)&Res)))
    {
        RdxLog(TEXT("mirror probe: no memory for its %s (%.1f MB) - the panes have no mirror image without rays"), What,
               (DOUBLE)Bytes / (1024.0 * 1024.0));
        return nullptr;
    }
    return Res;
}

UBOOL FMirrorProbe::Init(ID3D12Device* InDevice)
{
    if (bReady)
        return 1;
    if (!InDevice)
        return 0;
    Device = InDevice;
    const INT    CellCount = 6 * RdxMath::ProbeSide * RdxMath::ProbeSide;
    const UINT64 CellBytes = (UINT64)CellCount * CELL_BYTES, KeyBytes = (UINT64)CellCount * 4;
    const UINT64 LevelBytes = (UINT64)RdxMath::ProbeEntries() * ENTRY_BYTES;
    *Cells.Slot()     = MakeBuffer(CellBytes, TEXT("cells"));
    *Keys.Slot()      = MakeBuffer(KeyBytes, TEXT("keys"));
    *Levels.Slot()    = MakeBuffer(LevelBytes, TEXT("pyramid"));
    *Spare.Slot()     = MakeBuffer(CellBytes, TEXT("spare cells"));
    *Stats.Slot()     = MakeBuffer(STATS_BYTES, TEXT("counts"));
    *StatsBack.Slot() = MakeBuffer(STATS_BYTES, TEXT("counts' readback"), D3D12_HEAP_TYPE_READBACK);
    if (!Cells || !Keys || !Levels || !Spare || !Stats || !StatsBack)
    {
        Shutdown();
        return 0;
    }
    // The pyramid starts as the passes find it: the first update empties the cells, and goes over every entry of the
    // pyramid before the glass reads it.
    LevelsState = D3D12_RESOURCE_STATE_COMMON;

    // One table: the copies' picture, depth and stencil and the added target (t0..t3), then the probe's own five
    // (u0..u4); and the passes' constants.
    D3D12_DESCRIPTOR_RANGE Ranges[2];
    appMemzero(Ranges, sizeof(Ranges));
    Ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors                    = VIEW_CELLS;
    Ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Ranges[1].NumDescriptors                    = VIEWS - VIEW_CELLS;
    Ranges[1].OffsetInDescriptorsFromTableStart = VIEW_CELLS;
    D3D12_ROOT_PARAMETER Params[2];
    appMemzero(Params, sizeof(Params));
    Params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[0].DescriptorTable.NumDescriptorRanges = 2;
    Params[0].DescriptorTable.pDescriptorRanges   = Ranges;
    Params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[1].Constants.Num32BitValues            = CONSTANTS;
    Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters = 2;
    Desc.pParameters   = Params;
    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("mirror probe: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        Shutdown();
        return 0;
    }
    static const char* const Entries[PASSES] = { "CarveCS", "StatsClearCS", "KeyCS", "GatherCS", "ClearSpareCS",
                                                 "TransferKeyCS", "TransferGatherCS", "TransferBackCS", "BaseCS", "UpCS",
                                                 "DownCS" };
    for (INT p = 0; p < PASSES; p++)
    {
        TComRef<ID3DBlob> Cs;
        if (FAILED(RdxCompile(GMirrorProbeText, sizeof(GMirrorProbeText) - 1, "MirrorProbe", nullptr, nullptr, Entries[p],
                              "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Cs.Slot(), Error.Slot())))
        {
            RdxLog(TEXT("mirror probe: %s failed: %s"), appFromAnsi(Entries[p]),
                   Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
            Shutdown();
            return 0;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC Pso;
        appMemzero(&Pso, sizeof(Pso));
        Pso.pRootSignature     = Root.Get();
        Pso.CS.pShaderBytecode = Cs->GetBufferPointer();
        Pso.CS.BytecodeLength  = Cs->GetBufferSize();
        if (FAILED(Device->CreateComputePipelineState(&Pso, __uuidof(ID3D12PipelineState), Pipelines[p].VoidSlot())))
        {
            RdxLog(TEXT("mirror probe: the %s pipeline could not be made"), appFromAnsi(Entries[p]));
            Shutdown();
            return 0;
        }
    }
    bReady   = 1;
    bEmpty   = 1;
    bMoved   = 0;
    bCounted = 0;
    RdxLog(TEXT("mirror probe: made - %i x %i texels a face, %i levels, %.1f MB on the card"), (INT)RdxMath::ProbeSide,
           (INT)RdxMath::ProbeSide, (INT)RdxMath::ProbeLevels,
           (DOUBLE)(2 * CellBytes + KeyBytes + LevelBytes) / (1024.0 * 1024.0));
    return 1;
}

void FMirrorProbe::Shutdown()
{
    bReady = 0;
    for (INT p = 0; p < PASSES; p++)
        Pipelines[p].Release();
    Root.Release();
    Cells.Release();
    Keys.Release();
    Levels.Release();
    Spare.Release();
    Stats.Release();
    StatsBack.Release();
    LevelsState = D3D12_RESOURCE_STATE_COMMON;
    bEmpty      = 1;
    bMoved      = 0;
    bCounted    = 0;
    Frames      = 0;
}

void FMirrorProbe::NotePane(const FLOAT* Middle, const FLOAT* Normal, FLOAT Area)
{
    if (Middle && Normal)
        Place.Note(RdxMath::FVec3{ Middle[0], Middle[1], Middle[2] }, RdxMath::FVec3{ Normal[0], Normal[1], Normal[2] },
                   Area);
}

void FMirrorProbe::BeginFrame(const FLOAT* Eye)
{
    if (!Eye)
        return;
    const UBOOL          bWasPlaced = Place.bPlaced ? 1 : 0;
    const RdxMath::FVec3 Was        = Place.Centre;
    if (!Place.Choose(RdxMath::FVec3{ Eye[0], Eye[1], Eye[2] }))
        return;
    // A probe that holds nothing yet (its first place, or a move before its first update) is emptied where it stands
    // now; one that holds something takes it over from where it stood - unless a move is already waiting, whose old
    // place is still the one its cells are round.
    if (!bWasPlaced || bEmpty)
        bEmpty = 1;
    else if (!bMoved)
    {
        bMoved = 1;
        Moved  = Was;
    }
    static INT Placed = 0;
    if (Placed++ < 20 || (Placed % 100) == 0)
        RdxLog(TEXT("mirror probe: %s (%.0f, %.0f, %.0f) - %s"),
               Place.bAtPane ? TEXT("stands before a pane at") : TEXT("follows the camera, at"), Place.Centre.X,
               Place.Centre.Y, Place.Centre.Z,
               bMoved ? TEXT("what it held goes with it") : TEXT("empty, it gathers from there"));
}

void FMirrorProbe::Forget()
{
    Place.Forget();
    bEmpty = 1;
    bMoved = 0;
}

UBOOL FMirrorProbe::GetPlace(FLOAT* Centre, FLOAT& Reach) const
{
    Reach = 0.0f;
    // Not before its pyramid was made, nor in a frame it moved until its passes have emptied it or taken what it held
    // over: the glass would read it round another place.
    if (!bReady || !Place.bPlaced || bEmpty || bMoved || Frames == 0 || !Centre)
        return 0;
    Centre[0] = Place.Centre.X;
    Centre[1] = Place.Centre.Y;
    Centre[2] = Place.Centre.Z;
    Reach     = RdxMath::ProbeReach;
    return 1;
}

UBOOL FMirrorProbe::GetPane(FLOAT* Normal, FLOAT* Middle) const
{
    FLOAT Centre[3], Reach;
    if (!Normal || !Middle || !Place.bAtPane || !GetPlace(Centre, Reach))
        return 0;
    Normal[0] = Place.PaneNormal.X;
    Normal[1] = Place.PaneNormal.Y;
    Normal[2] = Place.PaneNormal.Z;
    Middle[0] = Place.PaneMiddle.X;
    Middle[1] = Place.PaneMiddle.Y;
    Middle[2] = Place.PaneMiddle.Z;
    return 1;
}

void FMirrorProbe::WriteOwnViews(D3D12_CPU_DESCRIPTOR_HANDLE First, UINT Step) const
{
    if (!bReady)
        return;
    const UINT64 CellBytes = (UINT64)6 * RdxMath::ProbeSide * RdxMath::ProbeSide * CELL_BYTES;
    ID3D12Resource* const Own[5]   = { Cells.Get(), Keys.Get(), Levels.Get(), Spare.Get(), Stats.Get() };
    const UINT64          Bytes[5] = { CellBytes, (UINT64)6 * RdxMath::ProbeSide * RdxMath::ProbeSide * 4,
                                       (UINT64)RdxMath::ProbeEntries() * ENTRY_BYTES, CellBytes, STATS_BYTES };
    for (INT k = 0; k < 5; k++)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav;
        appMemzero(&Uav, sizeof(Uav));
        Uav.Format             = DXGI_FORMAT_R32_TYPELESS;
        Uav.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Buffer.NumElements = (UINT)(Bytes[k] / 4);
        Uav.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE At = First;
        At.ptr += (SIZE_T)(VIEW_CELLS + k) * Step;
        Device->CreateUnorderedAccessView(Own[k], nullptr, &Uav, At);
    }
}

void FMirrorProbe::WriteLevelsView(ID3D12Device* InDevice, D3D12_CPU_DESCRIPTOR_HANDLE At) const
{
    if (!InDevice)
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
    appMemzero(&Srv, sizeof(Srv));
    Srv.Format                  = DXGI_FORMAT_R32_TYPELESS;
    Srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Srv.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
    Srv.Buffer.NumElements      = bReady ? (UINT)((UINT64)RdxMath::ProbeEntries() * ENTRY_BYTES / 4) : 1;
    InDevice->CreateShaderResourceView(bReady ? Levels.Get() : nullptr, &Srv, At);
}

// The last counting frame's counts, read back - it was COUNT_EVERY updates ago, and the card is long done with it.
void FMirrorProbe::TellCounts()
{
    if (!bCounted)
        return;
    bCounted = 0;
    UINT*             C     = nullptr;
    const D3D12_RANGE Range = { 0, STATS_BYTES };
    if (FAILED(StatsBack->Map(0, &Range, (void**)&C)) || !C)
        return;
    const UINT Carved = C[0], Taken = C[1], Known = C[2], Over = C[3];
    const D3D12_RANGE None = { 0, 0 };
    StatsBack->Unmap(0, &None);
    if (Told < 20 || (Told % 50) == 0)
        RdxLog(TEXT("mirror probe: %u of its %i cells known (%.1f%%); that frame %u carved out, %u taken from the ")
               TEXT("picture%s"), Known, 6 * RdxMath::ProbeSide * RdxMath::ProbeSide,
               100.0 * (DOUBLE)Known / (DOUBLE)(6 * RdxMath::ProbeSide * RdxMath::ProbeSide), Carved, Taken,
               Over ? *FString::Printf(TEXT(", %u taken over as it moved"), Over) : TEXT(""));
    Told++;
}

void FMirrorProbe::Update(ID3D12GraphicsCommandList* Cmd, D3D12_GPU_DESCRIPTOR_HANDLE Table, const FLOAT* Eye,
                          const FLOAT* Axes, FLOAT TanX, FLOAT TanY, const FLOAT* Rect, UINT Width, UINT Height)
{
    if (!bReady || !Cmd || !Eye || !Axes || !Rect || !Place.bPlaced || Width < 2 || Height < 2)
        return;
    D3D12_RESOURCE_BARRIER B[5];
    appMemzero(B, sizeof(B));
    INT Count = 0;
    if (LevelsState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        B[Count].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[Count].Transition.pResource   = Levels.Get();
        B[Count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B[Count].Transition.StateBefore = LevelsState;
        B[Count].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Count++;
    }
    // The other buffers are made in the common state and stay unordered access from their first update on.
    if (Frames == 0)
    {
        ID3D12Resource* const Own[4] = { Cells.Get(), Keys.Get(), Spare.Get(), Stats.Get() };
        for (INT k = 0; k < 4; k++)
        {
            B[Count].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B[Count].Transition.pResource   = Own[k];
            B[Count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B[Count].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            B[Count].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            Count++;
        }
    }
    if (Count)
        Cmd->ResourceBarrier(Count, B);

    Frames++;
    const UBOOL bCount = (Frames % COUNT_EVERY) == 1 ? 1 : 0;
    if (bCount)
        TellCounts();

    // MirrorProbe.hlsl's ProbeConsts.
    DWORD  C[CONSTANTS];
    FLOAT* const F = (FLOAT*)C;
    appMemzero(C, sizeof(C));
    for (INT a = 0; a < 3; a++)
        for (INT k = 0; k < 3; k++)
            F[a * 4 + k] = Axes[a * 3 + k];
    F[3]  = TanX;
    F[7]  = TanY;
    F[11] = RdxMath::ProbeReach;
    F[12] = Place.Centre.X - Eye[0];
    F[13] = Place.Centre.Y - Eye[1];
    F[14] = Place.Centre.Z - Eye[2];
    for (INT k = 0; k < 4; k++)
        F[16 + k] = Rect[k];
    F[20] = RdxMath::ProbeCarveSlack;
    F[21] = RdxMath::ProbeCarveShare;
    F[22] = RdxMath::ProbeSameShare;
    // Which pixel of each two by two gathers: all four in turn, the diagonal ones apart.
    static const DWORD Picks[4] = { 0u, 3u, 1u, 2u };
    C[24] = Frames;
    C[25] = Picks[Frames & 3u];
    C[27] = bEmpty ? 1u : 0u;
    F[28] = Moved.X - Place.Centre.X;
    F[29] = Moved.Y - Place.Centre.Y;
    F[30] = Moved.Z - Place.Centre.Z;
    C[31] = bCount;

    D3D12_RESOURCE_BARRIER Uav;
    appMemzero(&Uav, sizeof(Uav));
    Uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    Cmd->SetComputeRootSignature(Root.Get());
    Cmd->SetComputeRootDescriptorTable(0, Table);
    auto Pass = [&](INT Which, UINT Level, UINT X, UINT Y, UINT Z)
    {
        C[26] = Level;
        Cmd->SetPipelineState(Pipelines[Which].Get());
        Cmd->SetComputeRoot32BitConstants(1, CONSTANTS, C, 0);
        Cmd->Dispatch(X, Y, Z);
        Cmd->ResourceBarrier(1, &Uav);
    };
    const UINT Side = (UINT)RdxMath::ProbeSide;
    auto Groups = [](UINT N) { return (N + 7u) / 8u; };
    if (bCount)
        Pass(PASS_STATS_CLEAR, 0, 1, 1, 1);
    // A move: what it holds taken over round where it stands now, before the frame's own passes.
    if (bMoved && !bEmpty)
    {
        Pass(PASS_CLEAR_SPARE, 0, Groups(Side), Groups(Side), 6);
        Pass(PASS_TRANSFER_KEY, 0, Groups(Side), Groups(Side), 6);
        Pass(PASS_TRANSFER_GATHER, 0, Groups(Side), Groups(Side), 6);
        Pass(PASS_TRANSFER_BACK, 0, Groups(Side), Groups(Side), 6);
    }
    Pass(PASS_CARVE, 0, Groups(Side), Groups(Side), 6);
    Pass(PASS_KEY, 0, Groups((Width + 1u) / 2u), Groups((Height + 1u) / 2u), 1);
    Pass(PASS_GATHER, 0, Groups((Width + 1u) / 2u), Groups((Height + 1u) / 2u), 1);
    Pass(PASS_BASE, 0, Groups(Side), Groups(Side), 6);
    for (UINT k = 1; k < (UINT)RdxMath::ProbeLevels; k++)
        Pass(PASS_UP, k, Groups(Side >> k), Groups(Side >> k), 6);
    for (INT k = RdxMath::ProbeFillUp - 1; k >= 0; k--)
        Pass(PASS_DOWN, (UINT)k, Groups(Side >> k), Groups(Side >> k), 6);
    bEmpty = 0;
    bMoved = 0;

    INT N = 0;
    B[N].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B[N].Transition.pResource   = Levels.Get();
    B[N].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B[N].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    B[N].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    N++;
    if (bCount)
    {
        B[N]                        = B[0];
        B[N].Transition.pResource   = Stats.Get();
        B[N].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        N++;
    }
    Cmd->ResourceBarrier(N, B);
    LevelsState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (bCount)
    {
        Cmd->CopyBufferRegion(StatsBack.Get(), 0, Stats.Get(), 0, STATS_BYTES);
        B[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        B[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Cmd->ResourceBarrier(1, &B[1]);
        bCounted = 1;
    }
}

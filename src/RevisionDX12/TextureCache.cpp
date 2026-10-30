#include "Pch.h"
#include "Backend.h"
#include "TextureCache.h"
#include "Log.h"
#include "RenderMath.h"

#include <stddef.h>
#include <string.h>

// A thread-pool callback of FTextureCache::ParallelFor - and the calling thread's share too:
// takes the next index until none is left.
static VOID CALLBACK ParallelWorker(PTP_CALLBACK_INSTANCE, PVOID Param, PTP_WORK)
{
    FParallelJob* J = (FParallelJob*)Param;
    for (;;)
    {
        const LONG i = ::InterlockedIncrement(&J->Next) - 1;
        if (i >= J->Count)
            break;
        J->Body(J->Ctx, (INT)i);
    }
}

FTextureCache::FTextureCache()
{
    Device       = nullptr;
    Heap         = nullptr;
    appMemzero(Hash, sizeof(Hash));
    HeapStride   = 0;
    Count        = 0;
    CurrentFrame = 0;
    WhiteIndex   = -1;
    FrameSerial  = 0;
    RefreshCount = 0;
    appMemzero(Ring, sizeof(Ring));
    appMemzero(RingCpu, sizeof(RingCpu));
    appMemzero(RingUsed, sizeof(RingUsed));
    appMemzero(SpillChunk, sizeof(SpillChunk));
    appMemzero(SpillCount, sizeof(SpillCount));
    appMemzero(SpillCpu, sizeof(SpillCpu));
    appMemzero(SpillUsed, sizeof(SpillUsed));
    appMemzero(SpillSize, sizeof(SpillSize));
    Scratch      = nullptr;
    ScratchSize  = 0;
    Pool           = nullptr;
    appMemzero(&Job, sizeof(Job));
    PoolHelpers    = 0;
    DilateWork     = nullptr;
    DilateWorkHave = 0;
    bCheckParallel = 0;
    bGpuConvert    = 0;
    LastCacheId    = LastPaletteId = 0;
    LastMasked     = LastSrgb = 0;
    LastEntry      = -1;
    ParallelChecks = 0;
    ParallelBad    = 0;
    appMemzero(GpuChecks, sizeof(GpuChecks));
    appMemzero(GpuCheckCount, sizeof(GpuCheckCount));
    appMemzero(GpuCheckBytes, sizeof(GpuCheckBytes));
    GpuChecksDone = GpuChecksBad = GpuSmallChecks = 0;
    appMemzero(GpuOut, sizeof(GpuOut));
    appMemzero(GpuTemp, sizeof(GpuTemp));
    appMemzero(GpuScratchSize, sizeof(GpuScratchSize));
    appMemzero(GpuOld, sizeof(GpuOld));
    appMemzero(GpuOldCount, sizeof(GpuOldCount));
    appMemzero(Entries, sizeof(Entries));
    appMemzero(Staging, sizeof(Staging));
    appMemzero(StagingCount, sizeof(StagingCount));
    FrameUploads = FrameLightUploads = 0;
    FrameUploadBytes = FrameUploadSec = FrameCreateSec = 0.0;
    FrameOwnBufs = 0;
    FrameOwnBufSec = FrameConvertSec = FrameDilateSec = FrameCopySec = 0.0;
    appMemzero(TexHeaps, sizeof(TexHeaps));
    TexHeapCount   = 0;
    TexHeapCurrent = -1;
    TexHeapUsed    = 0;
}

FTextureCache::~FTextureCache()
{
    Shutdown();
}

UBOOL FTextureCache::Init(ID3D12Device* InDevice)
{
    Device = InDevice;

    D3D12_DESCRIPTOR_HEAP_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    Desc.NumDescriptors = MAX_TEXTURES;
    Desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(Device->CreateDescriptorHeap(&Desc, __uuidof(ID3D12DescriptorHeap), (void**)&Heap)))
    {
        RdxLog(TEXT("texture descriptor heap creation failed"));
        return 0;
    }

    HeapStride = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The upload rings. Not having one is not fatal - every upload then gets a buffer of
    // its own, which is slow but correct - so a failure is reported, not returned.
    D3D12_HEAP_PROPERTIES UploadHeap;
    appMemzero(&UploadHeap, sizeof(UploadHeap));
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC BufDesc;
    appMemzero(&BufDesc, sizeof(BufDesc));
    BufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufDesc.Width            = RING_SIZE;
    BufDesc.Height           = 1;
    BufDesc.DepthOrArraySize = 1;
    BufDesc.MipLevels        = 1;
    BufDesc.SampleDesc.Count = 1;
    BufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        D3D12_RANGE NoRead;
        NoRead.Begin = 0;
        NoRead.End   = 0;
        if (FAILED(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   __uuidof(ID3D12Resource), (void**)&Ring[f])) ||
            FAILED(Ring[f]->Map(0, &NoRead, (void**)&RingCpu[f])))
        {
            RdxLog(TEXT("upload ring %i could not be created - every upload gets a buffer of its own"), f);
            if (Ring[f]) { Ring[f]->Release(); Ring[f] = nullptr; }
            RingCpu[f] = nullptr;
        }
    }

    // The thread pool for converting big textures side by side (ConvertLevelsParallel). Not
    // having it is not fatal: the conversion then runs on the engine's thread, as before.
    if (!Pool)
    {
        SYSTEM_INFO Si;
        ::GetSystemInfo(&Si);
        // The engine's own thread takes a share too, and one processor is left alone.
        PoolHelpers = (INT)Si.dwNumberOfProcessors - 2;
        if (PoolHelpers > 6)
            PoolHelpers = 6;
        if (PoolHelpers > 0)
            Pool = ::CreateThreadpoolWork(ParallelWorker, &Job, nullptr);
        RdxLog(TEXT("texture conversion: %i worker threads"), Pool ? PoolHelpers : 0);
    }

    // Not fatal either: without it the conversion simply stays on the CPU.
    Gpu.Init(Device);
    return 1;
}

void FTextureCache::ReleaseAll()
{
    // A level change. The caller has already waited for the GPU, so the resources are
    // free to go. Without this a long session across several levels keeps growing until
    // the cache fills - and a full cache renders surfaces with no lighting.
    for (INT i = 0; i < Count; i++)
    {
        if (Entries[i].Resource)
        {
            Entries[i].Resource->Release();
            Entries[i].Resource = nullptr;
        }
        if (Entries[i].Kept)
        {
            appFree(Entries[i].Kept);
            Entries[i].Kept      = nullptr;
            Entries[i].KeptBytes = 0;
        }
    }
    Count      = 0;
    WhiteIndex = -1;
    LastEntry  = -1;
    HashClear();
    // The textures placed in the heaps are gone; the heaps stay for the next level, and
    // only the allocator rewinds.
    TexHeapCurrent = -1;
    TexHeapUsed    = 0;
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
        ReleaseFrameStaging(f);
}

void FTextureCache::Shutdown()
{
    // The GPU is idle by now (the backend waited): pending checks finish, buffers go.
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        GpuCompareFinish(f);
        if (GpuOut[f])  { GpuOut[f]->Release();  GpuOut[f]  = nullptr; }
        if (GpuTemp[f]) { GpuTemp[f]->Release(); GpuTemp[f] = nullptr; }
        GpuScratchSize[f] = 0;
    }
    Gpu.Shutdown();
    if (Pool)
    {
        ::WaitForThreadpoolWorkCallbacks(Pool, TRUE);
        ::CloseThreadpoolWork(Pool);
        Pool = nullptr;
    }
    if (DilateWork)
    {
        appFree(DilateWork);
        DilateWork     = nullptr;
        DilateWorkHave = 0;
    }

    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        if (Ring[f])
        {
            Ring[f]->Unmap(0, nullptr);
            Ring[f]->Release();
            Ring[f] = nullptr;
        }
        RingCpu[f]  = nullptr;
        RingUsed[f] = 0;
    }

    if (Scratch)
    {
        appFree(Scratch);
        Scratch = nullptr;
        ScratchSize = 0;
    }

    for (INT i = 0; i < Count; i++)
    {
        if (Entries[i].Resource)
        {
            Entries[i].Resource->Release();
            Entries[i].Resource = nullptr;
        }
        if (Entries[i].Kept)
        {
            appFree(Entries[i].Kept);
            Entries[i].Kept      = nullptr;
            Entries[i].KeptBytes = 0;
        }
    }
    Count     = 0;
    LastEntry = -1;

    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
        ReleaseFrameStaging(f);

    if (Heap)
    {
        Heap->Release();
        Heap = nullptr;
    }

    // After every texture placed in them is released (above).
    for (INT i = 0; i < TexHeapCount; i++)
        if (TexHeaps[i])
        {
            TexHeaps[i]->Release();
            TexHeaps[i] = nullptr;
        }
    TexHeapCount   = 0;
    TexHeapCurrent = -1;
    TexHeapUsed    = 0;
}

D3D12_GPU_DESCRIPTOR_HANDLE FTextureCache::GetGpuHandle(INT Index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE H = Heap->GetGPUDescriptorHandleForHeapStart();
    H.ptr += (UINT64)Index * HeapStride;
    return H;
}

D3D12_CPU_DESCRIPTOR_HANDLE FTextureCache::GetCpuHandle(INT Index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE H = Heap->GetCPUDescriptorHandleForHeapStart();
    H.ptr += (SIZE_T)Index * HeapStride;
    return H;
}

void FTextureCache::ReleaseFrameStaging(INT FrameIndex)
{
    // The slot's fence has passed: its GPU conversions are done, and can be checked.
    GpuCompareFinish(FrameIndex);
    for (INT i = 0; i < StagingCount[FrameIndex]; i++)
        if (Staging[FrameIndex][i])
        {
            Staging[FrameIndex][i]->Release();
            Staging[FrameIndex][i] = nullptr;
        }
    StagingCount[FrameIndex] = 0;
    // The slot's fence has passed: its ring memory is free again, and its spill chunks can go.
    RingUsed[FrameIndex] = 0;
    for (INT i = 0; i < SpillCount[FrameIndex]; i++)
        if (SpillChunk[FrameIndex][i])
        {
            SpillChunk[FrameIndex][i]->Unmap(0, nullptr);
            SpillChunk[FrameIndex][i]->Release();
            SpillChunk[FrameIndex][i] = nullptr;
        }
    SpillCount[FrameIndex] = 0;
    SpillCpu[FrameIndex]   = nullptr;
    SpillUsed[FrameIndex]  = 0;
    SpillSize[FrameIndex]  = 0;
}

UBOOL FTextureCache::RingAlloc(UINT64 Size, ID3D12Resource*& Buffer, UINT64& Offset, BYTE*& Cpu)
{
    const INT F = CurrentFrame;
    if (!Ring[F] || !RingCpu[F])
        return 0;
    // A copy's source offset must sit on the 512-byte placement boundary.
    const UINT64 Align = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    const UINT64 Start = (RingUsed[F] + Align - 1) & ~(Align - 1);
    if (Start + Size > (UINT64)RING_SIZE)
        return 0;
    RingUsed[F] = Start + Size;
    Buffer = Ring[F];
    Offset = Start;
    Cpu    = RingCpu[F] + Start;
    return 1;
}

// Past the ring: the slot's current spill chunk if it fits, else a new chunk - see the
// declaration. Chunks stay mapped until ReleaseFrameStaging, after the slot's fence.
UBOOL FTextureCache::SpillAlloc(UINT64 Size, ID3D12Resource*& Buffer, UINT64& Offset, BYTE*& Cpu)
{
    const INT    F     = CurrentFrame;
    const UINT64 Align = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    const INT    N     = SpillCount[F];
    if (N > 0)
    {
        const UINT64 Start = (SpillUsed[F] + Align - 1) & ~(Align - 1);
        if (Start + Size <= SpillSize[F])
        {
            SpillUsed[F] = Start + Size;
            Buffer = SpillChunk[F][N - 1];
            Offset = Start;
            Cpu    = SpillCpu[F] + Start;
            return 1;
        }
    }
    if (N >= MAX_SPILL)
        return 0;

    const UINT64 ChunkSize = (Size > (UINT64)SPILL_CHUNK) ? Size : (UINT64)SPILL_CHUNK;
    D3D12_HEAP_PROPERTIES UploadHeap;
    appMemzero(&UploadHeap, sizeof(UploadHeap));
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC BufDesc;
    appMemzero(&BufDesc, sizeof(BufDesc));
    BufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufDesc.Width            = ChunkSize;
    BufDesc.Height           = 1;
    BufDesc.DepthOrArraySize = 1;
    BufDesc.MipLevels        = 1;
    BufDesc.SampleDesc.Count = 1;
    BufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // Not zeroed: every byte a copy reads from here is written by us first, and the zeroing is
    // what a big upload buffer's creation costs - measured: 16 MB chunks took 8-10 ms each
    // (38.9 ms for the Liberty load frame's). A runtime that does not know the flag gets the
    // plain way.
    ID3D12Resource* Chunk = nullptr;
    HRESULT Hr = Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &BufDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 __uuidof(ID3D12Resource), (void**)&Chunk);
    if (FAILED(Hr))
        Hr = Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             __uuidof(ID3D12Resource), (void**)&Chunk);
    if (FAILED(Hr))
        return 0;
    D3D12_RANGE NoRead;
    NoRead.Begin = 0;
    NoRead.End   = 0;
    BYTE* Mapped = nullptr;
    if (FAILED(Chunk->Map(0, &NoRead, (void**)&Mapped)))
    {
        Chunk->Release();
        return 0;
    }
    SpillChunk[F][N] = Chunk;
    SpillCount[F]    = N + 1;
    SpillCpu[F]      = Mapped;
    SpillSize[F]     = ChunkSize;
    SpillUsed[F]     = Size;
    Buffer = Chunk;
    Offset = 0;
    Cpu    = Mapped;
    return 1;
}

void FTextureCache::EnsureWhite(ID3D12GraphicsCommandList* Cmd)
{
    if (WhiteIndex >= 0 || !Heap || Count >= TEXTURE_SLOTS)
        return;

    D3D12_HEAP_PROPERTIES DefaultHeap;
    appMemzero(&DefaultHeap, sizeof(DefaultHeap));
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC TexDesc;
    appMemzero(&TexDesc, sizeof(TexDesc));
    TexDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    TexDesc.Width            = 1;
    TexDesc.Height           = 1;
    TexDesc.DepthOrArraySize = 1;
    TexDesc.MipLevels        = 1;
    TexDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    TexDesc.SampleDesc.Count = 1;

    ID3D12Resource* Texture = nullptr;
    if (FAILED(Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &TexDesc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               __uuidof(ID3D12Resource), (void**)&Texture)))
        return;

    D3D12_HEAP_PROPERTIES UploadHeap;
    appMemzero(&UploadHeap, sizeof(UploadHeap));
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC BufDesc;
    appMemzero(&BufDesc, sizeof(BufDesc));
    BufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufDesc.Width            = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    BufDesc.Height           = 1;
    BufDesc.DepthOrArraySize = 1;
    BufDesc.MipLevels        = 1;
    BufDesc.SampleDesc.Count = 1;
    BufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* Upload = nullptr;
    if (FAILED(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               __uuidof(ID3D12Resource), (void**)&Upload)))
    {
        Texture->Release();
        return;
    }

    BYTE* Mapped = nullptr;
    D3D12_RANGE NoRead;
    NoRead.Begin = 0;
    NoRead.End   = 0;
    if (SUCCEEDED(Upload->Map(0, &NoRead, (void**)&Mapped)))
    {
        // Whatever value cancels the light scale exactly, so geometry with no light map
        // keeps its natural brightness. Never hard-code it: the two numbers belong
        // together (see RdxMath::NeutralLightTexel).
        const BYTE N = RdxMath::NeutralLightTexel(RDX_LIGHT_SCALE_VALUE);
        Mapped[0] = N; Mapped[1] = N; Mapped[2] = N; Mapped[3] = 255;
        Upload->Unmap(0, nullptr);
    }

    D3D12_TEXTURE_COPY_LOCATION Dst;
    appMemzero(&Dst, sizeof(Dst));
    Dst.pResource = Texture;
    Dst.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION Src;
    appMemzero(&Src, sizeof(Src));
    Src.pResource                          = Upload;
    Src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    Src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    Src.PlacedFootprint.Footprint.Width    = 1;
    Src.PlacedFootprint.Footprint.Height   = 1;
    Src.PlacedFootprint.Footprint.Depth    = 1;
    Src.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    Cmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);

    D3D12_RESOURCE_BARRIER Barrier;
    appMemzero(&Barrier, sizeof(Barrier));
    Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource   = Texture;
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Cmd->ResourceBarrier(1, &Barrier);

    Staging[CurrentFrame][StagingCount[CurrentFrame]++] = Upload;

    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc;
    appMemzero(&SrvDesc, sizeof(SrvDesc));
    SrvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SrvDesc.Texture2D.MipLevels     = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = Heap->GetCPUDescriptorHandleForHeapStart();
    CpuHandle.ptr += (SIZE_T)Count * HeapStride;
    Device->CreateShaderResourceView(Texture, &SrvDesc, CpuHandle);

    FEntry& E = Entries[Count];
    E.CacheId   = 0;
    E.PaletteId = 0;
    E.Resource  = Texture;
    E.SrvIndex  = Count;
    E.bMasked   = 0;
    E.bSrgb     = 0;
    E.bKeepAlpha = 0;
    E.USize     = 1;
    E.VSize     = 1;
    E.Format    = -1;          // ours, not the engines: never refreshed
    E.MipCount  = 1;
    E.RefreshSerial = FrameSerial;
    WhiteIndex  = Count;
    HashInsert(Count);
    Count++;

    RdxLog(TEXT("neutral light map created at slot %i"), WhiteIndex);
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

// The palette as ready-made 32-bit texels - see Convert.
// A masked texture's entry 0 is never drawn, but its colour is read: wherever a filter reaches further into the hidden
// texels than the edges' colours are carried over them (RdxMath::DilateTransparentEdges) - the coarse levels of a far
// tree's leaves, a mirror ray's look at them. The engine keeps a bright magenta there, and what reads it turns pink.
// In its place goes the average of the colours the picture does show, by how many texels of its largest level wear
// each (every fifth of them either way is counted): hidden, a texel is then of the picture's own colour.
static void BuildP8Texels(const FTextureInfo& Info, UBOOL bMasked, DWORD* Texel)
{
    for (INT i = 0; i < 256; i++)
    {
        const FColor& C = Info.Palette[i];
        RdxMath::FRgba E;
        E.R = C.R; E.G = C.G; E.B = C.B; E.A = C.A;
        Texel[i] = RdxMath::PackRgba(RdxMath::PaletteTexel((unsigned char)i, E, bMasked != 0));
    }
    const FMipmapBase* const Top = Info.Mips[0];
    if (!bMasked || !Top || !Top->DataPtr || Top->USize <= 0 || Top->VSize <= 0)
        return;
    DWORD Seen[256];
    appMemzero(Seen, sizeof(Seen));
    const INT Step = (Top->USize * Top->VSize > 4096) ? 5 : 1;
    for (INT y = 0; y < Top->VSize; y += Step)
    {
        const BYTE* const Row = Top->DataPtr + y * Top->USize;
        for (INT x = 0; x < Top->USize; x += Step)
            Seen[Row[x]]++;
    }
    QWORD Sum[3] = { 0, 0, 0 }, Count = 0;
    for (INT i = 1; i < 256; i++)
    {
        const FColor& C = Info.Palette[i];
        Sum[0] += (QWORD)C.R * Seen[i];
        Sum[1] += (QWORD)C.G * Seen[i];
        Sum[2] += (QWORD)C.B * Seen[i];
        Count  += Seen[i];
    }
    if (Count == 0)
        return;
    RdxMath::FRgba Hidden;
    Hidden.R = (BYTE)(Sum[0] / Count);
    Hidden.G = (BYTE)(Sum[1] / Count);
    Hidden.B = (BYTE)(Sum[2] / Count);
    Hidden.A = 0;
    Texel[0] = RdxMath::PackRgba(Hidden);
}

// Rows Y0..Y1-1 of one palettised level: one table lookup and one 32-bit write per texel.
// Touches nothing but its own rows, so bands of one level can run side by side.
static void ExpandP8Rows(const FMipmapBase* Mip, const DWORD* Texel, INT Y0, INT Y1,
                         BYTE* Dest, INT DestPitch)
{
    const INT   W   = Mip->USize;
    const BYTE* Src = Mip->DataPtr;
    for (INT y = Y0; y < Y1; y++)
    {
        DWORD*      Row = (DWORD*)(Dest + y * DestPitch);
        const BYTE* S   = Src + y * W;
        for (INT x = 0; x < W; x++)
            Row[x] = Texel[S[x]];
    }
}

UBOOL FTextureCache::Convert(const FTextureInfo& Info, INT MipIndex, UBOOL bMasked, UBOOL bLightData,
                             UBOOL bKeepAlpha, BYTE* Dest, INT DestPitch) const
{
    const FMipmapBase* Mip = Info.Mips[MipIndex];
    if (!Mip || !Mip->DataPtr)
        return 0;

    const INT W = Mip->USize;
    const INT H = Mip->VSize;

    if (Info.Format == TEXF_P8)
    {
        if (!Info.Palette)
            return 0;

        // The palette as ready-made texels, then one table lookup and one 32-bit write per
        // texel. The bytes are the same ones the per-byte version wrote - R, G, B, then alpha,
        // in that order in memory - but that version cost about 8.5 ms of a 23 ms frame while
        // turning: measured, expanding the 21.7 MB of new textures one frame brought.
        //
        // Masked textures use palette index 0 as the transparent colour. That is the engine's
        // convention, not the palette's: entry 0 still holds a real colour, it simply must not
        // be drawn.
        DWORD Texel[256];
        BuildP8Texels(Info, bMasked, Texel);
        ExpandP8Rows(Mip, Texel, 0, H, Dest, DestPitch);

        // Entry 0 is bright magenta. Alpha alone does not hide it: the sampler blends
        // neighbouring RGB whatever the alpha, so every silhouette would be outlined in
        // magenta. Give the transparent texels their neighbours' colour instead - on every
        // mip level, since each has its own transparent texels.
        if (bMasked)
        {
            const DOUBLE D0 = appSeconds();
            RdxMath::DilateTransparentEdges(Dest, W, H, DestPitch);
            FrameDilateSec += appSeconds() - D0;
        }

        return 1;
    }

    // Eight bits a channel, R G B A in memory, as they are: the renderer's own pictures (the plants' bark and leaves,
    // Trees3D) - the engine's textures never come so. A masked one has its hidden texels coloured as the palette's are.
    if (Info.Format == TEXF_RGBA8)
    {
        for (INT y = 0; y < H; y++)
            appMemcpy(Dest + y * DestPitch, Mip->DataPtr + y * W * 4, W * 4);
        if (bMasked)
        {
            const DOUBLE D0 = appSeconds();
            RdxMath::DilateTransparentEdges(Dest, W, H, DestPitch);
            FrameDilateSec += appSeconds() - D0;
        }
        return 1;
    }

    if (Info.Format == TEXF_RGBA7)
    {
        // Seven bits per channel, stored in a 32-bit word. Doubling recovers the full range; the
        // engine used the spare bit for overbright lighting. A light map's red and blue come the
        // other way round (RdxMath::LightMapTexel). A row at a time, four texels at once - byte
        // for byte what the texel-by-texel loop through FColor wrote (TestRgba7Rows), as long as
        // R, G, B are an FColor's bytes 0, 1, 2, which the assertion holds the SDK to.
        static_assert(offsetof(FColor, R) == 0 && offsetof(FColor, G) == 1 && offsetof(FColor, B) == 2,
                      "RdxMath::ExpandRgba7Row reads bytes: an FColor must be R, G, B, A in memory");
        for (INT y = 0; y < H; y++)
            RdxMath::ExpandRgba7Row(Mip->DataPtr + y * W * 4, Dest + y * DestPitch, W, bLightData != 0,
                                    bKeepAlpha != 0);
        return 1;
    }

    // DXT1 never reaches here: the GPU understands it natively (BC1), so the upload
    // copies the compressed blocks straight through instead of expanding them.
    //
    // Anything else is not handled yet. Report each format once: a silently skipped
    // texture looks exactly like a missing feature, and we would rather know which.
    static UBOOL Reported[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const INT Slot = ((INT)Info.Format >= 0 && (INT)Info.Format < 8) ? (INT)Info.Format : 7;
    if (!Reported[Slot])
    {
        Reported[Slot] = 1;
        RdxLog(TEXT("unsupported texture format %i (%ix%i) - geometry using it is skipped"),
               (INT)Info.Format, W, H);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

INT FTextureCache::HashOf(QWORD CacheId, QWORD PaletteId, UBOOL bMasked, UBOOL bSrgb)
{
    QWORD K = CacheId * 0x9E3779B97F4A7C15ULL;
    K ^= PaletteId + 0x165667B19E3779F9ULL + (K << 6) + (K >> 2);
    K ^= (QWORD)((bMasked ? 1 : 0) | (bSrgb ? 2 : 0)) * 0xC2B2AE3D27D4EB4FULL;
    K ^= K >> 29;
    return (INT)(K & (QWORD)(HASH_SIZE - 1));
}

void FTextureCache::HashClear()
{
    appMemzero(Hash, sizeof(Hash));
}

void FTextureCache::HashInsert(INT EntryIndex)
{
    const FEntry& E = Entries[EntryIndex];
    INT Slot = HashOf(E.CacheId, E.PaletteId, E.bMasked, E.bSrgb);
    for (INT Probe = 0; Probe < HASH_SIZE; Probe++)
    {
        if (Hash[Slot] == 0)
        {
            Hash[Slot] = EntryIndex + 1;
            return;
        }
        Slot = (Slot + 1) & (HASH_SIZE - 1);
    }
}

INT FTextureCache::Find(QWORD CacheId, QWORD PaletteId, UBOOL bMasked, UBOOL bSrgb) const
{
    INT Slot = HashOf(CacheId, PaletteId, bMasked, bSrgb);
    for (INT Probe = 0; Probe < HASH_SIZE; Probe++)
    {
        const INT Stored = Hash[Slot];
        if (Stored == 0)
            return -1;                  // an empty slot: it is certainly not in the cache
        const FEntry& E = Entries[Stored - 1];
        if (E.CacheId == CacheId && E.PaletteId == PaletteId &&
            E.bMasked == bMasked && E.bSrgb == bSrgb)
            return Stored - 1;
        Slot = (Slot + 1) & (HASH_SIZE - 1);
    }
    return -1;
}

INT FTextureCache::FindColour(QWORD CacheId, QWORD PaletteId, UBOOL bMasked, UBOOL bSrgb) const
{
    INT Entry = Find(CacheId, PaletteId, bMasked ? 1 : 0, bSrgb ? 1 : 0);
    if (Entry < 0 && !bMasked && bSrgb)
        Entry = Find(CacheId, PaletteId, 1, 1);
    return (Entry >= 0) ? Entries[Entry].SrvIndex : -1;
}

UBOOL FTextureCache::WrittenThisFrame(const FTextureInfo& Info, DWORD PolyFlags, UBOOL bSrgb) const
{
    const INT Entry = Find(Info.CacheID, Info.PaletteCacheID, (PolyFlags & PF_Masked) ? 1 : 0, bSrgb);
    return Entry >= 0 && Entries[Entry].RefreshSerial == FrameSerial;
}

// A transition barrier on the whole resource.
static void Transition(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* Resource,
                       D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
{
    D3D12_RESOURCE_BARRIER Barrier;
    appMemzero(&Barrier, sizeof(Barrier));
    Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource   = Resource;
    Barrier.Transition.StateBefore = Before;
    Barrier.Transition.StateAfter  = After;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Cmd->ResourceBarrier(1, &Barrier);
}

UBOOL FTextureCache::CanRecordCopy(const FTextureInfo& Info)
{
    // Never refuse silently: a refused texture is uploaded later, mid-play, and that is
    // felt as a stutter. Each reason is counted and reported once it first happens.
    static INT NoStaging = 0, NoData = 0;
    if (StagingCount[CurrentFrame] >= MAX_STAGING_PER_FRAME)
    {
        if (++NoStaging == 1) RdxLog(TEXT("texture refused: staging is full (%i)"), StagingCount[CurrentFrame]);
        return 0;
    }

    const FMipmapBase* Mip = Info.Mips[0];
    if (!Mip || !Mip->DataPtr || Mip->USize <= 0 || Mip->VSize <= 0)
    {
        if (++NoData == 1) RdxLog(TEXT("texture refused: no mip data (%ix%i fmt=%i)"),
                                     Info.USize, Info.VSize, (INT)Info.Format);
        return 0;
    }
    return 1;
}

// How many of the engine's mips make a valid D3D12 chain: every level present, and each
// exactly half the one above it, never below one. A light map is rebuilt every frame and
// only its top level is filled, so it gets exactly one.
static INT UsableMips(const FTextureInfo& Info)
{
    const FMipmapBase* Top = Info.Mips[0];
    if (!Top || !Top->DataPtr || Top->USize <= 0 || Top->VSize <= 0)
        return 0;
    if (Info.Format == TEXF_RGBA7)
        return 1;

    INT Limit = (Info.NumMips < MAX_MIPS) ? Info.NumMips : MAX_MIPS;
    if (Limit < 1)
        Limit = 1;
    INT N = 1;
    for (; N < Limit; N++)
    {
        const FMipmapBase* M = Info.Mips[N];
        const FMipmapBase* P = Info.Mips[N - 1];
        if (!M || !M->DataPtr)
            break;
        if (M->USize != ((P->USize > 1) ? P->USize / 2 : 1) ||
            M->VSize != ((P->VSize > 1) ? P->VSize / 2 : 1))
            break;
    }
    return N;
}

void FTextureCache::ParallelFor(INT N, void (*Body)(void* Ctx, INT Index), void* Ctx)
{
    Job.Next  = 0;
    Job.Count = N;
    Job.Body  = Body;
    Job.Ctx   = Ctx;
    const INT Helpers = (Pool && N > 1) ? ((N - 1 < PoolHelpers) ? N - 1 : PoolHelpers) : 0;
    for (INT k = 0; k < Helpers; k++)
        ::SubmitThreadpoolWork(Pool);
    ParallelWorker(nullptr, &Job, nullptr);      // this thread takes its share too
    if (Helpers)
        ::WaitForThreadpoolWorkCallbacks(Pool, FALSE);
}

UBOOL FTextureCache::EnsureScratch(UINT64 Bytes)
{
    if (Bytes > 0x7FFFFFFF)
        return 0;
    if ((INT)Bytes > ScratchSize)
    {
        if (Scratch)
            appFree(Scratch);
        Scratch     = (BYTE*)appMalloc((DWORD)Bytes, TEXT("RdxScratch"));
        ScratchSize = Scratch ? (INT)Bytes : 0;
    }
    return Scratch != nullptr;
}

namespace
{
    // One band of rows of one level, for the expansion.
    struct FConvertBand
    {
        INT Level;
        INT Y0;
        INT Y1;
    };

    // What the side-by-side conversion shares with its workers. Read-only while they run,
    // except Out, where each writes only its own rows (expansion) or its own level (dilation).
    struct FConvertCtx
    {
        const FTextureInfo*                       Info;
        const DWORD*                              Texel;
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Layouts;
        BYTE*                                     Out;
        const FConvertBand*                       Bands;
        unsigned int*                             Work[16];
    };

    void ExpandBand(void* P, INT i)
    {
        const FConvertCtx&  C = *(const FConvertCtx*)P;
        const FConvertBand& B = C.Bands[i];
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& L = C.Layouts[B.Level];
        ExpandP8Rows(C.Info->Mips[B.Level], C.Texel, B.Y0, B.Y1, C.Out + L.Offset,
                     (INT)L.Footprint.RowPitch);
    }

    void DilateLevel(void* P, INT m)
    {
        const FConvertCtx& C   = *(const FConvertCtx*)P;
        const FMipmapBase* Mip = C.Info->Mips[m];
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& L = C.Layouts[m];
        RdxMath::DilateTransparentEdges(C.Out + L.Offset, Mip->USize, Mip->VSize,
                                        (INT)L.Footprint.RowPitch, RDX_DILATE_PASSES, C.Work[m]);
    }
}

// Every level of a big palettised texture at once, into Out laid out as Layouts says: first
// the expansion, in bands of rows across all levels; then the dilation, one level per task,
// the biggest first - it takes longest, the others fit around it. The same functions as the
// plain way (Convert), on the same data, each band and level writing only its own memory: the
// result is the same, byte for byte. The workers call nothing of the engine's; everything they
// need - the work memory included - is set up here first.
UBOOL FTextureCache::ConvertLevelsParallel(const FTextureInfo& Info, INT MipCount, UBOOL bMasked,
                                           const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Layouts, BYTE* Out)
{
    if (MipCount <= 0 || MipCount > 16 || !Info.Palette)
        return 0;
    for (INT m = 0; m < MipCount; m++)
        if (!Info.Mips[m] || !Info.Mips[m]->DataPtr)
            return 0;

    DWORD Texel[256];
    BuildP8Texels(Info, bMasked, Texel);

    // At most 32 bands a level - enough to spread the biggest one over every thread, few
    // enough that handing them out costs nothing - so 16 levels fit MAX_BANDS.
    FConvertBand Bands[MAX_BANDS];
    INT NumBands = 0;
    for (INT m = 0; m < MipCount; m++)
    {
        const INT H    = Info.Mips[m]->VSize;
        INT       Step = (H + 31) / 32;
        if (Step < 16)
            Step = 16;
        for (INT y = 0; y < H; y += Step)
        {
            if (NumBands >= MAX_BANDS)
                return 0;
            Bands[NumBands].Level = m;
            Bands[NumBands].Y0    = y;
            Bands[NumBands].Y1    = (y + Step < H) ? y + Step : H;
            NumBands++;
        }
    }

    FConvertCtx C;
    appMemzero(&C, sizeof(C));
    C.Info    = &Info;
    C.Texel   = Texel;
    C.Layouts = Layouts;
    C.Out     = Out;
    C.Bands   = Bands;
    ParallelFor(NumBands, ExpandBand, &C);

    if (bMasked)
    {
        long Need = 0;
        for (INT m = 0; m < MipCount; m++)
            Need += RdxMath::DilateWorkWords(Info.Mips[m]->USize, Info.Mips[m]->VSize);
        if (Need > DilateWorkHave)
        {
            if (DilateWork)
                appFree(DilateWork);
            DilateWork     = (unsigned int*)appMalloc((DWORD)(Need * sizeof(unsigned int)), TEXT("RdxDilateWork"));
            DilateWorkHave = DilateWork ? Need : 0;
            if (!DilateWork)
                return 0;
        }
        long At = 0;
        for (INT m = 0; m < MipCount; m++)
        {
            C.Work[m] = DilateWork + At;
            At += RdxMath::DilateWorkWords(Info.Mips[m]->USize, Info.Mips[m]->VSize);
        }
        const DOUBLE D0 = appSeconds();
        ParallelFor(MipCount, DilateLevel, &C);
        FrameDilateSec += appSeconds() - D0;
    }
    return 1;
}

// Diagnostic, validation mode only: the side-by-side conversion must give exactly what the
// plain one gives. Every texture of 1024x1024 or more, and the first 400 of the smaller ones,
// are converted again, level by level with Convert, and compared row by row - the padding at
// the rows' ends is written by neither. (A limit on all of them was used up by a level's
// precache - hundreds of 256x256 ones - before the biggest ever came.) Logged: every
// mismatch; the first ten matches and every big one; a count at every fiftieth.
void FTextureCache::CheckParallel(const FTextureInfo& Info, INT MipCount, UBOOL bMasked, UBOOL bSrgb, UBOOL bKeepAlpha,
                                  const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Layouts, const BYTE* Got)
{
    const UBOOL bBig = Info.Mips[0]->USize * Info.Mips[0]->VSize >= 1024 * 1024;
    if (!bCheckParallel || (!bBig && ParallelChecks >= 400))
        return;
    ParallelChecks++;
    INT Bad = -1;
    for (INT m = 0; m < MipCount && Bad < 0; m++)
    {
        const FMipmapBase* Mip   = Info.Mips[m];
        const INT          Pitch = (INT)Layouts[m].Footprint.RowPitch;
        BYTE* Ref = (BYTE*)appMalloc((DWORD)(Pitch * Mip->VSize), TEXT("RdxCheck"));
        if (!Ref)
            return;
        if (!Convert(Info, m, bMasked, !bSrgb, bKeepAlpha, Ref, Pitch))
            Bad = m;
        for (INT y = 0; y < Mip->VSize && Bad < 0; y++)
            if (memcmp(Ref + y * Pitch, Got + Layouts[m].Offset + (SIZE_T)y * Pitch, Mip->USize * 4) != 0)
                Bad = m;
        appFree(Ref);
    }
    const TCHAR* Name = Info.Texture ? Info.Texture->GetName() : TEXT("(light map)");
    if (Bad >= 0)
    {
        ParallelBad++;
        RdxLog(TEXT("parallel conversion: %s - DIFFERS at level %i"), Name, Bad);
    }
    else if (ParallelChecks <= 10 || Info.Mips[0]->USize * Info.Mips[0]->VSize >= 1024 * 1024)
        RdxLog(TEXT("parallel conversion checked: %s %ix%i, %i levels, masked %i - identical"), Name,
               Info.Mips[0]->USize, Info.Mips[0]->VSize, MipCount, bMasked ? 1 : 0);
    if (ParallelChecks % 50 == 0)
        RdxLog(TEXT("parallel conversion: %i checked so far, %i differ"), ParallelChecks, ParallelBad);
}

static void GpuTransition(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* R,
                          D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
{
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = R;
    B.Transition.StateBefore = Before;
    B.Transition.StateAfter  = After;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Cmd->ResourceBarrier(1, &B);
}

UBOOL FTextureCache::EnsureGpuScratch(UINT64 Bytes)
{
    const INT S = CurrentFrame;
    if (GpuOut[S] && GpuTemp[S] && GpuScratchSize[S] >= Bytes)
        return 1;
    // Outgrown: this frame's command list may still name the old pair, so it goes with the slot.
    if (GpuOldCount[S] + 2 > MAX_GPU_OLD)
        return 0;
    if (GpuOut[S])  GpuOld[S][GpuOldCount[S]++] = GpuOut[S];
    if (GpuTemp[S]) GpuOld[S][GpuOldCount[S]++] = GpuTemp[S];
    GpuOut[S] = GpuTemp[S] = nullptr;
    GpuScratchSize[S] = 0;

    // Whole megabytes, and at least GPU_SCRATCH_MIN - see there.
    UINT64 Size = (Bytes + 0xFFFFF) & ~(UINT64)0xFFFFF;
    if (Size < (UINT64)GPU_SCRATCH_MIN)
        Size = GPU_SCRATCH_MIN;
    if (!CreateGpuScratch(S, Size))
        return 0;
    // The other slots' pairs as well, the first time: made at their own first use, they would
    // come mid-play just as likely.
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
        if (f != S && !GpuOut[f] && !GpuTemp[f])
            CreateGpuScratch(f, GPU_SCRATCH_MIN);
    return 1;
}

UBOOL FTextureCache::CreateGpuScratch(INT S, UINT64 Size)
{
    D3D12_HEAP_PROPERTIES Hp;
    appMemzero(&Hp, sizeof(Hp));
    Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = Size;
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // In COMMON: a buffer is promoted to UNORDERED_ACCESS by its first use in a command list and
    // decays back after it, so the only transitions written out are the ones around the copies
    // out of it.
    if (FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, __uuidof(ID3D12Resource), (void**)&GpuOut[S])) ||
        FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, __uuidof(ID3D12Resource), (void**)&GpuTemp[S])))
    {
        if (GpuOut[S])  { GpuOut[S]->Release();  GpuOut[S]  = nullptr; }
        if (GpuTemp[S]) { GpuTemp[S]->Release(); GpuTemp[S] = nullptr; }
        return 0;
    }
    GpuScratchSize[S] = Size;
    return 1;
}

// Validation mode's choice of what to check - see GpuCheckRecord. Every texture of 1024x1024 or
// more is checked. A limit on all of them was used up by a level's precache - hundreds of
// 256x256 ones - before the biggest came (measured: 300 checks, none of them big), as it once
// was for CheckParallel.
UBOOL FTextureCache::GpuCheckWanted(const FTextureInfo& Info, UINT64 Total) const
{
    const INT S = CurrentFrame;
    if (!bCheckParallel || Total > 0x7FFFFFFF)
        return 0;
    if (Info.Mips[0]->USize * Info.Mips[0]->VSize >= 1024 * 1024)
        return GpuCheckCount[S] < MAX_GPU_CHECKS &&
               (GpuCheckCount[S] == 0 || GpuCheckBytes[S] + Total <= 2 * (UINT64)GPU_CHECK_BUDGET);
    return GpuSmallChecks < MAX_GPU_CHECKS_TOTAL && GpuCheckCount[S] < MAX_GPU_CHECKS - 4 &&
           (GpuCheckCount[S] == 0 || GpuCheckBytes[S] + Total <= (UINT64)GPU_CHECK_BUDGET);
}

UBOOL FTextureCache::RecordGpuConvert(const FTextureInfo& Info, INT MipCount, UBOOL bMasked,
                                      const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Layouts, UINT64 Total,
                                      ID3D12GraphicsCommandList* Cmd)
{
    if (!Gpu.IsReady() || MipCount <= 0 || MipCount > 16 || !Info.Palette || Total > 0x7FFFFFFF)
        return 0;
    for (INT m = 0; m < MipCount; m++)
        if (!Info.Mips[m] || !Info.Mips[m]->DataPtr)
            return 0;
    // The buffers first: if they cannot be had, nothing has been used yet.
    if (!EnsureGpuScratch(Total))
        return 0;

    // The indices of every level, 4-byte aligned, then the ready-made texels - into this slot's
    // upload memory, like any upload: a quarter of the converted texture's bytes.
    FGpuConvert::FLevel L[16];
    UINT64 Bytes = 0;
    for (INT m = 0; m < MipCount; m++)
    {
        L[m].W        = (UINT)Info.Mips[m]->USize;
        L[m].H        = (UINT)Info.Mips[m]->VSize;
        L[m].IndexOff = (UINT)Bytes;
        L[m].Off      = (UINT)Layouts[m].Offset;
        L[m].Pitch    = Layouts[m].Footprint.RowPitch;
        Bytes += ((UINT64)L[m].W * L[m].H + 3) & ~(UINT64)3;
    }
    const UINT TableOff = (UINT)Bytes;
    Bytes += 256 * sizeof(DWORD);
    ID3D12Resource* Src    = nullptr;
    UINT64          SrcOff = 0;
    BYTE*           SrcCpu = nullptr;
    if (!RingAlloc(Bytes, Src, SrcOff, SrcCpu) && !SpillAlloc(Bytes, Src, SrcOff, SrcCpu))
        return 0;
    for (INT m = 0; m < MipCount; m++)
        appMemcpy(SrcCpu + L[m].IndexOff, Info.Mips[m]->DataPtr, (INT)(L[m].W * L[m].H));
    DWORD Texel[256];
    BuildP8Texels(Info, bMasked, Texel);
    appMemcpy(SrcCpu + TableOff, Texel, sizeof(Texel));

    // These dispatches replace the command list's pipeline state. Nothing downstream relies on
    // it: every pass sets its own before it draws (SolidPass::Flush, FinalPass).
    const INT S = CurrentFrame;
    Gpu.RecordLevels(Cmd, Src->GetGPUVirtualAddress() + SrcOff, TableOff, L, MipCount,
                     GpuOut[S], GpuTemp[S], bMasked, RDX_DILATE_PASSES);
    GpuTransition(Cmd, GpuOut[S], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return 1;
}

// GpuOut holds the GPU's result, in COPY_SOURCE: it is copied to a readback buffer, and Reference -
// the CPU's result, in the same layout - is copied aside, both kept until the frame slot's fence
// has passed (GpuCompareFinish).
void FTextureCache::GpuCheckRecord(const FTextureInfo& Info, INT MipCount, UBOOL bMasked,
                                   const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Layouts, UINT64 Total,
                                   const BYTE* Reference, ID3D12GraphicsCommandList* Cmd)
{
    const INT   S    = CurrentFrame;
    const UBOOL bBig = Info.Mips[0]->USize * Info.Mips[0]->VSize >= 1024 * 1024;
    if (GpuCheckCount[S] >= MAX_GPU_CHECKS || !GpuOut[S] || MipCount <= 0 || MipCount > 16 || Total > 0x7FFFFFFF)
        return;
    D3D12_HEAP_PROPERTIES Hp;
    appMemzero(&Hp, sizeof(Hp));
    Hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = Total;
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* Rb = nullptr;
    if (FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, __uuidof(ID3D12Resource), (void**)&Rb)))
        return;
    BYTE* Ref = (BYTE*)appMalloc((DWORD)Total, TEXT("RdxGpuCheck"));
    if (!Ref)
    {
        Rb->Release();
        return;
    }
    appMemcpy(Ref, Reference, (INT)Total);

    Cmd->CopyBufferRegion(Rb, 0, GpuOut[S], 0, Total);

    FGpuCheck& C = GpuChecks[S][GpuCheckCount[S]++];
    appMemzero(&C, sizeof(C));
    C.Readback  = Rb;
    C.Reference = Ref;
    C.Bytes     = Total;
    C.MipCount  = MipCount;
    C.bMasked   = bMasked;
    for (INT m = 0; m < MipCount; m++)
    {
        C.W[m]      = Info.Mips[m]->USize;
        C.H[m]      = Info.Mips[m]->VSize;
        C.Offset[m] = Layouts[m].Offset;
        C.Pitch[m]  = Layouts[m].Footprint.RowPitch;
    }
    const TCHAR* Name = Info.Texture ? Info.Texture->GetName() : TEXT("(light map)");
    INT k = 0;
    for (; k < 63 && Name[k]; k++)
        C.Name[k] = Name[k];
    C.Name[k] = 0;
    GpuCheckBytes[S] += Total;
    if (!bBig)
        GpuSmallChecks++;
}

void FTextureCache::GpuCompareFinish(INT S)
{
    for (INT i = 0; i < GpuCheckCount[S]; i++)
    {
        FGpuCheck& C = GpuChecks[S][i];
        INT   BadM = -1, BadX = 0, BadY = 0;
        DWORD GotV = 0, RefV = 0;
        BYTE* Got  = nullptr;
        D3D12_RANGE R;
        R.Begin = 0;
        R.End   = (SIZE_T)C.Bytes;
        if (C.Readback && SUCCEEDED(C.Readback->Map(0, &R, (void**)&Got)))
        {
            for (INT m = 0; m < C.MipCount && BadM < 0; m++)
                for (INT y = 0; y < C.H[m] && BadM < 0; y++)
                {
                    const DWORD* G = (const DWORD*)(Got + C.Offset[m] + (SIZE_T)y * C.Pitch[m]);
                    const DWORD* F = (const DWORD*)(C.Reference + C.Offset[m] + (SIZE_T)y * C.Pitch[m]);
                    for (INT x = 0; x < C.W[m]; x++)
                        if (G[x] != F[x])
                        {
                            BadM = m; BadX = x; BadY = y; GotV = G[x]; RefV = F[x];
                            break;
                        }
                }
            D3D12_RANGE None;
            None.Begin = None.End = 0;
            C.Readback->Unmap(0, &None);
        }
        else
            BadM = -2;
        GpuChecksDone++;
        if (BadM == -2)
            RdxLog(TEXT("gpu conversion: %s - the read-back failed"), C.Name);
        else if (BadM >= 0)
        {
            GpuChecksBad++;
            RdxLog(TEXT("gpu conversion: %s %ix%i - DIFFERS at level %i, %i,%i: gpu %08X, cpu %08X"),
                   C.Name, C.W[0], C.H[0], BadM, BadX, BadY, GotV, RefV);
        }
        else if (GpuChecksDone <= 10 || C.W[0] * C.H[0] >= 1024 * 1024)
            RdxLog(TEXT("gpu conversion checked: %s %ix%i, %i levels, masked %i - identical"),
                   C.Name, C.W[0], C.H[0], C.MipCount, C.bMasked ? 1 : 0);
        if (GpuChecksDone % 25 == 0)
            RdxLog(TEXT("gpu conversion: %i checked so far, %i differ"), GpuChecksDone, GpuChecksBad);
        if (C.Readback)
            C.Readback->Release();
        if (C.Reference)
            appFree(C.Reference);
        C.Readback  = nullptr;
        C.Reference = nullptr;
    }
    GpuCheckCount[S] = 0;
    GpuCheckBytes[S] = 0;
    for (INT i = 0; i < GpuOldCount[S]; i++)
        GpuOld[S][i]->Release();
    GpuOldCount[S] = 0;
}

// A measurement only (the marker %TEMP%\SecondRevision-fire-dump.txt, ten minutes fresh): the game's fire textures as
// the engine hands them over - they are drawn afresh every frame (real-time), so nothing on disk shows what they look
// like. A few frames of each, well apart, to %TEMP%\SecondRevision-fire-dump\<name>-<n>.bmp, the palette spelled out.
static bool FireDumpWanted()
{
    static int Wanted = -1;
    if (Wanted < 0)
    {
        Wanted = 0;
        wchar_t Path[MAX_PATH];
        const DWORD N = ::GetTempPathW(MAX_PATH, Path);
        if (N > 0 && N + 40 < MAX_PATH)
        {
            wcscat_s(Path, L"SecondRevision-fire-dump.txt");
            WIN32_FILE_ATTRIBUTE_DATA A;
            if (::GetFileAttributesExW(Path, GetFileExInfoStandard, &A))
            {
                FILETIME Now;
                ::GetSystemTimeAsFileTime(&Now);
                const ULONGLONG T = ((ULONGLONG)A.ftLastWriteTime.dwHighDateTime << 32) | A.ftLastWriteTime.dwLowDateTime;
                const ULONGLONG C = ((ULONGLONG)Now.dwHighDateTime << 32) | Now.dwLowDateTime;
                Wanted = (C >= T && C - T < 10ULL * 60ULL * 10000000ULL) ? 1 : 0;
            }
        }
    }
    return Wanted == 1;
}

static void DumpFireTexture(const FTextureInfo& Info)
{
    if (!Info.Texture || Info.Format != TEXF_P8 || !Info.Palette || !Info.Mips[0] || !Info.Mips[0]->DataPtr)
        return;
    const TCHAR* const Path   = Info.Texture->GetPathName();
    const TCHAR* const Prefix = TEXT("Effects.Fire.");
    INT k = 0;
    while (Prefix[k] && Path[k] == Prefix[k])
        k++;
    if (Prefix[k] || !FireDumpWanted())
        return;
    // Four frames of each, at least twenty frames apart.
    enum { SEEN = 32, EACH = 4 };
    static QWORD Ids[SEEN];
    static INT   Done[SEEN], Last[SEEN];
    static INT   Used = 0, Calls = 0;
    Calls++;
    INT s = 0;
    while (s < Used && Ids[s] != Info.CacheID)
        s++;
    if (s == Used)
    {
        if (Used == SEEN)
            return;
        Ids[Used] = Info.CacheID;
        Done[Used] = 0;
        Last[Used] = -1000;
        Used++;
    }
    if (Done[s] >= EACH || Calls - Last[s] < 20)
        return;
    Last[s] = Calls;
    const INT W = Info.Mips[0]->USize, H = Info.Mips[0]->VSize;
    if (W <= 0 || H <= 0 || W > 1024 || H > 1024)
        return;
    wchar_t Dir[MAX_PATH];
    const DWORD N = ::GetTempPathW(MAX_PATH, Dir);
    if (N == 0 || N + 80 >= MAX_PATH)
        return;
    wcscat_s(Dir, L"SecondRevision-fire-dump");
    ::CreateDirectoryW(Dir, nullptr);
    wchar_t File[MAX_PATH];
    appSprintf(File, TEXT("%s\\%s-%d.bmp"), Dir, Info.Texture->GetName(), Done[s]);
    Done[s]++;
    const INT   Row  = (W * 3 + 3) & ~3;
    const DWORD Data = (DWORD)(Row * H);
    BYTE Head[54];
    appMemzero(Head, sizeof(Head));
    Head[0] = 'B'; Head[1] = 'M';
    *(DWORD*)(Head + 2)  = 54 + Data;
    *(DWORD*)(Head + 10) = 54;
    *(DWORD*)(Head + 14) = 40;
    *(INT*)(Head + 18)   = W;
    *(INT*)(Head + 22)   = H;
    *(WORD*)(Head + 26)  = 1;
    *(WORD*)(Head + 28)  = 24;
    *(DWORD*)(Head + 34) = Data;
    HANDLE F = ::CreateFileW(File, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (F == INVALID_HANDLE_VALUE)
        return;
    DWORD Wrote = 0;
    ::WriteFile(F, Head, sizeof(Head), &Wrote, nullptr);
    BYTE Line[1024 * 3 + 4];
    const BYTE* const Pix = Info.Mips[0]->DataPtr;
    for (INT y = H - 1; y >= 0; y--)            // a BMP is stored bottom up
    {
        appMemzero(Line, Row);
        for (INT x = 0; x < W; x++)
        {
            const FColor C = Info.Palette[Pix[y * W + x]];
            Line[x * 3 + 0] = C.B;
            Line[x * 3 + 1] = C.G;
            Line[x * 3 + 2] = C.R;
        }
        ::WriteFile(F, Line, Row, &Wrote, nullptr);
    }
    ::CloseHandle(F);
    RdxLog(TEXT("fire dump: %s %ix%i -> %s"), Path, W, H, File);
}

UBOOL FTextureCache::RecordCopy(const FTextureInfo& Info, INT MipCount, UBOOL bMasked, UBOOL bSrgb,
                                ID3D12Resource* Texture, ID3D12GraphicsCommandList* Cmd, UBOOL bKeepAlpha)
{
    DumpFireTexture(Info);
    const UBOOL bCompressed = (Info.Format == TEXF_DXT1);

    // Where every level goes in upload memory. D3D12 owns that layout - the 256-byte row
    // pitch, the 512-byte placement, block rows for BC1 - and says here what it is.
    const D3D12_RESOURCE_DESC TexDesc = Texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layouts[MAX_MIPS];
    UINT   Rows[MAX_MIPS];
    UINT64 RowBytes[MAX_MIPS];
    UINT64 Total = 0;
    Device->GetCopyableFootprints(&TexDesc, 0, (UINT)MipCount, 0, Layouts, Rows, RowBytes, &Total);

    // A big palettised texture goes to the GPU (RecordGpuConvert, docs/terv-gpu-atalakitas.md):
    // up go its indices - a quarter of the converted bytes - and the palette as ready-made texels;
    // compute shaders expand and dilate them into a buffer laid out like upload memory, and the
    // levels are copied into the texture from there. The CPU's part is that one copy. If the GPU
    // path cannot run, the CPU converts, as below.
    if (bGpuConvert && !bCompressed && Info.Format == TEXF_P8 &&
        Info.Mips[0]->USize * Info.Mips[0]->VSize >= GPU_MIN_TEXELS)
    {
        const DOUBLE G0 = appSeconds();
        if (RecordGpuConvert(Info, MipCount, bMasked, Layouts, Total, Cmd))
        {
            ID3D12Resource* Out = GpuOut[CurrentFrame];
            for (INT m = 0; m < MipCount; m++)
            {
                D3D12_TEXTURE_COPY_LOCATION Dst;
                appMemzero(&Dst, sizeof(Dst));
                Dst.pResource        = Texture;
                Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                Dst.SubresourceIndex = (UINT)m;

                D3D12_TEXTURE_COPY_LOCATION Src;
                appMemzero(&Src, sizeof(Src));
                Src.pResource       = Out;
                Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                Src.PlacedFootprint = Layouts[m];

                Cmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
            }
            const DOUBLE Sec = appSeconds() - G0;
            FrameConvertSec += Sec;

            // What the CPU still spends on one: the first few, and every big one.
            static INT Shown = 0, BigLogged = 0;
            const UBOOL bBig = Info.Mips[0]->USize * Info.Mips[0]->VSize >= 1024 * 1024;
            if (++Shown <= 8 || (bBig && ++BigLogged <= 40))
                RdxLog(TEXT("gpu conversion: %s %ix%i, %i levels, masked %i - %.3f ms on the CPU"),
                       Info.Texture ? Info.Texture->GetName() : TEXT("(light map)"),
                       Info.Mips[0]->USize, Info.Mips[0]->VSize, MipCount, bMasked ? 1 : 0, Sec * 1000.0);

            // Validation mode: the CPU converts it as well - the side-by-side way, itself checked
            // against the plain one - and the two results are compared.
            if (GpuCheckWanted(Info, Total) && EnsureScratch(Total) &&
                ConvertLevelsParallel(Info, MipCount, bMasked, Layouts, Scratch))
            {
                CheckParallel(Info, MipCount, bMasked, bSrgb, bKeepAlpha, Layouts, Scratch);
                GpuCheckRecord(Info, MipCount, bMasked, Layouts, Total, Scratch, Cmd);
            }

            // Back, for the next texture's steps.
            GpuTransition(Cmd, Out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return 1;
        }
    }

    // From the frame slot's ring if it fits; otherwise from its spill chunks (see SpillAlloc).
    ID3D12Resource* Source = nullptr;
    UINT64          Offset = 0;
    BYTE*           Mapped = nullptr;
    if (!RingAlloc(Total, Source, Offset, Mapped))
    {
        const DOUBLE O0 = appSeconds();
        FrameOwnBufs++;
        const UBOOL bSpilled = SpillAlloc(Total, Source, Offset, Mapped);
        FrameOwnBufSec += appSeconds() - O0;
        if (!bSpilled)
            return 0;
    }

    UBOOL bConverted = 1;

    // A big palettised texture: every level at once, expanded in bands and dilated side by side on the thread pool
    // (ConvertLevelsParallel), into scratch memory laid out exactly like the upload, then copied across in one go.
    // Worth the fork because one 2048x2048 masked texture and its chain measures 9.6 ms of conversion, 5.6 of it
    // dilation - a whole frame, arriving in the middle of a turn. Small ones - most light maps - go level by level:
    // for them the fork costs more than it saves. The same path serves when the side-by-side one cannot run.
    const FMipmapBase* Top   = Info.Mips[0];
    UBOOL              bDone = 0;
    if (Pool && !bCompressed && Info.Format == TEXF_P8 && Top &&
        Top->USize * Top->VSize >= PARALLEL_MIN_TEXELS && EnsureScratch(Total))
    {
        const DOUBLE V0 = appSeconds();
        bDone = ConvertLevelsParallel(Info, MipCount, bMasked, Layouts, Scratch);
        const DOUBLE V1 = appSeconds();
        FrameConvertSec += V1 - V0;
        if (bDone)
        {
            appMemcpy(Mapped, Scratch, (INT)Total);
            FrameCopySec += appSeconds() - V1;
            CheckParallel(Info, MipCount, bMasked, bSrgb, bKeepAlpha, Layouts, Scratch);
            // Validation mode, when the GPU did not convert it: it converts it as well, to be
            // compared.
            if (GpuCheckWanted(Info, Total) && RecordGpuConvert(Info, MipCount, bMasked, Layouts, Total, Cmd))
            {
                GpuCheckRecord(Info, MipCount, bMasked, Layouts, Total, Scratch, Cmd);
                GpuTransition(Cmd, GpuOut[CurrentFrame], D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
    }

    for (INT m = 0; m < MipCount && bConverted && !bDone; m++)
    {
        const FMipmapBase* Mip   = Info.Mips[m];
        BYTE*              Dst   = Mapped + Layouts[m].Offset;
        const INT          Pitch = (INT)Layouts[m].Footprint.RowPitch;
        if (bCompressed)
        {
            // Straight block copy, row by row: the destination rows are padded to D3D12's
            // 256-byte boundary and the source rows are not.
            const INT SrcRow = ((Mip->USize + 3) / 4) * 8;
            const INT Bytes  = ((INT)RowBytes[m] < SrcRow) ? (INT)RowBytes[m] : SrcRow;
            const DOUBLE K0 = appSeconds();
            for (UINT y = 0; y < Rows[m]; y++)
                appMemcpy(Dst + y * Pitch, Mip->DataPtr + y * SrcRow, Bytes);
            FrameCopySec += appSeconds() - K0;
        }
        else
        {
            // A light map is prepared as data, never as colour - the same flag that keeps it
            // out of the sRGB conversion also tells the expansion which channel order to use.
            // NOT straight into the mapped upload buffer. That memory is write-combined:
            // writes to it bypass the cache and READS from it are uncached, and the edge
            // dilation reads neighbours back. Measured: unpacking a level's textures took
            // 27.5 of 27.6 seconds of precache time this way. Expanding into ordinary memory
            // and copying the result across leaves the reads cached and the writes sequential.
            const INT Needed = Pitch * (INT)Rows[m];
            if (EnsureScratch((UINT64)Needed))
            {
                const DOUBLE V0 = appSeconds();
                bConverted = Convert(Info, m, bMasked, !bSrgb, bKeepAlpha, Scratch, Pitch);
                const DOUBLE V1 = appSeconds();
                FrameConvertSec += V1 - V0;
                if (bConverted)
                    appMemcpy(Dst, Scratch, Needed);
                FrameCopySec += appSeconds() - V1;
            }
            else
            {
                const DOUBLE V0 = appSeconds();
                bConverted = Convert(Info, m, bMasked, !bSrgb, bKeepAlpha, Dst, Pitch);
                FrameConvertSec += appSeconds() - V0;
            }
        }
    }
    if (!bConverted)
    {
        // Ring and spill memory is simply left unused until the slot is emptied.
        return 0;
    }

    for (INT m = 0; m < MipCount; m++)
    {
        D3D12_TEXTURE_COPY_LOCATION Dst;
        appMemzero(&Dst, sizeof(Dst));
        Dst.pResource        = Texture;
        Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Dst.SubresourceIndex = (UINT)m;

        D3D12_TEXTURE_COPY_LOCATION Src;
        appMemzero(&Src, sizeof(Src));
        Src.pResource              = Source;
        Src.Type                   = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint        = Layouts[m];
        Src.PlacedFootprint.Offset += Offset;

        Cmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
    }

    // Ring and spill memory are still referenced by the command list; both are reclaimed with
    // the slot, once this frame's fence has passed (ReleaseFrameStaging).
    return 1;
}

ID3D12Resource* FTextureCache::CreateTexture(D3D12_RESOURCE_DESC Desc)
{
    // A measurement only (RdxForcedFailure): the textures are what a level fills a card WITH, so a card running out
    // of memory refuses one of these long before it refuses anything else. All three ways of making one are below,
    // and the caller handles a null - what that looks like on screen is the thing worth seeing.
    if (RdxForcedFailure(TEXT("texture")))
        return nullptr;
    // Small textures - most light maps - may sit at 4 KB alignment instead of 64 KB. D3D12's
    // rule, as the debug layer quotes it (warning #1380) when a bigger one asks: the most
    // detailed level fits in 16 tiles of 4 KB - 32x32 texels of a 32-bit format, 128x64 of
    // BC1, the two formats this cache makes. Only such a texture asks; the device then says
    // whether it qualifies, and if not, the default alignment it is.
    const UBOOL  bBc1   = Desc.Format == DXGI_FORMAT_BC1_UNORM || Desc.Format == DXGI_FORMAT_BC1_UNORM_SRGB;
    const UINT64 TileW  = bBc1 ? 128 : 32;
    const UINT64 TileH  = bBc1 ? 64 : 32;
    const UBOOL  bSmall = ((Desc.Width + TileW - 1) / TileW) * ((Desc.Height + TileH - 1) / TileH) <= 16;
    D3D12_RESOURCE_ALLOCATION_INFO A;
    A.SizeInBytes = ~(UINT64)0;
    A.Alignment   = 0;
    if (bSmall)
    {
        Desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
        A = Device->GetResourceAllocationInfo(0, 1, &Desc);
    }
    if (A.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT)
    {
        Desc.Alignment = 0;
        A = Device->GetResourceAllocationInfo(0, 1, &Desc);
    }

    if (A.SizeInBytes != ~(UINT64)0 && A.SizeInBytes <= (UINT64)TEX_HEAP_SIZE && A.Alignment > 0)
    {
        UINT64 Offset = (TexHeapUsed + A.Alignment - 1) & ~(A.Alignment - 1);
        if (TexHeapCurrent < 0 || Offset + A.SizeInBytes > (UINT64)TEX_HEAP_SIZE)
        {
            // On to the next heap: one kept from an earlier level, or a new one.
            if (TexHeapCurrent + 1 >= TexHeapCount && TexHeapCount < MAX_TEX_HEAPS)
            {
                D3D12_HEAP_DESC HeapDesc;
                appMemzero(&HeapDesc, sizeof(HeapDesc));
                HeapDesc.SizeInBytes     = TEX_HEAP_SIZE;
                HeapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                HeapDesc.Alignment       = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                HeapDesc.Flags           = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
                if (SUCCEEDED(Device->CreateHeap(&HeapDesc, __uuidof(ID3D12Heap), (void**)&TexHeaps[TexHeapCount])))
                    TexHeapCount++;
            }
            if (TexHeapCurrent + 1 < TexHeapCount)
            {
                TexHeapCurrent++;
                TexHeapUsed = 0;
                Offset      = 0;
            }
            else
                Offset = ~(UINT64)0;    // no heap left: fall through to a committed resource
        }
        if (Offset != ~(UINT64)0)
        {
            ID3D12Resource* Placed = nullptr;
            if (SUCCEEDED(Device->CreatePlacedResource(TexHeaps[TexHeapCurrent], Offset, &Desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       __uuidof(ID3D12Resource), (void**)&Placed)))
            {
                TexHeapUsed = Offset + A.SizeInBytes;
                return Placed;
            }
        }
    }

    // Too big for a heap, or out of heaps: a resource of its own, as before.
    D3D12_HEAP_PROPERTIES DefaultHeap;
    appMemzero(&DefaultHeap, sizeof(DefaultHeap));
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Desc.Alignment = 0;
    ID3D12Resource* Own = nullptr;
    if (FAILED(Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               __uuidof(ID3D12Resource), (void**)&Own)))
        return nullptr;
    return Own;
}

INT FTextureCache::Upload(const FTextureInfo& Info, UBOOL bMasked, UBOOL bSrgb, ID3D12GraphicsCommandList* Cmd,
                          UBOOL bKeepAlpha)
{
    static INT NoRoom = 0, BadBlock = 0;
    if (Count >= TEXTURE_SLOTS)
    {
        if (++NoRoom == 1) RdxLog(TEXT("texture refused: the cache is full (%i)"), Count);
        return -1;
    }
    if (!CanRecordCopy(Info))
        return -1;
    const DOUBLE T0 = appSeconds();

    const FMipmapBase* Mip = Info.Mips[0];
    const INT W = Mip->USize;
    const INT H = Mip->VSize;

    // Every level the engine has, not just the top one. A texture drawn smaller than its
    // top level with no mips below it shimmers - and with the compressed high-resolution
    // textures (up to 2048x2048) almost everything is drawn smaller than its top level.
    const INT MipCount = UsableMips(Info);
    if (MipCount <= 0)
        return -1;

    // DXT1 is block compressed and the GPU reads it as-is. Expanding it to RGBA would
    // cost eight times the memory and lose nothing but time, so it goes up untouched.
    // Everything else we expand to RGBA8 during the copy.
    const UBOOL bCompressed = (Info.Format == TEXF_DXT1);
    if (bCompressed && ((W & 3) || (H & 3)))
    {
        // BC1's top level has to be whole 4x4 blocks.
        if (++BadBlock <= 4)
            RdxLog(TEXT("DXT1 texture is not a multiple of 4 (%ix%i) - skipped"), W, H);
        return -1;
    }
    const DXGI_FORMAT Fmt = bCompressed ? DXGI_FORMAT_BC1_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
#if RDX_CLASSIC
    // Classic: sampled as the raw numbers, never decoded - see RDX_CLASSIC.
    const DXGI_FORMAT ViewFmt = Fmt;
#else
    const DXGI_FORMAT ViewFmt = bSrgb
        ? (bCompressed ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        : Fmt;
#endif

    // Destination on the GPU.
    D3D12_HEAP_PROPERTIES DefaultHeap;
    appMemzero(&DefaultHeap, sizeof(DefaultHeap));
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC TexDesc;
    appMemzero(&TexDesc, sizeof(TexDesc));
    TexDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    TexDesc.Width            = W;
    TexDesc.Height           = H;
    TexDesc.DepthOrArraySize = 1;
    TexDesc.MipLevels        = (UINT16)MipCount;
    TexDesc.Format           = Fmt;
    TexDesc.SampleDesc.Count = 1;
    const DOUBLE C0 = appSeconds();

    // Asked and refused already this frame: memory does not free up between two draws of the same frame, so the
    // answer would be the same. The next frame asks again - a level change or a texture going out of the cache may
    // well have made room by then.
    if (NoMemoryFrame == FrameSerial)
        return -1;
    ID3D12Resource* Texture = CreateTexture(TexDesc);
    FrameCreateSec += appSeconds() - C0;
    if (!Texture)
    {
        NoMemoryFrame = FrameSerial;
        // The card would not give us room for it. Said once, and then once in every ten thousand: the surface draws
        // untextured from here, and a player looking at a grey wall deserves a line that says why. Walked with the
        // create forced to fail: without this the log said NOTHING, while the renderer asked again for every texture
        // of every frame - 1.7 million times in 55 seconds.
        static INT NoMemory = 0;
        if (++NoMemory == 1 || (NoMemory % 10000) == 0)
            RdxLog(TEXT("texture refused: no room on the card for %ix%i (asked %i times so far) - the surface draws "
                        "untextured"), (INT)TexDesc.Width, (INT)TexDesc.Height, NoMemory);
        return -1;
    }

    if (!RecordCopy(Info, MipCount, bMasked, bSrgb, Texture, Cmd, bKeepAlpha))
    {
        Texture->Release();
        return -1;
    }
    Transition(Cmd, Texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc;
    appMemzero(&SrvDesc, sizeof(SrvDesc));
    SrvDesc.Format                  = ViewFmt;
    SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SrvDesc.Texture2D.MipLevels     = (UINT)MipCount;

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = Heap->GetCPUDescriptorHandleForHeapStart();
    CpuHandle.ptr += (SIZE_T)Count * HeapStride;
    Device->CreateShaderResourceView(Texture, &SrvDesc, CpuHandle);

    FEntry& E = Entries[Count];
    if (E.Kept)
        appFree(E.Kept);
    E.Kept          = nullptr;
    E.KeptBytes     = 0;
    E.bSrgb         = bSrgb;
    E.bKeepAlpha    = bKeepAlpha;
    E.CacheId       = Info.CacheID;
    E.PaletteId     = Info.PaletteCacheID;
    E.Resource      = Texture;
    E.SrvIndex      = Count;
    E.bMasked       = bMasked;
    E.USize         = W;
    E.VSize         = H;
    E.Format        = (INT)Info.Format;
    E.MipCount      = MipCount;
    E.RefreshSerial = FrameSerial;
    HashInsert(Count);

    // Say so before it becomes visible. A full cache means light maps stop uploading and
    // surfaces silently render with no lighting at all.
    if (Count == TEXTURE_SLOTS * 9 / 10)
        RdxLog(TEXT("WARNING: the texture cache is 90%% full (%i / %i)"), Count, (INT)TEXTURE_SLOTS);

    // Which textures the engine animates is a fact about the game, not something to
    // assume - report the first few.
    static INT Realtime = 0;
    if (Info.bRealtime && ++Realtime <= 12)
        RdxLog(TEXT("real-time texture: %s %ix%i fmt %i"),
               Info.Texture ? Info.Texture->GetName() : TEXT("(with no texture object)"),
               W, H, (INT)Info.Format);

    // Diagnostic: the big ones by name. Measured: a 21.7 MB texture chain first seen while
    // turning cost 18-20 ms of a 23-25 ms frame, where the stock renderers stayed at 12 ms.
    // Whether the other variant (masked or not) is already in the cache says whether the
    // precache took the texture, only as the variant this draw does not use.
    const DOUBLE SizeBytes = (DOUBLE)W * H * (bCompressed ? 0.5 : 4.0) * 4.0 / 3.0;
    static INT Big = 0;
    if (SizeBytes > 4.0 * 1024.0 * 1024.0 && ++Big <= 80)
        RdxLog(TEXT("large upload: %s %ix%i fmt %i, masked %i, the other variant %s, %.1f MB, frame %u"),
               Info.Texture ? Info.Texture->GetName() : TEXT("(light map)"), W, H, (INT)Info.Format,
               bMasked ? 1 : 0,
               Find(Info.CacheID, Info.PaletteCacheID, bMasked ? 0 : 1, bSrgb) >= 0 ? TEXT("ALREADY IN") : TEXT("not in"),
               SizeBytes / (1024.0 * 1024.0), (DWORD)FrameSerial);

    FrameUploads++;
    if (!Info.Texture)
        FrameLightUploads++;
    FrameUploadBytes += SizeBytes;
    FrameUploadSec   += appSeconds() - T0;
    return Count++;
}

void FTextureCache::Refresh(INT Index, const FTextureInfo& Info, ID3D12GraphicsCommandList* Cmd)
{
    FEntry& E = Entries[Index];

    // Once per frame is enough: every draw of a frame is recorded after its uploads.
    if (E.RefreshSerial == FrameSerial)
        return;

    // Same resource, new content, so size, format and mip count must not have moved. For
    // one CacheID they never should; if they do, keep the old picture and say so rather
    // than write past the end of the texture.
    const FMipmapBase* Mip = Info.Mips[0];
    const INT MipCount = UsableMips(Info);
    if (!Mip || Mip->USize != E.USize || Mip->VSize != E.VSize || (INT)Info.Format != E.Format ||
        MipCount != E.MipCount)
    {
        static INT Mismatches = 0;
        if (++Mismatches <= 4)
            RdxLog(TEXT("real-time refresh skipped: %ix%i fmt %i %i mips instead of %ix%i fmt %i %i mips"),
                   E.USize, E.VSize, E.Format, E.MipCount, Mip ? Mip->USize : 0, Mip ? Mip->VSize : 0,
                   (INT)Info.Format, MipCount);
        E.RefreshSerial = FrameSerial;
        return;
    }
    // A light or fog map the engine built again just as it was: the texture holds those very texels - nothing to
    // record. Its one level's bytes are all there is to it; the flags it goes up with are the entry's own.
    const UBOOL bKeepable = !Info.Texture && MipCount == 1 && Info.Format == TEXF_RGBA7 && Mip->DataPtr;
    const INT   Bytes     = bKeepable ? Mip->USize * Mip->VSize * 4 : 0;
    if (bKeepable && E.Kept && E.KeptBytes == Bytes && memcmp(E.Kept, Mip->DataPtr, Bytes) == 0)
    {
        E.RefreshSerial = FrameSerial;
        FrameRefreshesKept++;
        return;
    }
    if (!CanRecordCopy(Info))
        return;

    // The new texels go into the very resource the draws sample. The barrier makes the
    // GPU finish every earlier read of it first - including earlier frames still running,
    // which were submitted to the same queue before this one.
    const DOUBLE T0 = appSeconds();
    Transition(Cmd, E.Resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    const UBOOL bOk = RecordCopy(Info, E.MipCount, E.bMasked, E.bSrgb, E.Resource, Cmd, E.bKeepAlpha);
    Transition(Cmd, E.Resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    E.RefreshSerial = FrameSerial;
    {
        const DOUBLE Took = appSeconds() - T0;
        FrameRefreshes++;
        FrameRefreshSec += Took;
        if (!Info.Texture)
        {
            FrameMapRefreshes++;
            FrameMapRefreshSec += Took;
        }
    }

    // What went up is kept, to be told from the next build. A copy not recorded left the texture as it was, and what
    // was kept with it.
    if (bOk && bKeepable)
    {
        if (E.KeptBytes != Bytes)
        {
            if (E.Kept)
                appFree(E.Kept);
            E.Kept      = (BYTE*)appMalloc(Bytes, TEXT("RdxKeptMap"));
            E.KeptBytes = E.Kept ? Bytes : 0;
        }
        if (E.Kept)
            appMemcpy(E.Kept, Mip->DataPtr, Bytes);
    }

    if (bOk)
    {
        RefreshCount++;
        static INT Noted = 0;
        if (++Noted <= 24)
            RdxLog(TEXT("real-time refresh: %s %ix%i fmt %i, CacheID %08X%08X"),
                   Info.Texture ? Info.Texture->GetName() : TEXT("(with no texture object)"),
                   E.USize, E.VSize, E.Format, (DWORD)(Info.CacheID >> 32), (DWORD)Info.CacheID);
    }
}

INT FTextureCache::Prepare(const FTextureInfo& Info, DWORD PolyFlags, UBOOL bSrgb, ID3D12GraphicsCommandList* Cmd,
                           UBOOL bKeepAlpha)
{
    if (!Heap || !Cmd)
        return -1;

    // Masking changes the pixels we upload, so a masked and an unmasked use of the
    // same texture are two different GPU textures.
    const UBOOL bMasked = (PolyFlags & PF_Masked) ? 1 : 0;

    // The same texture as the last call's skips the hash - see LastEntry.
    INT Existing;
    if (LastEntry >= 0 && Info.CacheID == LastCacheId && Info.PaletteCacheID == LastPaletteId &&
        bMasked == LastMasked && bSrgb == LastSrgb)
        Existing = LastEntry;
    else
    {
        Existing = Find(Info.CacheID, Info.PaletteCacheID, bMasked, bSrgb);
        if (Existing < 0)
            return Upload(Info, bMasked, bSrgb, Cmd, bKeepAlpha);
        LastCacheId   = Info.CacheID;
        LastPaletteId = Info.PaletteCacheID;
        LastMasked    = bMasked;
        LastSrgb      = bSrgb;
        LastEntry     = Existing;
    }

    // The engine redraws some textures under the SAME CacheID and says so with bRealtimeChanged - "changed since
    // last render" (UnTex.h): a light map under a moving dynamic light, water, fire, animated screens. The flag has
    // to be honoured or the cache serves the first version of that CacheID for the rest of the level - a flashlight
    // then lights the one spot it first lit, wherever the player turns.
    if (Info.bRealtimeChanged)
    {
        // Every variant of this texture in the cache - masked or not, colour or data - is
        // out of date now, not only the one this draw asked for.
        for (INT m = 0; m < 2; m++)
            for (INT s = 0; s < 2; s++)
            {
                const INT Variant = (m == (INT)bMasked && s == (INT)bSrgb)
                                  ? Existing : Find(Info.CacheID, Info.PaletteCacheID, m, s);
                if (Variant >= 0)
                    Refresh(Variant, Info, Cmd);
            }

        // "Since last render": once drawn, it is no longer changed. The flag lives on the
        // texture object. A light map has no such object, and the engine sets the flag on
        // its info afresh whenever it rebuilds one.
        if (Info.Texture)
            Info.Texture->bRealtimeChanged = 0;
    }
    return Entries[Existing].SrvIndex;
}

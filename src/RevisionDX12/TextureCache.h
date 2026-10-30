// SecondRevision - texture upload and reuse.
//
// Measured facts this is built on (docs/meres-01 section 1.5, docs/spec-02 section 2):
//   - 99.9% of textures the engine hands us are TEXF_P8: 8-bit palettised, which no
//     modern GPU samples natively. Converting to RGBA8 on upload is the whole job.
//   - FTextureInfo::CacheID is the identity, not the pointer: the same texture arrives
//     through many different FTextureInfo instances.
//   - bRealtime / bRealtimeChanged mark textures that animate; those must be re-uploaded
//     or animated screens and water freeze.
#pragma once

#include "GpuConvert.h"

// One fork of FTextureCache::ParallelFor: indices 0..Count-1 handed out one at a time, to the
// thread pool's callbacks and to the calling thread alike.
struct FParallelJob
{
    volatile LONG Next;
    LONG          Count;
    void        (*Body)(void* Ctx, INT Index);
    void*         Ctx;
};

class FTextureCache
{
public:
    FTextureCache();
    ~FTextureCache();

    UBOOL Init(ID3D12Device* InDevice);
    void  Shutdown();

    // Drops every cached texture. Only safe once the GPU has finished with them, so the
    // backend waits first - see FBackend::FlushTextures.
    void  ReleaseAll();

    // Returns the shader-visible descriptor index for this texture, uploading it on
    // first sight. Returns -1 if the texture cannot be represented, in which case the
    // caller should draw untextured rather than draw nothing.
    // bSrgb: whether the texture holds colour (authored in sRGB, so sampling must
    // linearise it) or data (light maps are already light values and must not be
    // converted). Getting this wrong is invisible in the bright parts of a scene and
    // glaring in the dark ones - measured: our shadows came out six times too bright.
    // bKeepAlpha: a fog map - the engine's volumetric light on a surface - whose alpha is how much of the surface the fog
    // covers; a light map's alpha is nothing and is made opaque. The same CacheID always comes with the same answer.
    INT Prepare(const FTextureInfo& Info, DWORD PolyFlags, UBOOL bSrgb, ID3D12GraphicsCommandList* Cmd,
                UBOOL bKeepAlpha = 0);

    // Whether the texture Info names - as Prepare is given it - was written in this frame: uploaded, or refreshed for
    // its bRealtimeChanged. A refresh can be put off, a frame's uploads have their limits; it is asked for again then.
    UBOOL WrittenThisFrame(const FTextureInfo& Info, DWORD PolyFlags, UBOOL bSrgb) const;

    // The descriptor index of a colour texture the cache already holds, by its identity alone: nothing is uploaded
    // and nothing refreshed. The plain version, or the masked one where only that was ever drawn; -1 where it holds
    // neither. For the mirror rays, which meet a texture where no draw brought it.
    // bMasked: the masked version alone - the one whose clear texels carry no alpha. bSrgb 0: the version read as
    // numbers, not as colour - a modulated surface's multiplier.
    INT FindColour(QWORD CacheId, QWORD PaletteId, UBOOL bMasked = 0, UBOOL bSrgb = 1) const;

    // Staging buffers must outlive the copy, so they are released only once the GPU
    // has passed the frame that used them.
    void ReleaseFrameStaging(INT FrameIndex);

    ID3D12DescriptorHeap* GetHeap() const { return Heap; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(INT Index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(INT Index) const;
    INT GetReservedIndex(INT K) const { return TEXTURE_SLOTS + K; }     // see RESERVED_VIEWS
    // How many of them there are: the backend's last view has to be one of them.
    static constexpr INT ReservedViews() { return RESERVED_VIEWS; }
    // The relief's own height and colour maps (FPomLibrary), after the reserved views. They are not the game's
    // textures and do not come and go with a level, so they live outside the cache's own slots.
    INT GetPomIndex(INT K) const { return TEXTURE_SLOTS + RESERVED_VIEWS + K; }
    INT GetPomViews() const { return POM_VIEWS; }

    INT GetCount() const { return Count; }

    // Precaching offers a whole level between frames, and every upload holds a staging
    // buffer until the GPU has copied it. Past the limit uploads are refused and the
    // texture is left to be uploaded later, mid-play - which is felt as a stutter. The
    // backend uses this to flush and recycle before that happens.
    INT  GetStagingCount() const { return StagingCount[CurrentFrame]; }
    INT  GetStagingLimit() const { return MAX_STAGING_PER_FRAME; }

    // The precache flushes when this says so, which lets it reuse the ring instead of
    // falling back to a buffer per texture once the ring has filled.
    UBOOL RingNearlyFull() const { return RingUsed[CurrentFrame] > (UINT64)RING_SIZE / 8 * 7; }

    // Releases the staging buffers of the frame slot the cache is CURRENTLY filling -
    // not of some other slot. Reading one index and clearing another is why the first
    // attempt at this changed nothing.
    void ReleaseCurrentStaging() { ReleaseFrameStaging(CurrentFrame); }

    // A 1x1 white texture, so geometry without a light map can bind something and the
    // shader can multiply unconditionally. Created on first use, because uploading
    // needs a command list.
    INT  GetWhiteIndex() const { return WhiteIndex; }
    void EnsureWhite(ID3D12GraphicsCommandList* Cmd);

    // Diagnostic (validation mode): each big texture converted side by side is converted
    // again the plain way, and the two compared - see CheckParallel.
    void SetCheckParallel(UBOOL bOn) { bCheckParallel = bOn ? 1 : 0; }

    // Big palettised textures converted on the GPU instead of the CPU (RecordGpuConvert) - the
    // config's GpuTextureConvert. Off, or where the GPU path cannot run, the CPU converts.
    void SetGpuConvert(UBOOL bOn) { bGpuConvert = bOn ? 1 : 0; }

private:
    // MAX_STAGING_PER_FRAME bounds how many uploads one frame can record. 256 was
    // plenty while textures arrived a few at a time; precaching offers a whole level at
    // once (measured: 395 textures in the test scene), and anything over the limit is
    // simply refused and left to be uploaded later - which is exactly the stutter
    // precaching exists to remove.
    // 4096 was not enough. Every BSP surface a level has carries its OWN light map, so
    // walking around a map keeps adding entries; precaching alone spends 1750. Once the
    // cache filled, light maps stopped uploading and every new surface bound the neutral
    // map instead - which renders it with NO lighting at all. Measured symptom: flat
    // bright green grass and flat brown walls after walking for a while.
    //
    // The hash table must stay a power of two and comfortably larger than MAX_TEXTURES,
    // so lookups keep finding empty slots quickly.
    enum { MAX_TEXTURES = 36864, MAX_STAGING_PER_FRAME = 1024, HASH_SIZE = 131072 };
    // The last RESERVED_VIEWS descriptors of the heap are not the cache's own: the backend writes views of its targets
    // there (the water's two sets of seven - FBackend's WaterView* - the splashes' three, SplashView*, the RT water's
    // three: the shadow masks, the far probe and the probe's UAV, RtMaskView... - the relief's covers with the view of
    // nothing read in their place, ReliefCoverView and ReliefCoverNone - the see-through water's own depth as the water
    // writes it, WaterSurfaceUavView - the caustics' mask, CausticMaskView - and the map of the mirrors' views'
    // depth for the flashlight's light off them, BounceMapView - the rays' list's four, the own lamps' table and
    // maps, OwnLampTableView and OwnLampMapsView, the water's shade, WaterShadeUav, the mirror probe's passes'
    // nine, ProbeTableView, Reflections=RTLow's own list, RayAskedLowUav, the rays' scene's corners, RayCornersView's
    // three, the wet floors' mark as written and as read, WetMarkUav and WetMarkSrv, and the rain's cover over them,
    // WetCoverSrv, and the volumetric walk's picture the wet floors' flames are read from, WetFlameSrv), because a draw
    // sees only the one
    // shader-visible heap that is bound - this one, for the whole geometry pass. A level change never touches them.
    // POM_VIEWS follow them: two to a height map (the height itself and the colour that goes with it), so the relief's
    // maps are addressable from the same bound heap without taking slots a level's textures need. 2048 maps
    // (FPomLibrary's MAX_ENTRIES), and five more: the shader's table reads seven from a map's first (the water's
    // table, FSolidPass::MakeRoot), so the last map's table reaches five past its own.
    enum { RESERVED_VIEWS = 49, POM_VIEWS = 2 * 2048 + 5,
           TEXTURE_SLOTS = MAX_TEXTURES - RESERVED_VIEWS - POM_VIEWS };

    struct FEntry
    {
        QWORD           CacheId;
        QWORD           PaletteId;
        ID3D12Resource* Resource;
        INT             SrvIndex;
        UBOOL           bMasked;      // masked textures need their own upload
        UBOOL           bSrgb;
        UBOOL           bKeepAlpha;   // a fog map's: its alpha kept, for every refresh as for the first upload
        // What the resource was created as. A realtime texture is refreshed in place, so
        // new content has to fit it exactly.
        INT             USize;
        INT             VSize;
        INT             Format;
        INT             MipCount;
        DWORD           RefreshSerial; // the frame whose content it holds
        // A light or fog map's texels as it last went up in a refresh (Refresh): the engine marks such a map changed
        // whenever it builds it again, and most come out as they were. Null till then; KeptBytes of them.
        BYTE*           Kept;
        INT             KeptBytes;
    };

    INT Find(QWORD CacheId, QWORD PaletteId, UBOOL bMasked, UBOOL bSrgb) const;

    // The last Find that hit, so that the next call for the same texture skips the hash: a
    // mesh's triangles arrive one draw call each, in runs that share a texture (measured: 8500
    // gouraud calls a frame in Liberty). Entry indices only change with ReleaseAll, which
    // forgets it, as does Shutdown.
    QWORD  LastCacheId;
    QWORD  LastPaletteId;
    UBOOL  LastMasked;
    UBOOL  LastSrgb;
    INT    LastEntry;
    INT Upload(const FTextureInfo& Info, UBOOL bMasked, UBOOL bSrgb, ID3D12GraphicsCommandList* Cmd,
               UBOOL bKeepAlpha);

    // The shared half of an upload and a refresh: checks there is room and data, then
    // fills a staging buffer from the top mip and records its copy into Texture, which
    // must be in COPY_DEST.
    UBOOL CanRecordCopy(const FTextureInfo& Info);
    UBOOL RecordCopy(const FTextureInfo& Info, INT MipCount, UBOOL bMasked, UBOOL bSrgb,
                     ID3D12Resource* Texture, ID3D12GraphicsCommandList* Cmd, UBOOL bKeepAlpha);

    // A stretch of this frame slot's upload ring, or false if it does not fit.
    UBOOL RingAlloc(UINT64 Size, ID3D12Resource*& Buffer, UINT64& Offset, BYTE*& Cpu);

    // New content for a texture already on the GPU - see Prepare.
    void  Refresh(INT Index, const FTextureInfo& Info, ID3D12GraphicsCommandList* Cmd);

    // Expands one mip of a palettised or true-colour texture into RGBA8.
    // Returns false for formats we do not handle yet.
    UBOOL Convert(const FTextureInfo& Info, INT MipIndex, UBOOL bMasked, UBOOL bLightData, UBOOL bKeepAlpha,
                  BYTE* Dest, INT DestPitch) const;

    ID3D12Device*         Device;
    ID3D12DescriptorHeap* Heap;
    UINT                  HeapStride;

    // Textures live in large pre-allocated heaps as placed resources, one after another.
    // Measured: CreateCommittedResource cost about 0.47 ms per texture, and turning onto new
    // surfaces brings 50-100 new light maps in one frame (every BSP surface has its own):
    // 24-46 ms a frame. The cache only ever releases everything at once (ReleaseAll), so a
    // bump allocator is all the bookkeeping needed: ReleaseAll rewinds it, and the heaps are
    // kept for the next level.
    enum { TEX_HEAP_SIZE = 64 * 1024 * 1024, MAX_TEX_HEAPS = 64 };
    ID3D12Heap*     TexHeaps[MAX_TEX_HEAPS];
    INT             TexHeapCount;     // heaps created so far
    INT             TexHeapCurrent;   // the one being filled, -1 before the first texture
    UINT64          TexHeapUsed;      // bytes used in it
    // Placed in a heap if it fits one, committed otherwise. Created in COPY_DEST.
    ID3D12Resource* CreateTexture(D3D12_RESOURCE_DESC Desc);

    FEntry Entries[MAX_TEXTURES];
    INT    Count;

    // A linear scan was fine at 4096 entries; at 32768, with hundreds of draws per frame,
    // it is not. Open addressing, entry index + 1, zero means empty.
    INT    Hash[HASH_SIZE];
    static INT HashOf(QWORD CacheId, QWORD PaletteId, UBOOL bMasked, UBOOL bSrgb);
    void   HashInsert(INT EntryIndex);
    void   HashClear();

    ID3D12Resource* Staging[BACKEND_FRAME_COUNT][MAX_STAGING_PER_FRAME];
    INT             StagingCount[BACKEND_FRAME_COUNT];
    INT             CurrentFrame;
    INT             WhiteIndex;

    // Counts frames, so that a realtime texture is refreshed at most once per frame
    // however many draws use it.
    DWORD           FrameSerial;
    // The frame in which the card last refused a texture. Memory does not free up in the middle of a frame, so once
    // one create has been refused the rest of that frame would be refused too - and the cache asks per DRAW. Measured
    // with the create forced to fail: 1.7 million asks in 55 seconds. This makes it one an ask a frame.
    DWORD           NoMemoryFrame = ~(DWORD)0;
    INT             RefreshCount;
    // Diagnostic, per frame (reset in SetFrame): new textures created this frame, how many
    // of them light maps (no texture object), roughly how many bytes, and how long they took
    // - in all, and in CreateCommittedResource alone. Measured: 130-140 ms hitches while
    // turning, CPU-bound, which the stock renderer does not have.
    INT             FrameUploads;
    INT             FrameLightUploads;
    DOUBLE          FrameUploadBytes;
    DOUBLE          FrameUploadSec;
    DOUBLE          FrameCreateSec;
    INT             FrameOwnBufs;       // uploads too big for the frame's ring: a buffer of their own
    DOUBLE          FrameOwnBufSec;     // ... and creating and mapping that buffer
    DOUBLE          FrameConvertSec;    // expanding P8/RGBA7 to RGBA8, dilation included
    mutable DOUBLE  FrameDilateSec;     // the masked-edge dilation alone (Convert is const)
    DOUBLE          FrameCopySec;       // copying into upload memory
    // Diagnostic, per frame (reset in SetFrame): the textures the engine marked as changed and that were recorded
    // again (Refresh) - all of them, and those with no texture object, the light maps and the fog maps - with the
    // time the recording took on this thread.
    INT             FrameRefreshes     = 0;
    INT             FrameMapRefreshes  = 0;
    DOUBLE          FrameRefreshSec    = 0.0;
    DOUBLE          FrameMapRefreshSec = 0.0;
    INT             FrameRefreshesKept = 0;     // refreshes the texture already held (FEntry::Kept)

    // Upload memory: one persistently mapped buffer per frame slot, handed out front to
    // back and emptied once the GPU has finished with that slot. A committed buffer per
    // upload - created now, released two frames later - cost about 0.44 ms apiece.
    // Measured: the 16 light maps UNATCO's flickering lights rebuild every frame took
    // 7.1 ms of each frame and the frame rate fell from 198 to 71; with the flashlight on,
    // 19 ms and 32 FPS. What does not fit falls back to a buffer of its own - a whole
    // level's precache does not fit, and that is where the fallback is for.
    enum { RING_SIZE = 32 * 1024 * 1024 };
    ID3D12Resource* Ring[BACKEND_FRAME_COUNT];
    BYTE*           RingCpu[BACKEND_FRAME_COUNT];
    UINT64          RingUsed[BACKEND_FRAME_COUNT];

    // Upload memory past the ring: chunks of SPILL_CHUNK (or one texture's size, if that is
    // larger), handed out front to back like the ring and released with the slot. A committed
    // buffer per texture cost about 0.44 ms apiece - measured: the first frame after loading
    // Liberty made 212 of them, 92.5 ms.
    enum { SPILL_CHUNK = 16 * 1024 * 1024, MAX_SPILL = 64 };
    ID3D12Resource* SpillChunk[BACKEND_FRAME_COUNT][MAX_SPILL];
    INT             SpillCount[BACKEND_FRAME_COUNT];
    BYTE*           SpillCpu[BACKEND_FRAME_COUNT];     // the newest chunk's mapping
    UINT64          SpillUsed[BACKEND_FRAME_COUNT];    // ... bytes used in it
    UINT64          SpillSize[BACKEND_FRAME_COUNT];    // ... and its size
    UBOOL SpillAlloc(UINT64 Size, ID3D12Resource*& Buffer, UINT64& Offset, BYTE*& Cpu);

    // Ordinary (cached) memory to unpack into before the copy to the upload heap.
    // Reused and grown as needed - one allocation, not one per texture.
    BYTE*           Scratch;
    INT             ScratchSize;
    UBOOL EnsureScratch(UINT64 Bytes);

    // The levels of a big palettised texture are expanded in bands and dilated side by side,
    // on the process's thread pool - see ConvertLevelsParallel. One fork at a time, and only
    // ever from the engine's thread.
    enum { PARALLEL_MIN_TEXELS = 256 * 256, MAX_BANDS = 512 };
    PTP_WORK        Pool;
    FParallelJob    Job;
    INT             PoolHelpers;       // pool callbacks one fork asks for, at most
    void  ParallelFor(INT N, void (*Body)(void* Ctx, INT Index), void* Ctx);
    UBOOL ConvertLevelsParallel(const FTextureInfo& Info, INT MipCount, UBOOL bMasked,
                                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Layouts, BYTE* Out);
    // The dilation's work memory for those levels, taken here, on the engine's thread: the
    // engine's allocator is not thread-safe (see DilateTransparentEdges).
    unsigned int*   DilateWork;
    long            DilateWorkHave;    // in words
    UBOOL           bCheckParallel;
    INT             ParallelChecks;
    INT             ParallelBad;
    void  CheckParallel(const FTextureInfo& Info, INT MipCount, UBOOL bMasked, UBOOL bSrgb, UBOOL bKeepAlpha,
                        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Layouts, const BYTE* Got);

    // The conversion on the GPU - see GpuConvert.h and docs/terv-gpu-atalakitas.md. From
    // GPU_MIN_TEXELS up: the side-by-side conversion's threshold, until measured otherwise.
    FGpuConvert     Gpu;
    UBOOL           bGpuConvert;
    // GPU_SCRATCH_MIN: the size the GPU's buffers start at - a 2048x2048 texture with its chain,
    // 22.4 MB, fits. They are first made with a level's precache, where it is not felt; grown
    // later, they would grow mid-play, with a new big texture. Measured: a texture that made
    // them grow took 1.7-2.6 ms of CPU, the next ones of its size 0.1-0.3 ms.
    enum { GPU_MIN_TEXELS = 256 * 256, GPU_SCRATCH_MIN = 24 * 1024 * 1024 };
    // Records the conversion of every level into this frame slot's GpuOut, laid out as Layouts
    // says - the upload's layout, so it copies into the texture the same way. Leaves GpuOut in
    // COPY_SOURCE. False if it cannot run; then nothing is recorded.
    UBOOL RecordGpuConvert(const FTextureInfo& Info, INT MipCount, UBOOL bMasked,
                           const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Layouts, UINT64 Total,
                           ID3D12GraphicsCommandList* Cmd);

    // Validation mode only (SetCheckParallel): the GPU's result is read back and, once the frame
    // slot's fence has passed (ReleaseFrameStaging), compared row by row with the CPU's -
    // whichever of the two filled the texture, the other converts it as well. Nothing of it runs
    // in ordinary play.
    // Held per frame slot: at most MAX_GPU_CHECKS, and past the first, GPU_CHECK_BUDGET bytes of
    // CPU copies - the process has 32-bit addresses, and a 2048x2048 texture with its chain is
    // 22 MB. The textures under 1024x1024 get MAX_GPU_CHECKS_TOTAL checks in all; the big ones
    // are always checked, and may take a slot's last four places and twice the budget.
    enum { MAX_GPU_CHECKS = 32, MAX_GPU_CHECKS_TOTAL = 300, MAX_GPU_OLD = 16,
           GPU_CHECK_BUDGET = 48 * 1024 * 1024 };
    struct FGpuCheck
    {
        ID3D12Resource* Readback;          // the GPU result, in the upload layout
        BYTE*           Reference;         // the CPU result, the same layout (appMalloc)
        UINT64          Bytes;
        INT             MipCount;
        INT             W[16], H[16];
        UINT64          Offset[16];
        UINT            Pitch[16];
        UBOOL           bMasked;
        TCHAR           Name[64];
    };
    FGpuCheck       GpuChecks[BACKEND_FRAME_COUNT][MAX_GPU_CHECKS];
    INT             GpuCheckCount[BACKEND_FRAME_COUNT];
    UINT64          GpuCheckBytes[BACKEND_FRAME_COUNT];
    INT             GpuChecksDone;
    INT             GpuChecksBad;
    INT             GpuSmallChecks;    // checks of textures under 1024x1024 recorded so far
    // The two buffers the GPU steps ping-pong between, per frame slot; outgrown ones stay alive
    // with their slot, since this frame's command list may still name them.
    ID3D12Resource* GpuOut[BACKEND_FRAME_COUNT];
    ID3D12Resource* GpuTemp[BACKEND_FRAME_COUNT];
    UINT64          GpuScratchSize[BACKEND_FRAME_COUNT];
    ID3D12Resource* GpuOld[BACKEND_FRAME_COUNT][MAX_GPU_OLD];
    INT             GpuOldCount[BACKEND_FRAME_COUNT];
    UBOOL EnsureGpuScratch(UINT64 Bytes);
    UBOOL CreateGpuScratch(INT Slot, UINT64 Size);
    UBOOL GpuCheckWanted(const FTextureInfo& Info, UINT64 Total) const;
    void  GpuCheckRecord(const FTextureInfo& Info, INT MipCount, UBOOL bMasked,
                         const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Layouts, UINT64 Total,
                         const BYTE* Reference, ID3D12GraphicsCommandList* Cmd);
    void  GpuCompareFinish(INT Slot);

public:
    void SetFrame(INT FrameIndex)
    {
        CurrentFrame = FrameIndex;
        FrameSerial++;
        FrameUploads = FrameLightUploads = 0;
        FrameUploadBytes = FrameUploadSec = FrameCreateSec = 0.0;
        FrameOwnBufs = 0;
        FrameOwnBufSec = FrameConvertSec = FrameDilateSec = FrameCopySec = 0.0;
        FrameRefreshes = FrameMapRefreshes = 0;
        FrameRefreshSec = FrameMapRefreshSec = 0.0;
        FrameRefreshesKept = 0;
    }

    // Realtime refreshes recorded so far - see Prepare. Reported in the frame census.
    INT  GetRefreshCount() const { return RefreshCount; }
    INT    GetFrameUploads() const      { return FrameUploads; }
    INT    GetFrameLightUploads() const { return FrameLightUploads; }
    DOUBLE GetFrameUploadMB() const     { return FrameUploadBytes / (1024.0 * 1024.0); }
    DOUBLE GetFrameUploadMs() const     { return FrameUploadSec * 1000.0; }
    DOUBLE GetFrameCreateMs() const     { return FrameCreateSec * 1000.0; }
    INT    GetFrameOwnBufs() const      { return FrameOwnBufs; }
    DOUBLE GetFrameOwnBufMs() const     { return FrameOwnBufSec * 1000.0; }
    DOUBLE GetFrameConvertMs() const    { return FrameConvertSec * 1000.0; }
    DOUBLE GetFrameDilateMs() const     { return FrameDilateSec * 1000.0; }
    DOUBLE GetFrameCopyMs() const       { return FrameCopySec * 1000.0; }
    INT    GetFrameRefreshes() const    { return FrameRefreshes; }
    INT    GetFrameMapRefreshes() const { return FrameMapRefreshes; }
    DOUBLE GetFrameRefreshSec() const   { return FrameRefreshSec; }
    DOUBLE GetFrameMapRefreshSec() const { return FrameMapRefreshSec; }
    INT    GetFrameRefreshesKept() const { return FrameRefreshesKept; }
};

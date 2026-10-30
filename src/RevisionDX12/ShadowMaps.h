// SecondRevision - shadow maps (docs/terv-latvany-roadmap.md 3.2).
//
// Every light casting shadows gets two cube maps of depths, drawn from the light. The level's own geometry goes into one,
// drawn when the light is chosen or moves; the actors - characters, decorations, the player - into the other, every frame.
// A surface is in an actor's shadow where no wall stands between it and the light, but an actor does: the level's own
// shadows are in its light maps already. The lookup that reads the maps is FFinalPass's shadow factor; which lights and
// which casters is RenderDevice.cpp's choice.
#pragma once

#include "RenderMath.h"
#include "CardWriter.h"

// One light casting shadows this frame, as the engine side hands it over.
struct FShadowLightFrame
{
    void* Key;           // the light across frames (its actor): a new key, or a light that moved, redraws its world map
    FLOAT World[3];      // where the maps are drawn from, world units
    FLOAT Cam[3];        // the same point in camera space (X right, Y down, Z forward) - the lookup's
    FLOAT Radius;        // reach, engine units
    FLOAT Bright;        // brightness, 0..1
    FLOAT Colour[3];     // the light's own hue, 0..1 each (FGetHSV of LightHue and LightSaturation)
    FLOAT Weight;        // the graduated pick's weight, 0..1 (RdxMath::ShadowLightWeight, followed over time)
    FLOAT Spot[4];       // a spot's axis in camera space and the cosine of its half angle; the cosine below -1: a point light
    // Casters this light must not see - its own mesh, a lamp's shade around it: a bit each, by the order they were added.
    // For the maps also every caster whose shadow from this light cannot reach the view (the reach cull), which saves
    // the draws. A word for every thirty-two casters of the frame (ShadowCasterWords); the caller's own until
    // FShadowMaps::SetLights, which keeps a copy.
    DWORD* Exclude;
    // Of those, only the light's own: its mesh and its shade (ShadowCasterExcluded). The RT shadows read this one - a ray
    // passes through these, and every caster it passes through costs it a question to the shader (FRtPass::ListScene),
    // where the reach cull would put nearly every caster of the frame. As long as Exclude.
    DWORD* Own;
};

// The words a light's bits take with Casters casters in the frame (FShadowLightFrame::Exclude and Own).
inline INT ShadowCasterWords(INT Casters)
{
    return (Casters + 31) / 32;
}

class FShadowMaps
{
public:
    FShadowMaps();
    ~FShadowMaps();

    // The two cube arrays for the preset (six faces a light), the depth-only pipeline, both frame slots' actor vertex and
    // world index buffers, and the lookup's constant buffers. False, logged, on failure: the renderer goes on without shadows.
    UBOOL Init(ID3D12Device* Device, const RdxMath::FShadowPreset& Preset);
    // The maps' shaders compiled, nothing made of them: for a start with the shadows off (FBackend::WarmShaders) -
    // the same as CreatePipeline asks for, so the shader cache knows them when the shadows are switched on.
    static void WarmShaders();
    void  Shutdown();
    // A diagnostic (FBackend::NameResources): the cube arrays and the masks named, for DRED's report.
    void  NameResources();
    UBOOL IsReady() const { return bReady; }
    const RdxMath::FShadowPreset& GetPreset() const { return Preset; }
    INT   GetMaxLights() const { return MaxLights; }

    // The level's occluding polygons as a triangle list, x y z a vertex, world units. Replaces the level before it (kept
    // alive a few frames, the GPU may still be reading it), makes every world map due again, and sorts the triangles into a
    // grid, so that a light's world map is drawn with the triangles within its reach only. No vertices: no walls. The
    // vertices go on the card's own memory where Card can put them there (CardWriter.h), otherwise in an upload heap.
    UBOOL SetWorld(ID3D12Device* Device, const FLOAT* Xyz, INT VertexCount, FCardWriter* Card);
    // What the LEVEL owns let go - the world maps' vertices and their grid (the other half of Shutdown, beside
    // ReleasePreset): at a teardown, and at a level's change once the GPU is done with them (FBackend::ForgetShadowWorld).
    void  ForgetWorld();

    // A frame: BeginFrame with the frame slot (every frame, in Lock - so nothing of the frame before is drawn again), the
    // casters, the lights, then Render within the frame's command list. The slot's vertex buffers are made larger
    // here where a frame before asked for more than they hold: empty, as every frame writes its own.
    void   BeginFrame(INT FrameIndex);
    // Room for one caster's triangles - VertexCount x 3 floats, world units - and the sphere around them. Index: its place
    // in the frame's list (the lights' Exclude bits). The list is as long as the frame's casters are many. Null where
    // the slot's buffer has no room left for the triangles - what a frame asks for is noted, and the frames after it
    // have the room (BeginFrame) - and where no memory was given (ListsRefused).
    FLOAT* AddCaster(INT VertexCount, FLOAT X, FLOAT Y, FLOAT Z, FLOAT Radius, INT& Index);
    // The casters' list, the lights' or the frame's buffers could not be made longer this frame: the machine gave no
    // memory - the caller's sign to leave the shadows out as a whole.
    UBOOL  ListsRefused() const { return bListsRefused; }
    // The texture places of a caster AddCaster placed: three floats a vertex - u, v and its mask's slot.
    FLOAT* CasterUvw(INT Index);
    // The arena of shapes that do not change (RenderDevice.cpp's shape cache): room for VertexCount vertices - positions into
    // Xyz, texture places into Uvw - written once and drawn from there every frame after, with no copy. Offset: its first
    // vertex. False when the arena is full. Emptied with the level (SetWorld).
    UBOOL  ArenaAlloc(INT VertexCount, INT& Offset, FLOAT*& Xyz, FLOAT*& Uvw);
    // A caster whose shape is in the arena: Offset and VertexCount as ArenaAlloc gave them. False where the list could
    // not be made longer (ListsRefused).
    UBOOL  AddArenaCaster(INT Offset, INT VertexCount, FLOAT X, FLOAT Y, FLOAT Z, FLOAT Radius, INT& Index);
    // Whether some face of a caster uses a mask (a slot above 0 in its texture places) - for the RT shadows, which test the
    // mask where a ray meets such a caster (FRtPass::ListScene). Set after AddCaster or AddArenaCaster; 0 until then.
    void   SetCasterMasked(INT Index, UBOOL bMasked) { if (Index >= 0 && Index < CasterCount) Casters[Index].bMasked = bMasked; }
    UBOOL  IsCasterMasked(INT Index) const  { return Index >= 0 && Index < CasterCount && Casters[Index].bMasked; }
    // The player's own body while it is not drawn (first person): it casts its shadow like any other caster, but the RT
    // occlusion's rays pass it (FRtPass::ListScene) - a darkening under feet nobody sees is only a stain. Set after
    // AddCaster or AddArenaCaster; 0 until then.
    void   SetCasterOwnBody(INT Index) { if (Index >= 0 && Index < CasterCount) Casters[Index].bOwnBody = 1; }
    UBOOL  IsCasterOwnBody(INT Index) const { return Index >= 0 && Index < CasterCount && Casters[Index].bOwnBody; }
    UBOOL  IsCasterInArena(INT Index) const { return Index >= 0 && Index < CasterCount && Casters[Index].bArena; }
    // A caster's positions as they went in - three floats a vertex, GetCasterGeometry's Count of them - and the sphere it was
    // given (X Y Z R), read back from the upload heap: slow, for a measurement only (RenderDevice.cpp, DumpCasters).
    const FLOAT* GetCasterPositions(INT Index) const;
    void         GetCasterSphere(INT Index, FLOAT* Out) const;
    // What the mirror rays show of a caster where they meet it off the picture (FRtPass::PrepareWater writes it
    // into the rays' table): its textures' average colour (RdxMath::PackRtAlbedo); how much of a texture a unit of
    // the level holds on it; the light all round it, and what that and its lamps' light - which the rays work out
    // where they meet it (SolidPass.hlsl, RtLampLight) - are scaled by; and its textures' places in the texture heap
    // plus one, by the key its faces carry (RdxMath::PackCasterSlot) - 0 where the heap holds none. Set after
    // AddCaster or AddArenaCaster; until then mid grey, no texture, and no light.
    struct FCasterRt
    {
        DWORD Colour;
        FLOAT Density;
        DWORD Ambient;      // the light all round it (RdxMath::PackRtLight), the engine's measure
        FLOAT Scale;        // what that and its lamps' light are scaled by, into the picture's measure
        DWORD Cube[RdxMath::RtCubeWays];    // the engine's own light on it by the six ways (Colour marks it there)
        DWORD Texture[RdxMath::RtCasterKeys];
    };
    void   SetCasterRt(INT Index, const FCasterRt& Rt)
    {
        if (Index >= 0 && Index < CasterCount)
            Casters[Index].Rt = Rt;
    }
    const FCasterRt* GetCasterRt(INT Index) const
    {
        return (Index >= 0 && Index < CasterCount) ? &Casters[Index].Rt : nullptr;
    }
    // The texture places' buffers, for the RT shadows: the frame's (a caster's First counts from its start) or the arena's.
    D3D12_GPU_VIRTUAL_ADDRESS GetCasterUvw(UBOOL bArena) const;
    INT    GetArenaVertices() const { return ArenaUsed; }
    // Counts the arena's emptyings: what was written in it before another number is gone (ArenaAlloc).
    INT    GetArenaGen() const { return ArenaGen; }
    // The casters' buffers themselves, for a pass that hands their bytes on to another process (the rays' host,
    // docs/terv-rt-host.md): the positions (bUvw 0) or the texture places (1) - the frame's, this frame slot's, or the
    // arena's - twelve bytes a vertex; and the vertices in use in the frame's (the arena's: GetArenaVertices).
    ID3D12Resource* GetCasterBuffer(UBOOL bArena, UBOOL bUvw) const;
    INT    GetFrameVertices() const { return ActorVertexCount; }
    // The lookup's constants' buffer for a frame slot: GetLookupConstants' bytes, from its start.
    ID3D12Resource* GetLookupBuffer(INT FrameIndex) const;
    // The masks copied whole into To - a texture array of the same shape, in COPY_DEST - with the masks' own array left
    // in the state it was in.
    void   CopyMasks(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* To);
    // One caster's triangles where they already lie - the frame's buffer or the arena - so another pass (the motion
    // vectors of bodies that move by themselves) can draw the same shape without a copy. False for an index out of range.
    UBOOL  GetCasterGeometry(INT Index, D3D12_VERTEX_BUFFER_VIEW& View, INT& First, INT& Count) const;

    // Masked casters: a texture's mask - RdxMath::ShadowMaskSize squared, 255 where the texture is opaque - goes into
    // the maps' own texture array and onto the card at the next Render. Key identifies the texture across frames; a
    // null Coverage records a texture that has no mask to give. FindMask returns its slot: -1 not known yet, -2 known
    // to have none, or no room was left. Slot 0 is solid - what every face without a mask uses. Forgotten with the
    // level (SetWorld).
    INT    FindMask(void* Key) const;
    INT    AddMask(void* Key, const BYTE* Coverage);
    INT    GetMaskCount() const       { return MaskCount; }
    INT    GetUnmaskableCount() const { return UnmaskableCount; }
    // The frame's lights, after its casters: each one's Exclude and Own hold GetCasterWords words (null: none), and
    // are copied - GetLight's point at the copies.
    void   SetLights(const FShadowLightFrame* Lights, INT Count);
    INT    GetLightCount() const  { return LightCount; }
    INT    GetCasterCount() const { return CasterCount; }
    INT    GetCasterWords() const { return ShadowCasterWords(CasterCount); }
    const FShadowLightFrame& GetLight(INT i) const { return Lights[i]; }
    INT    GetSlot(INT i) const   { return LightSlot[i]; }
    // After Render: whether a light's maps are drawn. At most WORLD_REDRAWS_PER_FRAME world maps a frame
    // (RdxMath::WorldRedrawOrder): a light whose map is missing or unusable waits for its turn out of the frame's lookup; one
    // whose map is only stale uses it meanwhile.
    UBOOL  IsLightReady(INT i) const { return LightReady[i]; }

    // Draws what is due - the masks new since the last Render onto the card, a few world maps a frame (the missing ones first,
    // then the moved lights', the oldest first), every ready light's actor map - and leaves both cube arrays readable. SrvHeap
    // and MaskSrv: the caller's shader-visible heap with the masks' view (DescribeMaskView) in it. The caller binds its own
    // targets, viewport, scissor, pipeline and descriptor heap afterwards.
    void  Render(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* SrvHeap, D3D12_GPU_DESCRIPTOR_HANDLE MaskSrv);
    // Render's first part alone: the masks new since the last one onto the card, the array left readable. For the RT
    // shadows, which draw no maps but read the masks.
    void  UploadMasks(ID3D12GraphicsCommandList* Cmd);

    // The own lamps' maps (RdxMath::OwnLampLight): a map of depths a lamp, drawn from the lamp along its axis with
    // RdxMath::SpotMatrix - the level's triangles within OwnLampReach and the casters of the frame, the world's own
    // part as the cube maps have it. They are drawn before the world, which reads them (SolidPass.hlsl, OwnShadow), so
    // the casters are this frame's own (AddCaster before it).
    struct FSpot
    {
        FLOAT At[3];
        FLOAT Aim[3];           // a unit vector
        FLOAT Matrix[16];       // RdxMath::SpotMatrix's rows
    };
    // The maps' array, OwnLampMost slices, made when a frame first asks. False where the card refused it: no lamp
    // lights anything then, and the menu says so.
    UBOOL EnsureSpots(ID3D12Device* Device);
    UBOOL SpotsRefused() const      { return bSpotsRefused; }
    // Counts the times the array was made: whoever holds a view of it writes the view again when this changes.
    INT   SpotsMade() const         { return SpotsMadeCount; }
    ID3D12Resource* GetSpotMaps() const { return SpotMaps.Get(); }
    // The array's view: a 2D texture array of OwnLampMost slices, one float a texel.
    void  DescribeSpotView(D3D12_SHADER_RESOURCE_VIEW_DESC& Desc) const;
    // The lamps' maps, slice i lamp i's. A caster whose sphere holds the lamp - its bearer, the player's own body for
    // the flashlight - stays out of its map: the lamp is in front of it, and its light is never its own body's to
    // stop. SrvHeap and MaskSrv as for Render; the caller binds its own targets, viewport, scissor, pipeline and
    // descriptor heap afterwards. The maps are left readable by a pixel shader.
    void  RenderSpots(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* SrvHeap, D3D12_GPU_DESCRIPTOR_HANDLE MaskSrv,
                      const FSpot* Spots, INT Count);

    // The cube arrays and the view the lookup reads them through (a cube array of GetMaxLights cubes, one float).
    ID3D12Resource* GetWorldCubes() const { return WorldCubes.Get(); }
    ID3D12Resource* GetActorCubes() const { return ActorCubes.Get(); }
    ID3D12Resource* GetMaskArray() const  { return MaskArray.Get(); }
    void            DescribeCubeView(D3D12_SHADER_RESOURCE_VIEW_DESC& Desc) const;
    // The masks' view: a 2D texture array of RdxMath::ShadowMaxMasks, one byte a texel.
    void            DescribeMaskView(D3D12_SHADER_RESOURCE_VIEW_DESC& Desc) const;
    // The depth's near distance from the light (RdxMath::CubeFaceMatrix).
    FLOAT           GetNear() const { return 1.0f; }

    // The lookup's constants for a frame slot, mapped - LOOKUP_CONSTANT_BYTES of them - and their GPU address. The size
    // comes FROM the layout (RdxMath::ShadowCbCount), rounded up to D3D12's 256-byte constant-buffer alignment. It was a
    // flat 1024 once: at 50 lights the buffer needs 2592 bytes, and the light arrays would have run past the end of the
    // mapped upload heap without a word from anyone.
    enum { LOOKUP_CONSTANT_BYTES = (RdxMath::ShadowCbCount * 4 + 255) & ~255 };
    BYTE* GetLookupConstants(INT FrameIndex, D3D12_GPU_VIRTUAL_ADDRESS& Address);

    // Diagnostics. Of the last Render: world maps drawn, actor draw calls. Since the level was set: world maps drawn, lights
    // left out of a frame's lookup waiting for theirs, lights that used a stale one, and the triangles drawn into world maps.
    // The vertices in the buffers.
    INT GetWorldRedraws()        const { return StatWorldRedraws; }
    INT GetActorDraws()          const { return StatActorDraws; }
    INT GetFacesKept()           const { return StatFacesKept; }
    INT GetFacesMoving()         const { return StatFacesMoving; }
    INT GetFacesChanged()        const { return StatFacesChanged; }
    INT GetTotalWorldRedraws()   const { return TotalWorldRedraws; }
    INT GetTotalDeferred()       const { return TotalDeferred; }
    INT GetTotalStaleUsed()      const { return TotalStaleUsed; }
    DOUBLE GetTotalWorldTriangles() const { return TotalWorldTriangles; }
    INT GetWorldVertices()       const { return WorldVertexCount; }
    INT GetActorVertices()       const { return ActorVertexCount; }
    // Render's CPU time since the last call, in seconds: the whole of it, and the world maps' triangles within it
    // (CollectWorldTriangles and the faces' indices) - each summed, with its largest frame. Zeroed by the call.
    void TakeRenderCpu(DOUBLE& RenderSum, DOUBLE& RenderMax, DOUBLE& WorldSum, DOUBLE& WorldMax, INT& Frames) const
    {
        RenderSum    = CpuRenderSum;
        RenderMax    = CpuRenderMax;
        WorldSum     = CpuWorldSum;
        WorldMax     = CpuWorldMax;
        Frames       = CpuFrames;
        CpuRenderSum = CpuRenderMax = CpuWorldSum = CpuWorldMax = 0.0;
        CpuFrames    = 0;
    }

private:
    UBOOL CreatePipeline(ID3D12Device* Device);
    UBOOL CreateCubes(ID3D12Device* Device, INT Size, TComRef<ID3D12Resource>& Out, const TCHAR* What);
    UBOOL CreateMaskArray(ID3D12Device* Device);
    void  ForgetMasks();
    void  SetFace(ID3D12GraphicsCommandList* Cmd, INT Face, const RdxMath::FVec3& Light) const;
    UBOOL BuildWorldGrid(const FLOAT* Xyz, INT Triangles);
    // BuildWorldGrid's clusters and their lists in the grid's cells, once the grid is laid out; false where refused.
    UBOOL BuildClusters();
    void  FreeWorldGrid();
    // A new query's stamp: where the count would run out, every stamp of the triangles and the clusters is cleared.
    INT   NextStamp();
    // The two halves of Shutdown. ReleasePreset drops what the PRESET owns - the cube arrays, the pipelines, the
    // per-frame buffers, the slots; ForgetWorld drops what the LEVEL owns. Init calls only the first, so a change of
    // preset in the running game keeps the geometry the world maps are drawn from.
    void  ReleasePreset();
    // The level's triangles whose sphere reaches into the light's sphere, into Candidates; returns how many.
    INT   CollectWorldTriangles(const RdxMath::FVec3& Light, FLOAT Radius);
    // The level's triangles a spot may light (RenderSpots), as indices into the level's vertices from Out on, at most
    // Room: how many indices, or -1 where they do not fit.
    INT   CollectSpotTriangles(const RdxMath::FVec3& Pos, const RdxMath::FVec3& Aim, FLOAT Cos, FLOAT Sin, FLOAT Reach,
                               UINT* Out, INT Room);

    // The actors' buffers of a frame slot begin with room for FIRST_ACTOR_VERTICES vertices (4 bytes x 3 each) and are
    // made larger as the frames ask (ActorRoom); the world's index buffer of a frame slot holds WORLD_INDICES indices
    // (4 bytes each) - a world map that does not fit is drawn with the whole level.
    enum { FIRST_ACTOR_VERTICES = 500000, WORLD_INDICES = 1000000, WORLD_REDRAWS_PER_FRAME = 2, MAX_UNMASKABLE = 64,
           ARENA_VERTICES = 400000, FIRST_CASTERS = 128 };
    // A frame slot's two vertex buffers, with room for Vertices vertices each: the ones it had are let go - the GPU is
    // done with a slot's frame before the slot is written again. False, with the old ones kept, where they cannot be
    // made.
    UBOOL MakeActorBuffers(INT Slot, INT Vertices);
    // Room in the casters' list for Wanted of them, and in the scratch lists that are as long.
    UBOOL HoldCasters(INT Wanted);
    void  RefuseLists(const TCHAR* Which, INT Wanted);

    RdxMath::FShadowPreset Preset;
    INT                    MaxLights;
    // The own lamps' maps (EnsureSpots): released with the preset, as everything they are drawn with is.
    TComRef<ID3D12Resource>       SpotMaps;
    TComRef<ID3D12DescriptorHeap> SpotDsvHeap;
    D3D12_RESOURCE_STATES         SpotState      = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    UBOOL                         bSpotsRefused  = 0;
    INT                           SpotsMadeCount = 0;
    UBOOL                  bReady;       // Init went through to the end
    ID3D12Device*          Dev;          // the caller's, while Init holds: the frame's buffers are made again with it
    UBOOL                  bListsRefused;

    TComRef<ID3D12RootSignature> Root;
    TComRef<ID3D12PipelineState> Pipeline;         // the level's maps: positions only
    TComRef<ID3D12PipelineState> ActorPipeline;    // the actors': positions, texture places, and the masks' pixel shader

    // Six faces a light, the light's slot times six plus the face; in the depth-stencil heap the actors' views come first,
    // the world's after them. Tracked states: both rest readable between frames.
    TComRef<ID3D12Resource>       ActorCubes;
    TComRef<ID3D12Resource>       WorldCubes;
    D3D12_RESOURCE_STATES         ActorState;
    D3D12_RESOURCE_STATES         WorldState;
    TComRef<ID3D12DescriptorHeap> DsvHeap;

    TComRef<ID3D12Resource> WorldVb;
    INT                     WorldVertexCount;
    // The levels before, each until the GPU is surely done with it: its age in frames begun since (BeginFrame). A
    // second level before the first is free keeps both.
    enum { RETIRED_WORLD_MOST = 4 };
    ID3D12Resource*         RetiredWorldVb[RETIRED_WORLD_MOST];
    INT                     RetiredAge[RETIRED_WORLD_MOST];
    void                    FreeRetiredWorlds();

    // The level's triangles for the culling (BuildWorldGrid): a sphere each (x y z r), and a grid over the level whose cells
    // list the triangles their sphere's box touches; a triangle touching too many cells is in BigTris, which every query
    // looks through. Stamp: the query that last took a triangle.
    FLOAT* WorldSpheres;
    INT    WorldTriangles;
    INT*   CellStart;          // GridCells + 1 offsets into CellTris
    INT*   CellTris;
    INT*   BigTris;
    INT    BigCount;
    INT    GridX, GridY, GridZ, GridCells;
    FLOAT  GridMin[3];
    FLOAT  CellSize;
    INT*   Stamp;
    INT    StampClock;
    INT*   Candidates;
    BYTE*  FaceMasks;      // a candidate's faces (RdxMath::CubeFacesTouched), as long as Candidates
    // The same triangles in clusters (BuildClusters), what the spots' collection walks: runs of consecutive triangles
    // - a node's pieces lie together - each with a sphere holding its triangles' spheres, listed in the cells of the
    // same grid by that sphere's box; a cluster touching too many cells is in BigClusters. ClusterStamp: the query that
    // last took a cluster (StampClock's count).
    INT*   ClusterFirst;          // ClusterCount + 1: a cluster's first triangle
    FLOAT* ClusterSpheres;        // x y z r
    INT*   ClusterStamp;
    INT    ClusterCount;
    INT*   CellClusterStart;      // GridCells + 1 offsets into CellClusters
    INT*   CellClusters;
    FLOAT* CellClusterMaxR;       // GridCells: the largest sphere of the clusters a cell lists
    INT*   BigClusters;
    INT    BigClusterCount;

    TComRef<ID3D12Resource> ActorVb[BACKEND_FRAME_COUNT];
    FLOAT*                  ActorMapped[BACKEND_FRAME_COUNT];
    TComRef<ID3D12Resource> ActorUvwVb[BACKEND_FRAME_COUNT];      // beside ActorVb: u, v and the mask's slot a vertex
    FLOAT*                  ActorUvwMapped[BACKEND_FRAME_COUNT];
    INT                     ActorRoom[BACKEND_FRAME_COUNT];       // the vertices each slot's two have room for
    INT                     ActorAsked;     // the vertices this frame's casters asked for, placed or not
    INT                     ActorWanted;    // the most a frame has asked for: what the slots are made to hold
    // The arena (ArenaAlloc): positions and texture places, mapped for good; ArenaUsed vertices given out since the level.
    TComRef<ID3D12Resource> ArenaVb;
    TComRef<ID3D12Resource> ArenaUvwVb;
    FLOAT*                  ArenaMapped;
    FLOAT*                  ArenaUvwMapped;
    INT                     ArenaUsed;

    // The masks (FindMask, AddMask): the array on the card, and the mapped upload buffer - a mask's bytes at its slot times
    // ShadowMaskSize squared - MaskDirty's slots are copied from at the next Render.
    TComRef<ID3D12Resource> MaskArray;
    D3D12_RESOURCE_STATES   MaskState;
    TComRef<ID3D12Resource> MaskUpload;
    BYTE*                   MaskMapped;
    void*                   MaskKeys[RdxMath::ShadowMaxMasks];
    UBOOL                   MaskDirty[RdxMath::ShadowMaxMasks];
    INT                     MaskCount;
    void*                   Unmaskable[MAX_UNMASKABLE];
    INT                     UnmaskableCount;
    INT                     ActorVertexCount;
    TComRef<ID3D12Resource> WorldIb[BACKEND_FRAME_COUNT];
    UINT*                   WorldIbMapped[BACKEND_FRAME_COUNT];
    INT                     WorldIbUsed;
    INT                     FrameSlot;

    TComRef<ID3D12Resource> LookupCb[BACKEND_FRAME_COUNT];
    BYTE*                   LookupMapped[BACKEND_FRAME_COUNT];

    struct FCaster
    {
        INT   First, Count;     // vertices in the frame's buffer - or in the arena
        FLOAT X, Y, Z, R;       // the sphere around them
        UBOOL bArena;           // AddArenaCaster's
        UBOOL bMasked;          // see SetCasterMasked
        UBOOL bOwnBody;         // see SetCasterOwnBody
        FCasterRt Rt;           // see SetCasterRt
    };
    FCaster* Casters;           // CasterRoom of them (HoldCasters)
    INT      CasterRoom;
    INT      CasterCount;
    // Render's lists of one light's casters and the cube faces each touches: as long as Casters.
    INT*     NearCasters;
    BYTE*    NearFaces;
    // The lights' Exclude and Own bits (SetLights): two rows of LightWords words a light.
    DWORD*   LightBits;
    INT      LightBitsRoom;
    INT      LightWords;

    FShadowLightFrame       Lights[RdxMath::ShadowMaxLights];
    INT                     LightSlot[RdxMath::ShadowMaxLights];
    UBOOL                   LightReady[RdxMath::ShadowMaxLights];
    RdxMath::EWorldMapState LightState[RdxMath::ShadowMaxLights];     // SetLights: how its world map stands
    INT                     LightCount;

    // A slot is one cube in each array. A light keeps its slot while it stays chosen; how its world map stands (LightState,
    // RdxMath::ClassifyWorldMap) goes by where the map was drawn from (X Y Z, with reach R), and a new level drops it.
    enum { FACE_KEEP = 128 };     // the casters a kept face may have (FSlot::KeptFaces)
    struct FSlot
    {
        void* Key;
        FLOAT X, Y, Z, R;
        UBOOL bWorldValid;    // a world map is drawn for Key
        INT   LastUsed;
        INT   DrawnClock;     // UseClock when it was drawn: the redraw order's age
        // Which of the actor cube's six faces had anything drawn into them last time, bit f for face f. A face that is
        // empty now AND was empty before needs neither a draw nor a CLEAR - it already reads "nothing in the way". That
        // clear is what costs at high light counts: 50 lights x 6 faces is 300 depth clears of a megapixel every frame,
        // most of them into faces no caster ever reaches. All six bits (ALL_FACES) when the slot changes hands, the
        // level is forgotten or the cubes are new: what is in the faces then is another light's, or nothing that was
        // ever cleared, and a face marked as not drawn into would keep it - the new light would see the old one's
        // casters.
        BYTE  ActorFaceUsed;
        // The faces of the actor cube that hold what drawing them again would give, to the bit (Render): bit f for face
        // f, drawn from KeptAt with nothing but stretches of the arena - whose vertices and texture places never change
        // while ArenaGen stays - each face's in KeptFirst and KeptCount by their place in it, KeptLength of them, with
        // the masks as MaskGen had them. A face whose casters are those again, from the same place, is left as it is:
        // only depth is drawn, so the order they come in makes no difference to it. None wherever ActorFaceUsed goes
        // to ALL_FACES.
        BYTE           KeptFaces;
        BYTE           KeptLength[6];
        RdxMath::FVec3 KeptAt;
        INT            KeptArenaGen, KeptMaskGen;
        INT            KeptFirst[6][FACE_KEEP], KeptCount[6][FACE_KEEP];
    };
    enum { ALL_FACES = 0x3F };
    FSlot Slots[RdxMath::ShadowMaxLights];
    INT   UseClock;

    INT StatWorldRedraws;
    INT StatActorDraws;
    INT StatFacesKept;
    INT StatFacesMoving;         // drawn again for a caster not the arena's: one that moved or is animated
    INT StatFacesChanged;        // drawn again for other casters than before, or with none kept for it
    // Counted up whenever the arena is emptied and the masks are forgotten: a kept face (FSlot::KeptFaces) drawn
    // before is no longer what its casters would draw.
    INT ArenaGen = 0, MaskGen = 0;
    INT StatStaleUsed;
    INT TotalWorldRedraws;
    INT TotalDeferred;
    INT TotalStaleUsed;
    DOUBLE TotalWorldTriangles;   // a long session's sum outgrows an INT
    mutable DOUBLE CpuRenderSum = 0.0, CpuRenderMax = 0.0, CpuWorldSum = 0.0, CpuWorldMax = 0.0;   // TakeRenderCpu
    mutable INT    CpuFrames    = 0;
};

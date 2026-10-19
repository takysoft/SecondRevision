// SecondRevision - the rays' scene (docs/terv-rt-host.md). The rays - the RT shadows', the RT ambient occlusion's and
// the mirror images' - are traced by the 64-bit host: it makes the structures and runs the passes, as not every
// driver gives a 32-bit process ray tracing, and the host is the one way every card has. What is kept here is what
// the host's structures are made of and what its passes read - the level's triangles, the frame's bodies as a list,
// the shadows' constants and table, the mirror rays' table and the level's colours - and the far probe, which
// traces nothing.
// Included from Backend.h, which gives it BACKEND_FRAME_COUNT.
#pragma once

class FShadowMaps;

class FRtPass
{
public:
    FRtPass();
    ~FRtPass();

    // Once the host that is up says it traces (FBackend::SyncHost): the level's triangles (SetLevel, SetThin), the
    // frame's bodies as a list (ListScene) and the shadows' constants and table (FillShadowExclusion) are kept from
    // here on. No pipeline and no structure is made. False, logged, where the constants' buffer could not be made.
    UBOOL InitHosted(ID3D12Device5* Device);
    void  Shutdown();
    // IsHosted: InitHosted made the pass. IsReady: and its lists can be kept - no longer from the frame on where the
    // machine gave no memory for them, and the settings on rays run their fallbacks from there.
    UBOOL IsHosted() const { return bHosted; }
    UBOOL IsReady() const { return bReady; }
    // The frame's bodies as the host makes them into the actors' structure - every caster the shadow maps hold
    // triangles of, in their order, the ones a ray may pass not opaque: how many, or -1 where the lists could not
    // be held.
    // GetBody: one of them as the card would be told of it - where its corners begin (an address in the shadow maps'
    // buffer for this frame slot, or in their arena), how far apart they lie, how many, and whether it is opaque.
    INT   ListScene(INT FrameIndex, const FShadowMaps& Shadow);
    const D3D12_RAYTRACING_GEOMETRY_DESC& GetBody(INT Geo) const { return Geos[Geo]; }
    // Which of them is the player's own body, for the rays that pass it; -1: it is not among them.
    INT   GetOwnBody(INT FrameIndex) const
    {
        return (FrameIndex >= 0 && FrameIndex < BACKEND_FRAME_COUNT) ? OwnBodyGeo[FrameIndex] : -1;
    }
    // What the host's share of a level is made of: the three buffers of corners - 0 the level's, 1 the sky
    // walls', 2 the see-through surfaces'; null where there is none - the corners in each, twelve bytes apart, and a
    // number that changes whenever any of them does.
    ID3D12Resource* GetStaticBuffer(INT Which) const;
    INT   GetStaticVertices(INT Which) const;
    INT   GetStaticSerial() const { return StaticSerial; }
    // The buffers FillShadowExclusion writes, for copying on: the constants - a frame slot's
    // GetExclusionBytes at GetExclusionOffset - and the frame slot's table with the bytes of it in use.
    ID3D12Resource* GetExclusionBuffer() const { return ExclusionCb.Get(); }
    UINT64 GetExclusionOffset(INT FrameIndex) const { return (UINT64)EXCLUSION_STRIDE * FrameIndex; }
    UINT   GetExclusionBytes() const { return EXCLUSION_STRIDE; }
    ID3D12Resource* GetShadowTableBuffer(INT FrameIndex) const;
    UINT64 GetShadowTableBytes(INT FrameIndex) const;
    // What the host's pass traces on by from what a ray met, for copying on. The frame slot's table of the
    // mirror rays, as PrepareWater filled it, and the bytes of it a frame with this many bodies has in use. And the
    // level's colours: how many bytes they are (0: no level), a number that changes whenever any of them does on
    // the card, and the copy of them all from there into To - a buffer in COPY_DEST, at least as long; False:
    // nothing was copied, they are not on the card yet.
    ID3D12Resource* GetWaterTableBuffer(INT FrameIndex) const;
    UINT64 GetWaterTableBytes(INT Bodies) const;
    UINT64 GetMaterialBytes() const;
    INT    GetMaterialSerial() const { return MaterialSerial; }
    UBOOL  CopyMaterial(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* To) const;

    // The level's triangles, as the shadow maps get them (FBackend::SetShadowWorld), into a buffer of the scene's own - the
    // long ones cut into pieces first (RdxMath::SplitTriangle), for the rays' sake. The host makes its structure of
    // the level from it (FHostRayFeed). None: no level.
    // For the RT water: each triangle's surface (Surf, one a triangle; a piece takes its triangle's), and each surface's
    // colour (Albedo, SurfCount of them - RdxMath::PackRtAlbedo) - the level's colours (LevelMaterial), put on the card
    // at the next PrepareWater. Their lights come as the surfaces are drawn (SetSurfaceLight). And the level's sky walls
    // (Sky, PF_FakeBackdrop), a structure of their own, which only the water's rays see (RT_MASK_SKY): a ray that reaches
    // one has reached the sky.
    // The level's see-through surfaces - a pane, a grating, a force field's sheet - before SetLevel: their triangles
    // and each one's surface. A structure of their own, which only the mirror rays ask (RT_MASK_THIN) on their way
    // to what they meet: such a surface stops no ray, it adds its colour to what lies behind it (SolidPass.hlsl,
    // RtThinOver). None: the level has none.
    void  SetThin(const FLOAT* Xyz, INT VertexCount, const DWORD* Surf);
    void  SetLevel(const FLOAT* Xyz, INT VertexCount, const DWORD* Surf, const DWORD* Albedo, INT SurfCount,
                   const FLOAT* Sky, INT SkyVertexCount);
    // A surface's light for the RT water - its light map's average (RdxMath::RtLightAverage) - with the light map's
    // identity (the engine's CacheID), so that the same light map is not averaged again: WantsSurfaceLight says whether this
    // one is new for the surface.
    UBOOL WantsSurfaceLight(INT Surf, QWORD LightId) const
    {
        return Surf >= 0 && Surf < SurfCount && SurfLightId && SurfLightId[Surf] != LightId;
    }
    void  SetSurfaceLight(INT Surf, QWORD LightId, DWORD Light);
    // A surface's own picture for the mirror rays, as the surface is drawn: its texture's place in the texture heap
    // (SrvIndex), the rows that put a place of the level on it (Rows, eight numbers - RdxMath::RtSurfaceRows), and how
    // many texels a unit of the level holds (Density). bKeepRows: a surface whose texture the level pans by itself -
    // its rows change every frame, and are kept as first seen for as long as the texture stays.
    void  SetSurfaceTexture(INT Surf, INT SrvIndex, const FLOAT* Rows, FLOAT Density, UBOOL bKeepRows);
    // A surface the rays are to pass: its texture taken back - a ray goes by a surface with none (RtThinOver).
    void  ClearSurfaceTexture(INT Surf);
    // And its light map, the same way: its place in the texture heap, the rows that put a place of the level on it
    // (the texture's own rows with the light map's pan and scale), the lit part's edges on it (Edge: the least u
    // and v, the most u and v - past them the map holds nothing that was lit), and the gain the surface's draws
    // carry. A mirror ray then reads the light of the very spot it met.
    void  SetSurfaceLightMap(INT Surf, INT SrvIndex, const FLOAT* Rows, const FLOAT* Edge, FLOAT Gain);
    // A surface's own face, once a level: its normal, the light all round the zone before it (RdxMath::PackRtLight),
    // and whether only the lamps marked specially light it.
    // Kind: how the level draws it (RDX_RT_KIND_*), for the rays that pass a see-through one.
    void  SetSurfaceStatic(INT Surf, const FLOAT* Normal, DWORD Ambient, UBOOL bSpecial, DWORD Kind);
    // The level's lamps, a frame at a time, for the light the rays work out where they meet something
    // (SolidPass.hlsl, RtLampLight): Blob holds Count lamps of RDX_RT_LAMP_BYTES - place and reach, colour and a
    // spot's cone, aim and flags - then RDX_RT_LAMP_BUCKETS pairs (a list's start and length; a place of the level
    // finds its list by its cell of Cell units), the lists' lamp numbers, and Large numbers of the lamps too large
    // for the lists; Part: where each of the four starts in Blob. Gain: the made light into the picture's measure.
    void  SetLamps(const BYTE* Blob, INT Bytes, const DWORD* Part, INT Count, INT Large, FLOAT Cell, FLOAT Gain);
    // Before the frame's rays are asked, after ListScene (FBackend::BeginRayAsk): the level's colours onto the card
    // where they changed, and the RT water's table (SolidPass.hlsl's RtWater, a buffer) for the frame slot - the camera
    // (Eye, world units) and whether a quarter of the rays are asked (bHalf: Reflections=RTHalf), where the surfaces'
    // records start and the measuring view (View, FBackend::RequestRtWaterView), then a row for each actor geometry
    // (RdxMath::RtGeoUints): its first vertex in the shadow maps' buffers, and what the rays show of it
    // (FShadowMaps::FCasterRt). The table's address; 0 when there is nothing to trace against.
    D3D12_GPU_VIRTUAL_ADDRESS PrepareWater(ID3D12GraphicsCommandList* Cmd, INT FrameIndex, const FShadowMaps& Shadow,
                                           const FLOAT* Eye, INT View, UBOOL bHalf);
    // The level's colours as the RT water reads them (SolidPass.hlsl's LevelMaterial); 0 before a level.
    D3D12_GPU_VIRTUAL_ADDRESS GetLevelMaterial() const;
    // The far probe (Reflections=RT): what the picture shows far off, by direction - a cube, FAR_SIZE texels a face,
    // the world's axes. Each frame, before the water, every texel whose direction the picture shows further than
    // FAR_MIN units off (the sky, a skyline painted at the level's edge) takes the picture's colour there; the others
    // keep what they had - so what the railing hides one step back, or what lies above the view, is still there from an
    // earlier look. A water ray that reaches the sky reads it where the picture along its own direction has something
    // nearer in the way (SolidPass.hlsl, WaterReflectRt). Emptied with each level. Null until InitHosted made it.
    ID3D12Resource* GetFar() const { return Far.Get(); }
    void  DescribeFarViews(D3D12_SHADER_RESOURCE_VIEW_DESC& Srv, D3D12_UNORDERED_ACCESS_VIEW_DESC& Uav) const;
    // Records the update. Picture: a table of the picture's copy and the depth's (FBackend's water views, side by side);
    // FarUav: the probe's UAV, in the same heap; Axes (the camera's three, nine floats), TanX, TanY and Rect (x y width
    // height, 0..1): the world view. Leaves the probe readable by a pixel shader.
    void  UpdateFar(ID3D12GraphicsCommandList* Cmd, D3D12_GPU_DESCRIPTOR_HANDLE Picture, D3D12_GPU_DESCRIPTOR_HANDLE FarUav,
                    const FLOAT* Axes, FLOAT TanX, FLOAT TanY, const FLOAT* Rect);
    // The RT shadows' second constant buffer (ShadowFactor.hlsl's RtShadow, b1) and their table (RtGeo), written for
    // the frame slot after its ListScene. The table: each geometry's first vertex in the shadow maps' buffers, for
    // the masks; and after them, for each light the factor's constants carry, in their order (Lights: indices into the
    // shadow maps' list), a row of bits - the geometries of the scene its rays pass through, its
    // FShadowLightFrame::Own turned from the casters' order into the scene's. The constants: the RT occlusion's two
    // numbers (AoRadius, world units, and AoStrength - the SSAO's own), the RT factor's two costs (TracedLights,
    // EdgeSamples - RdxMath::RtShadowTracedLights, RtShadowEdgeSamples), the instances the occlusion's rays meet
    // (AoInstances, RT_MASK_*; 0 for the level and the actors - another value is a measurement's), and where the
    // table's rows start and how long one is. The constants' address; 0 before InitHosted or with no table.
    D3D12_GPU_VIRTUAL_ADDRESS FillShadowExclusion(INT FrameIndex, const FShadowMaps& Shadow, const INT* Lights, INT Count,
                                                  FLOAT AoRadius, FLOAT AoStrength, INT TracedLights, INT EdgeSamples,
                                                  INT AoInstances);

    // What a ray may meet, as the host's top level marks its instances: the occlusion's measurements name them.
    enum { RT_MASK_LEVEL = 0x01, RT_MASK_ACTORS = 0x02, RT_MASK_SKY = 0x04, RT_MASK_THIN = 0x08 };

private:
    // An upload buffer holding Floats floats from Src, for a structure to be made from; null when it cannot be made.
    ID3D12Resource* MakeVertexBuffer(const FLOAT* Src, INT Floats, const TCHAR* What);

    ID3D12Device5*               Device;          // the backend's, while InitHosted holds
    UBOOL                        bReady;
    UBOOL                        bHosted;         // InitHosted made the pass
    INT                          StaticSerial;    // counts the level's buffers' changes - see GetStaticSerial
    UBOOL MakeExclusion(ID3D12Device5* InDevice);
    // ListScene's work: the frame's casters into Geos, a geometry each, and the frame slot's lists and tables held
    // for them. False where the lists could not be held (the pass is then not ready).
    UBOOL GatherScene(INT FrameIndex, const FShadowMaps& Shadow, INT& Count, INT& Triangles, INT& NonOpaque);

    // The level: its triangles as SetLevel made them (12 bytes a vertex), and the one before them - kept until the
    // next level: a frame still on the GPU may be copying it for the host.
    TComRef<ID3D12Resource>      LevelVb;
    TComRef<ID3D12Resource>      RetiredLevelVb;
    INT                          LevelVertices;
    // The sky walls, the same way (SetLevel).
    TComRef<ID3D12Resource>      SkyVb;
    TComRef<ID3D12Resource>      RetiredSkyVb;
    INT                          SkyVertices;
    // The level's see-through surfaces (SetThin), the same way: their triangles, and each triangle's surface - which
    // goes after the level's own pieces in the colours' list (ThinBase: how many those are).
    TComRef<ID3D12Resource>      ThinVb;
    TComRef<ID3D12Resource>      RetiredThinVb;
    INT                          ThinVertices = 0;
    DWORD*                       ThinSurfIds  = nullptr;
    INT                          ThinBase     = 0;
    // Each frame slot's scene: which caster each geometry of the actors' structure is (a caster with no triangles is
    // left out, so the two orders differ), and how many there are.
    INT*                         GeoCaster[BACKEND_FRAME_COUNT];
    INT                          GeoRoom[BACKEND_FRAME_COUNT];
    INT                          GeoCount[BACKEND_FRAME_COUNT];
    INT                          OwnBodyGeo[BACKEND_FRAME_COUNT];   // the player's own body's geometry, or -1
    // ListScene's own lists, a place a caster: the geometries as the host's card is told of them, and a bit for every
    // one some light must not see.
    D3D12_RAYTRACING_GEOMETRY_DESC* Geos;
    DWORD*                       OwnBits;
    INT                          SceneRoom;
    // Room in those lists, and in the frame slot's own, for a frame of Casters casters.
    UBOOL HoldScene(INT FrameIndex, INT Casters);
    // FillShadowExclusion's constants, a frame slot's worth each - EXCLUSION_STRIDE bytes: the occlusion's float4, the
    // factor's uint4, the table's uint4.
    enum { EXCLUSION_STRIDE = 256 };
    TComRef<ID3D12Resource>      ExclusionCb;
    BYTE*                        ExclusionMapped;
    // A table a shader reads by the scene's geometries: an upload buffer of a frame slot's own, mapped for as long as
    // it lives, made again longer where a frame's geometries ask for more (HoldTable) - the GPU is done with a slot's
    // frame before the slot is written again.
    struct FTable
    {
        TComRef<ID3D12Resource> Res;
        BYTE*                   Mapped;
        UINT64                  Bytes;
    };
    // The RT shadows' (ShadowFactor.hlsl's RtGeo): a uint a geometry - its first vertex, bit 31 for the arena's buffer
    // - then a row of bits a light. The RT water's (SolidPass.hlsl's RtWater): the camera's float4 and the level's
    // uint4, then a row a geometry (RDX_RT_GEO_BYTES).
    FTable                       ShadowTable[BACKEND_FRAME_COUNT];
    FTable                       WaterTable[BACKEND_FRAME_COUNT];
    enum { WATER_TABLE_HEAD = RDX_RT_TABLE_HEAD };
    // The level's lamps for the rays' own light (SetLamps): the frame's, as handed over - the lamps, the lists they
    // are found by, and where each part starts.
    BYTE*                        LampBlob  = nullptr;
    INT                          LampRoom  = 0;
    INT                          LampBytes = 0;
    DWORD                        LampPart[4] = { 0, 0, 0, 0 };      // the lamps, the buckets, the lists, the large ones
    INT                          LampCount = 0;
    INT                          LampLarge = 0;
    FLOAT                        LampCell  = 1024.0f;
    FLOAT                        LampGain  = 1.0f;
    UBOOL HoldTable(FTable& Table, UINT64 Bytes, const TCHAR* What);
    // No memory for a list or a table: said, and the pass is no longer ready.
    void  RefuseLists(const TCHAR* Which, INT Wanted);
    // The level's colours (SetLevel): here, as the card is to have them - a surface a triangle piece, then two uints a
    // surface, the records, which change as the surfaces' lights come in - and each surface's light map's identity; on the
    // card, the buffer the water reads, and the one before it (kept until the next level: a frame may still read it); and
    // each frame slot's upload buffer, which carries the whole of it after SetLevel and the records after a change.
    DWORD*                       MaterialCpu;
    QWORD*                       SurfLightId;
    INT                          SurfCount;
    INT                          PieceCount;
    TComRef<ID3D12Resource>      LevelMaterial;
    TComRef<ID3D12Resource>      RetiredMaterial;
    D3D12_RESOURCE_STATES        MaterialState;
    TComRef<ID3D12Resource>      MaterialUpload[BACKEND_FRAME_COUNT];
    BYTE*                        MaterialUploadMapped[BACKEND_FRAME_COUNT];
    UINT64                       MaterialUploadBytes[BACKEND_FRAME_COUNT];
    UBOOL                        bMaterialDirty;   // all of it waits to be copied (SetLevel)
    UBOOL                        bRecordsDirty;    // the records do (SetSurfaceLight, SetSurfaceTexture):
    INT                          DirtyFirst;       // from this surface's
    INT                          DirtyLast;        // to this one's
    void  NoteRecordDirty(INT Surf);
    UBOOL                        bMaterialOnCard;
    INT                          MaterialSerial = 0;   // counts the copies onto the card - see GetMaterialSerial
    // The far probe (GetFar): its cube, its state, and the pass that fills it; bFarEmpty: the next update clears what it does
    // not fill (a new level).
    enum { FAR_SIZE = 512 };
    TComRef<ID3D12Resource>      Far;
    D3D12_RESOURCE_STATES        FarState;
    TComRef<ID3D12RootSignature> FarRoot;
    TComRef<ID3D12PipelineState> FarPipeline;
    UBOOL                        bFarEmpty;
    UBOOL InitFar(ID3D12Device5* InDevice);
    void  ForgetMaterial();
    void  UploadMaterial(ID3D12GraphicsCommandList* Cmd, INT FrameIndex);
    // For the log: what the last frames held.
    INT                          LoggedFrames;
};

// SecondRevision - the probe gathered from the picture (MirrorProbe.hlsl; docs/tukrozodes-szonda-terv.md).
//
// A pane of glass seen face on mirrors what stands before it - behind the eye, where the picture has nothing. Without
// rays (Reflections=High) the glass's layer reads it from this: a cube of the level round a point before the pane, each
// texel the nearest point of the level the world's views have shown in its direction from there, with its colour.
// Every frame, right after the water's copies of the picture and the depth (FBackend::CopyForWater), the passes carve
// out what the eye now sees past, take in what the eye sees of the level's own surfaces, and make the pyramid the
// glass reads - its finest level with the holes filled from the coarser ones.
//
// Where it stands: before the pane that takes up the most of the screen (RdxMath::FProbePlace), from the panes the
// frame before drew. As it moves what it holds goes with it - each point taken over round the new centre (the
// passes' Transfer*); with each level it empties.
//
// Now and then a frame counts what its passes did, and the next such frame says it in the log.
//
// The cells and the keys are the passes' own; the pyramid is read by the glass's layer, in the water's table
// (FBackend's WaterViewProbe) - raw buffers all, so no format a card has only as an option is needed.
#pragma once

#include "RenderMath.h"

class FMirrorProbe
{
public:
    FMirrorProbe();
    ~FMirrorProbe();

    // The buffers, the passes' root signature and pipelines. False, said in the log, where any of it could not be made:
    // the panes then have no mirror image without rays.
    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return bReady; }

    // A pane of the levels' glass the world's view draws this frame: its middle and its normal (towards the camera) in
    // the world, and how much of the screen it takes up.
    void  NotePane(const FLOAT* Middle, const FLOAT* Normal, FLOAT Area);
    // The frame's start: where the probe stands this frame, from the panes the frame before noted and where its camera
    // stood (Eye, the world). Its first place empties it; a move takes what it holds over (Update).
    void  BeginFrame(const FLOAT* Eye);
    // A new level: empty, standing nowhere.
    void  Forget();
    // Where it stands, in the world, and how far round it it holds the level; false while it stands nowhere, or holds
    // nothing of where it stands yet.
    UBOOL GetPlace(FLOAT* Centre, FLOAT& Reach) const;
    // The pane it stands before: its normal (towards the camera) and its middle, in the world; false while it follows
    // the camera, or as GetPlace.
    UBOOL GetPane(FLOAT* Normal, FLOAT* Middle) const;

    // The passes' table in the texture heap, VIEWS long: the copies' picture, depth and stencil, and the shadows' added
    // target - the backend's to write, being its own - then the probe's cells, keys, pyramid, spare cells and counts
    // (WriteOwnViews).
    enum { VIEW_PICTURE, VIEW_DEPTH, VIEW_KINDS, VIEW_ADDED, VIEW_CELLS, VIEW_KEYS, VIEW_LEVELS, VIEW_SPARE, VIEW_STATS,
           VIEWS };
    void  WriteOwnViews(D3D12_CPU_DESCRIPTOR_HANDLE First, UINT Step) const;
    // The pyramid's view for the glass's layer (a raw buffer); a view of nothing before Init.
    void  WriteLevelsView(ID3D12Device* Device, D3D12_CPU_DESCRIPTOR_HANDLE At) const;

    // The frame's passes on the copies, as MirrorProbe.hlsl lays them out. Table: the passes' table (above); Eye and
    // Axes: the world view's camera in the world (FBackend's CamEye, CamAxes); TanX, TanY and Rect: its view as
    // FRtPass::UpdateFar takes it; Width, Height: the copies' size. Leaves the pyramid readable by a pixel shader.
    void  Update(ID3D12GraphicsCommandList* Cmd, D3D12_GPU_DESCRIPTOR_HANDLE Table, const FLOAT* Eye, const FLOAT* Axes,
                 FLOAT TanX, FLOAT TanY, const FLOAT* Rect, UINT Width, UINT Height);

private:
    // A counting frame every COUNT_EVERY updates; the counts read back at the next one, long done by then.
    enum { CELL_BYTES = 32, ENTRY_BYTES = 16, CONSTANTS = 32, STATS_BYTES = 16, COUNT_EVERY = 240 };
    enum { PASS_CARVE, PASS_STATS_CLEAR, PASS_KEY, PASS_GATHER, PASS_CLEAR_SPARE, PASS_TRANSFER_KEY, PASS_TRANSFER_GATHER,
           PASS_TRANSFER_BACK, PASS_BASE, PASS_UP, PASS_DOWN, PASSES };
    ID3D12Device*                Device;
    UBOOL                        bReady;
    TComRef<ID3D12Resource>      Cells;
    TComRef<ID3D12Resource>      Keys;
    TComRef<ID3D12Resource>      Levels;
    TComRef<ID3D12Resource>      Spare;
    TComRef<ID3D12Resource>      Stats;
    TComRef<ID3D12Resource>      StatsBack;    // a counting frame's counts, read at the next
    D3D12_RESOURCE_STATES        LevelsState;
    TComRef<ID3D12RootSignature> Root;
    TComRef<ID3D12PipelineState> Pipelines[PASSES];
    RdxMath::FProbePlace         Place;
    UBOOL                        bEmpty;       // the next update empties every cell first (its first place, a new level)
    UBOOL                        bMoved;       // the next update takes what it holds over from Moved, where it stood
    RdxMath::FVec3               Moved;
    UBOOL                        bCounted;     // a counting frame's counts wait in StatsBack
    INT                          Told;         // the counts said so far
    UINT                         Frames;       // the updates so far: each cell's last sight, and which pixels gather
    ID3D12Resource* MakeBuffer(UINT64 Bytes, const TCHAR* What, D3D12_HEAP_TYPE Type = D3D12_HEAP_TYPE_DEFAULT);
    void  TellCounts();
};

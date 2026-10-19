// SecondRevision - the force field as a volume of the level's space (ForceField=3D).
//
// The game's force field is a flat surface with a small moving texture on it. Here it is a body of the level: every
// place within a half thickness of one of the field's surfaces holds some of it - the most on the surface itself,
// less the further off, nothing at the half thickness - and what it holds is made of a noise of the level's own three
// directions and the clock: arcs, bolts, sparks and a haze that are things of that space, not pictures on a surface.
// The pass lays it over the finished world a pixel at a time: the pixel's ray is walked through the stretches of
// space within reach of the field, and what the field gives off along it is summed. A place near two of the field's
// surfaces - a corner of a cabinet - holds what its distance from the nearer one says, once: the surfaces run into
// one another with no seam and no doubling.
#pragma once

class FFieldVolume
{
public:
    FFieldVolume() {}
    ~FFieldVolume() { Shutdown(); }

    // The most corners a polygon is handed over with; one with more goes in as several (the render device).
    enum { CORNERS = 12, ROWS = CORNERS + 3 };

    // The root signature, the shaders, the view heap, the noise and the first lists; the pipeline comes with the
    // scene's format (Record).
    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Root.Get() != nullptr && Heap.Get() != nullptr && Noise.Get() != nullptr; }

    // Where the pixels are: the engine's screen centre and projection in its own pixels, engine pixels to one of the
    // scene target's, and the view's place and axes in the world.
    struct FView
    {
        FLOAT Screen[2];
        FLOAT Focal;
        FLOAT EngPerPixel[2];
        FLOAT Eye[3];
        FLOAT AxisX[3], AxisY[3], AxisZ[3];
    };

    // Lays the field over the scene target the caller has bound, as the engine lays a translucent surface: the
    // field's light + target * (1 - light). Depth is the world's depth (reversed: 1 / view depth), readable by a
    // pixel shader, in the format DepthRead: the walk stops at what the world has in front.
    // Rows: Count polygons of ROWS rows of four numbers each - the plane's normal with the number of corners; a ball
    // about the polygon (its middle from the eye, its radius with the half thickness); the hue of the field it is
    // of, with how bright that field is laid on; the corners from the eye, in the world's axes, going round so that
    // an edge crossed with the normal points out of the polygon (RdxMath::FieldPolygonRows).
    // False where it could not be laid: the lists could not be made as long as the frame needs, or the pipeline.
    UBOOL Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, DXGI_FORMAT SceneFormat, ID3D12Resource* Depth,
                 DXGI_FORMAT DepthRead, const FView& View, const FLOAT* Rows, INT Count, FLOAT Half, FLOAT Time,
                 INT FrameIndex);

private:
    enum { FRAMES = 3, FIRST_POLYGONS = 32, NOISE_SIDE = 64 };
    UBOOL HoldPolygons(ID3D12Device* Device, INT Slot, INT Wanted);
    UBOOL EnsurePso(ID3D12Device* Device, DXGI_FORMAT SceneFormat);

    TComRef<ID3D12RootSignature>  Root;
    TComRef<ID3D12PipelineState>  Pso;
    DXGI_FORMAT                   PsoFormat = DXGI_FORMAT_UNKNOWN;
    TComRef<ID3DBlob>             Vs, Ps;       // kept for a pipeline made again in another format
    TComRef<ID3D12DescriptorHeap> Heap;         // [0] the depth, [1] the noise
    UINT                          HeapStep = 0;
    TComRef<ID3D12Resource>       Noise;        // on the card
    TComRef<ID3D12Resource>       NoiseUpload;  // its texels until the copy has been made, and after
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT NoiseFoot;
    UBOOL                         bNoiseToCopy = 0;
    TComRef<ID3D12Resource>       PolyUpload[FRAMES];       // upload buffers, kept mapped
    void*                         PolyMapped[FRAMES] = { nullptr, nullptr, nullptr };
    INT                           PolyRoom[FRAMES]   = { 0, 0, 0 };     // polygons each slot's list holds
    ID3D12Resource*               DepthSeen   = nullptr;
    DXGI_FORMAT                   DepthFormat = DXGI_FORMAT_UNKNOWN;
    INT                           ViewsAt     = 0;      // RdxViewGeneration when the depth's view was written
};

// SecondRevision - the fog of the level's volumetric lights, kept in the room it belongs to.
//
// The engine makes this fog itself - its fog maps on the level, a fog for every vertex of a mesh - and makes it the
// way a sphere would: through walls and floors. The thick fog of a room came up through the floor of the room above,
// and a surface or a mesh the level marks unlit got none, standing dark and sharp inside it.
//
// Measured on the engine's own values (scripts/fog_fit.py), the fog a light gives a point is the light's own factor
// times tau: the integral of 1 - r^2/R^2 along the eye's ray inside the light's sphere, divided by R - colour and
// alpha alike. This pass lays exactly that over the finished world, a pixel at a time, from each light whose zone
// the pixel or the eye is in; the light's factors are what the engine's own fog showed of it (FogRoomsLearn, in the
// device). A pixel's zone is found the engine's way (UModel::PointRegion): down the level's BSP from its root.
#pragma once

// One BSP node as the zone search needs it.
struct FFogNode
{
    FLOAT Plane[4];      // X, Y, Z, W: a point is in front where X*x + Y*y + Z*z - W >= 0
    INT   Back;          // the children; -1 where there is none
    INT   Front;
    UINT  Zones;         // the zone behind in the low byte, the zone in front in the next
    UINT  Pad;
};

// One light's fog, as the pass reads it.
struct FFogLight
{
    FLOAT Pos[3];        // the world
    FLOAT Radius;
    FLOAT Colour[3];     // the fog's colour per unit of tau
    FLOAT Alpha;         // and how much it covers, per unit of tau
    UINT  Zone;
    UINT  Pad[3];
};

class FFogRooms
{
public:
    FFogRooms() {}
    ~FFogRooms() { Shutdown(); }

    // The root signature, the view heap and the lights' buffer; the pipeline comes with the scene's format (Record).
    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Root.Get() != nullptr && Heap.Get() != nullptr && LightMapped[0] != nullptr; }

    // The level's BSP, copied to the card by the next Record. False if the device refused the buffers.
    UBOOL SetNodes(ID3D12Device* Device, const FFogNode* Nodes, INT Count);
    INT   NodeCount() const { return Nodes ? NodesCount : 0; }

    // Where the pixels are: the engine's screen centre and projection in its own pixels, engine pixels to one of the
    // scene target's, and the view's place and axes in the world.
    struct FView
    {
        FLOAT Screen[2];
        FLOAT Focal;
        FLOAT EngPerPixel[2];
        FLOAT Eye[3];
        FLOAT AxisX[3], AxisY[3], AxisZ[3];
        UINT  CameraZone;
    };

    // Lays the fog over the scene target the caller has bound: fog + target * (1 - alpha), as the engine's own fog.
    // Depth is the world's depth (reversed: 1 / view depth), readable by a pixel shader, in the format DepthRead.
    // Water, where the frame has it, is the see-through water's own depth (the same reversal, R32): the fog stops at
    // the water's surface instead of going on to the bed under it.
    // SceneFormat is the bound target's: the pipeline is made again when it changes. False where it could not be.
    // Lights: every light of the frame's fog, as many as there are - their list is made longer here where a frame
    // holds more. False as well where that could not be done (LightsRefused): none of the fog is laid then.
    // Mirror: null for the world's own view. For a mirror's view - the world seen from behind the mirror, View and
    // Depth then that view's own - the mirror's plane in the view's space (X, Y, Z, W: a point p is on it where
    // X*p.x + Y*p.y + Z*p.z = W): the fog is laid from the mirror on only - the way from the eye to the mirror is
    // the world's own view's to lay - and by the zone the way comes out into at the mirror, not by View's.
    UBOOL Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, DXGI_FORMAT SceneFormat, ID3D12Resource* Depth,
                 DXGI_FORMAT DepthRead, ID3D12Resource* Water, const FView& View, const FFogLight* Lights, INT Count,
                 INT FrameIndex, const FLOAT* Mirror = nullptr);
    // The lights' list could not be made as long as a frame needed. The caller then leaves the fog to the engine for
    // good: with a part of the lights the fog of the others would be missing, and which ones would go by where the
    // eye stands.
    UBOOL LightsRefused() const { return bLightsRefused; }

private:
    // A list per frame in flight, each with a first room - for the world's view, and as many again for a mirror's:
    // both are laid in one frame, and each one's list has to stay as it was when its fog was recorded.
    enum { FRAMES = 3, VIEWS = 2, SLOTS = FRAMES * VIEWS, FIRST_LIGHTS = 16 };
    // A frame slot's list made long enough for Wanted lights, twice its room at least. False where the device
    // refuses; the list is then as it was.
    UBOOL HoldLights(ID3D12Device* Device, INT Slot, INT Wanted);

    TComRef<ID3D12RootSignature>  Root;
    TComRef<ID3D12PipelineState>  Pso;
    DXGI_FORMAT                   PsoFormat = DXGI_FORMAT_UNKNOWN;
    TComRef<ID3DBlob>             Vs, Ps;       // kept for a pipeline made again in another format
    UBOOL EnsurePso(ID3D12Device* Device, DXGI_FORMAT SceneFormat);
    // [0] the depth, [1] the nodes, [2] the water's own depth; [3] to [5] the same of a mirror's view.
    TComRef<ID3D12DescriptorHeap> Heap;
    UINT                          HeapStep = 0;
    TComRef<ID3D12Resource>       Nodes;        // on the card
    TComRef<ID3D12Resource>       NodesUpload;  // kept until the copy has been made
    // The levels before: a frame still on the card may read them, so they go two level changes later.
    TComRef<ID3D12Resource>       OldNodes[2], OldUpload[2];
    INT                           NodesCount   = 0;
    UBOOL                         bNodesToCopy = 0;
    TComRef<ID3D12Resource>       LightUpload[SLOTS];       // upload buffers, kept mapped
    void*                         LightMapped[SLOTS] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    INT                           LightRoom[SLOTS]   = { 0, 0, 0, 0, 0, 0 };    // lights each slot's list holds
    UBOOL                         bLightsRefused = 0;       // see LightsRefused
    ID3D12Resource*               DepthSeen[VIEWS]   = { nullptr, nullptr };
    ID3D12Resource*               WaterSeen    = nullptr;
    UBOOL                         bWaterSeen   = 0;      // slot 2 has been written at all
    DXGI_FORMAT                   DepthFormat[VIEWS] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
    INT                           ViewsAt[VIEWS]     = { 0, 0 };    // RdxViewGeneration at a depth's view's making
};

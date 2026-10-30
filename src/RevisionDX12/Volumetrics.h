// SecondRevision - smoke and gas as a volume, walked ONCE a pixel over ALL of the frame's puffs.
//
// The game draws a cloud as a stack of flat cards, each always turned to face the camera. The first attempt here
// kept the cards and walked a volume inside each one, and that was wrong in three ways at once, all from the same
// cause: a card is a rectangle on the SCREEN and the volume in it is in the WORLD.
//
//   - the card cut the volume along a line that travelled with the head, so the smoke seemed to start where the
//     player looked;
//   - two cards that overlapped cut into one another instead of adding up;
//   - and a column of smoke is dozens of cards, so it was dozens of walks over most of the screen - which took a
//     frame from 180 down to 50.
//
// So the cards are not drawn at all. The draw path GATHERS them (FBackend::AddCloudPuff) and this pass walks every
// pixel once, asking at each step the puffs its ray can meet - listed by tile of the target, because a level places
// smoke by the hundred puffs and a pixel's ray meets a handful. Nothing here is per card: the cost is the screen times
// the steps, and the puffs only add to the work inside a step. Two puffs that overlap simply add up, and two clouds
// at opposite ends of a level never meet because neither reaches the other's part of the ray.
//
// At HALF the scene's width and height, because smoke has no edge to lose: the result is laid over the picture with
// a bilinear read, and what that costs in sharpness a cloud does not have to begin with.
#pragma once

#include "RenderLimits.h"

class FTextureCache;

class FVolumetrics
{
public:
    FVolumetrics() {}
    ~FVolumetrics() { Shutdown(); }

    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Walk.Get() != nullptr; }
    // A diagnostic (FBackend::NameResources): the target, the view heap and the lists named, for DRED's report.
    void  NameResources();

    // The half-size target and the buffers the puffs are uploaded through. Called with the scene's own size; a
    // change of size makes them again. False if the device refused, and then the pass is skipped for good.
    UBOOL EnsureTargets(ID3D12Device* Device, INT SceneW, INT SceneH);
    void  ReleaseTargets();

    // What one walk needs to know, all of it in view space except the axes, which say how to get from view space to
    // the world - and the world is where the noise lives, so that it stands still while the camera moves.
    struct FWalk
    {
        FLOAT Screen[2];      // the engine's screen centre, in its own pixels
        FLOAT Focal;          // the engine's projection, in its own pixels
        FLOAT EngPerPixel[2]; // engine pixels to one pixel of this half-size target
        FLOAT Time;           // seconds, for the drift and the curl
        FLOAT EyeWorld[3];
        FLOAT AxisX[3], AxisY[3], AxisZ[3];   // the view's axes in the world
        FLOAT LampView[3];    // the nearest lamp, view space
        FLOAT LampReach;
        FLOAT LampHue[3];
        FLOAT LampLit;
        FLOAT Grain;          // repeats of the noise per unit of the level
        FLOAT Ambient;
        FLOAT Rise;           // units a second the smoke climbs through the noise
        FLOAT Warp;           // how far the slow swirl bends the noise - what makes it curl instead of slide
        FLOAT SelfShade;      // how far towards the lamp to look for smoke in the way, the level's units
        FLOAT StepWorld;      // the length one step aims for; Steps is only the ceiling on their number
        INT   Steps;
        // The light shafts. Lamps is LampsHeld rows of two float4: (xyz the lamp in VIEW space, w its reach),
        // then (rgb its hue times its brightness, w which cube of the shadow array is its).
        const FLOAT* Lamps;
        INT   LampCount;      // 0: no shafts, and then nothing here is read
        // Which lamps a pixel's ray can meet, by tile of the target as the puffs' lists are (Record's Tiles): where
        // each tile's list starts and how many it holds, then the lists, each a lamp's place in Lamps - a lamp
        // wherever the ball of its reach can show. The shafts ask a pixel only its tile's lamps, so what they cost
        // goes by the lamps at the pixel and not by how many the frame has.
        const UINT* LampTiles;
        INT   LampTileUints;
        INT   ShaftSteps;
        FLOAT ShaftStrength;
        FLOAT ShaftNear;      // FShadowMaps::GetNear
        FLOAT ShaftMaxZ;      // no shaft is gathered past this view depth
        FLOAT ShaftBias;
        FLOAT ShaftG;         // how much of the scattering goes forwards
        // The waterfalls' columns (Water=Low and up), FallCount of them, FALL_FLOATS each, in the WORLD: the centre on
        // the level (x, y), the foot and the top (z); the direction across (x, y) and the half-widths along it and
        // across it; the water's own light, and 1 where the column comes down into water; the foam's light around its
        // foot, and how strongly the column glints. FBackend::DrawVolumetrics makes them from the level's sheets.
        const FLOAT* Falls;
        INT   FallCount;      // 0: no columns, and then nothing here is read
        FLOAT FallDensity;    // how fast the view closes up in a column's thickest water, per unit
        FLOAT FallAmbient;    // the light around a column, besides its nearest lamp
        FLOAT FallRate;       // how many streaks a second go by a point of it
        // The share of their cover of a pixel that the smoke and the columns tell the upscaler's reactive mask
        // (Record's Reactive), 0 for nothing told.
        FLOAT SmokeReactive;
        FLOAT FallReactive;
        // The standing fires throw sparks (the shader's Sparks).
        UBOOL bSparks;
        // The fires' flame bodies (the Fire3D setting, or the marker SecondRevision-fire3d.txt), FireCount of them,
        // FIRE_FLOATS each, in the WORLD: the middle on the level (x, y), the foot and the top (z); the direction along it
        // (x, y), the half-length along it and the half-width across it; the flame's own tint and a seed; which of the
        // game's flames it is, how bright, how many times stronger it glows by the unit, and how many times coarser its
        // grain is (RdxMath::FlameBodiesMake). FBackend::DrawVolumetrics makes them from the frame's pieces of flame.
        // They give light and hide almost nothing.
        const FLOAT* Fires;
        INT   FireCount;      // 0: no fires, and then nothing here is read
        // Which fires a pixel's ray can meet, by tile of the target as the puffs' lists are (Record's Tiles): where
        // each tile's list starts and how many it holds, then the lists, each a body's place in Fires - every body
        // wherever it can show (RdxMath::FlameBodyTiles). The walk asks a pixel only its tile's fires, so what a
        // flame costs goes by the flames at the pixel and not by how many burn in the frame.
        const UINT* FireTiles;
        INT   FireTileUints;
        FLOAT FireGlow;       // how bright the flame's hottest part is, per unit of the ray inside it
        FLOAT FireRate;       // how fast the flame's grain rises, repeats a second
        // A view through a mirror begins at its mirror: the mirror's plane in the view's space - its normal, and how
        // far along it the plane lies; all zero where the view is no mirror's. No ray is walked before the plane:
        // that view's eye stands behind its mirror, in whatever is there.
        FLOAT NearPlane[4];
        // The part of the target the view shows in, in the target's own pixels: left, top, and one past the right
        // and the bottom. Outside it nothing is walked, and nothing written but the empty pixel. The whole target
        // where the right is not past the left.
        INT   Window[4];
        // The weapon lamps' beams in the air (Flashlight=Everybody; the shader's OwnBeams): the own lamps' rows as the
        // world's passes read them (FBackend::OwnRowsNow, OWN_FLOATS of them: the head, then each lamp's seven, in the
        // WORLD), OwnCount lamps - the flashlight's own among them left out -, and their maps (FShadowMaps's
        // GetSpotMaps, readable by a compute shader) with their view (DescribeSpotView) and their making (SpotsMade),
        // by which the view is written again. OwnDust and OwnForward: RdxMath::OwnBeamDust and OwnBeamForward.
        const FLOAT*                           Own;
        INT                                    OwnCount;     // 0: no beams, and then nothing here is read
        ID3D12Resource*                        OwnMaps;
        const D3D12_SHADER_RESOURCE_VIEW_DESC* OwnMapsView;
        INT                                    OwnMapsMade;
        FLOAT                                  OwnDust;
        FLOAT                                  OwnForward;
    };

    // Fills the half-size target. Depth is the scene's depth as the water's copy holds it (one over the view depth),
    // and must already be readable by a shader. Puffs are FBackend::FCloudPuff, twelve floats each, as many as the
    // frame gathered - the lists have no top, and are made larger here where a frame holds more. Tiles says which puffs
    // a pixel's ray can meet: for each tile of the target (RdxMath::CloudTile pixels a side, TileCols of them across)
    // where its list starts and how many it holds, then the lists, each a puff's place in Puffs. The waterfalls'
    // columns ride in the walk (FWalk::Falls), and so do the fires' flame bodies (FWalk::Fires) - as many as the
    // frame holds, their list made larger here the same way.
    // False where a list of the smoke could not be made as large as the frame needs: the smoke is not walked then
    // (the rest is), and the caller goes back to the game's own pictures. The fires' list the same: FiresHeld.
    // DepthFormat is the format the depth copy's views are made with, which is NOT the resource's own: with the
    // shadows on that buffer carries a stencil, and a view of it must say so. A view whose format does not match
    // the resource is not a wrong picture - it is a device removed.
    // Cubes is FShadowMaps::GetWorldCubes and CubeView its DescribeCubeView; both may be null/unused when no
    // shafts are asked for. The cubes must already be readable by a shader.
    // Reactive: the temporal upscaler's reactive mask at the scene's full size, in UNORDERED_ACCESS - the fires write
    // it where they burn (their flames have no motion vectors), the smoke and the columns where a share is asked of
    // them (FWalk::SmokeReactive, FallReactive); null where there is none to write.
    // View: which of the frame's views is walked - 0 the world's own, from 1 on a further one, a mirror's. Each has
    // lists and numbers of its own in the frame slot (FViewSlot), so a frame may walk as many views as it has. The
    // further views read a depth of their own - the depth buffer where such a view stands drawn - through a table
    // apart from the world's, which reads the depth copy: one resource for all of a frame's further views.
    UBOOL Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Depth, DXGI_FORMAT DepthFormat,
                 const void* Puffs, INT PuffCount, const UINT* Tiles, INT TileUints, INT TileCols, const FWalk& Walk,
                 INT FrameSlot, ID3D12Resource* Cubes, const D3D12_SHADER_RESOURCE_VIEW_DESC* CubeView,
                 ID3D12Resource* Reactive, INT View = 0);

    // MAX_LAMPS: the shafts' lamps are of the shadows' lights (RdxMath::ShaftLampPick), and of those there are no
    // more than a preset may ask for.
    // OWN_FLOATS: the own lamps' rows, as many as FSolidPass's.
    enum { MAX_LAMPS = RDX_SHADOW_MAX_LIGHTS, LAMP_FLOATS = 8, MAX_FALLS = 16, FALL_FLOATS = 16, FIRE_FLOATS = 16,
           HAZE_CONSTANTS = 36, OWN_FLOATS = 4 * (RDX_OWN_HEAD_ROWS + RDX_OWN_LAMP_ROWS * RDX_OWN_LAMPS) };

    // The half-size result laid over the finished picture. It goes on AFTER the shadows and the occlusion, which is
    // not a detail: a shadow belongs to the wall behind the smoke, and a wall's shadow drawn ONTO the smoke in front
    // of it is the oldest tell there is. Drawn as one triangle over the whole target, blended src + dst*(1-cover).
    UBOOL InitComposite(ID3D12Device* Device, DXGI_FORMAT SceneFormat);
    // The heat haze's pass made now rather than at its first use (FBackend::WarmShaders), by its own flag.
    UBOOL WarmHaze(ID3D12Device* Device, DXGI_FORMAT SceneFormat)
    {
        if (HazeReady == 0)
            HazeReady = InitHaze(Device, SceneFormat) ? 1 : -1;
        return HazeReady > 0;
    }
    void  RecordComposite(ID3D12GraphicsCommandList* Cmd);
    UBOOL HasComposite() const { return Lay.Get() != nullptr; }

    // The air trembling over the standing fires: what is seen through the hot air above each flame
    // is bent a little this way and that, by a grain that rises with the heat. Scene is the finished picture - the
    // walk's result already laid on it - in RENDER_TARGET, Rtv its view of SceneFormat, View the part of it the world
    // is drawn in. The pass reads a copy of the picture and writes the picture: each fire's own part of the screen is
    // copied first, and no more. After a Record of this frame, whose depth must be readable by a pixel shader here.
    // FurtherView: after a further view's Record (a mirror's), by that view's own depth.
    void  RecordHaze(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Scene,
                     DXGI_FORMAT SceneFormat, D3D12_CPU_DESCRIPTOR_HANDLE Rtv, const D3D12_RECT& View,
                     const FWalk& Walk, UBOOL bFurtherView = 0);

    // Whether the last Record walked at all. Where it had nothing to walk - or could not - the target still holds
    // an earlier frame's result, and laying that over the picture would show smoke and light that are gone.
    UBOOL           Wrote() const { return bWrote; }
    // Whether the last Record had room for all the fires it was given. Where their lists could not be made as long
    // as the frame needed, none of them was walked.
    UBOOL           FiresHeld() const { return bFiresHeld; }
    // The same for the shafts' lamps: where their lists by tile could not be made as long as the frame needed, no
    // shaft was drawn.
    UBOOL           LampsHeld() const { return bLampsHeld; }
    ID3D12Resource* Target() const { return Result.Get(); }
    INT             TargetW() const { return HalfW; }
    INT             TargetH() const { return HalfH; }
    // A side of the target from the scene's: half of it, rounded up.
    static INT      TargetSide(INT Scene) { return (Scene + 1) / 2; }

    // Whether the fires can write the reactive mask here at all (the device's typed loads, Init).
    UBOOL WritesReactive() const { return bTypedLoad; }
    // The mask was made anew - possibly at the very address of the one before, so its view is made again regardless.
    void  ForgetReactive() { bReactiveStale[0] = bReactiveStale[1] = 1; }

private:
    // Table: 0 the world's view's table, 1 the further views'.
    void WriteReactiveView(ID3D12Device* Device, ID3D12Resource* Mask, INT Table);
    // The shadow cubes' view the same way: the level's cubes, or a view of nothing while the shafts have none to read.
    void WriteCubeView(ID3D12Device* Device, ID3D12Resource* Cubes, const D3D12_SHADER_RESOURCE_VIEW_DESC* View);
    // And the weapon lamps' maps', Made their making (FWalk::OwnMapsMade).
    void WriteOwnMapsView(ID3D12Device* Device, ID3D12Resource* Maps, const D3D12_SHADER_RESOURCE_VIEW_DESC* View,
                          INT Made);
    // A frame slot's list made large enough for Wanted items of Stride bytes, twice its size at least; the smaller
    // buffer is let go - the slot's last frame is done on the GPU by the time a frame records into it. False where
    // the device refuses, and the list is then as it was.
    UBOOL GrowList(ID3D12Device* Device, TComRef<ID3D12Resource>& Buffer, void*& At, INT& Room, INT Wanted, INT Stride,
                   const TCHAR* What);

    // FURTHER: where the further views' table begins in the heap - the world's own places over again.
    enum { FRAMES = 3, FIRST_PUFFS = 256, PUFF_FLOATS = 12, FIRST_TILE_UINTS = 16384, FIRST_FIRES = 128,
           WALK_CONSTANTS = 64, FURTHER = 7, HEAP_VIEWS = 14 };

    TComRef<ID3D12RootSignature> Root;
    TComRef<ID3D12PipelineState> Walk;
    TComRef<ID3D12RootSignature> LayRoot;
    TComRef<ID3D12PipelineState> Lay;
    DXGI_FORMAT                  LayFormat = DXGI_FORMAT_UNKNOWN;        // the scene's format Lay was made for
    // The heat haze (RecordHaze): its pass, made at its first use (HazeReady: 0 not tried, 1 up, -1 refused), and the
    // copy of the picture it reads, the scene's size and format.
    UBOOL InitHaze(ID3D12Device* Device, DXGI_FORMAT SceneFormat);
    TComRef<ID3D12RootSignature> HazeRoot;
    TComRef<ID3D12PipelineState> HazePipe;
    TComRef<ID3D12Resource>      HazeCopy;
    INT                          HazeReady = 0;
    DXGI_FORMAT                  HazePipeFormat = DXGI_FORMAT_UNKNOWN;   // the scene's format HazePipe was made for
    UINT64                       HazeW = 0;
    UINT                         HazeH = 0;
    DXGI_FORMAT                  HazeFormat = DXGI_FORMAT_UNKNOWN;
    TComRef<ID3D12Resource>      Result;        // half size, RGBA16F: the scattered light and how much it covers
    // [0] the scene's depth, [1] the result as a UAV, [2] the result as a texture, for laying it over the picture,
    // [3] the shadow cubes (a view of nothing while there are none), [4] the reactive mask as a UAV (the same),
    // [5] the heat haze's copy of the picture, [6] the weapon lamps' maps (a view of nothing while there are none).
    // From FURTHER on the same places over again for the further views' walks (Record's View): their own depth, and
    // the same result, cubes, mask and maps - their [5] unused.
    TComRef<ID3D12DescriptorHeap> Heap;
    // One view's lists and numbers in a frame in flight: written while the frame is recorded and read when it runs,
    // so no two walks of a frame may share them. Each frame slot's first is the world's view's, made with the
    // targets; a further view's - a mirror's (Record's View) - is made at that view's first walk in the slot, as
    // many as a frame has such views. Kept in memory that moves as the list grows: nothing in one points at itself,
    // and one of all zeroes is an empty one. The puffs', their tiles' and the fires' are made for a first room and
    // made larger when a frame holds more (GrowList).
    struct FViewSlot
    {
        TComRef<ID3D12Resource> Upload;             // the puffs
        TComRef<ID3D12Resource> TileUpload;         // their lists by tile
        TComRef<ID3D12Resource> LampUpload;         // the shafts' lamps
        TComRef<ID3D12Resource> LampTileUpload;     // their lists by tile
        TComRef<ID3D12Resource> FallUpload;         // the waterfalls' columns
        TComRef<ID3D12Resource> FireUpload;         // the fires' flame bodies
        TComRef<ID3D12Resource> FireTileUpload;     // their lists by tile
        TComRef<ID3D12Resource> ConstUpload;        // the walk's own numbers (WALK_CONSTANTS of them)
        TComRef<ID3D12Resource> OwnUpload;          // the weapon lamps' rows (OWN_FLOATS of them)
        void* Mapped;
        void* TileMapped;
        void* LampMapped;
        void* LampTileMapped;
        void* FallMapped;
        void* FireMapped;
        void* FireTileMapped;
        void* ConstMapped;
        void* OwnMapped;
        INT   Room;                                 // puffs the list holds
        INT   TileRoom;                             // numbers the puffs' tile lists hold
        INT   LampTileRoom;                         // numbers the lamps' tile lists hold
        INT   FireRoom;                             // fires the list holds
        INT   FireTileRoom;                         // numbers the fires' tile lists hold
    };
    FViewSlot*                   Slots[FRAMES]     = { nullptr, nullptr, nullptr };
    INT                          SlotCount[FRAMES] = { 0, 0, 0 };
    // A slot's buffers made and mapped, each with its first room; false where the device refuses one, and the slot
    // is then empty again. FreeSlot lets them go.
    UBOOL      MakeSlot(ID3D12Device* Device, FViewSlot& S);
    void       FreeSlot(FViewSlot& S);
    // A view's slot in a frame slot, made where the frame has not walked so many views before; null where it cannot be.
    FViewSlot* ViewSlot(ID3D12Device* Device, INT Frame, INT View);
    UINT                         HeapStep = 0;      // one descriptor's size in this heap
    INT                          HalfW = 0, HalfH = 0;
    INT                          SceneW = 0, SceneH = 0;
    ID3D12Resource*              DepthSeen = nullptr;   // whose view the heap holds; a new one needs a new view
    ID3D12Resource*              FurtherDepthSeen = nullptr;    // the same for the further views' table
    INT                          ViewsAt   = 0;         // RdxViewGeneration when the views were written
    ID3D12Resource*              CubeSeen  = nullptr;
    UBOOL                        bCubeStale = 0;        // the cubes were made anew or let go since the view was written
    ID3D12Resource*              OwnMapsSeen     = nullptr;   // the weapon lamps' maps the same way
    INT                          OwnMapsSeenMade = -1;
    UBOOL                        bOwnMapsStale   = 0;
    // By table: the world's view's, the further views'.
    ID3D12Resource*              ReactiveSeen[2]   = { nullptr, nullptr };
    UBOOL                        bReactiveStale[2] = { 0, 0 };
    UBOOL                        bTypedLoad = 0;
    UBOOL                        bWrote = 0;            // see Wrote
    UBOOL                        bFiresHeld = 1;        // see FiresHeld
    UBOOL                        bLampsHeld = 1;        // see LampsHeld
};

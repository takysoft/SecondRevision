// SecondRevisionHost64 - the rays (docs/terv-rt-host.md, Host64Protocol.h: Service_Rays).
//
// NVIDIA's driver gives a 32-bit process no ray tracing at all, so on those cards the renderer cannot trace a ray; this
// process can. What the rays are traced against comes over in shared buffers - the level's triangles once a share, the
// frame's bodies every frame - and the structures are made here, the way FRtPass makes the renderer's own: the level, the
// bodies, the sky's walls and the see-through surfaces an instance each, with FRtPass's numbers and masks. The passes that
// trace run here as well, into shared textures the renderer reads:
//   - the shadows' factor and the occlusion: the renderer's own shader (ShadowFactor.hlsl, built here with RDX_RT as the
//     renderer builds it) with the renderer's own constants and tables - bytes this process only passes on;
//   - the list: every ray the renderer's own pixel shaders would trace, written down in a buffer and answered in
//     another with what it met (RayList.hlsl) - for what the renderer shades itself, where the textures are. And
//     what those shaders trace on to from there - the lamps' light at the place, the see-through surfaces on the
//     ray's way - by the renderer's own tables of the lamps and the level's surfaces, copied over.
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "Host64Protocol.h"

class FHostRays
{
public:
    typedef void (*FLogFunc)(const char* Line);

    ~FHostRays() { Shutdown(); }

    // Asks the device whether it traces rays the inline way, and makes the passes' pipelines. False: no rays on this
    // card or driver - Why says why. A pass whose pipeline would not come up is only left out of Passes.
    bool Load(ID3D12Device* Device, FLogFunc Log, char* Why, size_t WhyChars);
    // The passes that came up: Host64::ERayPass bits.
    uint32_t Passes() const { return Ready; }
    // A share. What the last one brought is let go first - the caller has the GPU done with it - and this one's is
    // opened; the handles are closed whatever comes of it. False: something of it would not open, and its frames are
    // refused.
    bool Take(const Host64::FRayShare& Share);
    // The rays of submission Frame into List: the level's structures where the share's have not been made yet, the
    // bodies' from the frame's list, the scene, and the passes the list asks for. Every shared texture is in COMMON
    // before and after. False: refused - nothing was written.
    bool Trace(ID3D12GraphicsCommandList* List, uint64_t Frame);
    // Whether the submission Trace last recorded answered the rays' list.
    bool TracedList() const { return bTracedList; }
    // The share's resources and everything made of them, let go: the caller has the GPU done with them.
    void Release();
    void Shutdown();

private:
    struct FBuffer
    {
        ID3D12Resource* Res   = nullptr;
        UINT64          Bytes = 0;
    };
    // A buffer the commands of frames still in flight may hold: let go once the frame FrameSlots after it is recorded -
    // the host has the GPU done with a slot's frame before it records the next one there.
    struct FRetired
    {
        ID3D12Resource* Res;
        uint64_t        Frame;
    };
    enum { SRV_COUNT = 15, RTV_COUNT = 3, INSTANCES = 4 };

    void Say(const char* Format, ...);
    bool MakePipelines();
    bool MakeViews();
    bool Ensure(FBuffer& Buffer, UINT64 Bytes, D3D12_RESOURCE_STATES State, const char* What, uint64_t Frame);
    void Retire(FBuffer& Buffer, uint64_t Frame);
    void Bury(uint64_t Frame, bool bAll);
    bool BuildStatic(ID3D12GraphicsCommandList4* List, int Resource, uint32_t Vertices, FBuffer& Blas, const char* What,
                     uint64_t Frame);
    bool BuildBodies(ID3D12GraphicsCommandList4* List, const Host64::FRayFrame& Head, const Host64::FRayBody* Bodies,
                     uint64_t Frame);
    bool BuildScene(ID3D12GraphicsCommandList4* List, uint64_t Frame);
    void DrawPass(ID3D12GraphicsCommandList* List, uint32_t Pass, const Host64::FRayFrame& Head);
    void ListPass(ID3D12GraphicsCommandList* List, const Host64::FRayFrame& Head);
    UINT64 BufferBytes(int Resource) const;

    ID3D12Device5*        Device   = nullptr;
    FLogFunc              Log      = nullptr;
    uint32_t              Ready    = 0;          // ERayPass bits: the pipelines that came up
    ID3D12RootSignature*  FactorRoot = nullptr;  // the factor's and the occlusion's (ShadowFactor.hlsl)
    ID3D12PipelineState*  FactorPso  = nullptr;
    ID3D12PipelineState*  AoPso      = nullptr;
    ID3D12RootSignature*  ListRoot   = nullptr;  // the list's (RayList.hlsl), a compute pass
    ID3D12PipelineState*  ListPso    = nullptr;
    ID3D12DescriptorHeap* SrvHeap    = nullptr;  // the share's views, made as it is taken
    ID3D12DescriptorHeap* RtvHeap    = nullptr;
    UINT                  SrvStep    = 0;
    UINT                  RtvStep    = 0;

    // The share.
    ID3D12Resource*          Shared[Host64::Ray_Count] = {};
    const void*              Frames       = nullptr;   // the frames' lists, mapped
    uint32_t                 BodyRoom     = 0;
    uint32_t                 StaticVertices[3] = {};   // corners in Ray_Level, Ray_Sky, Ray_Thin
    bool                     bShare       = false;
    bool                     bTracedList  = false;
    bool                     bStaticDirty = false;     // the level's structures are yet to be made of this share

    // The structures. One of each, not one a frame slot: the commands run in the order they were recorded, so a frame's
    // passes are done with the scene before the next frame's build writes it. What the CPU writes for a frame - the
    // instances - has a place a slot.
    FBuffer                  StaticBlas[3];            // the level, the sky's walls, the see-through surfaces
    bool                     bStaticBuilt[3] = {};
    FBuffer                  BodyBlas, BodyScratch, Tlas, TlasScratch;
    bool                     bBodies      = false;     // the frame being recorded has bodies in its structure
    bool                     bSceneStands = false;     // the last traced frame's scene was made, of SceneBodies bodies
    uint32_t                 SceneBodies  = 0;
    ID3D12Resource*          Instances    = nullptr;
    D3D12_RAYTRACING_INSTANCE_DESC* InstancesMapped = nullptr;
    D3D12_RAYTRACING_GEOMETRY_DESC* Geos  = nullptr;   // the bodies' geometries, as long as the longest frame asked
    uint32_t                 GeoRoom      = 0;
    FRetired*                Retired      = nullptr;
    uint32_t                 RetiredCount = 0;
    uint32_t                 RetiredRoom  = 0;
    uint64_t                 Traced       = 0;         // frames traced, for the log's occasional line
};

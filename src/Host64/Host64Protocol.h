// SecondRevision - the 64-bit host's protocol (docs/terv-fsr4.md).
//
// The renderer runs inside the game's 32-bit process, and the libraries it wants to reach - FSR 4 and AMD's frame
// generation today, DLSS or XeSS one day - exist only as 64-bit DLLs, which a 32-bit process cannot load. So they live
// in a process of their own, SecondRevisionHost64.exe, and the two talk through this block of shared memory:
//   - the pictures never leave the card: the renderer makes the textures, shares them (NT handles duplicated into the
//     host), and the host opens the same memory with its own device on the same adapter;
//   - the two GPU queues wait on each other through two shared fences, so neither CPU waits for the other;
//   - what a frame needs besides the pictures - the jitter, the camera, the time - goes through here, a few hundred
//     bytes a frame, with an event to wake the host.
//
// Nothing here knows which vendor's library does the work. What every temporal upscaler and frame generator takes is
// the same - colour, depth, motion vectors, the jitter, the camera - and the conventions (the units of the jitter, the
// scale and sign of the motion vectors, the depth's direction) are fixed HERE, once; a backend in the host translates
// them for its SDK.
//
// The same bytes in both builds: fixed-width fields only, no pointers, nothing whose size depends on the build, laid out
// so that no padding appears anywhere (the static_asserts below hold both compilers to it). A handle is carried as a
// 64-bit number, valid in the process named beside it.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Host64
{
    // Bumped whenever the layout or the meaning of a field changes. The host refuses a block of another version, and
    // the renderer then runs its own FSR 3.1.
    enum { ProtocolVersion = 10 };
    enum : uint32_t { Magic = 0x34365848u };      // "HX64"

    // How many submissions can be in flight: a slot is Frame % FrameSlots. A frame the engine draws is up to three
    // submissions (its rays, the upscale, the generated picture), and the renderer runs up to three frames ahead - with
    // room to spare, because a renderer that finds no free slot gives the host up.
    enum { FrameSlots = 16 };
    enum { MessageChars = 256 };

    // What the host can do for a frame. More than one at a time: an upscale and then the generated frame from its
    // output. Frame generation comes in two halves because it reads two things at two times: FramePrepare takes the
    // frame's depth, motion and camera - in the upscale's submission, while those textures still hold this frame (the
    // next frame's copies overwrite them) - and FrameGen generates the frame from the finished pictures, in a submission
    // of its own at the frame's end. FramePrepare | FrameGen in one submission does both.
    // Rays: the frame's scene is made of what the rays' share holds, and the passes the frame asks for are traced
    // against it (FRayFrame) - a submission of its own in the middle of a frame, before what reads their results is drawn.
    enum EService : uint32_t
    {
        Service_Upscale      = 1u << 0,
        Service_FrameGen     = 1u << 1,
        Service_FramePrepare = 1u << 2,
        Service_Rays         = 1u << 3,
    };

    // Whose library does it. Test: a plain copy, no library at all - the pipeline's own check.
    enum EBackend : uint32_t
    {
        Backend_None = 0,
        Backend_Test = 1,
        Backend_Fsr  = 2,
        Backend_Dlss = 3,
        Backend_Xess = 4,
    };

    enum EHostState : uint32_t
    {
        HostState_Starting = 0,   // the block is written, the host has not answered yet
        HostState_Ready    = 1,   // the host took the block, has its device and waits for frames
        HostState_Failed   = 2,   // the host gave up; Message says why
        HostState_Exited   = 3,   // the host left on its own (the renderer asked it to, or its parent is gone)
    };

    // The shared textures, by what they hold.
    enum EResource : uint32_t
    {
        Resource_Color = 0,       // the scene, at the render size
        Resource_Depth,           // its depth, one float a pixel: 1 nearest, 0 the infinite far plane
        Resource_Motion,          // its motion vectors: where each pixel was on the last frame, in pixels, minus where it is
        Resource_Reactive,        // how much less of the history to keep (what moves without vectors); may be absent
        Resource_Output,          // the upscaled picture, at the display size
        Resource_Present,         // the finished picture as it is shown, HUD and all, at the display size, in the back
                                  // buffer's format: what frame generation generates from
        Resource_HudLess,         // the same picture without the HUD: where the two differ is the HUD, which frame
                                  // generation then keeps as it is instead of smearing it; may be absent
        Resource_Ui,              // the HUD alone, over transparency, for frame generation; may be absent
        Resource_Generated,       // the generated frame, at the display size, in the back buffer's format
        Resource_Count
    };

    // Per frame.
    enum EFrameFlags : uint32_t
    {
        FrameFlag_Reset    = 1u << 0,   // the camera jumped: throw the history away
        FrameFlag_Hdr      = 1u << 1,   // the colour is linear light above 1.0 (HDR output), not display-referred
        FrameFlag_Reactive = 1u << 2,   // Resource_Reactive holds this frame's mask; without the flag there is none
        FrameFlag_ScRgb    = 1u << 3,   // Resource_Present is linear scRGB (the HDR output); without it, sRGB-encoded
    };

    struct FResource
    {
        uint64_t Handle;          // the NT handle, valid in the HOST's process; 0: none
        uint32_t Width;
        uint32_t Height;
        uint32_t Format;          // a DXGI_FORMAT
        uint32_t Reserved;
    };

    struct FFrame
    {
        uint64_t Frame;           // counts up by one from 1; the fences' values are the frame numbers
        uint32_t Services;        // EService bits
        uint32_t Flags;           // EFrameFlags bits
        uint32_t RenderWidth;     // the part of the inputs drawn this frame
        uint32_t RenderHeight;
        uint32_t OutputWidth;
        uint32_t OutputHeight;
        float    JitterX;         // the sub-pixel offset the scene was drawn with, in render pixels
        float    JitterY;
        float    FrameTimeMs;     // since the previous frame
        float    FovY;            // the vertical field of view, radians
        float    NearZ;           // the near plane, in the level's units
        float    Sharpness;       // 0..1, for the backends that sharpen
        float    UnitsToMeters;   // how many metres one of the level's units is
        float    Reserved0;
        float    CameraPosition[3];   // in the level's space - frame generation wants the camera
        float    CameraUp[3];
        float    CameraRight[3];
        float    CameraForward[3];
    };

    // The rays (docs/terv-rt-host.md). A 32-bit process gets no ray tracing from NVIDIA's driver, so on those cards the
    // renderer cannot trace a ray itself; the host can. The renderer hands over what the rays are traced against and what
    // their passes read, the host makes the structures and runs the passes, and the renderer reads what they wrote.
    //
    // What is shared for them, by what it holds. The buffers are committed buffers on shared heaps, as the textures
    // are; the renderer writes them with copies on its queue before a frame's submission, and the host reads them on
    // its own. A corner of a triangle is three floats at the head of a vertex, in the level's space, three corners a
    // triangle, no indices - the way FRtPass makes its own structures.
    enum ERayResource : uint32_t
    {
        Ray_Level = 0,        // buffer: the level's triangles, twelve bytes a corner
        Ray_Sky,              // buffer: the sky's walls, the same way; may be absent
        Ray_Thin,             // buffer: the level's see-through surfaces, the same way; may be absent
        Ray_FrameXyz,         // buffer: the frame's bodies - written anew every frame (FRayBody says where each is)
        Ray_ArenaXyz,         // buffer: the bodies whose triangles stay from frame to frame; may be absent
        Ray_Tables,           // buffer: the passes' constants and tables for a frame, as the renderer lays them out
                              // (FRayFrame says where each begins)
        Ray_FrameUvw,         // buffer: the masked bodies' texture places - u, v and the mask's slot, three floats a
                              // corner - for Ray_FrameXyz's vertices; may be absent
        Ray_ArenaUvw,         // buffer: ... and for Ray_ArenaXyz's; may be absent
        Ray_Masks,            // texture array: the masks, slot 0 solid; may be absent
        Ray_WorldDepth,       // texture: the factor's views (ShadowFactor.hlsl, t0-t3). A depth-stencil buffer shared
        Ray_OnTopDepth,       // whole: its depth is read, and Ray_WorldDepth's stencil plane is the factor's third view
        Ray_SceneLight,
        Ray_Requests,         // buffer: the list pass's rays, a slot each (RenderLimits.h, RDX_RAY_*); slot 0 counts
                              // the slots taken. Written by the renderer's own pixel shaders.
        Ray_Answers,          // buffer, written: what each request's ray met, in the request's own slot
        Ray_Factor,           // texture, written: the shadows' factor (R8)
        Ray_Occlusion,        // texture, written: the occlusion (R8)
        Ray_Passed,           // buffer, written: the see-through surfaces a ray passed on its way, a slot each as
                              // the list pass takes them; slot 0 counts the slots taken, and the renderer puts it
                              // back at its start before a frame's rays. May be absent
        Ray_Material,         // buffer: the level's surfaces as the renderer's rays read them (FRtPass's LevelMaterial),
                              // for what the list pass traces on from what a ray met; may be absent
        Ray_Water,            // buffer: the mirror rays' table for a frame, as the renderer's rays read it (FRtPass::
                              // PrepareWater) - a row a body, and the level's lamps - for the same; may be absent
        Ray_SurfacePack,      // texture: the see-through water at some of the factor's samples (RG32): x its own
                              // depth - the depth buffer's value of its nearest surface, 0 where there is none - y the
                              // light its surface receives; may be absent
        Ray_SurfaceFactor,    // texture, written: the water surface's factor (R8), over the pack by the factor's own
                              // Scale; may be absent
        Ray_Count
    };

    // The passes a frame's rays can run, in this order.
    enum ERayPass : uint32_t
    {
        RayPass_Shadow = 1u << 0,   // the shadows' factor: the renderer's own shader (ShadowFactor.hlsl, PSMain)
        RayPass_Ao     = 1u << 1,   // the occlusion: the same source's AoPS
        RayPass_List   = 1u << 2,   // what each listed ray met (RayList.hlsl) - for what the renderer shades itself
        RayPass_Surface = 1u << 3,  // the water surface's factor: the shadows' factor's shader again, over the water's
                                    // pack in the world depth's place and the light target's, no surface kind but the
                                    // level's, nothing drawn after the ClearZ
    };

    enum ERayBodyFlags : uint32_t
    {
        RayBody_Arena = 1u << 0,    // its corners are in Ray_ArenaXyz, not in Ray_FrameXyz
        RayBody_Asks  = 1u << 1,    // not opaque: a ray asks the pass about every triangle of it that it meets
    };

    // One body of a frame - a geometry of the bodies' structure, in the list's order (the passes' tables go by it).
    struct FRayBody
    {
        uint32_t Offset;          // bytes from its buffer's start to its first corner
        uint32_t Stride;          // bytes from a corner to the next: twelve or more, a multiple of four
        uint32_t Vertices;        // three a triangle
        uint32_t Flags;           // ERayBodyFlags bits
    };

    // A frame's rays: one of these a frame slot, each followed by room for FRayShare::BodyRoom bodies, in a mapping of
    // their own (FRayShare::Frames) - the list has no fixed length, the renderer shares a longer one when a frame
    // outgrows it. The renderer fills the slot of the frame it is about to submit with Service_Rays.
    struct FRayFrame
    {
        uint32_t Passes;          // ERayPass bits
        uint32_t Bodies;          // how many FRayBody follow
        uint32_t Width;           // the part of the passes' textures this frame draws
        uint32_t Height;
        uint32_t FactorConstsAt;  // where in Ray_Tables, in bytes: the factor's constants (cbuffer Shadow) ...
        uint32_t RayConstsAt;     // ... the rays' own (cbuffer RtShadow) ...
        uint32_t GeoAt;           // ... the geometries' table (RtGeo) ...
        uint32_t ListConstsAt;    // ... and the list pass's constants (cbuffer List). Constants begin on a multiple of
                                  // 256 bytes, tables on a multiple of four.
        uint32_t Flags;           // ERayFrameFlags bits
        uint32_t ListRoom;        // how many slots of the requests' buffer the list pass may read, slot 0 among them
        uint32_t WaterBytes;      // how much of Ray_Water this frame's table takes - with Ray_Material, what the list
                                  // pass traces on by from what a ray met. 0: the frame brought none
        uint32_t PassedRoom;      // how many slots of Ray_Passed the pass may write, slot 0 among them
    };

    enum ERayFrameFlags : uint32_t
    {
        // The scene is the one the submission before this one was traced against - the same frame's other rays: its
        // bodies are listed here as there, and no structure is made again.
        RayFrame_SameScene = 1u << 0,
    };

    // The rays' share: written by the renderer, which then bumps FBlock::RayGeneration; the host lets the old one go,
    // opens this one and answers with the same number in RayAck. The level's structures are made of what a share
    // brings, once: a new level is a new share.
    struct FRayShare
    {
        FResource Resources[Ray_Count];   // a buffer's Width is its size in bytes (Height 1, Format 0)
        uint64_t  Frames;         // a file mapping's handle, valid in the HOST's process: the frames' lists
        uint32_t  BodyRoom;       // how many bodies a list has room for
        uint32_t  LevelVertices;  // corners in Ray_Level
        uint32_t  SkyVertices;    // ... in Ray_Sky
        uint32_t  ThinVertices;   // ... in Ray_Thin
    };

    // The size of one frame's list in the rays' mapping, and of the mapping.
    inline uint32_t RayFrameBytes(uint32_t BodyRoom)
    {
        return (uint32_t)sizeof(FRayFrame) + BodyRoom * (uint32_t)sizeof(FRayBody);
    }
    inline uint32_t RayFramesBytes(uint32_t BodyRoom) { return RayFrameBytes(BodyRoom) * (uint32_t)FrameSlots; }

    struct FBlock
    {
        // Written by the renderer before the host starts.
        uint32_t Magic;
        uint32_t Version;
        uint32_t RendererPid;
        uint32_t AdapterLuidLow;  // the adapter the renderer's device runs on: the host takes the same one
        int32_t  AdapterLuidHigh;
        uint32_t WantBackend;     // an EBackend: whose library the renderer wants the work done by
        uint64_t FenceToHost;     // handles valid in the host's process: the renderer signals the frame's inputs ready
        uint64_t FenceToRenderer; // the host signals the frame's outputs ready
        uint64_t WakeEvent;       // set by the renderer when a frame is waiting
        uint32_t QuitRequested;   // the renderer asks the host to leave - written while the host runs

        // Written by the host.
        uint32_t HostState;       // EHostState
        uint32_t HostPid;
        uint32_t HostVersion;     // the host's own build, for the log
        uint32_t Backends;        // 1 << EBackend for every backend the host found usable on this card
        uint32_t Services;        // EService bits it can run
        uint32_t FramesFailed;    // frames whose work the backend refused: their output is not this frame's picture
        uint32_t FailedInARow;    // ... since the last frame it did not refuse; a long run and the renderer gives up on it
        uint32_t WorkUs;          // the host's own GPU time for an upscale's submission (with frame generation's prepare
                                  // where it rides along), microseconds: a running average, 0 unknown
        uint32_t WorkFgUs;        // ... and for a generated frame's
        char     Message[MessageChars];   // plain ASCII: what went wrong, or what runs

        // The shared textures. The renderer writes them, then bumps ResourceGeneration; the host opens them, makes its
        // backend's context for their sizes - which can take a while, and a frame's work should not - and answers with
        // the same number in ResourceAck. A frame is only sent for the generation the host has acknowledged.
        FResource Resources[Resource_Count];
        uint32_t  ResourceGeneration;
        uint32_t  ResourceAck;

        // The frames: the renderer fills Frames[Frame % FrameSlots], then raises Submitted and sets the event; the host
        // raises Taken once it has submitted that frame's work to its GPU.
        uint64_t Submitted;
        uint64_t Taken;
        FFrame   Frames[FrameSlots];

        // The rays. Shared the way the textures are, with a generation of their own: a level's change shares these
        // anew and leaves the upscaler's textures - and the upscaler made for them - alone.
        FRayShare Rays;
        uint32_t  RayGeneration;
        uint32_t  RayAck;
        // Written by the host.
        uint32_t  RayPasses;      // ERayPass bits: the passes whose pipelines came up on this card - valid once
                                  // Service_Rays is among Services
        uint32_t  WorkRayUs;      // its GPU time for a hand-over of the rays without their list, as WorkUs
        uint32_t  DebugErrors;    // with the debug layer on in the host: how many errors it has had so far
        uint32_t  RayTaken;       // the last generation of the rays' share it could take: one it acknowledged and
                                  // could not take has its frames refused
        uint32_t  WorkListUs;     // its GPU time for a hand-over of the rays' list, as WorkUs
        uint32_t  WorkListTimed;  // how many hand-overs of the list it has timed so far
    };

    // Every field accounted for, so a padding byte anywhere - in either build - fails the compile.
    static_assert(sizeof(FResource) == 8 + 4 * 4, "FResource must be the same in both builds");
    static_assert(sizeof(FFrame) == 8 + 6 * 4 + 8 * 4 + 12 * 4, "FFrame must be the same in both builds");
    static_assert(sizeof(FRayBody) == 4 * 4 && sizeof(FRayFrame) == 12 * 4, "the rays' lists must be the same in both");
    static_assert(sizeof(FRayShare) == sizeof(FResource) * Ray_Count + 8 + 4 * 4,
                  "FRayShare must be the same in both builds");
    static_assert(sizeof(FBlock) == 6 * 4 + 3 * 8 + 10 * 4 + MessageChars + sizeof(FResource) * Resource_Count + 2 * 4 +
                                        2 * 8 + sizeof(FFrame) * FrameSlots + sizeof(FRayShare) + 8 * 4,
                  "FBlock must be the same in both builds");
    static_assert(offsetof(FBlock, Rays) % 8 == 0, "the rays' share holds handles: 8-byte aligned");
    // The two counters both processes change with interlocked operations: 8-byte aligned, or the 32-bit build's
    // interlocked 64-bit access is not atomic across processes.
    static_assert(offsetof(FBlock, Submitted) % 8 == 0 && offsetof(FBlock, Taken) % 8 == 0,
                  "the frame counters must be 8-byte aligned");

    // The host's own build number, reported in HostVersion.
    enum { HostBuild = 1 };
}

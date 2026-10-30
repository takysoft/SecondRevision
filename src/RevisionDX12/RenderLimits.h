// SecondRevision - numbers the C++ side and the shader files share (ShadowFactor.hlsl, SolidPass.hlsl): nothing but #defines,
// so dxc reads this header as it is, and the run-time compile gets the same values as macros. Included by RenderMath.h.
#pragma once

// The most lights a preset may ask for (the factor shader's arrays are this long). A macro as well as an enum
// (RdxMath::ShadowMaxLights): the shaders declare their arrays with it, so the two sides cannot drift apart.
#define RDX_SHADOW_MAX_LIGHTS 50
// The most of the shadows' lights the relief is lit by (LampShadows): every light of the High quality's pick
// (RdxMath::ShadowPresetFor). The shader's lamp rows and the C++ side's RdxMath::ReliefLampMax are this long.
#define RDX_RELIEF_LAMP_MAX 20

// How a light map value becomes a brightness multiplier. Calibrated against the stock renderer, not chosen: the median
// ratio between its frame and ours came out at 1.03 across 180000 pixels of the same saved game (docs/meres-04). A build
// may set it for a comparison.
#ifndef RDX_LIGHT_SCALE_VALUE
#define RDX_LIGHT_SCALE_VALUE 1.0f
#endif

// The ripples' field around the camera (RdxMath::RippleWorld, RippleTexels): world units across, texels across.
#define RDX_RIPPLE_WORLD_VALUE  2048.0f
#define RDX_RIPPLE_TEXELS_VALUE 256
// How finely the water's surface is cut to be lifted: one cut every this many of the engine's pixels along an edge.
#define RDX_WATER_EDGE_VALUE    24.0f

// The RT water's far probe (FRtPass::UpdateFar) keeps what the picture shows this many units off or further - the sky, a
// skyline at the level's edge; the water's mirror rays read it for what they meet as far off (SolidPass.hlsl).
#define RDX_RT_FAR_MIN 4096.0f
// The probe gathered from the picture (FMirrorProbe, MirrorProbe.hlsl; docs/tukrozodes-szonda-terv.md): texels a face of
// its finest level, the levels of its pyramid, how many levels up a hole takes its colour from, and the mirror rays'
// march through it - even steps, then halvings of the step that crossed. The shaders hold the same numbers in a block
// of their own (scripts/check_shaders.py compares them).
#define RDX_PROBE_SIDE_VALUE     128
#define RDX_PROBE_LEVELS_VALUE   8
#define RDX_PROBE_FILL_UP_VALUE  4
#define RDX_PROBE_STEPS_VALUE    8
#define RDX_PROBE_HALVINGS_VALUE 8
// A level surface's record for the mirror rays (FRtPass::SetLevel, SolidPass.hlsl's LevelMaterial), in bytes -
// thirty-two numbers: its texture's average colour; its light; its texture's place in the texture heap plus one (0:
// not known yet); a spare; the two rows that put a place of the level on the texture (RdxMath::RtSurfaceRows); how
// many of the texture's texels a unit of the level holds; and room for the light map's own.
#define RDX_RT_RECORD_BYTES 128
// Where the light map's own parts start in the record, in bytes: the gain the surface's draws carry, the two rows that
// put a place of the level on the light map, and the lit part's edges on it. Its place in the texture heap, plus
// one, is the record's fourth number.
#define RDX_RT_RECORD_GAIN       52
#define RDX_RT_RECORD_LIGHT_ROWS 64
#define RDX_RT_RECORD_LIGHT_EDGE 96
// And the surface's own face: its normal, and the light all round the zone before it.
#define RDX_RT_RECORD_NORMAL     112
#define RDX_RT_RECORD_AMBIENT    124
// How the level draws the surface, for the rays that pass a see-through one: laid over what is behind it, a
// multiplier of it, with holes where its texture is clear, and carrying its own light.
#define RDX_RT_RECORD_KIND       56
#define RDX_RT_KIND_TRANSLUCENT  1
#define RDX_RT_KIND_MODULATED    2
#define RDX_RT_KIND_MASKED       4
#define RDX_RT_KIND_UNLIT        8
// The most mirrors of the level's a frame's flashlight is given back by (the beam's block holds them, SetBeam): a
// frame with more views through mirrors than this has no light off any of them.
#define RDX_BEAM_MIRRORS         13
// The own lamps (RdxMath::OwnLampLight, OwnLampMost): how many a frame takes, and their rows - the head's, then each
// lamp's (SolidPass.hlsl, OwnRow).
#define RDX_OWN_LAMPS            9
#define RDX_OWN_HEAD_ROWS        4
#define RDX_OWN_LAMP_ROWS        7
// The see-through surfaces a mirror ray counts on its way, the nearest first.
#define RDX_RT_THIN_LAYERS       8
// And a caster's row of the rays' table (FRtPass::PrepareWater, RdxMath::RtGeoUints), with where its parts start, in
// bytes: how much of a texture a unit of the level holds on the caster, the light all round it, what its lamps'
// light is scaled by, and its textures by key.
#define RDX_RT_GEO_BYTES    128
#define RDX_RT_GEO_DENSITY  8
#define RDX_RT_GEO_AMBIENT  12
#define RDX_RT_GEO_SCALE    16
// And the engine's own light on it along the world's six ways, where the engine has shown it.
#define RDX_RT_GEO_CUBE     20
#define RDX_RT_GEO_TEXTURES 64
// The table's head, before the rows: the camera, the level's numbers, and where the level's lamps are in it
// (FRtPass::SetLamps) - each lamp RDX_RT_LAMP_BYTES, found by place through RDX_RT_LAMP_BUCKETS lists.
#define RDX_RT_TABLE_HEAD   64
#define RDX_RT_LAMP_BYTES   48
#define RDX_RT_LAMP_BUCKETS 4096
// The steps of a caster's normal each way (RdxMath::PackOctNormal).
#define RDX_RT_NORMAL_STEPS 64
// The rays' list (docs/terv-rt-host.md): what a pixel's shader would trace, written down for the 64-bit host and
// answered by it, a slot each in two buffers of the same length - the requests' and the answers'. A slot is
// RDX_RAY_SLOT_BYTES. The requests' slot 0 is no request: its first uint counts the slots taken (it starts at 1), its
// second says how many the buffers hold. What one pixel of one surface asks lies together, a block, and the block's
// first request carries what the pixel's own drawing finds the block by.
//   a request: floats 0-2 where the ray starts, in the level's space; float 3 how far it may go; uint 4 its way
//              (RayListSlots.hlsli, RayPackWay); uint 5 the pixel's depth as it is drawn, the block's key, and uint 6
//              the first slot of the next block at the same pixel (0: none) - both in a block's first request alone;
//              uint 7 packed: bits 0-1 the kind (0: the slot holds no request), 2-9 the instances the ray meets
//              (FRtPass's masks), 10 it passes the player's own body, 11 the lamps' bits are wanted for what it
//              meets, 12 what it passes on the way is wanted too, 13-17 how many requests the block has (its first
//              request alone), 18 the ray is traced only where the eye sees its start - nothing of the level or
//              of the bodies between the two, the player's own body none of them -, 19 whether the lamp at the eye
//              is in sight of the ray's start along the ray is wanted too, 20-27 how far from its start the ray
//              begins to look, in sixteenths of a unit, and 28-31 how faint a lamp's light on what the ray meets
//              may be and still be worked out, as a step (RayListSlots.hlsli, RayLeastFor)
//   an answer: uints 0-3 what the ray met, as a ray query's own answer is packed (SolidPass.hlsl, FRtHit; uint 0 all
//              ones: nothing; and uint 1 one with it: the eye does not see the ray's start, asked by bit 18) - the
//              same for a request that only asks whether anything is met. Where the request
//              asked the host to trace on from there (bits 11, 12 and 19): floats 4-6 the level's lamps' light at what
//              the ray met, each lamp's only where nothing stands between the two (RtLampLight.hlsli), and uint 7
//              packed (RayListSlots.hlsli, RayPackMore): bits 0-1 what stood in that light's way was looked for
//              among (0: no light worked out; 2: the bodies alone; 3: the level and the bodies), bits 2-5 how many
//              see-through surfaces the ray passed before what it met, bits 6-30 the first of them in the passed
//              surfaces' buffer, bit 31 the lamp at the eye is in sight (bit 19's answer)
// The passed surfaces' buffer is the host's to write: slots of the same size, and slot 0, as the requests', counts
// the slots taken (it starts at 1) and says how many there are.
//   a passed surface: uint 0 how far along the ray, a float's bits; uint 1 its triangle among the level's
//              see-through ones; floats 2-4 the lamps' light on it and uint 5 what stood in its way was looked for
//              among, as in an answer
#define RDX_RAY_SLOT_BYTES   32
#define RDX_RAY_KIND_NONE    0
#define RDX_RAY_KIND_FIRST   1
#define RDX_RAY_KIND_ANY     2
#define RDX_RAY_MASK_SHIFT   2
#define RDX_RAY_FLAG_NO_BODY 0x400
#define RDX_RAY_FLAG_LAMPS   0x800
#define RDX_RAY_FLAG_THIN    0x1000
#define RDX_RAY_COUNT_SHIFT  13
#define RDX_RAY_COUNT_MASK   0x1F
#define RDX_RAY_FLAG_SEEN    0x40000
#define RDX_RAY_FLAG_EYE     0x80000
#define RDX_RAY_FROM_SHIFT   20
#define RDX_RAY_FROM_MASK    0xFF
#define RDX_RAY_LEAST_SHIFT  28
#define RDX_RAY_MORE_PASSED_SHIFT 2
#define RDX_RAY_MORE_FIRST_SHIFT  6
#define RDX_RAY_MORE_FIRST_MASK   0x1FFFFFF
#define RDX_RAY_MORE_EYE          0x80000000
// The most requests one pixel of one surface writes down.
#define RDX_RAY_BLOCK_MOST   16
// How many blocks at a pixel a drawing looks through for its own.
#define RDX_RAY_CHAIN_MOST   32

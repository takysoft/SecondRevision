// SecondRevision - the mirror rays' table (FRtPass::PrepareWater), as its readers find their way in it: the renderer's
// own shader (SolidPass.hlsl) and the 64-bit host's pass that traces on from what a ray met (src/Host64/RayList.hlsl).
// One text, so the two cannot read it differently. RtWater - the table - is declared by whoever includes this.

// xyz: the camera; w: 1 where a quarter of the rays are asked - of each two by two pixels one for the others where
// their rays are alike (Reflections=RTHalf; SolidPass.hlsl, RtAskLed) -, 2 and past it where about a ninth are traced
// - of each three by three pixels one for the others the same way, chosen after they asked (RTLow; RaySparse.hlsl),
// and by how much past 2, how many lamps cast their shadows in its mirror image (RtLampLight.hlsli,
// RtLowShadowedLamps) -, else 0.
#define RtEye   asfloat(RtWater.Load4(0u))
// x: the records' first byte, y: their count; z: a view for measuring (0 none; 1 what the mirror rays met, 2 the
// glints, 3 how far, 4 the surfaces' own colours alone, 5 the sky from the far probe alone, 6 how high the rays met
// something, 7 as 4 with every texture's average colour in the texture's place - FBackend::RequestRtWaterView); w:
// where the level's see-through surfaces' triangles start in LevelMaterial's list of surfaces (RtThinOver).
#define RtLevel RtWater.Load4(16u)
// A geometry's row: its first vertex; its textures' average colour (RdxMath::PackRtAlbedo); at RDX_RT_GEO_DENSITY
// how much of a texture a unit of the level holds on it; at RDX_RT_GEO_CUBE the light that reaches it along the
// world's six ways (RdxMath::RtCubeAddLamp); at RDX_RT_GEO_TEXTURES its textures' places among RtTextures plus
// one, by the key its faces carry - 0 where there is none.
uint RtGeoRow(uint G)    { return RDX_RT_TABLE_HEAD + G * RDX_RT_GEO_BYTES; }
uint RtGeoFirst(uint G)  { return RtWater.Load(RtGeoRow(G)); }

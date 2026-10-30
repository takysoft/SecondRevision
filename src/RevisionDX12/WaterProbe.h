#pragma once

// Where the water is. The engine never says "this is water" on a draw, but it hands over the pieces: every BSP polygon
// it draws names its node (FSavedPoly::iNode), a node names the zones on its two sides (FBspNode::iZone), and a zone's
// actor says whether the zone is water (AZoneInfo::bWaterZone). A surface with water on one side and another real zone
// on the other is where the water meets the air: the level's own zone portal, textured (docs/meres-11).
//
// For now this measures before anything is built on it:
// - a census of every level as it is entered: the water zones, the surfaces on their borders, and the surfaces that
//   look like water without being on one (RdxWaterNoteFrame);
// - which of those the renderer is really handed to draw, and whether the node a drawn polygon names is the one the
//   drawn texture belongs to (RdxWaterNoteDrawn);
// - the drawing order: whether what lies behind a see-through water surface is already in the picture when the surface
//   is drawn - a refraction can only show what is there by then (RdxWaterNoteDrawn, RdxWaterNoteMesh, RdxWaterNoteTile).

void  RdxWaterForget();                                    // the level is over (the engine's Flush, and Exit)
void  RdxWaterNoteFrame(const FSceneNode* Frame);          // every scene node; a new level gets its census
void  RdxWaterFrameStart();                                // the engine's Lock: a new frame begins
UBOOL RdxWaterNoteDrawn(const FSceneNode* Frame, const FSurfaceInfo& Surface, const FSurfaceFacet& Facet);  // true: water
void  RdxWaterNoteMesh(const FSceneNode* Frame, FTransTexture** Pts, INT NumPts, UTexture* Texture);
void  RdxWaterNoteTile(const FSceneNode* Frame, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT Z, UTexture* Texture);

// Whether a BSP facet of the main view is a water surface - a liquid's zone on one side, another real zone on the other
// (docs/meres-11) - and if so, the constants its water run carries (FSolidPass::SetWaterConstants): the water's hue, the
// waves' strength for the size of this water, the time, the engine's projection, the absorption and the shoreline's
// width - the last two zero when the camera is under this water, where what lies beyond the surface is air. The level
// draws it translucent - see-through water - or, where the zone is water, opaque: the sewer's sludge, which the caller
// tells by the surface's own flag and draws opaque, only lifted (BLEND_Sludge). An ornamental fountain's pool is water
// too, a liquid's border or not (the census finds them, RdxMath::FountainFootInPool): its run carries the fountain's
// foot where the texture's own waves' numbers go - the constants' 24 below zero says so (RdxMath::FountainPoolSheet).
UBOOL RdxWaterSheet(const FSceneNode* Frame, const FSurfaceInfo& Surface, const FSurfaceFacet& Facet, FLOAT* Consts);
// The level's surface whose texture mapping a piece of a water's surface is drawn with, or -1 for its own: where one
// water's surface is made of several of the level's surfaces wearing the same texture laid on differently - a pool's
// side piece turned against the rest - each piece is drawn as the largest lays the texture on (the census at the level's
// start), so the texture and its waves go on unbroken over the seams between them.
INT   RdxWaterLeadOf(const UModel* Model, INT iSurf);
// The water a point lies in, for a waterfall's colour (RenderDevice, the fall run): the zone there, and if it is water,
// its hue as the water's own surface takes it (RdxWaterSheet) - the zone's view fog, only its hue, the brightest channel
// 1. False where the point is not in water.
UBOOL RdxWaterHueAt(const FSceneNode* Frame, const FVector& World, FVector& Hue);
// EndlessWater: the level's sea, where it has one - of the level water surfaces, the biggest whose outer line has sky
// windows on it (the census at the level's start). Its zone, its height and extent in the world, the sides whose line
// is sky (1 the least X, 2 the most X, 4 the least Y, 8 the most Y), and one of its border nodes. 0 where there is none.
UBOOL RdxWaterSea(INT& Zone, FLOAT& Height, FLOAT& MinX, FLOAT& MinY, FLOAT& MaxX, FLOAT& MaxY, INT& Sides, INT& Node);
// Whether a facet is the sea's surface: its first border node's liquid is the sea's zone, air on its other side.
UBOOL RdxWaterIsSea(const FSurfaceInfo& Surface, const FSurfaceFacet& Facet);
// Whether a facet is one of the black walls at the sea's edge: CoreTexMisc.Marker_sky WITHOUT the FakeBackdrop flag,
// drawn opaque - the band under the skyline, below the sky windows (the census marks them).
UBOOL RdxWaterIsSeaEdge(const FSurfaceInfo& Surface, const FSurfaceFacet& Facet);
// The water texture's own wave field this frame, once (RdxWaterSheet reads it out of the texture): W x H heights, 128 the
// still surface - for FBackend::SetWaterWaves. Null when there is none, or it was taken already.
const BYTE* RdxWaterTakeWaves(INT& W, INT& H);

// What pushes the water this frame, for the ripples (FBackend::SetRipples): every thing in the scene that is in a water
// zone with a level surface, near that surface, and within the ripples' field around the camera - four numbers each
// (world X, Y, radius, push) - and, in the room they leave, the drops of the game's rain as they go into the water. At
// most Max of them; returns how many.
INT RdxWaterRippleSources(const FSceneNode* Frame, FLOAT* Out, INT Max);
// How thick the game's rain is falling into the water around the camera, 0 none to 1: counted by
// RdxWaterRippleSources whenever it runs, and gone half a second after it last did. For the rings the water's own
// shader draws (SolidPass.hlsl, WaterRainRings).
FLOAT RdxWaterRainThick();
// How thick the game's rain is falling on the floors around the camera, 0 none to 1: its raindrops falling within the
// ripples' reach of it (RdxMath::FloorRainFullDrops the full rain), evened out over half a second of the level's clock.
// For the wet floors' puddles' rings (WetReflect.hlsl, RainRingsAt), wherever there is water or not. Once a frame.
FLOAT RdxFloorRainThick(ULevel* Level, const FVector& Cam);
// How much of a lamp's glare comes to the camera where a level water surface lies between the two - 1 where none
// does: the engine asks only the level's solid things whether a lamp is in sight. A surface the level draws
// see-through by what it lets through at that angle (RdxMath::WaterLampThrough); one it draws solid - the sewer's
// sludge - lets nothing through.
FLOAT RdxWaterLampShows(const FSceneNode* Frame, const FVector& Lamp);
// A measurement only (RenderDevice: the marker SecondRevision-ripple-probe.txt): once a second, the things over the level's
// water that move - their class and physics, their cylinder, their zone and how high above the surface they are, their
// speed the engine's way and as their place changed - and what RdxWaterRippleSources makes of them, and why.
void RdxWaterRippleProbe(const FSceneNode* Frame);
// The level's ornamental fountains' curtains near the camera, for their water drawn as falling drops
// (FBackend::SetCurtains): those within RdxMath::CurtainReach, the nearest first, at most Room - each curtain by
// itself, a slab's two as two. RdxMath::CurtainFloats numbers each: the two ends of the curtain's foot on the level
// (X, Y and X, Y), its pool's height, how high over the pool the slot is, how far the water spreads either way off
// the curtain's plane, and a number of its own. Returns how many.
INT   RdxWaterFountains(const FSceneNode* Frame, FLOAT* Out, INT Room);
// Whether a level surface is one of those fountains' curtains: the sheet the level draws where the drops fall.
UBOOL RdxWaterIsCurtain(INT iSurf);
// Whether this level has any water the ripples can run on (a level surface): without it they do not step at all.
UBOOL RdxWaterHasRipples();
// The level's see-through water surfaces near the camera, for the caustics laid on what lies under them
// (FBackend::SetCausticView): those within RdxMath::CausticReach, the nearest first, at most Room.
// RdxMath::CausticBoxFloats numbers each: the surface's extent on the level - its lowest X and Y, its highest X and Y -
// its height, and its water plus one - the liquid zones that touch under the water are one water
// (RdxMath::WaterBodyOf). Returns how many. RdxWaterBoxesNear: how many lay within that reach when last asked.
INT RdxWaterBoxes(const FSceneNode* Frame, FLOAT* Out, INT Room);
INT RdxWaterBoxesNear();
// What tells this frame's view which of its pixels lie in a see-through liquid, for the same caustics: the liquids'
// see-through borders near the camera and in its picture, as triangles, the nearest first -
// RdxMath::CausticPortalFloats numbers a corner, its place on the level and what the border writes into the mask
// (RdxMath::CausticMaskValue: into its liquid seen from the dry side, out of it seen from the liquid's) - and CamWet,
// the camera's own water plus one, 0 where it is in none. Verts is the probe's own list, good until the next
// call. Returns how many corners: none, and a camera in no liquid, means the frame has nothing to lay caustics on.
INT RdxWaterPortals(const FSceneNode* Frame, const FLOAT*& Verts, INT& CamWet);
// The lamps that glint on the water this frame (Reflections=High and up, FBackend::SetWaterLamps): lights whose light
// reaches a level water surface below them, the best RdxMath::WaterLampMax by RdxMath::WaterLampRank, at most Room.
// Eight numbers each: the lamp's place from the camera (world X, Y, Z), its reach, its colour at its brightness, and a
// zero. Returns how many.
INT RdxWaterLamps(const FSceneNode* Frame, FLOAT* Out, INT Room);
// A point on level water Ahead units ahead of the camera - for the ripples' test ring and the test splash (measurement
// markers, RenderDevice.cpp); Z, when asked for, gets that surface's height.
UBOOL RdxWaterTestPoint(const FSceneNode* Frame, FLOAT& X, FLOAT& Y, FLOAT* Z = NULL, FLOAT Ahead = 800.0f);
// Things falling into the water this frame (FBackend::AddSplashes): everything in the scene that has just gone from outside
// a level water zone into one, falling at least RdxMath::SplashMinFall. Five numbers each: where it went in (world X, Y and
// the surface's Z), its collision radius and how fast it fell. At most Room; returns how many.
INT RdxWaterSplashes(const FSceneNode* Frame, FLOAT* Out, INT Room);
// A measurement only (the marker SecondRevision-water-fields.txt): the water texture's own wave field written to files.
void  RdxWaterDumpFields(UBOOL bOn);
// The water texture's own waves step at RdxMath::WaterWaveRate a second while the water is on (Low and up) - the engine
// would step them once a frame. Off only for a measurement (the marker SecondRevision-water-rate-off.txt).
void  RdxWaterCapRate(UBOOL bOn);
// The water turned off: the textures this level's water capped step as the engine had them again.
void  RdxWaterRestoreRates();

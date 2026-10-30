// SecondRevision - the engine-facing side.
//
// This class implements the URenderDevice contract described in docs/spec-01. It owns
// no D3D12 objects itself: everything API-specific lives behind FBackend, so the
// backend choice stays cheap to revisit (project decision D3).
#pragma once

#include "Backend.h"

class URevisionRenderDevice : public URenderDevice
{
#pragma warning(push, 1)
    DECLARE_CLASS(URevisionRenderDevice, URenderDevice, CLASS_Config)
#pragma warning(pop)

    // Keeps the class's inherited config properties out of the game's own ini. IMPLEMENT_CLASS
    // passes StaticConfigName() in as ClassConfigName, and FConfigCacheIni::Find treats every
    // config name except "User" and "System" as a literal file name with .ini appended. "System"
    // is the game's own ini and "User" is RevisionUser.ini, which is why the stock drivers keep
    // their settings in a file of their own as well.
    //
    // Our OWN settings are not config properties at all: they are read from
    // SecondRevision.settings.ini by the reader in RenderDevice.cpp, so the engine neither loads
    // nor saves them. Only URenderDevice's inherited properties are affected by this name.
    //
    // It has to come AFTER DECLARE_CLASS - the SDK's own UWindowsClient carries the same note
    // (Engine\Inc\UnIn.h:45).
    static const TCHAR* StaticConfigName() { return TEXT("SecondRevision"); }

public:
    URevisionRenderDevice();
    void StaticConstructor();

    // --- URenderDevice ------------------------------------------------------
    virtual UBOOL Init(UViewport* InViewport, INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen);
    virtual UBOOL SetRes(INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen);
    virtual void  Exit();
    virtual void  Flush(UBOOL AllowPrecache);
    // The engine walks every texture a level uses and offers it to us before play
    // starts. Taking it up is what turns "upload when first seen" into "upload once,
    // up front" - and the first is felt as a stutter whenever the view turns onto
    // something new.
    virtual void  PrecacheTexture(FTextureInfo& Info, DWORD PolyFlags);
    virtual void  Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize);
    virtual void  Unlock(UBOOL Blit);
    virtual void  DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet);
    virtual void  DrawGouraudPolygon(FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span);
    virtual void  DrawTile(FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags);
    virtual void  Draw2DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2);
    virtual void  Draw2DPoint(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z);
    virtual void  ClearZ(FSceneNode* Frame);
    virtual void  PushHit(const BYTE* Data, INT Count);
    virtual void  PopHit(INT Count, UBOOL bForce);
    virtual void  GetStats(TCHAR* Result);
    virtual void  ReadPixels(FColor* Pixels);
    virtual void  SetSceneNode(FSceneNode* Frame);
    virtual void  EndFlash();

    // --- Config, [SecondRevision.RevisionRenderDevice] in SecondRevision.ini - see
    //     StaticConfigName above and StaticConstructor below.
    UBOOL ExclusiveFullscreen;
    UBOOL GpuTextureConvert;
    FString Preset;            // the ini key Preset: Recommended, Vanilla or Custom - see the property's comment
    UBOOL   Vanilla;           // NOT a setting any more: 1 while the preset is Vanilla. The rest of the renderer
                               // asks this, so the vanilla rule (docs/szabaly-vanilla.md) reads the same as before.
    UBOOL BorderlessWindow;
    FString ShadowQuality;
    // The shadows are ShadowQuality's own business now: its Off is "no shadows" (there is no Shadows key any more).
    FString DebugReadout;      // the readout on screen: Off, FPS or On - see the property's comment

    // The renderer's own menu (DebugKey, Scroll Lock by default) - see RenderDevice.cpp. A member because it reads and writes the
    // device's own settings and draws through the backend.
    void DrawRendererMenu();
    // What the picture runs with, from the preset and the custom set - see RenderDevice.cpp.
    UBOOL ApplyPreset(RdxMath::EPreset P);
    // The same for a picture of any values: the custom set put back (ShotPairFrame).
    UBOOL ApplyPicture(const RdxMath::FPresetPicture& S);
    // Screenshot=On: the pair of pictures Shift with the menu key takes, a step a frame - see RenderDevice.cpp.
    void ShotPairFrame();
    // The settings file: written whole from its template on a first start (bWhole), else each key's line set to
    // what the renderer has now.
    UBOOL SaveSettings(UBOOL bWhole);
    // The borderless window has a new monitor under it: the engine's resolution, the swap chain and what is known
    // about the display all follow it - see RenderDevice.cpp.
    void FollowMonitor(HWND Window);
    FLOAT   ShadowVerticalWeight; // how much more a light's height counts in the shadow pick - see the property
    FString AntiAliasing;      // the ini key AntiAliasing - see the property's comment for the values
    FString Hdr;               // the ini key HDR: Off, 10bitSDR or On - see the property's comment
    FString AmbientOcclusion;  // the ini key AmbientOcclusion: Off or SSAO - see the property's comment
    FLOAT   FsrSharpness;      // the upscaler's own sharpening, 0..1
    FString Upscale;           // the ini key Upscale: the temporal upscaler's size - NativeAA, Quality, Balanced, Performance
    UBOOL   FrameGeneration;   // every frame out as two pictures: the one generated before it, then its own
    // The relief on the walls, and what it is drawn from - see the properties' comments.
    FString Geometry;          // how the relief is drawn: Off, POM or Tessellation
    UBOOL   SoftParticles;     // smoke and steam fade out where they meet a wall instead of ending on a line
    UBOOL   Volumetrics;       // smoke and gas drawn as a volume rather than as a flat picture facing the camera
    UBOOL   MotionBlur;        // the picture smeared along what moved in it
    UBOOL   ChromaticAberration;  // the lens pulling the colours apart towards the corners
    UBOOL   Vignette;             // the lens darkening the corners
    UBOOL   FilmGrain;            // the film's own unevenness
    FString Bloom;                // the halo a bright thing wears: Off or On (all that is bright, and the coronas)
    FString LensFlare;            // the ghosts a lens throws, of the engine's coronas: Off or On
    UBOOL   DepthOfField;         // a conversation's camera focused on the speaker
    UBOOL   EndlessWater;         // the sea on past the level's sky windows, to the horizon
    UBOOL   Fire3D;               // the fires' flame sheets as a body of flame (docs/terv-3d-tuz.md)
    FString Grass3D;              // blades of grass on the level's grass, in the wind: Off, Low, Medium or High
    FString Trees3D;              // the game's trees and bushes grown again by code (TreeGen.h): Off, Low, Medium or High
    UBOOL   Spherifier;           // the level's and the models' round shapes rounded (Spherify.h)
    INT     SpherifierDistance;   // the most it moves a wall, in the level's units - the settings file's alone
    INT     SpherifierAngle;      // the sharpest angle between two faces it still rounds, degrees - the file's alone
    UBOOL   Wind;                 // the rain leaning with the level's wind, and the dust in it
    UBOOL   Puddles;              // the floors outdoors wet in the rain
    UBOOL   FogRooms;             // the level's fog kept in the room it belongs to, laid by the renderer (FFogRooms)
    FString ForceField;          // the game's force fields drawn as arcs of light: Engine, Arcs or 3D
    FString Flashlight;          // the game's flashlight (Original), a cone from the eye in its place (Fixed), and a
                                 // lamp on the guns of the game's people besides (Everybody)
    FString HdtpHoloFix;         // a hologram the game gave an HDTP model: Off, On (its own head) or Old (the original)
    UBOOL   RevisionSplashScreenFix;  // the game's loading pictures on a tall screen too, laid by their middle
    UBOOL   LoadFix;                  // a saved game loads beside the running level, not over it (LoadFixFrame)
    UBOOL   GodRays;              // the beams a lamp throws through the air

private:
    FBackend Backend;

    // A lost device made again - see the function.
    void  RecoverLostDevice(UBOOL bAtOnce = 0);

    // The size the engine last asked for. Compared against the scene node so a window
    // resize the engine forgot to announce still reaches the swap chain.
    INT   PendingX;
    INT   PendingY;
    INT   FrameCounter;
    INT   LastTriangles;   // previous frame's geometry, for the flicker detector

    // The frame's screen tint as Lock received it; EndFlash applies it.
    FPlane LastFlashScale;
    FPlane LastFlashFog;
};

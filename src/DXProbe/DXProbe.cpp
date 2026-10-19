// DXProbe - implementation. See ProbeDevice.h for what this module is for.
#include "Pch.h"
#include "ProbeDevice.h"

IMPLEMENT_PACKAGE(DXProbe);
IMPLEMENT_CLASS(UProbeRenderDevice);

// How many calls of each entry point log their full arguments before going quiet.
// Small on purpose: the interesting part is the first few of each, plus the totals.
#define DETAIL_BUDGET 4

#define PROBE_LOG(fmt) debugf(TEXT("DXProbe: ") TEXT(fmt))
#define PROBE_LOGF(fmt, ...) debugf(TEXT("DXProbe: ") TEXT(fmt), __VA_ARGS__)

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UProbeRenderDevice::UProbeRenderDevice()
{
    // What we claim here decides what the engine bothers to send us, so the capability
    // set is itself a thing to measure. PROBE_GREEDY selects which experiment we run:
    //
    //   0 = minimal   - the plain path only. Establishes the baseline.
    //   1 = greedy    - claim everything, so the engine offers detail textures,
    //                   fog maps, macro textures and a precache pass.
    //
    // Never claim SpanBased: that asks for the 1998 software-rasteriser path.
#ifndef PROBE_GREEDY
#define PROBE_GREEDY 1
#endif

    // HighDetailActors gets its own switch so it can be isolated from the rest of the
    // greedy set: measurement 01 saw the gouraud call count jump 13x between two runs
    // and this bit was the suspect, but the two runs looked at different scenes.
    // Build twice with only this changed, same launch, and compare the same frame window.
#ifndef PROBE_HIGHDETAIL
#define PROBE_HIGHDETAIL PROBE_GREEDY
#endif

    URenderDevice::SpanBased            = 0;
    URenderDevice::FullscreenOnly       = 0;

#if PROBE_GREEDY
    URenderDevice::SupportsFogMaps      = 1;
    URenderDevice::SupportsDistanceFog  = 1;
    URenderDevice::VolumetricLighting   = 1;
    URenderDevice::ShinySurfaces        = 1;
    URenderDevice::Coronas              = 1;
    URenderDevice::DetailTextures       = 1;
    URenderDevice::SupportsTC           = 1;
    URenderDevice::PrecacheOnFlip       = 1;
    URenderDevice::SupportsLazyTextures = 0;
    URenderDevice::PrefersDeferredLoad  = 0;
#else
    URenderDevice::SupportsFogMaps      = 0;
    URenderDevice::SupportsDistanceFog  = 0;
    URenderDevice::VolumetricLighting   = 0;
    URenderDevice::ShinySurfaces        = 0;
    URenderDevice::Coronas              = 0;
    URenderDevice::DetailTextures       = 0;
    URenderDevice::SupportsTC           = 0;
    URenderDevice::PrecacheOnFlip       = 0;
    URenderDevice::SupportsLazyTextures = 0;
    URenderDevice::PrefersDeferredLoad  = 0;
#endif

    URenderDevice::HighDetailActors     = PROBE_HIGHDETAIL;

    debugf(TEXT("DXProbe: capability mode = %s, highDetailActors = %i"),
        PROBE_GREEDY ? TEXT("GREEDY") : TEXT("MINIMAL"), (INT)PROBE_HIGHDETAIL);

    static const TCHAR* Names[CALL_MAX] =
    {
        TEXT("Init"), TEXT("SetRes"), TEXT("Exit"), TEXT("Flush"), TEXT("Exec"),
        TEXT("Lock"), TEXT("Unlock"),
        TEXT("DrawComplexSurface"), TEXT("DrawGouraudPolygon"), TEXT("DrawTile"),
        TEXT("Draw3DLine"), TEXT("Draw2DClippedLine"), TEXT("Draw2DLine"), TEXT("Draw2DPoint"),
        TEXT("ClearZ"), TEXT("PushHit"), TEXT("PopHit"), TEXT("GetStats"), TEXT("ReadPixels"),
        TEXT("EndFlash"), TEXT("DrawStats"), TEXT("SetSceneNode"), TEXT("PrecacheTexture")
    };

    for (INT i = 0; i < CALL_MAX; i++)
    {
        Calls[i].Name    = Names[i];
        Calls[i].Count   = 0;
        Calls[i].Verbose = DETAIL_BUDGET;
    }

    appMemzero(&Obs, sizeof(Obs));
    Obs.PolyMinPts  = MAXINT;
    Obs.FrameMinX   = MAXINT;
    Obs.FrameMinY   = MAXINT;
    FrameIndex      = 0;
}

void UProbeRenderDevice::StaticConstructor()
{
    // No config properties: the probe deliberately has no knobs.
}

// ---------------------------------------------------------------------------
// Recording helpers
// ---------------------------------------------------------------------------

void UProbeRenderDevice::Record(ECallSlot Slot)
{
    Calls[Slot].Count++;
}

UBOOL UProbeRenderDevice::ShouldDetail(ECallSlot Slot)
{
    if (Calls[Slot].Verbose <= 0)
        return 0;
    Calls[Slot].Verbose--;
    return 1;
}

void UProbeRenderDevice::NoteFrame(FSceneNode* Frame)
{
    if (!Frame)
        return;
    if (Frame->X < Obs.FrameMinX) Obs.FrameMinX = Frame->X;
    if (Frame->X > Obs.FrameMaxX) Obs.FrameMaxX = Frame->X;
    if (Frame->Y < Obs.FrameMinY) Obs.FrameMinY = Frame->Y;
    if (Frame->Y > Obs.FrameMaxY) Obs.FrameMaxY = Frame->Y;
    if (Frame->Recursion > Obs.MaxRecursion) Obs.MaxRecursion = Frame->Recursion;
    if (Frame->Mirror < 0.0f) Obs.MirrorSeen++;
}

void UProbeRenderDevice::NoteTexture(const FTextureInfo& Info)
{
    const INT Slot = ((INT)Info.Format >= 0 && (INT)Info.Format < 7) ? (INT)Info.Format : 7;
    Obs.FormatSeen[Slot]++;
}

void UProbeRenderDevice::ResetCounters()
{
    for (INT i = 0; i < CALL_MAX; i++)
        Calls[i].Count = 0;
}

void UProbeRenderDevice::Report(const TCHAR* Reason)
{
    PROBE_LOGF("===== report (%s), frames so far: %i =====", Reason, FrameIndex);

    for (INT i = 0; i < CALL_MAX; i++)
        if (Calls[i].Count)
            PROBE_LOGF("  calls %-22s %i", Calls[i].Name, Calls[i].Count);

    PROBE_LOGF("  surface texture   present=%i  NULL=%i", Obs.SurfaceTexturePresent, Obs.SurfaceTextureNull);
    PROBE_LOGF("  surface lightmap  present=%i", Obs.SurfaceLightMapPresent);
    PROBE_LOGF("  surface macro     present=%i", Obs.SurfaceMacroPresent);
    PROBE_LOGF("  surface detail    present=%i", Obs.SurfaceDetailPresent);
    PROBE_LOGF("  surface fogmap    present=%i", Obs.SurfaceFogMapPresent);

    if (Obs.PolyMinPts != MAXINT)
        PROBE_LOGF("  poly points       min=%i max=%i degenerate(<3)=%i", Obs.PolyMinPts, Obs.PolyMaxPts, Obs.PolyDegenerate);

    PROBE_LOGF("  tex formats       P8=%i RGBA7=%i RGB16=%i DXT1=%i RGB8=%i RGBA8=%i other=%i",
        Obs.FormatSeen[0], Obs.FormatSeen[1], Obs.FormatSeen[2],
        Obs.FormatSeen[3], Obs.FormatSeen[4], Obs.FormatSeen[5], Obs.FormatSeen[7]);

    PROBE_LOGF("  polyflags union   complex=0x%08X gouraud=0x%08X tile=0x%08X",
        Obs.FlagsComplex, Obs.FlagsGouraud, Obs.FlagsTile);
    PROBE_LOGF("  lock flags union  0x%08X", Obs.LockFlagsSeen);

    if (Obs.FrameMinX != MAXINT)
        PROBE_LOGF("  frame size        X %i..%i  Y %i..%i", Obs.FrameMinX, Obs.FrameMaxX, Obs.FrameMinY, Obs.FrameMaxY);

    PROBE_LOGF("  max recursion     %i   (>0 = portals/mirrors rendered)", Obs.MaxRecursion);
    PROBE_LOGF("  mirrored frames   %i", Obs.MirrorSeen);

    if (Obs.TileCount)
    {
        PROBE_LOGF("  tile count        %i", Obs.TileCount);
        PROBE_LOGF("  tile X span       %f .. %f      (frame X max %i)", Obs.TileMinX, Obs.TileMaxX, Obs.FrameMaxX);
        PROBE_LOGF("  tile Y span       %f .. %f      (frame Y max %i)", Obs.TileMinY, Obs.TileMaxY, Obs.FrameMaxY);
        PROBE_LOGF("  tile size         W %f..%f   H %f..%f", Obs.TileMinW, Obs.TileMaxW, Obs.TileMinH, Obs.TileMaxH);
        PROBE_LOGF("  tile Z            %f .. %f   nonPositive=%i", Obs.TileMinZ, Obs.TileMaxZ, Obs.TileNonPositiveZ);
        PROBE_LOGF("  tile U span       %f .. %f", Obs.TileMinU, Obs.TileMaxU);
        PROBE_LOGF("  tile V span       %f .. %f", Obs.TileMinV, Obs.TileMaxV);
        PROBE_LOGF("  tile outside frm  %i", Obs.TileOutsideFrame);
        PROBE_LOGF("  tile texel:pixel  1to1=%i  scaled=%i", Obs.TileTexelPixel1to1, Obs.TileTexelPixelOther);
        PROBE_LOGF("  tile uv in tex    inside=%i  outside=%i", Obs.TileUvWithinTexture, Obs.TileUvOutsideTexture);
    }

    if (Obs.GVerts)
    {
        PROBE_LOGF("  gouraud verts     %i sampled", Obs.GVerts);
        PROBE_LOGF("  gouraud screenX   %f .. %f   (frame X %i)", Obs.GScreenMinX, Obs.GScreenMaxX, Obs.FrameMaxX);
        PROBE_LOGF("  gouraud screenY   %f .. %f   (frame Y %i)", Obs.GScreenMinY, Obs.GScreenMaxY, Obs.FrameMaxY);
        PROBE_LOGF("  gouraud outside   %i", Obs.GScreenOutside);
        PROBE_LOGF("  gouraud RZ        %f .. %f", Obs.GRzMin, Obs.GRzMax);
        PROBE_LOGF("  gouraud point.Z   %f .. %f", Obs.GPointZMin, Obs.GPointZMax);
        PROBE_LOGF("  gouraud U         %f .. %f", Obs.GUMin, Obs.GUMax);
        PROBE_LOGF("  gouraud V         %f .. %f", Obs.GVMin, Obs.GVMax);
        PROBE_LOGF("  gouraud light RGB %f .. %f", Obs.GLightMin, Obs.GLightMax);
        PROBE_LOGF("  gouraud fog.W     %f .. %f", Obs.GFogWMin, Obs.GFogWMax);
        PROBE_LOGF("  gouraud outcodes  0x%02X", Obs.GFlagsSeen);
    }

    if (Obs.CSamples)
    {
        PROBE_LOGF("  cs samples        %i", Obs.CSamples);
        PROBE_LOGF("  cs map axis len   X %f..%f   Y %f..%f",
            Obs.MapXAxisLenMin, Obs.MapXAxisLenMax, Obs.MapYAxisLenMin, Obs.MapYAxisLenMax);
        PROBE_LOGF("  cs tex pan        X %f..%f   Y %f..%f",
            Obs.TexPanXMin, Obs.TexPanXMax, Obs.TexPanYMin, Obs.TexPanYMax);
        PROBE_LOGF("  cs tex scale      U %f..%f   V %f..%f",
            Obs.TexUScaleMin, Obs.TexUScaleMax, Obs.TexVScaleMin, Obs.TexVScaleMax);
        PROBE_LOGF("  cs diffuse norm   U %f..%f   V %f..%f  (1.0 = one full tile)",
            Obs.DiffUNormMin, Obs.DiffUNormMax, Obs.DiffVNormMin, Obs.DiffVNormMax);
        PROBE_LOGF("  cs light norm     U %f..%f   V %f..%f  (no half-texel fix applied)",
            Obs.LightUNormMin, Obs.LightUNormMax, Obs.LightVNormMin, Obs.LightVNormMax);
        PROBE_LOGF("  cs lightmap size  %i..%i", Obs.LightSizeMin, Obs.LightSizeMax);
        PROBE_LOGF("  cs lightmap scale U %f..%f   V %f..%f",
            Obs.LightUScaleMin, Obs.LightUScaleMax, Obs.LightVScaleMin, Obs.LightVScaleMax);
        PROBE_LOGF("  cs light /scale   U %f..%f   V %f..%f  (0..1 = exactly one light map)",
            Obs.LightScaledUMin, Obs.LightScaledUMax, Obs.LightScaledVMin, Obs.LightScaledVMax);
    }

    PROBE_LOG("===== end of report =====");
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

UBOOL UProbeRenderDevice::Init(UViewport* InViewport, INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen)
{
    Record(CALL_Init);
    PROBE_LOGF("Init  %ix%i  colorBytes=%i  fullscreen=%i", NewX, NewY, NewColorBytes, (INT)Fullscreen);

    URenderDevice::Viewport = InViewport;

    if (!InViewport)
    {
        PROBE_LOG("Init  viewport is NULL - refusing");
        return 0;
    }

    // Open question spec-01/1: which blit flags does a modern, non-DirectDraw device
    // need? Try the combination we believe is right and record whether the engine
    // accepted it. If it fails we retry with a bare request so we can tell which part
    // was refused, instead of guessing later.
    const DWORD Wanted = BLIT_HardwarePaint | BLIT_Direct3D | (Fullscreen ? BLIT_Fullscreen : 0);

    UBOOL Ok = InViewport->ResizeViewport(Wanted, NewX, NewY, NewColorBytes);
    PROBE_LOGF("Init  ResizeViewport(0x%04X) -> %i", Wanted, (INT)Ok);

    if (!Ok)
    {
        const DWORD Fallback = BLIT_HardwarePaint | (Fullscreen ? BLIT_Fullscreen : 0);
        Ok = InViewport->ResizeViewport(Fallback, NewX, NewY, NewColorBytes);
        PROBE_LOGF("Init  fallback ResizeViewport(0x%04X) -> %i", Fallback, (INT)Ok);
    }

    if (Ok)
        PROBE_LOGF("Init  viewport now reports %ix%i", InViewport->SizeX, InViewport->SizeY);

    return Ok;
}

UBOOL UProbeRenderDevice::SetRes(INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen)
{
    Record(CALL_SetRes);
    // Open question spec-01/2: is SetRes called separately from Init? The log answers it.
    PROBE_LOGF("SetRes  %ix%i  colorBytes=%i  fullscreen=%i", NewX, NewY, NewColorBytes, (INT)Fullscreen);

    if (!URenderDevice::Viewport)
        return 0;

    const DWORD Wanted = BLIT_HardwarePaint | BLIT_Direct3D | (Fullscreen ? BLIT_Fullscreen : 0);
    const UBOOL Ok = URenderDevice::Viewport->ResizeViewport(Wanted, NewX, NewY, NewColorBytes);
    PROBE_LOGF("SetRes  ResizeViewport(0x%04X) -> %i", Wanted, (INT)Ok);
    return Ok;
}

void UProbeRenderDevice::Exit()
{
    Record(CALL_Exit);
    Report(TEXT("Exit"));
}

void UProbeRenderDevice::Flush(UBOOL AllowPrecache)
{
    Record(CALL_Flush);
    PROBE_LOGF("Flush  allowPrecache=%i", (INT)AllowPrecache);
    Report(TEXT("Flush"));
    ResetCounters();
}

UBOOL UProbeRenderDevice::Exec(const TCHAR* Cmd, FOutputDevice& Ar)
{
    Record(CALL_Exec);
    if (ShouldDetail(CALL_Exec))
        PROBE_LOGF("Exec  \"%s\"", Cmd ? Cmd : TEXT(""));

    // "probe report" dumps the accumulated observations on demand from the console.
    if (Cmd && appStrcmp(Cmd, TEXT("PROBEREPORT")) == 0)
    {
        Report(TEXT("console"));
        return 1;
    }

    return URenderDevice::Exec(Cmd, Ar);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void UProbeRenderDevice::Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize)
{
    Record(CALL_Lock);
    Obs.LockFlagsSeen |= RenderLockFlags;

    if (ShouldDetail(CALL_Lock))
    {
        PROBE_LOGF("Lock  flags=0x%08X  hitData=%s  hitSize=%i",
            RenderLockFlags,
            HitData ? TEXT("yes") : TEXT("no"),
            HitSize ? *HitSize : -1);
        PROBE_LOGF("Lock  flashScale=(%f %f %f %f)", FlashScale.X, FlashScale.Y, FlashScale.Z, FlashScale.W);
        PROBE_LOGF("Lock  flashFog=(%f %f %f %f)", FlashFog.X, FlashFog.Y, FlashFog.Z, FlashFog.W);
        PROBE_LOGF("Lock  screenClear=(%f %f %f %f)", ScreenClear.X, ScreenClear.Y, ScreenClear.Z, ScreenClear.W);
    }
}

void UProbeRenderDevice::Unlock(UBOOL Blit)
{
    Record(CALL_Unlock);
    if (ShouldDetail(CALL_Unlock))
        PROBE_LOGF("Unlock  blit=%i", (INT)Blit);

    FrameIndex++;

    // A periodic report keeps a long session readable without flooding the log.
    if ((FrameIndex % 600) == 0)
    {
        Report(TEXT("periodic"));
        ResetCounters();
    }
}

void UProbeRenderDevice::SetSceneNode(FSceneNode* Frame)
{
    Record(CALL_SetSceneNode);
    NoteFrame(Frame);

    if (ShouldDetail(CALL_SetSceneNode) && Frame)
    {
        PROBE_LOGF("SetSceneNode  size=%ix%i  offset=%i,%i  zone=%i  recursion=%i  mirror=%f",
            Frame->X, Frame->Y, Frame->XB, Frame->YB, Frame->ZoneNumber, Frame->Recursion, Frame->Mirror);
        PROBE_LOGF("SetSceneNode  proj=(%f %f %f)  zoom=%f  FX15=%f FY15=%f",
            Frame->Proj.X, Frame->Proj.Y, Frame->Proj.Z, Frame->Zoom, Frame->FX15, Frame->FY15);
        PROBE_LOGF("SetSceneNode  level=%s", Frame->Level ? TEXT("present") : TEXT("NULL"));
    }
}

// ---------------------------------------------------------------------------
// Draw calls - measured, not drawn
// ---------------------------------------------------------------------------

void UProbeRenderDevice::DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet)
{
    Record(CALL_DrawComplexSurface);
    NoteFrame(Frame);
    Obs.FlagsComplex |= Surface.PolyFlags;

    // Every one of these five is documented as optional (docs/spec-02).
    if (Surface.Texture)       { Obs.SurfaceTexturePresent++; NoteTexture(*Surface.Texture); }
    else                       { Obs.SurfaceTextureNull++; }
    if (Surface.LightMap)      Obs.SurfaceLightMapPresent++;
    if (Surface.MacroTexture)  Obs.SurfaceMacroPresent++;
    if (Surface.DetailTexture) Obs.SurfaceDetailPresent++;
    if (Surface.FogMap)        Obs.SurfaceFogMapPresent++;

    for (const FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next)
    {
        if (Poly->NumPts < Obs.PolyMinPts) Obs.PolyMinPts = Poly->NumPts;
        if (Poly->NumPts > Obs.PolyMaxPts) Obs.PolyMaxPts = Poly->NumPts;
        if (Poly->NumPts < 3) Obs.PolyDegenerate++;
    }

    // Texture-mapping evidence, sampled. The engine gives no UV here: it gives a
    // mapping frame and world points. Project the points onto the mapping axes and
    // see whether the result behaves like texel coordinates.
    static INT CStride = 0;
    if (Surface.Texture && ((++CStride & 127) == 0))
    {
        const FCoords& MC = Facet.MapCoords;

        const FLOAT XLen = appSqrt(MC.XAxis | MC.XAxis);
        const FLOAT YLen = appSqrt(MC.YAxis | MC.YAxis);

        // Projecting the mapping origin gives the offset that puts the surface's own
        // corner at texel zero.
        const FLOAT UDot = MC.XAxis | MC.Origin;
        const FLOAT VDot = MC.YAxis | MC.Origin;

        const FTextureInfo& T = *Surface.Texture;

        if (Obs.CSamples == 0)
        {
            Obs.MapXAxisLenMin = Obs.MapXAxisLenMax = XLen;
            Obs.MapYAxisLenMin = Obs.MapYAxisLenMax = YLen;
            Obs.TexPanXMin = Obs.TexPanXMax = T.Pan.X;
            Obs.TexPanYMin = Obs.TexPanYMax = T.Pan.Y;
            Obs.TexUScaleMin = Obs.TexUScaleMax = T.UScale;
            Obs.TexVScaleMin = Obs.TexVScaleMax = T.VScale;
            Obs.DiffUNormMin = Obs.DiffVNormMin = Obs.LightUNormMin = Obs.LightVNormMin =  1.0e30f;
            Obs.DiffUNormMax = Obs.DiffVNormMax = Obs.LightUNormMax = Obs.LightVNormMax = -1.0e30f;
            Obs.LightSizeMin = MAXINT;
            Obs.LightUScaleMin = Obs.LightVScaleMin =  1.0e30f;
            Obs.LightUScaleMax = Obs.LightVScaleMax = -1.0e30f;
            Obs.LightScaledUMin = Obs.LightScaledVMin =  1.0e30f;
            Obs.LightScaledUMax = Obs.LightScaledVMax = -1.0e30f;
        }
        Obs.CSamples++;

        if (XLen < Obs.MapXAxisLenMin) Obs.MapXAxisLenMin = XLen;
        if (XLen > Obs.MapXAxisLenMax) Obs.MapXAxisLenMax = XLen;
        if (YLen < Obs.MapYAxisLenMin) Obs.MapYAxisLenMin = YLen;
        if (YLen > Obs.MapYAxisLenMax) Obs.MapYAxisLenMax = YLen;
        if (T.Pan.X   < Obs.TexPanXMin)   Obs.TexPanXMin   = T.Pan.X;
        if (T.Pan.X   > Obs.TexPanXMax)   Obs.TexPanXMax   = T.Pan.X;
        if (T.Pan.Y   < Obs.TexPanYMin)   Obs.TexPanYMin   = T.Pan.Y;
        if (T.Pan.Y   > Obs.TexPanYMax)   Obs.TexPanYMax   = T.Pan.Y;
        if (T.UScale  < Obs.TexUScaleMin) Obs.TexUScaleMin = T.UScale;
        if (T.UScale  > Obs.TexUScaleMax) Obs.TexUScaleMax = T.UScale;
        if (T.VScale  < Obs.TexVScaleMin) Obs.TexVScaleMin = T.VScale;
        if (T.VScale  > Obs.TexVScaleMax) Obs.TexVScaleMax = T.VScale;

        if (Surface.LightMap)
        {
            const INT LS = Surface.LightMap->USize;
            if (LS < Obs.LightSizeMin) Obs.LightSizeMin = LS;
            if (LS > Obs.LightSizeMax) Obs.LightSizeMax = LS;
            const FLOAT LU = Surface.LightMap->UScale, LV = Surface.LightMap->VScale;
            if (LU < Obs.LightUScaleMin) Obs.LightUScaleMin = LU;
            if (LU > Obs.LightUScaleMax) Obs.LightUScaleMax = LU;
            if (LV < Obs.LightVScaleMin) Obs.LightVScaleMin = LV;
            if (LV > Obs.LightVScaleMax) Obs.LightVScaleMax = LV;
        }

        for (const FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next)
        {
            for (int i = 0; i < Poly->NumPts; i++)
            {
                if (!Poly->Pts[i])
                    continue;
                const FVector& P = Poly->Pts[i]->Point;

                const FLOAT Uc = (MC.XAxis | P) - UDot;
                const FLOAT Vc = (MC.YAxis | P) - VDot;

                if (T.USize > 0 && T.VSize > 0)
                {
                    const FLOAT Un = (Uc - T.Pan.X) / (FLOAT)T.USize;
                    const FLOAT Vn = (Vc - T.Pan.Y) / (FLOAT)T.VSize;
                    if (Un < Obs.DiffUNormMin) Obs.DiffUNormMin = Un;
                    if (Un > Obs.DiffUNormMax) Obs.DiffUNormMax = Un;
                    if (Vn < Obs.DiffVNormMin) Obs.DiffVNormMin = Vn;
                    if (Vn > Obs.DiffVNormMax) Obs.DiffVNormMax = Vn;
                }

                if (Surface.LightMap && Surface.LightMap->USize > 0 && Surface.LightMap->VSize > 0)
                {
                    const FTextureInfo& L = *Surface.LightMap;
                    const FLOAT Un = (Uc - L.Pan.X) / (FLOAT)L.USize;
                    const FLOAT Vn = (Vc - L.Pan.Y) / (FLOAT)L.VSize;
                    if (Un < Obs.LightUNormMin) Obs.LightUNormMin = Un;
                    if (Un > Obs.LightUNormMax) Obs.LightUNormMax = Un;
                    if (Vn < Obs.LightVNormMin) Obs.LightVNormMin = Vn;
                    if (Vn > Obs.LightVNormMax) Obs.LightVNormMax = Vn;

                    if (L.UScale != 0.0f && L.VScale != 0.0f)
                    {
                        const FLOAT Us = Un / L.UScale;
                        const FLOAT Vs = Vn / L.VScale;
                        if (Us < Obs.LightScaledUMin) Obs.LightScaledUMin = Us;
                        if (Us > Obs.LightScaledUMax) Obs.LightScaledUMax = Us;
                        if (Vs < Obs.LightScaledVMin) Obs.LightScaledVMin = Vs;
                        if (Vs > Obs.LightScaledVMax) Obs.LightScaledVMax = Vs;
                    }
                }
            }
        }
    }

    if (ShouldDetail(CALL_DrawComplexSurface))
    {
        PROBE_LOGF("DrawComplexSurface  polyFlags=0x%08X  tex=%s light=%s macro=%s detail=%s fog=%s",
            Surface.PolyFlags,
            Surface.Texture       ? TEXT("y") : TEXT("n"),
            Surface.LightMap      ? TEXT("y") : TEXT("n"),
            Surface.MacroTexture  ? TEXT("y") : TEXT("n"),
            Surface.DetailTexture ? TEXT("y") : TEXT("n"),
            Surface.FogMap        ? TEXT("y") : TEXT("n"));

        if (Surface.Texture)
            PROBE_LOGF("DrawComplexSurface  tex fmt=%i size=%ix%i scale=%fx%f mips=%i pal=%s",
                (INT)Surface.Texture->Format, Surface.Texture->USize, Surface.Texture->VSize,
                Surface.Texture->UScale, Surface.Texture->VScale, Surface.Texture->NumMips,
                Surface.Texture->Palette ? TEXT("y") : TEXT("n"));
    }
}

void UProbeRenderDevice::DrawGouraudPolygon(FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span)
{
    Record(CALL_DrawGouraudPolygon);
    NoteFrame(Frame);
    NoteTexture(Info);
    Obs.FlagsGouraud |= PolyFlags;

    // Vertex format evidence. Sampling every call would cost more than the renderer
    // it is measuring, so take one polygon in every 64.
    static INT Stride = 0;
    if (Pts && ((++Stride & 63) == 0))
    {
        for (int i = 0; i < NumPts; i++)
        {
            const FTransTexture* P = Pts[i];
            if (!P)
                continue;

            if (Obs.GVerts == 0)
            {
                Obs.GScreenMinX = Obs.GScreenMaxX = P->ScreenX;
                Obs.GScreenMinY = Obs.GScreenMaxY = P->ScreenY;
                Obs.GRzMin      = Obs.GRzMax      = P->RZ;
                Obs.GPointZMin  = Obs.GPointZMax  = P->Point.Z;
                Obs.GUMin       = Obs.GUMax       = P->U;
                Obs.GVMin       = Obs.GVMax       = P->V;
                Obs.GLightMin   = Obs.GLightMax   = P->Light.X;
                Obs.GFogWMin    = Obs.GFogWMax    = P->Fog.W;
            }
            Obs.GVerts++;

            if (P->ScreenX < Obs.GScreenMinX) Obs.GScreenMinX = P->ScreenX;
            if (P->ScreenX > Obs.GScreenMaxX) Obs.GScreenMaxX = P->ScreenX;
            if (P->ScreenY < Obs.GScreenMinY) Obs.GScreenMinY = P->ScreenY;
            if (P->ScreenY > Obs.GScreenMaxY) Obs.GScreenMaxY = P->ScreenY;
            if (P->RZ      < Obs.GRzMin)      Obs.GRzMin      = P->RZ;
            if (P->RZ      > Obs.GRzMax)      Obs.GRzMax      = P->RZ;
            if (P->Point.Z < Obs.GPointZMin)  Obs.GPointZMin  = P->Point.Z;
            if (P->Point.Z > Obs.GPointZMax)  Obs.GPointZMax  = P->Point.Z;
            if (P->U       < Obs.GUMin)       Obs.GUMin       = P->U;
            if (P->U       > Obs.GUMax)       Obs.GUMax       = P->U;
            if (P->V       < Obs.GVMin)       Obs.GVMin       = P->V;
            if (P->V       > Obs.GVMax)       Obs.GVMax       = P->V;
            if (P->Fog.W   < Obs.GFogWMin)    Obs.GFogWMin    = P->Fog.W;
            if (P->Fog.W   > Obs.GFogWMax)    Obs.GFogWMax    = P->Fog.W;

            const FLOAT L[3] = { P->Light.X, P->Light.Y, P->Light.Z };
            for (int c = 0; c < 3; c++)
            {
                if (L[c] < Obs.GLightMin) Obs.GLightMin = L[c];
                if (L[c] > Obs.GLightMax) Obs.GLightMax = L[c];
            }

            Obs.GFlagsSeen |= (INT)P->Flags;

            if (Frame && (P->ScreenX < 0.0f || P->ScreenY < 0.0f ||
                          P->ScreenX > (FLOAT)Frame->X || P->ScreenY > (FLOAT)Frame->Y))
                Obs.GScreenOutside++;
        }
    }

    if (ShouldDetail(CALL_DrawGouraudPolygon))
    {
        PROBE_LOGF("DrawGouraudPolygon  numPts=%i  polyFlags=0x%08X  fmt=%i  span=%s",
            NumPts, PolyFlags, (INT)Info.Format, Span ? TEXT("y") : TEXT("n"));
        if (Pts && NumPts > 0 && Pts[0])
            PROBE_LOGF("DrawGouraudPolygon  v0 screen=(%f %f) RZ=%f pointZ=%f uv=(%f %f) light=(%f %f %f) fogW=%f flags=0x%02X",
                Pts[0]->ScreenX, Pts[0]->ScreenY, Pts[0]->RZ, Pts[0]->Point.Z,
                Pts[0]->U, Pts[0]->V,
                Pts[0]->Light.X, Pts[0]->Light.Y, Pts[0]->Light.Z,
                Pts[0]->Fog.W, (INT)Pts[0]->Flags);
    }
}

void UProbeRenderDevice::DrawTile(FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags)
{
    Record(CALL_DrawTile);
    NoteFrame(Frame);
    NoteTexture(Info);
    Obs.FlagsTile |= PolyFlags;

    // Coordinate-system evidence. We record ranges rather than individual calls:
    // a single tile tells us nothing, the envelope tells us the whole convention.
    if (Obs.TileCount == 0)
    {
        Obs.TileMinX = Obs.TileMaxX = X;
        Obs.TileMinY = Obs.TileMaxY = Y;
        Obs.TileMinW = Obs.TileMaxW = XL;
        Obs.TileMinH = Obs.TileMaxH = YL;
        Obs.TileMinZ = Obs.TileMaxZ = Z;
        Obs.TileMinU = Obs.TileMaxU = U;
        Obs.TileMinV = Obs.TileMaxV = V;
    }
    Obs.TileCount++;

    if (X      < Obs.TileMinX) Obs.TileMinX = X;
    if (X + XL > Obs.TileMaxX) Obs.TileMaxX = X + XL;
    if (Y      < Obs.TileMinY) Obs.TileMinY = Y;
    if (Y + YL > Obs.TileMaxY) Obs.TileMaxY = Y + YL;
    if (XL     < Obs.TileMinW) Obs.TileMinW = XL;
    if (XL     > Obs.TileMaxW) Obs.TileMaxW = XL;
    if (YL     < Obs.TileMinH) Obs.TileMinH = YL;
    if (YL     > Obs.TileMaxH) Obs.TileMaxH = YL;
    if (Z      < Obs.TileMinZ) Obs.TileMinZ = Z;
    if (Z      > Obs.TileMaxZ) Obs.TileMaxZ = Z;
    if (U      < Obs.TileMinU) Obs.TileMinU = U;
    if (U + UL > Obs.TileMaxU) Obs.TileMaxU = U + UL;
    if (V      < Obs.TileMinV) Obs.TileMinV = V;
    if (V + VL > Obs.TileMaxV) Obs.TileMaxV = V + VL;

    if (Frame && (X < 0.0f || Y < 0.0f || X + XL > (FLOAT)Frame->X || Y + YL > (FLOAT)Frame->Y))
        Obs.TileOutsideFrame++;

    // Is one texel exactly one pixel, or is the tile scaled?
    if (Abs(XL - UL) < 0.01f && Abs(YL - VL) < 0.01f)
        Obs.TileTexelPixel1to1++;
    else
        Obs.TileTexelPixelOther++;

    // Are U/V texel coordinates inside the texture, or normalised 0..1?
    if (U + UL <= (FLOAT)Info.USize + 0.01f && V + VL <= (FLOAT)Info.VSize + 0.01f)
        Obs.TileUvWithinTexture++;
    else
        Obs.TileUvOutsideTexture++;

    if (Z <= 0.0f)
        Obs.TileNonPositiveZ++;

    if (ShouldDetail(CALL_DrawTile))
        PROBE_LOGF("DrawTile  pos=(%f %f) size=(%f %f) uv=(%f %f) uvSize=(%f %f) Z=%f flags=0x%08X",
            X, Y, XL, YL, U, V, UL, VL, Z, PolyFlags);
}

void UProbeRenderDevice::Draw3DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector OrigP, FVector OrigQ)
{
    Record(CALL_Draw3DLine);
    NoteFrame(Frame);
}

void UProbeRenderDevice::Draw2DClippedLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2)
{
    Record(CALL_Draw2DClippedLine);
    NoteFrame(Frame);
}

void UProbeRenderDevice::Draw2DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2)
{
    Record(CALL_Draw2DLine);
    NoteFrame(Frame);
}

void UProbeRenderDevice::Draw2DPoint(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z)
{
    Record(CALL_Draw2DPoint);
    NoteFrame(Frame);
}

void UProbeRenderDevice::ClearZ(FSceneNode* Frame)
{
    Record(CALL_ClearZ);
    NoteFrame(Frame);
    if (ShouldDetail(CALL_ClearZ))
        PROBE_LOG("ClearZ");
}

// ---------------------------------------------------------------------------
// Selection, stats, readback
// ---------------------------------------------------------------------------

void UProbeRenderDevice::PushHit(const BYTE* Data, INT Count)
{
    Record(CALL_PushHit);
    // Open question spec-01/6: is hit testing used during normal play at all?
    if (ShouldDetail(CALL_PushHit))
        PROBE_LOGF("PushHit  count=%i", Count);
}

void UProbeRenderDevice::PopHit(INT Count, UBOOL bForce)
{
    Record(CALL_PopHit);
    if (ShouldDetail(CALL_PopHit))
        PROBE_LOGF("PopHit  count=%i force=%i", Count, (INT)bForce);
}

void UProbeRenderDevice::GetStats(TCHAR* Result)
{
    Record(CALL_GetStats);
    // The engine's buffer is 128 characters (docs/spec-01 section 3). Stay well inside it.
    if (Result)
        appSprintf(Result, TEXT("DXProbe: frames %i"), FrameIndex);
}

void UProbeRenderDevice::ReadPixels(FColor* Pixels)
{
    Record(CALL_ReadPixels);
    // Nothing is rendered, so there is nothing to hand back. Leave the buffer untouched.
    if (ShouldDetail(CALL_ReadPixels))
        PROBE_LOG("ReadPixels  (probe renders nothing)");
}

void UProbeRenderDevice::EndFlash()
{
    Record(CALL_EndFlash);
    if (ShouldDetail(CALL_EndFlash))
        PROBE_LOG("EndFlash");
}

void UProbeRenderDevice::DrawStats(FSceneNode* Frame)
{
    Record(CALL_DrawStats);
    NoteFrame(Frame);
}

void UProbeRenderDevice::PrecacheTexture(FTextureInfo& Info, DWORD PolyFlags)
{
    Record(CALL_PrecacheTexture);
    NoteTexture(Info);
    if (ShouldDetail(CALL_PrecacheTexture))
        PROBE_LOGF("PrecacheTexture  fmt=%i size=%ix%i flags=0x%08X",
            (INT)Info.Format, Info.USize, Info.VSize, PolyFlags);
}

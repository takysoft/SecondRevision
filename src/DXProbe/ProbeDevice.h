// DXProbe - the instrumentation device itself.
//
// Every URenderDevice entry point is implemented as a recorder: it bumps a counter,
// logs the first few calls with their arguments, and returns. Nothing is drawn.
//
// The vtable layout is fixed by the engine's own header (see docs/spec-01 section 3).
// We only override slots; the compiler places them from the base declaration, so the
// order of declarations in THIS file is for human readability, not for the ABI.
#pragma once

// ---------------------------------------------------------------------------
// Call recorder
// ---------------------------------------------------------------------------

// One row per entry point. Cheap to update on every call: the whole point is that
// instrumentation must not distort what we are measuring.
struct FCallRecord
{
    const TCHAR* Name;
    INT          Count;        // times called since the last report
    INT          Verbose;      // how many more calls should log their arguments
};

enum ECallSlot
{
    CALL_Init,
    CALL_SetRes,
    CALL_Exit,
    CALL_Flush,
    CALL_Exec,
    CALL_Lock,
    CALL_Unlock,
    CALL_DrawComplexSurface,
    CALL_DrawGouraudPolygon,
    CALL_DrawTile,
    CALL_Draw3DLine,
    CALL_Draw2DClippedLine,
    CALL_Draw2DLine,
    CALL_Draw2DPoint,
    CALL_ClearZ,
    CALL_PushHit,
    CALL_PopHit,
    CALL_GetStats,
    CALL_ReadPixels,
    CALL_EndFlash,
    CALL_DrawStats,
    CALL_SetSceneNode,
    CALL_PrecacheTexture,
    CALL_MAX
};

// ---------------------------------------------------------------------------
// Observations we accumulate across a run
// ---------------------------------------------------------------------------

struct FObservations
{
    // Which of the five optional texture slots of FSurfaceInfo actually arrive.
    // docs/spec-02 says all five may be null; this counts how often that happens.
    INT SurfaceTexturePresent;
    INT SurfaceTextureNull;
    INT SurfaceLightMapPresent;
    INT SurfaceMacroPresent;
    INT SurfaceDetailPresent;
    INT SurfaceFogMapPresent;

    // Polygon shapes handed to DrawComplexSurface.
    INT PolyDegenerate;      // NumPts < 3 - spec-01 open question 8
    INT PolyMinPts;
    INT PolyMaxPts;

    // Distinct texture formats seen, indexed by ETextureFormat (0..5 plus overflow).
    INT FormatSeen[8];

    // Bits ever observed set in PolyFlags, per call site. The same bit means
    // different things on different paths (docs/spec-02 section 1.3), so we keep
    // them apart instead of merging into one mask.
    DWORD FlagsComplex;
    DWORD FlagsGouraud;
    DWORD FlagsTile;

    // RenderLockFlags values ever seen.
    DWORD LockFlagsSeen;

    // Frame geometry range reported through FSceneNode.
    INT   FrameMinX, FrameMaxX;
    INT   FrameMinY, FrameMaxY;
    INT   MaxRecursion;      // >0 means portals or mirrors were rendered
    INT   MirrorSeen;        // frames with Mirror < 0

    // --- DrawTile coordinate system ---------------------------------------
    // Open question: are X/Y pixels from the top left of the frame, are U/V texels,
    // and is UL/XL a fixed texel-to-pixel ratio? Ranges answer all three.
    INT   TileCount;
    FLOAT TileMinX, TileMaxX;       // left edge, and right edge (X + XL)
    FLOAT TileMinY, TileMaxY;
    FLOAT TileMinW, TileMaxW;       // XL
    FLOAT TileMinH, TileMaxH;       // YL
    FLOAT TileMinZ, TileMaxZ;
    FLOAT TileMinU, TileMaxU;       // and the U extent (U + UL)
    FLOAT TileMinV, TileMaxV;
    INT   TileOutsideFrame;         // tiles falling outside 0..Frame->X / 0..Frame->Y
    INT   TileTexelPixel1to1;       // XL == UL and YL == VL
    INT   TileTexelPixelOther;
    INT   TileUvWithinTexture;      // U+UL <= USize and V+VL <= VSize
    INT   TileUvOutsideTexture;
    INT   TileNonPositiveZ;

    // --- Gouraud vertex format --------------------------------------------
    // In gameplay this path carries ~99% of the work, so its vertex layout is the
    // one that has to be right. FTransTexture already carries a projected screen
    // position, a per-vertex light and a fog value; the ranges tell us what units
    // they are in and whether the engine has already done the projection for us.
    INT   GVerts;
    FLOAT GScreenMinX, GScreenMaxX;
    FLOAT GScreenMinY, GScreenMaxY;
    FLOAT GRzMin, GRzMax;
    FLOAT GPointZMin, GPointZMax;
    FLOAT GUMin, GUMax;
    FLOAT GVMin, GVMax;
    FLOAT GLightMin, GLightMax;     // across R, G and B
    FLOAT GFogWMin, GFogWMax;
    INT   GScreenOutside;           // vertices outside 0..Frame->X / 0..Frame->Y
    INT   GFlagsSeen;               // union of FOutVector::Flags (clip outcodes)

    // --- DrawComplexSurface texture mapping --------------------------------
    // Unlike the gouraud path this one hands us no UV at all: only a mapping
    // coordinate frame (FSurfaceFacet::MapCoords) and world-space polygon points.
    // We reconstruct texture coordinates by projecting each point onto the mapping
    // axes, then ask the measurement whether the result lands inside the texture.
    INT   CSamples;
    FLOAT MapXAxisLenMin, MapXAxisLenMax;   // are the mapping axes unit vectors?
    FLOAT MapYAxisLenMin, MapYAxisLenMax;
    FLOAT TexPanXMin, TexPanXMax;
    FLOAT TexPanYMin, TexPanYMax;
    FLOAT TexUScaleMin, TexUScaleMax;
    FLOAT TexVScaleMin, TexVScaleMax;
    // Computed coordinate divided by texture size: 0..1 means exactly one tile.
    FLOAT DiffUNormMin, DiffUNormMax;
    FLOAT DiffVNormMin, DiffVNormMax;
    // Same for the light map, without any half-texel correction applied. If the
    // engine expects a correction, the range will sit slightly off 0..1.
    FLOAT LightUNormMin, LightUNormMax;
    FLOAT LightVNormMin, LightVNormMax;
    INT   LightSizeMin, LightSizeMax;
    FLOAT LightUScaleMin, LightUScaleMax;   // the light map has its own scale
    FLOAT LightVScaleMin, LightVScaleMax;
    // Same projection, but also divided by the light map's own scale. If this
    // lands in 0..1 the scale is the missing factor.
    FLOAT LightScaledUMin, LightScaledUMax;
    FLOAT LightScaledVMin, LightScaledVMax;
};

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------

class UProbeRenderDevice : public URenderDevice
{
#pragma warning(push, 1)
    DECLARE_CLASS(UProbeRenderDevice, URenderDevice, CLASS_Config)
#pragma warning(pop)

public:
    UProbeRenderDevice();
    void StaticConstructor();

    // --- URenderDevice contract, in the engine's own order -----------------
    virtual UBOOL Init(UViewport* InViewport, INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen);
    virtual UBOOL SetRes(INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen);
    virtual void  Exit();
    virtual void  Flush(UBOOL AllowPrecache);
    virtual UBOOL Exec(const TCHAR* Cmd, FOutputDevice& Ar);
    virtual void  Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize);
    virtual void  Unlock(UBOOL Blit);
    virtual void  DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet);
    virtual void  DrawGouraudPolygon(FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span);
    virtual void  DrawTile(FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags);
    virtual void  Draw3DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector OrigP, FVector OrigQ);
    virtual void  Draw2DClippedLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2);
    virtual void  Draw2DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2);
    virtual void  Draw2DPoint(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z);
    virtual void  ClearZ(FSceneNode* Frame);
    virtual void  PushHit(const BYTE* Data, INT Count);
    virtual void  PopHit(INT Count, UBOOL bForce);
    virtual void  GetStats(TCHAR* Result);
    virtual void  ReadPixels(FColor* Pixels);
    virtual void  EndFlash();
    virtual void  DrawStats(FSceneNode* Frame);
    virtual void  SetSceneNode(FSceneNode* Frame);
    virtual void  PrecacheTexture(FTextureInfo& Info, DWORD PolyFlags);

private:
    void Record(ECallSlot Slot);
    UBOOL ShouldDetail(ECallSlot Slot);   // true while this slot still logs arguments
    void NoteFrame(FSceneNode* Frame);
    void NoteTexture(const FTextureInfo& Info);
    void Report(const TCHAR* Reason);
    void ResetCounters();

    FCallRecord   Calls[CALL_MAX];
    FObservations Obs;
    INT           FrameIndex;   // Lock/Unlock pairs completed
};

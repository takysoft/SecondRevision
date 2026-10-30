#include "Pch.h"
#include "WaterProbe.h"
#include "RenderMath.h"
#include "Log.h"
#include <stdio.h>

enum
{
    MAX_ROWS    = 64,       // distinct (texture, flags, zones) the census keeps; the rest is only counted
    MAX_LISTED  = 12,       // printed of each kind
    MAX_DRAWN   = 8,        // distinct drawn kinds remembered per level
    NODE_CHECKS = 2000,     // drawn polygons the node check looks at per level
};

// The editor's own bits of a surface's flags: they differ between surfaces that look the same in the game.
static const DWORD EditorFlags = PF_Memorized | PF_Selected | PF_Highlighted | PF_FlatShaded | PF_EdCut;

// ---------------------------------------------------------------------------
// Zones
// ---------------------------------------------------------------------------

static AZoneInfo* ZoneActor(UModel* Model, INT Z)
{
    if (Z < 0 || Z >= Model->NumZones || Z >= FBspNode::MAX_ZONES)
        return NULL;
    AZoneInfo* Zone = Model->Zones[Z].ZoneActor;
    return (Zone && !Zone->bDeleteMe) ? Zone : NULL;
}

// A zone that is more than air: water, or one that hurts. Both count, because what the eye takes for water is not
// always a water zone to the engine, and a pool of acid is a liquid to the eye whatever the zone says.
static UBOOL ZoneIsLiquid(UModel* Model, INT Z)
{
    const AZoneInfo* Zone = ZoneActor(Model, Z);
    return (Zone && (Zone->bWaterZone || Zone->bPainZone)) ? 1 : 0;
}

// A liquid on exactly one side of the node - its back is iZone[0], its front iZone[1]. Wet gets the liquid's zone,
// Dry the other one.
static UBOOL NodeBorder(UModel* Model, INT iNode, INT& Wet, INT& Dry)
{
    if (iNode < 0 || iNode >= Model->Nodes.Num())
        return 0;
    const FBspNode& Node = Model->Nodes(iNode);
    const UBOOL bBack  = ZoneIsLiquid(Model, Node.iZone[0]);
    const UBOOL bFront = ZoneIsLiquid(Model, Node.iZone[1]);
    if (bBack == bFront)
        return 0;
    Wet = bBack ? Node.iZone[0] : Node.iZone[1];
    Dry = bBack ? Node.iZone[1] : Node.iZone[0];
    return 1;
}

// ---------------------------------------------------------------------------
// Text for the log
// ---------------------------------------------------------------------------

static void Append(TCHAR* Out, INT Max, const TCHAR* Text)
{
    INT n = 0;
    while (n < Max - 1 && Out[n])
        n++;
    for (INT k = 0; Text && Text[k] && n < Max - 1; k++, n++)
        Out[n] = Text[k];
    Out[n] = 0;
}

// "Package.Group.Name (Class)". Only while the texture is sure to live: during the census, or at the draw.
static void TextureText(UTexture* T, TCHAR* Out, INT Max)
{
    Out[0] = 0;
    if (!T)
    {
        Append(Out, Max, TEXT("no texture"));
        return;
    }
    TCHAR Path[512];
    Path[0] = 0;
    T->GetPathName(NULL, Path);
    Append(Out, Max, Path);
    Append(Out, Max, TEXT(" ("));
    Append(Out, Max, T->GetClass()->GetName());
    Append(Out, Max, TEXT(")"));
}

// The flags that tell a sheet of liquid from a wall, by name.
static void FlagNames(DWORD Flags, TCHAR* Out, INT Max)
{
    static const struct { DWORD Bit; const TCHAR* Name; } Known[] =
    {
        { PF_Invisible,   TEXT("Invisible") },   { PF_Masked,       TEXT("Masked") },
        { PF_Translucent, TEXT("Translucent") }, { PF_Modulated,    TEXT("Modulated") },
        { PF_NotSolid,    TEXT("NotSolid") },    { PF_Semisolid,    TEXT("Semisolid") },
        { PF_TwoSided,    TEXT("TwoSided") },    { PF_Portal,       TEXT("Portal") },
        { PF_FakeBackdrop,TEXT("FakeBackdrop") },{ PF_Environment,  TEXT("Environment") },
        { PF_AutoUPan,    TEXT("AutoUPan") },    { PF_AutoVPan,     TEXT("AutoVPan") },
        { PF_BigWavy,     TEXT("BigWavy") },     { PF_SmallWavy,    TEXT("SmallWavy") },
        { PF_Unlit,       TEXT("Unlit") },       { PF_Mirrored,     TEXT("Mirrored") },
    };
    Out[0] = 0;
    for (INT k = 0; k < ARRAY_COUNT(Known); k++)
        if (Flags & Known[k].Bit)
        {
            if (Out[0])
                Append(Out, Max, TEXT(" "));
            Append(Out, Max, Known[k].Name);
        }
    if (!Out[0])
        Append(Out, Max, TEXT("plain"));
}

static UBOOL ContainsWord(const TCHAR* Text, const char* Word)
{
    for (INT i = 0; Text && Text[i]; i++)
    {
        INT k = 0;
        for (; Word[k]; k++)
        {
            TCHAR C = Text[i + k];
            if (C >= 'A' && C <= 'Z')
                C = (TCHAR)(C - 'A' + 'a');
            if (C != (TCHAR)Word[k])
                break;
        }
        if (!Word[k])
            return 1;
    }
    return 0;
}

// Why a surface on no liquid's border still looks like one: the engine's wavy flags, a texture of a class other than
// the plain Texture (a procedural one), or a name that says so.
enum { Hint_Wavy = 1, Hint_Class = 2, Hint_Name = 4 };

static INT LiquidHints(UTexture* T, DWORD Flags)
{
    INT Hints = (Flags & (PF_SmallWavy | PF_BigWavy)) ? Hint_Wavy : 0;
    if (T)
    {
        if (appStricmp(T->GetClass()->GetName(), TEXT("Texture")) != 0)
            Hints |= Hint_Class;
        TCHAR Path[512];
        Path[0] = 0;
        T->GetPathName(NULL, Path);
        static const char* const Words[] = { "water", "wave", "ocean", "river", "acid", "sludge", "slime", "toxic" };
        for (INT w = 0; w < ARRAY_COUNT(Words); w++)
            if (ContainsWord(Path, Words[w]))
            {
                Hints |= Hint_Name;
                break;
            }
    }
    return Hints;
}

// ---------------------------------------------------------------------------
// The census: once per level, from the level's own BSP
// ---------------------------------------------------------------------------

enum { Row_Air = 0, Row_ZoneZero = 1, Row_Look = 2 };

struct FCensusRow
{
    UTexture* Texture;
    DWORD     Flags;
    INT       Kind;
    INT       Wet;          // the liquid's zone (the border kinds), or the hints (Row_Look)
    INT       Dry;          // the zone on the other side (the border kinds)
    INT       Count;        // nodes (the border kinds), or surfaces (Row_Look)
    FLOAT     TileU;        // one repeat of the texture on the first such surface, in world units (0: not known)
    FLOAT     TileV;
};

static FCensusRow GRows[MAX_ROWS];
static INT        GRowCount   = 0;
static INT        GRowsLost   = 0;

// How far each liquid zone's surface reaches across (world units, the smaller side of its largest level surface's
// extent - of all its borders' where it has none): how calm its waves are (RdxMath::WaterWaveStrength). Filled by the
// census, per level.
static FLOAT      GZoneAcross[FBspNode::MAX_ZONES];
// The liquids' level surfaces - what the ripples, the splashes, the lamps and the sea test things against: a height
// and an extent. A zone's border polygons that lie level with the liquid under them (RdxMath::WaterBorderIsSurface),
// those at one height together. Most zones have one: a pool, a canal, the sea. But one zone can hold water at several
// heights - two basins; the sea and a pool inside a base under it - and can have borders that are no surface at all,
// an upright pane between the water and a room. So the surfaces are listed by themselves, not a height a zone.
struct FWaterSurface
{
    INT   Zone;                         // the liquid's
    INT   Air;                          // the zone over it
    FLOAT Z;                            // its height: the mean of its corners'
    FLOAT MinX, MinY, MaxX, MaxY;       // its extent
    FLOAT ZSum;
    INT   Corners;
    INT   Clear;                        // how many of its pieces the level draws see-through (NodeSeeThrough)
};
enum { MAX_WATER_SURFACES = 128 };
static FWaterSurface GSurfaces[MAX_WATER_SURFACES];
static INT           GSurfaceCount  = 0;
static UBOOL         GAnyLevelWater = 0;
// Each of the level's surfaces: the surface whose texture mapping it is drawn with, -1 for its own (RdxWaterLeadOf) -
// for the census's level's model, of that many surfaces.
static INT*          GLeadOfSurf    = NULL;
static INT           GLeadSurfs     = 0;
static const UModel* GLeadModel     = NULL;
// The liquids' see-through borders with the zones around them, level or upright - the surface of a pool, a pane between
// a tank and a room: what the caustics' mask is drawn from (RdxWaterPortals). Each is a level's polygon as the BSP
// holds it: its corners, their middle and how far the farthest is from it, its liquid's zone, and its plane turned so
// that the front is the side that is not the liquid.
struct FWaterPortal
{
    FPlane  DryPlane;
    FVector Middle;
    FLOAT   Radius;
    INT     Zone;
    INT     First, Count;               // its corners in GPortalPoints
};
enum { MAX_WATER_PORTALS = 1024, MAX_PORTAL_POINTS = 8192 };
static FWaterPortal GPortals[MAX_WATER_PORTALS];
static FVector      GPortalPoints[MAX_PORTAL_POINTS];
static INT          GPortalCount      = 0;
static INT          GPortalPointCount = 0;
static INT          GPortalsLost      = 0;
static BYTE         GZoneClear[FBspNode::MAX_ZONES];      // the liquid zones that have such a border
// Each liquid zone's water (RdxMath::WaterBodyOf): the zones that touch under the water are one - a flooded room and
// the corridor beyond its doorway - and the caustics go by the water, not the zone. And the waters that have a
// see-through border: a camera in one of those is in a see-through liquid. Filled by the census, per level.
static INT          GZoneBody[FBspNode::MAX_ZONES];
static BYTE         GBodyClear[FBspNode::MAX_ZONES];
// How many see-through surfaces lay within the caustics' reach when they were last asked for (RdxWaterBoxes).
static INT          GBoxesNear = 0;
static INT WaterOf(INT Zone)
{
    return RdxMath::WaterBodyOf(GZoneBody, FBspNode::MAX_ZONES, Zone);
}
// The steady rate of the texture's waves (RdxWaterCapRate): the textures capped this level and what they had before -
// compared and restored only within the level (the census empties the list without following it).
static UBOOL      GCapRate      = 1;
static UTexture*  GCapped[8];
static FLOAT      GCappedWas[8];
static INT        GCappedCount  = 0;
// How often the field really stepped, said for the first seconds of a level: a measurement of the cap.
static DWORD      GStepHash     = 0;
static INT        GStepCount    = 0;
static DOUBLE     GStepSince    = 0.0;
static INT        GStepTold     = 0;

// The water texture's own wave field, read once a frame (ReadWaves, near RdxWaterSheet below).
static BYTE       GWaves[128 * 128];
static UTexture*  GWavesTexture   = NULL;        // compared only: the texture the field was read from, this level
static UBOOL      GWavesFresh     = 0;           // read this frame, not yet handed to the backend
static UBOOL      GWavesTried     = 0;           // this frame's read was tried already
static FLOAT      GWavesStepRms   = 0.0f;        // the field's height step over two cells, root mean square (ReadWaves)
// The level's sea (RdxWaterSea): its zone and which of the surfaces it is, one of that surface's nodes, the sides of
// its extent that end in sky, its height.
static INT        GSeaZone    = -1;
static INT        GSeaSurface = -1;
static INT        GSeaNode    = -1;
static INT        GSeaSides   = 0;
static FLOAT      GSeaZ       = 0.0f;
// Its black edge walls (RdxWaterIsSeaEdge): a mark per surface of the level, the engine's heap.
static BYTE*      GSeaEdge     = NULL;
static INT        GSeaEdgeSize = 0;

static void ForgetSeaEdge()
{
    if (GSeaEdge)
        appFree(GSeaEdge);
    GSeaEdge     = NULL;
    GSeaEdgeSize = 0;
}

static void AddRow(UTexture* T, DWORD Flags, INT Kind, INT Wet, INT Dry, FLOAT TileU = 0.0f, FLOAT TileV = 0.0f)
{
    for (INT r = 0; r < GRowCount; r++)
    {
        FCensusRow& R = GRows[r];
        if (R.Texture == T && R.Flags == Flags && R.Kind == Kind && R.Wet == Wet && R.Dry == Dry)
        {
            R.Count++;
            return;
        }
    }
    if (GRowCount == MAX_ROWS)
    {
        GRowsLost++;
        return;
    }
    FCensusRow& R = GRows[GRowCount++];
    R.Texture = T;
    R.Flags   = Flags;
    R.Kind    = Kind;
    R.Wet     = Wet;
    R.Dry     = Dry;
    R.Count   = 1;
    R.TileU   = TileU;
    R.TileV   = TileV;
}

// One repeat of the surface's texture in world units: the texture's size over the length of the surface's texture
// axis (texels per world unit), times the texture's own scale - the mapping the renderers draw with.
static void TileSize(UModel* Model, const FBspSurf& Surf, FLOAT& TileU, FLOAT& TileV)
{
    TileU = TileV = 0.0f;
    UTexture* T = Surf.Texture;
    if (!T)
        return;
    const FLOAT Scale = (T->Scale > 0.0f) ? T->Scale : 1.0f;
    if (Surf.vTextureU >= 0 && Surf.vTextureU < Model->Vectors.Num())
    {
        const FLOAT L = Model->Vectors(Surf.vTextureU).Size();
        if (L > 0.0f)
            TileU = T->USize * Scale / L;
    }
    if (Surf.vTextureV >= 0 && Surf.vTextureV < Model->Vectors.Num())
    {
        const FLOAT L = Model->Vectors(Surf.vTextureV).Size();
        if (L > 0.0f)
            TileV = T->VSize * Scale / L;
    }
}

// The texture's own settings: for the procedural water classes, everything that shapes the waves. Read through the
// engine's property list, so no header for those classes is needed. The settings of the classes below Texture are
// printed in full, Texture's own only where they are set; the bitmap's and the object's are left out.
static void DumpTextureSettings(UTexture* T)
{
    if (!T)
        return;
    TCHAR Tex[640];
    TextureText(T, Tex, 640);
    RDX_LOGF("water texture: %s - %ix%i", Tex, T->USize, T->VSize);
    static TCHAR Value[8192];                   // the engine writes these without a length: room to spare
    for (TFieldIterator<UProperty> It(T->GetClass()); It; ++It)
    {
        UProperty* P = *It;
        UClass* Owner = P->GetOwnerClass();
        if (!Owner || Owner == UBitmap::StaticClass() || Owner == UObject::StaticClass())
            continue;
        if (appStricmp(P->GetClass()->GetName(), TEXT("ArrayProperty")) == 0)
            continue;                           // a dynamic array: nothing that shapes the waves, and unbounded
        if (P->ArrayDim > 256 || !appStricmp(P->GetName(), TEXT("SourceFields")))
            continue;                           // the simulation's own lookup tables and field memory
        const UBOOL bOwn = (Owner != UTexture::StaticClass());
        INT Shown = 0, Set = 0;
        for (INT i = 0; i < P->ArrayDim; i++)
        {
            Value[0] = 0;
            P->ExportTextItem(Value, (BYTE*)T + P->Offset + i * P->ElementSize, NULL, 0);
            const UBOOL bEmpty = !Value[0] || !appStrcmp(Value, TEXT("0")) || !appStrcmp(Value, TEXT("0.000000")) ||
                                 !appStrcmp(Value, TEXT("False")) || !appStrcmp(Value, TEXT("None")) ||
                                 !appStrcmp(Value, TEXT("()"));
            if (bEmpty && (!bOwn || P->ArrayDim > 1))
                continue;
            Set++;
            if (Shown >= 12)
                continue;
            Shown++;
            Value[400] = 0;
            if (P->ArrayDim > 1)
                RDX_LOGF("water texture:   %s[%i] = %s", P->GetName(), i, Value);
            else
                RDX_LOGF("water texture:   %s = %s", P->GetName(), Value);
        }
        if (Set > Shown)
            RDX_LOGF("water texture:   %s: %i more set", P->GetName(), Set - Shown);
    }
}

// The rows of one kind, the largest first.
static void PrintRows(INT Kind, INT Limit)
{
    UBOOL Done[MAX_ROWS];
    for (INT r = 0; r < MAX_ROWS; r++)
        Done[r] = 0;
    INT Printed = 0, Left = 0;
    for (INT r = 0; r < GRowCount; r++)
        if (GRows[r].Kind == Kind)
            Left++;
    for (; Printed < Limit; Printed++)
    {
        INT Best = -1;
        for (INT r = 0; r < GRowCount; r++)
            if (!Done[r] && GRows[r].Kind == Kind && (Best < 0 || GRows[r].Count > GRows[Best].Count))
                Best = r;
        if (Best < 0)
            break;
        Done[Best] = 1;
        const FCensusRow& R = GRows[Best];
        TCHAR Tex[640], Names[256];
        TextureText(R.Texture, Tex, 640);
        FlagNames(R.Flags, Names, 256);
        if (Kind == Row_Look)
            RDX_LOGF("water census:   %s - %s (0x%08X) - %i surfaces;%s%s%s", Tex, Names, R.Flags, R.Count,
                     (R.Wet & Hint_Wavy)  ? TEXT(" wavy flags") : TEXT(""),
                     (R.Wet & Hint_Class) ? TEXT(" not a plain texture") : TEXT(""),
                     (R.Wet & Hint_Name)  ? TEXT(" the name") : TEXT(""));
        else
            RDX_LOGF("water census:   %s - %s (0x%08X) - %i nodes, liquid zone %i, other side zone %i; one repeat of "
                     "the texture %.0f x %.0f units", Tex, Names, R.Flags, R.Count, R.Wet, R.Dry, R.TileU, R.TileV);
    }
    if (Left > Printed)
        RDX_LOGF("water census:   ... and %i more", Left - Printed);
}

// Whether a border node is a piece of its liquid's surface (RdxMath::WaterBorderIsSurface), and at what height: the
// mean of its corners'. Wet is the liquid's zone (NodeBorder).
static UBOOL NodeSurface(UModel* Model, INT iNode, INT Wet, FLOAT& Z)
{
    if (iNode < 0 || iNode >= Model->Nodes.Num())
        return 0;
    const FBspNode& Node = Model->Nodes(iNode);
    if (Node.NumVertices < 3 || !RdxMath::WaterBorderIsSurface(Node.Plane.Z, Node.iZone[0] == Wet))
        return 0;
    FLOAT Sum   = 0.0f;
    INT   Count = 0;
    for (INT v = 0; v < Node.NumVertices; v++)
    {
        const INT iVert  = Node.iVertPool + v;
        const INT iPoint = (iVert >= 0 && iVert < Model->Verts.Num()) ? Model->Verts(iVert).pVertex : -1;
        if (iPoint < 0 || iPoint >= Model->Points.Num())
            continue;
        Sum += Model->Points(iPoint).Z;
        Count++;
    }
    if (!Count)
        return 0;
    Z = Sum / (FLOAT)Count;
    return 1;
}

// Whether the level draws a border node see-through: where the renderer's own see-through water is drawn
// (RdxWaterSheet), and so where the caustics belong. The texture's own flags count as the surface's, as at the draw.
static UBOOL NodeSeeThrough(UModel* Model, const FBspNode& Node)
{
    if (Node.iSurf < 0 || Node.iSurf >= Model->Surfs.Num())
        return 0;
    const FBspSurf& Surf  = Model->Surfs(Node.iSurf);
    const DWORD     Flags = Surf.PolyFlags | (Surf.Texture ? Surf.Texture->PolyFlags : 0);
    return ((Flags & PF_Translucent) && !(Flags & PF_Invisible)) ? 1 : 0;
}

// A see-through border into the list (the census). One the list has no room for is counted, and has no caustics
// behind it.
static void AddPortal(UModel* Model, const FBspNode& Node, INT Wet)
{
    if (Node.NumVertices < 3)
        return;
    if (GPortalCount >= MAX_WATER_PORTALS || GPortalPointCount + Node.NumVertices > MAX_PORTAL_POINTS)
    {
        GPortalsLost++;
        return;
    }
    FWaterPortal& P = GPortals[GPortalCount];
    P.First = GPortalPointCount;
    P.Count = 0;
    P.Zone  = Wet;
    FVector Sum(0.0f, 0.0f, 0.0f);
    for (INT v = 0; v < Node.NumVertices; v++)
    {
        const INT iVert  = Node.iVertPool + v;
        const INT iPoint = (iVert >= 0 && iVert < Model->Verts.Num()) ? Model->Verts(iVert).pVertex : -1;
        if (iPoint < 0 || iPoint >= Model->Points.Num())
            return;                     // a polygon without all its corners is left out whole
        GPortalPoints[P.First + P.Count++] = Model->Points(iPoint);
        Sum += Model->Points(iPoint);
    }
    P.Middle = Sum / (FLOAT)P.Count;
    P.Radius = 0.0f;
    for (INT v = 0; v < P.Count; v++)
        P.Radius = Max(P.Radius, (GPortalPoints[P.First + v] - P.Middle).Size());
    // The node's front is iZone[1].
    P.DryPlane = (Node.iZone[1] != Wet) ? Node.Plane : Node.Plane.Flip();
    GPortalPointCount += P.Count;
    GPortalCount++;
    GZoneClear[Wet] = 1;
}

// A zone's surface at a height, or -1.
static INT SurfaceOf(INT Zone, FLOAT Z)
{
    for (INT s = 0; s < GSurfaceCount; s++)
        if (GSurfaces[s].Zone == Zone && Abs(GSurfaces[s].Z - Z) <= RdxMath::WaterSurfaceStep)
            return s;
    return -1;
}

// The surface of a zone a thing is at: of the zone's surfaces it is over or under - their extent, Margin wider - the
// nearest in height; -1 where the zone has none there.
static INT SurfaceNear(INT Zone, const FVector& At, FLOAT Margin)
{
    INT   Best  = -1;
    FLOAT BestD = 0.0f;
    for (INT s = 0; s < GSurfaceCount; s++)
    {
        const FWaterSurface& S = GSurfaces[s];
        if (S.Zone != Zone || At.X < S.MinX - Margin || At.X > S.MaxX + Margin || At.Y < S.MinY - Margin ||
            At.Y > S.MaxY + Margin)
            continue;
        const FLOAT D = Abs(At.Z - S.Z);
        if (Best < 0 || D < BestD)
        {
            Best  = s;
            BestD = D;
        }
    }
    return Best;
}

// The sea, for EndlessWater: of the level water surfaces wide enough to be one (RdxMath::WaterIsSeaSized), the biggest
// whose outer line has a sky window on it - an upright face between zone 0 and the water or the air over it, flagged
// FakeBackdrop, every corner of it on the same side of the water's extent. Beyond those the engine draws no level at
// all, only the sky zone, and under the horizon that is black (docs/terv-vegtelen-viz.md; measured on Liberty Island,
// where all four sides are CoreTexMisc.Marker_sky).
static void FindSea(UModel* Model)
{
    GSeaZone    = -1;
    GSeaSurface = -1;
    GSeaNode    = -1;
    GSeaSides   = 0;
    ForgetSeaEdge();
    // The level surfaces, biggest first.
    INT Order[MAX_WATER_SURFACES];
    INT Count = 0;
    auto AreaOf = [](INT s) -> FLOAT
    {
        return (GSurfaces[s].MaxX - GSurfaces[s].MinX) * (GSurfaces[s].MaxY - GSurfaces[s].MinY);
    };
    for (INT i = 0; i < GSurfaceCount; i++)
    {
        const FLOAT Area = AreaOf(i);
        INT k = Count++;
        while (k > 0 && AreaOf(Order[k - 1]) < Area)
        {
            Order[k] = Order[k - 1];
            k--;
        }
        Order[k] = i;
    }
    const FLOAT Tol = 96.0f;       // how far off the extent's line a window may stand and still be on it
    for (INT i = 0; i < Count && GSeaZone < 0; i++)
    {
        const FWaterSurface& Sea = GSurfaces[Order[i]];
        if (!RdxMath::WaterIsSeaSized(Sea.MaxX - Sea.MinX, Sea.MaxY - Sea.MinY))
            continue;
        const INT   Wet   = Sea.Zone;
        const INT   Air   = Sea.Air;
        const FLOAT H     = Sea.Z;
        const FLOAT Ext[4] = { Sea.MinX, Sea.MaxX, Sea.MinY, Sea.MaxY };
        INT Sides = 0;
        for (INT n = 0; n < Model->Nodes.Num(); n++)
        {
            const FBspNode& Node = Model->Nodes(n);
            if (Node.NumVertices < 3 || Abs(Node.Plane.Z) > 0.3f)
                continue;
            const INT Other = (Node.iZone[0] == 0) ? Node.iZone[1] : ((Node.iZone[1] == 0) ? Node.iZone[0] : -1);
            if (Other <= 0 || (Other != Wet && Other != Air))
                continue;
            if (Node.iSurf < 0 || Node.iSurf >= Model->Surfs.Num() ||
                !(Model->Surfs(Node.iSurf).PolyFlags & PF_FakeBackdrop))
                continue;
            INT   Side = -1;
            UBOOL bOne = 1;
            FLOAT Lo = 1.0e30f, Hi = -1.0e30f;
            for (INT v = 0; v < Node.NumVertices && bOne; v++)
            {
                const INT iVert = Node.iVertPool + v;
                const INT iPoint = (iVert >= 0 && iVert < Model->Verts.Num()) ? Model->Verts(iVert).pVertex : -1;
                if (iPoint < 0 || iPoint >= Model->Points.Num())
                {
                    bOne = 0;
                    break;
                }
                const FVector& P = Model->Points(iPoint);
                Lo = Min(Lo, P.Z);
                Hi = Max(Hi, P.Z);
                const FLOAT D[4] = { Abs(P.X - Ext[0]), Abs(P.X - Ext[1]), Abs(P.Y - Ext[2]), Abs(P.Y - Ext[3]) };
                INT Best = 0;
                for (INT k = 1; k < 4; k++)
                    if (D[k] < D[Best])
                        Best = k;
                if (D[Best] > Tol || (Side >= 0 && Side != Best))
                    bOne = 0;
                Side = Best;
            }
            // Standing where the water is: from below its surface up past it, give or take.
            if (bOne && Side >= 0 && Lo <= H + 512.0f && Hi >= H - 512.0f)
                Sides |= (1 << Side);
        }
        if (!Sides)
            continue;
        GSeaZone    = Wet;
        GSeaSurface = Order[i];
        GSeaSides   = Sides;
        GSeaZ       = H;
        // One of the surface's own nodes - the ring's polygons carry it, so the drawing takes them for the sea
        // (RdxWaterSheet). Not any border of the zone: an upright pane of it, or a pool of the same zone at another
        // height, is not the sea.
        for (INT n = 0; n < Model->Nodes.Num() && GSeaNode < 0; n++)
        {
            INT   W = 0, D = 0;
            FLOAT Z = 0.0f;
            if (NodeBorder(Model, n, W, D) && W == Wet && D != 0 && NodeSurface(Model, n, W, Z) &&
                Abs(Z - H) <= RdxMath::WaterSurfaceStep)
                GSeaNode = n;
        }
        // And the walls on its outer line that the engine DOES draw: the same sky marker texture without the
        // FakeBackdrop flag, black and opaque (measured on Liberty Island: 51 million square units of them under 282
        // million of sky windows). They are the black band, and they would stand in front of the ring. Their tops as
        // well, whatever way they face: the same marker on the same line, and with the wall under it gone, a top left
        // alone floats over the water as a black strip (Liberty Island, from the shore).
        const INT NumSurfs = Model->Surfs.Num();
        GSeaEdge = (BYTE*)appMalloc(NumSurfs > 0 ? NumSurfs : 1, TEXT("RdxWaterSeaEdge"));
        if (GSeaEdge)
        {
            appMemzero(GSeaEdge, NumSurfs > 0 ? NumSurfs : 1);
            GSeaEdgeSize = NumSurfs;
            INT Marked = 0, Tops = 0;
            for (INT n = 0; n < Model->Nodes.Num(); n++)
            {
                const FBspNode& Node = Model->Nodes(n);
                if (Node.NumVertices < 3 || Node.iSurf < 0 || Node.iSurf >= NumSurfs)
                    continue;
                const INT Other = (Node.iZone[0] == 0) ? Node.iZone[1] : ((Node.iZone[1] == 0) ? Node.iZone[0] : -1);
                if (Other <= 0 || (Other != Wet && Other != Air))
                    continue;
                const FBspSurf& Surf = Model->Surfs(Node.iSurf);
                if ((Surf.PolyFlags & PF_FakeBackdrop) || !Surf.Texture ||
                    !appStrstr(Surf.Texture->GetName(), TEXT("Marker_sky")))
                    continue;
                INT   Side = -1;
                UBOOL bOne = 1;
                for (INT v = 0; v < Node.NumVertices && bOne; v++)
                {
                    const INT iVert  = Node.iVertPool + v;
                    const INT iPoint = (iVert >= 0 && iVert < Model->Verts.Num()) ? Model->Verts(iVert).pVertex : -1;
                    if (iPoint < 0 || iPoint >= Model->Points.Num())
                    {
                        bOne = 0;
                        break;
                    }
                    const FVector& P = Model->Points(iPoint);
                    const FLOAT D[4] = { Abs(P.X - Ext[0]), Abs(P.X - Ext[1]), Abs(P.Y - Ext[2]), Abs(P.Y - Ext[3]) };
                    INT Best = 0;
                    for (INT k = 1; k < 4; k++)
                        if (D[k] < D[Best])
                            Best = k;
                    if (D[Best] > Tol || (Side >= 0 && Side != Best))
                        bOne = 0;
                    Side = Best;
                }
                if (bOne && !GSeaEdge[Node.iSurf])
                {
                    GSeaEdge[Node.iSurf] = 1;
                    Marked++;
                    if (Abs(Node.Plane.Z) > 0.3f)
                        Tops++;
                }
            }
            // How many of the level's surfaces carry the marker at all, and are not sky windows: the rest are left
            // as the level has them.
            INT AllMarker = 0;
            for (INT s = 0; s < NumSurfs; s++)
            {
                const FBspSurf& Surf = Model->Surfs(s);
                if (!(Surf.PolyFlags & PF_FakeBackdrop) && Surf.Texture &&
                    appStrstr(Surf.Texture->GetName(), TEXT("Marker_sky")))
                    AllMarker++;
            }
            RDX_LOGF("water census: the sea's black edge walls - %i surfaces, %i of them facing up or down (a wall's "
                     "top or foot); of the level's %i surfaces with the sky marker that are no sky window, the others "
                     "are drawn as they are", Marked, Tops, AllMarker);
        }
    }
    if (GSeaZone >= 0 && GSeaNode >= 0 && GSeaSurface >= 0)
        RDX_LOGF("water census: the sea is zone %i, level at %.0f, %.0f..%.0f x %.0f..%.0f; sky windows on the side of "
                 "the least X %s, the most X %s, the least Y %s, the most Y %s (node %i)", GSeaZone, GSeaZ,
                 GSurfaces[GSeaSurface].MinX, GSurfaces[GSeaSurface].MaxX, GSurfaces[GSeaSurface].MinY,
                 GSurfaces[GSeaSurface].MaxY,
                 (GSeaSides & 1) ? TEXT("yes") : TEXT("no"), (GSeaSides & 2) ? TEXT("yes") : TEXT("no"),
                 (GSeaSides & 4) ? TEXT("yes") : TEXT("no"), (GSeaSides & 8) ? TEXT("yes") : TEXT("no"), GSeaNode);
    else
    {
        GSeaZone = GSeaNode = GSeaSurface = -1;
        RDX_LOG("water census: no sea - no level water surface wide enough ends in sky windows");
    }
}

// ---------------------------------------------------------------------------
// The fountains' pools
// ---------------------------------------------------------------------------

// An ornamental fountain's pool (RdxMath::FountainFootInPool): a level surface its curtain comes down into, with the
// curtain's foot - its lowest edge, the two ends on the level - that the rings on the pool spread from, and how high
// over the pool the curtain's top is. The level's own, found with the census. A pool past what the list holds stays
// what the level draws.
struct FFountainPool
{
    INT   iSurf;
    FLOAT MinX, MinY, MaxX, MaxY, Z;
    FLOAT FootA[2], FootB[2];
    FLOAT Rise;
    FLOAT Spread;       // half of how far apart its curtains stand: how thick its falling water is
    FLOAT Across;       // the whole basin's lesser extent, for how calm its water is
    INT   Feet;         // how many curtains come down into it
    INT   First;        // the pool it carries on (its own place where it is one's first)
    UBOOL bLiquid;      // a liquid's border as well: water whatever the fountain
};
enum { MAX_FOUNTAIN_POOLS = 32, MAX_FOUNTAIN_FEET = 64 };
static FFountainPool GPools[MAX_FOUNTAIN_POOLS];
static INT           GPoolCount = 0;
// The curtains that come down into a pool - the ones whose water the renderer makes itself: each one's surface, the
// two ends of its foot on the level, its pool's height and how high over that its top is. A fountain is often a slab
// with a curtain down either face: each falls where it stands.
struct FFountainCurtain
{
    INT   iSurf;
    FLOAT A[2], B[2];
    FLOAT Z, Rise;
};
static FFountainCurtain GCurtainSurfs[MAX_FOUNTAIN_FEET];
static INT              GCurtainSurfCount = 0;

static void AddCurtain(INT iSurf, const FLOAT* A, const FLOAT* B, FLOAT PoolZ, FLOAT TopZ)
{
    for (INT i = 0; i < GCurtainSurfCount; i++)
        if (GCurtainSurfs[i].iSurf == iSurf)
            return;
    if (GCurtainSurfCount >= MAX_FOUNTAIN_FEET)
        return;
    FFountainCurtain& C = GCurtainSurfs[GCurtainSurfCount++];
    C.iSurf = iSurf;
    C.A[0]  = A[0];
    C.A[1]  = A[1];
    C.B[0]  = B[0];
    C.B[1]  = B[1];
    C.Z     = PoolZ;
    C.Rise  = Max(TopZ - PoolZ, 1.0f);
}

static const FFountainPool* PoolOfSurf(INT iSurf)
{
    for (INT p = 0; p < GPoolCount; p++)
        if (GPools[p].iSurf == iSurf)
            return &GPools[p];
    return NULL;
}

// A curtain: the upright nodes of one surface whose texture is named a fountain.
struct FFountainFoot
{
    INT   iSurf;
    FLOAT AcrossX, AcrossY;     // the level's direction along it; its plane's normal is (AcrossY, -AcrossX)
    FLOAT BottomZ, TopZ;
    FLOAT A0, A1;               // how far it reaches along that direction, and its lowest edge by itself
    FLOAT Low0, Low1;
    FLOAT OffSum;               // where its plane lies along its normal, summed over its corners
    INT   Corners;
};

static UBOOL IsFountainSurf(UModel* Model, INT iSurf)
{
    if (iSurf < 0 || iSurf >= Model->Surfs.Num())
        return 0;
    const FBspSurf& Surf = Model->Surfs(iSurf);
    return (Surf.Texture && RdxMath::IsFountainName(Surf.Texture->GetName())) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// The fountains' chutes
// ---------------------------------------------------------------------------

// An ornamental fountain's chute (RenderMath.h, FountainChuteSheet): the lying, see-through pieces of the
// fountain's own surface its water runs down to the slot a curtain falls from. A piece with what it is found by -
// its extent, how low and how high it lies, its plane - and, once it is in a chute, which way its water runs on the
// level, how much it drops for a unit along that, where its lower end is along that way, how long the water takes
// from there to the slot and how much film lies between. The level's own, found with the census: as many as the
// level has.
struct FFountainChute
{
    INT   iSurf;
    FLOAT MinX, MinY, MaxX, MaxY, LowZ, HighZ;
    FLOAT MidX, MidY;
    FLOAT NormalX, NormalY, NormalZ;
    INT   Corners;
    UBOOL bTaken;           // in a chute: the numbers below are its own
    FLOAT WayX, WayY;
    FLOAT Drop;
    FLOAT EndS;
    FLOAT EndTime;          // seconds before the water is at the slot, as a number at most zero
    FLOAT EndWay;           // units of film
    FLOAT Seconds;          // the water's time over the piece
    FLOAT Film;             // the film on it, in units
    FLOAT Run;              // its length along that way, on the level
    FLOAT SlotX, SlotY, SlotZ;  // the middle of the slot its chute ends at: the curtain's top
};
static FFountainChute* GChutes     = NULL;
static INT             GChuteCount = 0;
static INT             GChuteRoom  = 0;

static const FFountainChute* ChuteOfSurf(INT iSurf)
{
    for (INT c = 0; c < GChuteCount; c++)
        if (GChutes[c].iSurf == iSurf)
            return GChutes[c].bTaken ? &GChutes[c] : NULL;
    return NULL;
}

// A piece into a chute, if its water runs towards the place given - the slot, or the piece below: the way it drops,
// or, level, the way the piece below runs (Below). EndTime and EndWay: its lower end's, the piece below's upper.
static UBOOL TakeChutePiece(UModel* Model, FFountainChute& P, FLOAT TowardsX, FLOAT TowardsY, FLOAT BelowX,
                            FLOAT BelowY, FLOAT EndTime, FLOAT EndWay)
{
    const FLOAT Flat = appSqrt(P.NormalX * P.NormalX + P.NormalY * P.NormalY);
    const FLOAT Drop = Flat / Max(Abs(P.NormalZ), 0.01f);
    FLOAT WayX = BelowX, WayY = BelowY, Runs = 0.0f;
    if (Drop >= RdxMath::ChuteLevelDrop)
    {
        // Down the piece: its plane's normal laid on the level, on the side the normal's height says.
        const FLOAT Side = (P.NormalZ > 0.0f) ? 1.0f : -1.0f;
        WayX = Side * P.NormalX / Flat;
        WayY = Side * P.NormalY / Flat;
        Runs = Drop;
    }
    if ((TowardsX - P.MidX) * WayX + (TowardsY - P.MidY) * WayY <= 0.0f)
        return 0;
    FLOAT S0 = 1.0e30f, S1 = -1.0e30f;
    for (INT n = 0; n < Model->Nodes.Num(); n++)
    {
        const FBspNode& Node = Model->Nodes(n);
        if (Node.iSurf != P.iSurf || Node.NumVertices < 3)
            continue;
        for (INT v = 0; v < Node.NumVertices; v++)
        {
            const INT iVert  = Node.iVertPool + v;
            const INT iPoint = (iVert >= 0 && iVert < Model->Verts.Num()) ? Model->Verts(iVert).pVertex : -1;
            if (iPoint < 0 || iPoint >= Model->Points.Num())
                continue;
            const FLOAT S = Model->Points(iPoint).X * WayX + Model->Points(iPoint).Y * WayY;
            S0 = Min(S0, S);
            S1 = Max(S1, S);
        }
    }
    if (S1 < S0)
        return 0;
    P.WayX    = WayX;
    P.WayY    = WayY;
    P.Drop    = Runs;
    P.EndS    = S1;
    P.EndTime = EndTime;
    P.EndWay  = EndWay;
    P.Seconds = RdxMath::ChuteTime(S1 - S0, Runs);
    P.Film    = RdxMath::ChuteLength(S1 - S0, Runs);
    P.Run     = S1 - S0;
    P.bTaken  = 1;
    return 1;
}

// The chutes, from the curtains (FindFountainPools' feet): each curtain's first piece is the one that ends at its
// top; then the pieces that carry a chute on upwards, one after another.
static void FindFountainChutes(UModel* Model, const FFountainFoot* Feet, INT FootCount)
{
    GChuteCount = 0;
    for (INT n = 0; n < Model->Nodes.Num(); n++)
    {
        const FBspNode& Node = Model->Nodes(n);
        if (Node.NumVertices < 3 || Abs(Node.Plane.Z) <= 0.5f || !IsFountainSurf(Model, Node.iSurf) ||
            !NodeSeeThrough(Model, Node))
            continue;
        INT c = 0;
        while (c < GChuteCount && GChutes[c].iSurf != Node.iSurf)
            c++;
        if (c == GChuteCount)
        {
            if (GChuteCount >= GChuteRoom)
            {
                const INT Room = Max(GChuteRoom * 2, 16);
                FFountainChute* const More = (FFountainChute*)appRealloc(GChutes, Room * sizeof(FFountainChute),
                                                                         TEXT("RdxFountainChutes"));
                if (!More)
                    return;
                GChutes    = More;
                GChuteRoom = Room;
            }
            FFountainChute& New = GChutes[GChuteCount++];
            appMemzero(&New, sizeof(New));
            New.iSurf   = Node.iSurf;
            New.MinX    = New.MinY = New.LowZ = 1.0e30f;
            New.MaxX    = New.MaxY = New.HighZ = -1.0e30f;
            New.NormalX = Node.Plane.X;
            New.NormalY = Node.Plane.Y;
            New.NormalZ = Node.Plane.Z;
        }
        FFountainChute& P = GChutes[c];
        for (INT v = 0; v < Node.NumVertices; v++)
        {
            const INT iVert  = Node.iVertPool + v;
            const INT iPoint = (iVert >= 0 && iVert < Model->Verts.Num()) ? Model->Verts(iVert).pVertex : -1;
            if (iPoint < 0 || iPoint >= Model->Points.Num())
                continue;
            const FVector& Q = Model->Points(iPoint);
            P.MinX  = Min(P.MinX, Q.X);
            P.MaxX  = Max(P.MaxX, Q.X);
            P.MinY  = Min(P.MinY, Q.Y);
            P.MaxY  = Max(P.MaxY, Q.Y);
            P.LowZ  = Min(P.LowZ, Q.Z);
            P.HighZ = Max(P.HighZ, Q.Z);
            P.MidX += Q.X;
            P.MidY += Q.Y;
            P.Corners++;
        }
    }
    for (INT c = 0; c < GChuteCount; c++)
    {
        GChutes[c].MidX /= (FLOAT)Max(GChutes[c].Corners, 1);
        GChutes[c].MidY /= (FLOAT)Max(GChutes[c].Corners, 1);
    }
    if (!GChuteCount)
        return;

    // The first pieces: at a curtain's top. Its water leaves off the curtain's plane, on the side away from the
    // piece.
    INT Chutes = 0;
    for (INT f = 0; f < FootCount; f++)
    {
        const FFountainFoot& Ft = Feet[f];
        const FLOAT Off  = Ft.OffSum / (FLOAT)Max(Ft.Corners, 1);
        const FLOAT Half = 0.5f * (Ft.A0 + Ft.A1);
        const FLOAT TopX = Ft.AcrossY * Off + Ft.AcrossX * Half;
        const FLOAT TopY = -Ft.AcrossX * Off + Ft.AcrossY * Half;
        for (INT c = 0; c < GChuteCount; c++)
        {
            FFountainChute& P = GChutes[c];
            if (P.bTaken || !P.Corners ||
                !RdxMath::ChuteStartsAt(P.LowZ, P.MinX, P.MinY, P.MaxX, P.MaxY, Ft.TopZ, TopX, TopY))
                continue;
            const FLOAT Side = ((TopX - P.MidX) * Ft.AcrossY - (TopY - P.MidY) * Ft.AcrossX < 0.0f) ? -1.0f : 1.0f;
            if (!TakeChutePiece(Model, P, TopX, TopY, Side * Ft.AcrossY, -Side * Ft.AcrossX, 0.0f, 0.0f))
                continue;
            P.SlotX = TopX;
            P.SlotY = TopY;
            P.SlotZ = Ft.TopZ;
            Chutes++;
        }
    }
    // And on upwards from every piece taken, until none is added.
    for (UBOOL bGrew = (Chutes > 0) ? 1 : 0; bGrew;)
    {
        bGrew = 0;
        for (INT b = 0; b < GChuteCount; b++)
        {
            if (!GChutes[b].bTaken)
                continue;
            for (INT c = 0; c < GChuteCount; c++)
            {
                FFountainChute&       P = GChutes[c];
                const FFountainChute& B = GChutes[b];
                if (P.bTaken || !P.Corners ||
                    !RdxMath::ChuteCarriesOn(P.LowZ, P.MinX, P.MinY, P.MaxX, P.MaxY, B.HighZ, B.MinX, B.MinY, B.MaxX,
                                             B.MaxY))
                    continue;
                if (TakeChutePiece(Model, P, B.MidX, B.MidY, B.WayX, B.WayY, B.EndTime - B.Seconds,
                                   B.EndWay + B.Film))
                {
                    P.SlotX = B.SlotX;
                    P.SlotY = B.SlotY;
                    P.SlotZ = B.SlotZ;
                    bGrew   = 1;
                }
            }
        }
    }
    INT Taken = 0;
    for (INT c = 0; c < GChuteCount; c++)
        if (GChutes[c].bTaken)
            Taken++;
    RDX_LOGF("water census: %i fountain chutes of %i pieces in all; %i lying fountain pieces are in none and stay "
             "as the level draws them", Chutes, Taken, GChuteCount - Taken);
    for (INT c = 0; c < GChuteCount; c++)
    {
        const FFountainChute& P = GChutes[c];
        if (P.bTaken)
            RDX_LOGF("water census:   chute piece %i runs %.2f %.2f, drops %.3f a unit, %.1f units of film in %.2f s; "
                     "its lower end %.2f s and %.1f units before the slot", P.iSurf, P.WayX, P.WayY, P.Drop, P.Film,
                     P.Seconds, -P.EndTime, P.EndWay);
    }
}

static void FindFountainPools(UModel* Model)
{
    GPoolCount        = 0;
    GCurtainSurfCount = 0;
    GChuteCount       = 0;
    FFountainFoot Feet[MAX_FOUNTAIN_FEET];
    INT FootCount = 0, FeetLost = 0, PoolsLost = 0, Apart = 0;
    // The curtains, by surface: how far down and up each reaches first, then its lowest edge.
    for (INT Pass = 0; Pass < 2; Pass++)
        for (INT n = 0; n < Model->Nodes.Num(); n++)
        {
            const FBspNode& Node = Model->Nodes(n);
            if (Node.NumVertices < 3 || Abs(Node.Plane.Z) > 0.5f || !IsFountainSurf(Model, Node.iSurf))
                continue;
            INT f = 0;
            while (f < FootCount && Feet[f].iSurf != Node.iSurf)
                f++;
            if (f == FootCount)
            {
                if (Pass > 0)
                    continue;
                if (FootCount >= MAX_FOUNTAIN_FEET)
                {
                    FeetLost++;
                    continue;
                }
                const FLOAT L = appSqrt(Node.Plane.X * Node.Plane.X + Node.Plane.Y * Node.Plane.Y);
                FFountainFoot& New = Feet[FootCount++];
                New.iSurf   = Node.iSurf;
                New.AcrossX = -Node.Plane.Y / L;
                New.AcrossY = Node.Plane.X / L;
                New.BottomZ = 1.0e30f;
                New.TopZ    = -1.0e30f;
                New.A0      = New.Low0 = 1.0e30f;
                New.A1      = New.Low1 = -1.0e30f;
                New.OffSum  = 0.0f;
                New.Corners = 0;
            }
            FFountainFoot& F = Feet[f];
            for (INT v = 0; v < Node.NumVertices; v++)
            {
                const INT iVert  = Node.iVertPool + v;
                const INT iPoint = (iVert >= 0 && iVert < Model->Verts.Num()) ? Model->Verts(iVert).pVertex : -1;
                if (iPoint < 0 || iPoint >= Model->Points.Num())
                    continue;
                const FVector& P = Model->Points(iPoint);
                const FLOAT    A = P.X * F.AcrossX + P.Y * F.AcrossY;
                if (Pass == 0)
                {
                    F.BottomZ = Min(F.BottomZ, P.Z);
                    F.TopZ    = Max(F.TopZ, P.Z);
                    F.A0      = Min(F.A0, A);
                    F.A1      = Max(F.A1, A);
                    F.OffSum += P.X * F.AcrossY - P.Y * F.AcrossX;
                    F.Corners++;
                }
                else if (P.Z <= F.BottomZ + 1.0f)
                {
                    F.Low0 = Min(F.Low0, A);
                    F.Low1 = Max(F.Low1, A);
                }
            }
        }
    if (!FootCount)
        return;

    // The level's see-through surfaces that lie level, each with its extent and whether a liquid's border is among
    // its polygons. A fountain's own lying pieces are not pools.
    struct FFlat
    {
        FLOAT MinX, MinY, MaxX, MaxY, ZSum;
        INT   Corners;
        UBOOL bLiquid;
    };
    const INT    NumSurfs = Model->Surfs.Num();
    FFlat* const Flat     = (FFlat*)appMalloc((NumSurfs > 0 ? NumSurfs : 1) * sizeof(FFlat), TEXT("RdxFountainPools"));
    if (!Flat)
        return;
    for (INT s = 0; s < NumSurfs; s++)
    {
        Flat[s].MinX    = Flat[s].MinY = 1.0e30f;
        Flat[s].MaxX    = Flat[s].MaxY = -1.0e30f;
        Flat[s].ZSum    = 0.0f;
        Flat[s].Corners = 0;
        Flat[s].bLiquid = 0;
    }
    for (INT n = 0; n < Model->Nodes.Num(); n++)
    {
        const FBspNode& Node = Model->Nodes(n);
        if (Node.NumVertices < 3 || Node.iSurf < 0 || Node.iSurf >= NumSurfs || Abs(Node.Plane.Z) < 0.99f ||
            !NodeSeeThrough(Model, Node) || IsFountainSurf(Model, Node.iSurf))
            continue;
        FFlat& F = Flat[Node.iSurf];
        INT Wet = 0, Dry = 0;
        if (NodeBorder(Model, n, Wet, Dry))
            F.bLiquid = 1;
        for (INT v = 0; v < Node.NumVertices; v++)
        {
            const INT iVert  = Node.iVertPool + v;
            const INT iPoint = (iVert >= 0 && iVert < Model->Verts.Num()) ? Model->Verts(iVert).pVertex : -1;
            if (iPoint < 0 || iPoint >= Model->Points.Num())
                continue;
            const FVector& P = Model->Points(iPoint);
            F.MinX = Min(F.MinX, P.X);
            F.MaxX = Max(F.MaxX, P.X);
            F.MinY = Min(F.MinY, P.Y);
            F.MaxY = Max(F.MaxY, P.Y);
            F.ZSum += P.Z;
            F.Corners++;
        }
    }

    // Under a foot: the pool. Two curtains side by side - a fountain is often built of two - are one foot, halfway
    // between them; a curtain that comes down elsewhere into the same pool is counted and leaves the first its own.
    for (INT s = 0; s < NumSurfs; s++)
    {
        const FFlat& F = Flat[s];
        if (!F.Corners)
            continue;
        const FLOAT Z = F.ZSum / (FLOAT)F.Corners;
        for (INT f = 0; f < FootCount; f++)
        {
            const FFountainFoot& Ft = Feet[f];
            const FLOAT Off  = Ft.OffSum / (FLOAT)Max(Ft.Corners, 1);
            const FLOAT E0   = (Ft.Low1 - Ft.Low0 >= 1.0f) ? Ft.Low0 : Ft.A0;
            const FLOAT E1   = (Ft.Low1 - Ft.Low0 >= 1.0f) ? Ft.Low1 : Ft.A1;
            FLOAT       A[2] = { Ft.AcrossY * Off + Ft.AcrossX * E0, -Ft.AcrossX * Off + Ft.AcrossY * E0 };
            FLOAT       B[2] = { Ft.AcrossY * Off + Ft.AcrossX * E1, -Ft.AcrossX * Off + Ft.AcrossY * E1 };
            if (!RdxMath::FountainFootInPool(0.5f * (A[0] + B[0]), 0.5f * (A[1] + B[1]), Ft.BottomZ, F.MinX, F.MinY,
                                             F.MaxX, F.MaxY, Z))
                continue;
            FFountainPool* Pool = const_cast<FFountainPool*>(PoolOfSurf(s));
            if (!Pool)
            {
                if (GPoolCount >= MAX_FOUNTAIN_POOLS)
                {
                    PoolsLost++;
                    break;
                }
                Pool = &GPools[GPoolCount];
                Pool->iSurf    = s;
                Pool->MinX     = F.MinX;
                Pool->MinY     = F.MinY;
                Pool->MaxX     = F.MaxX;
                Pool->MaxY     = F.MaxY;
                Pool->Z        = Z;
                Pool->FootA[0] = A[0];
                Pool->FootA[1] = A[1];
                Pool->FootB[0] = B[0];
                Pool->FootB[1] = B[1];
                Pool->Rise     = Ft.TopZ - Ft.BottomZ;
                Pool->Spread   = 0.0f;
                Pool->Across   = Min(F.MaxX - F.MinX, F.MaxY - F.MinY);
                Pool->Feet     = 1;
                Pool->First    = GPoolCount++;
                Pool->bLiquid  = F.bLiquid;
                AddCurtain(Ft.iSurf, A, B, Z, Ft.TopZ);
                continue;
            }
            // Whatever becomes of its foot below, the curtain itself comes down into this pool.
            AddCurtain(Ft.iSurf, A, B, Z, Ft.TopZ);
            // The same way round as the foot the pool has.
            const FLOAT Same = (B[0] - A[0]) * (Pool->FootB[0] - Pool->FootA[0]) +
                               (B[1] - A[1]) * (Pool->FootB[1] - Pool->FootA[1]);
            if (Same < 0.0f)
                for (INT k = 0; k < 2; k++)
                {
                    const FLOAT Keep = A[k];
                    A[k] = B[k];
                    B[k] = Keep;
                }
            const FLOAT GapA = (A[0] - Pool->FootA[0]) * (A[0] - Pool->FootA[0]) +
                               (A[1] - Pool->FootA[1]) * (A[1] - Pool->FootA[1]);
            const FLOAT GapB = (B[0] - Pool->FootB[0]) * (B[0] - Pool->FootB[0]) +
                               (B[1] - Pool->FootB[1]) * (B[1] - Pool->FootB[1]);
            if (Max(GapA, GapB) > 16.0f * 16.0f)
            {
                Apart++;
                continue;
            }
            const FLOAT Had = (FLOAT)Pool->Feet, All = Had + 1.0f;
            for (INT k = 0; k < 2; k++)
            {
                Pool->FootA[k] = (Pool->FootA[k] * Had + A[k]) / All;
                Pool->FootB[k] = (Pool->FootB[k] * Had + B[k]) / All;
            }
            Pool->Rise   = Max(Pool->Rise, Ft.TopZ - Ft.BottomZ);
            Pool->Spread = Max(Pool->Spread, 0.5f * (FLOAT)appSqrt(Max(GapA, GapB)));
            Pool->Feet++;
        }
    }

    // A basin's surface cut into several: the same texture at the same height, touching - with the same foot. Not a
    // liquid's border: a level's real water goes on as far as it likes, and is water without the fountain.
    for (UBOOL bGrew = (GPoolCount > 0) ? 1 : 0; bGrew;)
    {
        bGrew = 0;
        for (INT s = 0; s < NumSurfs; s++)
        {
            const FFlat& F = Flat[s];
            if (!F.Corners || F.bLiquid || PoolOfSurf(s))
                continue;
            const FLOAT Z = F.ZSum / (FLOAT)F.Corners;
            for (INT p = 0; p < GPoolCount; p++)
            {
                if (Model->Surfs(s).Texture != Model->Surfs(GPools[p].iSurf).Texture ||
                    !RdxMath::FountainPoolCarriesOn(F.MinX, F.MinY, F.MaxX, F.MaxY, Z, GPools[p].MinX, GPools[p].MinY,
                                                    GPools[p].MaxX, GPools[p].MaxY, GPools[p].Z))
                    continue;
                if (GPoolCount >= MAX_FOUNTAIN_POOLS)
                {
                    PoolsLost++;
                    break;
                }
                FFountainPool& New = GPools[GPoolCount++];
                New         = GPools[p];
                New.iSurf   = s;
                New.MinX    = F.MinX;
                New.MinY    = F.MinY;
                New.MaxX    = F.MaxX;
                New.MaxY    = F.MaxY;
                New.Z       = Z;
                New.bLiquid = 0;
                bGrew       = 1;
                break;
            }
        }
    }
    appFree(Flat);

    // How calm a basin's water is goes by the whole of it, not by the piece.
    for (INT p = 0; p < GPoolCount; p++)
    {
        FLOAT MinX = 1.0e30f, MinY = 1.0e30f, MaxX = -1.0e30f, MaxY = -1.0e30f;
        for (INT q = 0; q < GPoolCount; q++)
            if (GPools[q].First == GPools[p].First)
            {
                MinX = Min(MinX, GPools[q].MinX);
                MinY = Min(MinY, GPools[q].MinY);
                MaxX = Max(MaxX, GPools[q].MaxX);
                MaxY = Max(MaxY, GPools[q].MaxY);
            }
        GPools[p].Across = Min(MaxX - MinX, MaxY - MinY);
    }
    FindFountainChutes(Model, Feet, FootCount);
    RDX_LOGF("water census: %i fountain curtains, %i pool surfaces under them", FootCount, GPoolCount);
    for (INT p = 0; p < GPoolCount; p++)
    {
        const FFountainPool& P = GPools[p];
        RDX_LOGF("water census:   pool surface %i at height %.0f, %.0f x %.0f units (the basin %.0f across), %s; the "
                 "foot of %i curtains from %.0f %.0f to %.0f %.0f, %.0f units high", P.iSurf, P.Z, P.MaxX - P.MinX,
                 P.MaxY - P.MinY, P.Across, P.bLiquid ? TEXT("a liquid's border") : TEXT("no liquid's border"),
                 P.Feet, P.FootA[0], P.FootA[1], P.FootB[0], P.FootB[1], P.Rise);
    }
    if (FeetLost || PoolsLost || Apart)
        RDX_LOGF("water census: %i curtains and %i pool surfaces are past what the lists hold, %i curtains come down "
                 "into a pool apart from its first - those stay as the level draws them, or without rings",
                 FeetLost, PoolsLost, Apart);
}

// A node's polygon's area seen from above.
static FLOAT NodeAreaXY(UModel* Model, const FBspNode& Node)
{
    FLOAT Twice = 0.0f;
    for (INT v = 0; v < Node.NumVertices; v++)
    {
        const INT iA = Node.iVertPool + v, iB = Node.iVertPool + (v + 1) % Node.NumVertices;
        if (iA < 0 || iA >= Model->Verts.Num() || iB < 0 || iB >= Model->Verts.Num())
            return 0.0f;
        const INT pA = Model->Verts(iA).pVertex, pB = Model->Verts(iB).pVertex;
        if (pA < 0 || pA >= Model->Points.Num() || pB < 0 || pB >= Model->Points.Num())
            return 0.0f;
        const FVector& A = Model->Points(pA);
        const FVector& B = Model->Points(pB);
        Twice += A.X * B.Y - B.X * A.Y;
    }
    return 0.5f * Abs(Twice);
}

// Each water surface's largest piece - of the level's surfaces it is made of, the one most of it lies on - and the
// pieces that are drawn as that one lays the texture on (RdxWaterLeadOf): the same texture, drawn alike, its axes and
// base in the model. SurfWater and SurfArea: the census's, by the level's surface.
static void FindLeads(UModel* Model, const INT* SurfWater, const FLOAT* SurfArea)
{
    if (GLeadOfSurf)
        appFree(GLeadOfSurf);
    GLeadOfSurf = NULL;
    GLeadSurfs  = 0;
    GLeadModel  = NULL;
    const INT NumSurfs = Model->Surfs.Num();
    if (!SurfWater || !SurfArea || NumSurfs <= 0)
        return;
    GLeadOfSurf = (INT*)appMalloc(NumSurfs * sizeof(INT), TEXT("RdxWaterLeads"));
    if (!GLeadOfSurf)
        return;
    INT Lead[MAX_WATER_SURFACES];
    for (INT w = 0; w < MAX_WATER_SURFACES; w++)
        Lead[w] = -1;
    for (INT i = 0; i < NumSurfs; i++)
    {
        GLeadOfSurf[i] = -1;
        const INT w = SurfWater[i];
        if (w >= 0 && w < MAX_WATER_SURFACES && (Lead[w] < 0 || SurfArea[i] > SurfArea[Lead[w]]))
            Lead[w] = i;
    }
    const DWORD DrawnAlike = PF_Translucent | PF_Masked | PF_Modulated | PF_Invisible | PF_TwoSided;
    auto Mapped = [&](const FBspSurf& S)
    {
        return S.vTextureU >= 0 && S.vTextureU < Model->Vectors.Num() && S.vTextureV >= 0 &&
               S.vTextureV < Model->Vectors.Num() && S.pBase >= 0 && S.pBase < Model->Points.Num();
    };
    INT Pieces = 0;
    for (INT i = 0; i < NumSurfs; i++)
    {
        const INT w = SurfWater[i];
        if (w < 0 || w >= MAX_WATER_SURFACES || Lead[w] < 0 || Lead[w] == i)
            continue;
        const FBspSurf& Own = Model->Surfs(i);
        const FBspSurf& By  = Model->Surfs(Lead[w]);
        if (!Own.Texture || Own.Texture != By.Texture || ((Own.PolyFlags ^ By.PolyFlags) & DrawnAlike) || !Mapped(Own) ||
            !Mapped(By))
            continue;
        GLeadOfSurf[i] = Lead[w];
        Pieces++;
        RDX_LOGF("water census:   surface %i (%s) is drawn as surface %i lays its texture on - the larger piece of the "
                 "water at height %.0f (%.0f of its units^2 against %.0f)", i, Own.Texture->GetName(), Lead[w],
                 GSurfaces[w].Z, SurfArea[i], SurfArea[Lead[w]]);
    }
    GLeadSurfs = NumSurfs;
    GLeadModel = Model;
    RDX_LOGF("water census: %i pieces of the water surfaces drawn as their water's largest piece lays the texture on",
             Pieces);
}

INT RdxWaterLeadOf(const UModel* Model, INT iSurf)
{
    return (Model && Model == GLeadModel && GLeadOfSurf && Model->Surfs.Num() == GLeadSurfs && iSurf >= 0 &&
            iSurf < GLeadSurfs) ? GLeadOfSurf[iSurf] : -1;
}

static void Census(ULevel* Level)
{
    UModel* Model = Level->Model;
    const TCHAR* LevelName = Level->GetOuter() ? Level->GetOuter()->GetName() : TEXT("?");
    GRowCount = 0;
    GRowsLost = 0;

    // The zones: the whole list is short (at most 64), so every one that is more than air is named.
    const AZoneInfo* ZoneZero = ZoneActor(Model, 0);
    INT Special = 0;
    for (INT z = 0; z < Model->NumZones && z < FBspNode::MAX_ZONES; z++)
    {
        const AZoneInfo* Zone = ZoneActor(Model, z);
        if (Zone && (Zone->bWaterZone || Zone->bPainZone || Zone->bKillZone))
            Special++;
    }
    RDX_LOGF("water census: %s - %i zones, zone 0 is %s; %i of them water, hurting or killing", LevelName,
             Model->NumZones, ZoneZero ? ZoneZero->GetName() : TEXT("no actor"), Special);
    for (INT z = 0; z < Model->NumZones && z < FBspNode::MAX_ZONES; z++)
    {
        const AZoneInfo* Zone = ZoneActor(Model, z);
        if (!Zone || !(Zone->bWaterZone || Zone->bPainZone || Zone->bKillZone))
            continue;
        RDX_LOGF("water census:   zone %i: %s (%s)%s%s%s, damage %i a second (%s), view fog %.2f %.2f %.2f, "
                 "view flash %.2f %.2f %.2f, fluid friction %.2f", z, Zone->GetName(), Zone->GetClass()->GetName(),
                 Zone->bWaterZone ? TEXT(" water") : TEXT(""), Zone->bPainZone ? TEXT(" pain") : TEXT(""),
                 Zone->bKillZone ? TEXT(" kill") : TEXT(""), Zone->DamagePerSec, *Zone->DamageType,
                 Zone->ViewFog.X, Zone->ViewFog.Y, Zone->ViewFog.Z, Zone->ViewFlash.X, Zone->ViewFlash.Y,
                 Zone->ViewFlash.Z, Zone->ZoneFluidFriction);
    }

    // Every node with a liquid on one side: its surface's texture and flags, and the zone on the other side.
    // One mark per surface. The engine's heap, as elsewhere in the renderer: the SDK's import library carries no
    // TArray<BYTE>.
    const INT NumSurfs = Model->Surfs.Num();
    BYTE* OnBorder = (BYTE*)appMalloc(NumSurfs > 0 ? NumSurfs : 1, TEXT("RdxWaterCensus"));
    if (!OnBorder)
        return;
    appMemzero(OnBorder, NumSurfs > 0 ? NumSurfs : 1);
    // Which water surface each of the level's surfaces is a piece of (-1 none, -2 more than one) and how much of it
    // lies there: the largest piece lays the texture on for the rest (RdxWaterLeadOf).
    INT*   SurfWater = (INT*)appMalloc((NumSurfs > 0 ? NumSurfs : 1) * sizeof(INT), TEXT("RdxWaterCensus"));
    FLOAT* SurfArea  = (FLOAT*)appMalloc((NumSurfs > 0 ? NumSurfs : 1) * sizeof(FLOAT), TEXT("RdxWaterCensus"));
    for (INT i = 0; i < NumSurfs && SurfWater && SurfArea; i++)
    {
        SurfWater[i] = -1;
        SurfArea[i]  = 0.0f;
    }
    INT AirNodes = 0, ZeroNodes = 0;
    FBox  ZoneBox[FBspNode::MAX_ZONES];      // all of a zone's borders with other zones, level or not
    for (INT z = 0; z < FBspNode::MAX_ZONES; z++)
    {
        ZoneBox[z]     = FBox(0);
        GZoneAcross[z] = 0.0f;
    }
    GAnyLevelWater = 0;
    GSurfaceCount  = 0;
    INT SurfacesLost = 0;
    GPortalCount      = 0;
    GPortalPointCount = 0;
    GPortalsLost      = 0;
    appMemzero(GZoneClear, sizeof(GZoneClear));
    appMemzero(GBodyClear, sizeof(GBodyClear));
    RdxMath::WaterBodiesBegin(GZoneBody, FBspNode::MAX_ZONES);
    // A new level: the textures capped in the last one are not followed - their list goes, and the count of steps starts
    // over.
    GCappedCount = 0;
    GStepCount   = 0;
    GStepSince   = 0.0;
    GStepTold    = 0;
    for (INT n = 0; n < Model->Nodes.Num(); n++)
    {
        // Two liquids' zones with a node between them touch: one water (RdxMath::WaterBodiesJoin).
        {
            const FBspNode& Between = Model->Nodes(n);
            if (Between.iZone[0] != Between.iZone[1] && ZoneIsLiquid(Model, Between.iZone[0]) &&
                ZoneIsLiquid(Model, Between.iZone[1]))
                RdxMath::WaterBodiesJoin(GZoneBody, FBspNode::MAX_ZONES, Between.iZone[0], Between.iZone[1]);
        }
        INT Wet = 0, Dry = 0;
        if (!NodeBorder(Model, n, Wet, Dry))
            continue;
        // Where the water meets another zone: a piece of its surface where the border lies level with the liquid under
        // it - into the zone's surface at that height, a new one where it has none there.
        if (Dry != 0 && Wet >= 0 && Wet < FBspNode::MAX_ZONES)
        {
            const FBspNode& Node = Model->Nodes(n);
            FLOAT Z = 0.0f;
            INT   s = -1;
            if (NodeSurface(Model, n, Wet, Z))
            {
                s = SurfaceOf(Wet, Z);
                if (s < 0 && GSurfaceCount < MAX_WATER_SURFACES)
                {
                    s = GSurfaceCount++;
                    FWaterSurface& New = GSurfaces[s];
                    New.Zone    = Wet;
                    New.Air     = Dry;
                    New.Z       = Z;
                    New.MinX    = New.MinY = 1.0e30f;
                    New.MaxX    = New.MaxY = -1.0e30f;
                    New.ZSum    = 0.0f;
                    New.Corners = 0;
                    New.Clear   = 0;
                }
                else if (s < 0)
                    SurfacesLost++;
            }
            for (INT v = 0; v < Node.NumVertices; v++)
            {
                const INT iVert = Node.iVertPool + v;
                if (iVert < 0 || iVert >= Model->Verts.Num())
                    break;
                const INT iPoint = Model->Verts(iVert).pVertex;
                if (iPoint < 0 || iPoint >= Model->Points.Num())
                    continue;
                const FVector& P = Model->Points(iPoint);
                ZoneBox[Wet] += P;
                if (s >= 0)
                {
                    FWaterSurface& S = GSurfaces[s];
                    S.MinX = Min(S.MinX, P.X); S.MaxX = Max(S.MaxX, P.X);
                    S.MinY = Min(S.MinY, P.Y); S.MaxY = Max(S.MaxY, P.Y);
                    S.ZSum += P.Z;
                    S.Corners++;
                }
            }
            if (s >= 0 && GSurfaces[s].Corners > 0)
            {
                GSurfaces[s].Air = Dry;
                GSurfaces[s].Z   = GSurfaces[s].ZSum / (FLOAT)GSurfaces[s].Corners;
            }
            if (s >= 0 && SurfWater && SurfArea && Node.iSurf >= 0 && Node.iSurf < NumSurfs)
            {
                SurfWater[Node.iSurf] = (SurfWater[Node.iSurf] == -1 || SurfWater[Node.iSurf] == s) ? s : -2;
                SurfArea[Node.iSurf] += NodeAreaXY(Model, Node);
            }
            // And where the level draws it see-through, a border of the caustics' mask - level or not.
            if (NodeSeeThrough(Model, Node))
            {
                if (s >= 0)
                    GSurfaces[s].Clear++;
                AddPortal(Model, Node, Wet);
            }
        }
        const INT iSurf = Model->Nodes(n).iSurf;
        if (iSurf < 0 || iSurf >= Model->Surfs.Num())
            continue;
        const FBspSurf& Surf = Model->Surfs(iSurf);
        OnBorder[iSurf] = 1;
        const INT Kind = (Dry == 0) ? Row_ZoneZero : Row_Air;
        if (Kind == Row_Air)
            AirNodes++;
        else
            ZeroNodes++;
        FLOAT TileU = 0.0f, TileV = 0.0f;
        if (Kind == Row_Air)
            TileSize(Model, Surf, TileU, TileV);
        AddRow(Surf.Texture, Surf.PolyFlags & ~EditorFlags, Kind, Wet, Dry, TileU, TileV);
    }
    // And the surfaces on no border that still look like a liquid.
    INT LookSurfs = 0;
    for (INT s = 0; s < Model->Surfs.Num(); s++)
    {
        if (OnBorder[s])
            continue;
        const FBspSurf& Surf = Model->Surfs(s);
        const INT Hints = LiquidHints(Surf.Texture, Surf.PolyFlags);
        if (!Hints)
            continue;
        LookSurfs++;
        AddRow(Surf.Texture, Surf.PolyFlags & ~EditorFlags, Row_Look, Hints, 0);
    }
    appFree(OnBorder);
    FindLeads(Model, SurfWater, SurfArea);
    if (SurfWater)
        appFree(SurfWater);
    if (SurfArea)
        appFree(SurfArea);

    GAnyLevelWater = (GSurfaceCount > 0) ? 1 : 0;
    for (INT z = 0; z < FBspNode::MAX_ZONES; z++)
        if (ZoneBox[z].IsValid)
        {
            // How calm the zone's waves are: by its largest surface - a long, narrow pool is as calm as its width.
            const FVector Size = ZoneBox[z].Max - ZoneBox[z].Min;
            FLOAT Largest = -1.0f;
            INT   Has     = 0;
            GZoneAcross[z] = Min(Size.X, Size.Y);
            for (INT s = 0; s < GSurfaceCount; s++)
            {
                const FWaterSurface& S = GSurfaces[s];
                if (S.Zone != z)
                    continue;
                Has++;
                const FLOAT Wide = S.MaxX - S.MinX, Long = S.MaxY - S.MinY;
                if (Wide * Long > Largest)
                {
                    Largest        = Wide * Long;
                    GZoneAcross[z] = Min(Wide, Long);
                }
                RDX_LOGF("water census:   zone %i has a level surface at height %.0f, %.0f x %.0f units across - waves "
                         "of strength %.3f, ripples on", z, S.Z, Wide, Long,
                         RdxMath::WaterWaveStrength(Min(Wide, Long)));
            }
            if (!Has)
                RDX_LOGF("water census:   zone %i has no level surface - its borders with other zones (%.0f x %.0f "
                         "units across) stand or lean; no ripples", z, Size.X, Size.Y);
        }
    // The waters that have a see-through border; and a water of several zones said once, at the least of them.
    for (INT z = 0; z < FBspNode::MAX_ZONES; z++)
        if (GZoneClear[z])
            GBodyClear[WaterOf(z)] = 1;
    for (INT z = 0; z < FBspNode::MAX_ZONES; z++)
    {
        TCHAR Others[160] = TEXT("");
        INT   More        = 0;
        for (INT o = z + 1; o < FBspNode::MAX_ZONES; o++)
            if (WaterOf(o) == z)
            {
                TCHAR One[16];
                appSprintf(One, TEXT("%s%i"), More ? TEXT(", ") : TEXT(""), o);
                Append(Others, 160, One);
                More++;
            }
        if (More)
            RDX_LOGF("water census:   zones %i, %s touch under the water - one water to the caustics", z, Others);
    }
    if (SurfacesLost)
        RDX_LOGF("water census: %i level borders are of surfaces past the %i the list holds - no ripples on those",
                 SurfacesLost, (INT)MAX_WATER_SURFACES);
    RDX_LOGF("water census: %i see-through borders of liquids, %i corners - what the caustics' mask is drawn from",
             GPortalCount, GPortalPointCount);
    if (GPortalsLost)
        RDX_LOGF("water census: %i see-through borders are past what the list holds - no caustics behind those",
                 GPortalsLost);
    FindSea(Model);
    FindFountainPools(Model);
    RDX_LOGF("water census: %i nodes between a liquid and another zone:", AirNodes);
    PrintRows(Row_Air, MAX_LISTED);
    // The water surfaces' textures, each once.
    for (INT r = 0; r < GRowCount; r++)
    {
        if (GRows[r].Kind != Row_Air || !GRows[r].Texture)
            continue;
        UBOOL bSeen = 0;
        for (INT q = 0; q < r && !bSeen; q++)
            bSeen = (GRows[q].Kind == Row_Air && GRows[q].Texture == GRows[r].Texture);
        if (!bSeen)
            DumpTextureSettings(GRows[r].Texture);
    }
    RDX_LOGF("water census: %i nodes between a liquid and zone 0:", ZeroNodes);
    PrintRows(Row_ZoneZero, 4);
    RDX_LOGF("water census: %i surfaces on no liquid's border that look like one:", LookSurfs);
    PrintRows(Row_Look, MAX_LISTED);
    if (GRowsLost)
        RDX_LOGF("water census: %i more kinds did not fit in the table", GRowsLost);
}

// ---------------------------------------------------------------------------
// What is drawn
// ---------------------------------------------------------------------------

struct FDrawnRow
{
    UTexture* Texture;      // compared only - the name below was taken while the texture was sure to live
    DWORD     Flags;
    INT       Wet;
    INT       Dry;
    INT       Facets;
    INT       Snap;         // the slot of the frame kept for the motion measurement; -1 none yet, -2 measured
    DOUBLE    SnapTime;
    TCHAR     Name[160];
};

static FDrawnRow GDrawn[MAX_DRAWN];

// The motion: one frame of a water texture kept, and a second later the mean change of every texel's brightness.
// That is how strongly the waves move on the screen, whatever the texture's settings say.
enum { MAX_SNAPS = 4, SNAP_TEXELS = 256 * 256 };
static BYTE  GSnap[MAX_SNAPS][SNAP_TEXELS];
static FLOAT GSnapLum[MAX_SNAPS][256];
static INT   GSnapCount = 0;

static FLOAT PaletteLum(const FColor& C)
{
    return 0.299f * C.R + 0.587f * C.G + 0.114f * C.B;
}

static UBOOL SnapFits(const FTextureInfo& Info)
{
    return Info.Format == TEXF_P8 && Info.Palette && Info.NumMips > 0 && Info.Mips[0] && Info.Mips[0]->DataPtr &&
           Info.Mips[0]->USize * Info.Mips[0]->VSize <= SNAP_TEXELS;
}

// A measurement only (RdxWaterDumpFields): the procedural water classes keep their wave field in native memory the SDK
// does not describe (the SourceFields property holds its address). Written to a file twice, a second apart, to learn its
// layout - read under SEH, so a wrong guess about its size cannot bring the game down.
static UBOOL GFieldsDump = 0;
static INT   GFieldsShots = 0;

void RdxWaterDumpFields(UBOOL bOn)
{
    GFieldsDump = bOn ? 1 : 0;
}

static INT CopyGuarded(const BYTE* Src, BYTE* Dst, INT Max)
{
    INT Got = 0;
    __try
    {
        for (; Got < Max; Got++)
            Dst[Got] = Src[Got];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return Got;
}

static void DumpFields(UTexture* T)
{
    if (!GFieldsDump || !T || GFieldsShots >= 2)
        return;
    BYTE* Field = NULL;
    for (TFieldIterator<UProperty> It(T->GetClass()); It; ++It)
        if (!appStricmp(It->GetName(), TEXT("SourceFields")))
            Field = *(BYTE**)((BYTE*)T + It->Offset);
    if (!Field)
        return;
    static BYTE Copy[131072];
    const INT Got = CopyGuarded(Field, Copy, 131072);
    char Name[64];
    sprintf(Name, "SecondRevision-water-fields-%d.bin", GFieldsShots);
    FILE* F = fopen(Name, "wb");
    if (F)
    {
        fwrite(Copy, 1, Got, F);
        fclose(F);
    }
    RDX_LOGF("water (marker): %s's wave field at %08X - %i bytes readable, written to %s", T->GetName(), (INT)(size_t)Field,
             Got, appFromAnsi(Name));
    GFieldsShots++;
}

static void TakeSnap(FDrawnRow& R, const FTextureInfo& Info)
{
    if (GSnapCount == MAX_SNAPS || !SnapFits(Info))
        return;
    R.Snap     = GSnapCount++;
    R.SnapTime = appSeconds();
    appMemcpy(GSnap[R.Snap], Info.Mips[0]->DataPtr, Info.Mips[0]->USize * Info.Mips[0]->VSize);
    for (INT c = 0; c < 256; c++)
        GSnapLum[R.Snap][c] = PaletteLum(Info.Palette[c]);
}

static void MeasureMotion(FDrawnRow& R, const FTextureInfo& Info)
{
    const DOUBLE Gap = appSeconds() - R.SnapTime;
    if (Gap < 1.0 || !SnapFits(Info))
        return;
    const INT    N   = Info.Mips[0]->USize * Info.Mips[0]->VSize;
    const BYTE*  Now = Info.Mips[0]->DataPtr;
    DOUBLE Change = 0.0;
    INT    Moved  = 0;
    for (INT t = 0; t < N; t++)
    {
        const FLOAT D = Abs(PaletteLum(Info.Palette[Now[t]]) - GSnapLum[R.Snap][GSnap[R.Snap][t]]);
        Change += D;
        if (D >= 2.0f)
            Moved++;
    }
    RDX_LOGF("water drawn:   %s motion - over %.2f s the texels changed by %.2f on average (0-255), %i%% of them by "
             "2 or more", R.Name, Gap, Change / N, (INT)(100.0 * Moved / N + 0.5));
    R.Snap = -2;
    DumpFields(R.Texture);
}
static INT       GDrawnRows    = 0;
static INT       GDrawnMore    = 0;
static INT       GDrawnFacets  = 0;
static INT       GNodeChecked  = 0;
static INT       GNodeMatched  = 0;
static INT       GNodeOutside  = 0;

static ULevel*   GLevel        = NULL;          // compared only, never followed: the level may be gone
static FName     GLevelName    = NAME_None;
static UBOOL     GForgotten    = 1;

// The drawn texture is the surface's own, or a frame of its animation (the engine draws the current frame).
static UBOOL SameAnimation(UTexture* Own, UTexture* Drawn)
{
    UTexture* T = Own;
    for (INT k = 0; T && k < 64; k++)
    {
        if (T == Drawn)
            return 1;
        T = T->AnimNext;
        if (T == Own)
            break;
    }
    return 0;
}

static void ReportDrawn()
{
    if (GNodeChecked && GNodeChecked < NODE_CHECKS)
        RDX_LOGF("water: node check - %i of %i drawn BSP polygons name a node whose surface carries the drawn texture "
                 "(%i name no node at all)", GNodeMatched, GNodeChecked, GNodeOutside);
    if (!GDrawnFacets)
        return;
    RDX_LOGF("water drawn in this level: %i BSP facets on a liquid's border, %i kinds%s", GDrawnFacets, GDrawnRows,
             GDrawnMore ? TEXT(" and more") : TEXT(""));
    for (INT r = 0; r < GDrawnRows; r++)
        RDX_LOGF("water drawn:   %s - %i facets, liquid zone %i, other side zone %i", GDrawn[r].Name,
                 GDrawn[r].Facets, GDrawn[r].Wet, GDrawn[r].Dry);
}

static void ResetDrawn()
{
    GSnapCount   = 0;
    GDrawnRows   = 0;
    GDrawnMore   = 0;
    GDrawnFacets = 0;
    GNodeChecked = 0;
    GNodeMatched = 0;
    GNodeOutside = 0;
}

// How strongly a frame of the texture shows through the translucent blend: its mean brightness and the spread around
// it (luminance 0-255), and the mean step between neighbouring texels - the fine detail the waves are drawn with.
// Only for palettized textures, which is what the engine's procedural ones are.
static UBOOL PatternStats(const FTextureInfo& Info, FLOAT& Mean, FLOAT& Spread, FLOAT& Step)
{
    if (Info.Format != TEXF_P8 || !Info.Palette || Info.NumMips < 1 || !Info.Mips[0] || !Info.Mips[0]->DataPtr)
        return 0;
    const FMipmapBase* M = Info.Mips[0];
    const INT W = M->USize, H = M->VSize;
    if (W < 2 || H < 1 || W > 4096 || H > 4096)
        return 0;
    DOUBLE Sum = 0.0, Sum2 = 0.0, Steps = 0.0;
    for (INT y = 0; y < H; y++)
    {
        const BYTE* Row = M->DataPtr + y * W;
        FLOAT Prev = 0.0f;
        for (INT x = 0; x < W; x++)
        {
            const FLOAT L = PaletteLum(Info.Palette[Row[x]]);
            Sum  += L;
            Sum2 += L * L;
            if (x)
                Steps += Abs(L - Prev);
            Prev = L;
        }
    }
    const DOUBLE N = (DOUBLE)W * H;
    Mean   = (FLOAT)(Sum / N);
    Spread = (FLOAT)appSqrt(Max(0.0, Sum2 / N - (Sum / N) * (Sum / N)));
    Step   = (FLOAT)(Steps / ((DOUBLE)(W - 1) * H));
    return 1;
}

// Counted in the table of drawn border kinds, and logged the first time a kind turns up in a level.
static void NoteBorderDrawn(const FTextureInfo* Info, UTexture* Drawn, DWORD Flags, INT Wet, INT Dry)
{
    GDrawnFacets++;
    for (INT r = 0; r < GDrawnRows; r++)
        if (GDrawn[r].Texture == Drawn && GDrawn[r].Flags == Flags && GDrawn[r].Wet == Wet && GDrawn[r].Dry == Dry)
        {
            // The pattern a few seconds in, not at the first draw: a procedural texture starts from a still surface.
            FDrawnRow& R = GDrawn[r];
            FLOAT Mean = 0.0f, Spread = 0.0f, Step = 0.0f;
            if (++R.Facets == 300 && Dry != 0 && Info && PatternStats(*Info, Mean, Spread, Step))
            {
                RDX_LOGF("water drawn:   %s as drawn - brightness %.1f, spread %.1f, neighbour step %.1f (0-255); "
                         "texture scale %.2f x %.2f", R.Name, Mean, Spread, Step, Info->UScale, Info->VScale);
                TakeSnap(R, *Info);
                DumpFields(Drawn);
            }
            else if (R.Snap >= 0 && Info)
                MeasureMotion(R, *Info);
            return;
        }
    if (GDrawnRows == MAX_DRAWN)
    {
        GDrawnMore++;
        return;
    }
    FDrawnRow& R = GDrawn[GDrawnRows++];
    R.Texture = Drawn;
    R.Flags   = Flags;
    R.Wet     = Wet;
    R.Dry     = Dry;
    R.Facets  = 1;
    R.Snap    = -1;
    R.SnapTime = 0.0;
    TextureText(Drawn, R.Name, 160);
    TCHAR Names[256];
    FlagNames(Flags, Names, 256);
    RDX_LOGF("water drawn: %s - %s (0x%08X), liquid zone %i, other side zone %i - the first of its kind in this level",
             R.Name, Names, Flags, Wet, Dry);
}

// ---------------------------------------------------------------------------
// The drawing order: is what lies behind a see-through water surface already in the picture when the surface is
// drawn? A refraction reads the picture as it is at that moment, so anything drawn later would be missing from it.
// Only the main view counts (a mirror or the sky's view has coordinates of its own). It measures for the first frames
// of a level that show see-through water (ORDER_REPORT_AT), reports, and then stays out of the way until the next
// level: every draw of those frames is tested against every water surface, which is not free.
// ---------------------------------------------------------------------------

enum { MAX_SHEETS = 32, MAX_LATE = 8, MAX_SHAPE_POINTS = 64, ORDER_REPORT_AT = 240 };
enum { Shape_Level = 0, Shape_Mesh = 1, Shape_Sprite = 2, Shape_Kinds = 3 };

struct FScreenRect
{
    FLOAT MinX, MinY, MaxX, MaxY;
};

// A see-through water surface as this frame drew it: its plane, and the part of the screen it covers.
struct FSheet
{
    FLOAT       CamDist;        // the camera's signed distance to the plane
    FVector     M;              // the plane's normal in view space: a view-space point P lies at CamDist + (P | M)
    FScreenRect Rect;
};

struct FLateRow
{
    UTexture* Texture;          // compared only, like FDrawnRow
    INT       Kind;
    INT       Count;
    TCHAR     Name[160];
};

static FSheet   GSheets[MAX_SHEETS];         // this frame's, in the order drawn
static INT      GSheetCount       = 0;
static FSheet   GPrevSheets[MAX_SHEETS];     // the frame before's: the camera barely moves from one frame to the next
static INT      GPrevSheetCount   = 0;
static INT      GFrameDraws       = 0;       // draws of the main view in this frame
static INT      GFirstSheetDraw   = -1;      // the draw at which this frame's first sheet came
static INT      GLastFirstSheet   = -1;      // the same, and the draw count, of the last frame that had a sheet
static INT      GLastDraws        = 0;
static INT      GWaterFrames      = 0;       // frames of this level with a sheet in them
static INT      GEarly[Shape_Kinds];         // behind a sheet and drawn before it: in the picture in time
static INT      GLate[Shape_Kinds];          // behind a sheet and drawn after it: missing from a refraction
static INT      GBetween[Shape_Kinds];       // early, but after the frame's FIRST sheet: missing from a copy taken there
static FLateRow GLateRows[MAX_LATE];
static INT      GLateRowCount     = 0;
static UBOOL    GOrderReported    = 0;

static const TCHAR* ShapeName(INT Kind)
{
    return (Kind == Shape_Level) ? TEXT("level") : ((Kind == Shape_Mesh) ? TEXT("mesh") : TEXT("sprite"));
}

// Where view-space points land on the screen (FTransform::Project). A point at or behind the camera could land
// anywhere, so the rectangle is then the whole plane.
static void ScreenRect(const FSceneNode* Frame, const FVector* P, INT Num, FScreenRect& R)
{
    R.MinX = R.MinY = 1e9f;
    R.MaxX = R.MaxY = -1e9f;
    for (INT i = 0; i < Num; i++)
    {
        if (P[i].Z < 1.0f)
        {
            R.MinX = R.MinY = -1e9f;
            R.MaxX = R.MaxY = 1e9f;
            return;
        }
        const FLOAT RZ = Frame->Proj.Z / P[i].Z;
        const FLOAT X  = P[i].X * RZ + Frame->FX15;
        const FLOAT Y  = P[i].Y * RZ + Frame->FY15;
        R.MinX = Min(R.MinX, X);
        R.MaxX = Max(R.MaxX, X);
        R.MinY = Min(R.MinY, Y);
        R.MaxY = Max(R.MaxY, Y);
    }
}

// Behind the sheet: every point on its far side as the camera sees it - by more than a unit, because what lies on
// the surface itself is not behind it - and overlapping it on the screen.
static UBOOL BehindSheet(const FSheet& S, const FVector* P, INT Num, const FScreenRect& R)
{
    if (R.MaxX < S.Rect.MinX || R.MinX > S.Rect.MaxX || R.MaxY < S.Rect.MinY || R.MinY > S.Rect.MaxY)
        return 0;
    const UBOOL bCamAbove = (S.CamDist >= 0.0f);
    for (INT i = 0; i < Num; i++)
    {
        const FLOAT D = S.CamDist + (P[i] | S.M);
        if (bCamAbove ? (D > -1.0f) : (D < 1.0f))
            return 0;
    }
    return 1;
}

static void NoteLate(INT Kind, UTexture* Texture)
{
    GLate[Kind]++;
    for (INT r = 0; r < GLateRowCount; r++)
        if (GLateRows[r].Texture == Texture && GLateRows[r].Kind == Kind)
        {
            GLateRows[r].Count++;
            return;
        }
    if (GLateRowCount == MAX_LATE)
        return;
    FLateRow& R = GLateRows[GLateRowCount++];
    R.Texture = Texture;
    R.Kind    = Kind;
    R.Count   = 1;
    TextureText(Texture, R.Name, 160);
}

// One draw of the main view that is not a sheet: behind one drawn already (late), or behind one of the frame
// before that this frame has not reached yet (early).
static void NoteShape(const FSceneNode* Frame, const FVector* P, INT Num, INT Kind, UTexture* Texture)
{
    GFrameDraws++;
    if (Num <= 0)
        return;
    FScreenRect R;
    ScreenRect(Frame, P, Num, R);
    for (INT s = 0; s < GSheetCount; s++)
        if (BehindSheet(GSheets[s], P, Num, R))
        {
            NoteLate(Kind, Texture);
            return;
        }
    for (INT s = 0; s < GPrevSheetCount; s++)
        if (BehindSheet(GPrevSheets[s], P, Num, R))
        {
            GEarly[Kind]++;
            if (GSheetCount > 0)
                GBetween[Kind]++;
            return;
        }
}

static void AddSheet(const FSceneNode* Frame, const FPlane& Plane, const FVector* P, INT Num)
{
    if (GFirstSheetDraw < 0)
        GFirstSheetDraw = GFrameDraws;
    GFrameDraws++;
    if (GSheetCount == MAX_SHEETS)
        return;
    FSheet& S = GSheets[GSheetCount++];
    const FVector N(Plane.X, Plane.Y, Plane.Z);
    const FCoords& C = Frame->Coords;
    S.CamDist = Plane.PlaneDot(C.Origin);
    S.M       = FVector(N | C.XAxis, N | C.YAxis, N | C.ZAxis);
    ScreenRect(Frame, P, Num, S.Rect);
}

static void ReportOrder()
{
    GOrderReported = 1;
    if (!GWaterFrames)
        return;
    RDX_LOGF("water order: %i frames with see-through water; in the last one its first surface was draw %i of %i",
             GWaterFrames, GLastFirstSheet + 1, GLastDraws);
    RDX_LOGF("water order: behind it and drawn BEFORE it (in the picture in time) - level %i, meshes %i, sprites %i",
             GEarly[Shape_Level], GEarly[Shape_Mesh], GEarly[Shape_Sprite]);
    RDX_LOGF("water order: behind it and drawn AFTER it (missing from a refraction) - level %i, meshes %i, sprites %i",
             GLate[Shape_Level], GLate[Shape_Mesh], GLate[Shape_Sprite]);
    RDX_LOGF("water order: of the BEFORE ones, drawn after the frame's first water surface (missing from one copy taken "
             "there) - level %i, meshes %i, sprites %i", GBetween[Shape_Level], GBetween[Shape_Mesh],
             GBetween[Shape_Sprite]);
    for (INT r = 0; r < GLateRowCount; r++)
        RDX_LOGF("water order:   late: %s, %s - %i draws", GLateRows[r].Name, ShapeName(GLateRows[r].Kind),
                 GLateRows[r].Count);
}

static void ResetOrder()
{
    GSheetCount     = 0;
    GPrevSheetCount = 0;
    GFrameDraws     = 0;
    GFirstSheetDraw = -1;
    GLastFirstSheet = -1;
    GLastDraws      = 0;
    GWaterFrames    = 0;
    GLateRowCount   = 0;
    GOrderReported  = 0;
    for (INT k = 0; k < Shape_Kinds; k++)
        GEarly[k] = GLate[k] = GBetween[k] = 0;
}

void RdxWaterFrameStart()
{
    GWavesTried = 0;
    if (GSheetCount)
    {
        GWaterFrames++;
        GLastFirstSheet = GFirstSheetDraw;
        GLastDraws      = GFrameDraws;
    }
    for (INT s = 0; s < GSheetCount; s++)
        GPrevSheets[s] = GSheets[s];
    GPrevSheetCount = GSheetCount;
    GSheetCount     = 0;
    GFrameDraws     = 0;
    GFirstSheetDraw = -1;
    // A few seconds into the level: the load series plays each save for a fixed time and then ends the game, so the
    // report cannot wait for the level's end.
    if (!GOrderReported && GWaterFrames >= ORDER_REPORT_AT)
        ReportOrder();
}

// ---------------------------------------------------------------------------
// The engine's calls
// ---------------------------------------------------------------------------

UBOOL RdxWaterSea(INT& Zone, FLOAT& Height, FLOAT& MinX, FLOAT& MinY, FLOAT& MaxX, FLOAT& MaxY, INT& Sides, INT& Node)
{
    if (GSeaZone < 0 || GSeaZone >= FBspNode::MAX_ZONES || GSeaNode < 0 || GSeaSurface < 0 ||
        GSeaSurface >= GSurfaceCount)
        return 0;
    Zone   = GSeaZone;
    Height = GSeaZ;
    MinX   = GSurfaces[GSeaSurface].MinX;
    MinY   = GSurfaces[GSeaSurface].MinY;
    MaxX   = GSurfaces[GSeaSurface].MaxX;
    MaxY   = GSurfaces[GSeaSurface].MaxY;
    Sides  = GSeaSides;
    Node   = GSeaNode;
    return 1;
}

UBOOL RdxWaterIsSeaEdge(const FSurfaceInfo& Surface, const FSurfaceFacet& Facet)
{
    if (!GSeaEdge || GSeaZone < 0 || !Surface.Level || !Surface.Level->Model || !Facet.Polys)
        return 0;
    UModel* Model = Surface.Level->Model;
    const INT iNode = Facet.Polys->iNode;
    if (iNode < 0 || iNode >= Model->Nodes.Num())
        return 0;
    const INT iSurf = Model->Nodes(iNode).iSurf;
    return (iSurf >= 0 && iSurf < GSeaEdgeSize && GSeaEdge[iSurf]) ? 1 : 0;
}

UBOOL RdxWaterIsSea(const FSurfaceInfo& Surface, const FSurfaceFacet& Facet)
{
    if (GSeaZone < 0 || !Surface.Level || !Surface.Level->Model)
        return 0;
    UModel* Model = Surface.Level->Model;
    for (const FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next)
    {
        // The sea's own surface: not an upright border of its zone, nor a pool of the same zone at another height.
        INT   Wet = 0, Dry = 0;
        FLOAT Z   = 0.0f;
        if (NodeBorder(Model, Poly->iNode, Wet, Dry))
            return (Wet == GSeaZone && Dry != 0 && NodeSurface(Model, Poly->iNode, Wet, Z) &&
                    Abs(Z - GSeaZ) <= RdxMath::WaterSurfaceStep) ? 1 : 0;
    }
    return 0;
}

static void ForgetRippleSources();

void RdxWaterForget()
{
    ForgetSeaEdge();
    // The textures whose waves were capped are not followed past their level: their list goes here, where the level
    // is left, not only when the next one is first drawn.
    GCappedCount = 0;
    ForgetRippleSources();
    GSeaZone      = -1;
    GSeaSurface   = -1;
    GSeaNode      = -1;
    GSeaSides     = 0;
    GWavesTexture = NULL;
    GWavesFresh   = 0;
    GPoolCount    = 0;
    GCurtainSurfCount = 0;
    GChuteCount   = 0;
    ReportDrawn();
    if (!GOrderReported)
        ReportOrder();
    ResetDrawn();
    ResetOrder();
    GForgotten = 1;
}

void RdxWaterNoteFrame(const FSceneNode* Frame)
{
    if (!Frame || Frame->Parent || !Frame->Level || !Frame->Level->Model)
        return;
    ULevel* Level = Frame->Level;
    const FName Name = Level->GetOuter() ? Level->GetOuter()->GetFName() : FName(NAME_None);
    if (!GForgotten && Level == GLevel && Name == GLevelName)
        return;
    if (!GForgotten)
    {
        ReportDrawn();          // a level change with no Flush before it: what was seen still goes in the log
        if (!GOrderReported)
            ReportOrder();
    }
    ResetDrawn();
    ResetOrder();
    GForgotten = 0;
    GLevel     = Level;
    GLevelName = Name;
    Census(Level);
}

UBOOL RdxWaterNoteDrawn(const FSceneNode* Frame, const FSurfaceInfo& Surface, const FSurfaceFacet& Facet)
{
    if (!Surface.Level || !Surface.Level->Model || !Facet.Polys)
        return 0;
    UModel*   Model = Surface.Level->Model;
    UTexture* Drawn = Surface.Texture ? Surface.Texture->Texture : NULL;

    // Whether iNode is what it says: the node's surface should carry the texture being drawn. The whole approach
    // stands on this, so it is checked on the first polygons of every level rather than assumed.
    if (GNodeChecked < NODE_CHECKS)
    {
        GNodeChecked++;
        const INT iNode = Facet.Polys->iNode;
        if (iNode < 0 || iNode >= Model->Nodes.Num())
            GNodeOutside++;
        else
        {
            const INT iSurf = Model->Nodes(iNode).iSurf;
            if (iSurf >= 0 && iSurf < Model->Surfs.Num() && SameAnimation(Model->Surfs(iSurf).Texture, Drawn))
                GNodeMatched++;
        }
        if (GNodeChecked == NODE_CHECKS)
            RDX_LOGF("water: node check - %i of %i drawn BSP polygons name a node whose surface carries the drawn "
                     "texture (%i name no node at all)", GNodeMatched, GNodeChecked, GNodeOutside);
    }

    INT   Wet = 0, Dry = 0, iBorder = -1;
    for (const FSavedPoly* Poly = Facet.Polys; Poly && iBorder < 0; Poly = Poly->Next)
        if (NodeBorder(Model, Poly->iNode, Wet, Dry))
            iBorder = Poly->iNode;
    if (iBorder >= 0)
        NoteBorderDrawn(Surface.Texture, Drawn, Surface.PolyFlags, Wet, Dry);

    // The order, in the main view only: a see-through water surface becomes a sheet, anything else is tested against
    // the sheets. Water is a liquid zone against another real zone - zone 0 is solid (docs/meres-11).
    const UBOOL bSheet = (iBorder >= 0 && Dry != 0 && (Surface.PolyFlags & PF_Translucent)) ? 1 : 0;
    if (Frame && !Frame->Parent && !GOrderReported)
    {
        if (!bSheet && !GSheetCount && !GPrevSheetCount)
            GFrameDraws++;
        else
        {
            FVector P[MAX_SHAPE_POINTS];
            INT     Num = 0;
            for (const FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next)
                for (INT i = 0; i < Poly->NumPts && Num < MAX_SHAPE_POINTS; i++)
                    P[Num++] = Poly->Pts[i]->Point;
            if (bSheet)
                AddSheet(Frame, Model->Nodes(iBorder).Plane, P, Num);
            else
                NoteShape(Frame, P, Num, Shape_Level, Drawn);
        }
    }
    return (iBorder >= 0) ? 1 : 0;
}

void RdxWaterNoteMesh(const FSceneNode* Frame, FTransTexture** Pts, INT NumPts, UTexture* Texture)
{
    if (!Frame || Frame->Parent || GOrderReported)
        return;
    if (!GSheetCount && !GPrevSheetCount)
    {
        GFrameDraws++;
        return;
    }
    FVector P[MAX_SHAPE_POINTS];
    INT     Num = 0;
    for (INT i = 0; Pts && i < NumPts && Num < MAX_SHAPE_POINTS; i++)
        P[Num++] = Pts[i]->Point;
    NoteShape(Frame, P, Num, Shape_Mesh, Texture);
}

void RdxWaterNoteTile(const FSceneNode* Frame, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT Z, UTexture* Texture)
{
    if (!Frame || Frame->Parent || GOrderReported)
        return;
    if ((!GSheetCount && !GPrevSheetCount) || Z < 1.0f)
    {
        GFrameDraws++;
        return;
    }
    // The tile's corners back in view space at its depth: FTransform::Project the other way round.
    const FLOAT S = Z / Frame->Proj.Z;
    FVector P[4];
    P[0] = FVector((X - Frame->FX15) * S,      (Y - Frame->FY15) * S,      Z);
    P[1] = FVector((X + XL - Frame->FX15) * S, (Y - Frame->FY15) * S,      Z);
    P[2] = FVector((X - Frame->FX15) * S,      (Y + YL - Frame->FY15) * S, Z);
    P[3] = FVector((X + XL - Frame->FX15) * S, (Y + YL - Frame->FY15) * S, Z);
    NoteShape(Frame, P, 4, Shape_Sprite, Texture);
}

// ---------------------------------------------------------------------------
// The water surface as drawn (Water=Low and up)
// ---------------------------------------------------------------------------

// The water texture's own wave field (the procedural water classes' SourceFields, measured - docs/meres-11): for a 256x256
// texture two 128x128 fields of bytes, 128 the still surface, interleaved row by row - each 256-byte row holds a row of
// the one field and then the same row of the other; WaterParity says which is the current step. One texture's field a
// frame is enough - a scene with two different wave textures in view is not one this game has shown.

void RdxWaterCapRate(UBOOL bOn)
{
    GCapRate = bOn ? 1 : 0;
}

void RdxWaterRestoreRates()
{
    for (INT i = 0; i < GCappedCount; i++)
        GCapped[i]->MaxFrameRate = GCappedWas[i];
    GCappedCount = 0;
}

// What the capped textures' rates were, by the textures' names, kept through the levels: a texture that outlives its
// level - the same map loaded again - comes to the next one still capped and off the list (RdxWaterForget). By its name
// its own rate is found again, so that the engine's water can have it back (RdxWaterRestoreRates). Names, never
// pointers: these outlive the levels.
enum { CappedNameRoom = 16, CappedNameLength = 96 };
static TCHAR GCappedName[CappedNameRoom][CappedNameLength];
static FLOAT GCappedNameWas[CappedNameRoom];
static INT   GCappedNameCount = 0;

static void CapRate(UTexture* T)
{
    if (!GCapRate)
        return;
    for (INT i = 0; i < GCappedCount; i++)
        if (GCapped[i] == T)
            return;
    const TCHAR* Path  = T->GetPathName();
    INT          Known = -1;
    for (INT k = 0; k < GCappedNameCount && Known < 0; k++)
        if (!appStricmp(GCappedName[k], Path))
            Known = k;
    FLOAT Was = T->MaxFrameRate;
    if (T->MaxFrameRate == RdxMath::WaterWaveRate)
    {
        // Capped in an earlier level: back on the list with the rate it had. One never capped here that has this very
        // rate of its own is left as it is.
        if (Known < 0)
            return;
        Was = GCappedNameWas[Known];
        RDX_LOGF("water: %s is still capped from an earlier level - listed again with the %.0f steps a second it had",
                 T->GetName(), Was);
    }
    else
    {
        if (Known >= 0)
            GCappedNameWas[Known] = Was;
        else if (GCappedNameCount < CappedNameRoom && appStrlen(Path) < CappedNameLength)
        {
            appStrcpy(GCappedName[GCappedNameCount], Path);
            GCappedNameWas[GCappedNameCount++] = Was;
        }
        RDX_LOGF("water: %s's waves capped at %.0f steps a second (it had %.0f, 0 is once a frame)", T->GetName(),
                 RdxMath::WaterWaveRate, T->MaxFrameRate);
        T->MaxFrameRate = RdxMath::WaterWaveRate;
    }
    if (GCappedCount < 8)
    {
        GCapped[GCappedCount]    = T;
        GCappedWas[GCappedCount] = Was;
        GCappedCount++;
    }
}

static void CountSteps(UTexture* T)
{
    DWORD Hash = 0;
    for (INT i = 0; i < 128 * 128; i += 61)
        Hash = Hash * 31u + GWaves[i];
    if (Hash != GStepHash)
    {
        GStepHash = Hash;
        GStepCount++;
    }
    const DOUBLE Now = appSeconds();
    if (GStepSince <= 0.0)
        GStepSince = Now;
    if (Now - GStepSince >= 2.0)
    {
        if (GStepTold++ < 6)
            RDX_LOGF("water: %s's waves stepped %.0f times a second (MaxFrameRate %.0f)", T->GetName(),
                     GStepCount / (Now - GStepSince), T->MaxFrameRate);
        GStepCount = 0;
        GStepSince = Now;
    }
}

static UBOOL ReadWaves(UTexture* T)
{
    if (!T || T->USize != 256 || T->VSize != 256)
        return 0;
    BYTE* Field = NULL;
    INT   Parity = 0;
    for (TFieldIterator<UProperty> It(T->GetClass()); It; ++It)
    {
        if (!appStricmp(It->GetName(), TEXT("SourceFields")))
            Field = *(BYTE**)((BYTE*)T + It->Offset);
        else if (!appStricmp(It->GetName(), TEXT("WaterParity")))
            Parity = (It->ElementSize == 1) ? *((BYTE*)T + It->Offset) : *(INT*)((BYTE*)T + It->Offset);
    }
    if (!Field)
        return 0;
    static BYTE Rows[128 * 256];
    if (CopyGuarded(Field, Rows, sizeof(Rows)) != (INT)sizeof(Rows))
        return 0;
    const INT Half = (Parity & 1) ? 128 : 0;
    for (INT y = 0; y < 128; y++)
        appMemcpy(GWaves + y * 128, Rows + y * 256 + Half, 128);
    // How far the field's slope spreads: its height step over two cells, as the shader takes it (WaterSlopeAt), in the
    // texture's own units. What a pixel spanning many cells averages away, its mirror image spreads over instead
    // (SolidPass.hlsl, WaterSlopeSpread).
    DOUBLE Sum = 0.0;
    for (INT y = 0; y < 128; y++)
        for (INT x = 0; x < 128; x++)
        {
            const DOUBLE Hx = (GWaves[y * 128 + ((x + 1) & 127)] - GWaves[y * 128 + ((x + 127) & 127)]) / 255.0;
            const DOUBLE Hy = (GWaves[((y + 1) & 127) * 128 + x] - GWaves[((y + 127) & 127) * 128 + x]) / 255.0;
            Sum += 0.5 * (Hx * Hx + Hy * Hy);
        }
    // Held steady over time: the field's spread is the same water from frame to frame, and a value that followed every
    // frame's drops would make the mirror image's streaks wander.
    const FLOAT Now = (FLOAT)appSqrt(Sum / (128.0 * 128.0));
    GWavesStepRms = (GWavesStepRms > 0.0f) ? GWavesStepRms + (Now - GWavesStepRms) * 0.02f : Now;
    return 1;
}

const BYTE* RdxWaterTakeWaves(INT& W, INT& H)
{
    if (!GWavesFresh)
        return NULL;
    GWavesFresh = 0;
    W = 128;
    H = 128;
    return GWaves;
}

UBOOL RdxWaterHueAt(const FSceneNode* Frame, const FVector& World, FVector& Hue)
{
    if (!Frame || !Frame->Level || !Frame->Level->Model)
        return 0;
    const FPointRegion      R    = Frame->Level->Model->PointRegion(Frame->Level->GetLevelInfo(), World);
    const AZoneInfo* const Zone = R.Zone;
    if (!Zone || Zone->bDeleteMe || !Zone->bWaterZone)
        return 0;
    FVector Fog = Zone->ViewFog;
    if (Fog.X <= 0.0f && Fog.Y <= 0.0f && Fog.Z <= 0.0f)
        Fog = FVector(0.0f, 0.05f, 0.10f);
    Hue = Fog / Max(Fog.X, Max(Fog.Y, Fog.Z));
    return 1;
}

UBOOL RdxWaterSheet(const FSceneNode* Frame, const FSurfaceInfo& Surface, const FSurfaceFacet& Facet, FLOAT* Consts)
{
    // A surface the level leaves opaque is water all the same where a water zone meets the air - the sewer's sludge
    // (docs/meres-11): drawn as the engine draws it, only lifted by its waves (BLEND_Sludge). A masked, modulated or
    // invisible sheet there is not the water's surface.
    const UBOOL bOpaque = (Surface.PolyFlags & PF_Translucent) ? 0 : 1;
    if (!Frame || Frame->Parent || !Surface.Level || !Surface.Level->Model || !Facet.Polys ||
        (bOpaque && (Surface.PolyFlags & (PF_Masked | PF_Modulated | PF_Invisible))))
        return 0;
    UModel* Model = Surface.Level->Model;
    INT Wet = 0, Dry = 0, iBorder = -1;
    for (const FSavedPoly* Poly = Facet.Polys; Poly && iBorder < 0; Poly = Poly->Next)
        if (NodeBorder(Model, Poly->iNode, Wet, Dry))
            iBorder = Poly->iNode;
    // An ornamental fountain's pool (FindFountainPools) is water as well. Where it is no liquid's border - a basin -
    // only the fountain makes it so: what the level draws see-through, with no zone to take a hue from.
    const FFountainPool* Pool      = NULL;
    INT                  iPoolNode = -1;
    for (const FSavedPoly* Poly = Facet.Polys; Poly && GPoolCount && !Pool; Poly = Poly->Next)
        if (Poly->iNode >= 0 && Poly->iNode < Model->Nodes.Num())
        {
            Pool      = PoolOfSurf(Model->Nodes(Poly->iNode).iSurf);
            iPoolNode = Poly->iNode;
        }
    const UBOOL bBasin = (iBorder < 0 && Pool && !bOpaque) ? 1 : 0;
    // And so is a fountain's chute (FindFountainChutes): a film of water on the fountain's own lying pieces.
    const FFountainChute* Chute      = NULL;
    INT                   iChuteNode = -1;
    for (const FSavedPoly* Poly = Facet.Polys; Poly && GChuteCount && iBorder < 0 && !Pool && !bOpaque && !Chute;
         Poly = Poly->Next)
        if (Poly->iNode >= 0 && Poly->iNode < Model->Nodes.Num())
        {
            Chute      = ChuteOfSurf(Model->Nodes(Poly->iNode).iSurf);
            iChuteNode = Poly->iNode;
        }
    if (bBasin)
        iBorder = iPoolNode;
    else if (Chute)
        iBorder = iChuteNode;
    else if (iBorder < 0 || Dry == 0)      // zone 0 is solid: the pool's wall, not its surface
        return 0;
    // And only the liquid's own surface (RdxMath::WaterBorderIsSurface): a border lying level, with the liquid
    // under it. An upright one - a pane of glass between the sea and a room, a tank's wall - or a hatch under the
    // water stays the level's own see-through surface. The run is made for a level surface: its frame is laid by
    // how high the camera is over the surface, and on a wall that height is how far off the camera stands - a
    // water level that comes and goes with the eye.
    else if (!RdxMath::WaterBorderIsSurface(Model->Nodes(iBorder).Plane.Z, Model->Nodes(iBorder).iZone[0] == Wet))
        return 0;

    // The water's hue: the zone's view fog, the colour the engine gives the screen under this water - only its hue, as
    // the shader lights it with the surface's own light, so dark water stays dark. A zone that leaves it black gets the
    // hue most of the game's water has (docs/meres-11). A quarter of the light comes back from deep water.
    const AZoneInfo* Zone = (bBasin || Chute) ? NULL : ZoneActor(Model, Wet);
    // An opaque liquid only where the zone is water: a pool that only hurts - acid, lava - is no sludge to lift.
    if (bOpaque && !(Zone && Zone->bWaterZone))
        return 0;
    FVector Deep = Zone ? Zone->ViewFog : FVector(0.0f, 0.0f, 0.0f);
    if (Deep.X <= 0.0f && Deep.Y <= 0.0f && Deep.Z <= 0.0f)
        Deep = FVector(0.0f, 0.05f, 0.10f);
    Deep *= 0.25f / Max(Deep.X, Max(Deep.Y, Deep.Z));

    // Which side the camera is on: the node's back is iZone[0]. Under the water, what lies beyond the surface is air -
    // nothing to absorb, and no shore to fade the texture in at.
    const FBspNode& Node     = Model->Nodes(iBorder);
    const FLOAT     CamDist  = Node.Plane.PlaneDot(Frame->Coords.Origin);
    // A basin has no zone to tell its water's side by, nor has a chute: it is the side under the surface.
    const UBOOL     bBackWet = (bBasin || Chute) ? ((Node.Plane.Z > 0.0f) ? 1 : 0) : ((Node.iZone[0] == Wet) ? 1 : 0);
    const UBOOL     bUnder   = bBackWet ? (CamDist < 0.0f) : (CamDist > 0.0f);

    static DOUBLE Start = 0.0;
    const DOUBLE  Now   = appSeconds();
    if (Start <= 0.0)
        Start = Now;
    // How calm its waves are: by how far the surface this polygon is of reaches across - the pool's, where its zone
    // holds the sea as well.
    FLOAT Across = (!bBasin && !Chute && Wet >= 0 && Wet < FBspNode::MAX_ZONES) ? GZoneAcross[Wet] : 0.0f;
    if (bBasin)
        Across = Pool->Across;
    else if (!Chute)
    {
        FLOAT     Z = 0.0f;
        const INT s = NodeSurface(Model, iBorder, Wet, Z) ? SurfaceOf(Wet, Z) : -1;
        if (s >= 0)
            Across = Min(GSurfaces[s].MaxX - GSurfaces[s].MinX, GSurfaces[s].MaxY - GSurfaces[s].MinY);
    }

    Consts[0] = Deep.X;
    Consts[1] = Deep.Y;
    Consts[2] = Deep.Z;
    // From under the water everything above bends through the waves at once - the whole sky wavers - so the waves are
    // half as strong seen from there.
    Consts[3] = RdxMath::WaterWaveStrength(Across) * (bUnder ? 0.5f : 1.0f);
    Consts[4] = (FLOAT)(Now - Start);
    Consts[5] = Frame->Proj.Z;
    Consts[6] = bUnder ? 0.0f : 1.0f / 700.0f;     // a floor 100 units down keeps about seven eighths of its light
    Consts[7] = bUnder ? 0.0f : 8.0f;              // the texture comes in over the first 8 units of depth
    // What puts a pixel back into the world, for the ripples: the camera and the view's axes (their world X and Y), and
    // the screen's centre in the engine's pixels - the frame's offset included, as the vertices have it. The field's
    // corner (18, 19) is the backend's to fill: it knows which field the water reads (FBackend::GetRippleOrigin).
    const FCoords& C = Frame->Coords;
    Consts[8]  = C.Origin.X;
    Consts[9]  = C.Origin.Y;
    Consts[10] = C.XAxis.X;
    Consts[11] = C.XAxis.Y;
    Consts[12] = C.YAxis.X;
    Consts[13] = C.YAxis.Y;
    Consts[14] = C.ZAxis.X;
    Consts[15] = C.ZAxis.Y;
    Consts[16] = Frame->FX15 + (FLOAT)Frame->XB;
    Consts[17] = Frame->FY15 + (FLOAT)Frame->YB;
    Consts[18] = 1.0e9f;
    Consts[19] = 1.0e9f;
    // The camera's height above this surface - negative from under the water - and the Z of the view's axes: with the XY
    // above, the whole view, for the refraction's ray.
    Consts[20] = bUnder ? -Abs(CamDist) : Abs(CamDist);
    // A chute's piece may slope: the height is then the camera's straight up from the piece - over the place of
    // its plane under the camera - which is what the shader's frame is laid by.
    if (Chute && Abs(Node.Plane.Z) > 0.01f)
        Consts[20] /= Abs(Node.Plane.Z);
    Consts[21] = C.XAxis.Z;
    Consts[22] = C.YAxis.Z;
    Consts[23] = C.ZAxis.Z;
    // The texture's own waves: read once a frame from the first texture that has them, used by every run of that texture.
    // Not from a chute's: that one is the fountain's own picture, no water's.
    UTexture* Tex = Surface.Texture ? Surface.Texture->Texture : NULL;
    if (!GWavesTried && Tex && !Chute)
    {
        GWavesTried = 1;
        if (ReadWaves(Tex))
        {
            GWavesTexture = Tex;
            GWavesFresh   = 1;
            CapRate(Tex);
            CountSteps(Tex);
        }
    }
    // Laid on as the texture is drawn: by the water's largest piece where this piece is drawn as that one lays the
    // texture on (RdxWaterLeadOf).
    const INT       LeadSurf = RdxWaterLeadOf(Model, Node.iSurf);
    const FBspSurf& Surf     = Model->Surfs((LeadSurf >= 0) ? LeadSurf : Node.iSurf);
    FLOAT TileU = 0.0f, TileV = 0.0f;
    TileSize(Model, Surf, TileU, TileV);
    FVector DirU = (Surf.vTextureU >= 0 && Surf.vTextureU < Model->Vectors.Num()) ? Model->Vectors(Surf.vTextureU) : FVector(1, 0, 0);
    FVector DirV = (Surf.vTextureV >= 0 && Surf.vTextureV < Model->Vectors.Num()) ? Model->Vectors(Surf.vTextureV) : FVector(0, 1, 0);
    DirU.Z = 0.0f;
    DirV.Z = 0.0f;
    // A fountain's pool carries the fountain's foot where the texture's own waves' numbers go, and leaves those waves.
    const UBOOL bWaves = (!Pool && Tex && Tex == GWavesTexture && TileU > 0.0f && TileV > 0.0f &&
                          DirU.SizeSquared() > 1.0e-6f && DirV.SizeSquared() > 1.0e-6f) ? 1 : 0;
    if (bWaves)
    {
        DirU.Normalize();
        DirV.Normalize();
    }
    // 0: no field of its own; otherwise 1 plus the field's slope spread (GWavesStepRms), which is below 1.
    Consts[24] = bWaves ? 1.0f + Min(GWavesStepRms, 0.99f) : 0.0f;
    Consts[25] = RdxMath::WaterWaveHeight(Across) * (bUnder ? 0.5f : 1.0f);
    Consts[26] = bWaves ? TileU : 1.0f;
    Consts[27] = bWaves ? TileV : 1.0f;
    Consts[28] = DirU.X;
    Consts[29] = DirU.Y;
    Consts[30] = DirV.X;
    Consts[31] = DirV.Y;
    Consts[32] = Consts[33] = Consts[34] = Consts[35] = 0.0f;
    // The fountain over a pool: that it is one - which way the curtain comes down is the caller's to say - how high
    // the curtain's top is over it, how far either side of the foot's line its water comes down (a slab's two
    // curtains), and the two ends of that line on the level.
    if (Pool)
    {
        Consts[24] = RdxMath::FountainPoolSheet;
        Consts[25] = Pool->Rise;
        Consts[26] = Pool->Spread;
        Consts[28] = Pool->FootA[0];
        Consts[29] = Pool->FootA[1];
        Consts[30] = Pool->FootB[0];
        Consts[31] = Pool->FootB[1];
    }
    // A chute's piece, in the same places: that it is one, when the water at its lower end is at the slot and how
    // long it takes over a unit of the level; which way it runs, how much it drops for a unit, and where its lower
    // end is along its way.
    if (Chute)
    {
        Consts[24] = RdxMath::FountainChuteSheet;
        Consts[25] = Chute->EndTime;
        Consts[26] = RdxMath::ChuteTime(1.0f, Chute->Drop);
        Consts[27] = 0.0f;
        Consts[28] = Chute->WayX;
        Consts[29] = Chute->WayY;
        Consts[30] = Chute->Drop;
        Consts[31] = Chute->EndS;
    }
    return 1;
}

// Whether a thing is in the level's water at its surface, or floating on it - and that water's height. In it: its own
// zone is the water's, its middle near the surface - a body that fell in, the player wading. On it: its middle is in
// the air above, but its body reaches down into the water under it - a boat. Which water is under it the zones' boxes
// say first, and the engine's own question (PointRegion) only for a thing over one of them, low enough. A thing on a
// jetty over the water, its feet above the surface, is on neither.
enum ERippleWhy
{
    RippleWhy_Taken = 0,
    RippleWhy_NoBody,       // no collision cylinder: nothing to push the water with
    RippleWhy_Hidden,       // not there to be seen
    RippleWhy_Dry,          // neither in the water nor on it
    RippleWhy_Far,          // outside the ripples' field around the camera
    RippleWhy_Light,        // a light: it has a cylinder, as every actor has, and no body
};
static ERippleWhy RippleCandidate(ULevel* Level, const AActor* A, const AActor* Viewer, const FVector& Cam,
                                  FLOAT& SurfaceZ)
{
    // Things that are there to be seen - and the player, whose own body is hidden from the first-person view.
    if (A->CollisionRadius <= 0.0f)
        return RippleWhy_NoBody;
    if (A->bHidden && A != Viewer)
        return RippleWhy_Hidden;
    // A light is no body, whatever cylinder it carries - the flashlight's beam is one, not hidden, put down
    // wherever the beam ends: on the water, where the player looks at it.
    if (A->IsA(ALight::StaticClass()))
        return RippleWhy_Light;
    const FLOAT Reach = 0.5f * RdxMath::RippleWorld;
    if (Abs(A->Location.X - Cam.X) > Reach || Abs(A->Location.Y - Cam.Y) > Reach)
        return RippleWhy_Far;
    const INT Z    = A->Region.ZoneNumber;
    const INT Own  = (Z > 0 && Z < FBspNode::MAX_ZONES) ? SurfaceNear(Z, A->Location, 64.0f) : -1;
    if (Own >= 0)
    {
        SurfaceZ = GSurfaces[Own].Z;
        return (Abs(A->Location.Z - SurfaceZ) > A->CollisionHeight + 16.0f) ? RippleWhy_Dry : RippleWhy_Taken;
    }
    for (INT k = 0; k < GSurfaceCount; k++)
    {
        const FWaterSurface& S = GSurfaces[k];
        const FLOAT Bottom = A->Location.Z - A->CollisionHeight;
        if (A->Location.X < S.MinX || A->Location.X > S.MaxX || A->Location.Y < S.MinY || A->Location.Y > S.MaxY ||
            A->Location.Z < S.Z || Bottom > S.Z + 4.0f)
            continue;
        const FVector      Under(A->Location.X, A->Location.Y, S.Z - 4.0f);
        const FPointRegion R = Level->Model->PointRegion(Level->GetLevelInfo(), Under);
        if (R.ZoneNumber == S.Zone)
        {
            SurfaceZ = S.Z;
            return RippleWhy_Taken;
        }
    }
    return RippleWhy_Dry;
}

// The things the ripples heard from last frame, and where they were. A thing the engine moves along a path
// (PHYS_Interpolating), or puts in place itself, has no speed of its own - Velocity stays at nothing - so its speed is
// how far it went since. Two lists, last frame's and this one's. Pointers compared only, never followed; forgotten with
// the level.
enum { RippleMoverRoom = 64 };
struct FRippleMover
{
    const AActor* A;
    FVector       At;
};
static FRippleMover GRippleMovers[2][RippleMoverRoom];
static INT          GRippleMoverCount[2] = { 0, 0 };
static INT          GRippleMoverNow      = 0;
static DOUBLE       GRippleMoverTime     = 0.0;
static ULevel*      GRippleMoverLevel    = NULL;      // compared only, never followed

// The rain on the water. A raindrop that goes into a water zone is gone before the next frame is drawn - the game takes
// it away there and draws its own splash - so a drop is heard a frame ahead: one whose fall takes it down through a
// level water surface before the next frame taps the water where it goes in, and the rings that tap sends out are the
// ripples' own and meet the others as rings do. Which water is under it the surfaces' boxes say first; the level's own
// geometry (LineCheck) and the engine's own question (PointRegion) are asked only of a drop that comes to a surface's
// height now - nothing in its way down, and that water there. A drop is known by its class's name, its own or a
// parent's (the Precipitation package's Raindrop): what the game leaves where it lands differs from map to map, the
// drop does not. Nothing is kept between frames but that name, looked for until the package that holds it is loaded,
// and forgotten with the level.
static FName GRaindropName(NAME_None);
// How many drops a second are going into the water around the camera, evened out over half a second, and when that was
// last counted: what the rings the water's own shader draws go by (RdxWaterRainThick).
static FLOAT  GRainRate     = 0.0f;
static DOUBLE GRainRateTime = 0.0;
static FLOAT  GRainLevelTime = 0.0f;     // the level's clock at the last count

// The level is left (RdxWaterForget): what the ripples' sources kept of it goes, the rain's name with it - the next
// call starts as on a new level, whatever address that level comes to.
static void ForgetRippleSources()
{
    GRippleMoverLevel = NULL;
    GRaindropName     = NAME_None;
    GRainRate         = 0.0f;
    GRainLevelTime    = 0.0f;
}

static INT RainTaps(ULevel* Level, const FVector& Cam, FLOAT Dt, FLOAT* Out, INT Room)
{
    if (GRaindropName == NAME_None)
        GRaindropName = FName(TEXT("Raindrop"), FNAME_Find);
    static INT Turn = 0;
    INT N = 0, Drops = 0, Blocked = 0, Other = 0, Full = 0, Went = 0;
    if (GRaindropName != NAME_None && Dt > 0.0f)
    {
        const FLOAT Reach = 0.5f * RdxMath::RippleWorld;
        const FLOAT Ahead = Min(Dt, 0.1f);
        for (INT i = 0; i < Level->Actors.Num(); i++)
        {
            const AActor* A = Level->Actors(i);
            // Falling, and near enough: all but the rain and a thing dropped stop here.
            if (!A || A->bDeleteMe || A->Velocity.Z > -1.0f || Abs(A->Location.X - Cam.X) > Reach ||
                Abs(A->Location.Y - Cam.Y) > Reach)
                continue;
            UBOOL bDrop = 0;
            for (UClass* C = A->GetClass(); C && !bDrop; C = C->GetSuperClass())
                bDrop = (C->GetFName() == GRaindropName);
            if (!bDrop)
                continue;
            Drops++;
            const FLOAT EndZ = A->Location.Z + A->Velocity.Z * Ahead;
            for (INT k = 0; k < GSurfaceCount; k++)
            {
                const FWaterSurface& S = GSurfaces[k];
                if (A->Location.Z <= S.Z || EndZ > S.Z)
                    continue;
                const FLOAT T = (A->Location.Z - S.Z) / -A->Velocity.Z;
                const FLOAT X = A->Location.X + A->Velocity.X * T;
                const FLOAT Y = A->Location.Y + A->Velocity.Y * T;
                if (X < S.MinX || X > S.MaxX || Y < S.MinY || Y > S.MaxY)
                    continue;
                FCheckResult Hit(1.0f);
                if (!Level->Model->LineCheck(Hit, NULL, FVector(X, Y, S.Z + 1.0f), A->Location,
                                             FVector(0.0f, 0.0f, 0.0f), 0))
                {
                    Blocked++;
                    break;
                }
                if (Level->Model->PointRegion(Level->GetLevelInfo(), FVector(X, Y, S.Z - 4.0f)).ZoneNumber != S.Zone)
                {
                    Other++;
                    continue;
                }
                Went++;
                if ((Turn++ % RdxMath::RainRippleEvery) != 0)
                    break;
                if (N >= Room)
                {
                    Full++;
                    break;
                }
                Out[N * 4 + 0] = X;
                Out[N * 4 + 1] = Y;
                Out[N * 4 + 2] = RdxMath::RainRippleRadius;
                Out[N * 4 + 3] = -RdxMath::RainRipplePush;
                N++;
                break;
            }
        }
    }
    // While the level's clock stands - the game paused, its world still drawn - no drop moves: the count stays what it
    // was, so the rings stay on the water, and nothing taps.
    if (Dt > 0.0f)
        GRainRate += ((FLOAT)Went / Dt - GRainRate) * Min(Dt / 0.5f, 1.0f);
    GRainRateTime = appSeconds();
    // Whether it does anything at all, said wherever it rains: the last few hundred frames' drops, and what became
    // of those that came to a water's height.
    static INT Frames = 0, Taps = 0, Most = 0, Stopped = 0, Elsewhere = 0, NoRoom = 0, In = 0;
    Taps      += N;
    Most       = Max(Most, Drops);
    Stopped   += Blocked;
    Elsewhere += Other;
    NoRoom    += Full;
    In        += Went;
    if (++Frames >= 300)
    {
        if (Most > 0)
            RDX_LOGF("rain on the water: %i taps in the last %i frames for %i drops going in (one in %i taps), at most "
                     "%i raindrops near at a time; of the drops that came to a water's height %i more had the level "
                     "in their way, %i had no such water under them; %i taps found no room among the frame's pushes",
                     Taps, Frames, In, (INT)RdxMath::RainRippleEvery, Most, Stopped, Elsewhere, NoRoom);
        Frames = Taps = Most = Stopped = Elsewhere = NoRoom = In = 0;
    }
    return N;
}

FLOAT RdxWaterLampShows(const FSceneNode* Frame, const FVector& Lamp)
{
    if (!Frame || !Frame->Level || !Frame->Level->Model || !GSurfaceCount)
        return 1.0f;
    const FVector Eye = Frame->Coords.Origin;
    const FVector Way = Lamp - Eye;
    const FLOAT   Far = Way.Size();
    if (Far < 1.0f)
        return 1.0f;
    FLOAT Shows = 1.0f;
    for (INT k = 0; k < GSurfaceCount && Shows > 0.0f; k++)
    {
        const FWaterSurface& S = GSurfaces[k];
        if ((Eye.Z - S.Z) * (Lamp.Z - S.Z) >= 0.0f)
            continue;                               // both on one side of it
        const FVector At = Eye + Way * ((S.Z - Eye.Z) / Way.Z);
        if (At.X < S.MinX || At.X > S.MaxX || At.Y < S.MinY || At.Y > S.MaxY)
            continue;
        // The surface's box may hold more than the surface: the engine's own word on what lies under the place.
        const FPointRegion Under = Frame->Level->Model->PointRegion(Frame->Level->GetLevelInfo(),
                                                                    FVector(At.X, At.Y, S.Z - 4.0f));
        if (Under.ZoneNumber != S.Zone)
            continue;
        Shows *= S.Clear ? RdxMath::WaterLampThrough(Way.Z / Far, Eye.Z < S.Z) : 0.0f;
    }
    return Shows;
}

FLOAT RdxFloorRainThick(ULevel* Level, const FVector& Cam)
{
    static FLOAT          Thick   = 0.0f;
    static FLOAT          Clock   = 0.0f;
    static const ULevel*  Counted = NULL;     // compared only, never followed
    if (!Level)
        return 0.0f;
    if (Level != Counted)
    {
        Counted = Level;
        Thick   = 0.0f;
        Clock   = 0.0f;
    }
    if (GRaindropName == NAME_None)
        GRaindropName = FName(TEXT("Raindrop"), FNAME_Find);
    INT Drops = 0;
    if (GRaindropName != NAME_None)
    {
        const FLOAT Reach = 0.5f * RdxMath::RippleWorld;
        for (INT i = 0; i < Level->Actors.Num(); i++)
        {
            const AActor* A = Level->Actors(i);
            if (!A || A->bDeleteMe || A->Velocity.Z > -1.0f || Abs(A->Location.X - Cam.X) > Reach ||
                Abs(A->Location.Y - Cam.Y) > Reach)
                continue;
            for (UClass* C = A->GetClass(); C; C = C->GetSuperClass())
                if (C->GetFName() == GRaindropName)
                {
                    Drops++;
                    break;
                }
        }
    }
    // By the level's clock: a paused game's rain stands, and its rings with it.
    const FLOAT Now = Level->GetLevelInfo() ? Level->GetLevelInfo()->TimeSeconds : 0.0f;
    const FLOAT Dt  = (Clock > 0.0f && Now > Clock) ? Now - Clock : 0.0f;
    Clock           = Now;
    const FLOAT To  = Clamp((FLOAT)Drops / RdxMath::FloorRainFullDrops, 0.0f, 1.0f);
    Thick += (To - Thick) * Min(Dt / 0.5f, 1.0f);
    return Thick;
}

FLOAT RdxWaterRainThick()
{
    // Not counted for a while - no ripples running - is no rain.
    if (appSeconds() - GRainRateTime > 0.5)
        return 0.0f;
    return Clamp(GRainRate / RdxMath::RainRingFullRate, 0.0f, 1.0f);
}

INT RdxWaterRippleSources(const FSceneNode* Frame, FLOAT* Out, INT Max)
{
    if (!Frame || Frame->Parent || !Frame->Level || !GAnyLevelWater || GForgotten)
        return 0;
    ULevel* Level = Frame->Level;
    const FVector Cam = Frame->Coords.Origin;
    const AActor* Viewer = (Frame->Viewport) ? Frame->Viewport->Actor : NULL;
    const DOUBLE Now = appSeconds();
    if (GRippleMoverLevel != Level)
    {
        GRippleMoverLevel    = Level;
        GRippleMoverCount[0] = 0;
        GRippleMoverCount[1] = 0;
        GRippleMoverTime     = 0.0;
        GRaindropName        = NAME_None;
        GRainRate            = 0.0f;
        GRainLevelTime       = 0.0f;
    }
    const INT   Last = GRippleMoverNow;
    const INT   This = 1 - Last;
    const FLOAT Dt   = (GRippleMoverTime > 0.0) ? (FLOAT)(Now - GRippleMoverTime) : 0.0f;
    GRippleMoverNow          = This;
    GRippleMoverCount[This]  = 0;
    GRippleMoverTime         = Now;
    INT N = 0;
    for (INT i = 0; i < Level->Actors.Num() && N < Max; i++)
    {
        const AActor* A = Level->Actors(i);
        FLOAT SurfaceZ = 0.0f;
        if (!A || A->bDeleteMe || RippleCandidate(Level, A, Viewer, Cam, SurfaceZ) != RippleWhy_Taken)
            continue;
        // Its speed: the engine's, or how far it went since last frame - further than a thing goes in a frame is a jump
        // to another place, not a speed.
        FLOAT Along = A->Velocity.Size2D();
        for (INT k = 0; k < GRippleMoverCount[Last]; k++)
            if (GRippleMovers[Last][k].A == A)
            {
                const FLOAT Went = (A->Location - GRippleMovers[Last][k].At).Size2D();
                if (Dt > 1.0e-3f && Went < 128.0f && Went / Dt > Along)
                    Along = Went / Dt;
                break;
            }
        if (GRippleMoverCount[This] < RippleMoverRoom)
        {
            FRippleMover& M = GRippleMovers[This][GRippleMoverCount[This]++];
            M.A  = A;
            M.At = A->Location;
        }
        // A thing standing still pushes nothing, and would only take a push's place from one that does.
        const FLOAT Push = RdxMath::RipplePush(Along, -A->Velocity.Z);
        if (Abs(Push) < 0.005f)
            continue;
        Out[N * 4 + 0] = A->Location.X;
        Out[N * 4 + 1] = A->Location.Y;
        Out[N * 4 + 2] = Clamp(A->CollisionRadius, 8.0f, 64.0f);
        Out[N * 4 + 3] = Push;
        N++;
    }
    // The rain last, in the room the things left: a frame's pushes are few, and the rain has many drops to spare.
    // By the level's own clock, not the wall's: a drop's speed is in the level's seconds, and a paused game's drops
    // stand where they are - by the wall's clock the same drops would go in again every frame.
    const FLOAT LevelNow = Level->GetLevelInfo() ? Level->GetLevelInfo()->TimeSeconds : 0.0f;
    const FLOAT RainDt   = (GRainLevelTime > 0.0f && LevelNow > GRainLevelTime) ? LevelNow - GRainLevelTime : 0.0f;
    GRainLevelTime       = LevelNow;
    N += RainTaps(Level, Cam, RainDt, Out + N * 4, Max - N);
    return N;
}

void RdxWaterRippleProbe(const FSceneNode* Frame)
{
    static INT Calls = 0;
    if (!Frame || Frame->Parent || !Frame->Level || !GAnyLevelWater || GForgotten || (++Calls % 60) != 0)
        return;
    ULevel* Level = Frame->Level;
    const FVector Cam = Frame->Coords.Origin;
    const AActor* Viewer = (Frame->Viewport) ? Frame->Viewport->Actor : NULL;
    enum { Room = 512 };
    static const AActor* Was[Room];               // compared only, never followed
    static FVector       WasAt[Room];
    static INT           WasCount = 0;
    static DOUBLE        WasTime  = 0.0;
    static ULevel*       WasLevel = NULL;         // compared only, never followed
    static const AActor* Seen[Room];
    static FVector       SeenAt[Room];
    const DOUBLE Now = appSeconds();
    const FLOAT  Dt  = (WasLevel == Level && WasTime > 0.0) ? (FLOAT)(Now - WasTime) : 0.0f;
    static const TCHAR* const Why[] = { TEXT("taken"), TEXT("no collision cylinder"), TEXT("hidden"),
                                        TEXT("neither in the water nor on it"), TEXT("outside the ripples' field"),
                                        TEXT("a light, no body") };
    INT SeenCount = 0, Said = 0;
    for (INT i = 0; i < Level->Actors.Num(); i++)
    {
        const AActor* A = Level->Actors(i);
        if (!A || A->bDeleteMe)
            continue;
        if (Abs(A->Location.X - Cam.X) > RdxMath::RippleWorld || Abs(A->Location.Y - Cam.Y) > RdxMath::RippleWorld)
            continue;
        // Over a level water's box at all - near its surface, whatever its cylinder says.
        INT Over = -1;
        for (INT k = 0; k < GSurfaceCount && Over < 0; k++)
        {
            const FWaterSurface& S = GSurfaces[k];
            if (A->Location.X >= S.MinX && A->Location.X <= S.MaxX && A->Location.Y >= S.MinY &&
                A->Location.Y <= S.MaxY && Abs(A->Location.Z - S.Z) < 512.0f)
                Over = k;
        }
        if (Over < 0)
            continue;
        if (SeenCount < Room)
        {
            Seen[SeenCount]   = A;
            SeenAt[SeenCount] = A->Location;
            SeenCount++;
        }
        FLOAT Moved = -1.0f;
        for (INT s = 0; s < WasCount; s++)
            if (Was[s] == A)
            {
                Moved = (A->Location - WasAt[s]).Size2D();
                break;
            }
        const FLOAT Speed = (Dt > 0.0f && Moved >= 0.0f) ? Moved / Dt : -1.0f;
        if (A->Velocity.Size() < 1.0f && Speed < 4.0f)
            continue;
        if (Said++ >= 16)
            continue;
        FLOAT SurfaceZ = GSurfaces[Over].Z;
        const ERippleWhy W = RippleCandidate(Level, A, Viewer, Cam, SurfaceZ);
        RDX_LOGF("ripple probe: %s (%s), physics %i, cylinder %.0f x %.0f, its zone %i, %.0f above the water's surface "
                 "(zone %i), %.0f from the camera; velocity %.0f along, %.0f up; moving %.0f a second - %s",
                 A->GetName(), A->GetClass()->GetName(), (INT)A->Physics, A->CollisionRadius, A->CollisionHeight,
                 (INT)A->Region.ZoneNumber, A->Location.Z - SurfaceZ, GSurfaces[Over].Zone,
                 (A->Location - Cam).Size2D(), A->Velocity.Size2D(), A->Velocity.Z, Speed, Why[W]);
    }
    for (INT s = 0; s < SeenCount; s++)
    {
        Was[s]   = Seen[s];
        WasAt[s] = SeenAt[s];
    }
    WasCount = SeenCount;
    WasTime  = Now;
    WasLevel = Level;
}

INT RdxWaterLamps(const FSceneNode* Frame, FLOAT* Out, INT Room)
{
    if (!Frame || Frame->Parent || !Frame->Level || !GAnyLevelWater || GForgotten || Room <= 0)
        return 0;
    ULevel* Level = Frame->Level;
    const FVector Cam = Frame->Coords.Origin;
    Room = Min(Room, (INT)RdxMath::WaterLampMax);
    const AActor* Best[RdxMath::WaterLampMax];
    FLOAT         Rank[RdxMath::WaterLampMax];
    INT           N = 0;
    FLOAT         Cut = 0.0f;       // the best rank among the lamps left out (RdxMath::PickEdgeShare)
    for (INT i = 0; i < Level->Actors.Num(); i++)
    {
        // Not a light marked bSpecialLit: the engine lights only the surfaces marked the same with it, and a water's
        // surface is not one of them.
        const AActor* L = Level->Actors(i);
        if (!L || L->bDeleteMe || L->LightType == LT_None || L->LightBrightness == 0 || L->bSpecialLit)
            continue;
        const FLOAT R = L->WorldLightRadius();
        if (R < 64.0f)
            continue;
        // How far its light has to go to the nearest level water surface below it: to that surface's box, at its height.
        FLOAT Reach = R;
        for (INT k = 0; k < GSurfaceCount; k++)
        {
            const FWaterSurface& S = GSurfaces[k];
            if (L->Location.Z <= S.Z)
                continue;
            const FLOAT DX = Max(Max(S.MinX - L->Location.X, L->Location.X - S.MaxX), 0.0f);
            const FLOAT DY = Max(Max(S.MinY - L->Location.Y, L->Location.Y - S.MaxY), 0.0f);
            const FLOAT DZ = L->Location.Z - S.Z;
            Reach = Min(Reach, (FLOAT)appSqrt(DX * DX + DY * DY + DZ * DZ));
        }
        const FLOAT Score = RdxMath::WaterLampRank(L->LightBrightness / 255.0f, R, Reach, (L->Location - Cam).Size());
        if (Score <= 0.0f)
            continue;
        if (N == Room && Score <= Rank[N - 1])
        {
            Cut = Max(Cut, Score);
            continue;
        }
        // Into the list, best first; with the list full, its last falls out.
        if (N == Room)
            Cut = Max(Cut, Rank[N - 1]);
        INT At = (N < Room) ? N++ : N - 1;
        while (At > 0 && Rank[At - 1] < Score)
        {
            Best[At] = Best[At - 1];
            Rank[At] = Rank[At - 1];
            At--;
        }
        Best[At] = L;
        Rank[At] = Score;
    }
    // Said when the set changes, a few times a level - which lamps, how bright, how far their light has to go to the water.
    static const AActor* Said[RdxMath::WaterLampMax];
    static INT           SaidN = -1, SaidTimes = 0;
    static ULevel*       SaidLevel = NULL;              // compared only, never followed
    if (SaidLevel != Level)
    {
        SaidLevel = Level;
        SaidTimes = 0;
        SaidN     = -1;
    }
    UBOOL bSame = (SaidN == N);
    for (INT k = 0; k < N && bSame; k++)
        bSame = (Said[k] == Best[k]);
    if (!bSame && SaidTimes < 6)
    {
        SaidTimes++;
        SaidN = N;
        TCHAR List[400] = { 0 };
        for (INT k = 0; k < N; k++)
        {
            Said[k] = Best[k];
            TCHAR One[96];
            appSprintf(One, TEXT("%s%s (brightness %i, reach %.0f, rank %.3f)"), k ? TEXT(", ") : TEXT(""),
                       Best[k]->GetName(), (INT)Best[k]->LightBrightness, Best[k]->WorldLightRadius(), Rank[k]);
            if (appStrlen(List) + appStrlen(One) < 399)
                appStrcat(List, One);
        }
        RDX_LOGF("water: %i lamps glint on the water%s%s", N, N ? TEXT(": ") : TEXT(""), List);
    }
    for (INT k = 0; k < N; k++)
    {
        const AActor* L = Best[k];
        // The engine's own colour for the light, from its hue and saturation, at the light's brightness.
        const FPlane C  = FGetHSV(L->LightHue, L->LightSaturation, 255);
        const FLOAT  B  = (L->LightBrightness / 255.0f) * RdxMath::PickEdgeShare(Rank[k], Cut, RdxMath::WaterLampFade);
        FLOAT* const O  = Out + k * 8;
        O[0] = L->Location.X - Cam.X;
        O[1] = L->Location.Y - Cam.Y;
        O[2] = L->Location.Z - Cam.Z;
        O[3] = L->WorldLightRadius();
        O[4] = C.X * B;
        O[5] = C.Y * B;
        O[6] = C.Z * B;
        O[7] = 0.0f;
    }
    return N;
}

UBOOL RdxWaterIsCurtain(INT iSurf)
{
    if (GForgotten)
        return 0;
    for (INT i = 0; i < GCurtainSurfCount; i++)
        if (GCurtainSurfs[i].iSurf == iSurf)
            return 1;
    return 0;
}

INT RdxWaterFountains(const FSceneNode* Frame, FLOAT* Out, INT Room)
{
    if (!Frame || Frame->Parent || !Frame->Level || GForgotten || Room <= 0)
        return 0;
    const FVector Cam = Frame->Coords.Origin;
    FLOAT Near[RdxMath::CurtainMax];
    INT   N = 0;
    Room = Min(Room, (INT)RdxMath::CurtainMax);
    for (INT c = 0; c < GCurtainSurfCount; c++)
    {
        const FFountainCurtain& C = GCurtainSurfs[c];
        const FVector Mid(0.5f * (C.A[0] + C.B[0]), 0.5f * (C.A[1] + C.B[1]), C.Z + 0.5f * C.Rise);
        const FLOAT   D = (Mid - Cam).Size();
        if (D > RdxMath::CurtainReach || (N == Room && D >= Near[N - 1]))
            continue;
        // Into the list, the nearest first; with the list full, its last falls out.
        INT At = (N < Room) ? N++ : N - 1;
        for (; At > 0 && Near[At - 1] > D; At--)
        {
            Near[At] = Near[At - 1];
            for (INT k = 0; k < RdxMath::CurtainFloats; k++)
                Out[At * RdxMath::CurtainFloats + k] = Out[(At - 1) * RdxMath::CurtainFloats + k];
        }
        Near[At] = D;
        FLOAT* const O = Out + At * RdxMath::CurtainFloats;
        O[0] = C.A[0];
        O[1] = C.A[1];
        O[2] = C.B[0];
        O[3] = C.B[1];
        O[4] = C.Z;
        O[5] = C.Rise;
        O[6] = RdxMath::CurtainSpread;
        O[7] = (FLOAT)(C.iSurf & 0xFFFF);
        // The chute its water comes down by, where it has one: the pieces whose chute ends at this curtain's top,
        // from the slot upwards - each next one the piece whose water gets to the slot the next latest.
        FLOAT* const K = O + RdxMath::CurtainOwnFloats;
        appMemzero(K, RdxMath::ChuteRows * 4 * sizeof(FLOAT));
        const FLOAT TopX = 0.5f * (C.A[0] + C.B[0]), TopY = 0.5f * (C.A[1] + C.B[1]), TopZ = C.Z + C.Rise;
        const FLOAT Wide = 0.5f * appSqrt(Square(C.B[0] - C.A[0]) + Square(C.B[1] - C.A[1])) + 8.0f;
        INT   Pieces = 0, Over = 0;
        FLOAT Round = 0.0f, After = 1.0f;
        for (;;)
        {
            const FFountainChute* Next = NULL;
            for (INT p = 0; p < GChuteCount; p++)
            {
                const FFountainChute& P = GChutes[p];
                if (P.bTaken && P.EndTime < After && Abs(P.SlotZ - TopZ) < 8.0f &&
                    Square(P.SlotX - TopX) + Square(P.SlotY - TopY) < Wide * Wide &&
                    (!Next || P.EndTime > Next->EndTime))
                    Next = &P;
            }
            if (!Next)
                break;
            After = Next->EndTime;
            if (Pieces >= RdxMath::ChutePieces)
            {
                Over++;
                continue;
            }
            K[Pieces * 4]     = appAtan2(Next->WayY, Next->WayX);
            K[Pieces * 4 + 1] = Next->Drop;
            K[Pieces * 4 + 2] = Next->Seconds;
            K[Pieces * 4 + 3] = Next->Run;
            Round += Next->Seconds;
            Pieces++;
        }
        K[RdxMath::ChutePieces * 4]     = Round;
        K[RdxMath::ChutePieces * 4 + 1] = (FLOAT)Pieces;
        // A chute of more pieces than the drops are told of: said, once a fountain.
        static INT ToldSurf = -1;
        if (Over > 0 && ToldSurf != C.iSurf)
        {
            ToldSurf = C.iSurf;
            RDX_LOGF("fountain drops: the chute of curtain %i has %i pieces above the %i its drops ride", C.iSurf,
                     Over, (INT)RdxMath::ChutePieces);
        }
    }
    return N;
}

UBOOL RdxWaterHasRipples()
{
    return (GAnyLevelWater && !GForgotten) ? 1 : 0;
}

INT RdxWaterBoxes(const FSceneNode* Frame, FLOAT* Out, INT Room)
{
    if (!Frame || !Out || Room <= 0 || !GAnyLevelWater || GForgotten)
        return 0;
    Room = Min(Room, (INT)RdxMath::CausticBoxesMost);
    const FVector Cam = Frame->Coords.Origin;
    // How far the camera is from each surface: across the level to its extent, and up or down to its height.
    INT   Pick[RdxMath::CausticBoxesMost];
    FLOAT Far[RdxMath::CausticBoxesMost];
    INT   N = 0;
    GBoxesNear = 0;
    for (INT k = 0; k < GSurfaceCount; k++)
    {
        const FWaterSurface& S = GSurfaces[k];
        if (!S.Clear)
            continue;           // a surface the level draws opaque lets no light through
        const FLOAT DX = Max(Max(S.MinX - Cam.X, Cam.X - S.MaxX), 0.0f);
        const FLOAT DY = Max(Max(S.MinY - Cam.Y, Cam.Y - S.MaxY), 0.0f);
        const FLOAT D  = (FLOAT)appSqrt(DX * DX + DY * DY) + Abs(Cam.Z - S.Z);
        if (D > RdxMath::CausticReach)
            continue;
        GBoxesNear++;
        INT At = N;
        while (At > 0 && Far[At - 1] > D)
            At--;
        if (At >= Room)
            continue;
        for (INT j = Min(N, Room - 1); j > At; j--)
        {
            Pick[j] = Pick[j - 1];
            Far[j]  = Far[j - 1];
        }
        Pick[At] = k;
        Far[At]  = D;
        N = Min(N + 1, Room);
    }
    for (INT i = 0; i < N; i++)
    {
        const FWaterSurface& S = GSurfaces[Pick[i]];
        FLOAT* const O = Out + i * RdxMath::CausticBoxFloats;
        O[0] = S.MinX;
        O[1] = S.MinY;
        O[2] = S.MaxX;
        O[3] = S.MaxY;
        O[4] = S.Z;
        O[5] = (FLOAT)(WaterOf(S.Zone) + 1);
    }
    return N;
}

INT RdxWaterBoxesNear()
{
    return GBoxesNear;
}

INT RdxWaterPortals(const FSceneNode* Frame, const FLOAT*& Verts, INT& CamWet)
{
    static FLOAT Out[RdxMath::CausticPortalVertsMost * RdxMath::CausticPortalFloats];
    static INT   Order[MAX_WATER_PORTALS];
    static FLOAT Far[MAX_WATER_PORTALS];
    Verts  = Out;
    CamWet = 0;
    if (!Frame || !Frame->Level || !Frame->Level->Model || GForgotten || !GPortalCount)
        return 0;
    const FVector Cam  = Frame->Coords.Origin;
    const INT     Zone = Frame->Level->Model->PointRegion(Frame->Level->GetLevelInfo(), Cam).ZoneNumber;
    if (Zone > 0 && Zone < FBspNode::MAX_ZONES && GBodyClear[WaterOf(Zone)])
        CamWet = WaterOf(Zone) + 1;
    // The borders near enough for their caustics to show and not wholly outside the picture, the nearest first: each
    // writes the mask only behind itself, so the one drawn last at a pixel is the last the view crosses before it.
    // Outside the picture: every corner behind the camera, or every corner past the same edge of the view - taken a
    // little wider than the view, for the upscaler's shift.
    const FCoords& C     = Frame->Coords;
    const FLOAT    Focal = Max(Frame->Proj.Z, 1.0f);
    const FLOAT    SideX = 1.05f * Frame->FX2 / Focal;
    const FLOAT    SideY = 1.05f * Frame->FY2 / Focal;
    INT N = 0;
    for (INT i = 0; i < GPortalCount; i++)
    {
        const FWaterPortal& P = GPortals[i];
        const FVector To   = P.Middle - Cam;
        const FLOAT   Away = To.Size();
        if (Away - P.Radius > RdxMath::CausticReach)
            continue;
        INT Past[5] = { 0, 0, 0, 0, 0 };
        for (INT v = 0; v < P.Count; v++)
        {
            const FVector V  = GPortalPoints[P.First + v] - Cam;
            const FLOAT   X  = V | C.XAxis, Y = V | C.YAxis, Z = V | C.ZAxis;
            Past[0] += (Z < 1.0f) ? 1 : 0;
            Past[1] += (X > Z * SideX) ? 1 : 0;
            Past[2] += (X < -Z * SideX) ? 1 : 0;
            Past[3] += (Y > Z * SideY) ? 1 : 0;
            Past[4] += (Y < -Z * SideY) ? 1 : 0;
        }
        if (Past[0] == P.Count || Past[1] == P.Count || Past[2] == P.Count || Past[3] == P.Count || Past[4] == P.Count)
            continue;
        INT At = N++;
        for (; At > 0 && Far[At - 1] > Away; At--)
        {
            Order[At] = Order[At - 1];
            Far[At]   = Far[At - 1];
        }
        Order[At] = i;
        Far[At]   = Away;
    }
    INT Count = 0;
    for (INT k = 0; k < N; k++)
    {
        const FWaterPortal& P = GPortals[Order[k]];
        if (Count + (P.Count - 2) * 3 > (INT)RdxMath::CausticPortalVertsMost)
            break;              // the farthest are the ones left out
        // Seen from its dry side the view goes into the liquid through it, from the other side out of it.
        const FLOAT Side = P.DryPlane.PlaneDot(Cam);
        if (Side == 0.0f)
            continue;
        const FLOAT    Value = RdxMath::CausticMaskValue(Side > 0.0f, WaterOf(P.Zone));
        const FVector* Pt    = GPortalPoints + P.First;
        for (INT v = 1; v + 1 < P.Count; v++)
        {
            const FVector* const Corner[3] = { Pt, Pt + v, Pt + v + 1 };
            for (INT c = 0; c < 3; c++)
            {
                FLOAT* const O = Out + (Count++) * RdxMath::CausticPortalFloats;
                O[0] = Corner[c]->X;
                O[1] = Corner[c]->Y;
                O[2] = Corner[c]->Z;
                O[3] = Value;
            }
        }
    }
    return Count;
}

UBOOL RdxWaterTestPoint(const FSceneNode* Frame, FLOAT& X, FLOAT& Y, FLOAT* Z, FLOAT Ahead)
{
    if (!Frame || Frame->Parent || !GAnyLevelWater || GForgotten)
        return 0;
    // Ahead units in front of the camera on the level, pulled into the nearest level water surface's box - a point on
    // water the camera is looking towards, whatever the view's pitch.
    const FVector O = Frame->Coords.Origin;
    FVector Forward(Frame->Coords.ZAxis.X, Frame->Coords.ZAxis.Y, 0.0f);
    if (Forward.SizeSquared() < 1.0e-4f)
        return 0;
    Forward.Normalize();
    const FLOAT AX = O.X + Ahead * Forward.X, AY = O.Y + Ahead * Forward.Y;
    FLOAT Best = 2000.0f;
    UBOOL bHit = 0;
    for (INT k = 0; k < GSurfaceCount; k++)
    {
        const FWaterSurface& S = GSurfaces[k];
        const FLOAT PX = Clamp(AX, S.MinX + 32.0f, S.MaxX - 32.0f);
        const FLOAT PY = Clamp(AY, S.MinY + 32.0f, S.MaxY - 32.0f);
        const FLOAT Far = appSqrt((PX - AX) * (PX - AX) + (PY - AY) * (PY - AY));
        if (Far >= Best)
            continue;
        Best = Far;
        X    = PX;
        Y    = PY;
        bHit = 1;
        if (Z)
            *Z = S.Z;
    }
    return bHit;
}

// Who was in the water last frame, for the splashes: a small table by actor - compared only, never followed (an actor may
// be gone by the next frame), and emptied with the level. A place not written last frame is free again.
struct FWetMark
{
    const AActor* Key;
    INT           Frame;
    UBOOL         bWet;
};
static FWetMark GWetMarks[1024];
static INT      GWetFrame  = 1;
static ULevel*  GWetLevel  = NULL;            // compared only, never followed
static INT      GWetTold   = 0;

INT RdxWaterSplashes(const FSceneNode* Frame, FLOAT* Out, INT Room)
{
    if (!Frame || Frame->Parent || !Frame->Level || !GAnyLevelWater || GForgotten || Room <= 0)
        return 0;
    ULevel* Level = Frame->Level;
    if (GWetLevel != Level)
    {
        GWetLevel = Level;
        appMemzero(GWetMarks, sizeof(GWetMarks));
        GWetFrame = 1;
        GWetTold  = 0;
    }
    GWetFrame++;
    const FVector Cam    = Frame->Coords.Origin;
    const AActor* Viewer = (Frame->Viewport) ? Frame->Viewport->Actor : NULL;
    INT N = 0;
    for (INT i = 0; i < Level->Actors.Num(); i++)
    {
        const AActor* A = Level->Actors(i);
        if (!A || A->bDeleteMe || A->CollisionRadius <= 0.0f || (A->bHidden && A != Viewer))
            continue;
        if (Abs(A->Location.X - Cam.X) > 3000.0f || Abs(A->Location.Y - Cam.Y) > 3000.0f)
            continue;
        // Only what is over or in a level water surface's box: nothing else can fall into one.
        UBOOL bNear = 0;
        for (INT k = 0; k < GSurfaceCount && !bNear; k++)
        {
            const FWaterSurface& S = GSurfaces[k];
            bNear = A->Location.X > S.MinX - 64.0f && A->Location.X < S.MaxX + 64.0f &&
                    A->Location.Y > S.MinY - 64.0f && A->Location.Y < S.MaxY + 64.0f;
        }
        if (!bNear)
            continue;
        // In the water: its zone is a liquid's with a surface where it is - the one nearest it in height.
        const INT   Z    = A->Region.ZoneNumber;
        const INT   At   = (Z > 0 && Z < FBspNode::MAX_ZONES) ? SurfaceNear(Z, A->Location, 64.0f) : -1;
        const UBOOL bWet = (At >= 0) ? 1 : 0;
        // Its mark: found along its run of places, or the first free one there.
        const DWORD H    = ((DWORD)(size_t)A >> 4) * 2654435761u;
        FWetMark*   Mark = NULL;
        FWetMark*   Free = NULL;
        for (INT p = 0; p < 16 && !Mark; p++)
        {
            FWetMark& M = GWetMarks[(H + p) & 1023];
            if (M.Key == A)
                Mark = &M;
            else if (!Free && M.Frame < GWetFrame - 1)
                Free = &M;
        }
        const UBOOL bWasDry = Mark && Mark->Frame == GWetFrame - 1 && !Mark->bWet;
        if (!Mark)
            Mark = Free;
        if (Mark)
        {
            Mark->Key   = A;
            Mark->Frame = GWetFrame;
            Mark->bWet  = bWet;
        }
        if (!bWasDry || !bWet || -A->Velocity.Z < RdxMath::SplashMinFall || N >= Room)
            continue;
        FLOAT* const O = Out + N * 5;
        O[0] = A->Location.X;
        O[1] = A->Location.Y;
        O[2] = GSurfaces[At].Z;
        O[3] = A->CollisionRadius;
        O[4] = -A->Velocity.Z;
        N++;
        if (GWetTold++ < 8)
            RDX_LOGF("water: a splash - %s went in falling %.0f units a second (radius %.0f)", A->GetName(), -A->Velocity.Z,
                     A->CollisionRadius);
    }
    return N;
}

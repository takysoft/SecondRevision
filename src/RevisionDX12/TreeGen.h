// SecondRevision - trees and bushes grown by code (Trees3D, docs/terv-novenyzet-szel.md 6).
//
// A plant is grown into the room the game's own one takes up: its crown is a volume of that height and width, filled
// with points that pull the branches towards them - space colonization (Runions, Lane and Prusinkiewicz, "Modeling
// Trees with a Space Colonization Algorithm", 2007). Every point pulls the branch end nearest to it, every pulled end
// grows a step towards the average of its pulls, and a point a branch has come near is spent. The branches thicken
// from the twigs down, a parent as thick as its children together (the pipe model); they become tubes of bark, and
// the thin ends carry the leaves, a blade each. The crown shades itself, each way (FOpenness). Engine-independent, as
// RenderMath.h: the renderer, the tests and tools\TreeCheck grow the same plant from the same numbers.
#pragma once

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace RdxTree
{

// ---------------------------------------------------------------------------------------------------------------
// Small things
// ---------------------------------------------------------------------------------------------------------------

struct FVec
{
    float X, Y, Z;
};
inline FVec Vec(float X, float Y, float Z)
{
    FVec V = { X, Y, Z };
    return V;
}
inline FVec operator+(FVec A, FVec B) { return Vec(A.X + B.X, A.Y + B.Y, A.Z + B.Z); }
inline FVec operator-(FVec A, FVec B) { return Vec(A.X - B.X, A.Y - B.Y, A.Z - B.Z); }
inline FVec operator*(FVec A, float S) { return Vec(A.X * S, A.Y * S, A.Z * S); }
inline float Dot(FVec A, FVec B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
inline FVec Cross(FVec A, FVec B)
{
    return Vec(A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X);
}
inline float Length(FVec A) { return sqrtf(Dot(A, A)); }
// A unit vector the same way, or Fallback for one with no length.
inline FVec Unit(FVec A, FVec Fallback)
{
    const float L = Length(A);
    return (L > 1e-6f) ? A * (1.0f / L) : Fallback;
}

// A growing list of plain data.
template <typename T>
struct TList
{
    T*  Data = nullptr;
    int Num  = 0;
    int Room = 0;

    TList() {}
    ~TList() { free(Data); }
    TList(const TList&)            = delete;
    TList& operator=(const TList&) = delete;

    bool Reserve(int Want)
    {
        if (Want <= Room)
            return true;
        int More = Room ? Room : 64;
        while (More < Want)
            More *= 2;
        T* const Grown = (T*)realloc(Data, (size_t)More * sizeof(T));
        if (!Grown)
            return false;
        Data = Grown;
        Room = More;
        return true;
    }
    bool Add(const T& V)
    {
        if (Num == Room && !Reserve(Num + 1))
            return false;
        Data[Num++] = V;
        return true;
    }
    void Clear() { Num = 0; }
    T&       operator[](int i) { return Data[i]; }
    const T& operator[](int i) const { return Data[i]; }
};

// The plant's own random numbers: one seed, the same plant.
struct FRandom
{
    unsigned int State;
    explicit FRandom(unsigned int Seed) : State(Seed * 0x9E3779B9u + 0x7F4A7C15u)
    {
        if (!State)
            State = 0x6C8E9CF5u;
    }
    unsigned int Next()
    {
        State ^= State << 13;
        State ^= State >> 17;
        State ^= State << 5;
        return State;
    }
    float Unit() { return (float)(Next() & 0xFFFFFFu) / 16777216.0f; }      // 0 .. 1
    float Range(float A, float B) { return A + (B - A) * Unit(); }
    // A direction, evenly over the sphere.
    FVec Direction()
    {
        const float Z = Range(-1.0f, 1.0f);
        const float A = Range(0.0f, 6.2831853f);
        const float R = sqrtf(1.0f - Z * Z);
        return Vec(R * cosf(A), R * sinf(A), Z);
    }
};

inline unsigned int Hash(unsigned int X)
{
    X ^= X >> 16;
    X *= 0x7feb352dU;
    X ^= X >> 15;
    X *= 0x846ca68bU;
    X ^= X >> 16;
    return X;
}

// ---------------------------------------------------------------------------------------------------------------
// The shape: what kind of plant, and how it grows
// ---------------------------------------------------------------------------------------------------------------

// The game's own plants (docs/terv-novenyzet-szel.md 2.2), each with a growth of its own, so that a level has more than
// one kind of tree standing in it.
enum EKind
{
    Kind_Tree1, Kind_Tree2, Kind_Tree3, Kind_Tree4, Kind_Bush1, Kind_Bush2, Kind_Bush3,
    Kind_Small,         // a small tree of the level's own, Revision's crossed foliage sheets, grown again
    Kind_Rose,          // the long-stemmed flowers of a bed in the level's sheets, grown again as a shrub rose
    Kind_RoseYellow,    // the bed's yellow ones: the same shrub rose, its petals yellow (DrawRoseYellowPetals)
    Kind_Lily,          // the bed's orange lilies (pflanzentex149): leafy stems, a flower of six petals at each top
    Kind_Pink,          // the bed's pale pink flowers (pflanzentex148): long thin stems among grassy leaves
    Kind_Fern,          // a bed's fern (pflanzentex143, 145, 146): fronds arching out from its foot
    Kind_Count
};
// A shrub rose: canes from its foot, its leaves of five leaflets each, its flowers at the canes' ends (RoseLeaves).
inline bool IsRose(EKind Kind) { return Kind == Kind_Rose || Kind == Kind_RoseYellow; }
// A bed's flower or fern grown stem by stem (StemPlant), not into a crown - its leaves and petals in a sheet of their own
// (DrawStemSheet).
inline bool IsStemPlant(EKind Kind) { return Kind == Kind_Lily || Kind == Kind_Pink || Kind == Kind_Fern; }
inline bool IsBush(EKind Kind)
{
    return (Kind >= Kind_Bush1 && Kind <= Kind_Bush3) || IsRose(Kind) || IsStemPlant(Kind);
}

struct FShape
{
    EKind Kind;
    float Height;       // from the ground to the top, level units
    float Radius;       // the crown's widest half-width
    float CrownBase;    // where the crown begins, of the height
    float Widest;       // where it is widest, of the crown's own height
    float Lobes;        // how uneven its outline is: 0 an even ellipsoid
    int   Points;       // the points that pull the branches
    float Step;         // a branch segment's length
    float Reach;        // how far a point pulls
    float Spend;        // how near a branch comes to a point to spend it
    float Up;           // how much a growing end leans upwards besides its pulls
    float Twig;         // a twig's radius
    float Pipe;         // the pipe model's exponent: a parent's radius to it is the sum of its children's
    int   Stems;        // stems from the ground: a tree's one trunk, a bush's several
    float LeafSize;     // a leaf's length, its stalk with it
    float LeafEvery;    // side twigs a twig's tip puts out, on average (two along a twig)
    float Lean;         // the trunk's own lean from upright, radians at most
    float Box;          // 1: trimmed to a box, as a hedge is - flat faces, its leaves thick at them; 0: grown free
    bool  Cards;        // its leaves are EZ-Tree's cards, the renderer having their sheet: a hedge's skin is laid in them
    // The game's own plant's trunk: its radius at each tenth of the height (0: unknown). The grown tree's foot is as
    // thick as the lowest tenth's.
    float Bark[10];
};

// The shape of a kind of plant of the given height and crown half-width (level units) - the numbers the renderer reads
// off the game's own plant, or the class's sizes where it has nothing better.
inline FShape ShapeOf(EKind Kind, float Height, float Radius)
{
    FShape S;
    S.Kind   = Kind;
    S.Height = (Height > 8.0f) ? Height : 8.0f;
    S.Radius = (Radius > 2.0f) ? Radius : 2.0f;
    S.Box    = 0.0f;
    S.Cards  = false;
    switch (Kind)
    {
        case Kind_Tree1:    // a smaller broadleaf, round
            S.CrownBase = 0.36f; S.Widest = 0.45f; S.Lobes = 0.20f; S.Up = 0.25f; S.Lean = 0.05f;
            break;
        case Kind_Tree2:    // the tall one, an oval crown - the commonest
            S.CrownBase = 0.30f; S.Widest = 0.42f; S.Lobes = 0.18f; S.Up = 0.32f; S.Lean = 0.04f;
            break;
        case Kind_Tree3:    // low and broad
            S.CrownBase = 0.26f; S.Widest = 0.55f; S.Lobes = 0.24f; S.Up = 0.18f; S.Lean = 0.07f;
            break;
        case Kind_Tree4:    // the big one, wide
            S.CrownBase = 0.28f; S.Widest = 0.48f; S.Lobes = 0.22f; S.Up = 0.22f; S.Lean = 0.05f;
            break;
        case Kind_Bush1:    // the game's box hedge: trimmed square on its stems
            S.CrownBase = 0.04f; S.Widest = 0.50f; S.Lobes = 0.0f; S.Up = 0.20f; S.Lean = 0.30f; S.Box = 1.0f;
            break;
        case Kind_Bush2:    // a taller, narrower one
            S.CrownBase = 0.06f; S.Widest = 0.55f; S.Lobes = 0.20f; S.Up = 0.35f; S.Lean = 0.25f;
            break;
        case Kind_Small:    // a small tree in a bed: a slim trunk, a light round crown
            S.CrownBase = 0.35f; S.Widest = 0.50f; S.Lobes = 0.20f; S.Up = 0.30f; S.Lean = 0.06f;
            break;
        case Kind_Rose:     // a shrub rose: canes leaning out from its foot, a low round head, uneven
        case Kind_RoseYellow:
            S.CrownBase = 0.08f; S.Widest = 0.55f; S.Lobes = 0.30f; S.Up = 0.55f; S.Lean = 0.35f;
            break;
        case Kind_Lily:     // stems from one foot, laid one by one (StemPlant): no crown to fill
        case Kind_Pink:
        case Kind_Fern:
            S.CrownBase = 0.05f; S.Widest = 0.60f; S.Lobes = 0.0f; S.Up = 0.50f; S.Lean = 0.20f;
            break;
        default:            // a low, spreading one
            S.CrownBase = 0.03f; S.Widest = 0.40f; S.Lobes = 0.26f; S.Up = 0.10f; S.Lean = 0.45f;
            break;
    }
    const bool bBush = IsBush(Kind);
    // The steps by the plant's size: a tree's crown some fifty steps tall, a bush's some twenty - its twigs as fine.
    S.Step      = S.Height / (bBush ? 22.0f : 55.0f);
    S.Reach     = S.Step * 9.0f;
    S.Spend     = S.Step * 2.0f;
    S.Twig      = S.Height * (bBush ? 0.0022f : 0.0016f);
    // A tree's trunk carries its crown: thicker than a bush's stems, which share theirs.
    S.Pipe      = bBush ? 2.5f : 2.1f;
    S.Stems     = bBush ? 5 : 1;
    // How many points: by the crown's volume in steps, held to what grows in a moment.
    const float Crown = (1.0f - S.CrownBase) * S.Height;
    const float Cells = 4.18879f * S.Radius * S.Radius * Crown * 0.5f / (S.Step * S.Step * S.Step);
    int Points = (int)(Cells * 0.012f);
    Points     = (Points < 300) ? 300 : ((Points > 1600) ? 1600 : Points);
    S.Points   = Points;
    S.LeafSize  = bBush ? S.Height * 0.065f : S.Height * 0.022f;
    S.LeafEvery = 7.0f;
    // A rose's leaflet is small, five of them to a leaf (RoseLeaves), and its leaves sparser than a bush's.
    if (IsRose(Kind))
    {
        S.LeafSize  = S.Height * 0.05f;
        S.LeafEvery = 3.0f;
    }
    // A trimmed hedge's leaves are small, as the cutting makes them (SkinLeaves lays them).
    if (S.Box > 0.0f)
        S.LeafSize *= 0.8f;
    for (int b = 0; b < 10; b++)
        S.Bark[b] = 0.0f;
    return S;
}

// The crown's half-width at a height (level units from the ground) and round the trunk at an angle: an ellipsoid widest
// at Widest, its outline lifted and lowered by a few lobes - the plant's own, from its seed.
struct FOutline
{
    float Lobe[4];      // each lobe's strength
    float Turn[4];      // where round the trunk it stands
    float Height[4];    // and at what height of the crown, 0 .. 1
};
inline FOutline OutlineOf(const FShape& S, FRandom& R)
{
    FOutline O;
    for (int k = 0; k < 4; k++)
    {
        O.Lobe[k]   = S.Lobes * R.Range(0.4f, 1.0f) * ((k & 1) ? -0.6f : 1.0f);
        O.Turn[k]   = R.Range(0.0f, 6.2831853f);
        O.Height[k] = R.Range(0.2f, 0.9f);
    }
    return O;
}
inline float CrownWidth(const FShape& S, const FOutline& O, float Z, float Angle)
{
    const float Base = S.CrownBase * S.Height;
    const float T    = (Z - Base) / ((1.0f - S.CrownBase) * S.Height);
    if (T <= 0.0f || T >= 1.0f)
        return 0.0f;
    // A trimmed hedge: a box with its edges rounded off, its faces square to the plant's own axes.
    if (S.Box > 0.0f)
    {
        const float C    = fabsf(cosf(Angle)), Sn = fabsf(sinf(Angle));
        const float Side = 1.0f / powf(powf(C, 8.0f) + powf(Sn, 8.0f), 0.125f);
        const float V    = fabsf(2.0f * T - 1.0f);
        return S.Radius * Side * powf(1.0f - powf(V, 10.0f), 0.1f);
    }
    const float W = S.Widest;
    const float D = (T < W) ? (W - T) / W : (T - W) / (1.0f - W);
    float R = S.Radius * sqrtf(1.0f - D * D);
    for (int k = 0; k < 4; k++)
    {
        const float A  = cosf(Angle - O.Turn[k]);
        const float H  = (T - O.Height[k]) * 3.0f;
        const float In = (A > 0.0f) ? A * A : 0.0f;
        R *= 1.0f + O.Lobe[k] * In * expf(-H * H);
    }
    return R;
}

// ---------------------------------------------------------------------------------------------------------------
// The skeleton
// ---------------------------------------------------------------------------------------------------------------

struct FNode
{
    FVec  At;           // in the plant's own space: Z up, the root's foot at the origin
    int   Parent;       // -1: a root
    int   Children;
    float Radius;
    int   Branch;       // the node its branch started at - a root, or a fork's child: what its sway's beat goes by
    FVec  Pull;         // this round's pulls, summed
    int   Pulls;
    int   Next;         // the next node in its cell of the grid
};

struct FLeaf
{
    FVec  At;           // its foot: where its stalk leaves the twig
    FVec  Face;         // the way its blade looks
    FVec  Along;        // from its foot to its tip, in the blade's plane
    float Size;         // from its foot to its tip, the stalk and the blade
    int   Picture;      // which of the leaf pictures, 0 .. 3
    int   Node;         // the twig it grows from
    float Hue;          // 0 its picture's own green .. 1 a spring yellow-green
    float Phase;        // its own flutter's phase, 0 .. 1
    float Fold;         // how far its halves rise from the midrib, of the blade's width: 0 flat .. 0.5
    float Droop;        // how far its tip hangs, of its length: 0 .. 0.4
};

// A shrub rose's flower (RoseLeaves): its head one sheet wound round its axis from the outermost petal in, the petals
// lobes of it (FlowerHeadsOf) - drawn as the wood is, it moves as one with the twig it ends.
struct FFlower
{
    FVec  At;           // its heart: the twig's end
    FVec  Up;           // the way it opens to
    FVec  Across;       // a way across it, at right angles to Up
    float Size;         // a petal's length
    float Spin;         // where its sheet starts round the axis, radians
    int   Node;         // the twig it ends
    int   Picture;      // the leaf sheet's petal picture it takes its colour from, 6 or 7
    bool  bBud;         // a bud: its petals closed
};

struct FPlant
{
    FShape         Shape;
    FOutline       Outline;
    TList<FNode>   Nodes;
    TList<FLeaf>   Leaves;
    TList<FFlower> Flowers;
};

// A rose's head (FlowerHeadsOf): one sheet swept round the flower's axis from its outermost petal (K = 0) to its
// innermost (K = 1), Turns times round, the petals lobes along it - RosePerTurn a turn, each as long as the flower's
// petal in its middle and three quarters of that at its edges, the rim scalloped. Across the sheet a petal runs from
// the heart (S = 0) out to its rim (S = 1), leaning out from the axis by Lean at the outermost petal and less by the
// turn towards the heart (by e to the minus LeanFall over the whole), the innermost upright; it is bowed out as a cup,
// by RoseCup of its length at its middle, the less the more upright it stands. A turn further in leans less, so it lies
// nearer the axis at every height: the sheet never goes through itself. A bud: fewer turns, all of them near upright.
static const float RoseTurns = 7.0f, RosePerTurn = 3.6f, RoseLean = 0.95f, RoseLeanFall = 2.5f, RoseCup = 0.5f;
static const float BudTurns = 3.0f, BudLean = 0.3f, BudLeanFall = 1.2f;
// The head's grid: steps along the sheet a petal, and across it from the heart to the rim.
enum { RosePetalSteps = 6, RoseAcrossSteps = 6 };
// A rose's leaves along a twig, on average (RoseLeaves) - fewer than a bush's two.
static const float RoseLeavesAlong = 1.2f;

// How much of its parent's girth a fork's thickest child keeps at least, a step (FShape::Step) on.
static const float LeaderKeeps = 0.88f;

// A tree's trunk at its foot, by its height: a park tree's is some twenty-fifth of its height across - a 7 m maple's
// 28 cm. The game's models' trunks, measured off their vertices, came out several times that (their lowest tenth holds
// more than the trunk), and grew baobabs.
inline float TrunkFoot(float Height) { return Height * 0.02f; }

// The nodes by place: cells of the pull's reach, hashed into a fixed table; a cell's nodes are a chain through
// FNode::Next. Two cells may share a slot - every candidate is measured anyway.
struct FGrid
{
    enum { Slots = 8192 };
    int   Head[Slots];
    float Cell;

    void Reset(float InCell)
    {
        Cell = InCell;
        memset(Head, 0xFF, sizeof(Head));
    }
    int CellOf(float V) const { return (int)floorf(V / Cell); }
    static unsigned int Slot(int X, int Y, int Z)
    {
        return ((unsigned int)X * 73856093u ^ (unsigned int)Y * 19349663u ^ (unsigned int)Z * 83492791u) &
               (unsigned int)(Slots - 1);
    }
    void Insert(TList<FNode>& Nodes, int i)
    {
        const unsigned int S = Slot(CellOf(Nodes[i].At.X), CellOf(Nodes[i].At.Y), CellOf(Nodes[i].At.Z));
        Nodes[i].Next = Head[S];
        Head[S]       = i;
    }
    // The node nearest to P within Within, or -1; its distance in Dist.
    int Nearest(const TList<FNode>& Nodes, FVec P, float Within, float& Dist) const
    {
        const int Cx = CellOf(P.X), Cy = CellOf(P.Y), Cz = CellOf(P.Z);
        int   Best  = -1;
        float Best2 = Within * Within;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    for (int i = Head[Slot(Cx + dx, Cy + dy, Cz + dz)]; i >= 0; i = Nodes[i].Next)
                    {
                        const FVec  D  = Nodes[i].At - P;
                        const float D2 = Dot(D, D);
                        if (D2 < Best2)
                        {
                            Best2 = D2;
                            Best  = i;
                        }
                    }
        Dist = sqrtf(Best2);
        return Best;
    }
};

inline int AddNode(FPlant& P, FGrid& Grid, FVec At, int Parent)
{
    FNode N;
    N.At       = At;
    N.Parent   = Parent;
    N.Children = 0;
    N.Radius   = 0.0f;
    N.Branch   = -1;
    N.Pull     = Vec(0.0f, 0.0f, 0.0f);
    N.Pulls    = 0;
    N.Next     = -1;
    if (!P.Nodes.Add(N))
        return -1;
    const int i = P.Nodes.Num - 1;
    if (Parent >= 0)
        P.Nodes[Parent].Children++;
    Grid.Insert(P.Nodes, i);
    return i;
}

// The plant grown: its skeleton with the radii and the branches' beats, and its leaves. False if memory ran out.
// Grid: room the caller keeps (it is large), reused from plant to plant.
// A trimmed hedge's leaves are its faces: laid over them thick - its four sides and its top - each lying on its face a
// little in or out of it, its tip out a little, its blade turned out of the face, each a little its own way; and each on
// the twig nearest it, for its sway. The twigs inside bear none: nothing of them shows. In cards (S.Cards), twigs of
// small round leaves, in three layers - at the face, thick, and two further in, thinner - so that the hedge is dense to
// the eye, not a shell with nothing in it.
inline bool SkinLeaves(const FShape& S, FRandom& R, FPlant& Out, const FGrid& Grid)
{
    const float Base  = S.CrownBase * S.Height;
    const float Crown = S.Height - Base;
    const float Half  = S.Radius;
    const float Sides = 8.0f * Half * Crown;
    const float Lid   = 4.0f * Half * Half;
    const bool  bCards = S.Cards;
    const float Size0  = bCards ? S.Height * 0.085f : S.LeafSize;
    const float Each   = bCards ? 0.6f * Size0 * Size0 : 0.25f * S.LeafSize * S.LeafSize;
    const int   First  = (int)((Sides + Lid) * (bCards ? 3.0f : 1.4f) / Each);
    int Count = bCards ? First * 24 / 10 : First;
    Count = (Count > 20000) ? 20000 : Count;
    static const float In0[3] = { -0.15f, -0.7f, -1.4f }, In1[3] = { 0.1f, -0.35f, -0.9f };
    for (int k = 0; k < Count; k++)
    {
        const int Layer = !bCards ? 0 : ((k < First) ? 0 : ((k < First + First * 8 / 10) ? 1 : 2));
        FVec P, Nrm;
        if (R.Unit() * (Sides + Lid) < Lid)
        {
            const float X = R.Range(-Half, Half), Y = R.Range(-Half, Half);
            const float Z = S.Height - 0.03f * Crown;
            if (sqrtf(X * X + Y * Y) > CrownWidth(S, Out.Outline, Z, atan2f(Y, X)))
                continue;
            P   = Vec(X, Y, Z);
            Nrm = Vec(0.0f, 0.0f, 1.0f);
        }
        else
        {
            const unsigned int F = R.Next() & 3u;
            Nrm = (F == 0u) ? Vec(1.0f, 0.0f, 0.0f) : (F == 1u) ? Vec(-1.0f, 0.0f, 0.0f)
                : (F == 2u) ? Vec(0.0f, 1.0f, 0.0f) : Vec(0.0f, -1.0f, 0.0f);
            const FVec  Flat = Nrm * Half + Vec(-Nrm.Y, Nrm.X, 0.0f) * R.Range(-Half, Half);
            const float Z    = R.Range(Base, S.Height);
            const float W    = CrownWidth(S, Out.Outline, Z, atan2f(Flat.Y, Flat.X));
            const float Len  = sqrtf(Flat.X * Flat.X + Flat.Y * Flat.Y);
            if (W <= 0.0f || Len <= 0.0f)
                continue;
            P = Vec(Flat.X * W / Len, Flat.Y * W / Len, Z);
        }
        P = P + Nrm * (bCards ? R.Range(In0[Layer], In1[Layer]) * Size0 : R.Range(-0.35f, 0.12f) * S.LeafSize);
        FLeaf L;
        FVec  Along = R.Direction();
        Along       = Along - Nrm * Dot(Along, Nrm);
        L.Along     = Unit(Unit(Along, Vec(0.0f, 0.0f, 1.0f)) + Nrm * 0.35f, Nrm);
        L.Size      = Size0 * R.Range(0.8f, 1.2f);
        L.At        = P - L.Along * (0.5f * L.Size);
        FVec Face   = Nrm + R.Direction() * 0.35f;
        Face        = Face - L.Along * Dot(Face, L.Along);
        L.Face      = Unit(Face, Nrm);
        L.Picture   = bCards ? 2 : (int)(R.Next() & 3u);
        float Dist  = 0.0f;
        const int Near = Grid.Nearest(Out.Nodes, P, S.Reach, Dist);
        L.Node      = (Near >= 0) ? Near : 0;
        L.Hue       = (R.Unit() < 0.1f) ? R.Range(0.4f, 0.8f) : R.Range(0.0f, 0.25f);
        L.Phase     = R.Unit();
        L.Fold      = bCards ? 0.0f : R.Range(0.15f, 0.35f);
        L.Droop     = bCards ? 0.0f : R.Range(0.0f, 0.12f);
        if (!Out.Leaves.Add(L))
            return false;
    }
    return true;
}

// A shrub rose's leaves and flowers (IsRose), on the thin twigs as a tree's leaves are. Each side twig is a leaf's stalk:
// two pairs of leaflets along it and one at its end, all five in one plane, turned to the sky. At the end of a twig high
// in the head, a flower more often than not, now and then a bud (FFlower; its head made by FlowerHeadsOf). The leaflets
// are the leaf sheet's pictures 4 and 5 (DrawLeaves), the flowers take their colour from its petals, 6 and 7; a rose's
// blades are as wide as long (the renderer's width share), its leaflets narrower within their pictures.
inline bool RoseLeaves(const FShape& S, FRandom& R, FPlant& Out, FGrid& Grid)
{
    const float Base  = S.CrownBase * S.Height;
    const int   Grown = Out.Nodes.Num;
    // The leaves as they are drawn, made after the flowers are known: a leaf any leaflet of which lies in a flower's
    // head - within the head's reach of where it sits and no lower than it, by the leaflet's own size - is left out (a
    // flower's twig puts out its own leaves at its tip, right in the head). Its stalk stays, bare: a stalk is drawn
    // under its leaves by no wood of its own (Build), but the twig it grows from is drawn on to its tip only for a
    // child there - without it, the twig under a flower would end short of it. The draws stay in their order.
    struct FStalk
    {
        int   Parent, Branch;
        FVec  End;
        FLeaf Leaflets[5];
    };
    TList<FStalk> Stalks;
    for (int i = 0; i < Grown; i++)
    {
        const FNode N = Out.Nodes[i];
        if (N.Radius > S.Twig * 3.5f || N.At.Z < Base * 0.85f || N.Parent < 0)
            continue;
        const FVec Twig = Unit(N.At - Out.Nodes[N.Parent].At, Vec(0.0f, 0.0f, 1.0f));
        const FVec Away = Unit(Vec(N.At.X, N.At.Y, 0.0f), R.Direction());
        if (N.Children == 0 && N.At.Z > S.Height * 0.55f && R.Unit() < 0.6f)
        {
            FFlower F;
            F.bBud    = R.Unit() < 0.25f;
            F.Size    = S.LeafSize * (F.bBud ? 1.1f : 2.4f) * R.Range(0.85f, 1.15f);
            F.Up      = Unit(Twig + Vec(0.0f, 0.0f, 0.6f), Twig);
            F.Across  = Unit(Cross(F.Up, Away), Unit(Cross(F.Up, Vec(1.0f, 0.0f, 0.0f)), Vec(0.0f, 1.0f, 0.0f)));
            F.At      = N.At;
            F.Spin    = R.Range(0.0f, 6.2831853f);
            F.Node    = i;
            F.Picture = 6 + (int)(R.Next() & 1u);
            if (!Out.Flowers.Add(F))
                return false;
        }
        // The leaves: side twigs, LeafEvery at a twig's tip and RoseLeavesAlong along it, out towards the head's
        // outside.
        float Count = (N.Children == 0) ? S.LeafEvery : RoseLeavesAlong;
        while (Count > 0.0f)
        {
            if (Count < 1.0f && R.Unit() > Count)
                break;
            Count -= 1.0f;
            const FVec  Dir  = Unit(Twig * 0.5f + Away * 0.7f + Vec(0.0f, 0.0f, 0.2f) + R.Direction() * 0.5f, Twig);
            const float Long = S.LeafSize * R.Range(2.0f, 2.6f);
            FStalk      St;
            St.Parent = i;
            St.Branch = Out.Nodes[i].Branch;
            St.End    = N.At + Dir * Long;
            int        Made   = 0;
            const FVec Across = Unit(Cross(Dir, Vec(0.0f, 0.0f, 1.0f)), Away);
            for (int k = 0; k <= 2; k++)
            {
                const bool  bEnd = (k == 2);
                const float T    = bEnd ? 1.0f : 0.35f + 0.3f * (float)k;
                for (int Hand = (bEnd ? 0 : -1); Hand <= (bEnd ? 0 : 1); Hand += 2)
                {
                    FLeaf L;
                    L.At      = N.At + Dir * (Long * T);
                    L.Along   = bEnd ? Unit(Dir + R.Direction() * 0.1f, Dir)
                                     : Unit(Across * (float)Hand + Dir * 0.45f + R.Direction() * 0.1f, Dir);
                    FVec Face = Vec(0.0f, 0.0f, 1.0f) + R.Direction() * 0.3f;
                    Face      = Face - L.Along * Dot(Face, L.Along);
                    L.Face    = Unit(Face, Unit(Cross(L.Along, Vec(1.0f, 0.0f, 0.0f)), Vec(0.0f, 1.0f, 0.0f)));
                    L.Size    = S.LeafSize * (bEnd ? 1.15f : 1.0f) * R.Range(0.85f, 1.1f);
                    L.Picture = 4 + (int)(R.Next() & 1u);
                    L.Node    = -1;
                    L.Hue     = R.Range(0.0f, 0.15f);
                    L.Phase   = R.Unit();
                    L.Fold    = R.Range(0.12f, 0.25f);
                    L.Droop   = R.Range(0.04f, 0.15f);
                    St.Leaflets[Made++] = L;
                }
            }
            if (!Stalks.Add(St))
                return false;
        }
    }
    for (int s = 0; s < Stalks.Num; s++)
    {
        const FStalk& St    = Stalks[s];
        bool          bHead = false;
        for (int k = 0; k < 5 && !bHead; k++)
            for (int f = 0; f < Out.Flowers.Num && !bHead; f++)
            {
                const FFlower& F    = Out.Flowers[f];
                const FVec     Off  = St.Leaflets[k].At - F.At;
                const float    Near = F.Size + St.Leaflets[k].Size;
                bHead = Dot(Off, Off) < Near * Near && Dot(Off, F.Up) > -St.Leaflets[k].Size;
            }
        const int Side = AddNode(Out, Grid, St.End, St.Parent);
        if (Side < 0)
            return false;
        Out.Nodes[Side].Radius = S.Twig * 1.1f;
        Out.Nodes[Side].Branch = St.Branch;
        for (int k = 0; k < 5 && !bHead; k++)
        {
            FLeaf L = St.Leaflets[k];
            L.Node  = Side;
            if (!Out.Leaves.Add(L))
                return false;
        }
    }
    return true;
}

// A stem of a plant grown stem by stem (StemPlant): from Foot along Dir (of length one), bowed towards Bend - Bow of its
// length off the straight line at its tip -, in Count segments, its radius from Thick at its foot to Thin at its tip; its
// nodes' numbers into Nodes (Count + 1), its foot a root and its whole the beat of its own sway. Thin at least a drawn
// twig's (Build draws a node with no child from 1.25 twigs on), so the stem reaches its flower. False if memory ran out.
inline bool AddStem(FPlant& Out, FGrid& Grid, FVec Foot, FVec Dir, FVec Bend, float Length, float Bow, int Count,
                    float Thick, float Thin, int* Nodes)
{
    int Last = -1;
    for (int k = 0; k <= Count; k++)
    {
        const float T = (float)k / (float)Count;
        const int   n = AddNode(Out, Grid, Foot + Dir * (Length * T) + Bend * (Length * Bow * T * T), Last);
        if (n < 0)
            return false;
        Out.Nodes[n].Radius = Thick + (Thin - Thick) * T;
        Out.Nodes[n].Branch = (Last < 0) ? n : Out.Nodes[Last].Branch;
        Nodes[k]            = n;
        Last                = n;
    }
    return true;
}
// A blade (a leaf, a frond's leaflet or a petal) of a plant grown stem by stem, at At on node Node, leaning off the way
// Way by Lean radians towards Out (square to Way), Size long; its face looks back at Way's side - a petal's in at the
// flower's axis, a leaf's up and in at its stem -, so its fold cups it that way and its droop turns its tip out and down;
// or, given Look, as near that way as its length lets it - a frond's leaflet lies in the frond, its face up.
inline bool AddBlade(FPlant& Out, FRandom& R, FVec At, FVec Way, FVec Out1, float Lean, float Size, int Picture, int Node,
                     float Hue, float Fold, float Droop, const FVec* Look = nullptr)
{
    FLeaf L;
    L.At      = At;
    L.Along   = Unit(Way * cosf(Lean) + Out1 * sinf(Lean), Way);
    L.Face    = Look ? Unit(*Look - L.Along * Dot(*Look, L.Along), Way)
                     : Unit(Way * sinf(Lean) - Out1 * cosf(Lean), Way);
    L.Size    = Size;
    L.Picture = Picture;
    L.Node    = Node;
    L.Hue     = Hue;
    L.Phase   = R.Unit();
    L.Fold    = Fold;
    L.Droop   = Droop;
    return Out.Leaves.Add(L);
}
// Two ways square to Up, and to each other.
inline void AcrossOf(FVec Up, FVec& A, FVec& B)
{
    A = Unit(Cross(Up, (fabsf(Up.Z) < 0.9f) ? Vec(0.0f, 0.0f, 1.0f) : Vec(1.0f, 0.0f, 0.0f)), Vec(1.0f, 0.0f, 0.0f));
    B = Cross(Up, A);
}
// A flower at the top of a stem (StemPlant): Count petals round its axis Up, each leaning out from it by Open radians -
// a lily's six, a trumpet, their tips turned back; a pink's five, open as a cup -, the stem's last node theirs.
inline bool AddFlower(FPlant& Out, FRandom& R, FVec At, FVec Up, int Count, float Open, float Size, int Picture,
                      int Node, float Fold, float Droop)
{
    FVec A, B;
    AcrossOf(Up, A, B);
    const float Spin = R.Range(0.0f, 6.2831853f);
    for (int k = 0; k < Count; k++)
    {
        const float Turn = Spin + 6.2831853f * ((float)k + R.Range(-0.08f, 0.08f)) / (float)Count;
        if (!AddBlade(Out, R, At, Up, A * cosf(Turn) + B * sinf(Turn), Open * R.Range(0.9f, 1.1f),
                      Size * R.Range(0.9f, 1.1f), Picture, Node, 0.0f, Fold, Droop))
            return false;
    }
    return true;
}

// A bed's flower or fern of the level's own sheets, grown again stem by stem (IsStemPlant) into the shape's height and
// half-width - the stems laid as they grow, each its own sway's beat, their leaves along them (DrawStemSheet's
// pictures; the petals 6 and 7, IsPetal, so a far plant keeps its flowers whole):
// - a lily (Kind_Lily): two to four leafy stems from one foot, leaning out a little, their leaves lance-shaped round
//   them a golden angle apart, up and out; at each top a flower of six petals as a trumpet, its petals' tips turned
//   back, facing out and up;
// - a pink (Kind_Pink): two or three long thin stems, grassy leaves arching from their lower part, at each top a
//   flower of five petals open as a cup;
// - a fern (Kind_Fern): seven to eleven fronds from the foot all round, each rising and arching out to its tip - its
//   leaflets in pairs along it from a little way up, longest a third of the way, short at the tip, a little forward.
inline bool StemPlant(const FShape& S, FRandom& R, FPlant& Out, FGrid& Grid)
{
    const float H = S.Height, W = S.Radius;
    int         Nodes[17];
    if (S.Kind == Kind_Fern)
    {
        const int Fronds = 7 + (int)(R.Next() % 5u);
        const float Spin = R.Range(0.0f, 6.2831853f);
        for (int f = 0; f < Fronds; f++)
        {
            const float Turn = Spin + 6.2831853f * ((float)f + R.Range(-0.3f, 0.3f)) / (float)Fronds;
            const FVec  Away = Vec(cosf(Turn), sinf(Turn), 0.0f);
            // Its tip out by a share of the half-width and up by one of the height, bowed down by a quarter of its
            // length: the length and the way out of the foot that put it there.
            const float Reach = W * R.Range(0.7f, 1.1f), Rise = H * R.Range(0.55f, 0.95f), Bow = 0.35f;
            const float Len   = (2.0f * Bow * Rise + sqrtf(4.0f * Bow * Bow * Rise * Rise +
                                                         4.0f * (1.0f - Bow * Bow) * (Rise * Rise + Reach * Reach))) /
                                (2.0f * (1.0f - Bow * Bow));
            const FVec Dir  = Unit(Away * Reach + Vec(0.0f, 0.0f, Rise + Bow * Len), Vec(0.0f, 0.0f, 1.0f));
            const FVec Down = Vec(0.0f, 0.0f, -1.0f);
            if (!AddStem(Out, Grid, Away * (W * 0.04f), Dir, Down, Len, Bow, 12, S.Twig * 1.6f, S.Twig * 1.3f, Nodes))
                return false;
            const int Pairs = 14 + (int)(R.Next() % 5u);
            for (int k = 0; k < Pairs; k++)
            {
                const float T    = 0.14f + 0.84f * ((float)k + 0.5f) / (float)Pairs;
                const FVec  At   = Away * (W * 0.04f) + Dir * (Len * T) + Down * (Len * Bow * T * T);
                const FVec  Way  = Unit(Dir + Down * (2.0f * Bow * T), Dir);
                const FVec  Side = Unit(Cross(Way, Vec(0.0f, 0.0f, 1.0f)), Vec(-Away.Y, Away.X, 0.0f));
                const float Long = Len * 0.13f * ((T < 0.33f) ? 0.5f + 0.5f * T / 0.33f
                                                               : 1.0f - 0.8f * powf((T - 0.33f) / 0.67f, 1.4f));
                const int   Node = Nodes[(int)(T * 12.0f)];
                // The frond's own plane's face, up.
                FVec Up = Cross(Way, Side);
                Up      = (Up.Z < 0.0f) ? Up * -1.0f : Up;
                for (int Hand = -1; Hand <= 1; Hand += 2)
                {
                    // Out to its side and a little forward: leaning off the way along the frond by most of a right
                    // angle, in the frond's plane.
                    if (!AddBlade(Out, R, At, Way, Side * (float)Hand, R.Range(0.95f, 1.15f), Long * R.Range(0.9f, 1.1f),
                                  3 + (int)(R.Next() % 3u), Node, R.Range(0.0f, 0.2f), R.Range(0.05f, 0.15f),
                                  R.Range(0.04f, 0.12f), &Up))
                        return false;
                }
            }
        }
        return true;
    }
    const bool bLily  = (S.Kind == Kind_Lily);
    const int  Stems  = bLily ? 2 + (int)(R.Next() % 3u) : 2 + (int)(R.Next() % 2u);
    const float Spin  = R.Range(0.0f, 6.2831853f);
    for (int t = 0; t < Stems; t++)
    {
        const float Turn = Spin + 6.2831853f * ((float)t + R.Range(-0.25f, 0.25f)) / (float)Stems;
        const FVec  Away = Vec(cosf(Turn), sinf(Turn), 0.0f);
        const FVec  Foot = Away * (W * R.Range(0.0f, bLily ? 0.2f : 0.15f));
        const FVec  Dir  = Unit(Vec(0.0f, 0.0f, 1.0f) + Away * R.Range(0.05f, bLily ? 0.2f : 0.15f), Away);
        const float Len  = H * R.Range(0.75f, 1.0f);
        const float Bow  = R.Range(0.03f, 0.08f);
        if (!AddStem(Out, Grid, Foot, Dir, Away, Len, Bow, 8, S.Twig * (bLily ? 2.0f : 1.5f),
                     S.Twig * (bLily ? 1.5f : 1.3f), Nodes))
            return false;
        auto WayAt = [&](float T) { return Unit(Dir + Away * (2.0f * Bow * T), Dir); };
        auto At    = [&](float T) { return Foot + Dir * (Len * T) + Away * (Len * Bow * T * T); };
        // Its leaves.
        const int   Leaves = bLily ? 8 + (int)(R.Next() % 4u) : 3 + (int)(R.Next() % 3u);
        float       Round  = R.Range(0.0f, 6.2831853f);
        for (int l = 0; l < Leaves; l++)
        {
            const float T   = bLily ? 0.12f + 0.7f * ((float)l + R.Range(0.0f, 0.6f)) / (float)Leaves
                                    : 0.02f + 0.4f * ((float)l + R.Range(0.0f, 0.6f)) / (float)Leaves;
            const FVec  Way = WayAt(T);
            FVec        A, B;
            AcrossOf(Way, A, B);
            Round += 2.39996f;
            const FVec Out1 = A * cosf(Round) + B * sinf(Round);
            const int  Node = Nodes[(int)(T * 8.0f)];
            if (bLily ? !AddBlade(Out, R, At(T), Way, Out1, R.Range(0.6f, 0.95f), H * 0.2f * (1.0f - 0.35f * T) *
                                  R.Range(0.85f, 1.1f), (int)(R.Next() & 1u), Node, R.Range(0.0f, 0.2f),
                                  R.Range(0.25f, 0.4f), R.Range(0.1f, 0.25f))
                      : !AddBlade(Out, R, At(T), Way, Out1, R.Range(0.25f, 0.5f), H * R.Range(0.28f, 0.42f), 2, Node,
                                  R.Range(0.05f, 0.35f), R.Range(0.1f, 0.2f), R.Range(0.25f, 0.4f)))
                return false;
        }
        // Its flower, facing out and up.
        const FVec Up = Unit(WayAt(1.0f) * (bLily ? 0.7f : 0.85f) + Away * (bLily ? 0.7f : 0.4f), Dir);
        if (bLily ? !AddFlower(Out, R, At(1.0f), Up, 6, 0.7f, H * 0.13f, 6, Nodes[8], 0.3f, 0.3f)
                  : !AddFlower(Out, R, At(1.0f), Up, 5, 1.15f, H * 0.075f, 7, Nodes[8], 0.2f, 0.12f))
            return false;
    }
    return true;
}

inline bool Grow(const FShape& S, unsigned int Seed, FPlant& Out, FGrid& Grid)
{
    FRandom R(Seed);
    Out.Shape   = S;
    Out.Outline = OutlineOf(S, R);
    Out.Nodes.Clear();
    Out.Leaves.Clear();
    Out.Flowers.Clear();
    Grid.Reset(S.Reach);
    if (IsStemPlant(S.Kind))
        return StemPlant(S, R, Out, Grid);

    // The points: evenly in the crown.
    TList<FVec> Points;
    if (!Points.Reserve(S.Points))
        return false;
    const float Base = S.CrownBase * S.Height;
    for (int Tries = 0; Points.Num < S.Points && Tries < S.Points * 40; Tries++)
    {
        const float Z = R.Range(Base, S.Height);
        const float A = R.Range(0.0f, 6.2831853f);
        // A trimmed hedge's points in a shell under its faces and over its top: what is cut back grows thick at the
        // cut, and little is left inside.
        if (S.Box > 0.0f)
        {
            const float W = CrownWidth(S, Out.Outline, Z, A);
            if (W <= 0.0f)
                continue;
            const bool  bTop = Z > S.Height - 0.2f * (S.Height - Base);
            const float Dt   = W * (bTop ? sqrtf(R.Unit()) : R.Range(0.7f, 1.0f));
            Points.Add(Vec(Dt * cosf(A), Dt * sinf(A), Z));
            continue;
        }
        const float D = R.Range(0.0f, S.Radius * 1.3f);
        // Evenly over the disc's area: a point at D is kept with the chance D over the disc's radius.
        if (R.Unit() * S.Radius * 1.3f > D)
            continue;
        if (D > CrownWidth(S, Out.Outline, Z, A))
            continue;
        Points.Add(Vec(D * cosf(A), D * sinf(A), Z));
    }
    TList<unsigned char> Live;
    if (!Live.Reserve(Points.Num))
        return false;
    for (int i = 0; i < Points.Num; i++)
        Live.Add(1);

    // The stems: a tree's trunk straight up from the origin, a little leaning; a bush's several, from round about it,
    // leaning outwards. Each grows until the crown's points reach it.
    const int MaxNodes = 12000;
    for (int s = 0; s < S.Stems; s++)
    {
        const float A    = R.Range(0.0f, 6.2831853f);
        const float Off  = (S.Stems > 1) ? R.Range(0.05f, 0.25f) * S.Radius : 0.0f;
        const float Lean = R.Range(0.3f, 1.0f) * S.Lean;
        const FVec  Dir  = Unit(Vec(sinf(Lean) * cosf(A), sinf(Lean) * sinf(A), cosf(Lean)), Vec(0.0f, 0.0f, 1.0f));
        int Tip = AddNode(Out, Grid, Vec(Off * cosf(A), Off * sinf(A), 0.0f), -1);
        if (Tip < 0)
            return false;
        for (int Steps = 0; Steps < 400; Steps++)
        {
            bool bReached = false;
            for (int i = 0; i < Points.Num && !bReached; i++)
                bReached = Length(Points[i] - Out.Nodes[Tip].At) < S.Reach;
            if (bReached || Out.Nodes[Tip].At.Z > S.Height)
                break;
            const FVec Wobble = R.Direction() * 0.08f;
            Tip = AddNode(Out, Grid, Out.Nodes[Tip].At + Unit(Dir + Wobble, Dir) * S.Step, Tip);
            if (Tip < 0)
                return false;
        }
    }

    // The colonization. A node, once grown, stays: the node nearest a point can only change to one grown since it was
    // last asked - so each point keeps its nearest, and a round measures it against the round's new nodes alone.
    TList<int>   Nearest;
    TList<float> NearDist;
    if (!Nearest.Reserve(Points.Num) || !NearDist.Reserve(Points.Num))
        return false;
    for (int p = 0; p < Points.Num; p++)
    {
        float Dist = S.Reach;
        Nearest.Add(Grid.Nearest(Out.Nodes, Points[p], S.Reach, Dist));
        NearDist.Add(Dist);
    }
    for (int Round = 0; Round < 400 && Out.Nodes.Num < MaxNodes; Round++)
    {
        for (int i = 0; i < Out.Nodes.Num; i++)
        {
            Out.Nodes[i].Pull  = Vec(0.0f, 0.0f, 0.0f);
            Out.Nodes[i].Pulls = 0;
        }
        int Pulling = 0;
        for (int p = 0; p < Points.Num; p++)
        {
            if (!Live[p] || Nearest[p] < 0)
                continue;
            if (NearDist[p] < S.Spend)
            {
                Live[p] = 0;
                continue;
            }
            FNode& N = Out.Nodes[Nearest[p]];
            N.Pull = N.Pull + (Points[p] - N.At) * (1.0f / NearDist[p]);
            N.Pulls++;
            Pulling++;
        }
        if (Pulling == 0)
            break;
        const int Before = Out.Nodes.Num;
        int       Grown  = 0;
        for (int i = 0; i < Before && Out.Nodes.Num < MaxNodes; i++)
        {
            if (Out.Nodes[i].Pulls == 0)
                continue;
            const FNode& N    = Out.Nodes[i];
            const FVec   From = (N.Parent >= 0) ? Unit(N.At - Out.Nodes[N.Parent].At, Vec(0.0f, 0.0f, 1.0f))
                                                : Vec(0.0f, 0.0f, 1.0f);
            // Pulls that cancel out (a point either side) leave the end growing on as it was.
            FVec Dir = (Length(N.Pull) < 0.1f * (float)N.Pulls) ? From : Unit(N.Pull, From);
            Dir      = Unit(Dir + Vec(0.0f, 0.0f, S.Up) + R.Direction() * 0.06f, Dir);
            const FVec At = N.At + Dir * S.Step;
            // Not where something already grew: two points in balance would grow the same twig over and over.
            float Near = 0.0f;
            if (Grid.Nearest(Out.Nodes, At, S.Step * 0.35f, Near) >= 0)
                continue;
            if (AddNode(Out, Grid, At, i) < 0)
                return false;
            Grown++;
        }
        if (Grown == 0)
            break;
        for (int p = 0; p < Points.Num; p++)
        {
            if (!Live[p])
                continue;
            for (int n = Before; n < Out.Nodes.Num; n++)
            {
                const FVec  D  = Out.Nodes[n].At - Points[p];
                const float D2 = Dot(D, D);
                if (D2 < NearDist[p] * NearDist[p])
                {
                    NearDist[p] = sqrtf(D2);
                    Nearest[p]  = n;
                }
            }
        }
    }

    // The radii, from the twigs down: a parent's to the pipe's power is the sum of its children's. A tree whose game
    // model has a trunk of its own (S.Bark) takes the power that makes its foot just as thick - its limbs thicker with
    // it, as a tree's are, rather than a thick trunk under a thin crown.
    float Pipe = S.Pipe;
    if (S.Stems == 1 && S.Bark[0] > S.Twig * 2.0f)
    {
        int Tips = 0;
        for (int i = 0; i < Out.Nodes.Num; i++)
            Tips += (Out.Nodes[i].Children == 0) ? 1 : 0;
        if (Tips > 1)
        {
            Pipe = logf((float)Tips) / logf(S.Bark[0] / S.Twig);
            Pipe = (Pipe < 1.4f) ? 1.4f : ((Pipe > 3.0f) ? 3.0f : Pipe);
        }
    }
    Out.Shape.Pipe = Pipe;
    TList<float> Sum;
    if (!Sum.Reserve(Out.Nodes.Num))
        return false;
    for (int i = 0; i < Out.Nodes.Num; i++)
        Sum.Add(0.0f);
    const float TwigPow = powf(S.Twig, Pipe);
    for (int i = Out.Nodes.Num - 1; i >= 0; i--)
    {
        if (Sum[i] <= 0.0f)
            Sum[i] = TwigPow;
        Out.Nodes[i].Radius = powf(Sum[i], 1.0f / Pipe);
        if (Out.Nodes[i].Parent >= 0)
            Sum[Out.Nodes[i].Parent] += Sum[i];
    }
    // A fork's thickest child narrows from its parent's girth over many steps, as a tree's leader does, rather than
    // at once: a trunk that sheds several limbs within a short way at its crown's foot would otherwise end there in a
    // steep cone - from below, a stump with its limbs stuck into the top.
    {
        TList<int> Thick;
        if (!Thick.Reserve(Out.Nodes.Num))
            return false;
        for (int i = 0; i < Out.Nodes.Num; i++)
            Thick.Add(-1);
        for (int i = 0; i < Out.Nodes.Num; i++)
        {
            const int Pa = Out.Nodes[i].Parent;
            if (Pa >= 0 && (Thick[Pa] < 0 || Out.Nodes[i].Radius > Out.Nodes[Thick[Pa]].Radius))
                Thick[Pa] = i;
        }
        for (int i = 0; i < Out.Nodes.Num; i++)
        {
            const int Pa = Out.Nodes[i].Parent;
            if (Pa >= 0 && Thick[Pa] == i && Out.Nodes[i].Radius < LeaderKeeps * Out.Nodes[Pa].Radius)
                Out.Nodes[i].Radius = LeaderKeeps * Out.Nodes[Pa].Radius;
        }
    }
    // The branches: a root starts one, and so does each child of a fork; the rest go on their parent's.
    for (int i = 0; i < Out.Nodes.Num; i++)
    {
        const int Pa = Out.Nodes[i].Parent;
        Out.Nodes[i].Branch = (Pa < 0 || Out.Nodes[Pa].Children > 1) ? i : Out.Nodes[Pa].Branch;
    }

    // The leaves, on the thin twigs in the crown. Each twig puts out short side twigs - LeafEvery at its tip, two along
    // it, fewer deep in the crown, where little light reaches - each a node of its own, out towards the crown's outside
    // and the sky; along each its leaves in pairs, each pair turned a quarter round from the last, and one at its end.
    // A leaf on its stalk: its tip out and a little up, its blade turned to the sky more often than not; each its own
    // green, its own flutter, its own fold and hang.
    if (S.Box > 0.0f)
        return SkinLeaves(S, R, Out, Grid);
    if (IsRose(S.Kind))
        return RoseLeaves(S, R, Out, Grid);
    const int Grown = Out.Nodes.Num;
    for (int i = 0; i < Grown; i++)
    {
        const FNode N = Out.Nodes[i];
        if (N.Radius > S.Twig * 3.5f || N.At.Z < Base * 0.85f || N.Parent < 0)
            continue;
        const float Width = CrownWidth(S, Out.Outline, N.At.Z, atan2f(N.At.Y, N.At.X));
        float       Shell = (Width > 1e-3f) ? sqrtf(N.At.X * N.At.X + N.At.Y * N.At.Y) / Width : 1.0f;
        Shell             = (Shell > 1.0f) ? 1.0f : Shell;
        const FVec  Twig  = Unit(N.At - Out.Nodes[N.Parent].At, Vec(0.0f, 0.0f, 1.0f));
        const FVec  Away  = Unit(Vec(N.At.X, N.At.Y, 0.0f), R.Direction());
        float       Count = ((N.Children == 0) ? S.LeafEvery : 2.0f) * (0.4f + 0.6f * Shell);
        while (Count > 0.0f)
        {
            if (Count < 1.0f && R.Unit() > Count)
                break;
            Count -= 1.0f;
            const FVec  Dir  = Unit(Twig * 0.6f + Away * 0.6f + Vec(0.0f, 0.0f, 0.35f) + R.Direction() * 0.6f, Twig);
            const float Long = S.LeafSize * R.Range(1.3f, 2.0f);
            const int   Side = AddNode(Out, Grid, N.At + Dir * Long, i);
            if (Side < 0)
                return false;
            Out.Nodes[Side].Radius = S.Twig * 1.3f;
            Out.Nodes[Side].Branch = Out.Nodes[i].Branch;
            const FVec Across = Unit(Cross(Dir, Vec(0.0f, 0.0f, 1.0f)), Away);
            const FVec Over   = Cross(Dir, Across);
            const int  Pairs  = (Long > S.LeafSize * 1.7f) ? 4 : 3;
            for (int k = 0; k <= Pairs; k++)
            {
                const bool  bEnd = (k == Pairs);
                const float T    = bEnd ? 1.0f : 0.22f + 0.7f * (float)k / (float)Pairs;
                const float Turn = (float)k * 1.5708f + R.Range(-0.3f, 0.3f);
                const FVec  Out1 = Across * cosf(Turn) + Over * sinf(Turn);
                for (int Hand = (bEnd ? 0 : -1); Hand <= (bEnd ? 0 : 1); Hand += 2)
                {
                    FLeaf L;
                    L.At    = N.At + Dir * (Long * T);
                    L.Along = bEnd ? Unit(Dir + R.Direction() * 0.2f, Dir)
                                   : Unit(Out1 * ((float)Hand * 0.85f) + Dir * 0.5f + Vec(0.0f, 0.0f, 0.25f) +
                                              R.Direction() * 0.25f, Dir);
                    FVec Face = Vec(0.0f, 0.0f, 1.0f) + R.Direction() * 0.5f;
                    Face      = Face - L.Along * Dot(Face, L.Along);
                    L.Face    = Unit(Face, Unit(Cross(L.Along, Vec(1.0f, 0.0f, 0.0f)), Vec(0.0f, 1.0f, 0.0f)));
                    L.Size    = S.LeafSize * R.Range(0.8f, 1.2f);
                    L.Picture = (int)(R.Next() & 3u);
                    L.Node    = Side;
                    L.Hue     = (R.Unit() < 0.15f) ? R.Range(0.5f, 1.0f) : R.Range(0.0f, 0.3f);
                    L.Phase   = R.Unit();
                    L.Fold    = R.Range(0.18f, 0.42f);
                    L.Droop   = R.Range(0.08f, 0.3f);
                    if (!Out.Leaves.Add(L))
                        return false;
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The mesh
// ---------------------------------------------------------------------------------------------------------------

struct FVertex
{
    float X, Y, Z;      // in the plant's own space
    float NX, NY, NZ;   // the surface's normal
    float U, V;         // bark: round and along, in the bark picture's repeats; a leaf: the leaves' sheet
    unsigned short Wind[8]; // its sway in the wind: its chain's sums (FSway, PackSway), as halfs - the eighth unused
    unsigned char Open[4];  // how open the plant is round it, each way (FOpenness, PackOpen)
};

// A leaf as the vertex shader reads it, one instance of the blade's frame (LeafCorner): 56 bytes.
struct FLeafInstance
{
    float          X, Y, Z;        // its foot, in the plant's own space
    float          Size;           // its length
    short          Face[4];        // its blade's normal (x y z, 32767 a whole); w unused
    short          Along[4];       // from its foot to its tip (x y z); w unused
    unsigned char  Picture;        // which of the eight pictures (low 3 bits), how far its tip hangs (high 5, 31 = 0.4)
    unsigned char  Hue;            // 0 its picture's own green .. 255 a spring yellow-green
    unsigned char  Phase;          // its own flutter's phase, 0 .. 255
    unsigned char  Fold;           // how far its halves rise, 255 = half the blade's width
    unsigned char  Open[4];        // how open the plant is round its blade, each way (FOpenness, PackOpen)
    unsigned short Wind[8];        // its foot's sway in the wind, its twig's (FSway, PackSway), as halfs
};

// The blade's frame: 3 x 4 corners - across it the left edge, the midrib and the right edge, along it from its foot to
// its tip in thirds - in 12 triangles; the corners by number, a row of three at a time from the foot. After it, from
// LeafFlat on, the blade flat: two triangles over its four outer corners, for a far plant, whose leaves' fold and hang
// are a pixel or two by then.
static const unsigned short LeafFrame[42] = { 0, 1, 3, 3, 1, 4, 1, 2, 4, 4, 2, 5, 3, 4, 6, 6, 4, 7,
                                              4, 5, 7, 7, 5, 8, 6, 7, 9, 9, 7, 10, 7, 8, 10, 10, 8, 11,
                                              0, 2, 9, 9, 2, 11 };
enum { LeafCorners = 12, LeafFrameIndices = 36, LeafFlat = 36, LeafFlatIndices = 6 };

struct FMesh
{
    TList<FVertex>       Bark;
    TList<unsigned int>  BarkIndex;
    TList<FLeafInstance> Leaf;
    // The flowers' heads (FlowerHeadsOf): drawn as the bark is, in the leaf sheet's petal pictures, both of their sides.
    TList<FVertex>       Petal;
    TList<unsigned int>  PetalIndex;
};

// A flower's petal among a plant's leaves: pictures 6 and 7 of the leaves' sheet, a shrub rose's (RoseLeaves).
inline bool IsPetal(int Picture) { return (Picture & 7) >= 6; }
// How many of a mesh's leaves are petals - the first so many of them (LeafInstancesOf lays them before the rest).
inline int PetalsOf(const FMesh& M)
{
    int n = 0;
    while (n < M.Leaf.Num && IsPetal(M.Leaf[n].Picture))
        n++;
    return n;
}

// A float as a half (IEEE 754 binary16, rounded to the nearest, the largest finite one at most), and back.
inline unsigned short HalfOf(float F)
{
    unsigned int Bits;
    memcpy(&Bits, &F, 4);
    const unsigned int Sign = (Bits >> 16) & 0x8000u;
    const int          Exp  = (int)((Bits >> 23) & 0xFFu) - 127 + 15;
    unsigned int       Man  = Bits & 0x7FFFFFu;
    if (Exp <= 0)
    {
        if (Exp < -10)
            return (unsigned short)Sign;
        Man |= 0x800000u;
        const unsigned int Shift = (unsigned int)(14 - Exp);
        unsigned int       H     = Man >> Shift;
        if ((Man >> (Shift - 1u)) & 1u)
            H++;
        return (unsigned short)(Sign | H);
    }
    if (Exp >= 31)
        return (unsigned short)(Sign | 0x7BFFu);
    unsigned int H = ((unsigned int)Exp << 10) | (Man >> 13);
    if (Man & 0x1000u)
        H++;
    if (H >= 0x7C00u)
        H = 0x7BFFu;
    return (unsigned short)(Sign | H);
}
inline float FloatOfHalf(unsigned short H)
{
    const unsigned int Exp = (H >> 10) & 0x1Fu, Man = H & 0x3FFu;
    float              F;
    if (Exp == 0)
        F = (float)Man / 16777216.0f;
    else
    {
        const unsigned int Bits = ((Exp == 31u) ? 0x7F800000u : ((Exp - 15u + 127u) << 23)) | (Man << 13);
        memcpy(&F, &Bits, 4);
    }
    return (H & 0x8000u) ? -F : F;
}

inline short Snorm16(float V)
{
    V = (V < -1.0f) ? -1.0f : ((V > 1.0f) ? 1.0f : V);
    return (short)(V * 32767.0f + ((V >= 0.0f) ? 0.5f : -0.5f));
}

// A corner of a leaf's blade, at rest, in the plant's own space - what the vertex shader does before the wind
// (TreeShader.h's VSLeaf): along it the length from the foot, across it half the length wide, each half rising from the
// midrib by the fold, and the whole hanging by the droop towards its tip; the normal of its half there, and its place on
// the leaves' sheet (eight pictures side by side, each a leaf from its foot at the bottom to its tip at the top).
// Width: the blade's width as a share of its length - a leaf's half, an EZ-Tree card's whole (GLight.z in the shader).
inline void LeafCorner(const FLeafInstance& I, int Corner, FVec& Pos, FVec& Normal, float& U, float& V,
                       float WidthShare = 0.5f)
{
    const FVec  Face  = Vec((float)I.Face[0], (float)I.Face[1], (float)I.Face[2]) * (1.0f / 32767.0f);
    const FVec  Along = Vec((float)I.Along[0], (float)I.Along[1], (float)I.Along[2]) * (1.0f / 32767.0f);
    const FVec  Side  = Cross(Face, Along);
    const float Width = I.Size * WidthShare;
    const float Fold  = (float)I.Fold / 255.0f * 0.5f;
    const float Droop = (float)(I.Picture >> 3) / 31.0f * 0.4f;
    const int   C     = Corner % 3, Row = Corner / 3;
    const float Uu    = (float)(C - 1) * 0.5f, T = (float)Row / 3.0f;
    const float Hand  = (float)(C - 1);
    Pos = Vec(I.X, I.Y, I.Z) + Along * (T * I.Size) + Side * (Uu * Width) +
          Face * (2.0f * fabsf(Uu) * Fold * Width - Droop * T * T * I.Size);
    const FVec DU = Side * Width + Face * (2.0f * Hand * Fold * Width);
    const FVec DT = Along * I.Size - Face * (2.0f * Droop * T * I.Size);
    Normal = Unit(Cross(DT, DU), Face);
    U      = ((float)(I.Picture & 7) + Uu + 0.5f) * 0.125f;
    V      = 1.0f - T;
}

// The wind's reach on a node: the whole plant sways from its foot, more the higher; a thin branch on its own besides.
inline float BendAt(const FPlant& P, const FNode& N)
{
    const FShape& S    = P.Shape;
    const float   T    = N.At.Z / S.Height;
    const float   Root = (P.Nodes.Num > 0) ? P.Nodes[0].Radius : S.Twig;
    const float   Thin = 1.0f - ((N.Radius < Root) ? N.Radius / Root : 1.0f);
    return S.Height * (IsBush(S.Kind) ? 0.03f : 0.012f) * T * T + S.Height * 0.012f * Thin * Thin;
}

inline float BeatOf(int Branch) { return (float)(Hash((unsigned int)Branch * 2654435761u) & 0xFFFFu) / 65536.0f; }

// A point's sway in the wind (TreeShader.h's Swayed): the sums over its chain - its own branch from that branch's foot,
// then each branch it grows from, at the node it grows from, down to the root's. Each link adds its bend - how far the
// wind moves it there at its strongest, level units - and its bend by its branch's beat's cosine and sine, at once,
// twice and three times (the sway's three paces): from those the shader makes, with the plant's own beat and the
// time, every link's sway summed. A branch's first node sways as the node it grows from - its foot keeps to it - and
// the further along the branch, the further its own sway takes it.
struct FSway
{
    float S0, C1, S1, C2, S2, C3, S3;
};
static const float BranchBend = 0.04f;      // a branch's bend at its tip, of its length

inline void AddSway(FSway& S, float W, float Beat)
{
    const float A = 6.2831853f * Beat;
    S.S0 += W;
    S.C1 += W * cosf(A);
    S.S1 += W * sinf(A);
    S.C2 += W * cosf(2.0f * A);
    S.S2 += W * sinf(2.0f * A);
    S.C3 += W * cosf(3.0f * A);
    S.S3 += W * sinf(3.0f * A);
}

inline FSway LerpSway(const FSway& A, const FSway& B, float T)
{
    FSway S;
    S.S0 = A.S0 + (B.S0 - A.S0) * T;
    S.C1 = A.C1 + (B.C1 - A.C1) * T;
    S.S1 = A.S1 + (B.S1 - A.S1) * T;
    S.C2 = A.C2 + (B.C2 - A.C2) * T;
    S.S2 = A.S2 + (B.S2 - A.S2) * T;
    S.C3 = A.C3 + (B.C3 - A.C3) * T;
    S.S3 = A.S3 + (B.S3 - A.S3) * T;
    return S;
}

inline void PackSway(const FSway& S, unsigned short* Out)
{
    Out[0] = HalfOf(S.S0);
    Out[1] = HalfOf(S.C1);
    Out[2] = HalfOf(S.S1);
    Out[3] = HalfOf(S.C2);
    Out[4] = HalfOf(S.S2);
    Out[5] = HalfOf(S.C3);
    Out[6] = HalfOf(S.S3);
    Out[7] = 0;
}

inline FSway SwayFrom(const unsigned short* In)
{
    FSway S;
    S.S0 = FloatOfHalf(In[0]);
    S.C1 = FloatOfHalf(In[1]);
    S.S1 = FloatOfHalf(In[2]);
    S.C2 = FloatOfHalf(In[3]);
    S.S2 = FloatOfHalf(In[4]);
    S.C3 = FloatOfHalf(In[5]);
    S.S3 = FloatOfHalf(In[6]);
    return S;
}

// Each node's sums. Its own branch's bend: the root's by its height, as the whole plant leans from its foot; any
// other's from its first node along it, growing as the square of the way out, its tip by BranchBend of its length.
inline bool SwayOf(const FPlant& P, TList<FSway>& Out)
{
    const int N = P.Nodes.Num;
    Out.Clear();
    TList<float> Along, Long;
    if (!Out.Reserve(N) || !Along.Reserve(N) || !Long.Reserve(N))
        return false;
    const FShape& S = P.Shape;
    const float   H = (S.Height > 1e-3f) ? S.Height : 1.0f;
    for (int i = 0; i < N; i++)
    {
        Along.Add(0.0f);
        Long.Add(0.0f);
    }
    for (int i = 0; i < N; i++)
    {
        const FNode& Nd    = P.Nodes[i];
        const int    First = (Nd.Branch >= 0 && Nd.Branch <= i) ? Nd.Branch : i;
        if (i != First && Nd.Parent >= 0 && Nd.Parent < i)
            Along[i] = Along[Nd.Parent] + Length(Nd.At - P.Nodes[Nd.Parent].At);
        Long[First] = (Along[i] > Long[First]) ? Along[i] : Long[First];
    }
    for (int i = 0; i < N; i++)
    {
        const FNode& Nd    = P.Nodes[i];
        const int    First = (Nd.Branch >= 0 && Nd.Branch <= i) ? Nd.Branch : i;
        const int    Foot  = P.Nodes[First].Parent;
        FSway        Sw    = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        float        W     = 0.0f;
        if (Foot < 0 || Foot >= i)
        {
            const float T = Nd.At.Z / H;
            W = H * (IsBush(S.Kind) ? 0.03f : 0.012f) * T * T;
        }
        else
        {
            Sw = Out[Foot];
            W  = BranchBend * Along[i] * Along[i] / ((Long[First] > 1e-3f) ? Long[First] : 1e-3f);
        }
        AddSway(Sw, W, BeatOf(First));
        if (!Out.Add(Sw))
            return false;
    }
    return true;
}

// The plant as the shadows see it (the renderer's casters, laid again in the wind every frame): few enough triangles
// for that. In the plant's own space, each part with the bend and the beat its drawing sways it by (TreeShader.h's
// Swayed).
enum { ShadowWoodMost = 160, ShadowLeavesMost = 320, ShadowLeavesAll = 2000, ShadowLeafGrowMost = 4 };
// A shrub rose's flowers in its shadow's shape (ShadowFlowers): at most so many, each of so many corners - and with the
// wood, the most corners a plant's shadow shape has.
enum { ShadowFlowerMost = 64, ShadowFlowerCorners = 8, ShadowCornersMost = ShadowWoodMost * 6 + ShadowFlowerMost * 8 };
struct FShadowCorner
{
    float X, Y, Z;
    FSway Sway;
};
struct FShadowLeaf
{
    float X, Y, Z;          // its foot
    FSway Sway;             // its twig's sway
    float Corner[4][3];     // its blade flat, from the foot: the foot's left and right, the tip's left and right
    float U[4], V[4];       // those corners' places on the leaves' sheet
};

// Its wood: the segments from a node to its parent, the ShadowWoodMost thickest - none thinner than a twentieth of the
// root - each a prism of three sides, its corners as far out as makes it as wide as the round branch on average, each
// ring swaying as its node does (SwayOf). Appended to Corners; Index three a face, counted from the first corner this
// call adds.
inline bool ShadowWood(const FPlant& P, TList<FShadowCorner>& Corners, TList<unsigned int>& Index)
{
    const int N = P.Nodes.Num;
    if (N < 2)
        return true;
    TList<FSway> Sway;
    if (!SwayOf(P, Sway))
        return false;
    const float Root = P.Nodes[0].Radius;
    int   Pick[ShadowWoodMost];
    float PickR[ShadowWoodMost];
    int   Picked = 0;
    for (int i = 1; i < N; i++)
    {
        const FNode& Nd = P.Nodes[i];
        if (Nd.Parent < 0 || Nd.Parent >= N)
            continue;
        const float R = P.Nodes[Nd.Parent].Radius;
        if (R < Root * 0.05f)
            continue;
        if (Picked < ShadowWoodMost)
        {
            Pick[Picked]  = i;
            PickR[Picked] = R;
            Picked++;
            continue;
        }
        int Least = 0;
        for (int k = 1; k < Picked; k++)
            if (PickR[k] < PickR[Least])
                Least = k;
        if (R > PickR[Least])
        {
            Pick[Least]  = i;
            PickR[Least] = R;
        }
    }
    const unsigned int First = (unsigned int)Corners.Num;
    for (int s = 0; s < Picked; s++)
    {
        const FNode& B   = P.Nodes[Pick[s]];
        const FNode& A   = P.Nodes[B.Parent];
        const FVec   D   = B.At - A.At;
        const float  L   = Length(D);
        if (L < 1e-3f)
            continue;
        const FVec   Dir = D * (1.0f / L);
        const FVec   E1  = Unit(Cross(Dir, (fabsf(Dir.Z) < 0.9f) ? Vec(0.0f, 0.0f, 1.0f) : Vec(1.0f, 0.0f, 0.0f)),
                                Vec(1.0f, 0.0f, 0.0f));
        const FVec   E2  = Cross(Dir, E1);
        const unsigned int Base = (unsigned int)Corners.Num - First;
        for (int r = 0; r < 2; r++)
        {
            const FNode& Nd   = r ? B : A;
            const float  Out  = 1.2f * Nd.Radius;
            for (int c = 0; c < 3; c++)
            {
                const float A3  = 2.0943951f * (float)c;
                const FVec  Pos = Nd.At + (E1 * cosf(A3) + E2 * sinf(A3)) * Out;
                FShadowCorner Cn;
                Cn.X = Pos.X; Cn.Y = Pos.Y; Cn.Z = Pos.Z;
                Cn.Sway = Sway[r ? Pick[s] : B.Parent];
                if (!Corners.Add(Cn))
                    return false;
            }
        }
        for (int c = 0; c < 3; c++)
        {
            const unsigned int A0 = Base + (unsigned int)c, A1 = Base + (unsigned int)((c + 1) % 3);
            const unsigned int B0 = A0 + 3u, B1 = A1 + 3u;
            if (!Index.Add(A0) || !Index.Add(A1) || !Index.Add(B1) || !Index.Add(A0) || !Index.Add(B1) ||
                !Index.Add(B0))
                return false;
        }
    }
    return true;
}

// Its flowers (FPlant::Flowers), the first ShadowFlowerMost, for its shadow: each head a closed bowl of
// ShadowFlowerCorners corners - its heart, a ring round its outermost petals' tips, and a top over its innermost - its
// faces those from the heart to the ring and from the ring to the top, all swaying as its twig's end does. Appended to
// Corners after the wood (ShadowWood); Index counted from the corner From, where the wood's start.
inline bool ShadowFlowers(const FPlant& P, TList<FShadowCorner>& Corners, TList<unsigned int>& Index, unsigned int From)
{
    if (P.Flowers.Num == 0)
        return true;
    TList<FSway> Sway;
    if (!SwayOf(P, Sway))
        return false;
    const int Ring = ShadowFlowerCorners - 2;
    for (int f = 0; f < P.Flowers.Num && f < ShadowFlowerMost; f++)
    {
        const FFlower& F    = P.Flowers[f];
        const float    Lean = F.bBud ? BudLean : RoseLean;
        const float    Out  = F.Size * sinf(Lean), Up = F.Size * cosf(Lean), Top = F.Size;
        const FVec     B    = Cross(F.Up, F.Across);
        const unsigned int Base = (unsigned int)Corners.Num - From;
        for (int c = 0; c < ShadowFlowerCorners; c++)
        {
            FVec Pos = F.At;
            if (c == Ring + 1)
                Pos = F.At + F.Up * Top;
            else if (c > 0)
            {
                const float A = 6.2831853f * (float)(c - 1) / (float)Ring;
                Pos           = F.At + (F.Across * cosf(A) + B * sinf(A)) * Out + F.Up * Up;
            }
            FShadowCorner Cn;
            Cn.X    = Pos.X;
            Cn.Y    = Pos.Y;
            Cn.Z    = Pos.Z;
            Cn.Sway = Sway[F.Node];
            if (!Corners.Add(Cn))
                return false;
        }
        for (int c = 0; c < Ring; c++)
        {
            const unsigned int R0 = Base + 1u + (unsigned int)c, R1 = Base + 1u + (unsigned int)((c + 1) % Ring);
            const unsigned int Apex = Base + (unsigned int)(Ring + 1);
            if (!Index.Add(Base) || !Index.Add(R1) || !Index.Add(R0) || !Index.Add(R0) || !Index.Add(R1) ||
                !Index.Add(Apex))
                return false;
        }
    }
    return true;
}

// A trimmed hedge's (FShape::Box): a box - its four sides and its top, none of it letting light through - round its
// leaves' feet, half a leaf in from them: the leaves outside it, lit, are its skin. Its top's corners sway as its
// highest leaf does, its foot's not at all. Appended as ShadowWood's are.
inline bool ShadowBox(const FMesh& M, TList<FShadowCorner>& Corners, TList<unsigned int>& Index)
{
    const int N = M.Leaf.Num;
    if (N <= 0)
        return true;
    float X0 = 1e30f, Y0 = 1e30f, X1 = -1e30f, Y1 = -1e30f, Z1 = -1e30f, Size = 0.0f;
    int   Top = 0;
    for (int l = 0; l < N; l++)
    {
        const FLeafInstance& I = M.Leaf[l];
        X0   = (I.X < X0) ? I.X : X0;
        X1   = (I.X > X1) ? I.X : X1;
        Y0   = (I.Y < Y0) ? I.Y : Y0;
        Y1   = (I.Y > Y1) ? I.Y : Y1;
        Top  = (I.Z > Z1) ? l : Top;
        Z1   = (I.Z > Z1) ? I.Z : Z1;
        Size += I.Size;
    }
    const FSway Still = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, High = SwayFrom(M.Leaf[Top].Wind);
    const float In = 0.5f * Size / (float)N;
    X0 += In;
    Y0 += In;
    X1 -= In;
    Y1 -= In;
    Z1 -= In;
    if (X1 <= X0 || Y1 <= Y0 || Z1 <= 0.0f)
        return true;
    for (int c = 0; c < 8; c++)
    {
        FShadowCorner Cn;
        Cn.X    = (c & 1) ? X1 : X0;
        Cn.Y    = (c & 2) ? Y1 : Y0;
        Cn.Z    = (c & 4) ? Z1 : 0.0f;
        Cn.Sway = (c & 4) ? High : Still;
        if (!Corners.Add(Cn))
            return false;
    }
    static const unsigned int Faces[30] = { 0, 1, 5, 0, 5, 4, 1, 3, 7, 1, 7, 5, 3, 2, 6, 3, 6, 7,
                                            2, 0, 4, 2, 4, 6, 4, 5, 7, 4, 7, 6 };
    for (int i = 0; i < 30; i++)
        if (!Index.Add(Faces[i]))
            return false;
    return true;
}

// Its leaves: the first ShadowLeavesMost of the mesh's - the share a far plant draws (its order is the leaves' own,
// thinned evenly), each larger by as much as the share is less of the whole, so the crown covers as much - each blade
// flat (LeafCorner's corners 0, 2, 9 and 11, LeafFrame's flat two triangles). A crown of many small leaves (the own
// way's) takes more of them, so that none grows past ShadowLeafGrowMost times its size - ShadowLeavesAll at most. A
// flower's petals (PetalsOf) all before them, each at its own size. Width: as LeafCorner's.
inline bool ShadowLeaves(const FMesh& M, float Width, TList<FShadowLeaf>& Out)
{
    const int Fixed = PetalsOf(M);
    const int All   = M.Leaf.Num - Fixed;
    const int Most  = ShadowLeafGrowMost * ShadowLeafGrowMost;
    int       Want  = (All + Most - 1) / Most;
    Want            = (Want < ShadowLeavesMost) ? ShadowLeavesMost : ((Want > ShadowLeavesAll) ? ShadowLeavesAll : Want);
    const int Count = (All < Want) ? All : Want;
    if (Fixed + Count <= 0)
        return true;
    const float Grow = (Count > 0) ? sqrtf((float)All / (float)Count) : 1.0f;
    static const int Picks[4] = { 0, 2, 9, 11 };
    for (int l = 0; l < Fixed + Count; l++)
    {
        const FLeafInstance& I = M.Leaf[l];
        const float          G = (l < Fixed) ? 1.0f : Grow;
        FShadowLeaf S;
        S.X    = I.X;
        S.Y    = I.Y;
        S.Z    = I.Z;
        S.Sway = SwayFrom(I.Wind);
        for (int k = 0; k < 4; k++)
        {
            FVec  Pos, Nrm;
            float U = 0.0f, V = 0.0f;
            LeafCorner(I, Picks[k], Pos, Nrm, U, V, Width);
            S.Corner[k][0] = (Pos.X - I.X) * G;
            S.Corner[k][1] = (Pos.Y - I.Y) * G;
            S.Corner[k][2] = (Pos.Z - I.Z) * G;
            S.U[k] = U;
            S.V[k] = V;
        }
        if (!Out.Add(S))
            return false;
    }
    return true;
}

// How open a place in the plant is to the light, each way. A crown shades itself: a leaf on its lamp's side takes the
// lamp's light whole, one deep inside or on the far side little. So the leaves' area is laid into a grid of cells over
// the plant, and from each cell's middle the light's way out is followed in fourteen directions - along the axes and
// the diagonals - through the leaves on the way, each stopping as much light as half its area does (the share a leaf
// turned any way shows: Beer's law in a canopy). What comes through is kept a cell as an average and a lean, the
// first two orders of spherical harmonics: how open the place is towards a direction D is Average + Lean . D, and the
// lean is in the plant's own space, so it turns with the plant as its normals do. Between the cells' middles it is
// blended.
enum { OpenCells = 16, OpenWays = 14 };

struct FOpenness
{
    FVec         Least;             // the grid's corner, in the plant's own space
    float        Cell = 1.0f;       // a cell's edge
    int          N[3] = { 1, 1, 1 };
    TList<float> Stop;              // a cell's light stopped per unit of length
    TList<float> Open;              // a cell's average and lean, four a cell

    int At(int X, int Y, int Z) const { return (Z * N[1] + Y) * N[0] + X; }
};

// The fourteen ways out, of length one: the axes, then the diagonals.
inline FVec OpenWay(int k)
{
    if (k < 6)
    {
        const float S = (k & 1) ? -1.0f : 1.0f;
        return (k < 2) ? Vec(S, 0.0f, 0.0f) : ((k < 4) ? Vec(0.0f, S, 0.0f) : Vec(0.0f, 0.0f, S));
    }
    const int   d = k - 6;
    const float R = 0.57735027f;
    return Vec((d & 1) ? -R : R, (d & 2) ? -R : R, (d & 4) ? -R : R);
}

// A leaf's blade's middle, where its share of the crown's shade is taken and given.
inline FVec LeafMiddle(const FLeaf& L) { return L.At + L.Along * (L.Size * 0.5f); }

inline bool MakeOpenness(const FPlant& P, FOpenness& O)
{
    FVec Lo = Vec(1e30f, 1e30f, 1e30f), Hi = Vec(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < P.Nodes.Num + P.Leaves.Num; i++)
    {
        const FVec Q = (i < P.Nodes.Num) ? P.Nodes[i].At : LeafMiddle(P.Leaves[i - P.Nodes.Num]);
        Lo = Vec((Q.X < Lo.X) ? Q.X : Lo.X, (Q.Y < Lo.Y) ? Q.Y : Lo.Y, (Q.Z < Lo.Z) ? Q.Z : Lo.Z);
        Hi = Vec((Q.X > Hi.X) ? Q.X : Hi.X, (Q.Y > Hi.Y) ? Q.Y : Hi.Y, (Q.Z > Hi.Z) ? Q.Z : Hi.Z);
    }
    if (Lo.X > Hi.X)
        return false;
    // Cubes, as many along the plant's longest way as fit with one to spare at each end.
    const FVec  Span = Hi - Lo;
    float       Most = (Span.X > Span.Y) ? Span.X : Span.Y;
    Most             = (Span.Z > Most) ? Span.Z : Most;
    O.Cell           = ((Most > 1.0f) ? Most : 1.0f) / (float)(OpenCells - 2);
    const float Spans[3] = { Span.X, Span.Y, Span.Z };
    for (int a = 0; a < 3; a++)
    {
        const int Want = (int)ceilf(Spans[a] / O.Cell) + 2;
        O.N[a]         = (Want < 2) ? 2 : ((Want > OpenCells) ? OpenCells : Want);
    }
    const FVec Mid = (Lo + Hi) * 0.5f;
    O.Least = Mid - Vec((float)O.N[0], (float)O.N[1], (float)O.N[2]) * (0.5f * O.Cell);
    const int Count = O.N[0] * O.N[1] * O.N[2];
    O.Stop.Clear();
    O.Open.Clear();
    if (!O.Stop.Reserve(Count) || !O.Open.Reserve(Count * 4))
        return false;
    for (int i = 0; i < Count; i++)
        O.Stop.Add(0.0f);
    // Each leaf's blade - a length by half of it, its outline filling some seven tenths of that - into the eight
    // cells round its middle, by how near it is to each.
    const float PerVolume = 1.0f / (O.Cell * O.Cell * O.Cell);
    for (int l = 0; l < P.Leaves.Num; l++)
    {
        const FLeaf& L    = P.Leaves[l];
        const float  Area = L.Size * L.Size * 0.5f * 0.7f;
        const FVec   G    = (LeafMiddle(L) - O.Least) * (1.0f / O.Cell) - Vec(0.5f, 0.5f, 0.5f);
        const int    X0 = (int)floorf(G.X), Y0 = (int)floorf(G.Y), Z0 = (int)floorf(G.Z);
        const float  Fx = G.X - (float)X0, Fy = G.Y - (float)Y0, Fz = G.Z - (float)Z0;
        for (int c = 0; c < 8; c++)
        {
            const int X = X0 + (c & 1), Y = Y0 + ((c >> 1) & 1), Z = Z0 + ((c >> 2) & 1);
            if (X < 0 || Y < 0 || Z < 0 || X >= O.N[0] || Y >= O.N[1] || Z >= O.N[2])
                continue;
            const float W = ((c & 1) ? Fx : 1.0f - Fx) * ((c & 2) ? Fy : 1.0f - Fy) * ((c & 4) ? Fz : 1.0f - Fz);
            O.Stop[O.At(X, Y, Z)] += 0.5f * Area * PerVolume * W;
        }
    }
    // From each cell's middle out, a cell's length a step, each cell met by its own share; its own half a cell's.
    for (int Z = 0; Z < O.N[2]; Z++)
        for (int Y = 0; Y < O.N[1]; Y++)
            for (int X = 0; X < O.N[0]; X++)
            {
                const float Own = O.Stop[O.At(X, Y, Z)] * 0.5f * O.Cell;
                float       Average = 0.0f;
                FVec        Lean    = Vec(0.0f, 0.0f, 0.0f);
                for (int k = 0; k < OpenWays; k++)
                {
                    const FVec Way = OpenWay(k);
                    float      Tau = Own;
                    for (int j = 1;; j++)
                    {
                        const int Sx = (int)floorf((float)X + 0.5f + Way.X * (float)j);
                        const int Sy = (int)floorf((float)Y + 0.5f + Way.Y * (float)j);
                        const int Sz = (int)floorf((float)Z + 0.5f + Way.Z * (float)j);
                        if (Sx < 0 || Sy < 0 || Sz < 0 || Sx >= O.N[0] || Sy >= O.N[1] || Sz >= O.N[2])
                            break;
                        Tau += O.Stop[O.At(Sx, Sy, Sz)] * O.Cell;
                    }
                    const float Through = expf(-Tau);
                    Average += Through;
                    Lean     = Lean + Way * Through;
                }
                // The ways are even over the sphere as far as the first two orders go: the average is the mean, the lean
                // three times the mean of each way by what comes through it.
                O.Open.Add(Average / (float)OpenWays);
                O.Open.Add(Lean.X * (3.0f / (float)OpenWays));
                O.Open.Add(Lean.Y * (3.0f / (float)OpenWays));
                O.Open.Add(Lean.Z * (3.0f / (float)OpenWays));
            }
    return true;
}

// How open the plant is at a place: the cells' middles round it blended; Out its average and lean.
inline void OpenAt(const FOpenness& O, FVec At, float Out[4])
{
    const FVec G = (At - O.Least) * (1.0f / O.Cell) - Vec(0.5f, 0.5f, 0.5f);
    float Gs[3]  = { G.X, G.Y, G.Z };
    int   I0[3];
    float F[3];
    for (int a = 0; a < 3; a++)
    {
        const float Most = (float)(O.N[a] - 1);
        Gs[a] = (Gs[a] < 0.0f) ? 0.0f : ((Gs[a] > Most) ? Most : Gs[a]);
        I0[a] = (int)floorf(Gs[a]);
        I0[a] = (I0[a] > O.N[a] - 2) ? O.N[a] - 2 : I0[a];
        F[a]  = Gs[a] - (float)I0[a];
    }
    Out[0] = Out[1] = Out[2] = Out[3] = 0.0f;
    for (int c = 0; c < 8; c++)
    {
        const float W = ((c & 1) ? F[0] : 1.0f - F[0]) * ((c & 2) ? F[1] : 1.0f - F[1]) * ((c & 4) ? F[2] : 1.0f - F[2]);
        const int   i = O.At(I0[0] + (c & 1), I0[1] + ((c >> 1) & 1), I0[2] + ((c >> 2) & 1)) * 4;
        for (int k = 0; k < 4; k++)
            Out[k] += O.Open[i + k] * W;
    }
}

inline unsigned char Byte(float V);
// The four as the shaders read them, 0 .. 1 a byte: the average as it is, the lean from -1 .. 1.
inline void PackOpen(const float Open[4], unsigned char Out[4])
{
    Out[0] = Byte(Open[0]);
    for (int k = 1; k < 4; k++)
        Out[k] = Byte(0.5f + 0.5f * Open[k]);
}

// The light that reaches even the crown's inside, passed on from leaf to leaf, as a share of what comes from a way: a
// place open towards that way by Open takes OpenFloor + (1 - OpenFloor) * Open of it (TreeShader.h's CubeLight). And
// OpenGain: a cell's own leaves count against each of them as if they all stood in front of it, and the two orders
// smooth the way out - so even the crown's outermost leaves come out at some seven tenths open; brought up by this
// much, they take the light whole and only the deeper ones lose it.
static const float OpenFloor = 0.3f, OpenGain = 1.5f;

// How open a packed place is towards a direction D (of length one): what the shaders do (TreeShader.h, CubeLight).
inline float OpenTowards(const unsigned char Open[4], FVec D)
{
    const float Lean[3] = { (float)Open[1] / 127.5f - 1.0f, (float)Open[2] / 127.5f - 1.0f, (float)Open[3] / 127.5f - 1.0f };
    const float V       = OpenGain * ((float)Open[0] / 255.0f + Lean[0] * D.X + Lean[1] * D.Y + Lean[2] * D.Z);
    return (V < 0.0f) ? 0.0f : ((V > 1.0f) ? 1.0f : V);
}

// Bark: each node with a parent a tube from the parent to it, round and along in the bark's repeats - whole repeats
// round, about BarkWorld level units each where its run begins, and along the wood as many to a unit of its length as
// round it at its girth there (RunV). The tubes go in runs: up the thickest child while the wood stays more than a
// quarter as thick as where the run began - a carved run only while it is thick enough to carve - each tube starting
// on its parent's end ring - the same ring, the same sides and repeats round, turned along without a twist - so a trunk
// is one surface. A run starts at a root, at a fork's other children and where the wood has thinned, on the parent's
// axis, so its first ring lies inside the parent's wood and the tube comes out through the parent's bark - a fork's
// other child no thicker there than SideStarts of the parent, lest its ring stand out of the parent's side. A run that
// ends on wood no run goes on from has its last ring closed by a low dome, which its children come out of. A run of
// thin wood is a smooth tube, as many sides as it is thick there (five to ten: fewer read as a lath up close); one of
// thick wood - from ReliefFrom on - is carved: rings ReliefStep apart along it and as many round, each point drawn in
// from its radius down into the bark's crevices (BarkHeight) as deep as the wood is thick, ReliefDepth of it, and
// turned by the bark's slope there. Leaves: an instance each of the blade's frame (LeafCorner). Every point carries how
// open the plant is round it (FOpenness). Plain (bPlain): no wood carved and no leaves; and with Least, only the wood
// at least that thick - the bark a far plant draws at the trees' lower levels (Trees3D).
static const float ReliefFrom = 5.0f, ReliefStep = 1.6f, ReliefDepth = 0.09f, SideStarts = 0.85f, TipShare = 0.35f;
// How far along its first tube a fork's other child's first ring may be lifted onto its wood's bark (Build's Lift).
static const float LiftMost = 0.6f;

// How thick a run starts on its parent: a little thicker than its own first node, as a branch is where it leaves, and
// no thicker than its parent - a fork's other child than SideStarts of it.
inline float RunStart(float Own, float Parent, bool bLeader)
{
    const float Want = Own * 1.08f, Most = Parent * (bLeader ? 1.0f : SideStarts);
    return (Want < Most) ? Want : Most;
}
inline float BarkHeight(float U, float V);
inline void  BarkSlope(float U, float V, float& PerU, float& PerV);
// The leaves as the vertex shader reads them (FLeafInstance), in a shuffled order: any first share of them is the whole
// crown, thinned evenly - a far plant draws only so many, each larger. A flower's petals before them all (PetalsOf):
// a far plant draws every one of those at its own size, the flowers whole, and thins only the leaves after them.
inline bool LeafInstancesOf(const FPlant& P, const FOpenness& Openness, FMesh& Out)
{
    if (!Out.Leaf.Reserve(P.Leaves.Num))
        return false;
    TList<int> Order, Laid;
    if (!Order.Reserve(P.Leaves.Num) || !Laid.Reserve(P.Leaves.Num))
        return false;
    for (int i = 0; i < P.Leaves.Num; i++)
        Order.Add(i);
    FRandom Mix(0x5EEDu ^ (unsigned int)P.Leaves.Num);
    for (int i = P.Leaves.Num - 1; i > 0; i--)
    {
        const int j  = (int)(Mix.Next() % (unsigned int)(i + 1));
        const int Kp = Order[i];
        Order[i]     = Order[j];
        Order[j]     = Kp;
    }
    for (int o = 0; o < P.Leaves.Num; o++)
        if (IsPetal(P.Leaves[Order[o]].Picture))
            Laid.Add(Order[o]);
    for (int o = 0; o < P.Leaves.Num; o++)
        if (!IsPetal(P.Leaves[Order[o]].Picture))
            Laid.Add(Order[o]);
    TList<FSway> Sway;
    if (!SwayOf(P, Sway))
        return false;
    for (int o = 0; o < P.Leaves.Num; o++)
    {
        const FLeaf& L = P.Leaves[Laid[o]];
        FLeafInstance I;
        I.X        = L.At.X;
        I.Y        = L.At.Y;
        I.Z        = L.At.Z;
        I.Size     = L.Size;
        PackSway(Sway[L.Node], I.Wind);
        I.Face[0]  = Snorm16(L.Face.X);
        I.Face[1]  = Snorm16(L.Face.Y);
        I.Face[2]  = Snorm16(L.Face.Z);
        I.Face[3]  = 0;
        I.Along[0] = Snorm16(L.Along.X);
        I.Along[1] = Snorm16(L.Along.Y);
        I.Along[2] = Snorm16(L.Along.Z);
        I.Along[3] = 0;
        int Hang   = (int)(L.Droop / 0.4f * 31.0f + 0.5f);
        Hang       = (Hang < 0) ? 0 : ((Hang > 31) ? 31 : Hang);
        I.Picture  = (unsigned char)((L.Picture & 7) | (Hang << 3));
        I.Hue      = Byte(L.Hue);
        I.Phase    = Byte(L.Phase);
        I.Fold     = Byte(L.Fold / 0.5f);
        float Open[4];
        OpenAt(Openness, LeafMiddle(L), Open);
        PackOpen(Open, I.Open);
        if (!Out.Leaf.Add(I))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// EZ-Tree (https://github.com/dgreenheck/ez-tree, MIT License, Copyright (c) 2024 Daniel Greenheck - the notice is in
// THIRD-PARTY-NOTICES.md): its growth, ported from its tree.js. A tree is grown branch by branch from a queue, each
// branch in sections that taper, wander by its level's gnarliness, twist, and turn towards the sky; its children are set
// along it at its level's angle, spread evenly up it and round it, each starting on its axis, inside its wood; a branch
// of the last level bears leaves - two crossed cards of a twig's picture each - and so does each branch's end there.
// Its meshing is its own as well: a tube of rings, one ring a section, turned as the section is. The numbers are its
// presets'. It grows with Y up; the plant takes it with Z up, scaled to the height the game's model has. With Least,
// only the branches at least that thick where they start are meshed, and no leaves - a far plant's bark (Trees3D).
// ---------------------------------------------------------------------------------------------------------------

struct FEzPreset
{
    int   Levels;
    float Angle[4];         // a level's children's angle off their parent, degrees (from level 1)
    int   Children[4];      // the children a branch of a level puts out
    float Force;            // how strongly a section turns towards the sky
    float Gnarl[4], Length[4], Radius[4];
    int   Sections[4], Segments[4];
    float Start[4];         // where along their parent a level's children begin, 0 .. 1 (from level 1)
    float Taper[4], Twist[4];
    float LeafAngle;        // degrees
    int   LeafCount;
    float LeafStart, LeafSize, LeafVariance;
    int   Picture;          // its leaves' card in the sheet: 0 oak, 1 ash, 2 aspen, 3 oak mirrored
};

// The preset a kind grows by: EZ-Tree's oaks and ashes for the game's four trees, its bushes for the game's two natural
// bushes. The box hedge is not one of them (EzGrows).
inline const FEzPreset& EzPresetOf(EKind Kind)
{
    static const FEzPreset OakMedium = { 3, { 0, 54, 58, 32 }, { 6, 4, 3, 0 }, 0.02f, { 0, -0.1f, -0.15f, 0.09f },
        { 37.24f, 11.08f, 12.39f, 7.16f }, { 1.41f, 0.9f, 0.69f, 1.19f }, { 8, 6, 3, 1 }, { 7, 5, 3, 3 },
        { 0, 0.49f, 0.06f, 0.12f }, { 0.73f, 0.42f, 0.69f, 0.75f }, { -0.23f, 0.42f, 0, 0 }, 42, 18, 0.16f, 2.5f, 0.7f, 0 };
    static const FEzPreset AshMedium = { 3, { 0, 48, 75, 60 }, { 7, 4, 3, 0 }, 0.01f, { 0.03f, 0.25f, 0.2f, 0.09f },
        { 43.47f, 27.14f, 9.51f, 4.6f }, { 2.0f, 0.63f, 0.76f, 0.7f }, { 12, 8, 6, 4 }, { 12, 6, 4, 3 },
        { 0, 0.23f, 0.33f, 0 }, { 0.7f, 0.7f, 0.7f, 0.7f }, { 0.09f, -0.07f, 0, 0 }, 55, 16, 0, 2.67f, 0.72f, 1 };
    static const FEzPreset OakLarge = { 3, { 0, 54, 43, 32 }, { 9, 5, 3, 0 }, 0.02f, { -0.04f, 0.16f, -0.06f, 0.09f },
        { 47.7f, 29.39f, 17.62f, 7.16f }, { 3.0f, 0.69f, 0.69f, 1.19f }, { 16, 9, 8, 3 }, { 12, 5, 3, 3 },
        { 0, 0.35f, 0.1f, 0 }, { 0.73f, 0.42f, 0.69f, 0.75f }, { -0.23f, 0.42f, 0, 0 }, 36, 10, 0.16f, 4.5f, 0.7f, 3 };
    static const FEzPreset AshLarge = { 3, { 0, 39, 39, 51 }, { 10, 4, 3, 0 }, 0.01f, { -0.05f, 0.2f, 0.16f, 0.05f },
        { 45.0f, 29.42f, 15.3f, 4.6f }, { 3.03f, 0.53f, 0.79f, 1.11f }, { 12, 8, 6, 4 }, { 8, 6, 4, 3 },
        { 0, 0.32f, 0.34f, 0 }, { 0.7f, 0.62f, 0.76f, 0 }, { 0.09f, -0.07f, 0, 0 }, 30, 10, 0.01f, 4.62f, 0.72f, 1 };
    static const FEzPreset BushOne = { 3, { 0, 21.52f, 62.61f, 60 }, { 7, 3, 2, 0 }, 0, { 0.11f, 0.09f, 0.05f, 0.09f },
        { 0.1f, 15.3f, 5.59f, 4.6f }, { 0.58f, 0.95f, 0.76f, 0.7f }, { 6, 6, 10, 10 }, { 4, 4, 4, 3 },
        { 0, 0.53f, 0.33f, 0 }, { 0.7f, 0.7f, 0.7f, 0.7f }, { 0.3f, -0.07f, 0, 0 }, 55, 12, 0, 2.45f, 0.717f, 1 };
    static const FEzPreset BushTwo = { 2, { 0, 19.57f, 27.39f, 0 }, { 10, 3, 0, 0 }, 0, { 0.022f, 0.109f, 0.05f, 0 },
        { 0.1f, 19.65f, 7.7f, 0 }, { 0.58f, 0.95f, 0.76f, 0 }, { 3, 4, 10, 0 }, { 4, 4, 4, 0 },
        { 0, 0.64f, 0.71f, 0 }, { 0.7f, 0.7f, 0.7f, 0 }, { 0.36f, -0.043f, 0, 0 }, 55, 7, 0, 2.45f, 0.717f, 2 };
    static const FEzPreset AshSmall = { 2, { 0, 48, 75, 0 }, { 10, 3, 0, 0 }, 0.01f, { 0.11f, 0.09f, 0.05f, 0 },
        { 23.87f, 18.0f, 5.59f, 0 }, { 0.81f, 0.56f, 0.76f, 0 }, { 12, 10, 10, 0 }, { 8, 6, 4, 0 },
        { 0, 0.53f, 0.33f, 0 }, { 0.7f, 0.7f, 0.7f, 0 }, { 0.3f, -0.07f, 0, 0 }, 55, 30, 0, 2.05f, 0.717f, 1 };
    switch (Kind)
    {
        case Kind_Small: return AshSmall;
        case Kind_Tree1: return OakMedium;
        case Kind_Tree2: return AshMedium;
        case Kind_Tree3: return OakLarge;
        case Kind_Tree4: return AshLarge;
        case Kind_Bush2: return BushOne;
        default:         return BushTwo;
    }
}
// Whether a kind grows by EZ-Tree: all but the box hedge, which is trimmed (FShape::Box), and the shrub roses.
inline bool EzGrows(EKind Kind) { return Kind != Kind_Bush1 && !IsRose(Kind) && !IsStemPlant(Kind); }

// EZ-Tree's random numbers: a multiply-with-carry pair of its own seeding.
struct FEzRng
{
    unsigned int W, Z;
    explicit FEzRng(unsigned int Seed) : W(123456789u + Seed), Z(987654321u - Seed) {}
    // A number between Min and Max.
    float Random(float Max = 1.0f, float Min = 0.0f)
    {
        Z = 36969u * (Z & 65535u) + (Z >> 16);
        W = 18000u * (W & 65535u) + (W >> 16);
        const unsigned int R = (Z << 16) + (W & 65535u);
        return (Max - Min) * (float)((double)R / 4294967296.0) + Min;
    }
};

// Its turns: three.js's quaternions, and its Euler angles in the order X, Y, Z.
struct FEzTurn
{
    float X, Y, Z, W;
};
inline FEzTurn EzMul(FEzTurn A, FEzTurn B)
{
    const FEzTurn Q = { A.W * B.X + A.X * B.W + A.Y * B.Z - A.Z * B.Y, A.W * B.Y - A.X * B.Z + A.Y * B.W + A.Z * B.X,
                        A.W * B.Z + A.X * B.Y - A.Y * B.X + A.Z * B.W, A.W * B.W - A.X * B.X - A.Y * B.Y - A.Z * B.Z };
    return Q;
}
inline FEzTurn EzAxis(FVec Axis, float Angle)
{
    const float   S = sinf(0.5f * Angle);
    const FEzTurn Q = { Axis.X * S, Axis.Y * S, Axis.Z * S, cosf(0.5f * Angle) };
    return Q;
}
inline FEzTurn EzEuler(float X, float Y, float Z)
{
    const float   C1 = cosf(0.5f * X), C2 = cosf(0.5f * Y), C3 = cosf(0.5f * Z);
    const float   S1 = sinf(0.5f * X), S2 = sinf(0.5f * Y), S3 = sinf(0.5f * Z);
    const FEzTurn Q  = { S1 * C2 * C3 + C1 * S2 * S3, C1 * S2 * C3 - S1 * C2 * S3, C1 * C2 * S3 + S1 * S2 * C3,
                         C1 * C2 * C3 - S1 * S2 * S3 };
    return Q;
}
inline void EzEulerOf(FEzTurn Q, float& X, float& Y, float& Z)
{
    const float M11 = 1.0f - 2.0f * (Q.Y * Q.Y + Q.Z * Q.Z), M12 = 2.0f * (Q.X * Q.Y - Q.Z * Q.W);
    const float M13 = 2.0f * (Q.X * Q.Z + Q.Y * Q.W), M22 = 1.0f - 2.0f * (Q.X * Q.X + Q.Z * Q.Z);
    const float M23 = 2.0f * (Q.Y * Q.Z - Q.X * Q.W), M32 = 2.0f * (Q.Y * Q.Z + Q.X * Q.W);
    const float M33 = 1.0f - 2.0f * (Q.X * Q.X + Q.Y * Q.Y);
    Y = asinf((M13 < -1.0f) ? -1.0f : ((M13 > 1.0f) ? 1.0f : M13));
    if (fabsf(M13) < 0.9999999f)
    {
        X = atan2f(-M23, M33);
        Z = atan2f(-M12, M11);
    }
    else
    {
        X = atan2f(M32, M22);
        Z = 0.0f;
    }
}
inline FVec EzRot(FEzTurn Q, FVec V)
{
    const FVec U = Vec(Q.X, Q.Y, Q.Z);
    const FVec T = Cross(U, V) * 2.0f;
    return V + T * Q.W + Cross(U, T);
}
// From A towards B by T (three.js's slerp).
inline FEzTurn EzSlerp(FEzTurn A, FEzTurn B, float T)
{
    float Cos = A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W;
    if (Cos < 0.0f)
    {
        B.X = -B.X; B.Y = -B.Y; B.Z = -B.Z; B.W = -B.W;
        Cos = -Cos;
    }
    float Wa = 1.0f - T, Wb = T;
    if (Cos < 0.9995f)
    {
        const float Angle = acosf(Cos), Sin = sinf(Angle);
        Wa = sinf((1.0f - T) * Angle) / Sin;
        Wb = sinf(T * Angle) / Sin;
    }
    FEzTurn Q = { Wa * A.X + Wb * B.X, Wa * A.Y + Wb * B.Y, Wa * A.Z + Wb * B.Z, Wa * A.W + Wb * B.W };
    const float L = sqrtf(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W);
    Q.X /= L; Q.Y /= L; Q.Z /= L; Q.W /= L;
    return Q;
}

struct FEzSection
{
    FVec    At;
    FEzTurn Turn;
    float   Radius;
};
struct FEzBranch
{
    FVec    Origin;
    FEzTurn Turn;
    float   Length, Radius;
    int     Level, Sections, Segments;
    int     From;           // the section of its parent's it starts at; -1 for the trunk
};
struct FEzLeaf
{
    FVec    At;
    FEzTurn Turn;
    float   Size;
    int     Section;
};

// EZ-Tree's spread of children or leaves along a branch: Count of them, each in its own stretch of the branch from
// StartMin up and its own slot round it, the slots dealt out at random; each given the place, the turn and the girth
// there (Fn: Origin, Turn, Radius, the section it stands at).
template <typename TFn>
inline bool EzSpread(FEzRng& R, int Count, float StartMin, float AngleDeg, const FEzSection* Sec, int N, int First, TFn Fn)
{
    if (Count <= 0 || N < 2)
        return true;
    const float Offset = R.Random();
    const float Step   = (1.0f - StartMin) / (float)Count;
    int Slots[64];
    if (Count > 64)
        Count = 64;
    for (int k = 0; k < Count; k++)
        Slots[k] = k;
    for (int k = Count - 1; k > 0; k--)
    {
        const int r = (int)floorf(R.Random() * (float)(k + 1));
        const int t = Slots[k];
        Slots[k]    = Slots[r];
        Slots[r]    = t;
    }
    for (int i = 0; i < Count; i++)
    {
        const float  Start = StartMin + ((float)i + R.Random()) * Step;
        int          Ix    = (int)floorf(Start * (float)(N - 1));
        Ix                 = (Ix < 0) ? 0 : ((Ix > N - 1) ? N - 1 : Ix);
        const FEzSection& A = Sec[Ix];
        const FEzSection& B = (Ix == N - 1) ? A : Sec[Ix + 1];
        const float  Alpha = (Start - (float)Ix / (float)(N - 1)) * (float)(N - 1);
        const FVec   Origin = A.At + (B.At - A.At) * Alpha;
        const float  Radius = A.Radius + (B.Radius - A.Radius) * Alpha;
        const FEzTurn Parent = EzSlerp(B.Turn, A.Turn, Alpha);
        const float  Jitter = R.Random(0.5f, -0.5f);
        const float  Round  = 6.2831853f * (Offset + ((float)Slots[i] + Jitter) / (float)Count);
        const FEzTurn Q1 = EzAxis(Vec(1.0f, 0.0f, 0.0f), AngleDeg * 0.017453293f);
        const FEzTurn Q2 = EzAxis(Vec(0.0f, 1.0f, 0.0f), Round);
        if (!Fn(Origin, EzMul(Parent, EzMul(Q2, Q1)), Radius, First + Ix))
            return false;
    }
    return true;
}

// A tree grown by EZ-Tree's way (EzPresetOf the shape's kind), into Out's nodes - one a section, each on the one before
// it, a branch's first on its parent's section - and leaves, and Mesh's bark and leaves. The plant scaled to S.Height.
inline bool GrowEz(const FShape& S, unsigned int Seed, FPlant& Out, FMesh& Mesh, float BarkWorld, float Least = 0.0f)
{
    const FEzPreset& E = EzPresetOf(S.Kind);
    FEzRng           R(Seed);
    Out.Shape = S;
    const FOutline None = {};
    Out.Outline = None;
    Out.Nodes.Clear();
    Out.Leaves.Clear();
    Out.Flowers.Clear();
    Mesh.Bark.Clear();
    Mesh.BarkIndex.Clear();
    Mesh.Leaf.Clear();
    Mesh.Petal.Clear();
    Mesh.PetalIndex.Clear();

    // The skeleton, in EZ-Tree's own space and units: every branch's sections, and the leaves.
    TList<FEzSection> Sec;
    TList<FEzBranch>  Queue, Done;
    TList<int>        DoneFirst;
    TList<FEzLeaf>    Leaves;
    const FEzTurn     Still = { 0.0f, 0.0f, 0.0f, 1.0f };
    const FEzBranch   Trunk = { Vec(0.0f, 0.0f, 0.0f), Still, E.Length[0], E.Radius[0], 0, E.Sections[0], E.Segments[0], -1 };
    if (!Queue.Add(Trunk))
        return false;
    for (int Head = 0; Head < Queue.Num && Done.Num < 4000; Head++)
    {
        const FEzBranch B = Queue[Head];
        const int First = Sec.Num;
        float Ox, Oy, Oz;
        EzEulerOf(B.Turn, Ox, Oy, Oz);
        FVec At = B.Origin;
        const float Long = B.Length / (float)((B.Sections > 0) ? B.Sections : 1);
        for (int i = 0; i <= B.Sections; i++)
        {
            float Radius = B.Radius;
            if (i == B.Sections && B.Level == E.Levels)
                Radius = 0.001f;
            else
                Radius *= 1.0f - E.Taper[B.Level] * ((float)i / (float)((B.Sections > 0) ? B.Sections : 1));
            const FEzTurn Turn = EzEuler(Ox, Oy, Oz);
            const FEzSection Here = { At, Turn, Radius };
            if (!Sec.Add(Here))
                return false;
            At = At + EzRot(Turn, Vec(0.0f, Long, 0.0f));
            // Its turn wanders - the thinner, the more - and twists, and leans towards the sky.
            const float Root = 1.0f / sqrtf((Radius > 1e-6f) ? Radius : 1e-6f);
            const float G    = ((Root > 1.0f) ? Root : 1.0f) * E.Gnarl[B.Level];
            Ox += R.Random(G, -G);
            Oz += R.Random(G, -G);
            FEzTurn Q = EzMul(EzEuler(Ox, Oy, Oz), EzAxis(Vec(0.0f, 1.0f, 0.0f), E.Twist[B.Level]));
            const FVec Up   = EzRot(Q, Vec(0.0f, 1.0f, 0.0f));
            FVec       Axis = Cross(Up, Vec(0.0f, 1.0f, 0.0f));
            const float Sin = Length(Axis);
            if (Sin > 1e-6f)
            {
                Axis = Axis * (1.0f / Sin);
                const float Full = atan2f(Sin, Dot(Up, Vec(0.0f, 1.0f, 0.0f)));
                float Step = E.Force / ((Radius > 1e-6f) ? Radius : 1e-6f);
                Step = (Step > Full) ? Full : ((Step < -Full) ? -Full : Step);
                Q = EzMul(EzAxis(Axis, Step), Q);
            }
            EzEulerOf(Q, Ox, Oy, Oz);
        }
        if (!Done.Add(B) || !DoneFirst.Add(First))
            return false;
        const int N = Sec.Num - First;
        const FEzSection Last = Sec[Sec.Num - 1];
        // A branch goes on from its end, one level on; the last level's end is a leaf.
        if (B.Level < E.Levels)
        {
            const FEzBranch On = { Last.At, Last.Turn, E.Length[B.Level + 1], Last.Radius, B.Level + 1, B.Sections,
                                   B.Segments, Sec.Num - 1 };
            if (!Queue.Add(On))
                return false;
        }
        else
        {
            const FEzLeaf L = { Last.At, Last.Turn, E.LeafSize * (1.0f + R.Random(E.LeafVariance, -E.LeafVariance)),
                                Sec.Num - 1 };
            if (!Leaves.Add(L))
                return false;
        }
        if (B.Level == E.Levels)
        {
            const bool bOk = EzSpread(R, E.LeafCount, E.LeafStart, E.LeafAngle, &Sec[First], N, First,
                [&](FVec O, FEzTurn T, float, int At2)
                {
                    const FEzLeaf L = { O, T, E.LeafSize * (1.0f + R.Random(E.LeafVariance, -E.LeafVariance)), At2 };
                    return Leaves.Add(L);
                });
            if (!bOk)
                return false;
        }
        else if (B.Level < E.Levels)
        {
            const int Lv = B.Level + 1;
            const bool bOk = EzSpread(R, E.Children[B.Level], E.Start[Lv], E.Angle[Lv], &Sec[First], N, First,
                [&](FVec O, FEzTurn T, float Radius, int At2)
                {
                    const FEzBranch C = { O, T, E.Length[Lv], E.Radius[Lv] * Radius, Lv, E.Sections[Lv],
                                          E.Segments[Lv], At2 };
                    return Queue.Add(C);
                });
            if (!bOk)
                return false;
        }
    }

    // Into the plant's space: Z up, its height the shape's; its crown spread out or drawn in to the shape's width - the
    // game's model's - within reason, its wood round as it was.
    float Top = 1e-3f, Spread = 1e-3f;
    for (int i = 0; i < Sec.Num; i++)
    {
        Top    = (Sec[i].At.Y > Top) ? Sec[i].At.Y : Top;
        const float D = sqrtf(Sec[i].At.X * Sec[i].At.X + Sec[i].At.Z * Sec[i].At.Z);
        Spread = (D > Spread) ? D : Spread;
    }
    for (int l = 0; l < Leaves.Num; l++)
    {
        const float D = sqrtf(Leaves[l].At.X * Leaves[l].At.X + Leaves[l].At.Z * Leaves[l].At.Z);
        Spread = (D > Spread) ? D : Spread;
    }
    const float Scale = S.Height / Top;
    float       Wide  = S.Radius / (Spread * Scale);
    Wide              = (Wide < 0.8f) ? 0.8f : ((Wide > 1.6f) ? 1.6f : Wide);
    auto Place = [&](FVec V) { return Vec(V.X * Scale * Wide, V.Z * Scale * Wide, V.Y * Scale); };
    auto Way   = [&](FVec V) { return Unit(Vec(V.X, V.Z, V.Y), Vec(0.0f, 0.0f, 1.0f)); };
    if (!Out.Nodes.Reserve(Sec.Num))
        return false;
    for (int b = 0; b < Done.Num; b++)
    {
        const int First = DoneFirst[b];
        const int End   = (b + 1 < Done.Num) ? DoneFirst[b + 1] : Sec.Num;
        for (int i = First; i < End; i++)
        {
            FNode Nd;
            Nd.At       = Place(Sec[i].At);
            Nd.Parent   = (i > First) ? i - 1 : Done[b].From;
            Nd.Children = 0;
            Nd.Radius   = Sec[i].Radius * Scale;
            Nd.Branch   = First;
            Nd.Pull     = Vec(0.0f, 0.0f, 0.0f);
            Nd.Pulls    = 0;
            Nd.Next     = -1;
            if (!Out.Nodes.Add(Nd))
                return false;
        }
    }
    for (int i = 0; i < Out.Nodes.Num; i++)
        if (Out.Nodes[i].Parent >= 0)
            Out.Nodes[Out.Nodes[i].Parent].Children++;
    // The leaves: two crossed cards a place, the second turned a quarter round the twig's way.
    for (int l = 0; l < Leaves.Num; l++)
    {
        const FEzLeaf& L = Leaves[l];
        const unsigned int H = Hash((unsigned int)l * 2654435761u ^ Seed);
        for (int c = 0; c < 2; c++)
        {
            FLeaf Lf;
            Lf.At      = Place(L.At);
            Lf.Along   = Way(EzRot(L.Turn, Vec(0.0f, 1.0f, 0.0f)));
            Lf.Face    = Way(EzRot(L.Turn, c ? Vec(1.0f, 0.0f, 0.0f) : Vec(0.0f, 0.0f, 1.0f)));
            Lf.Size    = L.Size * Scale;
            Lf.Picture = (E.Picture == 0 && (H & 1u)) ? 3 : E.Picture;
            Lf.Node    = L.Section;
            Lf.Hue     = (float)((H >> 8) & 255u) / 255.0f * 0.25f;
            Lf.Phase   = (float)((H >> 16) & 255u) / 255.0f;
            Lf.Fold    = 0.0f;
            Lf.Droop   = 0.0f;
            if (!Out.Leaves.Add(Lf))
                return false;
        }
    }

    FOpenness Openness;
    if (!MakeOpenness(Out, Openness))
        return false;
    TList<FSway> Sway;
    if (!SwayOf(Out, Sway))
        return false;
    // The bark: each branch a tube of rings, one a section, turned as the section is; round it in whole repeats of
    // the bark's picture, along it by the length it has come.
    for (int b = 0; b < Done.Num; b++)
    {
        const int First = DoneFirst[b];
        const int End   = (b + 1 < Done.Num) ? DoneFirst[b + 1] : Sec.Num;
        if (End - First < 2 || Sec[First].Radius * Scale < Least)
            continue;
        int Segs = Done[b].Segments * 2;
        Segs     = (Segs < 6) ? 6 : ((Segs > 16) ? 16 : Segs);
        const float Round = 6.2831853f * Sec[First].Radius * Scale / ((BarkWorld > 1e-3f) ? BarkWorld : 1.0f);
        const float Wraps = (Round < 1.0f) ? 1.0f : floorf(Round + 0.5f);
        const unsigned int Base = (unsigned int)Mesh.Bark.Num;
        float Along = (float)(Hash((unsigned int)b * 0x9E3779B9u ^ Seed) & 1023u) / 1024.0f;
        for (int i = First; i < End; i++)
        {
            if (i > First)
                Along += Length(Out.Nodes[i].At - Out.Nodes[i - 1].At) / ((BarkWorld > 1e-3f) ? BarkWorld : 1.0f);
            unsigned short Wind[8];
            PackSway(Sway[i], Wind);
            for (int j = 0; j <= Segs; j++)
            {
                const float A = 6.2831853f * (float)j / (float)Segs;
                const FVec  Rim = EzRot(Sec[i].Turn, Vec(cosf(A), 0.0f, sinf(A)));
                const FVec  Nrm = Way(Rim);
                const FVec  Pos = Place(Sec[i].At) + Nrm * (Sec[i].Radius * Scale);
                FVertex V;
                V.X = Pos.X; V.Y = Pos.Y; V.Z = Pos.Z;
                V.NX = Nrm.X; V.NY = Nrm.Y; V.NZ = Nrm.Z;
                V.U    = Wraps * (float)j / (float)Segs;
                V.V    = Along;
                memcpy(V.Wind, Wind, sizeof(Wind));
                float Open[4];
                OpenAt(Openness, Pos, Open);
                PackOpen(Open, V.Open);
                if (!Mesh.Bark.Add(V))
                    return false;
            }
        }
        const unsigned int Ring = (unsigned int)(Segs + 1);
        for (int i = 0; i + 1 < End - First; i++)
            for (int j = 0; j < Segs; j++)
            {
                const unsigned int V1 = Base + (unsigned int)i * Ring + (unsigned int)j, V2 = V1 + 1u;
                const unsigned int V3 = V1 + Ring, V4 = V2 + Ring;
                if (!Mesh.BarkIndex.Add(V1) || !Mesh.BarkIndex.Add(V3) || !Mesh.BarkIndex.Add(V2) ||
                    !Mesh.BarkIndex.Add(V2) || !Mesh.BarkIndex.Add(V3) || !Mesh.BarkIndex.Add(V4))
                    return false;
            }
    }
    return (Least > 0.0f) ? true : LeafInstancesOf(Out, Openness, Mesh);
}

// The flowers' heads (FFlower) into Out's Petal and PetalIndex: each a grid along its sheet and across it, every point
// swaying as its twig's end does - the head moves as one -, lit by how open the plant is at its heart, its colour from
// the middle of its petal picture, pale at the heart and its red out to the rim. Laid twice, once a side: the second
// wound the other way, its normals turned (a plant's faces are drawn from their front alone).
inline bool FlowerHeadsOf(const FPlant& P, const FOpenness& Openness, const TList<FSway>& Sway, FMesh& Out)
{
    TList<FVec>  Grid;
    TList<FVec>  Normal;
    TList<float> PhiOf;
    for (int f = 0; f < P.Flowers.Num; f++)
    {
        const FFlower& F      = P.Flowers[f];
        const float    Turns  = F.bBud ? BudTurns : RoseTurns;
        const float    Lean   = F.bBud ? BudLean : RoseLean;
        const float    Fall   = F.bBud ? BudLeanFall : RoseLeanFall;
        const int      Along  = (int)(Turns * RosePerTurn * (float)RosePetalSteps) + 1;
        const int      Across = RoseAcrossSteps + 1;
        const float    Petal  = 6.2831853f / RosePerTurn;
        const FVec     B      = Cross(F.Up, F.Across);
        Grid.Clear();
        Normal.Clear();
        PhiOf.Clear();
        if (!Grid.Reserve(Along * Across) || !Normal.Reserve(Along * Across) || !PhiOf.Reserve(Along))
            return false;
        for (int i = 0; i < Along; i++)
        {
            const float K    = (float)i / (float)(Along - 1);
            const float Th   = F.Spin + K * Turns * 6.2831853f;
            const float Phi  = Th / Petal - floorf(Th / Petal);
            const float Edge = 1.0f - 2.0f * Phi;
            const float Lobe = 1.0f - Edge * Edge * Edge * Edge * 0.25f;
            const float Tilt = Lean * expf(-Fall * K);
            const FVec  Way  = F.Across * cosf(Th) + B * sinf(Th);
            PhiOf.Add(Phi);
            for (int j = 0; j < Across; j++)
            {
                const float S   = (float)j / (float)(Across - 1);
                const float Cup = RoseCup * 4.0f * (S * S - S) * (S * S - S) * sinf(Tilt);
                const float Len = F.Size * Lobe;
                const float Rad = Len * (S * sinf(Tilt) + Cup * cosf(Tilt));
                const float Hgt = Len * (S * cosf(Tilt) - Cup * sinf(Tilt));
                Grid.Add(F.At + Way * Rad + F.Up * Hgt);
            }
        }
        // The normals: along the sheet by across it, from the neighbours either side; the heart's from the row beside.
        for (int i = 0; i < Along; i++)
            for (int j = 0; j < Across; j++)
            {
                const int  i0 = (i > 0) ? i - 1 : i, i1 = (i + 1 < Along) ? i + 1 : i;
                const int  jj = (j > 0) ? j : 1;
                const int  j0 = jj - 1, j1 = (jj + 1 < Across) ? jj + 1 : jj;
                const FVec Th = Grid[i1 * Across + jj] - Grid[i0 * Across + jj];
                const FVec Sv = Grid[i * Across + j1] - Grid[i * Across + j0];
                Normal.Add(Unit(Cross(Th, Sv), F.Up));
            }
        unsigned short Wind[8];
        PackSway(Sway[F.Node], Wind);
        float Open[4];
        OpenAt(Openness, F.At, Open);
        const unsigned int Base = (unsigned int)Out.Petal.Num, Side = (unsigned int)(Along * Across);
        for (int Back = 0; Back < 2; Back++)
            for (int i = 0; i < Along; i++)
                for (int j = 0; j < Across; j++)
                {
                    const FVec& Q = Grid[i * Across + j];
                    const FVec  N = Normal[i * Across + j] * (Back ? -1.0f : 1.0f);
                    FVertex     V;
                    V.X  = Q.X;
                    V.Y  = Q.Y;
                    V.Z  = Q.Z;
                    V.NX = N.X;
                    V.NY = N.Y;
                    V.NZ = N.Z;
                    V.U  = ((float)F.Picture + 0.5f + (PhiOf[i] - 0.5f) * 0.12f) / 8.0f;
                    V.V  = 1.0f - (0.05f + 0.85f * (float)j / (float)(Across - 1));
                    memcpy(V.Wind, Wind, sizeof(Wind));
                    PackOpen(Open, V.Open);
                    if (!Out.Petal.Add(V))
                        return false;
                }
        for (int i = 0; i + 1 < Along; i++)
            for (int j = 0; j + 1 < Across; j++)
            {
                const unsigned int A = Base + (unsigned int)(i * Across + j), Bn = A + (unsigned int)Across;
                const unsigned int C = A + 1u, D = Bn + 1u;
                const unsigned int Front[6] = { A, Bn, C, C, Bn, D };
                const unsigned int Rear[6]  = { A + Side, C + Side, Bn + Side, C + Side, D + Side, Bn + Side };
                for (int t = 0; t < 6; t++)
                    if (!Out.PetalIndex.Add(Front[t]))
                        return false;
                for (int t = 0; t < 6; t++)
                    if (!Out.PetalIndex.Add(Rear[t]))
                        return false;
            }
    }
    return true;
}

inline bool Build(const FPlant& P, FMesh& Out, float BarkWorld, bool bPlain = false, float Least = 0.0f)
{
    Out.Bark.Clear();
    Out.Leaf.Clear();
    Out.BarkIndex.Clear();
    Out.Petal.Clear();
    Out.PetalIndex.Clear();
    const FShape& S = P.Shape;
    // Plain: no wood carved - every run a smooth tube - and the bark alone.
    const float CarveFrom = bPlain ? 3.0e38f : ReliefFrom;
    FOpenness Openness;
    if (!MakeOpenness(P, Openness))
        return false;
    TList<FSway> Sway;
    if (!SwayOf(P, Sway))
        return false;
    TList<int>   Thickest, Sides;
    TList<float> Begun, Round;
    TList<bool>  On;
    TList<FVec>  EndSide;
    if (!Thickest.Reserve(P.Nodes.Num) || !Sides.Reserve(P.Nodes.Num) || !Begun.Reserve(P.Nodes.Num) ||
        !Round.Reserve(P.Nodes.Num) || !On.Reserve(P.Nodes.Num) || !EndSide.Reserve(P.Nodes.Num))
        return false;
    for (int i = 0; i < P.Nodes.Num; i++)
    {
        Thickest.Add(-1);
        Sides.Add(3);
        Begun.Add(0.0f);
        Round.Add(1.0f);
        On.Add(false);
        EndSide.Add(Vec(1.0f, 0.0f, 0.0f));
    }
    for (int i = 0; i < P.Nodes.Num; i++)
    {
        const int Pa = P.Nodes[i].Parent;
        if (Pa >= 0 && (Thickest[Pa] < 0 || P.Nodes[i].Radius > P.Nodes[Thickest[Pa]].Radius))
            Thickest[Pa] = i;
    }
    // The runs, parents before children.
    for (int i = 0; i < P.Nodes.Num; i++)
    {
        const FNode& N  = P.Nodes[i];
        const int    Pa = N.Parent;
        On[i] = Pa >= 0 && Thickest[Pa] == i && N.Radius >= 0.25f * Begun[Pa] &&
                !(Begun[Pa] >= CarveFrom && N.Radius < CarveFrom);
        if (On[i])
        {
            Begun[i] = Begun[Pa];
            Sides[i] = Sides[Pa];
            Round[i] = Round[Pa];
            continue;
        }
        const float R0 = (Pa < 0) ? N.Radius : RunStart(N.Radius, P.Nodes[Pa].Radius, Thickest[Pa] == i);
        Begun[i] = R0;
        // Round in whole repeats, so the bark meets itself.
        const float Repeats = floorf(6.2831853f * R0 / BarkWorld + 0.5f);
        Round[i]            = (Repeats < 1.0f) ? 1.0f : Repeats;
        int Count = (R0 >= CarveFrom) ? (int)(Round[i] * BarkWorld / ReliefStep + 0.5f) : (int)(R0 * 2.0f) + 5;
        Sides[i]  = (R0 >= CarveFrom) ? ((Count > 160) ? 160 : Count) : ((Count > 10) ? 10 : Count);
    }
    // The finest twigs are under the leaves: none of their own. Which nodes a drawn tube of their own run goes on from.
    auto Drawn = [&](int i)
    {
        return !(P.Nodes[i].Radius < S.Twig * 1.25f && P.Nodes[i].Children == 0) && P.Nodes[i].Radius >= Least;
    };
    TList<bool> GoesOn;
    if (!GoesOn.Reserve(P.Nodes.Num))
        return false;
    for (int i = 0; i < P.Nodes.Num; i++)
        GoesOn.Add(false);
    for (int i = 0; i < P.Nodes.Num; i++)
        if (On[i] && Drawn(i))
            GoesOn[P.Nodes[i].Parent] = true;
    // Where a run ends and wood still grows on from it - its thickest child a run of its own from there, the wood
    // thinned past carving or past a quarter of the run's start, or only the other children of a fork - the wood is
    // as thick at the run's end as it is: the next runs start inside it and come out through its bark or through the
    // end's dome (below). Narrowed to a branch's tip there, the end would stand inside their first rings with the gaps
    // between open.
    TList<bool> Leads;
    if (!Leads.Reserve(P.Nodes.Num))
        return false;
    for (int i = 0; i < P.Nodes.Num; i++)
        Leads.Add(false);
    for (int i = 0; i < P.Nodes.Num; i++)
    {
        const int Pa = P.Nodes[i].Parent;
        if (Pa >= 0 && !On[i] && Drawn(i))
            Leads[Pa] = true;
    }
    // And the thickest of the drawn children there goes on as a run of its own whose first ring is the ended run's last
    // one - the same place, the same plane (the joint's bisector, Tangent), as wide - so the wood bends on from its end
    // as along a run, only its sides and its bark's repeats round its own; the thickest drawn, past a thicker twig the
    // leaves hide. Ended in a dome with the wood starting over out of it, or out of its side, a limb that turned there
    // would look cut off, the way on standing out of its stump.
    TList<int> Joins;
    if (!Joins.Reserve(P.Nodes.Num))
        return false;
    for (int i = 0; i < P.Nodes.Num; i++)
        Joins.Add(-1);
    for (int i = 0; i < P.Nodes.Num; i++)
    {
        const int Pa = P.Nodes[i].Parent;
        if (Pa < 0 || P.Nodes[Pa].Parent < 0 || GoesOn[Pa] || On[i] || !Drawn(i))
            continue;
        if (Joins[Pa] < 0 || P.Nodes[i].Radius > P.Nodes[Joins[Pa]].Radius)
            Joins[Pa] = i;
    }
    // How far along the bark picture each node's end ring is, in its repeats: along the wood, as many to a unit of
    // length as there are round it at the wood's girth there - so the plates are as long for their width on a thin
    // branch as on the trunk, and run along a leaning branch rather than up by height.
    TList<float> RunV;
    if (!RunV.Reserve(P.Nodes.Num))
        return false;
    for (int i = 0; i < P.Nodes.Num; i++)
        RunV.Add(0.0f);
    // A fork's other child's run while it is still inside the wood it forks from: the fork's node (Fork) and whether
    // the tube to the node lies wholly within that wood's girth (Hidden). A thick limb's first steps are often all
    // inside a thick trunk: they are not drawn, and the first tube that comes out is the one seamed onto the girth.
    TList<int>  Fork;
    TList<bool> Hidden;
    if (!Fork.Reserve(P.Nodes.Num) || !Hidden.Reserve(P.Nodes.Num))
        return false;
    // The way the wood goes at each node, square to which its tube's end ring lies: the tube's own way, and at a node
    // its run goes on through, halfway between the tube in and the tube out - so where a run bends, the ring there
    // is the joint's bisector, and the rings between turn to it a little each. Squared to the way in at one end and to
    // the way out at the other, a thick trunk's rings crossed at a bend: its inner side folded back over itself.
    TList<FVec> Tangent;
    if (!Tangent.Reserve(P.Nodes.Num))
        return false;
    for (int i = 0; i < P.Nodes.Num; i++)
    {
        const int Pa = P.Nodes[i].Parent;
        Tangent.Add((Pa < 0) ? Vec(0.0f, 0.0f, 1.0f) : Unit(P.Nodes[i].At - P.Nodes[Pa].At, Vec(0.0f, 0.0f, 1.0f)));
    }
    for (int i = 0; i < P.Nodes.Num; i++)
    {
        const int Pa = P.Nodes[i].Parent;
        if (Pa < 0 || !(On[i] || Joins[Pa] == i) || P.Nodes[Pa].Parent < 0)
            continue;
        const FVec In = Unit(P.Nodes[Pa].At - P.Nodes[P.Nodes[Pa].Parent].At, Vec(0.0f, 0.0f, 1.0f));
        Tangent[Pa]   = Unit(In + Unit(P.Nodes[i].At - P.Nodes[Pa].At, In), In);
    }
    // The child each node's run goes on in (On), -1 where the run ends there; and whether a ball lies wholly within a
    // stretch of wood - a tube from A to B of the girth given, as long as it is and not on past its ends.
    TList<int> RunsOn;
    if (!RunsOn.Reserve(P.Nodes.Num))
        return false;
    for (int i = 0; i < P.Nodes.Num; i++)
        RunsOn.Add(-1);
    for (int i = 0; i < P.Nodes.Num; i++)
        if (On[i])
            RunsOn[P.Nodes[i].Parent] = i;
    auto InTube = [](FVec C, float R, FVec A, FVec B, float Girth)
    {
        const FVec  Ab = B - A;
        const float L2 = Dot(Ab, Ab);
        if (L2 < 1e-6f)
            return false;
        const float T = Dot(C - A, Ab) / L2;
        return T >= 0.0f && T <= 1.0f && Length(C - (A + Ab * T)) + R < Girth;
    };
    for (int i = 0; i < P.Nodes.Num; i++)
    {
        Fork.Add(-1);
        Hidden.Add(false);
        const int Pa = P.Nodes[i].Parent;
        if (Pa < 0)
            continue;
        if (!On[i] && Thickest[Pa] != i && Joins[Pa] != i && P.Nodes[Pa].Parent >= 0)
            Fork[i] = Pa;
        else if (On[i] && Fork[Pa] >= 0 && Hidden[Pa])
            Fork[i] = Fork[Pa];
        if (Fork[i] < 0)
            continue;
        // Inside the wood the fork is on: the tube that comes into the fork, or the one its run goes on in - not the
        // line of the tube in on past the fork, where a run that ends or bends there has no wood.
        const FNode& Fn    = P.Nodes[Fork[i]];
        const FNode& N     = P.Nodes[i];
        const float  Girth = Fn.Radius * (1.0f - 0.5f * ReliefDepth);
        const int    Next  = RunsOn[Fork[i]];
        Hidden[i] = InTube(N.At, N.Radius, P.Nodes[Fn.Parent].At, Fn.At, Girth) ||
                    (Next >= 0 && InTube(N.At, N.Radius, Fn.At, P.Nodes[Next].At,
                                         ((P.Nodes[Next].Radius < Fn.Radius) ? P.Nodes[Next].Radius : Fn.Radius) *
                                             (1.0f - 0.5f * ReliefDepth)));
    }
    // The bends: where a run goes on through a node - its drawn On child, or a run joined onto its end - turning there
    // by a fifth of a right angle or more, on wood thick enough for the turn to show (carved, or a unit and a half
    // round), the wood turns along a curve rather than at a corner: a quadratic one from Bend before the node on the
    // way in to as far on the way out, the node its corner; the joint's ring at the curve's middle, square to the way
    // between the two (the plane Tangent gives it). At a corner a thick limb's short tube on a sharp turn has its
    // rings cross and its joint stand out as a knob, and the limb reads as cut off there. BendTo: the node the run
    // goes on to.
    TList<float> Bend;
    TList<int>   BendTo;
    if (!Bend.Reserve(P.Nodes.Num) || !BendTo.Reserve(P.Nodes.Num))
        return false;
    for (int n = 0; n < P.Nodes.Num; n++)
    {
        Bend.Add(0.0f);
        BendTo.Add(-1);
        const int Pa = P.Nodes[n].Parent;
        const int C  = (RunsOn[n] >= 0 && Drawn(RunsOn[n])) ? RunsOn[n] : Joins[n];
        if (C < 0 || Pa < 0 || Hidden[n] || !Drawn(n) || !(Begun[n] >= CarveFrom || P.Nodes[n].Radius >= 1.5f))
            continue;
        const FVec  In  = P.Nodes[n].At - P.Nodes[Pa].At, Ow = P.Nodes[C].At - P.Nodes[n].At;
        const float Lin = Length(In), Lout = Length(Ow);
        if (Lin < 1e-4f || Lout < 1e-4f || Dot(In, Ow) > 0.94f * Lin * Lout)
            continue;
        float D   = P.Nodes[n].Radius;
        D         = (D < 0.45f * Lin) ? D : 0.45f * Lin;
        D         = (D < 0.45f * Lout) ? D : 0.45f * Lout;
        Bend[n]   = D;
        BendTo[n] = C;
    }
    // A point of a bend's curve: U from 0 where it leaves the way in to 1 where it is on the way out, a half the joint.
    auto BendAt = [&](int n, float U)
    {
        const FVec  At = P.Nodes[n].At;
        const FVec  E  = At - Unit(At - P.Nodes[P.Nodes[n].Parent].At, Vec(0.0f, 0.0f, 1.0f)) * Bend[n];
        const FVec  X  = At + Unit(P.Nodes[BendTo[n]].At - At, Vec(0.0f, 0.0f, 1.0f)) * Bend[n];
        return E * ((1.0f - U) * (1.0f - U)) + At * (2.0f * U * (1.0f - U)) + X * (U * U);
    };
    // How thick a tube is at its end ring (Build's R1, below).
    auto EndOf = [&](int n)
    {
        const float Third = P.Nodes[n].Radius * TipShare;
        return (GoesOn[n] || Leads[n]) ? P.Nodes[n].Radius : ((Third >= S.Twig * 2.5f) ? Third : 0.0f);
    };
    for (int i = 0; i < P.Nodes.Num; i++)
    {
        const FNode& N = P.Nodes[i];
        if (N.Parent < 0)
            continue;
        const FNode& Pa = P.Nodes[N.Parent];
        if (!Drawn(i))
            continue;
        const FVec Axis = Unit(N.At - Pa.At, Vec(0.0f, 0.0f, 1.0f));
        // On a run, the parent's end ring and its turn carried onto this axis; else a ring of the tube's own.
        const bool bOn = (On[i] || Joins[N.Parent] == i) && Pa.Parent >= 0;
        // The rings' planes: square at the head to the way at the parent (on a run) or to the tube's own way, at the
        // end to the way at this node (Tangent), turned between; their sides carried from ring to ring without a twist.
        const FVec Normal0 = bOn ? Tangent[N.Parent] : Axis;
        const FVec Normal1 = Tangent[i];
        const FVec Side0   = bOn ? EndSide[N.Parent]
                                 : Unit(Cross(Axis, (fabsf(Axis.Z) < 0.9f) ? Vec(0.0f, 0.0f, 1.0f) : Vec(1.0f, 0.0f, 0.0f)),
                                        Vec(1.0f, 0.0f, 0.0f));
        const FVec Ahead0  = Cross(Normal0, Side0);
        EndSide[i]         = Unit(Side0 - Normal1 * Dot(Side0, Normal1), Side0);
        // A run joined onto the end of the one it goes on from (Joins) as wide as that end - and where that end is carved,
        // its first ring carved as that end ring is (JoinCarved, below), so the two rings lie on each other.
        const bool  bJoin       = !On[i] && Joins[N.Parent] == i;
        const bool  bJoinCarved = bJoin && Begun[N.Parent] >= CarveFrom;
        float       R0          = (On[i] || bJoin) ? Pa.Radius : RunStart(N.Radius, Pa.Radius, Thickest[N.Parent] == i);
        // Where its run ends - no drawn wood of the run goes on from the node (GoesOn) - the wood narrows to a third of
        // its girth, as a branch's end does, rather than ending as thick as it was, cut off; unless the wood goes on
        // there as a run of its own (Leads). A third too thin to close with a dome (below) narrows on to a point, so
        // that no twig's end is an open tube to look into. Where the run goes on, the wood goes on through it.
        const float R1    = EndOf(i);
        const FVec  From  = Pa.At;
        const float Long  = Length(N.At - From);
        // A tube within the wood its run forks from (Hidden): not drawn; only how its end ring is turned (EndSide,
        // above) and how far along the picture it is go on to the next.
        if (Hidden[i])
        {
            const float Rm = 0.5f * (R0 + R1);
            RunV[i] = RunV[N.Parent] + Long * Round[i] / (6.2831853f * ((Rm > 0.05f) ? Rm : 0.05f));
            continue;
        }
        // The first tube of a fork's other child's run that comes out of the wood it forks from starts on that wood's
        // bark: each point of its first ring moved out along its axis to where it leaves that wood - so the ring is a
        // saddle on the bark and none of the tube is inside; the rings between laid from there to its end, each line
        // along on its own (Lift). Its first ring sways with the fork, so the seam holds in the wind.
        const bool bSeam = Fork[i] >= 0;
        float      Lift[161];
        for (int k = 0; k < 161; k++)
            Lift[k] = 0.0f;
        const FNode& Fn = P.Nodes[bSeam ? Fork[i] : N.Parent];
        if (bSeam)
        {
            // The wood there: the tube into the fork, on round its bend's curve if it has one, and the tube its run goes
            // on in - a drawn On child, or the run joined onto its end (Joins) - narrowing as that one does, to that
            // one's own bend; the line of its middle a few straight steps. Not the line of the tube in on past the
            // joint, where a run that turns there has no wood. Each within its own faces - a thin run is a few-sided
            // tube, its flat faces' middles nearer its axis than its corners by the cosine of half a side - and on carved
            // wood down at its crevices' floor, so that no crevice leaves a gap under the seam. Proud: how far a point
            // (from the fork) stands out of that wood, in the fork's radius.
            const int   F      = Fork[i];
            const FVec  Up     = Unit(Fn.At - P.Nodes[Fn.Parent].At, Axis);
            const int   Nx     = (RunsOn[F] >= 0 && Drawn(RunsOn[F])) ? RunsOn[F] : Joins[F];
            auto        Within = [&](int n)
            {
                return ((Begun[n] >= CarveFrom) ? 1.0f - ReliefDepth : 1.0f) * cosf(3.14159265f / (float)Sides[n]);
            };
            const float In0 = Within(F), In1 = (Nx >= 0) ? Within(Nx) : In0;
            FVec        Line[16];
            float       Wide[16];
            int         Steps = 0;
            Line[Steps]   = P.Nodes[Fn.Parent].At - Fn.At;
            Wide[Steps++] = Fn.Radius * In0;
            if (Nx >= 0 && Bend[F] > 0.0f)
                for (int u = 0; u <= 8; u++)
                {
                    Line[Steps]   = BendAt(F, (float)u / 8.0f) - Fn.At;
                    Wide[Steps++] = Fn.Radius * ((u <= 4) ? In0 : In1);
                }
            else
            {
                Line[Steps]   = Vec(0.0f, 0.0f, 0.0f);
                Wide[Steps++] = Fn.Radius * In0;
            }
            if (Nx >= 0)
            {
                const float Rx = EndOf(Nx);
                if (Bend[Nx] > 0.0f)
                {
                    const FVec  To  = BendAt(Nx, 0.0f) - Fn.At;
                    const float Run = Length(P.Nodes[Nx].At - Fn.At);
                    const float Far = (Run > 1e-4f) ? 1.0f - Bend[Nx] / Run : 1.0f;
                    Line[Steps]   = To;
                    Wide[Steps++] = (Fn.Radius + (Rx - Fn.Radius) * Far) * In1;
                    Line[Steps]   = BendAt(Nx, 0.5f) - Fn.At;
                    Wide[Steps++] = Rx * In1;
                }
                else
                {
                    Line[Steps]   = P.Nodes[Nx].At - Fn.At;
                    Wide[Steps++] = Rx * In1;
                }
            }
            // Each step's rings lie square to the way between it and the next (Cut): the wood is each step's tube
            // between those planes, as its rings are laid.
            FVec Cut[16];
            for (int k = 0; k < Steps; k++)
            {
                const FVec Back  = (k > 0) ? Unit(Line[k] - Line[k - 1], Up) : Vec(0.0f, 0.0f, 0.0f);
                const FVec Ahead = (k + 1 < Steps) ? Unit(Line[k + 1] - Line[k], Up) : Vec(0.0f, 0.0f, 0.0f);
                Cut[k]           = Unit(Back + Ahead, Up);
            }
            auto Proud = [&](FVec X)
            {
                // Where the run ends at the fork, nothing past its end ring.
                if (Nx < 0 && Dot(X, Up) > 0.0f)
                    return 1.0f;
                float Least = 1e30f;
                for (int k = 0; k + 1 < Steps; k++)
                {
                    const FVec  Ab = Line[k + 1] - Line[k];
                    const float L2 = Dot(Ab, Ab);
                    if (L2 < 1e-8f || Dot(X - Line[k], Cut[k]) < 0.0f || Dot(X - Line[k + 1], Cut[k + 1]) > 0.0f)
                        continue;
                    const float T  = Dot(X - Line[k], Ab) / L2;
                    const float Tc = (T < 0.0f) ? 0.0f : ((T > 1.0f) ? 1.0f : T);
                    const float D  = Length(X - (Line[k] + Ab * T)) - (Wide[k] + (Wide[k + 1] - Wide[k]) * Tc);
                    Least          = (D < Least) ? D : Least;
                }
                return (Least < 1e29f) ? Least / Fn.Radius : 1.0f;
            };
            // A little within it, so the seam does not lie on the bark's own face.
            const float Margin = -0.02f;
            // The first ring within the wood where it starts: made narrower until none of it stands out - a twig as
            // thick as a few-sided twig it leaves would stand out of its flat faces; never under half as wide.
            const FVec Base = From - Fn.At;
            float      Fit  = 1.0f;
            for (int k = 0; k < Sides[i]; k++)
            {
                const float A = 6.2831853f * (float)k / (float)Sides[i];
                const FVec  D = Side0 * cosf(A) + Ahead0 * sinf(A);
                if (Proud(Base + D * (R0 * Fit)) <= Margin)
                    continue;
                float Lo = 0.5f, Hi = Fit;
                for (int b = 0; b < 6; b++)
                {
                    const float M = 0.5f * (Lo + Hi);
                    if (Proud(Base + D * (R0 * M)) <= Margin)
                        Lo = M;
                    else
                        Hi = M;
                }
                Fit = Lo;
            }
            R0 *= Fit;
            // Out along the axis to where it leaves the wood - the first of a few steps that is out of it, then halved
            // down to the bark - at most LiftMost of the way to the tube's end ring: a twig leaving its wood at a slant
            // lifted further would fold up into a flat sliver, its sides turned inside out; held there, the rest of the
            // line along stays inside the wood, and the tube comes out through the bark.
            const float Most = Long * LiftMost;
            for (int k = 0; k <= Sides[i]; k++)
            {
                const float A    = 6.2831853f * (float)k / (float)Sides[i];
                const FVec  W    = Base + (Side0 * cosf(A) + Ahead0 * sinf(A)) * R0;
                float       In   = 0.0f, Past = -1.0f;
                for (int t = 1; t <= 8 && Past < 0.0f; t++)
                {
                    const float At = Most * (float)t / 8.0f;
                    if (Proud(W + Axis * At) > Margin)
                        Past = At;
                    else
                        In = At;
                }
                for (int b = 0; b < 6 && Past > 0.0f; b++)
                {
                    const float M = 0.5f * (In + Past);
                    if (Proud(W + Axis * M) > Margin)
                        Past = M;
                    else
                        In = M;
                }
                Lift[k] = In;
            }
        }
        // The ring it starts on sways as the node it starts from, so the joint holds in the wind; the end ring as
        // this node; the rings between, between the two.
        const FSway Sway0 = Sway[bSeam ? Fork[i] : N.Parent], Sway1 = Sway[i];
        // The rings: the two ends, and on carved wood as many between as ReliefStep goes into the tube's length.
        const bool  bCarved = Begun[i] >= CarveFrom;
        int         Rings   = bCarved ? (int)ceilf(Long / ReliefStep) : 1;
        Rings               = (Rings < 1) ? 1 : Rings;
        // Its two ends on the bends (Bend): from the joint's middle at its start, and to the next one's at its end - not
        // a seam's start, which lies on the bark; at least four rings, so the curve reads as one.
        const float BendS  = (bOn && !bSeam && BendTo[N.Parent] == i) ? Bend[N.Parent] : 0.0f;
        const float BendE  = Bend[i];
        const FVec  InWay  = (BendS > 0.0f) ? Unit(Pa.At - P.Nodes[Pa.Parent].At, Axis) : Axis;
        const FVec  OutWay = (BendE > 0.0f) ? Unit(P.Nodes[BendTo[i]].At - N.At, Axis) : Axis;
        const FVec  Way0   = (BendS > 0.0f) ? Axis : Normal0;
        const FVec  Way1   = (BendE > 0.0f) ? Axis : Normal1;
        if ((BendS > 0.0f || BendE > 0.0f) && Rings < 4)
            Rings = 4;
        const int   Round1  = Sides[i] + 1;
        const unsigned int First = (unsigned int)Out.Bark.Num;
        // The first ring of a run joined onto carved wood (bJoinCarved) is that wood's end ring itself - as many sides,
        // the same corners - stitched to this run's own sides at the next ring, so the joint is closed; every other
        // ring has this run's sides. Where each ring's corners start.
        const int  Ring0     = bJoinCarved ? Sides[N.Parent] : Sides[i];
        auto       RingStart = [&](int j) { return First + ((j == 0) ? 0u : (unsigned int)(Ring0 + 1 + (j - 1) * Round1)); };
        // The parent's end ring's place in the picture, and from there each ring by the girth half a ring back.
        float Vv = RunV[N.Parent];
        FVec  Sd = Side0;
        for (int j = 0; j <= Rings; j++)
        {
            const float T     = (float)j / (float)Rings;
            // On a bend: the curve's point and its way; between, the straight wood, its rings turning from the one end's
            // plane to the other's.
            const float Sl    = T * Long;
            FVec        Mid   = From + (N.At - From) * T;
            FVec        Along = Unit(Way0 + (Way1 - Way0) * T, Axis);
            if (BendS > 0.0f && Sl < BendS)
            {
                const float U = 0.5f + 0.5f * Sl / BendS;
                Mid           = BendAt(N.Parent, U);
                Along         = Unit(InWay * (1.0f - U) + Axis * U, Axis);
            }
            else if (BendE > 0.0f && Sl > Long - BendE)
            {
                const float U = 0.5f * (1.0f - (Long - Sl) / BendE);
                Mid           = BendAt(i, U);
                Along         = Unit(Axis * (1.0f - U) + OutWay * U, Axis);
            }
            Sd                = Unit(Sd - Along * Dot(Sd, Along), Sd);
            const FVec  Ah    = Cross(Along, Sd);
            const float R     = R0 + (R1 - R0) * T;
            if (j > 0)
            {
                const float Rm = R0 + (R1 - R0) * ((float)j - 0.5f) / (float)Rings;
                Vv += (Long / (float)Rings) * Round[i] / (6.2831853f * ((Rm > 0.05f) ? Rm : 0.05f));
            }
            const float PerLength = Round[i] / (6.2831853f * ((R > 0.05f) ? R : 0.05f));
            const float Depth = R * ReliefDepth;
            float       Open[4];
            OpenAt(Openness, Mid, Open);
            const int Nk = (j == 0) ? Ring0 : Sides[i];
            for (int k = 0; k <= Nk; k++)
            {
                const float A  = 6.2831853f * (float)k / (float)Nk;
                const FVec  D  = Sd * cosf(A) + Ah * sinf(A);
                const float U  = Round[i] * (float)k / (float)Nk;
                // This line along's own start (Lift): its place on the axis, and as far on in the picture.
                const float Shift = Lift[k] * (1.0f - T);
                const FVec  C     = Mid + Axis * Shift;
                const float Vk    = Vv + Shift * PerLength;
                FVec        Q   = C + D * R, Nm = D;
                if (j == 0 && bJoinCarved)
                {
                    // The ended run's last ring, as it was carved - its own repeats round, its own place along - and
                    // turned as it was, so that the light does not change across the joint either.
                    const float Up = Round[N.Parent] * (float)k / (float)Nk;
                    const float Pl = Round[N.Parent] / (6.2831853f * ((R > 0.05f) ? R : 0.05f));
                    const float Dp = R * ReliefDepth;
                    float PerU, PerV;
                    BarkSlope(Up, RunV[N.Parent], PerU, PerV);
                    const FVec Round0 = Ah * cosf(A) - Sd * sinf(A);
                    Q  = C + D * (R - Dp * (1.0f - BarkHeight(Up, RunV[N.Parent])));
                    Nm = Unit(D - Round0 * (Dp * PerU * Round[N.Parent] / (6.2831853f * R)) - Along * (Dp * PerV * Pl), D);
                }
                else if (bCarved)
                {
                    // In from the radius down into the crevice; the face turned by the bark's slope round and along.
                    float PerU, PerV;
                    BarkSlope(U, Vk, PerU, PerV);
                    const FVec Round0 = Ah * cosf(A) - Sd * sinf(A);
                    Q  = C + D * (R - Depth * (1.0f - BarkHeight(U, Vk)));
                    Nm = Unit(D - Round0 * (Depth * PerU * Round[i] / (6.2831853f * R)) -
                                  Along * (Depth * PerV * PerLength), D);
                }
                FVertex V;
                V.X = Q.X; V.Y = Q.Y; V.Z = Q.Z;
                V.NX = Nm.X; V.NY = Nm.Y; V.NZ = Nm.Z;
                V.U     = U;
                V.V     = Vk;
                PackSway(LerpSway(Sway0, Sway1, T), V.Wind);
                PackOpen(Open, V.Open);
                if (!Out.Bark.Add(V))
                    return false;
            }
        }
        for (int j = 0; j < Rings; j++)
        {
            const unsigned int Lo = RingStart(j), Hi = RingStart(j + 1);
            const int          NLo = (j == 0) ? Ring0 : Sides[i], NHi = Sides[i];
            if (NLo == NHi)
            {
                for (int k = 0; k < NHi; k++)
                {
                    const unsigned int A = Lo + (unsigned int)k, B = A + 1u, C = Hi + (unsigned int)k, D = C + 1u;
                    const unsigned int Tri[6] = { A, B, C, C, B, D };
                    for (int t = 0; t < 6; t++)
                        if (!Out.BarkIndex.Add(Tri[t]))
                            return false;
                }
                continue;
            }
            // Two rings of different sides: round both together by their corners' angles, each step a triangle that
            // takes the next corner of whichever ring comes first.
            int m = 0, n = 0;
            while (m < NLo || n < NHi)
            {
                const bool bLo = (n >= NHi) || (m < NLo && (float)(m + 1) / (float)NLo <= (float)(n + 1) / (float)NHi);
                const unsigned int Tri[3] = { bLo ? Lo + (unsigned int)m : Hi + (unsigned int)n,
                                              bLo ? Lo + (unsigned int)(m + 1) : Lo + (unsigned int)m,
                                              bLo ? Hi + (unsigned int)n : Hi + (unsigned int)(n + 1) };
                for (int t = 0; t < 3; t++)
                    if (!Out.BarkIndex.Add(Tri[t]))
                        return false;
                if (bLo)
                    m++;
                else
                    n++;
            }
        }
        // A tube from a root - a trunk's foot, a bush's stems - starts on no wood: its first ring closed by a low dome
        // back along its axis, as a run's end is. A stem leaving the ground at a slant stands half out of it there.
        if (P.Nodes[N.Parent].Parent < 0)
        {
            const FVec Foot = From - Axis * (R0 * 0.5f);
            float      Open[4];
            OpenAt(Openness, Foot, Open);
            FVertex V;
            V.X = Foot.X; V.Y = Foot.Y; V.Z = Foot.Z;
            V.NX = -Axis.X; V.NY = -Axis.Y; V.NZ = -Axis.Z;
            V.U    = Round[i] * 0.5f;
            V.V    = RunV[N.Parent];
            PackSway(Sway0, V.Wind);
            PackOpen(Open, V.Open);
            const unsigned int Apex = (unsigned int)Out.Bark.Num;
            if (!Out.Bark.Add(V))
                return false;
            for (int k = 0; k < Ring0; k++)
            {
                const unsigned int Tri[3] = { First + (unsigned int)k + 1u, First + (unsigned int)k, Apex };
                for (int t = 0; t < 3; t++)
                    if (!Out.BarkIndex.Add(Tri[t]))
                        return false;
            }
        }
        RunV[i]    = Vv;
        EndSide[i] = Sd;
        // The run ends here: its last ring closed by a low dome round the axis, its top half the wood's girth on - on
        // wood thick enough for the hole to show, and always where only other children go on from it, out of the dome;
        // not where a run is joined onto its end ring (Joins), which closes it.
        if (!GoesOn[i] && Joins[i] < 0 && (Leads[i] || R1 >= S.Twig * 2.5f))
        {
            const FVec Top = N.At + Axis * (R1 * 0.5f);
            float      Open[4];
            OpenAt(Openness, Top, Open);
            FVertex V;
            V.X = Top.X; V.Y = Top.Y; V.Z = Top.Z;
            V.NX = Axis.X; V.NY = Axis.Y; V.NZ = Axis.Z;
            V.U    = Round[i] * 0.5f;
            V.V    = Vv;
            PackSway(Sway1, V.Wind);
            PackOpen(Open, V.Open);
            const unsigned int Apex = (unsigned int)Out.Bark.Num;
            if (!Out.Bark.Add(V))
                return false;
            const unsigned int End = RingStart(Rings);
            for (int k = 0; k < Sides[i]; k++)
            {
                const unsigned int Tri[3] = { End + (unsigned int)k, End + (unsigned int)k + 1u, Apex };
                for (int t = 0; t < 3; t++)
                    if (!Out.BarkIndex.Add(Tri[t]))
                        return false;
            }
        }
    }
    return bPlain ? true : (FlowerHeadsOf(P, Openness, Sway, Out) && LeafInstancesOf(P, Openness, Out));
}

// ---------------------------------------------------------------------------------------------------------------
// The pictures, drawn by code: bark and leaves (no picture of the game's, nor anyone else's)
// ---------------------------------------------------------------------------------------------------------------

// Value noise that repeats every Period cells each way - a picture wrapped round a trunk meets itself.
inline float LatticeValue(int X, int Y, int PeriodX, int PeriodY, unsigned int Salt)
{
    X = ((X % PeriodX) + PeriodX) % PeriodX;
    Y = ((Y % PeriodY) + PeriodY) % PeriodY;
    return (float)(Hash((unsigned int)X * 0x8da6b343u ^ (unsigned int)Y * 0xd8163841u ^ Salt) & 0xFFFFu) / 65535.0f;
}
inline float SmoothNoise(float X, float Y, int PeriodX, int PeriodY, unsigned int Salt)
{
    const int   Ix = (int)floorf(X), Iy = (int)floorf(Y);
    float       Fx = X - (float)Ix, Fy = Y - (float)Iy;
    Fx = Fx * Fx * (3.0f - 2.0f * Fx);
    Fy = Fy * Fy * (3.0f - 2.0f * Fy);
    const float A = LatticeValue(Ix, Iy, PeriodX, PeriodY, Salt), B = LatticeValue(Ix + 1, Iy, PeriodX, PeriodY, Salt);
    const float C = LatticeValue(Ix, Iy + 1, PeriodX, PeriodY, Salt), D = LatticeValue(Ix + 1, Iy + 1, PeriodX, PeriodY, Salt);
    return (A + (B - A) * Fx) * (1.0f - Fy) + (C + (D - C) * Fx) * Fy;
}
// Octaves of it, each twice as fine; U and V from 0 to 1 over the picture, Cells across it at the coarsest.
inline float Fractal(float U, float V, int CellsU, int CellsV, int Octaves, unsigned int Salt)
{
    float Sum = 0.0f, Weight = 0.5f, Total = 0.0f;
    for (int o = 0; o < Octaves; o++)
    {
        const int Cu = CellsU << o, Cv = CellsV << o;
        Sum   += Weight * SmoothNoise(U * (float)Cu, V * (float)Cv, Cu, Cv, Salt + (unsigned int)o * 101u);
        Total += Weight;
        Weight *= 0.5f;
    }
    return Sum / Total;
}

inline unsigned char Byte(float V)
{
    V = (V < 0.0f) ? 0.0f : ((V > 1.0f) ? 1.0f : V);
    return (unsigned char)(V * 255.0f + 0.5f);
}

// A cell net for the bark: a point in each cell of a grid CellsU x CellsV, jittered, the grid repeating both ways.
// The way from X Y (in cells) to the nearest cell's edge - the nearest point's border with each other near point,
// measured square to it, the least of them - in cells; and the nearest point's own number.
inline float BarkEdge(float X, float Y, int CellsU, int CellsV, unsigned int Salt, unsigned int& Id)
{
    const int Cx = (int)floorf(X), Cy = (int)floorf(Y);
    float Px[25], Py[25];
    unsigned int Ids[25];
    int   Near = 0;
    float Best = 1e9f;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++)
        {
            const int k  = (j + 2) * 5 + (i + 2);
            const int Wx = (((Cx + i) % CellsU) + CellsU) % CellsU, Wy = (((Cy + j) % CellsV) + CellsV) % CellsV;
            Ids[k] = Hash((unsigned int)Wx * 0x9E3779B1u ^ (unsigned int)Wy * 0x85EBCA77u ^ Salt);
            Px[k]  = (float)(Cx + i) + 0.05f + 0.9f * (float)(Ids[k] & 0xFFFFu) / 65535.0f;
            Py[k]  = (float)(Cy + j) + 0.05f + 0.9f * (float)((Ids[k] >> 16) & 0xFFFFu) / 65535.0f;
            const float D = (Px[k] - X) * (Px[k] - X) + (Py[k] - Y) * (Py[k] - Y);
            if (D < Best)
            {
                Best = D;
                Near = k;
            }
        }
    Id = Ids[Near];
    float Edge = 1e9f;
    for (int k = 0; k < 25; k++)
    {
        if (k == Near)
            continue;
        const float Dx = Px[k] - Px[Near], Dy = Py[k] - Py[Near];
        const float L  = sqrtf(Dx * Dx + Dy * Dy);
        if (L < 1e-6f)
            continue;
        const float Mx = 0.5f * (Px[k] + Px[Near]), My = 0.5f * (Py[k] + Py[Near]);
        const float E  = ((Mx - X) * Dx + (My - Y) * Dy) / L;
        Edge           = (E < Edge) ? E : Edge;
    }
    return Edge;
}

// The bark of a trunk, one repeat - BarkSpan level units round and along - on a grid of BarkGrid x BarkGrid, made once
// and read by both the trunk's carving (Build) and its picture (DrawBark) - the way Infinigen's random bark is made
// (THIRD-PARTY-NOTICES.md): the cells of a net, laid in a space squeezed along the trunk - a
// quarter as long - and wavered along it by noise, then stretched back, so each plate is a long cell, most of its
// edges running up the trunk; the way to the cell's edge, its first tenths only, is the crevice, the rest the plate's
// flat top. A second net, unsqueezed and finer, lays faint cracks across the plates, and noise of many octaves
// roughens their tops. Height 1 on a plate's top, 0 at a crevice's bottom; and each plate's own shade.
enum { BarkGrid = 256 };
static const float BarkSpan = 64.0f;
struct FBarkMap
{
    float Height[BarkGrid * BarkGrid];
    float Tone[BarkGrid * BarkGrid];
    bool  bMade = false;
};
inline const FBarkMap& BarkMap()
{
    static FBarkMap Map;
    if (Map.bMade)
        return Map;
    const int Around = 12, Along = 3;               // the plates' cells a repeat: round, and along squeezed four times
    const int Fine   = 9;                           // the cracks' cells a repeat, each way
    for (int y = 0; y < BarkGrid; y++)
        for (int x = 0; x < BarkGrid; x++)
        {
            const float U = ((float)x + 0.5f) / (float)BarkGrid, V = ((float)y + 0.5f) / (float)BarkGrid;
            // The squeezed space, wavered along the trunk - by cells, so the plates break and shear unevenly.
            const float Waver = (Fractal(U, V, 2, 1, 4, 23u) - 0.5f) * 7.0f + (Fractal(U, V, 8, 4, 2, 29u) - 0.5f) * 0.8f;
            unsigned int Id;
            const float  Edge = BarkEdge(U * (float)Around + (Fractal(U, V, 4, 3, 2, 31u) - 0.5f) * 0.6f,
                                         V * (float)Along + Waver, Around, Along, 0x5BD1E995u, Id);
            float Plate = Edge / 0.16f;
            Plate       = (Plate < 0.0f) ? 0.0f : ((Plate > 1.0f) ? 1.0f : Plate);
            Plate       = Plate * (2.0f - Plate);
            // Faint cracks from the finer net.
            unsigned int Id2;
            float Crack = BarkEdge(U * (float)Fine, V * (float)Fine, Fine, Fine, 0x27D4EB2Du, Id2) / 0.07f;
            Crack       = (Crack < 0.0f) ? 0.0f : ((Crack > 1.0f) ? 1.0f : Crack);
            // The wood's own swell under the bark, broad and of many octaves, more than the plates' own height - one
            // plate sits higher than its neighbour - and a plate's top rough.
            const float Swell = Fractal(U, V, 3, 3, 5, 71u);
            const float Rough = Fractal(U, V, 16, 16, 5, 53u) - 0.5f;
            float H = Plate * (0.9f + 0.1f * Crack) * (0.45f + 0.55f * Swell) + 0.2f * Rough * Plate;
            H       = (H < 0.0f) ? 0.0f : ((H > 1.0f) ? 1.0f : H);
            Map.Height[y * BarkGrid + x] = H;
            Map.Tone[y * BarkGrid + x]   = 0.84f + 0.32f * (float)(Id >> 20 & 0xFFu) / 255.0f;
        }
    Map.bMade = true;
    return Map;
}
// The bark's height at a place - U round and V along, in repeats - read between the grid's points; and its slope there,
// per repeat each way.
inline float BarkHeight(float U, float V)
{
    const FBarkMap& M = BarkMap();
    const float X = U * (float)BarkGrid - 0.5f, Y = V * (float)BarkGrid - 0.5f;
    const int   Ix = (int)floorf(X), Iy = (int)floorf(Y);
    const float Fx = X - (float)Ix, Fy = Y - (float)Iy;
    auto At = [&](int Gx, int Gy)
    {
        Gx = ((Gx % BarkGrid) + BarkGrid) % BarkGrid;
        Gy = ((Gy % BarkGrid) + BarkGrid) % BarkGrid;
        return M.Height[Gy * BarkGrid + Gx];
    };
    const float A = At(Ix, Iy), B = At(Ix + 1, Iy), C = At(Ix, Iy + 1), D = At(Ix + 1, Iy + 1);
    return (A + (B - A) * Fx) * (1.0f - Fy) + (C + (D - C) * Fx) * Fy;
}
inline void BarkSlope(float U, float V, float& PerU, float& PerV)
{
    const float E = 1.0f / (float)BarkGrid;
    PerU = (BarkHeight(U + E, V) - BarkHeight(U - E, V)) / (2.0f * E);
    PerV = (BarkHeight(U, V + E) - BarkHeight(U, V - E)) / (2.0f * E);
}

// The bark's picture, Width x Height texels, R G B A, one repeat of the map: grey-brown plates, each its own shade, a
// fine grain along them, now and then the orange-brown of a flake gone and a little grey-green lichen; the crevices'
// depths a dark brown.
inline void DrawBark(unsigned char* Rgba, int Width, int Height)
{
    const FBarkMap& M = BarkMap();
    for (int y = 0; y < Height; y++)
        for (int x = 0; x < Width; x++)
        {
            const float U = ((float)x + 0.5f) / (float)Width, V = ((float)y + 0.5f) / (float)Height;
            const float H     = BarkHeight(U, V);
            const int   Gx    = (int)(U * (float)BarkGrid) % BarkGrid, Gy = (int)(V * (float)BarkGrid) % BarkGrid;
            const float Tone  = M.Tone[Gy * BarkGrid + Gx];
            const float Grain = 0.84f + 0.32f * Fractal(U, V, 64, 12, 3, 83u);
            const float Lit   = Tone * Grain;
            float R = 0.47f * Lit, G = 0.43f * Lit, B = 0.38f * Lit;
            // A flake gone: the fresher bark under it, orange-brown.
            float Flake = (Fractal(U, V, 12, 10, 2, 91u) - 0.66f) / 0.12f;
            Flake       = ((Flake < 0.0f) ? 0.0f : ((Flake > 1.0f) ? 1.0f : Flake)) * H;
            R += (0.58f * Lit - R) * Flake * 0.8f;
            G += (0.40f * Lit - G) * Flake * 0.8f;
            B += (0.27f * Lit - B) * Flake * 0.8f;
            // Lichen in patches on the plates.
            float Green = (Fractal(U, V, 5, 4, 3, 97u) - 0.64f) / 0.2f;
            Green       = ((Green < 0.0f) ? 0.0f : ((Green > 1.0f) ? 1.0f : Green)) * H;
            R += (0.44f * Lit - R) * Green * 0.6f;
            G += (0.50f * Lit - G) * Green * 0.6f;
            B += (0.38f * Lit - B) * Green * 0.6f;
            // Down into the crevice: dark brown.
            const float Deep = (H > 0.0f) ? powf(H, 0.7f) : 0.0f;
            R = 0.13f + (R - 0.13f) * Deep;
            G = 0.105f + (G - 0.105f) * Deep;
            B = 0.085f + (B - 0.085f) * Deep;
            unsigned char* const T = Rgba + 4 * (y * Width + x);
            T[0] = Byte(R);
            T[1] = Byte(G);
            T[2] = Byte(B);
            T[3] = 255;
        }
}

// The leaves' sheet, Width x Height texels, R G B A: four pictures side by side, each one leaf - its foot at the
// picture's bottom, its tip at the top. A stalk, then the blade: rounded at its base, widest a little below its middle,
// drawn out to a point, its edge finely toothed. A pale midrib, thinner to the tip; side veins leaving it out and a
// little up, arching up near the edge and running along it towards the next; between them a fine net of veins. A fresh
// spring green, each picture its own, darker towards the edge. Alpha 1 on the leaf, 0 off it, a texel's soft edge.
// A shrub rose's picture on the leaf sheet, in cell Picture of Cell texels across (RoseLeaves): 4 and 5 a leaflet - a
// short stalk, an oval blade a little over half as wide as long, its edge toothed, dark glossy green with a paler midrib
// and side veins; 6 and 7 a petal - a narrow claw at its foot, widening to a rounded top with a shallow notch in its
// middle, pale yellow-white at the foot going over into deep red (6) or a pinker red (7) - a yellow rose's (bYellow)
// into a deep golden yellow or a paler one, as bright against the game's own yellow flower (pflanzentex152) as the red
// against its red - its fine veins fanning out from the foot. Each from its foot at the bottom to its tip at the top, as
// wide as the cell where it is widest; in cell At of the sheet, the picture's own where none is said.
inline void DrawRosePicture(unsigned char* Rgba, int Width, int Height, int Cell, int Picture, int At = -1,
                            bool bYellow = false)
{
    const bool  bPetal = Picture >= 6;
    const float Red[2][3] = { { 0.52f, 0.02f, 0.05f }, { 0.74f, 0.10f, 0.18f } };
    const float Yellow[2][3] = { { 0.85f, 0.66f, 0.18f }, { 0.96f, 0.82f, 0.32f } };
    const float PerX = (float)Cell * 0.5f;
    for (int y = 0; y < Height; y++)
        for (int x = 0; x < Cell; x++)
        {
            const float Px = ((float)x + 0.5f) / (float)Cell;
            const float T  = 1.0f - ((float)y + 0.5f) / (float)Height;
            const float Ax = fabsf((Px - 0.5f) * 2.0f);
            float       H  = 0.0f;
            if (bPetal)
            {
                // The outline: the claw, widening to its widest some way up, the top round, a shallow dip in its
                // middle.
                const float Tm = 0.62f;
                if (T > 0.0f && T < Tm)
                    H = 0.96f * powf(T / Tm, 0.55f);
                else if (T >= Tm && T < 1.0f)
                {
                    const float U = (T - Tm) / (1.0f - Tm);
                    H             = 0.96f * sqrtf((U < 1.0f) ? 1.0f - U * U : 0.0f);
                }
                H *= (T < 0.12f) ? 0.35f + 0.65f * T / 0.12f : 1.0f;
                if (Ax < 0.3f && T > 1.0f - 0.05f * (1.0f - (Ax / 0.3f) * (Ax / 0.3f)))
                    H = 0.0f;
            }
            else
            {
                const float Sb = (T - 0.06f) / 0.94f;
                if (T < 0.07f)
                    H = 0.035f;
                else if (Sb < 1.0f)
                {
                    H = 0.64f * powf(sinf(3.14159265f * powf(Sb, 0.8f)), 0.75f);
                    const float Tooth = Sb * 17.0f + (float)Picture * 0.31f;
                    H *= 1.0f - 0.09f * (Tooth - floorf(Tooth));
                }
            }
            float A = (H - Ax) * PerX + 0.5f;
            A       = (A < 0.0f) ? 0.0f : ((A > 1.0f) ? 1.0f : A);
            float Cr = 0.0f, Cg = 0.0f, Cb = 0.0f;
            if (A > 0.0f && bPetal)
            {
                const float* const C    = bYellow ? Yellow[Picture - 6] : Red[Picture - 6];
                float              Body = (T - 0.04f) / 0.3f;
                Body                    = (Body < 0.0f) ? 0.0f : ((Body > 1.0f) ? 1.0f : Body);
                Body                    = Body * Body * (3.0f - 2.0f * Body);
                const float Edge = Ax / ((H > 1e-3f) ? H : 1e-3f);
                const float Lit  = 1.04f - 0.18f * Edge * Edge;
                // The veins: fine lines fanning out from the foot, a little paler.
                const float Fan  = atan2f((Px - 0.5f) * 2.0f, T + 0.15f) * 22.0f + (float)Picture;
                const float Vein = 1.0f + 0.07f * cosf(6.2831853f * Fan) * (1.0f - Body * 0.5f);
                Cr = (0.95f + (C[0] - 0.95f) * Body) * Lit * Vein;
                Cg = (0.86f + (C[1] - 0.86f) * Body) * Lit * Vein;
                Cb = (0.60f + (C[2] - 0.60f) * Body) * Lit * Vein;
            }
            else if (A > 0.0f)
            {
                const float Sb   = (T - 0.06f) / 0.94f;
                const float Edge = Ax / ((H > 1e-3f) ? H : 1e-3f);
                const float Lit  = 1.05f - 0.25f * Edge * Edge;
                float       Rib  = (0.02f - Ax) * PerX + 0.5f;
                Rib              = (Rib < 0.0f) ? 0.0f : ((Rib > 1.0f) ? 1.0f : Rib);
                // The side veins: up and out from the midrib, seven a side.
                const float Up   = (Sb - Ax * 0.55f) * 7.0f + (float)Picture * 0.2f;
                float       Vein = 1.0f - fabsf(Up - floorf(Up) - 0.5f) * 2.0f;
                Vein             = (Vein > 0.8f) ? (Vein - 0.8f) * 5.0f * 0.5f : 0.0f;
                const float Pale = (Rib > Vein) ? Rib : Vein;
                const float Gr   = 0.96f + 0.08f * Fractal(Px, T, 64, 128, 2, 211u + (unsigned int)Picture);
                Cr = (0.10f + 0.20f * Pale) * Lit * Gr;
                Cg = (0.27f + 0.22f * Pale) * Lit * Gr;
                Cb = (0.07f + 0.08f * Pale) * Lit * Gr;
            }
            unsigned char* const P = Rgba + 4 * (y * Width + ((At >= 0) ? At : Picture) * Cell + x);
            P[0] = Byte(Cr);
            P[1] = Byte(Cg);
            P[2] = Byte(Cb);
            P[3] = Byte(A);
        }
}

// A yellow shrub rose's petals (Kind_RoseYellow): pictures 6 and 7 of the leaves' sheet yellow, side by side in a sheet
// of their own, two cells across - the leaves' sheet has no room left, and a second whole one would be a waste of the
// address space for two pictures. Its flowers' heads read it in place of the sheet's petals (U four times as wide).
inline void DrawRoseYellowPetals(unsigned char* Rgba, int Width, int Height)
{
    memset(Rgba, 0, (size_t)Width * (size_t)Height * 4u);
    DrawRosePicture(Rgba, Width, Height, Width / 2, 6, 0, true);
    DrawRosePicture(Rgba, Width, Height, Width / 2, 7, 1, true);
}

// A picture of the sheet of the plants grown stem by stem (StemPlant, DrawStemSheet), in cell Picture of Cell texels
// across, from its foot at the bottom to its tip at the top - its width a share of its length, on a blade as wide as long
// (the renderer's width share): 0 and 1 a lily's leaf, lance-shaped, a fifth as wide as long, deep green with its veins
// running along it (1 a little yellower); 2 a pink's grassy leaf, narrow, tapering to a point, its midrib pale; 3, 4 and
// 5 a fern's leaflet, lance-shaped with shallow lobes, three greens; 6 a lily's petal, pointed, orange with a yellow
// throat, a pale line up its middle and dark speckles low on it; 7 a pink's petal, round-topped and scalloped, pale pink
// going paler at its foot, its veins fanning out.
inline void DrawStemPicture(unsigned char* Rgba, int Width, int Height, int Cell, int Picture)
{
    const float PerX = (float)Cell * 0.5f;
    for (int y = 0; y < Height; y++)
        for (int x = 0; x < Cell; x++)
        {
            const float Px = ((float)x + 0.5f) / (float)Cell;
            const float T  = 1.0f - ((float)y + 0.5f) / (float)Height;
            const float Ax = fabsf((Px - 0.5f) * 2.0f);
            float       H  = 0.0f;
            if (Picture <= 1)
                H = (T > 0.0f && T < 1.0f) ? 0.2f * powf(sinf(3.14159265f * powf(T, 0.8f)), 0.85f) : 0.0f;
            else if (Picture == 2)
                H = (T > 0.0f && T < 1.0f) ? ((T < 0.8f) ? 0.07f * (1.0f - 0.3f * T) : 0.056f * (1.0f - T) / 0.2f)
                                           : 0.0f;
            else if (Picture <= 5)
            {
                H = (T > 0.0f && T < 1.0f) ? 0.24f * powf(sinf(3.14159265f * powf(T, 0.75f)), 0.8f) : 0.0f;
                const float Lobe = T * 9.0f + (float)Picture * 0.37f;
                H *= 1.0f - 0.12f * (Lobe - floorf(Lobe));
            }
            else if (Picture == 6)
                H = (T > 0.0f && T < 1.0f) ? 0.38f * powf(sinf(3.14159265f * powf(T, 0.9f)), 0.9f) : 0.0f;
            else
            {
                const float Tm = 0.62f;
                if (T > 0.0f && T < Tm)
                    H = 0.9f * powf(T / Tm, 0.6f);
                else if (T >= Tm && T < 1.0f)
                {
                    const float U = (T - Tm) / (1.0f - Tm);
                    H             = 0.9f * sqrtf((U < 1.0f) ? 1.0f - U * U : 0.0f);
                }
                H *= (T < 0.12f) ? 0.3f + 0.7f * T / 0.12f : 1.0f;
                // Its scalloped top: a few shallow notches.
                const float Notch = atan2f((Px - 0.5f) * 2.0f, T) * 3.0f;
                H *= 1.0f - 0.06f * (T > 0.8f ? 1.0f : 0.0f) * (0.5f + 0.5f * cosf(6.2831853f * Notch));
            }
            float A = (H - Ax) * PerX + 0.5f;
            A       = (A < 0.0f) ? 0.0f : ((A > 1.0f) ? 1.0f : A);
            float       Cr = 0.0f, Cg = 0.0f, Cb = 0.0f;
            const float Edge = Ax / ((H > 1e-3f) ? H : 1e-3f);
            const float Gr   = 0.95f + 0.1f * Fractal(Px, T, 32, 128, 2, 401u + (unsigned int)Picture);
            if (A > 0.0f && Picture <= 5)
            {
                const float Lit = 1.05f - 0.25f * Edge * Edge;
                float       Rib = (0.03f - Ax) * PerX * 0.6f + 0.5f;
                Rib             = (Rib < 0.0f) ? 0.0f : ((Rib > 1.0f) ? 1.0f : Rib);
                // A lily's and a grass's veins run along the blade; a fern's leaflet has its midrib alone.
                const float Vein = (Picture <= 2) ? 0.5f + 0.5f * cosf(Edge * 3.14159265f * 7.0f) : 0.0f;
                const float Pale = (Rib > 0.15f * Vein) ? Rib : 0.15f * Vein;
                const float Base[6][3] = { { 0.08f, 0.24f, 0.06f }, { 0.11f, 0.27f, 0.06f }, { 0.15f, 0.33f, 0.09f },
                                           { 0.09f, 0.28f, 0.07f }, { 0.12f, 0.32f, 0.08f }, { 0.10f, 0.30f, 0.10f } };
                const float* const C = Base[Picture];
                Cr = (C[0] + 0.18f * Pale) * Lit * Gr;
                Cg = (C[1] + 0.2f * Pale) * Lit * Gr;
                Cb = (C[2] + 0.07f * Pale) * Lit * Gr;
            }
            else if (A > 0.0f && Picture == 6)
            {
                const float Lit    = 1.05f - 0.2f * Edge * Edge;
                float       Throat = 1.0f - T / 0.35f;
                Throat             = (Throat < 0.0f) ? 0.0f : Throat;
                Cr = 0.97f;
                Cg = 0.47f + 0.3f * Throat;
                Cb = 0.12f + 0.1f * Throat;
                // The pale line up its middle, and the dark speckles low on it, a few a side.
                const float Line = (Ax < 0.08f) ? 0.25f * (1.0f - Ax / 0.08f) * (1.0f - T) : 0.0f;
                Cg += Line * 0.4f;
                Cb += Line * 0.3f;
                const float Sx = Px * 9.0f, Sy = T * 18.0f;
                const unsigned int Spot = Hash((unsigned int)floorf(Sx) * 73u + (unsigned int)floorf(Sy) * 977u + 5u);
                const float Fx = Sx - floorf(Sx) - 0.5f, Fy = Sy - floorf(Sy) - 0.5f;
                if (T > 0.1f && T < 0.5f && (Spot & 3u) == 0u && Fx * Fx + Fy * Fy < 0.06f)
                {
                    Cr = 0.38f;
                    Cg = 0.09f;
                    Cb = 0.04f;
                }
                Cr *= Lit * Gr;
                Cg *= Lit * Gr;
                Cb *= Lit * Gr;
            }
            else if (A > 0.0f)
            {
                float Body = (T - 0.04f) / 0.35f;
                Body       = (Body < 0.0f) ? 0.0f : ((Body > 1.0f) ? 1.0f : Body);
                Body       = Body * Body * (3.0f - 2.0f * Body);
                const float Lit  = 1.04f - 0.18f * Edge * Edge;
                const float Fan  = atan2f((Px - 0.5f) * 2.0f, T + 0.15f) * 20.0f;
                const float Vein = 1.0f + 0.06f * cosf(6.2831853f * Fan) * (1.0f - Body * 0.5f);
                Cr = (0.98f + (0.93f - 0.98f) * Body) * Lit * Vein;
                Cg = (0.90f + (0.62f - 0.90f) * Body) * Lit * Vein;
                Cb = (0.86f + (0.72f - 0.86f) * Body) * Lit * Vein;
            }
            unsigned char* const P = Rgba + 4 * (y * Width + Picture * Cell + x);
            P[0] = Byte(Cr);
            P[1] = Byte(Cg);
            P[2] = Byte(Cb);
            P[3] = Byte(A);
        }
}
// The sheet itself: the eight pictures side by side (DrawStemPicture), each an eighth of its width.
inline void DrawStemSheet(unsigned char* Rgba, int Width, int Height)
{
    memset(Rgba, 0, (size_t)Width * (size_t)Height * 4u);
    for (int Picture = 0; Picture < 8; Picture++)
        DrawStemPicture(Rgba, Width, Height, Width / 8, Picture);
}
// The wood of a plant grown stem by stem (StemPlant) in place of the bark: a green stem's skin, round it (U) and along
// it (V) in repeats as the bark's - a soft green, fine streaks along it, a little paler here and there.
inline void DrawStemSkin(unsigned char* Rgba, int Width, int Height)
{
    for (int y = 0; y < Height; y++)
        for (int x = 0; x < Width; x++)
        {
            const float U = ((float)x + 0.5f) / (float)Width, V = ((float)y + 0.5f) / (float)Height;
            const float Streak = 0.5f + 0.5f * sinf(6.2831853f * (U * 12.0f + 0.3f * sinf(6.2831853f * V * 2.0f)));
            const float Gr     = 0.92f + 0.16f * Fractal(U, V, 8, 32, 3, 977u);
            unsigned char* const P = Rgba + 4 * (y * Width + x);
            P[0] = Byte((0.17f + 0.04f * Streak) * Gr);
            P[1] = Byte((0.33f + 0.06f * Streak) * Gr);
            P[2] = Byte((0.10f + 0.02f * Streak) * Gr);
            P[3] = 255;
        }
}

inline void DrawLeaves(unsigned char* Rgba, int Width, int Height)
{
    memset(Rgba, 0, (size_t)Width * (size_t)Height * 4u);
    // Eight pictures side by side: four leaves of the trees, then the shrub rose's two leaflets and two petals.
    const int   Cell = Width / 8;
    for (int Picture = 4; Picture < 8; Picture++)
        DrawRosePicture(Rgba, Width, Height, Cell, Picture);
    const float Greens[4][3] = { { 0.20f, 0.44f, 0.11f }, { 0.15f, 0.36f, 0.10f },
                                 { 0.27f, 0.47f, 0.12f }, { 0.19f, 0.41f, 0.12f } };
    for (int Picture = 0; Picture < 4; Picture++)
    {
        FRandom     R(0x1EAF5u + (unsigned int)Picture * 7919u);
        const float Stalk = R.Range(0.10f, 0.14f);      // the stalk's share of the length
        const float Most  = R.Range(0.36f, 0.46f);      // where the blade is widest, a share of its own length
        const float Wide  = R.Range(0.78f, 0.92f);      // its half-width there, of the picture's half-width
        const float Bow   = logf(0.5f) / logf(Most);
        auto Half = [&](float Sb)
        {
            if (Sb <= 0.0f || Sb >= 1.0f)
                return 0.0f;
            float H         = Wide * powf(sinf(3.14159265f * powf(Sb, Bow)), 0.85f);
            const float Tip = (Sb > 0.8f) ? (Sb - 0.8f) / 0.2f : 0.0f;
            return H * (1.0f - 0.35f * Tip * Tip);
        };
        // The side veins: where each leaves the midrib, how far up the blade it climbs, a path of points out to the edge
        // and along it.
        const int Veins = 7 + (int)(R.Next() % 3u);
        enum { Steps = 14 };
        float     PathX[12][Steps], PathS[12][Steps], From[12], Top[12];
        for (int k = 0; k < Veins; k++)
        {
            const float S0    = 0.05f + 0.82f * ((float)k + 0.4f) / (float)Veins + R.Range(-0.02f, 0.02f);
            const float Reach = R.Range(0.16f, 0.22f);
            From[k] = S0;
            Top[k]  = S0;
            for (int i = 0; i < Steps; i++)
            {
                const float Tau = 1.4f * (float)i / (float)(Steps - 1);
                float       Sb, X;
                if (Tau <= 1.0f)
                {
                    Sb = S0 + Reach * (0.55f * Tau + 0.45f * Tau * Tau);
                    X  = 0.88f * Half(Sb) * powf(Tau, 0.9f);
                }
                else
                {
                    Sb = S0 + Reach * (1.0f + (Tau - 1.0f) * 0.6f);
                    X  = 0.88f * Half(Sb);
                }
                PathX[k][i] = X;
                PathS[k][i] = Sb;
                Top[k]      = (Sb > Top[k]) ? Sb : Top[k];
            }
        }
        const float PerX = (float)Cell * 0.5f, PerS = (float)Height * (1.0f - Stalk);    // texels a unit
        for (int y = 0; y < Height; y++)
            for (int x = 0; x < Cell; x++)
            {
                const float Px = ((float)x + 0.5f) / (float)Cell;
                const float T  = 1.0f - ((float)y + 0.5f) / (float)Height;
                const float Ax = fabsf((Px - 0.5f) * 2.0f);
                float       A = 0.0f, Cr = 0.0f, Cg = 0.0f, Cb = 0.0f;
                if (T < Stalk + 0.01f)
                {
                    // The stalk: thin, a little thicker where it meets the blade.
                    const float W = 0.022f + 0.012f * T / Stalk;
                    float       In = (W - Ax) * PerX + 0.5f;
                    In             = (In < 0.0f) ? 0.0f : ((In > 1.0f) ? 1.0f : In);
                    if (In > 0.0f)
                    {
                        A  = In;
                        Cr = 0.34f;
                        Cg = 0.38f;
                        Cb = 0.15f;
                    }
                }
                const float Sb = (T - Stalk) / (1.0f - Stalk);
                if (Sb > 0.0f && Sb < 1.0f)
                {
                    float       H     = Half(Sb);
                    const float Tooth = Sb * 46.0f + (float)Picture * 0.37f;
                    H *= 1.0f - 0.03f * (Tooth - floorf(Tooth));
                    float In = (H - Ax) * PerX + 0.5f;
                    In       = (In < 0.0f) ? 0.0f : ((In > 1.0f) ? 1.0f : In);
                    if (In > A)
                    {
                        A = In;
                        // The green, darker towards the edge.
                        const float Edge = Ax / ((H > 1e-3f) ? H : 1e-3f);
                        const float Lit  = 1.06f - 0.3f * Edge * Edge;
                        Cr = Greens[Picture][0] * Lit;
                        Cg = Greens[Picture][1] * Lit;
                        Cb = Greens[Picture][2] * Lit;
                        // The midrib.
                        const float RibW = (0.03f - 0.022f * Sb) * PerX;
                        float       Rib  = RibW - Ax * PerX + 0.5f;
                        Rib              = (Rib < 0.0f) ? 0.0f : ((Rib > 1.0f) ? 1.0f : Rib);
                        // The side veins.
                        float Vein = 0.0f;
                        for (int k = 0; k < Veins; k++)
                        {
                            if (Sb < From[k] - 0.03f || Sb > Top[k] + 0.03f)
                                continue;
                            for (int i = 0; i + 1 < Steps; i++)
                            {
                                const float X0 = PathX[k][i] * PerX, S0 = PathS[k][i] * PerS;
                                const float X1 = PathX[k][i + 1] * PerX, S1 = PathS[k][i + 1] * PerS;
                                const float Qx = Ax * PerX, Qs = Sb * PerS;
                                const float Dx = X1 - X0, Ds = S1 - S0;
                                float       F  = ((Qx - X0) * Dx + (Qs - S0) * Ds) / (Dx * Dx + Ds * Ds + 1e-6f);
                                F              = (F < 0.0f) ? 0.0f : ((F > 1.0f) ? 1.0f : F);
                                const float Ex = Qx - (X0 + Dx * F), Es = Qs - (S0 + Ds * F);
                                const float W  = 2.6f - 1.6f * ((float)i + F) / (float)(Steps - 1);
                                float       On = W - sqrtf(Ex * Ex + Es * Es) + 0.5f;
                                On             = (On < 0.0f) ? 0.0f : ((On > 1.0f) ? 1.0f : On);
                                Vein           = (On > Vein) ? On : Vein;
                            }
                        }
                        // The fine net between them.
                        unsigned int Id;
                        const float  Mesh = BarkEdge(Px * (float)Cell / 26.0f, T * (float)Height / 26.0f, 4096, 4096,
                                                     0xC0FFEEu + (unsigned int)Picture, Id) * 26.0f;
                        float Net = 1.2f - Mesh;
                        Net       = (Net < 0.0f) ? 0.0f : ((Net > 1.0f) ? 1.0f : Net);
                        float Pale = Rib;
                        Pale       = (Vein * 0.75f > Pale) ? Vein * 0.75f : Pale;
                        Pale       = (Net * 0.2f > Pale) ? Net * 0.2f : Pale;
                        const float Grain = 0.96f + 0.08f * Fractal(Px, T, 64, 128, 2, 101u + (unsigned int)Picture);
                        Cr = (Cr + (0.56f - Cr) * Pale) * Grain;
                        Cg = (Cg + (0.62f - Cg) * Pale) * Grain;
                        Cb = (Cb + (0.22f - Cb) * Pale) * Grain;
                    }
                }
                unsigned char* const P = Rgba + 4 * (y * Width + Picture * Cell + x);
                P[0] = Byte(Cr);
                P[1] = Byte(Cg);
                P[2] = Byte(Cb);
                P[3] = Byte(A);
            }
    }
}

// A bullet's mark in bark, Size x Size texels, R G B A, as a MULTIPLIER of what is under it (the modulated blend:
// 128 leaves the bark as it is): a dark hole in the middle, the bark torn round it - fresh wood, lighter, in splinters
// along the grain (up the picture) - and nothing at the picture's edge. Alpha whole: the blend alone decides.
inline void DrawBulletMark(unsigned char* Rgba, int Size)
{
    FRandom R(0xB0117u);
    float Splinter[12];
    for (int k = 0; k < 12; k++)
        Splinter[k] = R.Range(0.7f, 1.6f);
    for (int y = 0; y < Size; y++)
        for (int x = 0; x < Size; x++)
        {
            const float Dx = ((float)x + 0.5f) / (float)Size * 2.0f - 1.0f;
            const float Dy = ((float)y + 0.5f) / (float)Size * 2.0f - 1.0f;
            // Along the grain the torn ring reaches further: the wood splits up and down the trunk.
            const float A     = atan2f(Dy, Dx);
            const int   Ray   = (int)((A + 3.14159265f) / 6.2831853f * 12.0f) % 12;
            const float Grain = 1.0f + 0.9f * fabsf(Dy) / (fabsf(Dx) + fabsf(Dy) + 1e-3f);
            const float D     = sqrtf(Dx * Dx + Dy * Dy);
            float V = 0.5f;
            if (D < 0.16f)
                V = 0.12f + 0.25f * (D / 0.16f);                          // the hole
            else if (D < 0.34f * Grain * Splinter[Ray])
            {
                const float T = (D - 0.16f) / (0.34f * Grain * Splinter[Ray] - 0.16f);
                V = 0.72f - 0.22f * T;                                    // fresh wood, lighter, to the bark
            }
            // To nothing at the edge.
            const float Fade = (D > 0.8f) ? 0.0f : ((D > 0.6f) ? (0.8f - D) / 0.2f : 1.0f);
            V = 0.5f + (V - 0.5f) * Fade;
            unsigned char* const T = Rgba + 4 * (y * Size + x);
            T[0] = Byte(V * 1.00f);
            T[1] = Byte(V * 0.97f);
            T[2] = Byte(V * 0.92f);
            T[3] = 255;
        }
}

// The trunk of a grown tree, from its foot up the thickest child each time while it stays a trunk (a third of the
// foot's radius at least): up to Room points, x y z and the radius, evenly along it. Its count.
inline int TrunkOf(const FPlant& P, float (*Out)[4], int Room)
{
    if (P.Nodes.Num <= 0 || Room <= 0)
        return 0;
    TList<int> Thickest;
    if (!Thickest.Reserve(P.Nodes.Num))
        return 0;
    for (int i = 0; i < P.Nodes.Num; i++)
        Thickest.Add(-1);
    for (int i = 0; i < P.Nodes.Num; i++)
    {
        const int Pa = P.Nodes[i].Parent;
        if (Pa >= 0 && (Thickest[Pa] < 0 || P.Nodes[i].Radius > P.Nodes[Thickest[Pa]].Radius))
            Thickest[Pa] = i;
    }
    TList<int> Chain;
    const float Foot = P.Nodes[0].Radius;
    for (int i = 0; i >= 0 && P.Nodes[i].Radius >= Foot * 0.33f; i = Thickest[i])
        if (!Chain.Add(i))
            break;
    const int N = (Chain.Num < Room) ? Chain.Num : Room;
    for (int k = 0; k < N; k++)
    {
        const int i = Chain[(N > 1) ? (k * (Chain.Num - 1)) / (N - 1) : 0];
        Out[k][0] = P.Nodes[i].At.X;
        Out[k][1] = P.Nodes[i].At.Y;
        Out[k][2] = P.Nodes[i].At.Z;
        Out[k][3] = P.Nodes[i].Radius;
    }
    return N;
}

} // namespace RdxTree
